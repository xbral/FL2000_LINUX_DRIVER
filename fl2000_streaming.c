// SPDX-License-Identifier: GPL-2.0
/*
 * Original driver uses default altsetting (#0) of streaming interface, which allows bursts of bulk
 * transfers of 15x1024 bytes on output. But the HW actually works incorrectly here: it uses same
 * endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification: endpoint
 * addresses can be shared only between alternate settings, not interfaces. In order to workaround
 * this we use isochronous transfers instead of bulk. There is a possibility that we still can use
 * bulk transfers with interface 0, but this is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"
#include <linux/sched.h>

/* Triple buffering:
 *  - one buffer for HDMI rendering
 *  - one buffer for USB transmission
 *  - one buffer for DRM/KMS data copy
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

/* Max bulk URBs in flight. A full 1080p frame is ~4 MB in a single URB with
 * ~1000 SG entries; letting 3-4 of those pile up bursts 12-16 MB into the xHCI
 * ring and stalls the whole USB controller (and the CPU mapping them).
 * 2 keeps the chip fed (it paces at frame rate) with half the burst.
 */
#define FL2000_MAX_INFLIGHT 2

#define FL2000_URB_TIMEOUT 100

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct drm_crtc *crtc;
	/* Each buffer journey: render->transmit->wait->... */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* List access from bh and interrupt contexts */
	size_t buf_size;
	u32 bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct semaphore work_sem;
	bool enabled;
	struct usb_anchor anchor;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kcalloc(nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&stream->render_list));

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	UNUSED(dev);

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;

	if (stream) {
		unsigned long flags;

		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		drm_crtc_handle_vblank(stream->crtc);

		/* Kick transmit workqueue */
		up(&stream->work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *last_sb = NULL;
	struct urb *data_urb;
	bool need_copy;
	size_t copy_size;

	/* Streaming é bulk contínuo a 60fps: não compete com a UI. Roda com
	 * nice +5 para o compositor continuar fluido sob carga (Hybris/IDE).
	 */
	set_user_nice(current, 5);

	while (stream->enabled) {
		unsigned long flags;
		unsigned int inflight = 0;
		struct fl2000_stream_buf *tmp;

		ret = down_interruptible(&stream->work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			stream->enabled = false;
			return;
		}

		/* Disable wakes us through the semaphore precisely so we can exit: URB
		 * completions are the only other source of ups, and they stop (or never come,
		 * on hot-unplug) once streaming winds down. Without this check the worker sat
		 * parked in down_interruptible() forever and drain_workqueue() in
		 * fl2000_stream_disable() deadlocked inside an atomic commit that held every
		 * modeset lock - freezing all displays system-wide
		 */
		if (!stream->enabled)
			return;

		spin_lock_irqsave(&stream->list_lock, flags);

		/* Throttle: conta URBs em voo (wait_list). Se já temos o bastante,
		 * devolve o buffer e espera o próximo completion (que dá up no sem).
		 * Sem isso o enable dava kick de 3 frames de 4 MB de uma vez.
		 */
		list_for_each_entry(tmp, &stream->wait_list, list) {
			if (++inflight >= FL2000_MAX_INFLIGHT)
				break;
		}
		if (inflight >= FL2000_MAX_INFLIGHT) {
			spin_unlock_irqrestore(&stream->list_lock, flags);
			continue;
		}

		/* If no buffers are available for immediate transmission - then copy latest
		 * transmission data. Toma posse do buffer (list_del) e faz o memcpy
		 * SEM lock e COM irqs ligadas: o memcpy de 4 MB sob spinlock com
		 * irqsave era a causa das travadas system-wide (ms com IRQs off).
		 */
		if (list_empty(&stream->transmit_list)) {
			if (list_empty(&stream->render_list)) {
				/* Backpressure: USB mais lento que o compositor.
				 * Antes era BUG_ON; agora só perde esse tick.
				 */
				spin_unlock_irqrestore(&stream->list_lock, flags);
				continue;
			}
			if (list_empty(&stream->wait_list))
				last_sb = list_last_entry(&stream->render_list,
							  struct fl2000_stream_buf, list);
			else
				last_sb = list_last_entry(&stream->wait_list,
							  struct fl2000_stream_buf, list);
			cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf,
						  list);
			list_del(&cur_sb->list);
			need_copy = true;
			copy_size = stream->buf_size;
		} else {
			cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf,
						  list);
			list_del(&cur_sb->list);
			need_copy = false;
			copy_size = 0;
		}
		spin_unlock_irqrestore(&stream->list_lock, flags);

		if (need_copy) {
			/* Fonte continua válida: buffers só são liberados com
			 * stream parado (disable espera este worker). No pior
			 * caso, um completion move a fonte de lista no meio da
			 * cópia e um frame sai com tear — benigno perto de
			 * travar a máquina com IRQ off.
			 */
			if (last_sb && last_sb != cur_sb)
				memcpy(cur_sb->vaddr, last_sb->vaddr, copy_size);

			spin_lock_irqsave(&stream->list_lock, flags);
			if (!stream->enabled) {
				list_add_tail(&cur_sb->list, &stream->render_list);
				spin_unlock_irqrestore(&stream->list_lock, flags);
				return;
			}
			list_add_tail(&cur_sb->list, &stream->wait_list);
			spin_unlock_irqrestore(&stream->list_lock, flags);
		} else {
			spin_lock_irqsave(&stream->list_lock, flags);
			if (!stream->enabled) {
				list_add_tail(&cur_sb->list, &stream->render_list);
				spin_unlock_irqrestore(&stream->list_lock, flags);
				return;
			}
			list_add_tail(&cur_sb->list, &stream->wait_list);
			spin_unlock_irqrestore(&stream->list_lock, flags);
		}

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			stream->enabled = false;
			return;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb, GFP_KERNEL);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			/* Must unanchor before dropping the reference: a failed-submit URB left
			 * in the anchor is never completed and never given back, and teardown's
			 * kill loop chokes on it - observed as a kworker live-spinning in
			 * usb_kill_anchored_urbs() inside disconnect for tens of minutes while
			 * holding the USB device mutex, wedging the whole port
			 */
			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);
			stream->enabled = false;
		}
	}
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int xx = 0;

	for (unsigned int x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	void *dst;
	u32 dst_line_len;
	u32 bytes_pix;
	size_t buf_size;
	unsigned long flags;
	static unsigned long drop_count;

	/* Toma posse de um buffer livre e converte SEM lock e COM irqs ligadas.
	 * Antes, os ~4 MB de conversão rodavam sob spin_lock_irqsave — ms com
	 * IRQs desligadas a cada damage, que é exatamente a "travada" da máquina
	 * (mouse/áudio/scheduler). Listas só precisam do lock no take/put.
	 */
	spin_lock_irqsave(&stream->list_lock, flags);
	if (list_empty(&stream->render_list)) {
		/* Backpressure: USB mais lento que o compositor produzindo.
		 * Antes era BUG_ON (oops). Agora descarta o frame mais antigo:
		 * o worker vai reenviar o último via fallback, sem crash.
		 */
		spin_unlock_irqrestore(&stream->list_lock, flags);
		if ((++drop_count & 0x3f) == 1)
			pr_debug_ratelimited("fl2000: compress drop (render empty) x%lu\n",
					     drop_count);
		return;
	}
	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	list_del(&cur_sb->list);
	bytes_pix = stream->bytes_pix;
	buf_size = stream->buf_size;
	spin_unlock_irqrestore(&stream->list_lock, flags);

	dst = cur_sb->vaddr;
	dst_line_len = width * bytes_pix;

	if (fl2000_test_fill) {
		/* Diagnostic: ignore the framebuffer and transmit solid white. The chip paces
		 * bulk URBs at frame rate, so it demonstrably consumes what we send; a white
		 * screen here proves the chip->DAC->monitor data path and isolates a black
		 * screen to the framebuffer copy path, while a black screen with this set
		 * means the chip is discarding/blanking pixel data internally
		 */
		memset(dst, 0xff, buf_size);
	} else {
		for (unsigned int y = 0; y < height; y++) {
			switch (bytes_pix) {
			case 2:
				fl2000_xrgb888_to_rgb565_line(dst, src, width);
				break;
			case 3:
				fl2000_xrgb888_to_rgb888_line(dst, src, width);
				break;
			default: /* Shouldn't happen */
				break;
			}
			src += pitch;
			dst += dst_line_len;
		}
	}

	spin_lock_irqsave(&stream->list_lock, flags);
	list_add_tail(&cur_sb->list, &stream->transmit_list);
	spin_unlock_irqrestore(&stream->list_lock, flags);
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;
	unsigned long flags;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~7U;

	stream->bytes_pix = bytes_pix;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Mode_set só é chamado com stream parado (disable já drenou o worker
	 * e os completions). Se ainda houver buffers em voo, não libera por
	 * baixo deles: só loga e recria o que está em render.
	 */
	spin_lock_irqsave(&stream->list_lock, flags);
	if (!list_empty(&stream->transmit_list) || !list_empty(&stream->wait_list))
		pr_warn("fl2000: mode_set with buffers in flight, recreating render only\n");
	spin_unlock_irqrestore(&stream->list_lock, flags);

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	/* Antes era BUG_ON(transmit vazio) — um modeset sem damage prévio
	 * derrubava a máquina. Se não há frame novo, o worker gera via
	 * fallback (último frame / zero). Kick reduzido: 2 URBs em voo
	 * bastam (FL2000_MAX_INFLIGHT); 3 rajadas de 4 MB travavam o xHCI.
	 */
	if (list_empty(&stream->transmit_list))
		pr_debug("fl2000: enable without queued frame, worker will duplicate\n");

	/* The anchor is empty here; re-init clears any poisoned state a previous disable left
	 * behind, which would otherwise auto-reject every URB of the new session
	 */
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (int i = 0; i < FL2000_MAX_INFLIGHT; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	stream->enabled = false;

	/* Unblock a worker parked on the semaphore so drain can complete; the worker checks
	 * 'enabled' on wake and exits
	 */
	up(&stream->work_sem);

	drain_workqueue(stream->work_queue);

	/* Poison rather than kill: poisoning also rejects any URB that races in afterwards.
	 * Anything still anchored after that (e.g. a failed-submit straggler that can never
	 * complete) is scuttled - dropped from the anchor without waiting - because waiting on
	 * a URB the HCD will never give back spins forever on a glitched/dead port. The next
	 * fl2000_stream_enable() starts from a clean semaphore; re-init the anchor state too
	 */
	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000)) {
		usb_poison_anchored_urbs(&stream->anchor);
		usb_scuttle_anchored_urbs(&stream->anchor);
	}

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock(&stream->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 *
 * Return: Operation result
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsettung 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, stream);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->transmit_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->usb_dev = usb_dev;
	stream->crtc = crtc;

	/* Fila ordenada, unbound e freezable: um item por vez, sem prender CPU
	 * do compositor, e congela no suspend. create_workqueue() antigo criava
	 * workers per-CPU com prioridade normal competindo com a UI.
	 */
	stream->work_queue = alloc_ordered_workqueue("fl2000_stream",
						     WQ_FREEZABLE | WQ_MEM_RECLAIM);
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}

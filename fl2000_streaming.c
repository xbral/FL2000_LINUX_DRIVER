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

/* Streaming stall watchdog. Vblanks on this device ARE stream URB completions, and the
 * transmit worker is self-sustaining (it re-submits the last frame at chip pace even
 * with no compositor damage). Host-side accounting guarantees that a parked worker
 * always has at least one URB in flight, so a stall with zero driver errors means the
 * chip or the xHCI endpoint stopped completing bulk transfers: the monitor loses sync
 * and goes black while the connector still reports connected (observed repeatedly,
 * silent - see NOTES.md "black screen" vblank WARNs). The watchdog detects that,
 * logs it (the missing evidence for every previous occurrence) and attempts recovery.
 *
 * Recovery ladder, in escalation order:
 *   1-2  soft: unlink the stuck URBs and kick the worker. Unlink requests
 *        cancellation from the HCD; if the endpoint is merely starved the
 *        givebacks return the buffers and the worker resubmits. (Scuttling
 *        was tried first and is useless here: the scuttled URBs never come
 *        back, so the stuck buffers pin wait_list at FL2000_MAX_INFLIGHT and
 *        the throttle blocks every resubmission - recovery was a no-op.)
 *   3    host endpoint reset: if the unlink givebacks emptied the ring but
 *        the endpoint is left in a bad state. Refused with "EP not empty"
 *        when the ring still has TDs, in which case we escalate anyway.
 *   4-5  hard: full USB device reset (usb_reset_device). The wedge signature
 *        is: control transfers complete, chip app-reset succeeds, but bulk
 *        completions never resume - the chip's streaming engine is dead and
 *        nothing short of a port reset (what a physical replug does) revives
 *        it. After the reset the chip is reprogrammed from scratch and the
 *        stream restarted.
 *   6    give up: only a physical replug revives the display
 *
 * Climbing that ladder costs one FL2000_WATCHDOG_PERIOD tick per rung, so the hard reset
 * is ~3.25 s behind the stall. When the chip is going to drop off the bus anyway that is
 * far too late: the re-enumeration changes the DRM minor, which is what leaves the
 * compositor holding a stale output and the display black until the session restarts.
 * The observed signature of that case (2026-10-09) is a failing VGA status read in the
 * very same tick as the stall - the bulk endpoint is dead and a control transfer fails
 * too, ~350 ms before "USB disconnect". So rung 1 escalates straight to the hard reset
 * on that signal, same work item, before the chip can leave the bus.
 */
#define FL2000_STALL_TIMEOUT msecs_to_jiffies(250)
#define FL2000_WATCHDOG_PERIOD msecs_to_jiffies(1000)
#define FL2000_STALL_SOFT_TRIES 2
#define FL2000_STALL_EP_TRIES 1
#define FL2000_STALL_RESET_TRIES 2

/* Size of the chunk a frame is split into, in KB. 0 = one URB per frame (the
 * old behaviour).
 *
 * Why split at all: the dongle pushes a whole 1080p frame as ONE 4,15 MB bulk
 * transfer, which on a 5 Gbps link is 6,6 ms of *continuous* bulk with no gap
 * for the controller to service anybody else. The Bluetooth dongle is a
 * full-speed USB 2.0 device on the USB2 companion bus of the SAME xHCI
 * controller, and its transfers are queued behind that burst: measured with
 * the dongle streaming, 38% of the mouse reports reached the input layer more
 * than 100 ms late - up to 3,6 s late. With the dongle idle, 100% of the
 * reports arrived within 5 ms of their HCI packet.
 *
 * Splitting the frame into ~1 MB chunks makes the burst 4x shorter and lets
 * the controller interleave the Bluetooth transfers in between. The byte
 * stream the chip sees is unchanged - the chunks go out back to back, in
 * order, and only the last one carries the ZLP that closes the frame.
 */
static int fl2000_chunk_kb = 1024;
module_param(fl2000_chunk_kb, int, 0644);
MODULE_PARM_DESC(fl2000_chunk_kb,
		 "Size in KB of each bulk chunk a frame is split into (0 = one URB per frame)");

#define FL2000_CHUNK_BYTES ((size_t)fl2000_chunk_kb * 1024)

struct fl2000_stream_buf {
	struct list_head list;
	/* One scatterlist table per chunk. With chunking disabled there is a
	 * single table covering the whole frame, exactly as before.
	 */
	struct sg_table *chunk_sgt;
	unsigned int n_chunks;
	unsigned int chunk_bytes;	/* bytes per chunk, last one may be shorter */
	unsigned int chunk_next;	/* next chunk to submit */
	unsigned int chunk_done;	/* chunks completed */
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
	/* Chunks of the frame currently being sent, in flight. Counted in
	 * chunks, not frames: this is what limits how much bulk data the
	 * controller is holding for us at any time.
	 */
	atomic_t chunks_inflight;
	/* The frame whose chunks are being sent. The chunks of one frame must
	 * reach the chip in order, so the frame is kept here until all of its
	 * chunks are out; only then the next frame starts.
	 */
	struct fl2000_stream_buf *cur_buf;
	/* Stall watchdog state, see FL2000_STALL_* above */
	struct delayed_work watchdog_work;
	struct workqueue_struct *watchdog_wq;
	/* True while the watchdog is supposed to be running. 'enabled' alone is not
	 * enough: the reset path clears 'enabled' transiently (pre_reset ->
	 * fl2000_stream_disable) and the watchdog must survive that, but a teardown
	 * must stop it for good. Without this distinction a tick cannot tell "come
	 * back later" from "you are done", and 1.0.5 chose wrong: it re-armed
	 * forever after a disable, and the watchdog never came back at all when it
	 * should have (incident 2026-10-28).
	 */
	bool watchdog_armed;
	unsigned int watchdog_ticks;
	unsigned long last_completion;
	unsigned int stall_recoveries;
	bool gave_up;
	/* Stall forensics: the last completion status and per-session counters, so a wedge
	 * leaves evidence in dmesg (previously the stall was completely silent)
	 */
	int last_urb_status;
	unsigned int completion_count;
	unsigned int error_count;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	for (unsigned int i = 0; i < sb->n_chunks; i++)
		sg_free_table(&sb->chunk_sgt[i]);

	kfree(sb->chunk_sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

/* Builds the per-chunk scatterlist tables. A chunk always starts on a page
 * boundary (the chunk size is a multiple of PAGE_SIZE), so the offset within
 * the first page is always zero and the last chunk is simply shorter.
 */
static int fl2000_alloc_chunk_sgts(struct fl2000_stream_buf *sb, unsigned int size)
{
	unsigned int chunk_bytes = FL2000_CHUNK_BYTES;
	unsigned int pages_per_chunk;
	unsigned int n_chunks;
	int ret;

	if (!chunk_bytes)
		chunk_bytes = size;	/* chunking disabled: one chunk = one frame */

	n_chunks = DIV_ROUND_UP(size, chunk_bytes);
	pages_per_chunk = PAGE_ALIGN(chunk_bytes) >> PAGE_SHIFT;

	sb->chunk_sgt = kcalloc(n_chunks, sizeof(struct sg_table), GFP_KERNEL);
	if (!sb->chunk_sgt)
		return -ENOMEM;

	for (unsigned int k = 0; k < n_chunks; k++) {
		unsigned int off = k * chunk_bytes;
		unsigned int first_page = off >> PAGE_SHIFT;
		unsigned int npages = min_t(unsigned int, pages_per_chunk,
					    sb->nr_pages - first_page);
		unsigned int nbytes = min_t(unsigned int, chunk_bytes, size - off);

		ret = sg_alloc_table_from_pages(&sb->chunk_sgt[k],
						 sb->pages + first_page, npages,
						 0, nbytes, GFP_KERNEL);
		if (ret)
			return ret;
		sb->n_chunks++;
	}

	sb->chunk_bytes = chunk_bytes;

	return 0;
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

	ret = fl2000_alloc_chunk_sgts(sb, size);
	if (ret)
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

	stream->cur_buf = NULL;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	/* Um modeset não pode derrubar a máquina: se algo chegou aqui com a
	 * lista ocupada, devolve erro em vez de BUG_ON (panic/reboot).
	 */
	if (WARN_ON(!list_empty(&stream->render_list)))
		return -EBUSY;

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

/* Cancel the stall watchdog, but never wait on ourselves.
 *
 * When usb_reset_device() force-unbinds this driver from inside the watchdog's own work
 * item, the unbind path calls fl2000_stream_disable()/fl2000_stream_release() right here.
 * cancel_delayed_work_sync() would then block that worker on its own completion, pinning
 * the USB device lock forever, stalling hub_event in usb_disconnect() and freezing the
 * whole machine with no panic/kdump (2026-09-10). pre_reset/post_reset now keep the driver
 * bound, but this guard keeps the teardown paths safe regardless of who calls them.
 */
static void fl2000_watchdog_cancel(struct fl2000_stream *stream)
{
	if (current_work() == &stream->watchdog_work.work)
		return;

	stream->watchdog_armed = false;
	cancel_delayed_work_sync(&stream->watchdog_work);

	dev_info(&stream->usb_dev->dev, "stall watchdog cancelled");
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	UNUSED(dev);

	if (!stream || IS_ERR(stream))
		return;
	fl2000_watchdog_cancel(stream);
	if (stream->work_queue) {
		fl2000_stream_disable(stream);
		destroy_workqueue(stream->work_queue);
		stream->work_queue = NULL;
	}
	if (stream->watchdog_wq) {
		destroy_workqueue(stream->watchdog_wq);
		stream->watchdog_wq = NULL;
	}
	fl2000_stream_put_buffers(stream);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;
	bool frame_done = false;

	/* Desancora primeiro: sem isso a âncora acumulava todos os URBs já
	 * completados (leak) e usb_wait_anchor_empty_timeout() no disable
	 * nunca esvaziava — todo disable/modeset travava 1 s com todos os
	 * modeset locks presos, congelando todas as telas. Com o unanchor a
	 * âncora rastreia só os URBs em voo (0-2) e o disable retorna rápido.
	 */
	usb_unanchor_urb(urb);

	if (stream) {
		unsigned long flags;

		/* Um frame pode ter sido dividido em varios chunks. O buffer só
		 * volta para a lista de render - e o vblank só é gerado - quando
		 * o ULTIMO chunk completa: antes disso o chip ainda está
		 * consumindo este frame.
		 */
		cur_sb->chunk_done++;
		atomic_dec(&stream->chunks_inflight);

		if (cur_sb->chunk_done >= cur_sb->n_chunks) {
			frame_done = true;

			spin_lock_irqsave(&stream->list_lock, flags);
			list_move_tail(&cur_sb->list, &stream->render_list);
			spin_unlock_irqrestore(&stream->list_lock, flags);
		}

		/* Only a SUCCESSFUL completion proves the pipeline is delivering pixels.
		 * Error completions (e.g. an -EPROTO flood while the chip's streaming
		 * engine is wedged) still call back at frame rate and previously kept
		 * refreshing the stall timer, so the watchdog never fired: black screen,
		 * thousands of logged errors, no recovery, manual replug required. Leave
		 * the timer alone on errors so the ladder (unlink -> endpoint reset ->
		 * device reset) runs and self-heals.
		 */
		if (!urb->status)
			stream->last_completion = jiffies;
		stream->last_urb_status = urb->status;
		stream->completion_count++;
		if (urb->status)
			stream->error_count++;

		if (frame_done)
			drm_crtc_handle_vblank(stream->crtc);

		/* Kick transmit workqueue */
		up(&stream->work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

/* Escolhe o proximo frame a enviar: um buffer ja pronto (transmit_list) ou,
 * se nao houver, um buffer livre com uma copia do ultimo frame enviado, para
 * que o stream continue mesmo sem damage do compositor. O buffer entra na
 * wait_list (frames em voo) e volta para a render_list quando o ULTIMO chunk
 * completa.
 */
static struct fl2000_stream_buf *fl2000_stream_take_buffer(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *last_sb = NULL;
	unsigned long flags;
	bool need_copy;
	size_t copy_size = 0;

	spin_lock_irqsave(&stream->list_lock, flags);

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
			return NULL;
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
	}

	spin_lock_irqsave(&stream->list_lock, flags);
	if (!stream->enabled) {
		list_add_tail(&cur_sb->list, &stream->render_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);
		return NULL;
	}
	list_add_tail(&cur_sb->list, &stream->wait_list);
	spin_unlock_irqrestore(&stream->list_lock, flags);

	cur_sb->chunk_next = 0;
	cur_sb->chunk_done = 0;

	return cur_sb;
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct urb *data_urb;

	/* Streaming é bulk contínuo a 60fps: não compete com a UI. Roda com
	 * nice +5 para o compositor continuar fluido sob carga (Hybris/IDE).
	 */
	set_user_nice(current, 5);

	while (stream->enabled) {
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

		/* Envia chunks até o tubo encher.
		 *
		 * Os chunks de um frame precisam chegar ao chip em ordem, então o
		 * frame em curso fica em stream->cur_buf até que todos os seus
		 * chunks tenham saído; só depois começa o próximo frame. Com o
		 * chunking desligado (n_chunks == 1) isto é exatamente o
		 * comportamento antigo: um URB por frame.
		 *
		 * O throttle conta CHUNKS em voo, não frames: é o que limita
		 * quantos bytes o controlador está guardando para nós de uma vez.
		 */
		while (atomic_read(&stream->chunks_inflight) < FL2000_MAX_INFLIGHT) {
			unsigned int idx;
			unsigned int len;

			cur_sb = stream->cur_buf;
			if (!cur_sb || cur_sb->chunk_next >= cur_sb->n_chunks) {
				cur_sb = fl2000_stream_take_buffer(stream);
				if (!cur_sb)
					break;
				stream->cur_buf = cur_sb;
			}

			idx = cur_sb->chunk_next;
			len = cur_sb->chunk_bytes;
			if ((size_t)(idx + 1) * len > stream->buf_size)
				len = (unsigned int)(stream->buf_size - (size_t)idx * len);

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
					  (int)len, fl2000_stream_data_completion, stream);
			data_urb->interval = 0;
			data_urb->sg = cur_sb->chunk_sgt[idx].sgl;
			data_urb->num_sgs = cur_sb->chunk_sgt[idx].nents;
			/* Só o ÚLTIMO chunk do frame leva o ZLP: é ele que fecha o frame
			 * para o chip. Um ZLP no meio seria lido como fim de frame.
			 */
			if (idx + 1 >= cur_sb->n_chunks)
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
				break;
			}

			cur_sb->chunk_next++;
			atomic_inc(&stream->chunks_inflight);
		}
	}
}

/* Full in-place USB device reset, the recovery that actually revives a wedged chip: the
 * streaming engine is dead and nothing short of a port reset (what a physical replug does)
 * restarts it. Performed in place because the USB driver's pre_reset/post_reset hooks keep
 * this interface bound - without both hooks usb_reset_device() force-unbinds it, and the
 * DRM shutdown runs synchronously inside the caller's work item; fl2000_stream_disable()
 * then waits on the watchdog work item that is executing it (self-deadlock holding the USB
 * device lock, freezing the machine with no panic/kdump, 2026-09-10).
 *
 * Keeping the interface bound is also what makes this invisible to userspace: the DRM
 * device is never unregistered, the minor does not change, and the compositor keeps the
 * output it already had. That is the whole difference between a stall that recovers by
 * itself and one that leaves the display black until logout.
 *
 * Returns 0 if the reset went through and streaming was restarted.
 */
static int fl2000_stream_reset_device(struct fl2000_stream *stream)
{
	struct usb_device *usb_dev = stream->usb_dev;
	unsigned long flags;
	int ret;

	/* Park the transmit worker first: it must not submit URBs across the reset */
	stream->enabled = false;
	up(&stream->work_sem);
	drain_workqueue(stream->work_queue);

	ret = usb_lock_device_for_reset(usb_dev, NULL);
	if (ret < 0) {
		dev_err(&usb_dev->dev, "Cannot lock device for reset (%d)", ret);
		goto failed;
	}

	ret = usb_reset_device(usb_dev);
	if (ret)
		dev_err(&usb_dev->dev, "USB device reset failed (%d)", ret);
	usb_unlock_device(usb_dev);

	if (ret)
		goto failed;

	/* post_reset() already reprogrammed the chip and restarted the aborted
	 * interrupt URB. Drop any giveback straggler the reset left anchored,
	 * then restart the streaming session.
	 */
	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000)) {
		usb_poison_anchored_urbs(&stream->anchor);
		usb_scuttle_anchored_urbs(&stream->anchor);
	}
	init_usb_anchor(&stream->anchor);

	/* The chunks that were on the wire are gone with the reset: the chip was
	 * reprogrammed from scratch, so a frame that was half sent must not be
	 * finished. Put every frame back in the render list with its chunk
	 * counters cleared, and zero the in-flight counter - the poisoned URBs
	 * never complete, so nothing else would ever decrement it and the
	 * throttle would wedge the stream for good.
	 */
	spin_lock_irqsave(&stream->list_lock, flags);
	while (!list_empty(&stream->wait_list)) {
		struct fl2000_stream_buf *sb;

		sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf,
				     list);
		list_del(&sb->list);
		sb->chunk_next = 0;
		sb->chunk_done = 0;
		list_add_tail(&sb->list, &stream->render_list);
	}
	spin_unlock_irqrestore(&stream->list_lock, flags);

	stream->cur_buf = NULL;
	atomic_set(&stream->chunks_inflight, 0);

	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	stream->last_completion = jiffies;
	stream->stall_recoveries = 0;
	stream->gave_up = false;
	stream->last_urb_status = 0;
	stream->completion_count = 0;
	stream->error_count = 0;
	queue_work(stream->work_queue, &stream->work);
	for (int i = 0; i < FL2000_MAX_INFLIGHT; i++)
		up(&stream->work_sem);

	/* Closing line of the whole recovery. Without it "reprogramming chip mode" was the
	 * last thing dmesg ever showed, which made a successful reset and a reset that left
	 * the chip dark indistinguishable after the fact (incident 2026-10-16 09:33).
	 */
	dev_err(&usb_dev->dev,
		"USB device reset done, stream re-enabled and %d URBs queued: if the monitor stays dark the chip did not answer the reprogram",
		FL2000_MAX_INFLIGHT);

	return 0;

failed:
	/* Hand the worker and the watchdog back rather than leaving enabled cleared,
	 * which would silence both for the rest of the session with nothing in dmesg to
	 * say why. The stream is still stalled, so the next tick retries and the
	 * ladder eventually reaches the give-up log line
	 */
	stream->enabled = true;
	up(&stream->work_sem);

	return ret;
}

/* Runs on its own workqueue: the transmit worker parks inside its work function while
 * waiting on work_sem, so an ordered queue shared with it would never get to run this
 */
static void fl2000_stream_watchdog(struct work_struct *work)
{
	struct fl2000_stream *stream =
		container_of(work, struct fl2000_stream, watchdog_work.work);
	unsigned int inflight = 0;
	unsigned int inflight_chunks = 0;
	unsigned long flags;
	struct fl2000_stream_buf *tmp;
	long stalled_ms;

	/* The watchdog was cancelled: the stream is being torn down (or was re-enabled
	 * and re-armed elsewhere). Do not re-arm here - a delayed work that re-arms
	 * itself from inside its own run survives cancel_delayed_work_sync(), so a
	 * cancelled watchdog that re-armed would tick forever on a dead stream.
	 */
	if (!stream->watchdog_armed) {
		dev_info(&stream->usb_dev->dev,
			"stall watchdog tick after cancellation, not re-arming");
		return;
	}

	/* A tick that lands while the stream is disabled must not be the last one. Returning
	 * here unarmed killed the recovery ladder for the rest of the session, silently:
	 * fl2000_drm_pre_reset() clears 'enabled' through fl2000_stream_disable() on every
	 * device-reset rung, and a tick in that window left the chip dark with no ladder, no
	 * give-up line and no watchdog thread left in /proc to notice (incident 2026-10-16
	 * 09:33: rung 4 ran, "reprogramming chip mode" was the last line the driver ever
	 * printed, the transmit worker parked in down_interruptible for the rest of the day
	 * and only a physical replug brought the monitor back).
	 */
	if (!stream->enabled) {
		dev_warn_ratelimited(&stream->usb_dev->dev,
			"watchdog tick with the stream disabled, re-arming");
		goto rearm;
	}

	if (!time_after(jiffies, stream->last_completion + FL2000_STALL_TIMEOUT)) {
		if (stream->stall_recoveries) {
			stream->stall_recoveries = 0;
			if (stream->gave_up) {
				stream->gave_up = false;
				dev_info(&stream->usb_dev->dev, "stream resumed");
			}
		}
		goto rearm;
	}

	stalled_ms = jiffies_to_msecs(jiffies - stream->last_completion);

	spin_lock_irqsave(&stream->list_lock, flags);
	list_for_each_entry(tmp, &stream->wait_list, list)
		inflight++;
	spin_unlock_irqrestore(&stream->list_lock, flags);

	/* How many chunks are actually on the wire. With the frame split, this
	 * is the number that says how much data the controller is holding for us.
	 */
	inflight_chunks = atomic_read(&stream->chunks_inflight);

	if (stream->gave_up)
		goto rearm;

	stream->stall_recoveries++;

	if (stream->stall_recoveries <= FL2000_STALL_SOFT_TRIES) {
		struct urb *stuck;
		int status_ret;

		dev_err(&stream->usb_dev->dev,
			"stream stall: no URB completion for %ld ms (%u frames, %u chunks in flight), soft recovery %u (last status %d, %u completions, %u errors)",
			stalled_ms, inflight, inflight_chunks, stream->stall_recoveries, stream->last_urb_status,
			stream->completion_count, stream->error_count);

		/* Snapshot the chip's own status register: control transfers still work during
		 * this wedge, so this is the chip's view of the dead output - the evidence every
		 * previous occurrence never captured
		 */
		status_ret = fl2000_log_vga_status(stream->usb_dev);

		/* ...unless they do not. A control transfer failing while the bulk
		 * endpoint is already dead means the port or the chip is failing, not that
		 * the endpoint is starved, so unlinking URBs and resetting the endpoint
		 * below can only fail - and paying a watchdog tick per rung to find that
		 * out is fatal, because the chip drops off the bus in the meantime. When it
		 * does, the device re-enumerates on a different DRM minor and the display
		 * stays black until the session restarts. Take the in-place reset now,
		 * same work item: pre_reset/post_reset keep the interface bound, so the
		 * compositor never sees the device leave.
		 */
		if (status_ret) {
			dev_err(&stream->usb_dev->dev,
				"stall with a failing VGA status read (%d), escalating straight to USB device reset recovery",
				status_ret);

			fl2000_stream_reset_device(stream);
			goto rearm;
		}

		/* Ask the HCD to cancel the stuck URBs. Never poison/kill: both block
		 * until the HCD gives every URB back, which never happens on a wedged
		 * port and would pin this worker forever. Each URB is popped from the
		 * anchor (the giveback's unanchor becomes a no-op) and unlinked
		 * asynchronously; when a giveback lands, the completion returns the
		 * buffer and kicks the worker.
		 */
		while ((stuck = usb_get_from_anchor(&stream->anchor)) != NULL)
			usb_unlink_urb(stuck);

		/* Restart the self-sustaining transmit loop with whatever buffers are on
		 * render_list once the givebacks land
		 */
		up(&stream->work_sem);
		up(&stream->work_sem);
	} else if (stream->stall_recoveries <=
		   FL2000_STALL_SOFT_TRIES + FL2000_STALL_EP_TRIES) {
		dev_err(&stream->usb_dev->dev,
			"stream stall persists, endpoint reset recovery %u",
			stream->stall_recoveries);

		/* Only meaningful after the unlink givebacks emptied the ring; the HCD
		 * refuses ("EP not empty") while TDs are still pending, which is fine -
		 * the device reset rung below handles that case.
		 */
		usb_reset_endpoint(stream->usb_dev, 0x01);

		up(&stream->work_sem);
		up(&stream->work_sem);
	} else if (stream->stall_recoveries <=
		   FL2000_STALL_SOFT_TRIES + FL2000_STALL_EP_TRIES + FL2000_STALL_RESET_TRIES) {
		dev_err(&stream->usb_dev->dev,
			"stream stall persists, USB device reset recovery %u",
			stream->stall_recoveries);

		/* The distinguishing signature: control transfers still complete (chip
		 * app-reset succeeds) while bulk completions never resume. The chip's
		 * streaming engine is wedged and only a port reset - exactly what a
		 * physical replug does - revives it. Do that in software, in place, so the
		 * interface stays bound and the compositor never sees the device leave.
		 */
		fl2000_stream_reset_device(stream);
	} else {
		stream->gave_up = true;
		dev_err(&stream->usb_dev->dev,
			"stream stalled for %ld ms, %u recoveries failed: giving up, unplug and replug the dongle to recover the display",
			stalled_ms, stream->stall_recoveries - 1);
	}

rearm:
	/* mod_delayed_work(), NOT queue_delayed_work().
	 *
	 * Re-arming a delayed work from inside its own callback is exactly the case
	 * queue_delayed_work() cannot serve: the timer is detached while its callback
	 * runs, so the internal timer_delete_sync() fails and the re-arm is silently
	 * dropped. The symptom was a watchdog that ran a few ticks and then vanished
	 * - its workqueue worker thread came and went (observed at 2026-10-28
	 * 16:07, present; 16:08, gone; 16:25, gone again; 16:26, back), with not a
	 * single line in dmesg, because the watchdog logs nothing when the stream is
	 * healthy.
	 *
	 * mod_delayed_work() is the API for this: it re-arms the timer of a work
	 * that is pending or running, from inside the callback.
	 *
	 * Its return value is the opposite of what it looks like: mod_delayed_work()
	 * returns FALSE when the work was IDLE (which is the normal case here - the
	 * work was consumed by the tick that just ran, so this call queues it fresh,
	 * i.e. SUCCESS) and TRUE when it was pending and only had its timer moved.
	 * So only the TRUE case needs a complaint.
	 */
	if (mod_delayed_work(stream->watchdog_wq, &stream->watchdog_work,
			     FL2000_WATCHDOG_PERIOD)) {
		dev_warn_ratelimited(&stream->usb_dev->dev,
			"stall watchdog re-armed while still pending");
	}

	/* Heartbeat. A healthy watchdog used to be completely invisible: no log, no
	 * thread you could count on, no way to tell "running" from "dead". Once a
	 * minute, say so - and say which thread is doing the ticking, because the
	 * workqueue worker for this pool is not the one you would expect (see
	 * NOTES.md, session 12: no worker named fl2000_stall ever showed up).
	 */
	stream->watchdog_ticks++;
	if (!(stream->watchdog_ticks % 60)) {
		dev_info(&stream->usb_dev->dev,
			"stall watchdog alive: %u ticks, %u completions, %u errors (tid %d, %s)",
			stream->watchdog_ticks, stream->completion_count,
			stream->error_count, task_pid_nr(current), current->comm);
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

	if (!stream || IS_ERR(stream) || !src)
		return;

	/* Defensive: never walk past the allocated buffers even if a caller passes
	 * geometry that does not match the current mode (the OOB page fault of kdump
	 * 202609040702 came through exactly such a mismatch)
	 */
	if ((u64)height * width * stream->bytes_pix > stream->buf_size) {
		pr_err_ratelimited("fl2000: compress geometry %ux%u x%u exceeds buffer %zu, dropping frame\n",
				   width, height, stream->bytes_pix, stream->buf_size);
		return;
	}

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

	if (!stream || IS_ERR(stream))
		return -ENODEV;
	if (!pixels || !bytes_pix)
		return -EINVAL;

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

	/* Say how the frame was split: with chunking on, one frame is several
	 * bulk transfers, and that is what the Bluetooth interference story is
	 * about (see FL2000_CHUNK_BYTES).
	 */
	{
		struct fl2000_stream_buf *sb = NULL;

		spin_lock_irqsave(&stream->list_lock, flags);
		if (!list_empty(&stream->render_list))
			sb = list_first_entry(&stream->render_list,
					      struct fl2000_stream_buf, list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		if (sb) {
			dev_info(&stream->usb_dev->dev,
				"frame of %u bytes split into %u chunks of %u bytes",
				size, sb->n_chunks, sb->chunk_bytes);
		}
	}

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	if (!stream || IS_ERR(stream))
		return -ENODEV;
	if (!stream->work_queue)
		return -ENODEV;
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
	stream->last_completion = jiffies;
	stream->stall_recoveries = 0;
	stream->gave_up = false;
	stream->last_urb_status = 0;
	stream->completion_count = 0;
	stream->error_count = 0;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (int i = 0; i < FL2000_MAX_INFLIGHT; i++)
		up(&stream->work_sem);

	/* Arm the stall watchdog for the whole streaming session.
	 *
	 * queue_delayed_work() returns false if the work was already pending, which
	 * would silently skip the arming; log it.
	 */
	stream->watchdog_armed = true;
	stream->watchdog_ticks = 0;
	stream->cur_buf = NULL;
	atomic_set(&stream->chunks_inflight, 0);
	if (!queue_delayed_work(stream->watchdog_wq, &stream->watchdog_work,
				FL2000_WATCHDOG_PERIOD)) {
		dev_err(&stream->usb_dev->dev,
			"stall watchdog was already pending when arming - the display will not self-heal");
	}

	dev_info(&stream->usb_dev->dev,
		"stream enabled: stall watchdog armed (tick every %lu ms)",
		(unsigned long)FL2000_WATCHDOG_PERIOD);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	unsigned long flags;

	if (!stream || IS_ERR(stream))
		return;
	if (!stream->work_queue)
		return;

	stream->enabled = false;

	/* The frame being chunked out is gone: drop the pointer so the worker
	 * cannot touch a buffer that the mode set is about to free.
	 */
	stream->cur_buf = NULL;

	/* Stop the watchdog first: it must not attempt recovery against a stream that is
	 * being torn down (its 'enabled' check races a running iteration)
	 */
	fl2000_watchdog_cancel(stream);

	dev_info(&stream->usb_dev->dev, "stream disabled");

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

	spin_lock_irqsave(&stream->list_lock, flags);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock_irqrestore(&stream->list_lock, flags);
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

	/* devres_alloc() hands back kmalloc() memory - NOT zeroed.
	 *
	 * INIT_WORK() clears the work flags, so the plain work item was fine, but
	 * INIT_DELAYED_WORK() only fills the timer's callback and flags: everything
	 * else the timer owns (including its lock) stays whatever was lying in that
	 * slab. A delayed work armed on garbage memory silently never fires.
	 *
	 * That is precisely what happened, in every session: the stall watchdog was
	 * armed - the log said "stream enabled: stall watchdog armed" - and never ran
	 * a single tick. No fl2000_stall worker thread ever existed, the recovery
	 * ladder that revives a wedged chip never executed, and the display stayed
	 * black until a physical replug (2026-10-16 09:33, 2026-10-28 12:06).
	 *
	 * The kernel's contract for INIT_WORK/INIT_DELAYED_WORK is "zero
	 * initialization followed by setting the function pointer", and
	 * timer_setup() (which does memset) is the alternative. Zero the struct.
	 */
	memset(stream, 0, sizeof(*stream));

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_DELAYED_WORK(&stream->watchdog_work, &fl2000_stream_watchdog);
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

	/* Watchdog em fila própria: o worker de transmissão estaciona dentro da
	 * própria work function esperando o semáforo, então uma fila ordenada
	 * compartilhada nunca rodaria o watchdog. Freezable para não "recuperar"
	 * um stream congelado durante suspend.
	 */
	stream->watchdog_wq = alloc_ordered_workqueue("fl2000_stall",
						      WQ_FREEZABLE);
	if (!stream->watchdog_wq) {
		dev_err(&usb_dev->dev, "Allocate stall watchdog workqueue failed");
		devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}

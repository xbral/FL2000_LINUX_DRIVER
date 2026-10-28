#!/usr/bin/env python3
"""Diagnostico A/B da travada do mouse Bluetooth: link BT vs. carga do fl2000.

Por que nao serve medir so o input layer
---------------------------------------
O timestamp do input_event e o instante em que o evento chegou ao input core.
Se o usuario simplesmente parou de mexer o mouse, o intervalo entre dois
eventos fica grande - e parece travada, mas nao e. Medir so ali erra.

Por que medir no nivel HCI
--------------------------
O btmon marca o instante em que o host recebeu o pacote do controlador. Ele
cobre a cadeia inteira ate o socket HCI: RF -> controlador -> USB bulk -> host.
Alinhar as duas timelines (HCI e input) separa as duas causas:

  * atraso grande entre o pacote HCI e o evento de input  -> o host atrasou
  * buraco grande na timeline do HCI                       -> o link atrasou

A metrica que independe do movimento da mao
--------------------------------------------
Um buraco no HCI seguido de uma rajada (varios pacotes com menos de BURST_MS
entre si) significa que os reports ficaram retidos em algum lugar e foram
entregues de uma vez. Esse e o "grudinho" que o usuario sente. Um buraco sem
rajada depois dele e so o mouse parado.

Uso: sudo scripts/bt_stutter_ab.py [segundos] [rotulo]
"""
import bisect
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

GAP_MS = 100.0     # buraco no HCI grande demais para ser normal
BURST_MS = 5.0     # pacotes colados: sinal de fila retida
SEC = struct.Struct("qqHHi")
RECLEN = SEC.size
EV_REL, EV_KEY = 2, 1
RX_RE = re.compile(r'^> ACL Data RX: Handle (\d+).*?([\d.]+)$')


def find_mouse_device():
    """Acha o eventN do mouse Bluetooth pelo endereco BD_ADDR em Uniq=."""
    path = "/proc/bus/input/devices"
    addr = None
    dev = None
    try:
        text = open(path).read()
    except OSError:
        return None
    blocks = text.split("\n\n")
    for b in blocks:
        if "Uniq=" in b and "input" in b:
            m = re.search(r"Uniq=([0-9a-fA-F:]+)", b)
            if m:
                addr = m.group(1)
                h = re.search(r"Handlers=.*\bevent(\d+)", b)
                if h:
                    name = re.search(r'N: Name="([^"]+)"', b)
                    dev = ("/dev/input/event" + h.group(1),
                           name.group(1) if name else "?",
                           addr)
                    break
    return dev


def fl2000_cpu_seconds():
    """CPU consumida por todos os threads do fl2000, em segundos.

    Agrega por NOME do thread, nao por tid: o worker do watchdog troca de tid
    (o trabalho vai para o pool compartilhado do no, cujos workers sao recriados),
    e um delta por tid daria negativo quando o tid e' reciclado.
    """
    total = 0.0
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        tdir = "/proc/%s/task" % pid
        try:
            tids = os.listdir(tdir)
        except OSError:
            continue
        for tid in tids:
            try:
                with open("%s/%s/stat" % (tdir, tid)) as f:
                    stat = f.read()
                comm = stat[stat.index("(") + 1:stat.rindex(")")]
                if "fl2000" not in comm:
                    continue
                fields = stat[stat.rindex(")") + 2:].split()
                total += (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
            except (OSError, ValueError, IndexError):
                continue
    return total


def fl2000_watchdog_alive():
    """O watchdog e' melhor julgado pelo heartbeat do driver do que pela thread:
    a thread do workqueue 'fl2000_stall' nao aparece no /proc (ver NOTES.md,
    session 12), mas o heartbeat 'stall watchdog alive' no dmesg prova que ele
    esta ticking.
    """
    try:
        out = subprocess.run(["journalctl", "-k", "-b", "--no-pager"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return "desconhecido"
    if "stall watchdog alive" in out:
        return "vivo"
    if "stall watchdog armed" in out:
        return "armado"
    return "AUSENTE"


def read_events(dev, seconds):
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    ts = []
    end = time.monotonic() + seconds
    try:
        while time.monotonic() < end:
            try:
                data = os.read(fd, RECLEN * 512)
            except BlockingIOError:
                time.sleep(0.002)
                continue
            if not data:
                time.sleep(0.002)
                continue
            for off in range(0, len(data) - RECLEN + 1, RECLEN):
                sec, usec, typ, code, val = SEC.unpack_from(data, off)
                if typ in (EV_REL, EV_KEY) and val != 0:
                    ts.append(sec + usec / 1e6)
    finally:
        os.close(fd)
    return ts


def parse_hci(path):
    ts = []
    for line in open(path, errors="replace"):
        m = RX_RE.match(line)
        if m:
            ts.append(float(m.group(2)))
    ts.sort()
    return ts


def analyse(hci, ev):
    out = {}
    if len(hci) < 3 or len(ev) < 3:
        out["erro"] = "poucos dados (hci=%d ev=%d)" % (len(hci), len(ev))
        return out

    # ---- timeline do HCI -------------------------------------------------
    gaps = [(b - a) * 1000.0 for a, b in zip(hci, hci[1:])]
    big = [i for i, g in enumerate(gaps) if g > GAP_MS]

    # ---- rajadas depois de um buraco: a assinatura da travada -------------
    held = 0
    bursts = 0
    burst_detail = []
    for i in big:
        g = gaps[i]
        # os dois pacotes seguintes chegaram colados?
        j = i + 1
        colados = 0
        while j < len(hci) and (hci[j] - hci[j - 1]) * 1000.0 < BURST_MS:
            colados += 1
            j += 1
        if colados >= 2:
            bursts += 1
            held += g
            burst_detail.append((hci[i + 1] - hci[0], g, colados + 1))

    out["hci_packets"] = len(hci)
    out["hci_rate"] = len(hci) / (hci[-1] - hci[0])
    out["hci_gaps_big"] = len(big)
    out["hci_bursts"] = bursts
    out["hci_held_ms"] = held
    out["hci_max_gap_ms"] = max(gaps)
    out["burst_detail"] = burst_detail

    # ---- atraso HCI -> input --------------------------------------------
    # O offset bruto (primeiro evento - primeiro pacote) fica torto se o
    # usuario nao mexeu o mouse no comeco da janela. Em vez de confiar nele,
    # procura o offset que minimiza o atraso mediano - o mesmo pacote HCI
    # alimenta o evento de input, entao o delay real e de poucos ms.
    best = None
    ev_med = sorted(ev)[len(ev) // 2]
    for delta_ms in range(-2000, 2001, 5):
        offset = ev_med - (hci[len(hci) // 2] + delta_ms / 1000.0)
        ds = []
        for t in ev:
            tt = t - offset
            i = bisect.bisect_right(hci, tt) - 1
            if i >= 0:
                ds.append((tt - hci[i]) * 1000.0)
        if len(ds) < 10:
            continue
        ds.sort()
        med = ds[len(ds) // 2]
        if best is None or med < best[1]:
            best = (offset, med, ds)
    if best:
        offset, med, delays = best
        delays = sorted(delays)
        def pct(p):
            return delays[min(len(delays) - 1, int(len(delays) * p))]
        out["delay_p50"] = pct(.5)
        out["delay_p90"] = pct(.9)
        out["delay_p99"] = pct(.99)
        out["delay_max"] = delays[-1]
        out["delay_over_100ms"] = sum(1 for d in delays if d > 100)
        out["delay_offset"] = offset

    # ---- gaps do input layer (so descriptivo) ----------------------------
    igaps = [(b - a) * 1000.0 for a, b in zip(ev, ev[1:])]
    out["input_events"] = len(ev)
    out["input_rate"] = len(ev) / (ev[-1] - ev[0])
    out["input_gaps_20_500ms"] = sum(1 for g in igaps if 20 <= g < 500)
    out["input_gaps_over_500ms"] = sum(1 for g in igaps if g >= 500)
    return out


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
    label = sys.argv[2] if len(sys.argv) > 2 else "medida"

    mouse = find_mouse_device()
    if not mouse:
        print("nao encontrei o mouse Bluetooth em /proc/bus/input/devices")
        return 1
    dev, name, addr = mouse
    print("mouse: %s (%s) em %s" % (name, addr, dev))
    print("rodando %.0fs -- MEXA O MOUSE CONTINUAMENTE\n" % secs)

    tmp = tempfile.NamedTemporaryFile(prefix="btmon-", suffix=".txt", delete=False)
    cpu0 = fl2000_cpu_seconds()
    watchdog_before = fl2000_watchdog_alive()
    bt = subprocess.Popen(["btmon"], stdout=open(tmp.name, "w"),
                           stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        ev = read_events(dev, secs)
    finally:
        time.sleep(1.0)
        bt.terminate()
        try:
            bt.wait(timeout=5)
        except subprocess.TimeoutExpired:
            bt.kill()
    cpu1 = fl2000_cpu_seconds()
    cpu_delta = max(0.0, cpu1 - cpu0)

    hci = parse_hci(tmp.name)
    r = analyse(hci, ev)

    # guarda os timestamps crus para reanalise fora do script
    raw = os.path.join(tempfile.gettempdir(), "btab-events-%d.bin" % os.getpid())
    with open(raw, "wb") as f:
        import struct as _s
        f.write(b"".join(_s.pack("d", t) for t in ev))
    hciraw = os.path.join(tempfile.gettempdir(), "btab-hci-%d.bin" % os.getpid())
    with open(hciraw, "wb") as f:
        import struct as _s
        f.write(b"".join(_s.pack("d", t) for t in hci))

    print("=" * 72)
    print("RESULTADO: %s" % label)
    print("=" * 72)
    print("carga do fl2000 durante a janela: %.3f s de CPU  (watchdog %s)" %
          (cpu_delta, watchdog_before))
    if "erro" in r:
        print(r["erro"])
        return 1
    print("HCI: %d pacotes (%.1f/s)   input: %d eventos (%.1f/s)" %
          (r["hci_packets"], r["hci_rate"], r["input_events"], r["input_rate"]))
    # Sanity check: com pouco movimento do mouse o alinhamento entre as duas
    # timelines fica ambiguo e o atraso medido vira lixo (uma janela com o
    # dongle ocioso pode 'dar' pior que uma com o dongle transmitting).
    if r["input_rate"] < 60:
        print("  [AVISO] MOVIMENTO INSUFICIENTE (%.1f eventos/s): esta janela"
              " NAO e comparavel. Mova o mouse de forma continua e densa"
              " (circulos grandes) durante a janela." % r["input_rate"])
    print("buracos no HCI > %.0fms: %d" % (GAP_MS, r["hci_gaps_big"]))
    print("  deles seguidos de rajada (RETIDOS e entregues de uma vez): %d" % r["hci_bursts"])
    print("  tempo total retido nessas rajadas: %.0f ms" % r["hci_held_ms"])
    print("  maior buraco no HCI: %.0f ms" % r["hci_max_gap_ms"])
    print("atraso HCI -> input (ms): p50=%.1f p90=%.1f p99=%.1f max=%.0f  (>100ms: %d)" %
          (r.get("delay_p50", 0), r.get("delay_p90", 0), r.get("delay_p99", 0),
           r.get("delay_max", 0), r.get("delay_over_100ms", 0)))
    print("gaps do input layer: 20-500ms=%d  >500ms=%d" %
          (r["input_gaps_20_500ms"], r["input_gaps_over_500ms"]))
    if r["burst_detail"]:
        print("detalhe das rajadas (t, buraco, pacotes colados):")
        for t, g, n in r["burst_detail"][:15]:
            print("   t=+%.2fs  buraco=%.0fms  rajada de %d pacotes" % (t, g, n))
    print("(timestamps crus: %s | %s)" % (raw, hciraw))
    print("(captura cru: %s)" % tmp.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

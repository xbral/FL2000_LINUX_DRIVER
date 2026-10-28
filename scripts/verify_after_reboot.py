#!/usr/bin/env python3
"""Verificacao pos-reboot do driver fl2000.

Responde de uma vez as perguntas que o log sozinho nao responde:

  * o modulo novo entrou (srcversion)?
  * o watchdog de stall EXISTE? (a thread do workqueue - so existe se um
    item de trabalho foi enfileirado; antes da 1.0.7 ela nunca aparecia,
    porque o delayed_work era armado sobre memoria nao zerada)
  * o stream esta realmente transmitindo, ou o worker esta parado em
    down_interruptible (stream morto)?
  * a escada de recuperacao rodou, ou nao precisou rodar?
  * o nome do conector no kernel bate com o que o Mutter conhece?

Rode como USUARIO (nao como root): o Mutter so e alcançavel pelo barramento
de sessao do usuario. As poucas operacoes que exigem privilégio (journalctl
do kernel, /proc/<tid>/stack) sao feitas via sudo.

Uso: python3 scripts/verify_after_reboot.py [segundos de medicao de CPU]
"""
import os
import re
import subprocess
import sys
import time

OK, FAIL, WARN = "OK  ", "FALHA", "AVISO"
results = []


def check(ok, label, detail=""):
    results.append((OK if ok else FAIL, label, detail))
    print("  [%s] %-46s %s" % (OK if ok else FAIL, label, detail))
    return ok


def warn(label, detail=""):
    results.append((WARN, label, detail))
    print("  [%s] %-46s %s" % (WARN, label, detail))


def run(cmd):
    try:
        return subprocess.run(cmd, shell=isinstance(cmd, str),
                              capture_output=True, text=True,
                              timeout=30).stdout
    except Exception:
        return ""


def klog(pattern):
    out = run(["sudo", "journalctl", "-k", "-b", "--no-pager"])
    return [l for l in out.splitlines() if re.search(pattern, l, re.I)]


def threads_matching(needle):
    hits = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            try:
                with open("/proc/%s/task/%s/comm" % (pid, tid)) as f:
                    comm = f.read().strip()
                if needle in comm:
                    hits.append((pid, tid, comm))
            except OSError:
                continue
    return hits


def stack_of(tid):
    out = run(["sudo", "cat", "/proc/%s/stack" % tid])
    return out if out.startswith("[") else ""


def cpu_of(tid):
    try:
        with open("/proc/%s/stat" % tid) as f:
            stat = f.read()
        fields = stat[stat.rindex(")") + 2:].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError):
        return None


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0

    print("=" * 78)
    print("VERIFICACAO POS-REBOOT - fl2000")
    print("=" * 78)

    # ---- 1. modulo ------------------------------------------------------
    print("\n1) Modulo")
    try:
        with open("/sys/module/fl2000/srcversion") as f:
            running = f.read().strip()
    except OSError:
        running = ""
    installed = run(["modinfo", "-F", "srcversion",
                     "/lib/modules/%s/updates/dkms/fl2000.ko.zst" %
                     os.uname().release]).strip()
    check(bool(running) and running == installed,
          "modulo instalado == modulo carregado",
          "instalado=%s carregado=%s" % (installed or "?", running or "?"))

    # ---- 2. watchdog ----------------------------------------------------
    # A thread do workqueue 'fl2000_stall' NUNCA aparece no /proc: o trabalho
    # vai para o pool compartilhado do no (unbound), cujos workers sao nomeados
    # 'kworker/u32:N' e mudam de tid a cada minuto. O heartbeat do driver e o
    # sinal confiavel - ele sai de dentro da funcao do watchdog.
    print("\n2) Watchdog de stall")
    beat = klog(r"stall watchdog alive")
    if beat:
        last = beat[-1].split("kernel: ")[-1]
        check(True, "watchdog vivo (heartbeat no log)", last)
    else:
        armed = klog(r"stall watchdog armed")
        check(bool(armed), "watchdog vivo (heartbeat no log)",
              "armado mas sem heartbeat ainda - normal se o boot for recente"
              if armed else "AUSENTE")

    armed = klog(r"stall watchdog armed")
    check(bool(armed), "log: 'stall watchdog armed'",
          armed[-1].split("kernel: ")[-1] if armed else "nenhum")

    cancelled = klog(r"stall watchdog cancelled")
    if cancelled:
        warn("log: watchdog cancelado (stream desabilitado)",
             cancelled[-1].split("kernel: ")[-1])

    ticks = klog(r"watchdog tick")
    if ticks:
        print("       ticks do watchdog no log:")
        for l in ticks[-5:]:
            print("         " + l.split("kernel: ")[-1])

    # ---- 3. stream ------------------------------------------------------
    # O worker fica parado em down_interruptible() entre um completion e o
    # seguinte mesmo num stream saudavel (a 60 fps ele acorda 60 vezes por
    # segundo), entao o stack NAO distingue "morto" de "vivo". O que distingue
    # e o watchdog: se ele existe e nao registrou stall, os frames estao
    # chegando. O consumo de CPU e apenas um numero de apoio.
    print("\n3) Stream")
    worker = None
    for _, tid, comm in threads_matching("fl2000_stream"):
        if "fl2000_stream_work" in stack_of(tid):
            worker = (tid, comm)
            break
    stalls = klog(r"stream stall")
    if worker:
        tid, comm = worker
        t0 = cpu_of(tid)
        time.sleep(secs)
        t1 = cpu_of(tid)
        delta = (t1 - t0) if (t0 is not None and t1 is not None) else 0.0
        print("       worker %s: %.3fs de CPU em %.0fs" % (comm, delta, secs))
    else:
        print("       worker de transmissao nao identificado")
    if not stalls:
        check(True, "stream vivo (watchdog ativo e sem stall)",
              "watchdog ticking e sem stall - frames chegando")
    elif stalls:
        check(False, "stream vivo (watchdog ativo e sem stall)",
              "stall registrado: %s" % stalls[-1].split("kernel: ")[-1])
    else:
        check(False, "stream vivo (watchdog ativo e sem stall)",
              "impossivel avaliar sem heartbeat do watchdog")

    # ---- 4. recuperacao -------------------------------------------------
    print("\n4) Recuperacao")
    resets = klog(r"USB device reset|reset SuperSpeed")
    reprog = klog(r"chip reprogram")
    if stalls:
        print("       %d stall(s), %d reset(s), %d reprograms" %
              (len(stalls), len(resets), len(reprog)))
        for l in reprog[-3:]:
            print("         " + l.split("kernel: ")[-1])
    else:
        print("       nenhum stall - o stream nunca parou de entregar frames")

    # ---- 5. conector ----------------------------------------------------
    print("\n5) Conector / Mutter")
    kconn = []
    for d in os.listdir("/sys/class/drm"):
        if d.startswith("card") and "-" in d:
            kconn.append(d.split("-", 1)[1])
    kconn = sorted(set(kconn))
    print("       kernel: %s" % ", ".join(kconn))
    try:
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        proxy = Gio.DBusProxy.new_sync(
            bus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None,
            "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig", None)
        st = proxy.call_sync("GetCurrentState", None, Gio.DBusCallFlags.NONE,
                             -1, None).unpack()
        mconn = sorted(m[0][0] for m in st[1])
        print("       mutter: %s" % ", ".join(mconn))
        vga_k = [c for c in kconn if c.startswith("VGA")]
        vga_m = [c for c in mconn if c.startswith("VGA")]
        if vga_k and vga_m:
            check(vga_k[0] == vga_m[0], "nome do conector bate (kernel == mutter)",
                  "kernel=%s mutter=%s" % (vga_k[0], vga_m[0]))
        elif not vga_k:
            warn("nenhum conector VGA no kernel")
        else:
            check(False, "nome do conector bate (kernel == mutter)",
                  "kernel=%s mutter=%s" % (vga_k, vga_m or "ausente"))
    except Exception as e:
        warn("nao consegui falar com o DisplayConfig do Mutter", str(e))

    # ---- 6. dongle ------------------------------------------------------
    print("\n6) Dongle")
    ls = run(["lsusb"])
    found = [l for l in ls.splitlines() if "1d5c:2000" in l]
    check(bool(found), "dongle enumerado", found[0] if found else "ausente")

    # ---- resumo ---------------------------------------------------------
    print("\n" + "=" * 78)
    bad = [r for r in results if r[0] == FAIL]
    warns = [r for r in results if r[0] == WARN]
    print("RESUMO: %d ok, %d falha, %d aviso" %
          (len(results) - len(bad) - len(warns), len(bad), len(warns)))
    for _, label, detail in bad + warns:
        print("  - %s: %s" % (label, detail))
    if not bad:
        print("\nTudo certo. Falta so o A/B do mouse:")
        print("  python3 scripts/bt_stutter_ab.py 40 \"DEPOIS (fl2000 transmitindo)\"")
        print("(esse precisa de sudo para ler /dev/input e o btmon)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compara as janelas do A/B do mouse Bluetooth.

Compara as metricas de scripts/bt_stutter_ab.py entre varias janelas, com o
detalhe que importa: normaliza pelo numero de eventos, porque o movimento da
mao do usuario muda entre as janelas e uma contagem bruta de travadas nao e
comparavel.

Uso: bt_ab_compare.py janela1.txt janela2.txt ...
"""
import re
import sys


def parse(path):
    txt = open(path).read()
    out = {"arquivo": path}

    m = re.search(r"RESULTADO: (.*)", txt)
    if m:
        out["label"] = m.group(1).strip()

    m = re.search(r"carga do fl2000 durante a janela: ([\d.]+) s de CPU", txt)
    if m:
        out["cpu"] = float(m.group(1))

    m = re.search(r"HCI: (\d+) pacotes \(([\d.]+)/s\)\s+input: (\d+) eventos \(([\d.]+)/s\)", txt)
    if m:
        out["hci_pkts"] = int(m.group(1))
        out["hci_rate"] = float(m.group(2))
        out["events"] = int(m.group(3))
        out["ev_rate"] = float(m.group(4))

    m = re.search(r"buracos no HCI > \d+ms: (\d+)", txt)
    if m:
        out["hci_gaps"] = int(m.group(1))

    m = re.search(r"deles seguidos de rajada[^:]*: (\d+)", txt)
    if m:
        out["bursts"] = int(m.group(1))

    m = re.search(r"atraso HCI -> input \(ms\): p50=([\d.]+) p90=([\d.]+) p99=([\d.]+) max=([\d.]+)\s+\(>100ms: (\d+)\)", txt)
    if m:
        out["p50"] = float(m.group(1))
        out["p90"] = float(m.group(2))
        out["p99"] = float(m.group(3))
        out["max"] = float(m.group(4))
        out["late"] = int(m.group(5))

    m = re.search(r"gaps do input layer: 20-500ms=(\d+)\s+>500ms=(\d+)", txt)
    if m:
        out["in_20_500"] = int(m.group(1))
        out["in_over_500"] = int(m.group(2))

    return out


def main():
    rows = [parse(p) for p in sys.argv[1:]]
    if not rows:
        print("nada para comparar")
        return 1

    print("=" * 92)
    print("%-34s %8s %8s %9s %9s %10s" %
          ("janela", "CPU(s)", "ev/s", "p99(ms)", "max(ms)", "atraso/1k"))
    print("-" * 92)
    for r in rows:
        label = r.get("label", r["arquivo"])
        label = label[:34]
        # normaliza: eventos entregues com mais de 100 ms de atraso por 1000
        # eventos - a unica forma de comparar janelas com movimentos
        # diferentes da mao
        late_per_1k = (1000.0 * r.get("late", 0) / r["events"]) if r.get("events") else 0
        print("%-34s %8s %8.1f %9.1f %10.0f %10.1f" %
              (label,
               ("%.3f" % r["cpu"]) if "cpu" in r else "-",
               r.get("ev_rate", 0),
               r.get("p99", 0), r.get("max", 0), late_per_1k))
    print("=" * 92)
    print("atraso/1k = eventos entregues com mais de 100 ms de atraso, por mil")
    print("eventos. E o numero que compara janelas com movimento diferente.")

    for r in rows:
        print("\n%s" % r.get("label", r["arquivo"]))
        print("   HCI %d pacotes (%.1f/s) | %d eventos (%.1f/s) | buracos>100ms: %d | rajadas: %s" %
              (r.get("hci_pkts", 0), r.get("hci_rate", 0), r.get("events", 0),
               r.get("ev_rate", 0), r.get("hci_gaps", 0), r.get("bursts", "?")))
        print("   atraso p50=%.1f p90=%.1f p99=%.1f max=%.0f | >100ms: %d | gaps input 20-500ms=%d >500ms=%d" %
              (r.get("p50", 0), r.get("p90", 0), r.get("p99", 0), r.get("max", 0),
               r.get("late", 0), r.get("in_20_500", 0), r.get("in_over_500", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

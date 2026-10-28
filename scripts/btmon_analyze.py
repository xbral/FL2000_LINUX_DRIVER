#!/usr/bin/env python3
"""Analisa uma captura do btmon: intervalos dos pacotes ACL RX e o modo do link.

O timestamp do btmon e o instante em que o host recebeu o pacote do
controlador, cobrindo a cadeia RF -> controlador -> USB bulk -> host. Buracos
aqui sao buracos do link, nao do host.

Uso: btmon_analyze.py <captura.txt>
"""
import re
import sys

path = sys.argv[1]

RX = re.compile(r'^> ACL Data RX: Handle (\d+).*?([\d.]+)$')
MODE = re.compile(r'Mode: (\w+) \(0x([0-9a-f]+)\)')
INTERVAL = re.compile(r'Interval: ([\d.]+) msec')

rx = []
modes = []
pending_interval = None

for line in open(path, errors="replace"):
    m = RX.match(line)
    if m:
        rx.append(float(m.group(2)))
        continue
    m = MODE.search(line)
    if m:
        pending_interval = None
        modes.append([m.group(1), None, None])
        continue
    m = INTERVAL.search(line)
    if m and modes:
        modes[-1][2] = float(m.group(1))

if len(rx) < 3:
    print("poucos pacotes RX (%d)" % len(rx))
    sys.exit()

rx.sort()
gaps = [(b - a) * 1000.0 for a, b in zip(rx, rx[1:])]
gs = sorted(gaps)


def pct(p):
    return gs[min(len(gs) - 1, int(len(gs) * p))]


print("ACL RX: %d pacotes em %.1fs (%.1f/s)" % (len(rx), rx[-1] - rx[0],
                                                len(rx) / (rx[-1] - rx[0])))
print("  intervalos (ms): p50=%.1f p90=%.1f p99=%.1f max=%.1f" %
      (pct(.5), pct(.9), pct(.99), gs[-1]))
buckets = {"<15ms": 0, "15-30ms": 0, "30-60ms": 0, "60-150ms": 0,
           "150-400ms": 0, ">400ms": 0}
for g in gaps:
    if g < 15:
        buckets["<15ms"] += 1
    elif g < 30:
        buckets["15-30ms"] += 1
    elif g < 60:
        buckets["30-60ms"] += 1
    elif g < 150:
        buckets["60-150ms"] += 1
    elif g < 400:
        buckets["150-400ms"] += 1
    else:
        buckets[">400ms"] += 1
for k, v in buckets.items():
    print("    %-10s %6d (%5.1f%%)" % (k, v, 100.0 * v / len(gaps)))
print("modos do link:")
for name, _, iv in modes:
    print("    %-8s%s" % (name, "  (sniff interval %s ms)" % iv if iv else ""))
base = rx[0]
print("buracos > 100ms:")
for a, b in zip(rx, rx[1:]):
    g = (b - a) * 1000
    if g > 100:
        print("   t=+%7.2fs  gap=%8.1f ms" % (b - base, g))

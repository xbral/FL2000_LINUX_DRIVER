#!/bin/sh
# Testa as hipoteses do lado do Bluetooth para a travada do mouse.
#
#   C1  baseline (dongle BT com autosuspend em 'auto')
#   C2  autosuspend do dongle BT desligado ('on')
#
# A travada e do lado do BT (ver NOTES.md, session 12): o link entra em sniff
# mode e o host so o acorda a cada ~30 s. O autosuspend USB do dongle e o
# candidato mais obvio: com o controlador dormindo, o report do mouse fica
# retido no periferico ate o proximo poll.
#
# Mexa o mouse de forma continua e densa em TODAS as janelas.
#
# Uso: sudo scripts/bt_side_test.sh [segundos por janela]
set -e
SECS=${1:-45}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-/tmp/opencode}
mkdir -p "$OUT"

BTDEV=/sys/bus/usb/devices/3-10/power/control

echo "=============================================================="
echo " Lado Bluetooth: autosuspend do dongle BT"
echo " $SECS s por janela. MOVIMENTO DENSO E CONTINUO do mouse."
echo "=============================================================="

echo
echo "--- C1: baseline (autosuspend = auto)"
echo "    control = $(cat $BTDEV)"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "C1 baseline (autosuspend auto)" | tee "$OUT/bt_C1_baseline.txt"

echo
echo "--- C2: autosuspend do dongle BT desligado"
echo on | sudo tee "$BTDEV" >/dev/null
sleep 3
echo "    control = $(cat $BTDEV)"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "C2 autosuspend OFF" | tee "$OUT/bt_C2_nosuspend.txt"

echo
echo "--- restaurando autosuspend = auto"
echo auto | sudo tee "$BTDEV" >/dev/null
sleep 2

echo
python3 "$HERE/bt_ab_compare.py" "$OUT/bt_C1_baseline.txt" "$OUT/bt_C2_nosuspend.txt"

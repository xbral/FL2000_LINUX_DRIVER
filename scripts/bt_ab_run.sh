#!/bin/sh
# A/B do mouse Bluetooth com o fl2000: mesma sessao, mesma janela, com e sem
# o dongle transmitindo. O movimento da mao e o controle; a saida do dongle e
# a unica variavel.
#
# O toggle do Mutter precisa do barramento de sessao do USUARIO, e a medicao
# precisa de root (/dev/input e btmon). Daí o hybrid: medicao em sudo, toggle
# em sudo -u <usuario> com o barramento de sessao explicitado.
#
# Uso: sudo scripts/bt_ab_run.sh [segundos por janela]
set -e
SECS=${1:-60}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-/tmp/opencode}
mkdir -p "$OUT"

USER_NAME=${SUDO_USER:-$(logname 2>/dev/null || whoami)}
USER_UID=$(id -u "$USER_NAME")
export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$USER_UID/bus"
export XDG_RUNTIME_DIR="/run/user/$USER_UID"

toggle() {
    sudo -u "$USER_NAME" env DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
        python3 "$HERE/fl2000_toggle.py" "$1"
}

echo "### religando a saida do dongle e confirmando que ele transmitindo"
toggle on >/dev/null 2>&1 || true
sleep 6

echo "### JANELA 1: fl2000 TRANSMITINDO -- MEXA O MOUSE CONTINUAMENTE"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "A: fl2000 TRANSMITINDO" | tee "$OUT/ab_A_on.txt"

echo
echo "### desligando a saida do dongle (stream para, chip ocioso)"
toggle off >/dev/null
sleep 6

echo "### JANELA 2: fl2000 OCIOSO -- MEXA O MOUSE CONTINUAMENTE"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "B: fl2000 OCIOSO" | tee "$OUT/ab_B_off.txt"

echo
echo "### religando a saida do dongle"
toggle on >/dev/null 2>&1 || true
sleep 3
echo "resultados em $OUT/ab_A_on.txt e $OUT/ab_B_off.txt"

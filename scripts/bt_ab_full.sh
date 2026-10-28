#!/bin/sh
# A/B completo do mouse Bluetooth contra a carga do fl2000.
#
#   A1  fl2000 transmitindo, frame em chunks de 1 MB (padrao do 1.0.10)
#   A2  fl2000 transmitindo, frame em 1 URB (chunk_kb=0, o comportamento antigo)
#   B   fl2000 ocioso (saida desligada) - o controle
#
# O movimento da mao e o CONTROLE: mexa o mouse continuamente em TODAS as
# janelas, do mesmo jeito. O script normaliza pelo numero de eventos no
# comparador, mas quanto mais consistente o movimento, melhor.
#
# O toggle da saida forca um modeset completo ("Mode configured"), e os
# buffers - com as tabelas de scatterlist por chunk - sao re-alocados no
# modeset. Por isso mudar chunk_kb em runtime e efetivo sem recompilar.
#
# Uso: sudo scripts/bt_ab_full.sh [segundos por janela]
set -e
SECS=${1:-45}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-/tmp/opencode}
mkdir -p "$OUT"

USER_NAME=${SUDO_USER:-$(logname 2>/dev/null || whoami)}
USER_UID=$(id -u "$USER_NAME")
export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$USER_UID/bus"
export XDG_RUNTIME_DIR="/run/user/$USER_UID"
PARAM=/sys/module/fl2000/parameters/fl2000_chunk_kb

toggle() {
    sudo -u "$USER_NAME" env DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
        python3 "$HERE/fl2000_toggle.py" "$@" >/dev/null 2>&1 || true
}

set_chunk() {
    echo $1 | sudo tee "$PARAM" >/dev/null
}

# Reaplica o modo e confere no dmesg se o split realmente mudou.
# Um toggle simples (off/on) NAO forca um modeset quando o modo nao muda:
# o Mutter reaplica a mesma configuracao e o driver nao reprograma os
# buffers. Por isso o modo e cyclado (outra resolucao e de volta), o que
# forca um "Mode configured" de verdade.
apply_and_verify() {
    want=$1
    expect=$2
    tag=$3
    before=$(sudo journalctl -k -b --no-pager | grep -c "split into")
    toggle off
    sleep 3
    toggle on 1280x1024
    sleep 5
    toggle on 1920x1080
    sleep 6
    after=$(sudo journalctl -k -b --no-pager | grep -c "split into")
    line=$(sudo journalctl -k -b --no-pager | grep "split into" | tail -1)
    echo "    $tag: $line"
    if [ "$after" -gt "$before" ]; then
        case "$line" in
            *"$expect"*) echo "    OK: split aplicado (modeset novo)" ;;
            *) echo "    AVISO: modeset novo, mas esperava '$expect'" ;;
        esac
    else
        echo "    AVISO: nenhum modeset novo (o split nao mudou)"
    fi
}

echo "=============================================================="
echo " A/B do mouse Bluetooth x carga do fl2000"
echo " $SECS s por janela. MEXA O MOUSE CONTINUAMENTE EM TODAS:"
echo " desenhe circulos grandes e densos, sem parar. A medicao so e"
echo " valida com pelo menos 60 eventos/s - o script avisa se nao for."
echo "=============================================================="

echo
echo "--- A1: chunks de 1 MB (padrao)"
set_chunk 1024
apply_and_verify 1024 "4 chunks" "split"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "A1 chunks 1MB" | tee "$OUT/ab_A1_chunks.txt"

echo
echo "--- A2: 1 URB por frame (chunk_kb=0, comportamento antigo)"
set_chunk 0
apply_and_verify 0 "1 chunk" "split"
python3 "$HERE/bt_stutter_ab.py" "$SECS" "A2 1 URB por frame" | tee "$OUT/ab_A2_single.txt"

echo
echo "--- B: fl2000 ocioso (controle)"
toggle off
sleep 6
python3 "$HERE/bt_stutter_ab.py" "$SECS" "B fl2000 ocioso" | tee "$OUT/ab_B_idle.txt"

echo
echo "--- restaurando: chunks de 1 MB e religando a saida"
set_chunk 1024
# IMPORTANTE: um toggle sem mudanca de modo NAO refaz o modeset, e portanto
# nao realoca os buffers - o chunk_kb novo so entraria em vigor depois de um
# modeset de verdade. O ciclo de resolucao e o que garante isso.
toggle off
sleep 3
toggle on 1280x1024
sleep 5
toggle on 1920x1080
sleep 6
sudo journalctl -k -b --no-pager | grep "split into" | tail -1

echo
python3 "$HERE/bt_ab_compare.py" "$OUT/ab_A1_chunks.txt" "$OUT/ab_A2_single.txt" "$OUT/ab_B_idle.txt"
echo
echo "resultados brutos em $OUT/ab_A1_chunks.txt, $OUT/ab_A2_single.txt, $OUT/ab_B_idle.txt"

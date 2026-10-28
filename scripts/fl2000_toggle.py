#!/usr/bin/env python3
"""Liga/desliga a saida VGA do fl2000 pelo DisplayConfig do Mutter.

Funcao: forcar fl2000_display_disable()/enable(), que rearma o watchdog de
stall e reinicia a sessao de streaming - o caminho que o usuario nao tem
(nao ha xrandr para o VGA no Xwayland, e o mutter so ve a saida se ela
estiver habilitada).

Mutter exige que os logical monitors nao deixem buracos na grade e que haja um
primary, entao o layout e reempacotado (x acumulado, y=0).

Uso: fl2000_toggle.py off | on
"""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

BUS = Gio.bus_get_sync(Gio.BusType.SESSION, None)
PROXY = Gio.DBusProxy.new_sync(
    BUS, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None,
    "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
    "org.gnome.Mutter.DisplayConfig", None)


def get_state():
    s = PROXY.call_sync("GetCurrentState", None, Gio.DBusCallFlags.NONE, -1, None)
    return s.unpack()


def current_modes(monitors):
    """{conector: (mode_id, width)} do modo a usar.

    Um monitor que esta fora de qualquer logical monitor (saida desabilitada)
    nao tem modo 'is-current' - o Mutter so marca isso para os modos em uso.
    Nesses casos cai para o modo preferido, depois para o primeiro da lista,
    senao nao da para religar a saida.
    """
    out = {}
    for m in monitors:
        conn = m[0][0]
        cur = None
        pref = None
        for mode in m[1]:
            props = mode[6]
            if props.get("is-current"):
                cur = mode
                break
            if pref is None and props.get("is-preferred"):
                pref = mode
        mode = cur or pref or (m[1][0] if m[1] else None)
        if mode:
            out[conn] = (mode[0], mode[1])
    return out


def resolve_mode(modes, conn, wanted):
    """Resolve um mode_id pedido (por prefixo) para o conector, com a largura."""
    if not wanted:
        return None
    allm = modes.get("_all", {})
    for m in allm.get(conn, []):
        if m[0] == wanted or m[0].startswith(wanted):
            return (m[0], m[1])
    return None


def build(monitors, logical, keep_vga, vga_mode=None):
    modes = current_modes(monitors)
    vga = [c for c in modes if c.startswith("VGA")]
    vga_conn = vga[0] if vga else None

    # todos os modos disponiveis por conector (id -> largura), para resolver por
    # prefixo e para reempacotar o layout quando a largura muda
    allm = {"_all": {}}
    for m in monitors:
        allm["_all"][m[0][0]] = [(mo[0], mo[1]) for mo in m[1]]
    modes.update(allm)

    order = []
    for lm in logical:
        for s in lm[5]:
            if s[0].startswith("VGA"):
                continue
            if s[0] not in order:
                order.append(s[0])
    if keep_vga and vga_conn:
        order.insert(1 if len(order) > 1 else len(order), vga_conn)

    lms = []
    x = 0
    for conn in order:
        if conn not in modes or conn == "_all":
            continue
        mode_id, width = modes[conn]
        if conn == vga_conn and vga_mode:
            resolved = resolve_mode(modes, conn, vga_mode)
            if resolved:
                mode_id, width = resolved
            else:
                print("  AVISO: modo '%s' nao encontrado para %s; usando %s"
                      % (vga_mode, conn, mode_id))
        lms.append((x, 0, 1.0, 0, not lms, [(conn, mode_id, {})]))
        x += width
    return lms, vga_conn


def apply(serial, lms):
    PROXY.call_sync(
        "ApplyMonitorsConfig",
        GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})", (serial, 1, lms, {})),
        Gio.DBusCallFlags.NONE, -1, None)


def main():
    want = sys.argv[1]
    vga_mode = sys.argv[2] if len(sys.argv) > 2 else None
    serial, monitors, logical, _ = get_state()
    lms, conn = build(monitors, logical, keep_vga=(want != "off"), vga_mode=vga_mode)
    print("conector VGA=%s -> %s%s" % (conn, want, " modo=%s" % vga_mode if vga_mode else ""))
    print("  layout: %s" % [(l[0], [(s[0], s[1]) for s in l[5]]) for l in lms])
    apply(serial, lms)
    print("  ApplyMonitorsConfig OK (temporary)")


if __name__ == "__main__":
    main()

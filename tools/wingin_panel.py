#!/usr/bin/env python3
"""Panel of WING-IN: audio from the WING (its USB outputs) into Rack, 8 channels, each with its source on the WING.

Usage:  wingin_panel.py real                   -> res/WingIn.svg + src/WingInLayout.hpp
        wingin_panel.py preview OUT.png [RACK] -> preview with Rack components and a mock display
"""
import sys
import satpanel as sp
from satpanel import wl

p = sp.SatPanel("in", 22)
p.header("WING-IN", "FROM THE WING")
p.e(f'<rect x="{p.W - 24:.2f}" y="5.55" width="6.4" height="1.08" rx="0.54" fill="{wl.ACC}"/>')
p.dark(3.4, 10.6, 60.0, 11.2, "DISPLAY")
# USB start (− / +) and SYNC (routes the WING's USB outputs to the sources chosen here)
BY = 15.4
p.button("DOWN", 69.0, BY)
p.text(69.0, BY + 6.0, "−", 2.2, wl.TXT, 800, "middle", 0)
p.area("START", 73.4, BY - 3.0, 13.2, 6.0)
p.button("UP", 91.0, BY)
p.text(91.0, BY + 6.0, "+", 2.2, wl.TXT, 800, "middle", 0)
p.button("SYNC", 103.6, BY)
p.text(103.6, BY + 6.2, "SYNC", 1.35, wl.TXT, 800, "middle", 0.25)
X0 = 3.4
CW = (p.W - 6.8) / 8
for i in range(8):
    x0 = X0 + CW * i
    cx = x0 + CW / 2
    p.e(f'<rect x="{x0 + 0.3:.2f}" y="25.0" width="{CW - 0.6:.2f}" height="98.5" rx="1" fill="{wl.PLATE}"/>')
    p.e(f'<rect x="{x0 + 0.3:.2f}" y="25.0" width="{CW - 0.6:.2f}" height="0.75" rx="0.35" fill="{wl.ACC}"/>')
    p.area(f"SRC{i + 1}", x0 + 0.9, 27.4, CW - 1.8, 13.0)   # its source (a click: CH, AUX, BUS, MAIN, MTX, LCL …)
    p.area(f"METER{i + 1}", cx - 1.4, 43.5, 2.8, 42.0)      # its level, as it arrives
    p.knob(f"LVL{i + 1}", cx, 95.0)
    p.text(cx, 100.6, "LEVEL", 1.15, wl.DIM, 800, "middle", 0.2)
    p.jack(f"OUT{i + 1}", cx, 112.6, f"USB {i + 1} OUT", True)

if len(sys.argv) < 2 or sys.argv[1] != "preview":
    sp.write_real(p, "WingIn.svg", "WingInLayout.hpp", "inlayout", "wingin_panel.py")
    sys.exit(0)


def mock(o, put):
    x, y, w, h = p.areas["DISPLAY"]
    put("AUDIO ← USB 1–8 OUT", x + 2, y + 4.8, 2.6, "#ffffff", 800, "start", 0.3)
    put("●  VIA WING-LINK · 48 kHz · 7 SOURCES", x + 2, y + 8.9, 1.35, "#7ee08a", 700, "start", 0.2)
    ax, ay, aw, ah = p.areas["START"]
    o.insert(-1, f'<rect x="{ax}" y="{ay}" width="{aw}" height="{ah}" rx="0.8" fill="#07080a"/>')
    put("1 OUT", ax + aw / 2, ay + 4.0, 1.6, "#ff6b5b", 800, "middle", 0.2)
    srcs = [("LCL 1", "VOCAL", "#ff6b5b", 0.62), ("LCL 2", "GUITAR", "#ff9a3e", 0.41), ("CH 9", "SYNTH · POST", "#3dc9ff", 0.55),
            ("BUS 1", "FX", "#b48cff", 0.30), ("BUS 2", "DRUMS", "#7ee08a", 0.72), ("MAIN L", "MIX", "#d9dbe2", 0.66),
            ("MAIN R", "MIX", "#d9dbe2", 0.64), ("", "", "", 0)]
    for i, (s_, nm, col, lv) in enumerate(srcs):
        ax, ay, aw, ah = p.areas[f"SRC{i + 1}"]
        o.insert(-1, f'<rect x="{ax}" y="{ay}" width="{aw}" height="{ah}" rx="0.6" fill="#07080a"/>')
        mx, my, mw, mh = p.areas[f"METER{i + 1}"]
        o.insert(-1, f'<rect x="{mx}" y="{my}" width="{mw}" height="{mh}" rx="0.6" fill="#07080a"/>')
        if s_:
            o.insert(-1, f'<rect x="{ax + 1}" y="{ay + 1}" width="{aw - 2}" height="0.8" rx="0.3" fill="{col}"/>')
            put(s_, ax + aw / 2, ay + 6.0, 1.45, "#f2f3f7", 800, "middle", 0.1)
            put(nm, ax + aw / 2, ay + 10.0, 1.1, "#8a8d99", 700, "middle", 0.1)
            hh = (mh - 1.2) * lv
            o.insert(-1, f'<rect x="{mx + 0.6}" y="{my + mh - 0.6 - hh:.2f}" width="{mw - 1.2}" height="{hh:.2f}" rx="0.4" fill="#7ee08a"/>')
        else:
            put("—", ax + aw / 2, ay + 6.0, 1.45, "#5d606b", 800, "middle", 0.1)
            put("CHOOSE", ax + aw / 2, ay + 10.0, 1.05, "#5d606b", 700, "middle", 0.1)


sp.write_preview(p, mock, sys.argv[2])

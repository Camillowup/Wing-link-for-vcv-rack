#!/usr/bin/env python3
"""Panel of WING-KNOBS: the WING's custom controls (its rotaries and buttons set to MIDI CC) as CV and gates in Rack.

Usage:  wingknobs_panel.py real                   -> res/WingKnobs.svg + src/WingKnobsLayout.hpp
        wingknobs_panel.py preview OUT.png [RACK] -> preview with Rack components and a mock display
"""
import sys
import satpanel as sp
from satpanel import wl

p = sp.SatPanel("knobs", 14)
p.header("", "")
p.text(p.W / 2, 7.4, "WING-KNOBS", 3.1, wl.TITLE, 800, "middle", 0.55)
p.dark(3.4, 10.6, p.W - 6.8, 11.2, "DISPLAY")
CW = (p.W - 6.8 - 2.0) / 2
cols = [(3.4, "KNOBS", "K", "CV"), (3.4 + CW + 2.0, "BUTTONS", "B", "GATE")]
Y0, DY = 33.6, 11.7
for x0, title, pre, kind in cols:
    p.plate(x0, 24.2, CW, 99.3, title)
    p.text(x0 + 17.3, 28.9, "LEVEL", 1.15, wl.DIM, 800, "middle", 0.2)
    for i in range(8):
        y = Y0 + DY * i
        p.area(f"{pre}{i + 1}", x0 + 1.0, y - 3.4, 11.8, 6.8)
        p.knob(f"LVL_{pre}{i + 1}", x0 + 17.3, y - 0.6)     # how strong: the output's level (CV: 0..10 V, gates: their height)
        p.jack(f"OUT_{pre}{i + 1}", x0 + CW - 5.0, y - 0.6, "", True)

if len(sys.argv) < 2 or sys.argv[1] != "preview":
    sp.write_real(p, "WingKnobs.svg", "WingKnobsLayout.hpp", "knobslayout", "wingknobs_panel.py")
    sys.exit(0)


def mock(o, put):
    x, y, w, h = p.areas["DISPLAY"]
    put("LAYER 1 · 6 LEARNED", x + 2, y + 4.8, 2.0, "#ffffff", 800, "start", 0.3)
    put("●  MIDI VIA WING-LINK", x + 2, y + 8.6, 1.35, "#7ee08a", 700, "start", 0.2)
    names = ["CUTOFF", "RES", "DELAY", "VERB", "DRIVE", "LFO", "", ""]
    vals = [0.72, 0.31, 0.5, 0.18, 0.86, 0.44, 0, 0]
    bnames = ["MUTE", "NEW", "FILL", "DROP", "", "", "", ""]
    bon = [0, 1, 0, 1, 0, 0, 0, 0]
    for i in range(8):
        ax, ay, aw, ah = p.areas[f"K{i + 1}"]
        o.insert(-1, f'<rect x="{ax}" y="{ay}" width="{aw}" height="{ah}" rx="0.6" fill="#07080a"/>')
        if names[i]:
            put(names[i], ax + 1, ay + 2.6, 1.15, "#d9dbe2", 800, "start", 0.15)
            put(f"CC {20 + i}", ax + 1, ay + 6.2, 1.0, "#5d606b", 700, "start", 0.1)
            o.insert(-1, f'<rect x="{ax + 1}" y="{ay + 3.4}" width="{aw - 2}" height="1.1" rx="0.5" fill="#2a2c33"/>')
            o.insert(-1, f'<rect x="{ax + 1}" y="{ay + 3.4}" width="{(aw - 2) * vals[i]:.2f}" height="1.1" rx="0.5" fill="{wl.ACC}"/>')
        else:
            put("LEARN", ax + aw / 2, ay + 4.3, 1.2, "#5d606b", 700, "middle", 0.3)
        bx, by, bw, bh = p.areas[f"B{i + 1}"]
        o.insert(-1, f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="0.6" fill="#07080a"/>')
        if bnames[i]:
            o.insert(-1, f'<circle cx="{bx + 2.2}" cy="{by + bh / 2}" r="1.1" fill="{"#b8f23a" if bon[i] else "#2a2c33"}"/>')
            put(bnames[i], bx + 4.0, by + 2.6, 1.1, "#d9dbe2", 800, "start", 0.15)
            put(f"CC {40 + i}", bx + 4.0, by + 5.4, 1.0, "#5d606b", 700, "start", 0.1)
        else:
            put("LEARN", bx + bw / 2, by + 4.3, 1.2, "#5d606b", 700, "middle", 0.3)


sp.write_preview(p, mock, sys.argv[2])

#!/usr/bin/env python3
"""Panel of WING-DYN, the expander for a WING channel's gate and compressor (in the look of WING-LINK).

Usage:  wingdyn_panel.py real                   -> res/WingDyn.svg + src/WingDynLayout.hpp
        wingdyn_panel.py preview OUT.png [RACK] -> preview with Rack components and a mock display
"""
import math
import sys
import satpanel as sp
from satpanel import wl

GATE_C, COMP_C = "#3dc9ff", "#ff9a3e"
p = sp.SatPanel("dyn", 24)
p.header("WING-DYN", "GATE · COMP")
p.e(f'<rect x="{p.W - 24:.2f}" y="5.55" width="6.4" height="1.08" rx="0.54" fill="{wl.ACC}"/>')
p.dark(5.4, 10.6, 64.0, 11.2, "DISPLAY")
BY = 15.4
p.button("PREV", 77.0, BY); p.tri(77.0, 21.2, True)
p.button("NEXT", 88.0, BY); p.tri(88.0, 21.2, False)
p.button("GATE_ON", 100.5, BY); p.text(100.5, 21.6, "GATE", 1.4, wl.TXT, 800, "middle", 0.25)
p.button("COMP_ON", 112.5, BY); p.text(112.5, 21.6, "COMP", 1.4, wl.TXT, 800, "middle", 0.25)
p.dark(5.4, 24.0, 42.0, 33.0, "CURVE")          # in -> out
p.dark(49.4, 24.0, 67.1, 33.0, "ENV")           # attack / hold / release in time

# GATE
x0, w = 5.4, 42.0
p.plate(x0, 59.5, w, 64.0, "GATE", GATE_C)
for (n, lab, x) in (("G_THR", "THRESH", x0 + 11), ("G_RANGE", "RANGE", x0 + 31)):
    p.mknob(n, x, 73.1)
    p.text(x, 80.9, lab, 1.45, wl.TXT, 800, "middle", 0.3)
    p.area("V" + n, x - 9, 81.4, 18, 2.4)
for i, (n, lab) in enumerate((("G_ATT", "ATT"), ("G_HOLD", "HOLD"), ("G_REL", "REL"), ("G_ACC", "ACC"))):
    x = x0 + 6 + 10 * i
    p.knob(n, x, 89.2)
    p.text(x, 94.4, lab, 1.25, wl.TXT, 800, "middle", 0.2)
    p.area("V" + n, x - 5, 94.9, 10, 2.2)
p.e(f'<line x1="{x0 + 2}" y1="99" x2="{x0 + w - 2}" y2="99" stroke="{wl.LINE}" stroke-width="0.3"/>')
for i, (n, lab) in enumerate((("CV_G_THR", "THRESH"), ("CV_G_RANGE", "RANGE"), ("CV_G_ON", "ON"))):
    p.jack(n, x0 + 7 + 14 * i, 104.6, lab)
for i, (n, lab) in enumerate((("CV_G_ATT", "ATT"), ("CV_G_REL", "REL"))):
    p.jack(n, x0 + 14 + 14 * i, 116.2, lab)

# COMPRESSOR
x0, w = 49.4, 67.1
p.plate(x0, 59.5, w, 64.0, "COMPRESSOR", COMP_C)
for (n, lab) in (("C_DET", "RMS"), ("C_ENV", "LOG"), ("C_AUTO", "AUTO")):
    pass
for i, (n, lab) in enumerate((("C_DET", "RMS"), ("C_ENV", "LOG"), ("C_AUTO", "AUTO"))):
    x = x0 + w - 28 + 12 * i
    p.sbutton(n, x, 63.7)
    p.text(x - 3.9, 64.2, lab, 1.2, wl.DIM, 800, "end", 0.2)
COLS = [x0 + 8.4 + 16.8 * i for i in range(4)]
for (n, lab), x in zip((("C_THR", "THRESH"), ("C_RATIO", "RATIO"), ("C_GAIN", "GAIN"), ("C_MIX", "MIX")), COLS):
    p.mknob(n, x, 73.1)
    p.text(x, 80.9, lab, 1.45, wl.TXT, 800, "middle", 0.3)
    p.area("V" + n, x - 8, 81.4, 16, 2.4)
for (n, lab), x in zip((("C_KNEE", "KNEE"), ("C_ATT", "ATT"), ("C_HOLD", "HOLD"), ("C_REL", "REL")), COLS):
    p.knob(n, x, 89.2)
    p.text(x, 94.4, lab, 1.25, wl.TXT, 800, "middle", 0.2)
    p.area("V" + n, x - 8, 94.9, 16, 2.2)
p.e(f'<line x1="{x0 + 2}" y1="99" x2="{x0 + w - 2}" y2="99" stroke="{wl.LINE}" stroke-width="0.3"/>')
for (n, lab), x in zip((("CV_C_THR", "THRESH"), ("CV_C_RATIO", "RATIO"), ("CV_C_GAIN", "GAIN"), ("CV_C_MIX", "MIX")), COLS):
    p.jack(n, x, 104.6, lab)
for (n, lab), x in zip((("CV_C_KNEE", "KNEE"), ("CV_C_ATT", "ATT"), ("CV_C_REL", "REL"), ("CV_C_ON", "ON")), COLS):
    p.jack(n, x, 116.2, lab)

if len(sys.argv) < 2 or sys.argv[1] != "preview":
    sp.write_real(p, "WingDyn.svg", "WingDynLayout.hpp", "dynlayout", "wingdyn_panel.py")
    sys.exit(0)


def mock(o, put):
    x, y, w, h = p.areas["DISPLAY"]
    o.insert(-1, f'<rect x="{x + 2.6}" y="{y + 1.8}" width="1.1" height="7.4" rx="0.4" fill="#ff6b5b"/>')
    put("CH 1 · KICK", x + 5.4, y + 5.5, 3.2, "#ffffff", 800, "start", 0.4)
    put("●  STRIP 1 · GATE ON · COMP ON", x + 5.4, y + 8.9, 1.45, "#7ee08a", 700, "start", 0.3)
    # in -> out: gate below its threshold, compressor above its
    x, y, w, h = p.areas["CURVE"]
    x0, y0, s, sv = x + 4, y + h - 3, w - 8, h - 6
    o.insert(-1, f'<line x1="{x0}" y1="{y0}" x2="{x0 + s}" y2="{y0 - s}" stroke="#34363f" stroke-width="0.25" stroke-dasharray="0.8 0.6"/>'.replace(f'y2="{y0 - s}"', f'y2="{y0 - sv}"'))
    gt, ct, r = 0.3, 0.62, 4.0
    pts = []
    for k in range(81):
        u = k / 80
        if u < gt:
            v = u - 0.25
        elif u < ct:
            v = u
        else:
            v = ct + (u - ct) / r
        pts.append(f"{x0 + u * s:.2f},{y0 - max(0, v) * sv:.2f}")
    o.insert(-1, f'<polyline points="{" ".join(pts)}" fill="none" stroke="#f2f3f7" stroke-width="0.5"/>')
    o.insert(-1, f'<line x1="{x0 + gt * s:.2f}" y1="{y + 2}" x2="{x0 + gt * s:.2f}" y2="{y0}" stroke="{GATE_C}" stroke-width="0.3"/>')
    o.insert(-1, f'<line x1="{x0 + ct * s:.2f}" y1="{y + 2}" x2="{x0 + ct * s:.2f}" y2="{y0}" stroke="{COMP_C}" stroke-width="0.3"/>')
    put("IN →", x + w - 2, y + h - 1.2, 1.3, "#5d606b", 700, "end", 0.2)
    put("OUT", x + 1.4, y + 3.0, 1.3, "#5d606b", 700, "start", 0.2)
    put("4:1", x0 + ct * s + 1, y + 6, 1.5, COMP_C, 800, "start", 0.2)
    # time: the gate opening, holding, closing; the compressor's attack and release
    x, y, w, h = p.areas["ENV"]
    b = y + h - 4
    g = [(0, b), (0.06, b - 18), (0.45, b - 18), (0.75, b - 3), (1, b - 3)]
    o.insert(-1, '<polyline points="' + " ".join(f"{x + 3 + u * (w - 6):.2f},{v:.2f}" for u, v in g) + f'" fill="none" stroke="{GATE_C}" stroke-width="0.45"/>')
    c = [(0, y + 5)] + [(0.08 + u * 0.02, y + 5 + 9 * (1 - math.exp(-u * 3))) for u in range(6)] + [(0.45, y + 14), (0.7, y + 5.5), (1, y + 5)]
    o.insert(-1, '<polyline points="' + " ".join(f"{x + 3 + u * (w - 6):.2f},{v:.2f}" for u, v in c) + f'" fill="none" stroke="{COMP_C}" stroke-width="0.45"/>')
    put("GATE  2 · 40 · 120 ms", x + 3, b + 2.6, 1.35, GATE_C, 700, "start", 0.2)
    put("COMP  −6 dB · 10 · 80 ms", x + w - 3, y + 3.2, 1.35, COMP_C, 700, "end", 0.2)
    vals = {"VG_THR": "−42 dB", "VG_RANGE": "40 dB", "VG_ATT": "2 ms", "VG_HOLD": "40", "VG_REL": "120",
            "VG_ACC": "50%", "VC_THR": "−18 dB", "VC_RATIO": "4.0", "VC_GAIN": "+3.0 dB", "VC_MIX": "100%",
            "VC_KNEE": "2", "VC_ATT": "10 ms", "VC_HOLD": "20", "VC_REL": "80"}
    for n, s_ in vals.items():
        ax, ay, aw, ah = p.areas[n]
        put(s_, ax + aw / 2, ay + 1.7, 1.25, "#8a8d99", 600, "middle", 0.15)


sp.write_preview(p, mock, sys.argv[2])

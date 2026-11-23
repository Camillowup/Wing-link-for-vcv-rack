#!/usr/bin/env python3
"""Panel of WING-REC (draft): records every track of WING-LINK / X / WING-IN as WAVs in Rack and drives the WING-LIVE
SD cards (transport, markers, sessions), plus SPLIT for old SD sessions.

Usage:  wingrec_panel.py real                   -> res/WingRec.svg + src/WingRecLayout.hpp
        wingrec_panel.py preview OUT.png [RACK] -> preview with Rack components and a mock display
"""
import sys
import satpanel as sp
from satpanel import wl

REC = "#ff3b3b"
p = sp.SatPanel("rec", 34)
W = p.W
p.header("WING-REC", "RACK + SD RECORDER")
p.e(f'<rect x="{W - 24:.2f}" y="5.55" width="6.4" height="1.08" rx="0.54" fill="{wl.ACC}"/>')

# ---- top: display, targets, SYNC, REC / STOP / MARK
p.dark(3.4, 10.6, 62.0, 11.2, "DISPLAY")
TY = 10.6
for i, n in enumerate(("RACK", "SD1", "SD2")):
    p.area("T_" + n, 68.4 + i * 10.6, TY, 9.2, 11.2)          # target tiles (a click: on / off)
BY = 15.4
p.button("SYNC", 106.0, BY)
p.text(106.0, 21.6, "SYNC", 1.4, wl.TXT, 800, "middle", 0.25)
p.area("RECBIG", 114.4, 10.2, 13.6, 12.0)                     # the big REC button
p.text(121.2, 24.4, "REC", 1.5, REC, 800, "middle", 0.3)
for (n, lab, x) in (("STOP", "STOP", 140.0), ("MARK", "MARK", 154.0)):
    p.button(n, x, BY)
    p.text(x, 21.6, lab, 1.4, wl.TXT, 800, "middle", 0.25)

# ---- SD cards: transport, markers, mode, session, tracks, SPLIT
p.plate(3.4, 26.4, W - 6.8, 12.4, "SD CARDS", wl.ACC)
SY = 33.6
p.area("CARD", 22.0, SY - 3.2, 13.0, 6.4)                    # CARD 1 / 2 / BOTH (a click: menu)
p.text(13.2, 34.2, "CARD", 1.15, wl.DIM, 800, "middle", 0.2)
xs = [42.0, 51.0, 60.0]
for n, lab, x in zip(("PLAY", "PAUSE", "SDSTOP"), ("PLAY", "PAUSE", "STOP"), xs):
    p.sbutton(n, x, SY)
    p.text(x, SY + 4.0, lab, 1.15, wl.DIM, 800, "middle", 0.2)
p.sbutton("MPREV", 70.0, SY); p.tri(70.0, SY + 3.6, True)
p.area("MNUM", 73.2, SY - 2.6, 9.0, 5.2)
p.sbutton("MNEXT", 85.4, SY); p.tri(85.4, SY + 3.6, False)
p.text(77.7, SY + 4.0, "MARKER", 1.05, wl.DIM, 800, "middle", 0.2)
p.area("MODE", 92.0, SY - 2.6, 16.0, 5.2)
p.area("SESSION", 110.5, SY - 2.6, 30.0, 5.2)
p.area("TRACKS", 143.0, SY - 2.6, 10.0, 5.2)
for lab, x in (("MODE", 100.0), ("SESSION", 125.5), ("TRACKS", 148.0)):
    p.text(x, SY + 4.0, lab, 1.05, wl.DIM, 800, "middle", 0.2)
p.sbutton("SPLIT", 160.5, SY)
p.text(160.5, SY + 4.0, "SPLIT", 1.15, wl.TXT, 800, "middle", 0.2)

# ---- the tracks: 8 per row (WING-LINK, WING-LINK X, WING-IN), ST between each pair
TW = (W - 6.8) / 8
ROWS = [("WING-LINK", 41.2), ("WING-LINK X", 60.6), ("WING-IN", 80.0)]
for r, (title, y0) in enumerate(ROWS):
    p.text(4.2, y0 + 2.6, title, 1.35, wl.DIM, 800, "start", 0.35)
    for i in range(8):
        x = 3.4 + TW * i
        p.area(f"TR{r * 8 + i + 1}", x + 0.4, y0 + 4.0, TW - 0.8, 9.6)
    for k in range(4):
        cx = 3.4 + TW * (2 * k + 1)
        p.sbutton(f"ST{r * 4 + k + 1}", cx, y0 + 16.6)
        p.text(cx + 4.0, y0 + 17.1, "ST", 1.1, wl.DIM, 800, "start", 0.2)

# ---- jacks: IN 1–16 (a cable takes the track over), triggers, gates out
p.e(f'<line x1="3.4" y1="100.6" x2="{W - 3.4:.2f}" y2="100.6" stroke="{wl.LINE}" stroke-width="0.3"/>')
p.text(4.2, 104.0, "IN", 1.35, wl.DIM, 800, "start", 0.35)
for i in range(16):
    row, col = divmod(i, 8)
    p.jack(f"IN{i + 1}", 13.0 + col * 9.2, 107.6 + row * 10.6, f"IN {i + 1}")
TX = 96.0
for i, lab in enumerate(("REC", "STOP", "MARK", "SD PLAY")):
    p.jack("TRIG_" + lab.replace(" ", ""), TX + i * 10.0, 107.6, lab)
for i, lab in enumerate(("REC", "SD PLAY")):
    p.jack("GATE_" + lab.replace(" ", ""), TX + 50.0 + i * 10.0, 107.6, lab, True)
for i, lab in enumerate(("CLOCK", "RUN")):
    p.jack("IN_" + lab, TX + i * 10.0, 118.2, lab)
p.text(TX + 22.0, 119.0, "← without GENRA", 1.2, wl.DIM, 700, "start", 0.2)

if len(sys.argv) < 2 or sys.argv[1] != "preview":
    sp.write_real(p, "WingRec.svg", "WingRecLayout.hpp", "reclayout", "wingrec_panel.py")
    sys.exit(0)


def mock(o, put):
    def box(name, fill="#07080a", stroke=None):
        x, y, w, h = p.areas[name]
        s = f' stroke="{stroke}" stroke-width="0.35"' if stroke else ""
        o.insert(-1, f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="0.8" fill="{fill}"{s}/>')
        return x, y, w, h

    x, y, w, h = p.areas["DISPLAY"]
    put("● REC  03:12", x + 2, y + 4.9, 2.6, REC, 800, "start", 0.3)
    put("BAR 49 · DROP 1", x + 30, y + 4.9, 1.9, "#ffffff", 800, "start", 0.2)
    put("●  SYNC · GENRA 132 BPM · 22 TRACKS · 6 MARKERS", x + 2, y + 8.0, 1.25, "#7ee08a", 700, "start", 0.15)
    put("D:\\Recordings\\2026-11-22 17-46 UK Garage", x + 2, y + 10.2, 1.15, "#8a8d99", 700, "start", 0.1)

    for n, lab, sub, on in (("T_RACK", "RACK", "412 GB", True), ("T_SD1", "SD 1", "1:42 h", True),
                            ("T_SD2", "SD 2", "3:05 h", False)):
        x, y, w, h = box(n, "#3a1d1d" if on else "#07080a", REC if on else "#34363f")
        put(lab, x + w / 2, y + 4.6, 1.5, "#ffffff" if on else "#8a8d99", 800, "middle", 0.15)
        put(sub, x + w / 2, y + 8.6, 1.1, "#ffb0a8" if on else "#5d606b", 700, "middle", 0.05)

    x, y, w, h = p.areas["RECBIG"]
    cx, cy = x + w / 2, y + h / 2
    o.insert(-1, f'<circle cx="{cx}" cy="{cy}" r="6.0" fill="#2a2c33" stroke="#4a4c57" stroke-width="0.4"/>')
    o.insert(-1, f'<circle cx="{cx}" cy="{cy}" r="4.3" fill="{REC}"/>')
    o.insert(-1, f'<circle cx="{cx}" cy="{cy}" r="5.3" fill="none" stroke="{REC}" stroke-opacity="0.45" stroke-width="0.5"/>')

    x, y, w, h = box("CARD")
    put("CARD 1", x + w / 2, y + 4.2, 1.45, "#ffffff", 800, "middle", 0.15)
    x, y, w, h = box("MNUM")
    put("M 6", x + w / 2, y + 3.6, 1.45, "#ff6b5b", 800, "middle", 0.15)
    for n, lab in (("MODE", "LOOP ▾"), ("SESSION", "S07 · 2026-11-22 ▾"), ("TRACKS", "16 ▾")):
        x, y, w, h = box(n)
        put(lab, x + w / 2, y + 3.6, 1.35, "#ffffff", 800, "middle", 0.1)

    names = [("KICK", "#7b6cff"), ("SNARE", "#7b6cff"), ("HAT CL", "#7b6cff"), ("HAT OP", "#7b6cff"),
             ("BASS", "#7ee08a"), ("SUB", "#7ee08a"), ("PAD L", "#e0779e"), ("PAD R", "#e0779e"),
             ("LEAD", "#3dc9ff"), ("ARP", "#3dc9ff"), ("VOX", "#ff9a3e"), ("GTR", "#ff9a3e"),
             ("FX", "#c9ccd4"), ("DRONE", "#c9ccd4"), ("", ""), ("", ""),
             ("BUS 1", "#b48cff"), ("BUS 2", "#7ee08a"), ("MAIN L", "#d9dbe2"), ("MAIN R", "#d9dbe2"),
             ("", ""), ("", ""), ("", ""), ("", "")]
    lv = [0.8, 0.6, 0.4, 0.5, 0.75, 0.6, 0.55, 0.5, 0.65, 0.35, 0.45, 0.3, 0.25, 0.4, 0, 0,
          0.5, 0.7, 0.8, 0.78, 0, 0, 0, 0]
    stereo = {3, 9}  # pair index (0-based) lit: PAD L/R, MAIN L/R
    for i, (nm, col) in enumerate(names):
        x, y, w, h = box(f"TR{i + 1}")
        pair = i // 2
        if pair in stereo and i % 2 == 0:
            o.insert(-1, f'<rect x="{x - 0.1}" y="{y - 0.1}" width="{2 * w + 0.98:.2f}" height="{h + 0.2}" rx="0.9" '
                         f'fill="none" stroke="{wl.ACC}" stroke-width="0.3" stroke-dasharray="0.8 0.5"/>')
        if nm:
            o.insert(-1, f'<rect x="{x + 0.8}" y="{y + 0.8}" width="{w - 1.6:.2f}" height="0.7" rx="0.3" fill="{col}"/>')
            put(f"{i + 1:02d}" if i < 16 else f"I{i - 15}", x + 1.2, y + 4.4, 1.1, "#5d606b", 800, "start", 0.05)
            put(nm, x + w / 2 + 1.2, y + 4.6, 1.45, "#f2f3f7", 800, "middle", 0.1)
            bw = (w - 2.4) * lv[i]
            o.insert(-1, f'<rect x="{x + 1.2}" y="{y + 6.6}" width="{w - 2.4:.2f}" height="1.3" rx="0.4" fill="#1b1d22"/>')
            o.insert(-1, f'<rect x="{x + 1.2}" y="{y + 6.6}" width="{bw:.2f}" height="1.3" rx="0.4" fill="#7ee08a"/>')
            o.insert(-1, f'<circle cx="{x + w - 1.6:.2f}" cy="{y + 3.9}" r="0.75" fill="{REC}"/>')
        else:
            put("—", x + w / 2, y + 5.4, 1.45, "#3d4049", 800, "middle", 0.1)
    for st in (4, 10):  # the lit ST buttons of the stereo pairs
        r, k = divmod(st - 1, 4)
        cx = 3.4 + TW * (2 * k + 1)
        cy = ROWS[r][1] + 16.6
        o.insert(-1, f'<circle cx="{cx}" cy="{cy}" r="1.25" fill="{wl.ACC}" opacity="0.9"/>')


sp.write_preview(p, mock, sys.argv[2])

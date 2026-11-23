#!/usr/bin/env python3
"""Makes WING-REC's Ableton Live 12 template (res/wingrec/) from a Live 12 set.

Source: "templates/Ableton 12 Template.als" of github.com/steven-ahfu/suno-to-ableton (AGPL-3.0; see
licenses/suno-to-ableton.txt). The set is cut down to one audio track (the prototype WING-REC clones for every WAV),
its return tracks, main track and pre-hear track stay as they are.

Placeholders (filled in by src/WingRec.cpp):
  live12.xml        WR_NEXTID (NextPointeeId), WR_BPM (tempo), WR_TRACKS (the audio tracks), WR_LOCATORS (locators)
  live12_track.xml  WR_TID (track id), WR_NAME (name), WR_COLOR (Live colour index), WR_EVENTS (the arrangement clip)

Usage:  wingrec_als_template.py "Ableton 12 Template.als"
"""
import gzip
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
x = gzip.open(sys.argv[1]).read().decode("utf-8")

# the tracks: the first audio track is the prototype, every other audio / MIDI track goes
blocks = []
for tag in ("AudioTrack", "MidiTrack"):
    for m in re.finditer(rf"[ \t]*<{tag} Id=.*?</{tag}>\n?", x, re.S):
        blocks.append((m.start(), m.end(), tag))
blocks.sort()
proto = None
out, pos = [], 0
for s, e, tag in blocks:
    out.append(x[pos:s])
    if tag == "AudioTrack" and proto is None:
        proto = x[s:e]
        out.append("WR_TRACKS\n")
    pos = e
out.append(x[pos:])
x = "".join(out)

proto = re.sub(r'<AudioTrack Id="\d+"', '<AudioTrack Id="WR_TID"', proto, count=1)
proto = re.sub(r'<EffectiveName Value="[^"]*" />', '<EffectiveName Value="WR_NAME" />', proto, count=1)
proto = re.sub(r'<UserName Value="[^"]*" />', '<UserName Value="WR_NAME" />', proto, count=1)
proto = re.sub(r'(</Name>\s*<Color Value=")\d+(" />)', r"\1WR_COLOR\2", proto, count=1)
i = proto.index("<Sample>")
j = proto.index("<Events />", i)
proto = proto[:j] + "<Events>WR_EVENTS</Events>" + proto[j + len("<Events />"):]
assert "WR_COLOR" in proto and proto.count("WR_NAME") == 2

# tempo, next id, locators, the play position
bpm = re.search(r'<Tempo>\s*<LomId Value="0" />\s*<Manual Value="([0-9.]+)" />', x).group(1)
x = x.replace(f'Value="{bpm}"', 'Value="WR_BPM"')
x = re.sub(r'<NextPointeeId Value="\d+" />', '<NextPointeeId Value="WR_NEXTID" />', x, count=1)
x = re.sub(r"<Locators>\s*<Locators />", "<Locators>\n\t\t\t<Locators>WR_LOCATORS</Locators>", x, count=1)
x = re.sub(r'<CurrentTime Value="[0-9.]+" />', '<CurrentTime Value="0" />', x, count=1)
x = re.sub(r'<LoopOn Value="true" />', '<LoopOn Value="false" />', x)
for k in ("WR_BPM", "WR_NEXTID", "WR_LOCATORS", "WR_TRACKS"):
    assert k in x, k

d = os.path.join(ROOT, "res", "wingrec")
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "live12.xml"), "w").write(x)
open(os.path.join(d, "live12_track.xml"), "w").write(proto)
print("wrote res/wingrec/live12.xml (%d bytes) and live12_track.xml (%d bytes)" % (len(x), len(proto)))

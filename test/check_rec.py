#!/usr/bin/env python3
"""Checks what test_rec wrote: the WAVs (sizes, markers), the gzip, the Live set (XML, unique ids, tracks, locators)."""
import gzip, re, struct, sys, collections
import xml.etree.ElementTree as ET
d = sys.argv[1]

def chunks(path):
    b = open(path, "rb").read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", path
    assert struct.unpack("<I", b[4:8])[0] == len(b) - 8, ("riff size", path)
    p, out = 12, {}
    while p + 8 <= len(b):
        cid, sz = b[p:p + 4], struct.unpack("<I", b[p + 4:p + 8])[0]
        out[cid] = b[p + 8:p + 8 + sz]
        p += 8 + sz + (sz & 1)
    assert p == len(b), ("chunk walk", path, p, len(b))
    return out

for name, ch, bits, fmt in (("01 Pad", 2, 24, 1), ("03 Kick", 1, 24, 1), ("04 Float", 1, 32, 3)):
    c = chunks(f"{d}/{name}.wav")
    f, nch, rate, _, ba, bps = struct.unpack("<HHIIHH", c[b"fmt "][:16])
    assert (f, nch, rate, bps) == (fmt, ch, 48000, bits), (name, f, nch, rate, bps)
    assert len(c[b"data"]) == 96001 * ch * bits // 8, (name, len(c[b"data"]))
    n = struct.unpack("<I", c[b"cue "][:4])[0]
    assert n == 2
    assert b"INTRO 1" in c[b"LIST"] and b"DROP & BASS" in c[b"LIST"]
    print("ok", name)
assert gzip.open(f"{d}/z.gz").read() == b"x" * 200000 + b"end"
print("ok gzip")
x = gzip.open(f"{d}/Test.als").read().decode()
root = ET.fromstring(x)
tracks = root.findall("./LiveSet/Tracks/AudioTrack")
assert [t.find("./Name/UserName").get("Value") for t in tracks] == ["Pad", "Kick", "Float"], tracks
ids = collections.Counter()
for el in root.iter():
    if (el.tag.endswith("Target") or el.tag == "Pointee") and "Id" in el.attrib:
        ids[el.get("Id")] += 1
dup = [k for k, v in ids.items() if v > 1]
assert not dup, ("duplicate ids", dup[:10])
nxt = int(root.find("./LiveSet/NextPointeeId").get("Value"))
assert nxt > max(int(k) for k in ids), "NextPointeeId"
for pid in root.iter("PointeeId"):
    assert pid.get("Value") in ids or pid.get("Value") == "0", ("dangling PointeeId", pid.get("Value"))
locs = root.findall("./LiveSet/Locators/Locators/Locator")
assert [l.find("Name").get("Value") for l in locs] == ["INTRO 1", "DROP & BASS"]
assert abs(float(locs[1].find("Time").get("Value")) - 72000 / 48000 * 132 / 60) < 1e-4
clip = tracks[0].find(".//MainSequencer/Sample/ArrangerAutomation/Events/AudioClip")
assert clip is not None and clip.find("./SampleRef/FileRef/RelativePath").get("Value") == "01 Pad.wav"
assert root.find(".//Tempo/Manual").get("Value") == "132.0000"
print("ok als: %d tracks, %d locators, %d ids" % (len(tracks), len(locs), len(ids)))

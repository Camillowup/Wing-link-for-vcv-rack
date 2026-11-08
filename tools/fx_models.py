#!/usr/bin/env python3
"""Writes src/WingFxModels.hpp: the WING's effect models (FW 3.1) for WING-FX, from tools/data/wing_fx_params.json.

The JSON is the parameter appendix of Patrick-Gilles Maillot's "WING Remote Protocols" (V3.1.2), parsed. For every
model: its name on the WING (the value of /fx/N/mdl), PREMIUM (only on FX 1–8) or STANDARD, all its parameter keys in
order (the CC parameter numbers), and the four the panel shows (with range, curve, unit)."""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M = json.load(open(os.path.join(ROOT, "tools", "data", "wing_fx_params.json")))

# the lists of /fx/N/mdl (the document: FX 1–8 take all, FX 9–16 only the standard ones)
PREMIUM = ["HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED", "REVERSE",
           "DEL/REV", "SHIMMER", "SPRING", "DIMCRS", "CHORUS", "FLANGER", "ST-DL", "TAP-DL", "TAPE-DL", "OILCAN",
           "BBD-DL", "PITCH", "D-PITCH", "VSS3", "BPLATE"]
STANDARD = ["GEQ", "PIA", "DOUBLE", "PCORR", "LIMITER", "DE-S2", "ENHANCE", "EXCITER", "P-BASS", "ROTARY", "PHASER",
            "PANNER", "TAPE", "MOOD", "SUB", "RACKAMP", "UKROCK", "ANGEL", "JAZZC", "DELUXE", "BODY", "SOUL", "E88",
            "E84", "F110", "PULSAR", "MACH4", "C5-CMB", "SUB-M", "V-IMG", "SPKMAN", "DEQ3"]

# the parsed tables: fix what the document's layout broke (the flanger is listed as CHORUS, the mood filter is split)
models = {}
seen_chorus = False
pending = None
for m in M:
    mdl = m["mdl"].strip()
    if mdl == "CHORUS":
        if seen_chorus:
            mdl = "FLANGER"
        seen_chorus = True
    if mdl == "V_IMG":
        mdl = "V-IMG"
    if not re.match(r"^[A-Z0-9/\-]+$", mdl):
        if pending:  # the second half of the mood filter
            models[pending]["params"] += m["params"]
        continue
    if mdl in models:
        continue
    models[mdl] = dict(name=(m["name"] or mdl).rstrip(", "), params=m["params"])
    pending = mdl if mdl == "MOOD" else None

# the four on the panel (by hand for the premium effects, else the first four that turn)
PICK = {
    "HALL": "dcy pdel size damp", "ROOM": "dcy pdel size damp", "CHAMBER": "dcy pdel size damp",
    "PLATE": "dcy pdel size damp", "CONCERT": "dcy pdel size damp", "AMBI": "dcy pdel size damp",
    "BPLATE": "dcy pdel size damp", "V-ROOM": "dcy pdel size dens", "V-REV": "dcy pdel lmult hc",
    "V-PLATE": "dcy pdel lc col", "GATED": "dcy pdel att dens", "REVERSE": "dcy pdel rise diff",
    "DEL/REV": "time feed dcy d2r", "SHIMMER": "dcy shim pdel shine", "SPRING": "dcy dens low high",
    "DIMCRS": "sw1 sw2 sw3 sw4", "CHORUS": "spd depl depr sprd", "FLANGER": "spd depl feed phase",
    "ST-DL": "time feed fhc hc", "TAP-DL": "time rep slp diff", "TAPE-DL": "time sust drv wf",
    "OILCAN": "time sust wb tone", "BBD-DL": "dly feed", "PITCH": "semi cent dly mix",
    "D-PITCH": "semi1 semi2 lvl1 lvl2", "VSS3": "dcy rvpdly diff erlvl", "ROTARY": "sw lo hi bal",
    "PHASER": "spd depth reso stg", "PANNER": "spd depth phase wave", "TAPE": "drv spd low hi",
    "MOOD": "fbase reso drv env", "LIMITER": "gin gout att rel", "GEQ": "63 250 1k 4k", "PIA": "63 250 1k 4k",
}
LABEL = {"dcy": "DECAY", "pdel": "PRE", "size": "SIZE", "damp": "DAMP", "dens": "DENSITY", "lmult": "LO MULT",
         "hc": "HI CUT", "lc": "LO CUT", "col": "COLOR", "att": "ATTACK", "rise": "RISE", "diff": "DIFFUSE",
         "time": "TIME", "feed": "FEEDBACK", "d2r": "DLY>REV", "shim": "SHIMMER", "shine": "SHINE", "low": "BASS",
         "high": "TREBLE", "spd": "SPEED", "depl": "DEPTH L", "depr": "DEPTH R", "sprd": "SPREAD", "phase": "PHASE",
         "fhc": "FB HICUT", "rep": "REPEATS", "slp": "SLOPE", "sust": "SUSTAIN", "drv": "DRIVE", "wf": "FLUTTER",
         "wb": "WOBBLE", "tone": "TONE", "dly": "DELAY", "semi": "SEMI", "cent": "CENT", "mix": "MIX",
         "semi1": "SEMI 1", "semi2": "SEMI 2", "lvl1": "LEVEL 1", "lvl2": "LEVEL 2", "rvpdly": "PRE", "erlvl": "EARLY",
         "sw": "SPEED", "lo": "LO", "hi": "HI", "bal": "BALANCE", "depth": "DEPTH", "reso": "RESO",
         "stg": "STAGES", "wave": "WAVE", "fbase": "FREQ", "env": "ENV", "gin": "IN", "gout": "OUT", "rel": "RELEASE",
         "gain": "GAIN", "thr": "THRESH", "ratio": "RATIO", "trim": "TRIM", "out": "OUT", "vol": "VOLUME",
         "bass": "BASS", "mid": "MID", "treb": "TREBLE", "mstr": "MASTER", "wid": "WIDTH", "amnt": "AMOUNT",
         "body": "BODY", "int": "INTENS", "xf": "FREQ", "rng": "RANGE", "oct1": "OCT 1", "oct2": "OCT 2",
         "freq": "FREQ", "lf": "LO FREQ", "lg": "LO GAIN", "hf": "HI FREQ", "hg": "HI GAIN", "g": "GAIN"}


def unit_of(p):
    d = p.get("desc", "")
    u = d.split(",")[0].strip()
    return u if u in ("ms", "s", "Hz", "dB", "%", "m") else ""


def usable(p):
    t = p.get("type")
    if t == "str":
        return bool(p.get("list"))
    if t == "fader":
        return True
    return t in ("int", "linf", "logf") and "min" in p and "max" in p and p["max"] > p["min"]


def fl(x):
    return repr(float(x)) + "f"


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def param_c(p, lab_override=None):
    """One Param initialiser, or None if the parameter can't be turned (no range)."""
    if not usable(p):
        return None, None
    t = p["type"]
    lab = lab_override or LABEL.get(p["key"], p["key"].upper()[:8])
    if t == "str":
        return "LIST", (lab, p)
    if t == "fader":
        return "FADER", (lab, p)
    curve = {"int": "INT", "linf": "LIN", "logf": "LOG"}[t]
    if curve == "LOG" and p["min"] <= 0:
        curve = "LIN"
    return curve, (lab, p)


out = ["// Generated by tools/fx_models.py - do not edit", "// The WING's effect models (FW 3.1), after Patrick-Gilles "
       "Maillot's \"WING Remote Protocols\" V3.1.2, appendix \"Effects and Plugins' Parameters list\".",
       "#pragma once", "namespace wingfx {", "",
       "enum Curve { LIN, LOG, INT, LIST, FADER };",
       "struct Param {", "\tconst char* key;", "\tconst char* label;", "\tint curve;", "\tfloat min, max;",
       "\tconst char* unit;", "\tconst char* const* list;  // LIST: the names (nullptr-terminated)", "};",
       "struct Model {", "\tconst char* mdl;    // the value of /fx/N/mdl", "\tconst char* name;",
       "\tbool premium;      // only on FX 1–8",
       "\tint nAll;", "\tconst Param* all;  // every parameter that turns (a knob can take any of them)",
       "\tint nShow;         // the parameters on the panel by default (≤ 4)",
       "\tint show[4];       // (indices into all)", "\tint timeParam;     // the one that follows the tempo (TIME in ms, index into all), −1: none",
       "\tint nKeys;", "\tconst char* const* keys;  // all its parameters in order: CC parameter n = keys[n − 1]", "};",
       ""]
lists, keys, alls = [], [], []
entries = []
order = [("NONE", False)] + [(m, True) for m in PREMIUM] + [(m, False) for m in STANDARD]
for mdl, prem in order:
    if mdl == "NONE":
        entries.append('\t{"NONE", "None", false, 0, nullptr, 0, {-1, -1, -1, -1}, -1, 0, nullptr},')
        continue
    if mdl not in models:
        raise SystemExit(f"model {mdl} not in the tables")
    m = models[mdl]
    ps = [p for p in m["params"] if p.get("key")]
    sid = re.sub(r"\W", "_", mdl)
    kname, aname = f"K_{sid}", f"A_{sid}"
    keys.append(f"static const char* const {kname}[] = {{{', '.join(cstr(p['key']) for p in ps)}}};")
    allc, allkeys = [], []
    tp = -1
    for p in ps:
        curve, d = param_c(p)
        if not curve:
            continue
        lab, p = d
        if curve == "LIST":
            ln = f"L_{sid}_{len(allc)}"
            lists.append(f"static const char* const {ln}[] = {{{', '.join(cstr(x) for x in p['list'])}, nullptr}};")
            allc.append(f'{{{cstr(p["key"])}, {cstr(lab)}, LIST, 0.f, {fl(len(p["list"]) - 1)}, "", {ln}}}')
        elif curve == "FADER":
            allc.append(f'{{{cstr(p["key"])}, {cstr(lab)}, FADER, -144.f, 10.f, "dB", nullptr}}')
        else:
            u = unit_of(p)
            if p["key"] == "time" and u == "ms" and tp < 0:
                tp = len(allc)
            allc.append(f'{{{cstr(p["key"])}, {cstr(lab)}, {curve}, {fl(p["min"])}, {fl(p["max"])}, {cstr(u)}, nullptr}}')
        allkeys.append(p["key"])
    alls.append(f"static const Param {aname}[] = {{\n\t" + ",\n\t".join(allc) + "\n};" if allc else f"static const Param* const {aname} = nullptr;")
    # the four on the panel by default
    pick = [k for k in PICK.get(mdl, "").split() if k in allkeys]
    for k, p in zip(allkeys, [None] * len(allkeys)):
        pass
    for k in allkeys:
        if len(pick) >= 4:
            break
        if k not in pick:
            pc = next(q for q in ps if q["key"] == k)
            if pc.get("type") != "str":
                pick.append(k)
    show = [allkeys.index(k) for k in pick[:4]]
    nshow = len(show)
    show += [-1] * (4 - len(show))
    entries.append(f'\t{{{cstr(mdl)}, {cstr(m["name"])}, {"true" if prem else "false"}, {len(allc)}, '
                   f'{aname if allc else "nullptr"}, {nshow}, {{{", ".join(map(str, show))}}}, {tp}, {len(ps)}, {kname}}},')
out += lists + keys + alls + ["", "static const Model MODELS[] = {"] + entries + ["};",
       "static const int NUM_MODELS = sizeof(MODELS) / sizeof(MODELS[0]);", "", "}  // namespace wingfx", ""]
open(os.path.join(ROOT, "src", "WingFxModels.hpp"), "w").write("\n".join(out))
print(f"wrote src/WingFxModels.hpp: {len(entries)} models")

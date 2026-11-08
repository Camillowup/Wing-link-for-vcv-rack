// SPDX-License-Identifier: GPL-3.0-only
// WING-LINK: the Behringer WING's remote protocol over USB MIDI, without Rack (tested in test/test_proto.cpp).
//
// Sources: Patrick-Gilles Maillot, "WING Remote Protocols" (V3.1.2, chapter "WING MIDI (Remote-Control)" and
// "SYSEX Messages"), and the behringer.world "WING MIDI Table".
//
//  * Setting a value: sysex  F0 00 20 32 57 02 <ASCII node data> F7, e.g. "/ch.33.fdr=-5.5" or
//    "/ch.33.mute=1,pan=-30" (dots between the levels, values as text, dB for faders and sends, −144 = −∞).
//  * Asking for a value: F0 00 20 32 57 03 /ch.33.fdr F7 -> F0 00 20 32 57 04 <value as text> F7 (no address in the
//    reply: one question at a time). 07 <text> = an error ("NODE NOT FOUND" …). Ident: 00 -> 01 "WING,…".
//  * The WING sends its faders, mutes and pans as CCs when they move (7 bit): MIDI channel 1 faders, 2 mutes, 3 pans;
//    CC 12–31 = ch 1–20, CC 44–63 = ch 21–40, CC 70–77 = aux 1–8. Solo and sends have no CC.
//  * The WING COMPACT's 48 inputs are /ch 1–40 and /aux 1–8: WING-LINK numbers them 1–48 (41–48 = AUX 1–8).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wingproto {

static const int NUM_IN = 48;  // 40 channels + 8 aux

/** The node of input k (1..48): "ch.33" or "aux.2". */
inline std::string node(int k) {
	char b[16];
	if (k <= 40)
		std::snprintf(b, sizeof b, "ch.%d", k);
	else
		std::snprintf(b, sizeof b, "aux.%d", k - 40);
	return b;
}

/** The name shown on the strip: "CH 33" or "AUX 2". */
inline std::string label(int k) {
	char b[16];
	if (k <= 40)
		std::snprintf(b, sizeof b, "CH %d", k);
	else
		std::snprintf(b, sizeof b, "AUX %d", k - 40);
	return b;
}

/** The fader's CC for input k (MIDI channel 1 faders, 2 mutes, 3 pans), −1 if none. */
inline int ccOf(int k) {
	if (k >= 1 && k <= 20)
		return 11 + k;
	if (k >= 21 && k <= 40)
		return 43 + (k - 20);
	if (k >= 41 && k <= 48)
		return 69 + (k - 40);
	return -1;
}

/** The input a CC belongs to (1..48), 0 if none. */
inline int inputOfCc(int cc) {
	if (cc >= 12 && cc <= 31)
		return cc - 11;
	if (cc >= 44 && cc <= 63)
		return cc - 43 + 20;
	if (cc >= 70 && cc <= 77)
		return cc - 69 + 40;
	return 0;
}

// ---------------------------------------------------------------- the fader law (position 0..1 <-> dB)
// As the X32 / WING faders: 0.75 = 0 dB, 1 = +10 dB, 0.5 = −10, 0.25 = −30, 0.0625 = −60, 0 = −∞ (the protocol's
// examples: 0 dB = 0.75, −1 = 0.725, +3 = 0.825).
static const float MINUS_INF = -144.f;

inline float posToDb(float p) {
	if (p <= 0.0005f)
		return MINUS_INF;
	if (p >= 0.5f)
		return p * 40.f - 30.f;
	if (p >= 0.25f)
		return p * 80.f - 50.f;
	if (p >= 0.0625f)
		return p * 160.f - 70.f;
	return p * 480.f - 90.f;
}

inline float dbToPos(float db) {
	if (db <= -89.9f)
		return 0.f;
	float p;
	if (db >= -10.f)
		p = (db + 30.f) / 40.f;
	else if (db >= -30.f)
		p = (db + 50.f) / 80.f;
	else if (db >= -60.f)
		p = (db + 70.f) / 160.f;
	else
		p = (db + 90.f) / 480.f;
	return p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
}

/** dB as the WING takes it: one decimal, −144 for −∞. */
inline std::string dbText(float db) {
	char b[16];
	if (db <= -89.9f)
		return "-144";
	std::snprintf(b, sizeof b, "%.1f", std::round(db * 10.f) / 10.f);
	std::string s = b;
	if (s == "-0.0")
		s = "0.0";
	return s;
}

// ---------------------------------------------------------------- sysex
static const uint8_t HEAD[5] = {0xF0, 0x00, 0x20, 0x32, 0x57};
enum Cmd { IDENT = 0x00, IDENT_REPLY = 0x01, SET = 0x02, GET = 0x03, GET_REPLY = 0x04, ERROR_REPLY = 0x07 };

/** A sysex with the command and the ASCII text (bytes ≥ 0x80 are dropped: sysex data is 7 bit). */
inline std::vector<uint8_t> sysex(int cmd, const std::string& text) {
	std::vector<uint8_t> m(HEAD, HEAD + 5);
	m.push_back((uint8_t) cmd);
	for (unsigned char c : text)
		if (c < 0x80)
			m.push_back(c);
	m.push_back(0xF7);
	return m;
}

/** "/ch.33." + what, e.g. set(33, "fdr=-5.5") or set(33, "mute=1,pan=-30"). */
inline std::vector<uint8_t> set(int k, const std::string& what) { return sysex(SET, "/" + node(k) + "." + what); }
inline std::vector<uint8_t> get(int k, const std::string& what) { return sysex(GET, "/" + node(k) + "." + what); }

/** A name as the WING takes it: at most 16 characters, ASCII, quoted when it holds a space or a comma. */
inline std::string nameText(const std::string& name) {
	std::string s;
	for (unsigned char c : name)
		if (c >= 0x20 && c < 0x7f && c != '"')
			s += (char) c;
	if (s.size() > 16)
		s.resize(16);
	if (s.empty() || s.find_first_of(" ,=") != std::string::npos)
		return "\"" + s + "\"";
	return s;
}

/** A sysex from the WING: its command (−1 if not the WING's) and its text. */
inline int parse(const std::vector<uint8_t>& m, std::string* text) {
	if (m.size() < 7 || m.back() != 0xF7)
		return -1;
	for (int i = 0; i < 5; i++)
		if (m[i] != HEAD[i])
			return -1;
	if (text)
		text->assign(m.begin() + 6, m.end() - 1);
	return m[5];
}

/** A value the WING sent as text: a number ("-1.0", "10", "-oo" / "oo" = −∞, "ON" / "OFF"). */
inline bool number(const std::string& s0, float* v) {
	std::string s;
	for (char c : s0)
		if (c != ' ' && c != '"' && c != '\n' && c != '\r')
			s += c;
	if (s.empty())
		return false;
	if (s == "-oo" || s == "oo" || s == "-inf") {
		*v = MINUS_INF;
		return true;
	}
	if (s == "ON" || s == "on") {
		*v = 1.f;
		return true;
	}
	if (s == "OFF" || s == "off") {
		*v = 0.f;
		return true;
	}
	char* end = nullptr;
	double d = std::strtod(s.c_str(), &end);
	if (end == s.c_str())
		return false;
	*v = (float) d;
	return true;
}

// ---------------------------------------------------------------- colours and icons
/** The WING's channel colours, index 1..12 (the order as in the WING's own colour picker; if they come out shifted on
 * the console, this list is the place to fix). The strip shows the RGB value. */
struct Colour {
	const char* name;
	uint8_t r, g, b;
};
static const Colour COLOURS[] = {
	{"Gray blue", 0x6d, 0x86, 0xa8}, {"Medium blue", 0x3d, 0x7f, 0xff}, {"Dark blue", 0x3a, 0x48, 0xd6},
	{"Turquoise", 0x3d, 0xc9, 0xff}, {"Green", 0x5d, 0xe0, 0x5a}, {"Olive green", 0xb8, 0xf2, 0x3a},
	{"Yellow", 0xff, 0xd8, 0x4a}, {"Orange", 0xff, 0x8a, 0x3d}, {"Red", 0xff, 0x4f, 0x4f},
	{"Coral", 0xff, 0x6b, 0x5b}, {"Pink", 0xe0, 0x77, 0x9e}, {"Mauve", 0x7b, 0x6c, 0xff},
};
static const int NUM_COLOURS = 12;

/** The icon groups (the WING numbers its icons in hundreds: 0–14 general, 100–114 vocals …). */
struct IconGroup {
	const char* tag;
	const char* name;
	int base, count;
};
static const IconGroup ICONS[] = {
	{"GEN", "General", 0, 15},  {"VOX", "Vocals / mics", 100, 15}, {"DRM", "Drums", 200, 25},
	{"STR", "Strings / winds", 300, 20}, {"KEY", "Keys", 400, 10}, {"SPK", "Speakers", 500, 25},
	{"SPC", "Specials", 600, 15},
};
static const int NUM_ICON_GROUPS = 7;

/** "DRM 4" for icon 204, "" if the number is in no group. */
inline std::string iconText(int icon) {
	for (const IconGroup& g : ICONS)
		if (icon >= g.base && icon < g.base + g.count) {
			char b[16];
			std::snprintf(b, sizeof b, "%s %d", g.tag, icon - g.base);
			return b;
		}
	return "";
}

/** The next icon: the first of the next group (a click on the strip's icon). */
inline int nextIconGroup(int icon) {
	for (int i = 0; i < NUM_ICON_GROUPS; i++)
		if (icon >= ICONS[i].base && icon < ICONS[i].base + ICONS[i].count)
			return ICONS[(i + 1) % NUM_ICON_GROUPS].base;
	return ICONS[0].base;
}

}  // namespace wingproto

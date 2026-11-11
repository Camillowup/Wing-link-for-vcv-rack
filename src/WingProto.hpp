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

// ---------------------------------------------------------------- what a strip controls
// A strip follows START CH (AUTO: an input 1..48) or is set to any channel, aux, bus, main, matrix or DCA.
enum TgtType { T_NONE = 0, T_CH, T_AUX, T_BUS, T_MAIN, T_MTX, T_DCA, NUM_TGT };
static const char* const TGT_NODE[NUM_TGT] = {"", "ch", "aux", "bus", "main", "mtx", "dca"};
static const char* const TGT_LABEL[NUM_TGT] = {"", "CH", "AUX", "BUS", "MAIN", "MTX", "DCA"};
static const char* const TGT_NAME[NUM_TGT] = {"", "Channel", "Aux", "Bus", "Main", "Matrix", "DCA"};
static const int TGT_COUNT[NUM_TGT] = {0, 40, 8, 16, 4, 8, 16};

struct Tgt {
	int type = T_NONE, n = 0;
	bool ok() const { return type > T_NONE && type < NUM_TGT && n >= 1 && n <= TGT_COUNT[type]; }
	bool operator==(const Tgt& o) const { return type == o.type && n == o.n; }
	bool audio() const { return type == T_CH || type == T_AUX; }  // takes USB audio, has bus sends
	bool pan() const { return ok() && type != T_DCA; }
	bool solo() const { return ok() && type != T_DCA; }
};
inline Tgt tgt(int type, int n) {
	Tgt t;
	t.type = type;
	t.n = n;
	return t;
}
/** Input k (1..48) of the WING COMPACT: CH 1–40, then AUX 1–8. */
inline Tgt inputTgt(int k) { return k >= 1 && k <= 40 ? tgt(T_CH, k) : (k >= 41 && k <= 48 ? tgt(T_AUX, k - 40) : Tgt()); }
inline std::string tgtNode(const Tgt& t) {
	char b[16];
	std::snprintf(b, sizeof b, "%s.%d", TGT_NODE[t.type], t.n);
	return b;
}
inline std::string tgtLabel(const Tgt& t) {
	if (!t.ok())
		return "–";
	char b[16];
	std::snprintf(b, sizeof b, "%s %d", TGT_LABEL[t.type], t.n);
	return b;
}

/** The CC of a target's fader / mute / pan (MIDI channels 1 / 2 / 3; DCA: faders 4, mutes 5); −1 if none. */
inline int tgtCc(const Tgt& t) {
	switch (t.type) {
		case T_CH: return t.n <= 20 ? 11 + t.n : 43 + (t.n - 20);
		case T_AUX: return 69 + t.n;
		case T_BUS: return 77 + t.n;
		case T_MAIN: return t.n <= 2 ? 93 + t.n : 99 + t.n;  // 94, 95, 102, 103
		case T_MTX: return 103 + t.n;
		case T_DCA: return 11 + t.n;
		default: return -1;
	}
}
/** What a CC on MIDI channel ch (0-based, 0..4) moves: the target and what (0 fader, 1 mute, 2 pan). */
inline bool ccTgt(int ch, int cc, Tgt* t, int* what) {
	if (ch == 3 || ch == 4) {
		if (cc < 12 || cc > 27)
			return false;
		*t = tgt(T_DCA, cc - 11);
		*what = ch == 3 ? 0 : 1;
		return true;
	}
	if (ch > 2)
		return false;
	*what = ch;
	if (cc >= 12 && cc <= 31) *t = tgt(T_CH, cc - 11);
	else if (cc >= 44 && cc <= 63) *t = tgt(T_CH, cc - 43 + 20);
	else if (cc >= 70 && cc <= 77) *t = tgt(T_AUX, cc - 69);
	else if (cc >= 78 && cc <= 93) *t = tgt(T_BUS, cc - 77);
	else if (cc == 94 || cc == 95) *t = tgt(T_MAIN, cc - 93);
	else if (cc == 102 || cc == 103) *t = tgt(T_MAIN, cc - 99);
	else if (cc >= 104 && cc <= 111) *t = tgt(T_MTX, cc - 103);
	else
		return false;
	return true;
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

// ---------------------------------------------------------------- numbers as text, whatever the locale
// (printf and strtod follow the host's locale: a DAW may set a decimal comma, and "fdr=-5,5" is two values for the WING)
/** v with a fixed number of decimals (0..6) and a decimal point: fixed(-5.46, 1) = "-5.5". */
inline std::string fixed(double v, int decimals) {
	if (!(v == v))
		return "0";
	if (decimals < 0)
		decimals = 0;
	if (decimals > 6)
		decimals = 6;
	double scale = 1.0;
	for (int i = 0; i < decimals; i++)
		scale *= 10.0;
	double a = std::fabs(v) * scale;
	if (a > 9e17)
		a = 9e17;
	unsigned long long n = (unsigned long long) std::floor(a + 0.5);
	bool neg = v < 0.0 && n > 0;
	std::string digits;
	do {
		digits.insert(digits.begin(), (char) ('0' + (int) (n % 10)));
		n /= 10;
	} while (n > 0);
	while ((int) digits.size() <= decimals)
		digits.insert(digits.begin(), '0');
	std::string s = neg ? "-" : "";
	s += digits.substr(0, digits.size() - decimals);
	if (decimals > 0)
		s += "." + digits.substr(digits.size() - decimals);
	return s;
}

/** A number at the start of s ("-5.5", "+3", ".5", "1e3"; the decimal point only): false if there is none. */
inline bool parseNumber(const char* s, double* v) {
	const char* p = s;
	bool neg = false;
	if (*p == '+' || *p == '-')
		neg = *p++ == '-';
	double x = 0.0;
	int nd = 0;
	for (; *p >= '0' && *p <= '9'; p++, nd++)
		x = x * 10.0 + (*p - '0');
	if (*p == '.') {
		double f = 0.1;
		for (p++; *p >= '0' && *p <= '9'; p++, nd++, f *= 0.1)
			x += (*p - '0') * f;
	}
	if (nd == 0)
		return false;
	if (*p == 'e' || *p == 'E') {
		const char* q = p + 1;
		bool eneg = false;
		if (*q == '+' || *q == '-')
			eneg = *q++ == '-';
		if (*q >= '0' && *q <= '9') {
			int e = 0;
			for (; *q >= '0' && *q <= '9'; q++)
				if (e < 400)
					e = e * 10 + (*q - '0');
			x *= std::pow(10.0, eneg ? -e : e);
		}
	}
	*v = neg ? -x : x;
	return true;
}

/** dB as the WING takes it: one decimal, −144 for −∞. */
inline std::string dbText(float db) {
	if (db <= -89.9f)
		return "-144";
	return fixed(db, 1);
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
inline std::vector<uint8_t> setPath(const std::string& path) { return sysex(SET, "/" + path); }
inline std::vector<uint8_t> getPath(const std::string& path) { return sysex(GET, "/" + path); }

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
	// "10k02" = 10.02 k, "2k5" = 2.5 k (the WING writes frequencies so)
	size_t kpos = s.find('k');
	if (kpos != std::string::npos && kpos > 0 && s.find_first_not_of("0123456789.-k") == std::string::npos) {
		std::string t = s;
		t[kpos] = '.';
		double d;
		if (!parseNumber(t.c_str(), &d))
			return false;
		*v = (float) (d * 1000.0);
		return true;
	}
	double d;
	if (!parseNumber(s.c_str(), &d))
		return false;
	*v = (float) d;
	return true;
}

/** An FX node: "fx.3". */
inline std::string fxNode(int slot) {
	char b[16];
	std::snprintf(b, sizeof b, "fx.%d", slot);
	return b;
}
inline std::vector<uint8_t> fxSet(int slot, const std::string& what) { return sysex(SET, "/" + fxNode(slot) + "." + what); }
inline std::vector<uint8_t> fxGet(int slot, const std::string& what) { return sysex(GET, "/" + fxNode(slot) + "." + what); }

// The FX CCs (MIDI channels 9–16; FX n and FX n+8 share a channel): ON, MIX, MODEL, parameters 1..33.
// FX 1–8: CC12 insert on, CC13 mix, CC14 model, CC15–31 = parameters 1–17, CC44–58 = 18–32.
// FX 9–16: CC70 insert on, CC71 mix, CC72 model, CC73–95 = parameters 1–23, CC102–111 = 24–33.
enum FxCc { FXCC_NONE, FXCC_ON, FXCC_MIX, FXCC_MODEL, FXCC_PARAM };
/** What a CC on MIDI channel ch (0-based, 8..15) is: returns the kind, sets slot (1..16) and param (1..33). */
inline int fxCc(int ch, int cc, int* slot, int* param) {
	if (ch < 8 || ch > 15)
		return FXCC_NONE;
	int lo = ch - 8 + 1, hi = ch - 8 + 9;
	*param = 0;
	if (cc == 12 || cc == 70) {
		*slot = cc == 12 ? lo : hi;
		return FXCC_ON;
	}
	if (cc == 13 || cc == 71) {
		*slot = cc == 13 ? lo : hi;
		return FXCC_MIX;
	}
	if (cc == 14 || cc == 72) {
		*slot = cc == 14 ? lo : hi;
		return FXCC_MODEL;
	}
	if (cc >= 15 && cc <= 31) { *slot = lo; *param = cc - 14; return FXCC_PARAM; }
	if (cc >= 44 && cc <= 58) { *slot = lo; *param = cc - 44 + 18; return FXCC_PARAM; }
	if (cc >= 73 && cc <= 95) { *slot = hi; *param = cc - 72; return FXCC_PARAM; }
	if (cc >= 102 && cc <= 111) { *slot = hi; *param = cc - 102 + 24; return FXCC_PARAM; }
	return FXCC_NONE;
}
/** The ON CC of FX slot (1..16): channel (0-based) and CC number. */
inline void fxOnCc(int slot, int* ch, int* cc) {
	*ch = 8 + (slot - 1) % 8;
	*cc = slot <= 8 ? 12 : 70;
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

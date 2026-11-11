// SPDX-License-Identifier: GPL-3.0-only
// WING-FX: a knob (0..1) <-> an effect parameter's value, as text for the WING and for the panel (without Rack).
#pragma once
#include "WingProto.hpp"
#include "WingFxModels.hpp"
#include <cstring>

namespace wingfx {

inline int listSize(const Param& p) {
	int n = 0;
	if (p.list)
		while (p.list[n])
			n++;
	return n;
}

/** The knob's value (0..1) as the parameter's value (LIST: the index). */
inline float toValue(const Param& p, float n) {
	n = n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
	switch (p.curve) {
		case LOG: return p.min * std::pow(p.max / p.min, n);
		case INT: return std::round(p.min + (p.max - p.min) * n);
		case LIST: {
			int c = listSize(p);
			return c > 0 ? std::round(n * (c - 1)) : 0.f;
		}
		case FADER: return wingproto::posToDb(n);
		default: return p.min + (p.max - p.min) * n;
	}
}

/** The parameter's value as a knob position (0..1). */
inline float toNorm(const Param& p, float v) {
	float n;
	switch (p.curve) {
		case LOG: n = (v <= p.min) ? 0.f : std::log(v / p.min) / std::log(p.max / p.min); break;
		case LIST: {
			int c = listSize(p);
			n = c > 1 ? v / (c - 1) : 0.f;
			break;
		}
		case FADER: n = wingproto::dbToPos(v); break;
		default: n = p.max > p.min ? (v - p.min) / (p.max - p.min) : 0.f;
	}
	return n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
}

/** The value as the WING takes it. */
inline std::string wingText(const Param& p, float v) {
	char b[32];
	switch (p.curve) {
		case LIST: {
			int i = (int) v, c = listSize(p);
			if (i < 0 || i >= c)
				return "";
			std::string t = p.list[i];
			return t.find_first_of(" ,") != std::string::npos ? "\"" + t + "\"" : t;
		}
		case INT: std::snprintf(b, sizeof b, "%d", (int) std::round(v)); return b;
		case FADER: return wingproto::dbText(v);
		default:  // (a decimal point whatever the locale)
			return wingproto::fixed(v, std::fabs(v) >= 100.f ? 0 : (std::fabs(v) >= 10.f ? 1 : 3));
	}
}

/** The value for the panel: "2.4 s", "8k0 Hz" → "8.0 kHz", "35 %". */
inline std::string panelText(const Param& p, float v) {
	char b[32];
	if (p.curve == LIST) {
		int i = (int) v;
		return (i >= 0 && i < listSize(p)) ? p.list[i] : "";
	}
	if (p.curve == FADER)
		return v <= -89.9f ? "-inf dB" : wingproto::dbText(v) + " dB";
	std::string u = p.unit ? p.unit : "";
	if (u == "Hz" && v >= 1000.f) {
		std::snprintf(b, sizeof b, v >= 10000.f ? "%.1f kHz" : "%.2f kHz", v / 1000.f);
		return b;
	}
	if (p.curve == INT || std::fabs(v) >= 100.f)
		std::snprintf(b, sizeof b, "%.0f", v);
	else if (std::fabs(v) >= 10.f)
		std::snprintf(b, sizeof b, "%.1f", v);
	else
		std::snprintf(b, sizeof b, "%.2f", v);
	return u.empty() ? std::string(b) : std::string(b) + " " + u;
}

/** A value the WING sent (a number, or a name for LIST). */
inline bool parseValue(const Param& p, const std::string& text, float* v) {
	if (p.curve == LIST) {
		std::string t;
		for (char c : text)
			if (c != '"' && c != '\n' && c != '\r')
				t += c;
		while (!t.empty() && t.back() == ' ')
			t.pop_back();
		while (!t.empty() && t[0] == ' ')
			t.erase(0, 1);
		for (int i = 0; i < listSize(p); i++)
			if (t == p.list[i]) {
				*v = (float) i;
				return true;
			}
		return false;
	}
	return wingproto::number(text, v);
}

/** The parameter on knob k (0..3): the one picked for it (choice: index into all, −1: none), else the default. */
inline const Param* knobParam(const Model& M, int k, int choice) {
	if (choice >= 0 && choice < M.nAll)
		return &M.all[choice];
	if (k >= 0 && k < 4 && M.show[k] >= 0)
		return &M.all[M.show[k]];
	return nullptr;
}

/** A parameter by its key (index into all), −1 if the model has none (or it doesn't turn). */
inline int findKey(const Model& M, const char* key) {
	for (int i = 0; i < M.nAll; i++)
		if (!std::strcmp(M.all[i].key, key))
			return i;
	return -1;
}

/** The model with this name on the WING (index into MODELS), 0 (NONE) if unknown. */
inline int modelIndex(const std::string& name0) {
	std::string name;
	for (char c : name0)
		if (c != '"' && c != ' ' && c != '\n' && c != '\r')
			name += c;
	for (int i = 0; i < NUM_MODELS; i++)
		if (name == MODELS[i].mdl)
			return i;
	return 0;
}

/** For the dropdown: the effects by kind. */
struct Group {
	const char* name;
	bool premium;
	const char* models;  // space separated
};
static const Group GROUPS[] = {
	{"Reverbs", true, "HALL ROOM CHAMBER PLATE CONCERT AMBI V-ROOM V-REV V-PLATE BPLATE VSS3 GATED REVERSE SHIMMER SPRING"},
	{"Delays", true, "ST-DL TAP-DL TAPE-DL OILCAN BBD-DL DEL/REV"},
	{"Modulation", true, "CHORUS FLANGER DIMCRS"},
	{"Pitch", true, "PITCH D-PITCH"},
	{"EQ", false, "GEQ PIA MACH4 SOUL E88 E84 F110 PULSAR DEQ3"},
	{"Dynamics", false, "LIMITER C5-CMB DE-S2"},
	{"Amps and tape", false, "RACKAMP UKROCK ANGEL JAZZC DELUXE TAPE"},
	{"Modulation and filter", false, "PHASER PANNER ROTARY MOOD"},
	{"Enhance and bass", false, "ENHANCE EXCITER P-BASS SUB SUB-M BODY V-IMG DOUBLE PCORR SPKMAN"},
};
static const int NUM_GROUPS = sizeof(GROUPS) / sizeof(GROUPS[0]);

}  // namespace wingfx

// SPDX-License-Identifier: GPL-3.0-only
// WING-LINK, WING-LINK X and WING-FX: the modules (src/WingLink.cpp: WING-LINK's engine and panels, src/WingFx.cpp:
// WING-FX's panel). WING-LINK does all the talking to the WING; WING-LINK X and WING-FX are its panels: they link to
// it by docking, by right-click (Connect to WING-LINK) or by themselves when there's only one WING-LINK in the patch.
#pragma once
#include "plugin.hpp"
#include "Ui.hpp"
#include "WingProto.hpp"
#include "WingFxMap.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <set>

namespace winglink {

namespace P = wingproto;

static const int NS = 8;       // strips per module
static const int NT = 2 * NS;  // strips with WING-LINK X
static const int NSLOT = 16;   // the WING's FX slots
static const int NPAGE = 4;    // FX slots on WING-FX

// ---------------------------------------------------------------- the strips' parameters (WING-LINK and X)
enum Field { FADER, PAN, FX1, FX2, MUTE, SOLO, NF };
enum ChInput { AUDIO, CV_FDR, CV_PAN, CV_FX1, CV_FX2, NCI };
enum ChOutput { OUT_FDR, OUT_MUTE, NCO };
inline int pid(int j, int f) { return j * NF + f; }
inline int iid(int j, int k) { return j * NCI + k; }
inline int oid(int j, int k) { return j * NCO + k; }

// ---------------------------------------------------------------- WING-FX's parameters
enum FxField { FON, FMIX, FP1, FP2, FP3, FP4, NFX };
static const int FMODEL = 100;  // (a query: the model)
static const int FTIME = 101;   // (a query: the delay time, for the tempo)
inline int fxPid(int j, int f) { return j * NFX + f; }
inline int fxCv(int j, int k) { return j * 5 + k; }  // k: 0 MIX, 1..4 P1..P4

// ---------------------------------------------------------------- WING-EQ's parameters (a channel's STD EQ and filter)
// bands b = 0 LOW, 1..4 BAND 1–4, 5 HIGH; each GAIN (dB), FREQ and Q (0..1, logarithmic)
enum EqField { EQ_ON = 0, EQ_BAND = 1, EQ_LSHV = 19, EQ_HSHV, EQ_LC, EQ_LCF, EQ_HC, EQ_HCF, NEQ };
static const int NBAND = 6;
static const int EQ_MDL = 100;  // (a query: the EQ model)
inline int eqG(int b) { return EQ_BAND + b * 3; }
inline int eqF(int b) { return EQ_BAND + b * 3 + 1; }
inline int eqQ(int b) { return EQ_BAND + b * 3 + 2; }
inline bool eqSwitch(int f) { return f == EQ_ON || f == EQ_LSHV || f == EQ_HSHV || f == EQ_LC || f == EQ_HC; }
inline bool eqGain(int f) { return f >= EQ_BAND && f < EQ_LSHV && (f - EQ_BAND) % 3 == 0; }
/** A logarithmic field's range (FREQ, Q, the filters' frequencies): lo..hi; false for the others. */
inline bool eqLogRange(int f, float* lo, float* hi) {
	if (f == EQ_LCF) { *lo = 20.f; *hi = 2000.f; return true; }
	if (f == EQ_HCF) { *lo = 50.f; *hi = 20000.f; return true; }
	if (f < EQ_BAND || f >= EQ_LSHV || eqGain(f))
		return false;
	int b = (f - EQ_BAND) / 3;
	if ((f - EQ_BAND) % 3 == 2) { *lo = 0.44f; *hi = 10.f; return true; }
	*lo = b == 5 ? 50.f : 20.f;
	*hi = b == 0 ? 2000.f : 20000.f;
	return true;
}
/** The knob (0..1, or dB for a gain, 0 / 1 for a switch) → the WING's value, and back. */
inline float eqValue(int f, float p) {
	float lo, hi;
	if (eqLogRange(f, &lo, &hi))
		return lo * std::pow(hi / lo, clamp(p, 0.f, 1.f));
	return p;
}
inline float eqParam(int f, float v) {
	float lo, hi;
	if (eqLogRange(f, &lo, &hi))
		return clamp(std::log(std::max(v, lo) / lo) / std::log(hi / lo), 0.f, 1.f);
	return v;
}
/** The node below the channel (eq.lg, flt.lcf …). */
inline std::string eqNode(int f) {
	static const char* const B[NBAND] = {"l", "1", "2", "3", "4", "h"};
	static const char* const K[3] = {"g", "f", "q"};
	switch (f) {
		case EQ_ON: return "eq.on";
		case EQ_LSHV: return "eq.leq";
		case EQ_HSHV: return "eq.heq";
		case EQ_LC: return "flt.lc";
		case EQ_LCF: return "flt.lcf";
		case EQ_HC: return "flt.hc";
		case EQ_HCF: return "flt.hcf";
		default: return std::string("eq.") + B[(f - EQ_BAND) / 3] + K[(f - EQ_BAND) % 3];
	}
}
/** A frequency as the panels show it (80 Hz, 2k5, 12k). */
inline std::string freqText(float v) {
	if (v < 1000.f)
		return string::f("%.0f Hz", v);
	if (v < 10000.f)
		return string::f("%dk%d", (int) (v / 1000.f), (int) std::fmod(v / 100.f, 10.f));
	return string::f("%.0fk", v / 1000.f);
}
static const char* const EQ_MODELS[7] = {"STD", "SOUL", "E88", "E84", "F110", "PULSAR", "MACH4"};

/** What we know of a value both sides change. */
struct Sync {
	float wing = NAN;   // what the WING has (as far as we know)
	float local = NAN;  // what the panel had at the last look
	float hold = 0.f;   // after a change here: the WING's echoes are ignored this long (s)
	bool dirty = false; // to send
};

struct Strip {
	Sync f[NF];
	float peak = 0.f, env = 0.f;
	std::atomic<int> tType{0}, tNum{0};  // what it controls: T_NONE = AUTO (follows START CH), else a fixed one
};

/** Name, colour and icon of one WING channel / bus …: they belong to the WING's channel, not to a strip. */
struct Meta {
	std::string name;
	int colour = 0;    // 1..12, 0: unknown
	int icon = -1;     // −1: unknown
	bool known = false;          // read from the WING (or set here)
	int dirty = 0;               // to send: 1 name, 2 colour, 4 icon
	float wait = 0.f, hold = 0.f;  // send after a pause; the WING's old values are ignored this long
	// The WING shows (and edits on its home page) the name / colour / icon of the channel's SOURCE while the channel
	// is linked to it (clink = 1): then they are read and written there (e.g. /io.in.USB.1).
	int link = -1;         // clink: −1 unknown
	std::string grp;       // in.conn.grp (LCL, USB, A …), "" unknown
	int in = 0;            // in.conn.in
	bool srcBad = false;   // the WING refused the source's name: use the channel's
};
enum { M_NAME = 1, M_COL = 2, M_ICON = 4, M_LINK = 8, M_GRP = 16, M_IN = 32 };

struct FxSlot {
	std::atomic<int> model{0};  // index into wingfx::MODELS
	bool modelKnown = false;
	Sync f[NFX];                // knob positions (0..1); ON 0 / 1
	float val[NFX] = {};        // the last known positions
	bool known[NFX] = {};
	float timeMs = NAN;         // the delay time on the WING (models with a TIME), for the tempo
};

struct WingLink;

/** WING-LINK X and WING-FX: panels of a WING-LINK. */
struct Satellite : Module {
	std::atomic<int64_t> linkId{-1};          // a chosen WING-LINK (−1: docked, or the only one)
	std::atomic<WingLink*> host{nullptr};     // the WING-LINK it belongs to (set by the UI)
	virtual bool dockLeft() = 0;              // the WING-LINK is on its left (WING-LINK X) or right (WING-FX)
	WingLink* hostSafe();                     // (UI thread) the host if it still exists
	void onRemove(const RemoveEvent& e) override;
	~Satellite();
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};

struct WingLinkX : Satellite {
	enum ParamId { DOWN_PARAM = NS * NF, UP_PARAM, NUM_PARAMS };
	enum LightId { MUTE_LIGHT, SOLO_LIGHT = MUTE_LIGHT + NS, NUM_LIGHTS = SOLO_LIGHT + NS };
	std::atomic<int> xStart{-1};  // its first WING input (1..48); −1: follows WING-LINK (its START CH + 8)
	std::atomic<int> xStep{8};    // START CH − / + move by 1 or 8
	WingLinkX();
	bool dockLeft() override { return true; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};

struct WingFx : Satellite {
	enum ParamId { PAGE_DOWN_PARAM = NPAGE * NFX, PAGE_UP_PARAM, TAP_PARAM, SYNC_PARAM, NUM_PARAMS };
	enum InputId { CLOCK_INPUT = NPAGE * 5, NUM_INPUTS };
	enum OutputId { CLOCK_OUTPUT, NUM_OUTPUTS };
	enum LightId { ON_LIGHT, SYNC_LIGHT = ON_LIGHT + NPAGE, TAP_LIGHT, NUM_LIGHTS };
	std::atomic<int> first{0};      // the first slot shown (0..12)
	std::atomic<int> slotStep{4};   // SLOTS − / + move by 1 or 4
	std::atomic<int> ppqIn{4}, ppqOut{4};  // clock pulses per quarter note (4 = 16ths)
	std::atomic<int> choice[wingfx::NUM_MODELS][4];  // per effect: the parameter on each knob (index into all, −1: default)
	std::atomic<bool> refresh{false};  // (a knob got another parameter: show it, ask the WING)
	WingFx();
	void process(const ProcessArgs& args) override;
	const wingfx::Param* knob(int model, int k) { return wingfx::knobParam(wingfx::MODELS[model], k, choice[model][k]); }
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};

struct WingEq : Satellite {
	enum ParamId { PREV_PARAM = NEQ, NEXT_PARAM, FLAT_PARAM, NUM_PARAMS };
	enum InputId { CV_LC = NBAND * 3, CV_HC, CV_ON, NUM_INPUTS };  // band b: b * 3 + 0 GAIN, 1 FREQ, 2 Q
	enum LightId { ON_LIGHT, LSHV_LIGHT, HSHV_LIGHT, LC_LIGHT, HC_LIGHT, NUM_LIGHTS };
	enum Mode { M_STRIP, M_FIXED, M_SELECT };
	std::atomic<int> mode{M_STRIP};
	std::atomic<int> strip{0};       // (M_STRIP) the strip of WING-LINK (0..15) whose channel it shows
	std::atomic<int> fixedKey{101};  // (M_FIXED) the channel: type * 100 + number (101: CH 1)
	std::atomic<int> shownKey{-1};   // (the engine) the channel shown; −1: none (not a channel)
	std::atomic<int> model{-1};      // the channel's EQ model on the WING (index into EQ_MODELS), −1 unknown
	WingEq();
	void process(const ProcessArgs& args) override;
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};
/** The input that moves a field (−1: none). */
inline int eqInput(int f) {
	if (f >= EQ_BAND && f < EQ_LSHV)
		return f - EQ_BAND;
	if (f == EQ_LCF)
		return WingEq::CV_LC;
	if (f == EQ_HCF)
		return WingEq::CV_HC;
	if (f == EQ_ON)
		return WingEq::CV_ON;
	return -1;
}

bool hostAlive(WingLink* h);
void linkPanels(Satellite* s);
void appendLinkMenu(Menu* menu, Satellite* s);
void addScrews(ModuleWidget* w);
FxSlot* hostFx(WingLink* h, int slot);
int hostFxBus(WingLink* h, int k);
void requestFxModel(WingLink* h, int slot, int model);
float hostRackBpm(WingLink* h);
float hostWingBpm(WingLink* h, int* slot);
bool hostLinked(WingLink* h, WingFx* f);
bool hostLinkedEq(WingLink* h, WingEq* e);
Meta hostMetaOf(WingLink* h, int key);
std::string hostStripLabel(WingLink* h, int s);
bool hostHasX(WingLink* h);

}  // namespace winglink

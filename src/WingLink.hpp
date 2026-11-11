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
/** (UI thread) The last frame's duration; Rack gives NaN on the very first frame after starting (a NaN would stop every
 *  timer that counts down with it for good). */
inline float frameDt() {
	double d = APP->window->getLastFrameDuration();
	return std::isfinite(d) && d > 0.0 ? (float) std::min(d, 1.0) : 0.f;
}

struct Satellite : Module {
	std::atomic<int64_t> linkId{-1};          // a chosen WING-LINK (−1: docked, or the only one)
	std::atomic<int64_t> lastId{-1};          // the WING-LINK it was linked to last (saved: it finds it again)
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

// ---------------------------------------------------------------- WING-DYN: a channel's gate and compressor (STD: GATE, COMP)
enum DynField {
	D_GON, D_GTHR, D_GRANGE, D_GATT, D_GHOLD, D_GREL, D_GACC,
	D_CON, D_CTHR, D_CRATIO, D_CGAIN, D_CMIX, D_CKNEE, D_CATT, D_CHOLD, D_CREL, D_CDET, D_CENV, D_CAUTO,
	NDYN
};
static const int D_GMDL = 100, D_CMDL = 101;  // (queries: the models)
static const int NRATIO = 17;
static const char* const DYN_RATIOS[NRATIO] = {"1.1", "1.2", "1.3", "1.5", "1.7", "2.0", "2.5", "3.0", "3.5",
                                               "4.0", "5.0", "6.0", "8.0", "10", "20", "50", "100"};
struct DynSpec {
	const char* node;   // below the channel
	float lo, hi, def;  // the knob's range (dB, ms, %; a release 0..1 logarithmic; a switch 0..1; RATIO an index)
	bool sw, logr;      // a switch; a release (4..4000 ms, logarithmic)
};
static const DynSpec DYN[NDYN] = {
	{"gate.on", 0, 1, 0, true, false},       {"gate.thr", -80, 0, -40, false, false},
	{"gate.range", 3, 60, 30, false, false}, {"gate.att", 0, 120, 1, false, false},
	{"gate.hld", 0, 200, 20, false, false},  {"gate.rel", 0, 1, 0.45f, false, true},
	{"gate.acc", 0, 100, 0, false, false},
	{"dyn.on", 0, 1, 0, true, false},        {"dyn.thr", -60, 0, -20, false, false},
	{"dyn.ratio", 0, NRATIO - 1, 9, false, false}, {"dyn.gain", -6, 12, 0, false, false},
	{"dyn.mix", 0, 100, 100, false, false},  {"dyn.knee", 0, 5, 2, false, false},
	{"dyn.att", 0, 120, 10, false, false},   {"dyn.hld", 1, 200, 20, false, false},
	{"dyn.rel", 0, 1, 0.4f, false, true},    {"dyn.det", 0, 1, 0, true, false},
	{"dyn.env", 0, 1, 1, true, false},       {"dyn.auto", 0, 1, 0, true, false},
};
inline float dynRelMs(float p) { return 4.f * std::pow(1000.f, clamp(p, 0.f, 1.f)); }
inline float dynRelParam(float ms) { return clamp(std::log(std::max(ms, 4.f) / 4.f) / std::log(1000.f), 0.f, 1.f); }

struct WingDyn : Satellite {
	enum ParamId { PREV_PARAM = NDYN, NEXT_PARAM, NUM_PARAMS };
	enum InputId { CV_G_THR, CV_G_RANGE, CV_G_ON, CV_G_ATT, CV_G_REL,
		           CV_C_THR, CV_C_RATIO, CV_C_GAIN, CV_C_MIX, CV_C_KNEE, CV_C_ATT, CV_C_REL, CV_C_ON, NUM_INPUTS };
	enum LightId { GON_LIGHT, CON_LIGHT, DET_LIGHT, ENV_LIGHT, AUTO_LIGHT, NUM_LIGHTS };
	std::atomic<int> mode{WingEq::M_STRIP};
	std::atomic<int> strip{0};
	std::atomic<int> fixedKey{101};
	std::atomic<int> shownKey{-1};
	std::atomic<int> gateModel{-1}, compModel{-1};  // 1: the STD model (GATE / COMP), 0: another, −1 unknown
	char gateMdl[12] = "", compMdl[12] = "";
	WingDyn();
	void process(const ProcessArgs& args) override;
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};
/** The input that moves a field (−1: none). */
inline int dynInput(int f) {
	switch (f) {
		case D_GTHR: return WingDyn::CV_G_THR;
		case D_GRANGE: return WingDyn::CV_G_RANGE;
		case D_GON: return WingDyn::CV_G_ON;
		case D_GATT: return WingDyn::CV_G_ATT;
		case D_GREL: return WingDyn::CV_G_REL;
		case D_CTHR: return WingDyn::CV_C_THR;
		case D_CRATIO: return WingDyn::CV_C_RATIO;
		case D_CGAIN: return WingDyn::CV_C_GAIN;
		case D_CMIX: return WingDyn::CV_C_MIX;
		case D_CKNEE: return WingDyn::CV_C_KNEE;
		case D_CATT: return WingDyn::CV_C_ATT;
		case D_CREL: return WingDyn::CV_C_REL;
		case D_CON: return WingDyn::CV_C_ON;
		default: return -1;
	}
}

// ---------------------------------------------------------------- WING-KNOBS: the WING's custom controls (MIDI CC / notes)
static const int NKNOB = 8;
struct WingKnobs : Satellite {
	enum ParamId { ENUMS(KLVL_PARAM, NKNOB), ENUMS(BLVL_PARAM, NKNOB), NUM_PARAMS };
	enum OutputId { ENUMS(K_OUTPUT, NKNOB), ENUMS(B_OUTPUT, NKNOB), NUM_OUTPUTS };
	enum LightId { NUM_LIGHTS };
	// each slot: what it learned (−1: nothing): MIDI channel * 256 + CC (a note: + 128 in the low byte)
	std::atomic<int> learned[2 * NKNOB];
	std::atomic<int> value[2 * NKNOB];   // 0..127
	std::atomic<int> learning{-1};       // the slot learning now (−1: none)
	std::string names[2 * NKNOB];        // (UI) its name
	WingKnobs();
	/** (the engine of WING-LINK) a CC or a note from the WING; true when a slot took it */
	bool feed(int ch, int num, int val, bool note);
	void process(const ProcessArgs& args) override;
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};

// ---------------------------------------------------------------- WING-IN: audio from the WING's USB outputs
static const int NIN = 8;
/** A source on the WING for a USB output: a physical input (LCL, AUX, A …) straight, or a channel / bus / main / matrix
 *  through a user signal (USR n, n = the slot: 1..8). */
enum InGroup { IG_OFF, IG_LCL, IG_AUX, IG_A, IG_B, IG_C, IG_SC, IG_USB, IG_CH, IG_AUXCH, IG_BUS, IG_MAIN, IG_MTX, NIG };
static const char* const IG_NAMES[NIG] = {"OFF", "LCL", "AUX", "A", "B", "C", "SC", "USB", "CH", "AUX", "BUS", "MAIN", "MTX"};
inline bool igInternal(int g) { return g >= IG_CH; }
inline int igMax(int g) {
	static const int MX[NIG] = {0, 24, 8, 48, 48, 48, 32, 48, 40, 8, 16, 4, 8};
	return g >= 0 && g < NIG ? MX[g] : 0;
}
struct InSource {
	int grp = IG_OFF, n = 1;
	int tap = 1;  // (internal) 0 PRE, 1 POST
	int lr = 0;   // (internal) 0 L+R, 1 L, 2 R
};
struct WingIn : Satellite {
	enum ParamId { ENUMS(LVL_PARAM, NIN), DOWN_PARAM, UP_PARAM, SYNC_PARAM, NUM_PARAMS };
	enum OutputId { ENUMS(OUTPUT, NIN), NUM_OUTPUTS };
	enum LightId { SYNC_LIGHT, NUM_LIGHTS };
	std::atomic<int> start{1};           // the first USB output of the WING (1..41)
	std::mutex srcMutex;
	InSource src[NIN];                   // (srcMutex) what each takes
	std::atomic<bool> known[NIN];        // its source read from the WING (or set here)
	std::atomic<bool> syncReq{false};
	std::atomic<bool> pending{false};    // (UI) a source chosen here, not yet sent (SYNC)
	std::atomic<float> audio[NIN];       // (the engine of WING-LINK) the sample now
	std::atomic<float> peak[NIN];        // (its meter)
	std::atomic<bool> hasAudio{false};
	WingIn();
	void process(const ProcessArgs& args) override;
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};
std::string inSourceLabel(const InSource& s);
/** The key (type * 100 + n) of an internal source (for its name and colour), −1 for a physical input. */
inline int inSourceKey(const InSource& s) {
	switch (s.grp) {
		case IG_CH: return wingproto::T_CH * 100 + s.n;
		case IG_AUXCH: return wingproto::T_AUX * 100 + s.n;
		case IG_BUS: return wingproto::T_BUS * 100 + s.n;
		case IG_MAIN: return wingproto::T_MAIN * 100 + s.n;
		case IG_MTX: return wingproto::T_MTX * 100 + s.n;
		default: return -1;
	}
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
bool hostLinkedSat(WingLink* h, Satellite* s);   // (WING-DYN, WING-KNOBS, WING-IN) it is the one this WING-LINK serves
std::string hostAudioName(WingLink* h);
float hostDeviceRate(WingLink* h);
bool hostSysexOk(WingLink* h);
Meta hostMetaOf(WingLink* h, int key);
std::string hostStripLabel(WingLink* h, int s);
bool hostHasX(WingLink* h);

}  // namespace winglink

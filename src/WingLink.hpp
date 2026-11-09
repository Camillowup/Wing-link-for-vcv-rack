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
};
enum { M_NAME = 1, M_COL = 2, M_ICON = 4 };

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
	std::atomic<int> xStep{1};    // START CH − / + move by 1 or 8
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
	const wingfx::Param* knob(int model, int k) { return wingfx::knobParam(wingfx::MODELS[model], k, choice[model][k]); }
	bool dockLeft() override { return false; }
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
};

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

}  // namespace winglink

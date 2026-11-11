// SPDX-License-Identifier: GPL-3.0-only
// WING-LINK – a bridge from VCV Rack to a Behringer WING over one USB cable, in the look of the GENRA plugin.
//
//  * Audio: WING-LINK is an audio port of its own (as Rack's Audio module): its 8 AUDIO inputs, and WING-LINK X's 8,
//    go to the WING's USB inputs (by default USB 33… for CH 33…, so they never clash with an Audio module on 1–16).
//  * Control: USB MIDI (the WING's remote-control port, src/WingProto.hpp): faders, pan, mute, solo and two bus sends
//    both ways; name, colour and icon of every channel from the panel; SYNC also routes the channels to their USB
//    inputs. The WING sends faders / mutes / pans / FX as CCs when they move; exact values are asked for by sysex.
//  * WING-LINK X: 8 more channels. WING-FX: the WING's 16 effects, 4 at a time, and the tempo (Rack clock or tap → the
//    delays; the delays' time → CLOCK out). Both are WING-LINK's panels: WING-LINK reads and writes them.
// Panels: tools/winglink_panel.py, tools/wingfx_panel.py.
#include "WingLink.hpp"
#include "WingLinkLayout.hpp"
#include <map>

namespace winglink {

namespace L = wllayout::wl;
namespace F = wingfx;
using namespace wlui;

// ---------------------------------------------------------------- the WING-LINKs alive (the panels check theirs)
static std::mutex regMutex;
static std::set<WingLink*> registry;

bool hostAlive(WingLink* h) {
	std::lock_guard<std::mutex> lock(regMutex);
	return h && registry.count(h);
}

struct FaderQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float db = P::posToDb(getValue());
		return db <= -89.9f ? "−∞" : string::f("%+.1f", db);
	}
	std::string getUnit() override { return P::posToDb(getValue()) <= -89.9f ? "" : " dB"; }
	void setDisplayValueString(std::string s) override {
		float db;
		if (P::number(s, &db))
			setValue(P::dbToPos(db));
	}
};
struct PanQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int v = (int) std::round(getValue() * 100.f);
		return v == 0 ? "C" : (v < 0 ? string::f("L%d", -v) : string::f("R%d", v));
	}
};

static void configStrips(Module* m, int base) {
	for (int j = 0; j < NS; j++) {
		std::string n = string::f("Strip %d", base + j + 1);
		m->configParam<FaderQuantity>(pid(j, FADER), 0.f, 1.f, 0.75f, n + " fader");
		m->configParam<PanQuantity>(pid(j, PAN), -1.f, 1.f, 0.f, n + " pan");
		m->configParam<FaderQuantity>(pid(j, FX1), 0.f, 1.f, 0.f, n + " FX 1 send");
		m->configParam<FaderQuantity>(pid(j, FX2), 0.f, 1.f, 0.f, n + " FX 2 send");
		m->configSwitch(pid(j, MUTE), 0.f, 1.f, 0.f, n + " mute", {"Off", "On"});
		m->configSwitch(pid(j, SOLO), 0.f, 1.f, 0.f, n + " solo", {"Off", "On"});
		m->configInput(iid(j, AUDIO), n + " audio (to the WING over USB)");
		m->configInput(iid(j, CV_FDR), n + " fader CV (0–10 V, moves the fader)");
		m->configInput(iid(j, CV_PAN), n + " pan CV (±5 V)");
		m->configInput(iid(j, CV_FX1), n + " FX 1 send CV (0–10 V)");
		m->configInput(iid(j, CV_FX2), n + " FX 2 send CV (0–10 V)");
		m->configOutput(oid(j, OUT_FDR), n + " fader (0–10 V, from the WING too)");
		m->configOutput(oid(j, OUT_MUTE), n + " mute gate (10 V when muted)");
	}
}

// ---------------------------------------------------------------- the audio port (as Rack's Audio module, outputs only)
static const int MAXCH = NT;

struct WingAudioPort : audio::Port {
	Module* module;
	dsp::DoubleRingBuffer<dsp::Frame<MAXCH>, 32768> engineInputBuffer;
	dsp::SampleRateConverter<MAXCH> inputSrc;
	std::atomic<int> deviceNumOutputs{0};
	std::atomic<int> deviceTotalOutputs{0};  // (all the device's outputs: 16 for the DAW of Rack's plugin)
	std::atomic<int> wantOffset{0};          // the first device channel: taken at the start of a buffer only
	std::atomic<int> wantInOffset{0};        // (WING-IN) its first device input, the same way
	std::atomic<bool> wantInputs{false};     // (WING-IN) is there
	dsp::DoubleRingBuffer<dsp::Frame<NIN>, 32768> engineOutputBuffer;   // (WING-IN) the WING's audio, at the engine's rate
	dsp::SampleRateConverter<NIN> outputSrc;
	std::atomic<int> deviceNumInputs{0};
	float deviceSampleRate = 0.f;
	int requestedEngineFrames = 0;

	WingAudioPort(Module* m) : module(m) {
		maxInputs = NIN;
		maxOutputs = MAXCH;
		inputSrc.setQuality(6);
		outputSrc.setQuality(6);
	}
	bool isMaster() { return APP->engine->getMasterModule() == module; }

	void processInput(const float* input, int inputStride, int frames) override {
		outputOffset = wantOffset;  // (the same for the whole buffer, whatever the engine changes meanwhile)
		inputOffset = wantInOffset;
		deviceTotalOutputs = device ? device->getNumOutputs() : 0;
		deviceNumOutputs = std::min(getNumOutputs(), MAXCH);
		deviceSampleRate = getSampleRate();
		if (!APP->engine->getMasterModule())
			APP->engine->setMasterModule(module);
		if (isMaster())
			APP->engine->setSuggestedSampleRate(deviceSampleRate);
		float ratio = APP->engine->getSampleRate() / deviceSampleRate;
		requestedEngineFrames = std::max((int) std::ceil(frames * ratio) - (int) engineInputBuffer.size(), 0);
		// (WING-IN) the WING's USB outputs into the engine's rate
		int ni = wantInputs ? std::min(getNumInputs(), NIN) : 0;
		deviceNumInputs = ni;
		if (ni > 0 && input) {
			outputSrc.setRates(deviceSampleRate, APP->engine->getSampleRate());
			outputSrc.setChannels(ni);
			int inFrames = frames;
			int outFrames = engineOutputBuffer.capacity();
			outputSrc.process(input, inputStride, &inFrames, (float*) engineOutputBuffer.endData(), NIN, &outFrames);
			engineOutputBuffer.endIncr(outFrames);
		}
	}
	void processBuffer(const float* input, int inputStride, float* output, int outputStride, int frames) override {
		if (isMaster() && requestedEngineFrames > 0)
			APP->engine->stepBlock(requestedEngineFrames);
	}
	void processOutput(float* output, int outputStride, int frames) override {
		float engineRate = APP->engine->getSampleRate();
		int n = deviceNumOutputs;
		if (n > 0) {
			inputSrc.setRates(engineRate, deviceSampleRate);
			inputSrc.setChannels(n);
			int inFrames = engineInputBuffer.size();
			int outFrames = frames;
			inputSrc.process((const float*) engineInputBuffer.startData(), MAXCH, &inFrames, output, outputStride, &outFrames);
			engineInputBuffer.startIncr(inFrames);
			for (int i = 0; i < outFrames; i++)
				for (int c = 0; c < n; c++)
					output[i * outputStride + c] = clamp(output[i * outputStride + c], -1.f, 1.f);
			for (int i = outFrames; i < frames; i++)
				for (int c = 0; c < n; c++)
					output[i * outputStride + c] = 0.f;
		}
		int maxEngineFrames = (int) std::ceil(frames * engineRate / deviceSampleRate * 2.0) - 1;
		if ((int) engineInputBuffer.size() > maxEngineFrames)
			engineInputBuffer.clear();
	}
	void onStartStream() override {
		engineInputBuffer.clear();
		engineOutputBuffer.clear();
	}
	void onStopStream() override {
		deviceNumOutputs = 0;
		deviceNumInputs = 0;
		deviceTotalOutputs = 0;
		engineOutputBuffer.clear();
		deviceSampleRate = 0.f;
		engineInputBuffer.clear();
		if (APP->engine->getMasterModule() == module)
			APP->engine->setMasterModule_NoLock(NULL);
	}
};

// ---------------------------------------------------------------- WING-LINK
struct WingLink : Module {
	enum ParamId { DOWN_PARAM = NS * NF, UP_PARAM, SYNC_PARAM, TRACKS_PARAM, NUM_PARAMS };
	enum InputId { NUM_INPUTS = NS * NCI };
	enum OutputId { NUM_OUTPUTS = NS * NCO };
	enum LightId { MUTE_LIGHT, SOLO_LIGHT = MUTE_LIGHT + NS, SYNC_LIGHT = SOLO_LIGHT + NS, NUM_LIGHTS };

	// ---- settings (saved)
	std::atomic<int> wingStart{1};    // the WING input of strip 1 (1..48; 41–48 = AUX 1–8)
	std::atomic<int> usbMode{0};      // 0: USB n = WING channel n, else: the first USB channel
	std::atomic<int> fxBus[2] = {{1}, {2}};
	std::atomic<bool> autoMidi{true}, autoAudio{true};
	std::atomic<int> chStep{8};       // START CH − / + move by 1 or 8
	std::atomic<int> audioStart{1};   // the WING input of AUDIO 1 (SYNC sets it to START CH; paging doesn't move it)
	Strip st[NT];
	std::map<int, Meta> meta;          // name, colour, icon per WING channel (key: type * 100 + number)

	// ---- the SD recorder (WING-LIVE card): two cards, each started / stopped on its own
	struct Sd {
		std::atomic<int> state{-1};    // −1 unknown, 0 STOP, 1 PPAUSE, 2 PLAY, 3 REC
		std::atomic<int> card{-1};     // −1 unknown, 0 NONE, 1 READY, 2 PROTECT, 3 ERASE, 4 ERROR
		std::atomic<float> etime{0.f}, sdfree{0.f};  // ms
		std::atomic<bool> toggle{false};  // (the panel: start / stop)
	} sd[2];
	std::mutex metaMutex;  // the names (the UI writes them, the engine sends them)
	// the WING's last complaint (shown in the display) and what it was about
	std::string lastError;              // (errMutex)
	std::atomic<float> errorAge{1e9f};  // (the engine counts it: no lock there)
	std::mutex errMutex;
	float setGap = 0.f, qDrain = 0.f;  // after a command: wait for a complaint; after a lost answer: wait for it
	float qStarve = 0.f;               // how long a question has waited for the commands to pause
	bool setSinceAsk = false;          // a command went out since the question: a complaint may be its
	float metaPoll = 0.f;              // names, colours, icons: asked every 6 s (the values every 1.5 s)
	int metaTick = 0;                  // (the names' timers and sends: every 8th tick)
	bool fxShow = false;               // a slot's model changed: show the panel again (no questions)
	std::string lastSet;
	int lastSetMeta = -1, lastSetPart = 0;
	std::deque<std::string> outbox;     // commands of SYNC (the routing), sent one by one
	std::atomic<int> nameQuote{0};      // names unquoted (0) or "quoted" (1): switched when the WING says no

	// ---- the panels linked to it (set by the UI; cleared when one goes)
	std::atomic<WingLinkX*> xMod{nullptr};
	std::atomic<WingFx*> fxMod{nullptr};
	std::atomic<WingEq*> eqMod{nullptr};
	std::atomic<WingDyn*> dynMod{nullptr};
	std::atomic<WingKnobs*> knobsMod{nullptr};
	std::atomic<WingIn*> inMod{nullptr};

	// ---- connection
	midi::InputQueue midiIn;
	midi::Output midiOut;
	WingAudioPort port;
	std::atomic<bool> sysexOk{false}, midiSeen{false};
	std::string wingIdent;
	std::mutex identMutex;
	int lastInDev = -2, lastOutDev = -2;
	float identTimer = 0.f;

	// one question at a time
	struct Query {
		int kind;  // 0 strip, 1 FX slot, 2 SD card, 3 name / colour / icon of a WING channel (s: its key, f: part),
		           // 4 WING-EQ's channel (s: its key, f: the field), 5 the channel selected on the WING
		int s, f;
		bool src;  // (kind 3: asked at the channel's source; false when left out)
	};
	std::deque<Query> queries;
	bool qWaiting = false;
	Query qNow{0, 0, 0};
	float qTimer = 0.f, pollTimer = 0.f;
	int noAnswer = 0, answered = 0;  // (the display warns when the WING doesn't answer)
	std::atomic<bool> pullAll{false}, syncNow{false}, wantReset{false};
	dsp::BooleanTrigger xDownT, xUpT;
	int sendRR = 0;
	float sendWait = 0.f;
	dsp::ClockDivider ctl;
	dsp::BooleanTrigger downT, upT, syncT;
	float syncFlash = 0.f;
	float routeTimer = 0.f;
	std::atomic<bool> routeWrong{false};  // the WING's channels don't take AUDIO from here: SYNC would fix it
	std::atomic<bool> fresh{true};  // a new module (not loaded from a patch): its AUDIO follows the WING's routing once
	std::string audioName, driverName;  // (UI thread)

	// ---- WING-EQ: the channel shown, its fields
	WingEq* eqSeen = nullptr;
	int eqKeySeen = -1;
	Sync eqs[NEQ];
	dsp::BooleanTrigger eqPrevT, eqNextT, eqFlatT;
	// ---- WING-DYN
	WingDyn* dynSeen = nullptr;
	int dynKeySeen = -1;
	Sync dyns[NDYN];
	dsp::BooleanTrigger dynPrevT, dynNextT;
	// ---- WING-IN
	WingIn* inSeen = nullptr;
	int inStartSeen = -1;
	dsp::BooleanTrigger inDownT, inUpT, inSyncT;
	float inSyncFlash = 0.f;
	int selKey = -1;         // the channel selected on the WING (SELECT), −1 unknown
	float selTimer = 0.f;
	bool selAsked = false;

	// ---- FX and tempo
	FxSlot fxs[NSLOT];
	std::atomic<int> fxModelReq[NSLOT];  // the UI picked a model (−1: none)
	int fxPageSeen = -1;
	WingFx* fxSeen = nullptr;
	dsp::BooleanTrigger pageDownT, pageUpT, tapT;
	dsp::SchmittTrigger clockIn;
	dsp::PulseGenerator clockOut;
	double now = 0.0, lastClock = -1.0, lastTap = -10.0;
	float clockPeriods[4] = {}, tapGaps[4] = {};
	int nPeriods = 0, nGaps = 0;
	std::atomic<float> rackBpm{0.f}, wingBpm{0.f};
	std::atomic<int> wingBpmSlot{-1};
	float clockPhase = 0.f, tempoTimer = 0.f, tapFlash = 0.f;

	WingLink() : port(this) {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configStrips(this, 0);
		configButton(DOWN_PARAM, "Start channel down");
		configButton(UP_PARAM, "Start channel up");
		configButton(SYNC_PARAM, "SYNC: names, colours, icons and USB routing to the WING, then read its faders back");
		configSwitch(TRACKS_PARAM, 0.f, 1.f, 0.f, "SD recording tracks", {"8", "16"});
		ctl.setDivision(32);
		noRandomize(this);
		fillMeta();
		midiOut.setChannel(-1);
		midiIn.setChannel(-1);
		for (int i = 0; i < NSLOT; i++)
			fxModelReq[i] = -1;
		std::lock_guard<std::mutex> lock(regMutex);
		registry.insert(this);
	}
	~WingLink() {
		{
			std::lock_guard<std::mutex> lock(regMutex);
			registry.erase(this);
		}
		port.setDriverId(-1);
	}
	void onRemove(const RemoveEvent& e) override {
		{
			std::lock_guard<std::mutex> lock(regMutex);
			registry.erase(this);
		}
		if (WingLinkX* x = xMod.load())
			x->host = nullptr;
		if (WingFx* f = fxMod.load())
			f->host = nullptr;
		if (WingEq* q = eqMod.load())
			q->host = nullptr;
		if (WingDyn* q = dynMod.load())
			q->host = nullptr;
		if (WingKnobs* q = knobsMod.load())
			q->host = nullptr;
		if (WingIn* q = inMod.load())
			q->host = nullptr;
	}

	void onReset() override {
		wingStart = 1;
		usbMode = 0;
		fxBus[0] = 1;
		fxBus[1] = 2;
		audioStart = 1;
		fresh = true;
		{
			std::lock_guard<std::mutex> lock(metaMutex);
			meta.clear();
			for (int s = 0; s < NT; s++) {
				st[s].tType = 0;
				st[s].tNum = 0;
			}
		}
		fillMeta();
		// (Initialize put the panel to its defaults: they are not sent, the WING's values are read again)
		wantReset = true;
	}
	/** An entry for every channel, bus … the WING has: the engine never has to make one (no allocation there). */
	void fillMeta() {
		std::lock_guard<std::mutex> lock(metaMutex);
		for (int ty = P::T_CH; ty < P::NUM_TGT; ty++)
			for (int n = 1; n <= P::TGT_COUNT[ty]; n++)
				meta[ty * 100 + n];
	}
	/** (metaMutex held) The entry of a key, nullptr if the WING has no such channel. */
	Meta* metaOf(int key) {
		auto it = meta.find(key);
		return it == meta.end() ? nullptr : &it->second;
	}

	// ---- helpers
	Module* owner(int s, int* j) {
		*j = s % NS;
		return s < NS ? (Module*) this : (Module*) xMod.load();
	}
	/** WING-LINK X's first input: its own START CH, or WING-LINK's + 8. */
	int xStartNow() {
		WingLinkX* x = xMod.load();
		int v = x ? x->xStart.load() : -1;
		return v > 0 ? v : wingStart + NS;
	}
	P::Tgt target(int s) {
		int ty = st[s].tType;
		if (ty > P::T_NONE)
			return P::tgt(ty, st[s].tNum);
		return P::inputTgt(s < NS ? wingStart + s : xStartNow() + (s - NS));
	}
	static int metaKey(const P::Tgt& t) { return t.type * 100 + t.n; }
	static P::Tgt keyTgt(int k) { return P::tgt(k / 100, k % 100); }
	std::string node(int s) { return P::tgtNode(target(s)); }
	bool active(int s) {
		int j;
		return target(s).ok() && owner(s, &j);
	}
	/** Whether the strip's field means something for what it controls (sends: channels and aux only …). */
	bool has(int s, int f) {
		P::Tgt t = target(s);
		if (f == FX1 || f == FX2)
			return t.audio();
		if (f == PAN)
			return t.pan();
		if (f == SOLO)
			return t.solo();
		return t.ok();
	}
	/** AUDIO s goes to USB usbStart() + s, and SYNC routes WING input audioStart + s to it (paging moves neither). */
	int usbStart() { return usbMode.load() > 0 ? usbMode.load() : audioStart.load(); }
	/** An audio device with at most 16 outputs (the DAW, when Rack is a plugin): AUDIO 1–16 are its outputs 1–16, the DAW
	 *  routes them to the WING (its USB channels as set here). */
	bool smallDevice() {
		int n = port.deviceTotalOutputs;
		return n > 0 && n <= 16;
	}
	int portOffset() { return smallDevice() ? 0 : clamp(usbStart() - 1, 0, 47); }
	int usbOf(int s) { return usbStart() + s; }
	int fxVisible(int slot) {  // its place on WING-FX (0..3), −1 if not shown
		WingFx* f = fxMod.load();
		if (!f)
			return -1;
		int j = slot - f->first;
		return (j >= 0 && j < NPAGE) ? j : -1;
	}
	/** The parameter on knob k of a slot (the one picked on WING-FX, else the model's default). */
	const F::Param* fxKnob(int slot, int k) {
		int m = fxs[slot].model;
		WingFx* f = fxMod.load();
		return f ? f->knob(m, k) : F::knobParam(F::MODELS[m], k, -1);
	}

	// ---- the MIDI log (right-click › Log MIDI to file): the engine notes, the UI writes the file
	std::atomic<float> logLeft{0.f};    // seconds still to log
	std::vector<std::string> logLines;
	std::mutex logMutex;
	std::string logPath;                // (UI thread: where it went)
	std::atomic<bool> logPending{false};  // lines wait for the UI
	/** Whether the log runs: its lines are only put together then (no strings in the engine otherwise). */
	bool logging() { return logLeft.load(std::memory_order_relaxed) > 0.f; }
	void logLine(const std::string& l) {
		if (!logging())
			return;
		std::unique_lock<std::mutex> lock(logMutex, std::try_to_lock);
		if (lock.owns_lock() && logLines.size() < 20000) {
			logLines.push_back(string::f("%9.3f  ", now) + l);
			logPending = true;
		}
	}
	static std::string describe(const std::vector<uint8_t>& b) {
		if (b.empty())
			return "(empty)";
		if (b[0] == 0xF0) {
			std::string hex, txt;
			for (size_t i = 0; i < b.size(); i++) {
				hex += string::f("%02X ", b[i]);
				if (i >= 6 && i + 1 < b.size())
					txt += (b[i] >= 0x20 && b[i] < 0x7f) ? (char) b[i] : '.';
			}
			return string::f("SYSEX cmd %02X  \"%s\"  [%s]", b.size() > 5 ? b[5] : 0, txt.c_str(), hex.c_str());
		}
		std::string hex;
		for (uint8_t x : b)
			hex += string::f("%02X ", x);
		if ((b[0] >> 4) == 0xB && b.size() >= 3)
			return string::f("CC  ch %d  cc %d  val %d  [%s]", (b[0] & 15) + 1, b[1], b[2], hex.c_str());
		return "[" + hex + "]";
	}
	void send(const std::vector<uint8_t>& bytes) {
		midi::Message m;
		m.bytes = bytes;
		midiOut.sendMessage(m);
		if (logging())
			logLine("OUT  " + describe(bytes));
	}
	void sendCc(int ch, int cc, int v) {
		midi::Message m;
		m.bytes = {(uint8_t) (0xB0 | ch), (uint8_t) cc, (uint8_t) clamp(v, 0, 127)};
		midiOut.sendMessage(m);
		if (logging())
			logLine("OUT  " + describe(m.bytes));
	}

	/** The routing on the WING (read with the names: in.conn.grp / in): its channels 1–16 shown take USB 1–16 (a
	 *  snapshot, an earlier SYNC)? Then AUDIO goes there, no SYNC needed. Else the display says SYNC. Changes nothing on
	 *  the WING. */
	void checkRouting() {
		if (port.deviceNumOutputs <= 0) {
			routeWrong = false;  // (no audio to the WING: nothing to route)
			return;
		}
		std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		// the strips whose channel takes their AUDIO jack (the channel = AUDIO's home; paged away: not these)
		int n = 0, fits = 0, k = -1, want = 0;
		bool same = true;
		for (int s = 0; s < NT; s++) {
			if (!active(s) || st[s].tType != 0)
				continue;
			P::Tgt t = target(s), home = P::inputTgt(audioStart + s);
			if (t.type != home.type || t.n != home.n)
				continue;
			want++;
			auto it = meta.find(metaKey(t));
			if (it == meta.end() || it->second.grp.empty() || (it->second.grp == "USB" && it->second.in <= 0))
				continue;  // (its source isn't known yet)
			const Meta& m = it->second;
			n++;
			if (m.grp == "USB" && m.in == usbOf(s))
				fits++;
			int d = m.grp == "USB" ? m.in - s : -1;
			if (d < 1 || (k >= 0 && d != k))
				same = false;
			k = d;
		}
		if (n == 0) {
			routeWrong = false;
			return;
		}
		if (fresh) {
			// a new WING-LINK: AUDIO goes where the WING (its snapshot) takes it from, no SYNC needed
			if (n < want) {
				routeWrong = false;  // (wait until every channel's source is known)
				return;
			}
			fresh = false;
			if (same && fits < n && k + n - 1 <= 48) {
				usbMode = k == audioStart ? 0 : k;
				if (logging())
					logLine(string::f("--   the WING's routing: AUDIO now to USB %d–", k));
				routeWrong = false;
				return;
			}
		}
		routeWrong = fits < n;
	}

	/** After the channels changed (start, connect, load): take the panels as they are, ask the WING. */
	void resetSync() {
		for (int s = 0; s < NT; s++) {
			int j;
			Module* m = owner(s, &j);
			static const int CVOF[NF] = {CV_FDR, CV_PAN, CV_FX1, CV_FX2, -1, -1};
			for (int f = 0; f < NF; f++) {
				Sync& y = st[s].f[f];
				y.local = m ? m->params[pid(j, f)].getValue() : NAN;
				y.wing = y.local;
				// (a field with a cable: its value goes to the (new) channel)
				if (m && CVOF[f] >= 0 && m->inputs[iid(j, CVOF[f])].isConnected())
					y.local = y.wing = NAN;
				y.dirty = false;
				y.hold = 0.f;
				y.sent = 0;
			}
		}
		for (int k = 0; k < NSLOT; k++)
			for (int f = 0; f < NFX; f++) {
				fxs[k].f[f].dirty = false;
				fxs[k].f[f].hold = 0.f;
				fxs[k].f[f].sent = 0;
			}
		fxPageSeen = -1;  // (the FX panel: taken as it is again)
		eqKeySeen = -1;   // (WING-EQ and WING-DYN: their knobs as they are, the channel read again)
		dynKeySeen = -1;
		queries.clear();
		if (qWaiting)
			qDrain = 0.3f;  // (a late answer must not land on the first new question)
		qWaiting = false;
		pullAll = true;
	}

	// ---- values from the WING
	void fromWing(int s, int f, float v) {
		int j;
		Module* m = owner(s, &j);
		if (!m)
			return;
		Sync& y = st[s].f[f];
		if (y.hold > 0.f)
			return;
		y.wing = v;
		if (stripKey(s, f, v) != y.sent)
			y.sent = 0;  // (the WING has another value now: ours goes out again when it changes)
		static const int CVOF[NF] = {CV_FDR, CV_PAN, CV_FX1, CV_FX2, -1, -1};
		if (CVOF[f] >= 0 && m->inputs[iid(j, CVOF[f])].isConnected())
			return;
		m->params[pid(j, f)].setValue(v);
		y.local = v;
		y.dirty = false;
	}
	void fromWingFx(int slot, int f, float n) {
		FxSlot& x = fxs[slot];
		Sync& y = x.f[f];
		if (y.hold > 0.f)
			return;
		y.wing = n;
		y.sent = 0;
		x.val[f] = n;
		x.known[f] = true;
		int j = fxVisible(slot);
		WingFx* fm = fxMod.load();
		if (j < 0 || !fm)
			return;
		if (f != FON && fm->inputs[fxCv(j, f - FMIX)].isConnected())
			return;
		fm->params[fxPid(j, f)].setValue(n);
		y.local = n;
		y.dirty = false;
	}
	void setFxModel(int slot, int model) {
		FxSlot& x = fxs[slot];
		x.model = model;
		x.modelKnown = true;
		for (int f = FP1; f < NFX; f++) {
			x.known[f] = false;
			x.f[f].wing = NAN;
			x.f[f].sent = 0;
		}
		fxShow = true;  // (refresh the panel; the caller asks for this slot's values)
	}
	/** Where the WING keeps what it shows for this channel: its source while linked to it, else the channel. */
	static bool viaSource(const P::Tgt& t, const Meta& m) {
		return (t.type == P::T_CH || t.type == P::T_AUX) && m.link == 1 && !m.srcBad && m.in > 0 && !m.grp.empty() &&
		       m.grp != "OFF";
	}
	static std::string metaNode(const P::Tgt& t, const Meta& m) {
		if (viaSource(t, m))
			return string::f("io.in.%s.%d", m.grp.c_str(), m.in);
		return P::tgtNode(t);
	}
	void askMeta(int s) {
		int k = metaKey(target(s));
		P::Tgt t = keyTgt(k);
		if (t.type == P::T_CH || t.type == P::T_AUX) {
			queries.push_back({3, k, M_LINK});
			queries.push_back({3, k, M_GRP});
			queries.push_back({3, k, M_IN});
		}
		queries.push_back({3, k, M_NAME});
		queries.push_back({3, k, M_COL});
		queries.push_back({3, k, M_ICON});
	}
	void askMetaKey(int k) {
		P::Tgt t = keyTgt(k);
		if (t.type == P::T_CH || t.type == P::T_AUX) {
			queries.push_back({3, k, M_LINK});
			queries.push_back({3, k, M_GRP});
			queries.push_back({3, k, M_IN});
		}
		queries.push_back({3, k, M_NAME});
		queries.push_back({3, k, M_COL});
	}
	void askEq() {
		if (eqKeySeen < 0)
			return;
		queries.push_back({4, eqKeySeen, EQ_MDL});
		for (int f = 0; f < NEQ; f++)
			queries.push_back({4, eqKeySeen, f});
	}
	/** CH 1–40, then AUX 1–8: the next (d = +1) or the one before (−1). */
	static int eqStepKey(int key, int d) {
		P::Tgt t = keyTgt(key);
		int i = t.type == P::T_AUX ? 40 + t.n - 1 : (t.type == P::T_CH ? t.n - 1 : 0);
		i = ((i + d) % 48 + 48) % 48;
		return i < 40 ? metaKey(P::tgt(P::T_CH, i + 1)) : metaKey(P::tgt(P::T_AUX, i - 40 + 1));
	}
	void fromWingEq(int f, float v) {
		Sync& y = eqs[f];
		if (y.hold > 0.f)
			return;
		y.wing = v;
		y.sent = 0;
		WingEq* em = eqMod.load();
		if (!em)
			return;
		int in = eqInput(f);
		if (in >= 0 && em->inputs[in].isConnected())
			return;
		em->params[f].setValue(v);
		y.local = v;
		y.dirty = false;
	}
	std::string eqText(int f, float p) {
		if (f == EQ_LSHV || f == EQ_HSHV)
			return p > 0.5f ? "SHV" : "PEQ";
		if (eqSwitch(f))
			return p > 0.5f ? "1" : "0";
		if (eqGain(f))
			return P::fixed(clamp(p, -15.f, 15.f), 1);
		float v = eqValue(f, p);
		return P::fixed(v, v < 100.f ? 2 : 0);
	}

	// ---- WING-EQ: its panel <-> the channel's EQ
	void controlEq(float dt, bool connected) {
		WingEq* em = eqMod.load();
		if (em != eqSeen) {
			eqSeen = em;
			eqKeySeen = -1;
		}
		if (!em)
			return;
		int cur = eqKeySeen >= 0 ? eqKeySeen : em->fixedKey.load();
		if (eqPrevT.process(em->params[WingEq::PREV_PARAM].getValue() > 0.f)) {
			em->fixedKey = eqStepKey(cur, -1);
			em->mode = WingEq::M_FIXED;
		}
		if (eqNextT.process(em->params[WingEq::NEXT_PARAM].getValue() > 0.f)) {
			em->fixedKey = eqStepKey(cur, +1);
			em->mode = WingEq::M_FIXED;
		}
		int key;
		if (em->mode == WingEq::M_STRIP)
			key = metaKey(target(clamp(em->strip.load(), 0, NT - 1)));
		else if (em->mode == WingEq::M_SELECT)
			key = selKey >= 0 ? selKey : cur;
		else
			key = em->fixedKey;
		P::Tgt t = keyTgt(key);
		if (!t.ok() || (t.type != P::T_CH && t.type != P::T_AUX))
			key = -1;
		if (key != eqKeySeen) {
			// another channel: keep the knobs until the WING answers, ask it
			eqKeySeen = key;
			em->shownKey = key;
			em->model = -1;
			for (int f = 0; f < NEQ; f++) {
				eqs[f] = Sync();
				int in = eqInput(f);
				// (a field with a cable: its value goes to the new channel)
				eqs[f].local = (in >= 0 && em->inputs[in].isConnected()) ? NAN : em->params[f].getValue();
			}
			if (key >= 0 && connected && sysexOk) {
				askEq();
				askMetaKey(key);
			}
		}
		if (eqFlatT.process(em->params[WingEq::FLAT_PARAM].getValue() > 0.f))
			for (int b = 0; b < NBAND; b++)
				em->params[eqG(b)].setValue(0.f);
		// CV: GAIN ±5 V = ±15 dB, FREQ / Q / the filters 0–10 V, EQ ON a gate
		for (int f = 0; f < NEQ; f++) {
			int in = eqInput(f);
			if (in < 0 || !em->inputs[in].isConnected())
				continue;
			float v = em->inputs[in].getVoltage();
			if (eqGain(f))
				em->params[f].setValue(clamp(v * 3.f, -15.f, 15.f));
			else if (f == EQ_ON)
				em->params[f].setValue(v >= 1.f ? 1.f : 0.f);
			else
				em->params[f].setValue(clamp(v / 10.f, 0.f, 1.f));
		}
		for (int f = 0; f < NEQ; f++) {
			Sync& y = eqs[f];
			float v = em->params[f].getValue();
			if (v != y.local) {
				y.local = v;
				if (key >= 0 && connected && !(std::fabs(v - y.wing) < 1e-4f)) {
					y.dirty = true;
					y.hold = 0.4f;
				}
			}
			y.hold = std::max(0.f, y.hold - dt);
		}
		em->lights[WingEq::ON_LIGHT].setBrightness(em->params[EQ_ON].getValue() > 0.5f ? 1.f : 0.f);
		em->lights[WingEq::LSHV_LIGHT].setBrightness(em->params[EQ_LSHV].getValue() > 0.5f ? 1.f : 0.f);
		em->lights[WingEq::HSHV_LIGHT].setBrightness(em->params[EQ_HSHV].getValue() > 0.5f ? 1.f : 0.f);
		em->lights[WingEq::LC_LIGHT].setBrightness(em->params[EQ_LC].getValue() > 0.5f ? 1.f : 0.f);
		em->lights[WingEq::HC_LIGHT].setBrightness(em->params[EQ_HC].getValue() > 0.5f ? 1.f : 0.f);
		// SELECT on the WING: asked often
		if (em->mode == WingEq::M_SELECT && connected && sysexOk) {
			selTimer -= dt;
			if (selTimer <= 0.f && !selAsked) {
				selTimer = 0.3f;
				selAsked = true;
				queries.push_front({5, 0, 0});
			}
		}
	}

	// ---- WING-DYN: its panel <-> the channel's gate and compressor
	void askDyn() {
		if (dynKeySeen < 0)
			return;
		queries.push_back({6, dynKeySeen, D_GMDL});
		queries.push_back({6, dynKeySeen, D_CMDL});
		for (int f = 0; f < NDYN; f++)
			queries.push_back({6, dynKeySeen, f});
	}
	void fromWingDyn(int f, float v) {
		Sync& y = dyns[f];
		if (y.hold > 0.f)
			return;
		y.wing = v;
		y.sent = 0;
		WingDyn* dm = dynMod.load();
		if (!dm)
			return;
		int in = dynInput(f);
		if (in >= 0 && dm->inputs[in].isConnected())
			return;
		dm->params[f].setValue(v);
		y.local = v;
		y.dirty = false;
	}
	static std::string dynText(int f, float p) {
		const DynSpec& d = DYN[f];
		if (f == D_CDET)
			return p > 0.5f ? "RMS" : "PEAK";
		if (f == D_CENV)
			return p > 0.5f ? "LOG" : "LIN";
		if (d.sw)
			return p > 0.5f ? "1" : "0";
		if (f == D_CRATIO)
			return DYN_RATIOS[clamp((int) std::round(p), 0, NRATIO - 1)];
		if (f == D_CKNEE)
			return string::f("%d", clamp((int) std::round(p), 0, 5));
		if (d.logr)
			return P::fixed(dynRelMs(p), 0);
		return P::fixed(clamp(p, d.lo, d.hi), 1);
	}
	/** The WING's text → the knob (NAN: not understood). */
	static float dynParse(int f, const std::string& t) {
		const DynSpec& d = DYN[f];
		if (f == D_CDET)
			return t == "RMS" ? 1.f : (t == "PEAK" ? 0.f : NAN);
		if (f == D_CENV)
			return t == "LOG" ? 1.f : (t == "LIN" ? 0.f : NAN);
		float v;
		if (!P::number(t, &v))
			return NAN;
		if (d.sw)
			return v > 0.5f ? 1.f : 0.f;
		if (f == D_CRATIO) {
			int best = 0;
			float bd = 1e9f;
			for (int i = 0; i < NRATIO; i++) {
				double rd = 0.0;
				P::parseNumber(DYN_RATIOS[i], &rd);
				float r = (float) rd, dd = std::fabs(r - v);
				if (dd < bd)
					bd = dd, best = i;
			}
			return (float) best;
		}
		if (d.logr)
			return v >= 3.f && v <= 4400.f ? dynRelParam(v) : NAN;
		if (v < d.lo - 0.5f || v > d.hi + 0.5f)
			return NAN;
		return clamp(v, d.lo, d.hi);
	}
	void controlDyn(float dt, bool connected) {
		WingDyn* dm = dynMod.load();
		if (dm != dynSeen) {
			dynSeen = dm;
			dynKeySeen = -1;
		}
		if (!dm)
			return;
		int cur = dynKeySeen >= 0 ? dynKeySeen : dm->fixedKey.load();
		if (dynPrevT.process(dm->params[WingDyn::PREV_PARAM].getValue() > 0.f)) {
			dm->fixedKey = eqStepKey(cur, -1);
			dm->mode = WingEq::M_FIXED;
		}
		if (dynNextT.process(dm->params[WingDyn::NEXT_PARAM].getValue() > 0.f)) {
			dm->fixedKey = eqStepKey(cur, +1);
			dm->mode = WingEq::M_FIXED;
		}
		int key;
		if (dm->mode == WingEq::M_STRIP)
			key = metaKey(target(clamp(dm->strip.load(), 0, NT - 1)));
		else if (dm->mode == WingEq::M_SELECT)
			key = selKey >= 0 ? selKey : cur;
		else
			key = dm->fixedKey;
		P::Tgt t = keyTgt(key);
		if (!t.ok() || (t.type != P::T_CH && t.type != P::T_AUX))
			key = -1;
		if (key != dynKeySeen) {
			dynKeySeen = key;
			dm->shownKey = key;
			dm->gateModel = -1;
			dm->compModel = -1;
			for (int f = 0; f < NDYN; f++) {
				dyns[f] = Sync();
				int in = dynInput(f);
				dyns[f].local = (in >= 0 && dm->inputs[in].isConnected()) ? NAN : dm->params[f].getValue();
			}
			if (key >= 0 && connected && sysexOk) {
				askDyn();
				askMetaKey(key);
			}
		}
		// CV: 0–10 V across a knob's range; ON a gate
		for (int f = 0; f < NDYN; f++) {
			int in = dynInput(f);
			if (in < 0 || !dm->inputs[in].isConnected())
				continue;
			const DynSpec& d = DYN[f];
			float v = dm->inputs[in].getVoltage();
			if (d.sw)
				dm->params[f].setValue(v >= 1.f ? 1.f : 0.f);
			else {
				float x = d.lo + clamp(v / 10.f, 0.f, 1.f) * (d.hi - d.lo);
				if (f == D_CRATIO || f == D_CKNEE)
					x = std::round(x);
				dm->params[f].setValue(x);
			}
		}
		for (int f = 0; f < NDYN; f++) {
			Sync& y = dyns[f];
			float v = dm->params[f].getValue();
			if (v != y.local) {
				y.local = v;
				if (key >= 0 && connected && !(std::fabs(v - y.wing) < 1e-4f)) {
					y.dirty = true;
					y.hold = 0.4f;
				}
			}
			y.hold = std::max(0.f, y.hold - dt);
		}
		if (dm->mode == WingEq::M_SELECT && connected && sysexOk) {
			selTimer -= dt;
			if (selTimer <= 0.f && !selAsked) {
				selTimer = 0.3f;
				selAsked = true;
				queries.push_front({5, 0, 0});
			}
		}
	}

	// ---- WING-IN: the WING's USB outputs -> Rack
	static const char* inGroupNode(int g) {
		switch (g) {
			case IG_LCL: return "LCL";
			case IG_AUX: return "AUX";
			case IG_A: return "A";
			case IG_B: return "B";
			case IG_C: return "C";
			case IG_SC: return "SC";
			case IG_USB: return "USB";
			default: return "OFF";
		}
	}
	static const char* inUserGrp(int g) {
		switch (g) {
			case IG_CH: return "CH";
			case IG_AUXCH: return "AUX";
			case IG_BUS: return "BUS";
			case IG_MAIN: return "MAIN";
			case IG_MTX: return "MTX";
			default: return "OFF";
		}
	}
	/** Asks the WING what its USB outputs (those of WING-IN) take; part: 0 grp, 1 in, 2 USR grp, 3 USR in, 4 tap, 5 lr */
	void askIn() {
		WingIn* im = inMod.load();
		if (!im)
			return;
		for (int i = 0; i < NIN; i++)
			for (int part = 0; part < 6; part++)
				queries.push_back({7, i, part});
	}
	/** SYNC of WING-IN: each USB output to its source (a channel, bus … through the user signal of its number). */
	void syncIn(WingIn* im) {
		int st = clamp(im->start.load(), 1, 41);
		std::lock_guard<std::mutex> lock(im->srcMutex);
		for (int i = 0; i < NIN; i++) {
			const InSource& s = im->src[i];
			int usb = st + i;
			if (s.grp == IG_OFF)
				outbox.push_back(string::f("io.out.USB.%d.grp=OFF", usb));
			else if (igInternal(s.grp)) {
				static const char* LR[3] = {"L+R", "L", "R"};
				outbox.push_back(string::f("io.in.USR.%d.user.grp=%s,in=%d,tap=%s,lr=%s", i + 1, inUserGrp(s.grp), s.n,
				                           s.tap ? "POST" : "PRE", LR[clamp(s.lr, 0, 2)]));
				outbox.push_back(string::f("io.out.USB.%d.grp=USR,in=%d", usb, i + 1));
			}
			else
				outbox.push_back(string::f("io.out.USB.%d.grp=%s,in=%d", usb, inGroupNode(s.grp), s.n));
			im->known[i] = true;
		}
		im->pending = false;
	}
	void controlIn(float dt, bool connected) {
		WingIn* im = inMod.load();
		port.wantInputs = im != nullptr;
		if (im != inSeen) {
			inSeen = im;
			inStartSeen = -1;
			port.engineOutputBuffer.clear();  // (what an earlier WING-IN left there is old)
		}
		inSyncFlash = std::max(0.f, inSyncFlash - dt);
		if (!im)
			return;
		int st = clamp(im->start.load(), 1, 41);
		if (inDownT.process(im->params[WingIn::DOWN_PARAM].getValue() > 0.f) && st > 1)
			im->start = st = std::max(1, ((st - 2) / 8) * 8 + 1);
		if (inUpT.process(im->params[WingIn::UP_PARAM].getValue() > 0.f) && st < 41)
			im->start = st = std::min(41, ((st - 1) / 8 + 1) * 8 + 1);
		port.wantInOffset = smallDevice() ? 0 : st - 1;
		if (st != inStartSeen) {
			inStartSeen = st;
			for (int i = 0; i < NIN; i++)
				im->known[i] = false;
			if (connected && sysexOk)
				askIn();
		}
		bool doSync = inSyncT.process(im->params[WingIn::SYNC_PARAM].getValue() > 0.f) || im->syncReq.exchange(false);
		if (doSync && connected && sysexOk) {
			syncIn(im);
			inSyncFlash = 0.3f;
		}
		im->lights[WingIn::SYNC_LIGHT].setBrightness(inSyncFlash > 0.f ? 1.f : 0.f);
	}
	/** (a reply about WING-IN's routing) */
	void fromWingIn(int i, int part, const std::string& t0) {
		WingIn* im = inMod.load();
		if (!im || i < 0 || i >= NIN || im->known[i])
			return;
		std::string t;
		for (char ch : t0)
			if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r')
				t += ch;
		std::unique_lock<std::mutex> lock(im->srcMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		InSource& s = im->src[i];
		float v;
		switch (part) {
			case 0: {
				int g = -1;
				for (int k = IG_OFF; k <= IG_USB; k++)
					if (t == inGroupNode(k))
						g = k;
				if (t == "USR")
					g = 100;  // (internal: the user signal tells)
				if (g >= 0 && g != 100)
					s.grp = g;
				else if (g == 100 && !igInternal(s.grp))
					s.grp = IG_CH;
				break;
			}
			case 1:
				if (!igInternal(s.grp) && P::number(t, &v))
					s.n = clamp((int) v, 1, std::max(1, igMax(s.grp)));
				if (!igInternal(s.grp))
					im->known[i] = true;
				break;
			case 2:
				if (igInternal(s.grp)) {
					if (t == "CH") s.grp = IG_CH;
					else if (t == "AUX") s.grp = IG_AUXCH;
					else if (t == "BUS") s.grp = IG_BUS;
					else if (t == "MAIN") s.grp = IG_MAIN;
					else if (t == "MTX") s.grp = IG_MTX;
				}
				break;
			case 3:
				if (igInternal(s.grp) && P::number(t, &v))
					s.n = clamp((int) v, 1, std::max(1, igMax(s.grp)));
				break;
			case 4:
				if (igInternal(s.grp))
					s.tap = t == "PRE" ? 0 : 1;
				break;
			case 5:
				if (igInternal(s.grp))
					s.lr = t == "L" ? 1 : (t == "R" ? 2 : 0);
				im->known[i] = true;
				break;
		}
	}

	void askFx(int slot, bool model) {
		if (model)
			queries.push_back({1, slot, FMODEL});
		queries.push_back({1, slot, FMIX});
		for (int k = 0; k < 4; k++)
			if (fxKnob(slot, k))
				queries.push_back({1, slot, FP1 + k});
		if (F::MODELS[fxs[slot].model].timeParam >= 0)
			queries.push_back({1, slot, FTIME});
	}

	void onReply(int cmd, const std::string& text) {
		if (!qWaiting)
			return;
		qWaiting = false;
		answered++;
		noAnswer = 0;
		if (cmd != P::GET_REPLY) {
			qDrain = 0.1f;  // (if that complaint was late, the real answer comes now: let it pass)
			if (qNow.kind == 3 && (qNow.f == M_LINK || qNow.src)) {
				std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
				if (Meta* m = lock.owns_lock() ? metaOf(qNow.s) : nullptr) {
					if (qNow.f == M_LINK)
						m->link = 0;
					if (qNow.src)
						m->srcBad = true;  // the WING has no name / colour / icon at that source: use the channel's
				}
			}
			return;
		}
		if (qNow.kind == 3) {
			// name, colour or icon of a WING channel
			std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
			Meta* mp = lock.owns_lock() ? metaOf(qNow.s) : nullptr;
			if (!mp)
				return;
			Meta& m = *mp;
			std::string t = text;
			while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
				t.pop_back();
			if (qNow.f >= M_LINK) {
				// how the channel is linked to its source
				std::string u;
				for (char ch : t)
					if (ch != '"' && ch != ' ')
						u += ch;
				float v;
				if (qNow.f == M_GRP) {
					if (!u.empty() && u != m.grp) {
						m.grp = u;
						m.srcBad = false;
					}
				}
				else if (P::number(u, &v)) {
					if (qNow.f == M_LINK)
						m.link = v > 0.5f ? 1 : 0;
					else if ((int) v != m.in) {
						m.in = (int) v;
						m.srcBad = false;
					}
				}
				return;
			}
			if (qNow.src != viaSource(keyTgt(qNow.s), m))
				return;  // (the link changed meanwhile)
			if (m.hold > 0.f || (m.dirty & qNow.f))
				return;  // just changed here: ours stands
			if (qNow.f == M_NAME) {
				if (t.size() >= 2 && t[0] == '"' && t.back() == '"')
					t = t.substr(1, t.size() - 2);
				m.name = t.substr(0, 16);
			}
			else {
				float v;
				if (!P::number(t, &v))
					return;
				if (qNow.f == M_COL && v >= 0.f && v <= 18.f)
					m.colour = (int) v;
				else if (qNow.f == M_ICON && v >= 0.f && v <= 999.f)
					m.icon = (int) v;
				else
					return;
			}
			m.known = true;
			return;
		}
		if (qNow.kind == 7) {
			fromWingIn(qNow.s, qNow.f, text);
			return;
		}
		if (qNow.kind == 6) {
			if (qNow.s != dynKeySeen)
				return;
			std::string t;
			for (char ch : text)
				if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r')
					t += ch;
			if (qNow.f == D_GMDL || qNow.f == D_CMDL) {
				if (WingDyn* dm = dynMod.load()) {
					bool g = qNow.f == D_GMDL;
					(g ? dm->gateMdl : dm->compMdl) = packName(t);
					(g ? dm->gateModel : dm->compModel) = t == (g ? "GATE" : "COMP") ? 1 : 0;
				}
				return;
			}
			if (qNow.f >= 0 && qNow.f < NDYN) {
				float v = dynParse(qNow.f, t);
				if (!std::isnan(v))
					fromWingDyn(qNow.f, v);
			}
			return;
		}
		if (qNow.kind == 5) {
			// the channel selected on the WING: 1–40 CH, 41–48 AUX (buses … have no channel EQ)
			float v;
			if (P::number(text, &v)) {
				int n = (int) std::round(v);
				if (n >= 1 && n <= 40)
					selKey = metaKey(P::tgt(P::T_CH, n));
				else if (n >= 41 && n <= 48)
					selKey = metaKey(P::tgt(P::T_AUX, n - 40));
			}
			return;
		}
		if (qNow.kind == 4) {
			if (qNow.s != eqKeySeen)
				return;  // (another channel meanwhile)
			std::string t;
			for (char ch : text)
				if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r')
					t += ch;
			int f = qNow.f;
			if (f == EQ_MDL) {
				if (WingEq* em = eqMod.load())
					for (int i = 0; i < 7; i++)
						if (t == EQ_MODELS[i])
							em->model = i;
				return;
			}
			if (f == EQ_LSHV || f == EQ_HSHV) {
				if (t == "SHV" || t == "PEQ")
					fromWingEq(f, t == "SHV" ? 1.f : 0.f);
				return;
			}
			float v;
			if (!P::number(t, &v))
				return;
			if (eqSwitch(f))
				fromWingEq(f, v > 0.5f ? 1.f : 0.f);
			else if (eqGain(f)) {
				if (v >= -15.5f && v <= 15.5f)
					fromWingEq(f, clamp(v, -15.f, 15.f));
			}
			else {
				float lo, hi;
				if (eqLogRange(f, &lo, &hi) && v >= lo * 0.9f && v <= hi * 1.1f)
					fromWingEq(f, eqParam(f, v));
			}
			return;
		}
		if (qNow.kind == 2) {
			// the SD recorder
			Sd& c = sd[qNow.s];
			std::string t;
			for (char ch : text)
				if (ch != '"' && ch != ' ' && ch != '\n')
					t += ch;
			float v;
			if (qNow.f == 0) {
				static const char* const ST[] = {"STOP", "PPAUSE", "PLAY", "REC"};
				for (int i = 0; i < 4; i++)
					if (t == ST[i])
						c.state = i;
			}
			else if (qNow.f == 1 && P::number(t, &v))
				c.etime = v;
			else if (qNow.f == 2 && P::number(t, &v))
				c.sdfree = v;
			else if (qNow.f == 3) {
				static const char* const CS[] = {"NONE", "READY", "PROTECT", "ERASE", "ERROR"};
				for (int i = 0; i < 5; i++)
					if (t == CS[i])
						c.card = i;
			}
			return;
		}
		if (qNow.kind == 1) {
			int slot = qNow.s, f = qNow.f;
			if (f == FMODEL) {
				int m = F::modelIndex(text);
				if (m != fxs[slot].model || !fxs[slot].modelKnown) {
					setFxModel(slot, m);
					askFx(slot, false);
				}
				return;
			}
			float v;
			if (f == FMIX) {
				if (P::number(text, &v))
					fromWingFx(slot, FMIX, clamp(v / 100.f, 0.f, 1.f));
				return;
			}
			if (f == FTIME) {
				const F::Model& M = F::MODELS[fxs[slot].model];
				if (M.timeParam >= 0 && F::parseValue(M.all[M.timeParam], text, &v))
					fxs[slot].timeMs = v;
				return;
			}
			const F::Param* p = fxKnob(slot, f - FP1);
			if (p && F::parseValue(*p, text, &v)) {
				fromWingFx(slot, f, F::toNorm(*p, v));
				if (!std::strcmp(p->key, "time"))
					fxs[slot].timeMs = v;
			}
			return;
		}
		float v;
		if (!P::number(text, &v))
			return;
		int f = qNow.f;
		// only what fits the question (an answer that slipped would move the wrong fader)
		if (f == FADER || f == FX1 || f == FX2) {
			if (v < -144.5f || v > 10.5f)
				return;
			v = P::dbToPos(v);
		}
		else if (f == PAN) {
			if (v < -100.5f || v > 100.5f)
				return;
			v = clamp(v / 100.f, -1.f, 1.f);
		}
		else {
			if (v != 0.f && v != 1.f)
				return;
		}
		fromWing(qNow.s, f, v);
	}

	void onMidi(const midi::Message& msg) {
		if (msg.bytes.empty())
			return;
		if (logging())
			logLine("IN   " + describe(msg.bytes) + (qWaiting ? string::f("   (waiting: kind %d s %d f %d)", qNow.kind, qNow.s, qNow.f) : ""));
		if (msg.bytes[0] == 0xF0) {
			std::string text;
			int cmd = P::parse(msg.bytes, &text);
			if (cmd < 0)
				return;
			midiSeen = true;
			sysexOk = true;
			if (cmd == P::IDENT_REPLY) {
				std::lock_guard<std::mutex> lock(identMutex);
				wingIdent = text;
				return;
			}
			if (cmd == P::ERROR_REPLY && setGap > 0.f && !(qWaiting && !setSinceAsk)) {
				// a complaint about the last command (not an answer to a question: no command went out since that
				// was asked, or none is waiting)
				{
					std::unique_lock<std::mutex> lock(errMutex, std::try_to_lock);
					if (lock.owns_lock()) {
						lastError = text + " (" + lastSet + ")";
						errorAge = 0.f;
					}
				}
				if (lastSetMeta >= 0 && lastSetPart == M_NAME && nameQuote == 0) {
					// names: try them "quoted"
					nameQuote = 1;
					std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
					if (Meta* m = lock.owns_lock() ? metaOf(lastSetMeta) : nullptr)
						m->dirty |= M_NAME;
				}
				return;
			}
			if (cmd == P::GET_REPLY || cmd == P::ERROR_REPLY)
				onReply(cmd, text);
			return;
		}
		int stat = msg.getStatus();
		if (stat == 0xB || stat == 0x9 || stat == 0x8) {
			if (WingKnobs* kn = knobsMod.load()) {
				bool note = stat != 0xB;
				int v = stat == 0x8 ? 0 : msg.getValue();
				if (kn->feed(msg.getChannel(), msg.getNote(), v, note)) {
					midiSeen = true;
					return;
				}
			}
		}
		if (msg.getStatus() != 0xB)
			return;
		midiSeen = true;
		int ch = msg.getChannel(), cc = msg.getNote(), val = msg.getValue();
		// FX (MIDI channels 9–16)
		int slot, param;
		int kind = P::fxCc(ch, cc, &slot, &param);
		if (kind != P::FXCC_NONE) {
			slot -= 1;
			// (questions only when they can be asked; none twice)
			auto ask = [&](int f, bool front) {
				if (!sysexOk)
					return;
				for (const Query& q : queries)
					if (q.kind == 1 && q.s == slot && q.f == f)
						return;
				front ? queries.push_front({1, slot, f}) : queries.push_back({1, slot, f});
			};
			if (kind == P::FXCC_ON)
				fromWingFx(slot, FON, val >= 64 ? 1.f : 0.f);
			else if (kind == P::FXCC_MODEL)
				ask(FMODEL, true);
			else if (kind == P::FXCC_MIX) {
				if (fxs[slot].f[FMIX].hold <= 0.f)
					ask(FMIX, true);
			}
			else {
				const F::Model& M = F::MODELS[fxs[slot].model];
				if (param >= 1 && param <= M.nKeys) {
					const char* key = M.keys[param - 1];
					for (int k = 0; k < 4; k++) {
						const F::Param* p = fxKnob(slot, k);
						if (p && !std::strcmp(key, p->key) && fxs[slot].f[FP1 + k].hold <= 0.f)
							ask(FP1 + k, true);
					}
					if (!std::strcmp(key, "time"))
						ask(FTIME, false);
				}
			}
			return;
		}
		P::Tgt t;
		int what;
		if (!P::ccTgt(ch, cc, &t, &what))
			return;
		for (int s = 0; s < NT; s++) {
			if (!active(s) || !(target(s) == t))
				continue;
			if (what == 0) {
				float now = st[s].f[FADER].wing;
				if (!std::isnan(now) && (int) std::round(now * 127.f) == val)
					continue;
				fromWing(s, FADER, val / 127.f);
			}
			else if (what == 1)
				fromWing(s, MUTE, val >= 64 ? 1.f : 0.f);
			else {
				float now = st[s].f[PAN].wing;
				if (!std::isnan(now) && (int) std::round((now + 1.f) * 63.5f) == val)
					continue;
				fromWing(s, PAN, val == 64 ? 0.f : clamp((val - 64) / 63.f, -1.f, 1.f));
			}
		}
	}

	// ---- values to the WING
	/** A strip's value as the WING gets it (0.1 dB, a pan step, a CC step), as a key (never 0): the same isn't sent
	 *  again. Sysex and CC keys never meet. */
	uint32_t stripKey(int s, int f, float v) {
		bool sx = sysexOk;
		int q;
		switch (f) {
			case FADER:
			case FX1:
			case FX2: {
				float db = P::posToDb(v);
				q = (sx || f != FADER) ? (db <= -89.9f ? -1440 : (int) std::round(db * 10.f))
				                       : (int) std::round(v * 127.f);
				break;
			}
			case PAN: q = sx ? (int) std::round(v * 100.f) : (int) std::round((v + 1.f) * 63.5f); break;
			default: q = v > 0.5f ? 1 : 0;
		}
		return (uint32_t) (q + 4000) * 2u + (sx ? 1u : 0u) + 2u;
	}
	/** Sends a strip's value; false when the WING has that (rounded) value from us already. */
	bool sendField(int s, int f, float v) {
		Sync& y = st[s].f[f];
		uint32_t key = stripKey(s, f, v);
		if (key == y.sent)
			return false;
		y.sent = key;
		P::Tgt t = target(s);
		std::string n = P::tgtNode(t) + ".";
		bool sx = sysexOk, dca = t.type == P::T_DCA;
		int cc = P::tgtCc(t);
		switch (f) {
			case FADER:
				if (sx)
					sendSet(n + "fdr=" + P::dbText(P::posToDb(v)));
				else
					sendCc(dca ? 3 : 0, cc, (int) std::round(v * 127.f));
				break;
			case PAN:
				if (sx)
					sendSet(n + string::f("pan=%d", (int) std::round(v * 100.f)));
				else
					sendCc(2, cc, (int) std::round((v + 1.f) * 63.5f));
				break;
			case MUTE:
				if (sx)
					sendSet(n + (v > 0.5f ? "mute=1" : "mute=0"));
				else
					sendCc(dca ? 4 : 1, cc, v > 0.5f ? 127 : 0);
				break;
			case SOLO: sendSet(n + (v > 0.5f ? "$solo=1" : "$solo=0")); break;
			case FX1:
			case FX2:
				sendSet(n + string::f("send.%d.on=1,lvl=", fxBus[f == FX2].load()) + P::dbText(P::posToDb(v)));
				break;
		}
		return true;
	}
	/** Sends an FX value; false when nothing went out (the same rounded value as last time, or no such knob). */
	bool sendFx(int slot, int f, float n) {
		Sync& y = fxs[slot].f[f];
		if (f == FON) {
			uint32_t key = n > 0.5f ? 2u : 1u;
			if (key == y.sent)
				return false;
			y.sent = key;
			int ch, cc;
			P::fxOnCc(slot + 1, &ch, &cc);
			sendCc(ch, cc, n > 0.5f ? 127 : 0);
			return true;
		}
		if (f == FMIX) {
			int mix = (int) std::round(n * 100.f);
			uint32_t key = (uint32_t) (mix + 1000);
			if (key == y.sent)
				return false;
			y.sent = key;
			sendSet(P::fxNode(slot + 1) + string::f(".fxmix=%d", mix));
			return true;
		}
		const F::Param* p = fxKnob(slot, f - FP1);
		if (!p)
			return false;
		float v = F::toValue(*p, n);
		std::string what = std::string(p->key) + "=" + F::wingText(*p, v);
		uint32_t key = textKey(what);
		if (key == y.sent)
			return false;
		y.sent = key;
		sendSet(P::fxNode(slot + 1) + "." + what);
		if (!std::strcmp(p->key, "time"))
			fxs[slot].timeMs = v;
		return true;
	}
	std::string queryOf(int f) {
		switch (f) {
			case FADER: return "fdr";
			case PAN: return "pan";
			case MUTE: return "mute";
			case SOLO: return "$solo";
			case FX1: return string::f("send.%d.lvl", fxBus[0].load());
			default: return string::f("send.%d.lvl", fxBus[1].load());
		}
	}
	/** One part (name, colour or icon) of a WING channel's meta. */
	void sendMetaPart(int key, int part, const Meta& m) {
		P::Tgt t = keyTgt(key);
		bool src = viaSource(t, m);
		std::string what;
		if (part == M_NAME) {
			std::string n = P::nameText(m.name);
			if (nameQuote && n[0] != '"')
				n = "\"" + n + "\"";
			what = "name=" + n;
		}
		else if (part == M_COL)
			what = string::f("col=%d", m.colour);
		else if (t.type != P::T_DCA)
			what = string::f("icon=%d", m.icon);
		if (what.empty())
			return;
		sendSet(metaNode(t, m) + "." + what);
		if (src && logging())
			logLine(string::f("--   %s is linked to its source %s: name / colour / icon go there",
			                  P::tgtLabel(t).c_str(), metaNode(t, m).c_str()));
		lastSetMeta = key;
		lastSetPart = part;
	}
	/** A command: then a pause, in which a complaint of the WING belongs to it (not to a question). */
	void sendSet(const std::string& path) {
		send(P::setPath(path));
		lastSet = path;
		lastSetMeta = -1;
		setGap = 0.04f;
		setSinceAsk = true;
	}

	// ---- WING-FX: its panel <-> the slots, the tempo
	void controlFx(float dt, bool connected) {
		WingFx* fm = fxMod.load();
		if (fm != fxSeen) {
			fxSeen = fm;
			fxPageSeen = -1;
		}
		if (!fm)
			return;
		int step = fm->slotStep == 1 ? 1 : 4;
		if (pageDownT.process(fm->params[WingFx::PAGE_DOWN_PARAM].getValue() > 0.f))
			fm->first = std::max(0, step == 4 ? (fm->first - 1) / 4 * 4 : fm->first - 1);
		if (pageUpT.process(fm->params[WingFx::PAGE_UP_PARAM].getValue() > 0.f))
			fm->first = std::min(NSLOT - NPAGE, step == 4 ? fm->first / 4 * 4 + 4 : fm->first + 1);
		if (fm->refresh.exchange(false))
			fxPageSeen = -1;
		int first = fm->first;
		bool show = fxShow;
		fxShow = false;
		if (first != fxPageSeen || show) {
			// new slots on the panel (or a new model, or another parameter on a knob): show what we know, then ask
			// (a new model only: shown again, its slot's values are asked where the model came)
			bool ask = first != fxPageSeen;
			if (fxPageSeen >= 0 && first != fxPageSeen)
				for (int k = 0; k < NSLOT; k++)
					for (int f = 0; f < NFX; f++)
						fxs[k].known[f] = fxs[k].known[f] && f <= FMIX;  // (the knobs may mean other parameters there)
			fxPageSeen = first;
			for (int j = 0; j < NPAGE; j++) {
				int slot = first + j;
				FxSlot& x = fxs[slot];
				for (int f = 0; f < NFX; f++) {
					float v = x.known[f] ? x.val[f] : fm->params[fxPid(j, f)].getValue();
					fm->params[fxPid(j, f)].setValue(v);
					x.f[f].local = v;
					x.f[f].wing = v;
					x.f[f].dirty = false;
					x.f[f].sent = 0;
				}
				if (connected && sysexOk && ask)
					askFx(slot, true);
			}
		}
		// the panel's changes (and CV)
		for (int j = 0; j < NPAGE; j++) {
			int slot = first + j;
			FxSlot& x = fxs[slot];
			for (int k = 0; k < 5; k++)
				if (fm->inputs[fxCv(j, k)].isConnected())
					fm->params[fxPid(j, FMIX + k)].setValue(clamp(fm->inputs[fxCv(j, k)].getVoltage() / 10.f, 0.f, 1.f));
			for (int f = 0; f < NFX; f++) {
				Sync& y = x.f[f];
				float v = fm->params[fxPid(j, f)].getValue();
				if (v != y.local) {
					y.local = v;
					x.val[f] = v;
					x.known[f] = true;
					if (connected && !(std::fabs(v - y.wing) < 1e-4f) && (f <= FMIX || fxKnob(slot, f - FP1))) {
						y.dirty = true;
						y.hold = 0.4f;
					}
				}
			}
			fm->lights[WingFx::ON_LIGHT + j].setBrightness(fm->params[fxPid(j, FON)].getValue() > 0.5f ? 1.f : 0.f);
		}
		for (int slot = 0; slot < NSLOT; slot++)
			for (int f = 0; f < NFX; f++)
				fxs[slot].f[f].hold = std::max(0.f, fxs[slot].f[f].hold - dt);

		// tempo: TAP
		if (tapT.process(fm->params[WingFx::TAP_PARAM].getValue() > 0.f)) {
			double gap = now - lastTap;
			if (gap > 0.15 && gap < 2.5) {
				tapGaps[nGaps % 4] = (float) gap;
				nGaps++;
				float sum = 0.f;
				int n = std::min(nGaps, 4);
				for (int i = 0; i < n; i++)
					sum += tapGaps[i];
				rackBpm = 60.f / (sum / n);
			}
			else
				nGaps = 0;
			lastTap = now;
			tapFlash = 0.1f;
		}
		tapFlash = std::max(0.f, tapFlash - dt);
		fm->lights[WingFx::TAP_LIGHT].setBrightness(tapFlash > 0.f ? 1.f : 0.f);
		bool sync = fm->params[WingFx::SYNC_PARAM].getValue() > 0.5f;
		fm->lights[WingFx::SYNC_LIGHT].setBrightness(sync ? 1.f : 0.f);

		// tempo: SYNC on → every delay takes Rack's tempo; off → the first delay's time is the WING's tempo
		tempoTimer -= dt;
		if (tempoTimer > 0.f)
			return;
		tempoTimer = 0.25f;
		float wb = 0.f;
		int wslot = -1;
		for (int slot = 0; slot < NSLOT; slot++) {
			FxSlot& x = fxs[slot];
			const F::Model& M = F::MODELS[x.model];
			if (M.timeParam < 0)
				continue;
			const F::Param& p = M.all[M.timeParam];
			if (sync && rackBpm > 0.f && connected && sysexOk) {
				float ms = clamp(60000.f / rackBpm, p.min, p.max);
				if (std::isnan(x.timeMs) || std::fabs(x.timeMs - ms) >= 1.f) {
					send(P::fxSet(slot + 1, std::string("time=") + F::wingText(p, ms)));
					x.timeMs = ms;
					// a knob that shows TIME moves with it
					int j = fxVisible(slot);
					for (int k = 0; k < 4 && j >= 0; k++) {
						const F::Param* kp = fxKnob(slot, k);
						if (kp && !std::strcmp(kp->key, "time")) {
							float n = F::toNorm(*kp, ms);
							int f = FP1 + k;
							x.val[f] = n;
							x.known[f] = true;
							x.f[f].local = x.f[f].wing = n;
							x.f[f].dirty = false;
							x.f[f].sent = 0;
							fm->params[fxPid(j, f)].setValue(n);
						}
					}
				}
			}
			if (wslot < 0 && !std::isnan(x.timeMs) && x.timeMs > 1.f) {
				wb = 60000.f / x.timeMs;
				while (wb < 60.f)
					wb *= 2.f;
				while (wb > 240.f)
					wb /= 2.f;
				wslot = slot;
			}
		}
		wingBpm = wb;
		wingBpmSlot = wslot;
	}

	/** Per sample: CLOCK in → Rack's tempo, CLOCK out ← the tempo in use. */
	void processTempo(float dt) {
		now += dt;
		WingFx* fm = fxMod.load();
		if (!fm)
			return;
		if (clockIn.process(fm->inputs[WingFx::CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastClock >= 0.0) {
				float per = (float) (now - lastClock) * fm->ppqIn;
				if (per > 0.12f && per < 3.f) {
					clockPeriods[nPeriods % 4] = per;
					nPeriods++;
					float sum = 0.f;
					int n = std::min(nPeriods, 4);
					for (int i = 0; i < n; i++)
						sum += clockPeriods[i];
					rackBpm = 60.f / (sum / n);
				}
			}
			lastClock = now;
		}
		bool sync = fm->params[WingFx::SYNC_PARAM].getValue() > 0.5f;
		float bpm = sync ? rackBpm.load() : wingBpm.load();
		if (bpm > 0.f) {
			clockPhase += dt * bpm / 60.f * fm->ppqOut;
			if (clockPhase >= 1.f) {
				clockPhase -= std::floor(clockPhase);
				clockOut.trigger(1e-3f);
			}
		}
		fm->outputs[WingFx::CLOCK_OUTPUT].setVoltage(clockOut.process(dt) ? 10.f : 0.f);
	}

	// ---- the control tick (every 32 samples)
	void control(float dt) {
		midiOut.channel = -1;  // (a pick in the MIDI menu may set one: keep our own channels)
		midiIn.channel = -1;
		port.wantOffset = portOffset();

		int inDev = midiIn.getDeviceId(), outDev = midiOut.getDeviceId();
		if (inDev != lastInDev || outDev != lastOutDev) {
			lastInDev = inDev;
			lastOutDev = outDev;
			sysexOk = false;
			midiSeen = false;
			identTimer = 0.f;
			resetSync();
		}
		bool connected = outDev >= 0;
		if (connected && !sysexOk) {
			identTimer -= dt;
			if (identTimer <= 0.f) {
				send(P::sysex(P::IDENT, ""));
				identTimer = 2.f;
			}
		}

		setGap = std::max(0.f, setGap - dt);
		qDrain = std::max(0.f, qDrain - dt);
		metaPoll = std::max(0.f, metaPoll - dt);
		errorAge.store(errorAge.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
		if (wantReset.exchange(false))
			resetSync();
		// WING-LINK X's own START CH
		if (WingLinkX* x = xMod.load()) {
			bool x8 = x->xStep == 8;
			int cur = xStartNow();
			int nxt = cur;
			if (xDownT.process(x->params[WingLinkX::DOWN_PARAM].getValue() > 0.f) && cur > 1)
				nxt = x8 ? std::max(1, ((cur - 2) / 8) * 8 + 1) : cur - 1;
			if (xUpT.process(x->params[WingLinkX::UP_PARAM].getValue() > 0.f) && cur < P::NUM_IN)
				nxt = x8 ? std::min(41, ((cur - 1) / 8 + 1) * 8 + 1) : cur + 1;
			if (nxt != cur) {
				x->xStart = nxt;
				resetSync();
			}
		}
		bool by8 = chStep == 8;
		if (downT.process(params[DOWN_PARAM].getValue() > 0.f) && wingStart > 1) {
			wingStart = by8 ? std::max(1, ((wingStart - 2) / 8) * 8 + 1) : wingStart - 1;
			resetSync();
		}
		if (upT.process(params[UP_PARAM].getValue() > 0.f) && wingStart < P::NUM_IN) {
			wingStart = by8 ? std::min(41, ((wingStart - 1) / 8 + 1) * 8 + 1) : wingStart + 1;
			resetSync();
		}
		if (syncT.process(params[SYNC_PARAM].getValue() > 0.f))
			syncNow = true;
		syncFlash = std::max(0.f, syncFlash - dt);
		routeTimer -= dt;
		if (routeTimer <= 0.f) {
			routeTimer = 0.5f;
			checkRouting();
		}
		lights[SYNC_LIGHT].setBrightness(syncFlash > 0.f ? 1.f : 0.f);

		// the strips
		const float envFall = std::pow(0.5f, dt / 0.12f);
		for (int s = 0; s < NT; s++) {
			int j;
			Module* m = owner(s, &j);
			Strip& t = st[s];
			t.env = std::max(t.peak, t.env * envFall);
			t.peak = 0.f;
			if (!m)
				continue;
			bool on = active(s);
			auto cv = [&](int in, int f, float v) {
				if (m->inputs[iid(j, in)].isConnected())
					m->params[pid(j, f)].setValue(v);
			};
			cv(CV_FDR, FADER, clamp(m->inputs[iid(j, CV_FDR)].getVoltage() / 10.f, 0.f, 1.f));
			cv(CV_PAN, PAN, clamp(m->inputs[iid(j, CV_PAN)].getVoltage() / 5.f, -1.f, 1.f));
			cv(CV_FX1, FX1, clamp(m->inputs[iid(j, CV_FX1)].getVoltage() / 10.f, 0.f, 1.f));
			cv(CV_FX2, FX2, clamp(m->inputs[iid(j, CV_FX2)].getVoltage() / 10.f, 0.f, 1.f));
			for (int f = 0; f < NF; f++) {
				Sync& y = t.f[f];
				float v = m->params[pid(j, f)].getValue();
				if (v != y.local) {
					y.local = v;
					if (on && connected && has(s, f) && !(std::fabs(v - y.wing) < 1e-4f)) {
						y.dirty = true;
						y.hold = 0.4f;
					}
				}
				y.hold = std::max(0.f, y.hold - dt);
			}
			float fd = m->params[pid(j, FADER)].getValue();
			bool mu = m->params[pid(j, MUTE)].getValue() > 0.5f;
			bool so = m->params[pid(j, SOLO)].getValue() > 0.5f;
			m->outputs[oid(j, OUT_FDR)].setVoltage(fd * 10.f);
			m->outputs[oid(j, OUT_MUTE)].setVoltage(mu ? 10.f : 0.f);
			m->lights[j].setBrightness(mu ? 1.f : 0.f);          // MUTE_LIGHT + j (the same on both panels)
			m->lights[NS + j].setBrightness(so ? 1.f : 0.f);     // SOLO_LIGHT + j
		}

		// FX models picked on WING-FX
		for (int slot = 0; slot < NSLOT; slot++) {
			int req = fxModelReq[slot].exchange(-1);
			if (req >= 0 && connected) {
				send(P::fxSet(slot + 1, std::string("mdl=") + F::MODELS[req].mdl));
				setFxModel(slot, req);
				queries.push_back({1, slot, FMODEL});
				askFx(slot, false);
			}
		}
		controlFx(dt, connected);
		controlEq(dt, connected);
		controlDyn(dt, connected);
		controlIn(dt, connected);
		if (!connected)
			return;

		// the SD recorder: a click on a card starts it (with the tracks set) or stops it
		for (int c = 0; c < 2; c++)
			if (sd[c].toggle.exchange(false)) {
				std::string n = string::f("cards.wlive.%d.", c + 1);
				if (sd[c].state == 3)
					outbox.push_back(n + "$ctl.control=STOP");
				else {
					outbox.push_back(n + (params[TRACKS_PARAM].getValue() > 0.5f ? "cfg.rectracks=16" : "cfg.rectracks=8"));
					outbox.push_back(n + "$ctl.control=REC");
				}
				queries.push_front({2, c, 0});
			}

		// SYNC: the AUDIO jacks are routed from here (START CH now); names, colours, icons of the strips shown go out
		if (syncNow.exchange(false)) {
			syncFlash = 0.3f;
			fresh = false;
			routeWrong = false;
			audioStart = wingStart.load();
			for (int s = 0; s < NT; s++) {
				int j;
				if (!owner(s, &j))
					continue;
				P::Tgt home = P::inputTgt(audioStart + s);
				if (home.ok()) {
					outbox.push_back(P::tgtNode(home) + string::f(".in.conn.grp=USB,in=%d", usbOf(s)));
					// (its source is known now: USB n)
					std::lock_guard<std::mutex> lock(metaMutex);
					Meta& m = meta[metaKey(home)];
					m.grp = "USB";
					m.in = usbOf(s);
					m.srcBad = false;
				}
			}
			{
				std::lock_guard<std::mutex> lock(metaMutex);
				for (int s = 0; s < NT; s++) {
					if (!active(s))
						continue;
					// (whether the channel is linked to its source is asked with everything else below: resetSync;
					// names / colours / icons wait for it)
					int k = metaKey(target(s));
					Meta& m = meta[k];
					int d = (m.name.empty() ? 0 : M_NAME) | (m.colour > 0 ? M_COL : 0) | (m.icon >= 0 ? M_ICON : 0);
					if (d) {
						m.dirty |= d;
						m.wait = 0.f;
						m.hold = 3.f;
					}
				}
			}
			resetSync();
		}

		// the names' timers (every 8th tick)
		bool metaNow = (++metaTick & 7) == 0;
		if (metaNow) {
			std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
			if (lock.owns_lock())
				for (auto& kv : meta) {
					kv.second.wait = std::max(0.f, kv.second.wait - 8.f * dt);
					kv.second.hold = std::max(0.f, kv.second.hold - 8.f * dt);
				}
		}

		// a question that has waited 100 ms for the commands to pause (a CV moves a fader all the time): it goes first
		if (sysexOk && !qWaiting && qDrain <= 0.f && !queries.empty())
			qStarve += dt;
		else
			qStarve = 0.f;
		bool qFirst = qStarve > 0.1f;
		// (just asked: no command for 30 ms, so a complaint in that time is the question's answer)
		bool justAsked = qWaiting && qTimer > 0.22f;

		// send: one command every 3 ms at most. SYNC's routing and the names never while a question waits for its
		// answer; the strips', FX', EQ's and DYN's values also then, but not right after it was asked, and not while a
		// question has waited too long. A value goes out only when it differs, rounded as the WING takes it, from
		// what was sent last. (order: SYNC's routing, then strips and FX round robin, then names)
		sendWait -= dt;
		if (sendWait <= 0.f) {
			bool sent = false;
			if (!outbox.empty() && sysexOk && !qWaiting) {
				sendSet(outbox.front());
				outbox.pop_front();
				sent = true;
			}
			const int NSTRIP = NT * NF, NFXALL = NSLOT * NFX, NALL = NSTRIP + NFXALL + NEQ + NDYN;
			for (int n = 0; n < NALL && !sent && !qFirst && !justAsked; n++) {
				int i = (sendRR + n) % NALL;
				if (i >= NSTRIP + NFXALL + NEQ) {
					int f = i - NSTRIP - NFXALL - NEQ;
					Sync& y = dyns[f];
					if (!y.dirty)
						continue;
					y.dirty = false;
					if (!sysexOk || dynKeySeen < 0)
						continue;
					std::string what = std::string(DYN[f].node) + "=" + dynText(f, y.local);
					uint32_t key = textKey(what);
					y.wing = y.local;
					if (key == y.sent)
						continue;  // (the WING has that from us)
					y.sent = key;
					sendSet(P::tgtNode(keyTgt(dynKeySeen)) + "." + what);
					y.hold = 0.4f;
					sendRR = i + 1;
					sent = true;
					break;
				}
				if (i >= NSTRIP + NFXALL) {
					int f = i - NSTRIP - NFXALL;
					Sync& y = eqs[f];
					if (!y.dirty)
						continue;
					y.dirty = false;
					if (!sysexOk || eqKeySeen < 0)
						continue;
					std::string what = eqNode(f) + "=" + eqText(f, y.local);
					uint32_t key = textKey(what);
					y.wing = y.local;
					if (key == y.sent)
						continue;  // (the WING has that from us)
					y.sent = key;
					sendSet(P::tgtNode(keyTgt(eqKeySeen)) + "." + what);
					y.hold = 0.4f;
					sendRR = i + 1;
					sent = true;
					break;
				}
				if (i >= NSTRIP) {
					int slot = (i - NSTRIP) / NFX, f = (i - NSTRIP) % NFX;
					Sync& y = fxs[slot].f[f];
					if (!y.dirty)
						continue;
					y.dirty = false;
					if (f != FON && !sysexOk)
						continue;
					y.wing = y.local;
					if (!sendFx(slot, f, y.local))
						continue;  // (the WING has that from us)
					y.hold = 0.4f;
					sendRR = i + 1;
					sent = true;
					break;
				}
				int s = i / NF, f = i % NF;
				Sync& y = st[s].f[f];
				if (!y.dirty || !active(s))
					continue;
				y.dirty = false;
				if ((f == SOLO || f == FX1 || f == FX2) && !sysexOk)
					continue;
				y.wing = y.local;
				if (!sendField(s, f, y.local))
					continue;  // (the WING has that from us)
				y.hold = 0.4f;
				sendRR = i + 1;
				sent = true;
				break;
			}
			if (!sent && sysexOk && !qWaiting && !qFirst && metaNow) {
				std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
				if (lock.owns_lock())
					for (auto& kv : meta) {
						Meta& m = kv.second;
						if (!m.dirty || m.wait > 0.f)
							continue;
						P::Tgt mt = keyTgt(kv.first);
						if ((mt.type == P::T_CH || mt.type == P::T_AUX) && (m.link < 0 || m.grp.empty()))
							continue;  // (where the WING keeps them is not known yet)
						int part = (m.dirty & M_NAME) ? M_NAME : ((m.dirty & M_COL) ? M_COL : M_ICON);
						m.dirty &= ~part;
						m.hold = std::max(m.hold, 1.5f);
						sendMetaPart(kv.first, part, m);
						sent = true;
						break;
					}
			}
			if (sent)
				sendWait = 0.003f;
		}

		// ask: after a connect / SYNC everything, then now and then what has no CC (solo, sends, FX on the panel)
		if (sysexOk) {
			if (pullAll) {
				pullAll = false;
				queries.clear();
				selAsked = false;
				metaPoll = 6.f;
				for (int s = 0; s < NT; s++)
					if (active(s)) {
						for (int f = 0; f < NF; f++)
							if (has(s, f))
								queries.push_back({0, s, f});
						askMeta(s);
					}
				if (WingFx* fm = fxMod.load())
					for (int j = 0; j < NPAGE; j++)
						askFx(fm->first + j, true);
				if (eqMod.load() && eqKeySeen >= 0) {
					askEq();
					askMetaKey(eqKeySeen);
				}
				if (dynMod.load() && dynKeySeen >= 0) {
					askDyn();
					askMetaKey(dynKeySeen);
				}
				askIn();
				for (int ch = 8; ch < 16; ch++) {  // FX: ask the WING for its CCs (ON has no other way)
					sendCc(ch, 9, ch - 8);
					sendCc(ch, 9, ch - 8 + 8);
				}
			}
			pollTimer -= dt;
			if (pollTimer <= 0.f && queries.empty()) {
				pollTimer = 1.5f;
				// (names, colours, icons and the channels' sources change seldom: every 6 s)
				bool withMeta = metaPoll <= 0.f;
				if (withMeta)
					metaPoll = 6.f;
				for (int s = 0; s < NT; s++)
					if (active(s)) {
						for (int f : {SOLO, FX1, FX2})
							if (has(s, f))
								queries.push_back({0, s, f});
						if (withMeta)
							askMeta(s);
					}
				if (WingFx* fm = fxMod.load())
					for (int j = 0; j < NPAGE; j++)
						askFx(fm->first + j, true);
				if (eqMod.load() && eqKeySeen >= 0) {
					askEq();
					if (withMeta)
						askMetaKey(eqKeySeen);
				}
				if (dynMod.load() && dynKeySeen >= 0) {
					askDyn();
					if (withMeta)
						askMetaKey(dynKeySeen);
				}
				askIn();
				for (int c = 0; c < 2; c++)
					for (int f = 0; f < 4; f++)
						queries.push_back({2, c, f});
			}
			if (qWaiting) {
				qTimer -= dt;
				if (qTimer <= 0.f) {
					qWaiting = false;
					qDrain = 0.3f;  // (a late answer must not land on the next question)
					noAnswer++;
					if (qNow.kind == 3 && qNow.f >= M_LINK) {
						// no answer: take the channel's own fields
						std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
						if (Meta* m = lock.owns_lock() ? metaOf(qNow.s) : nullptr) {
							if (qNow.f == M_LINK && m->link < 0)
								m->link = 0;
							if (qNow.f == M_GRP && m->grp.empty())
								m->grp = "OFF";
						}
					}
					if (logging())
						logLine(string::f("--   no answer (kind %d s %d f %d)", qNow.kind, qNow.s, qNow.f));
				}
			}
			// (after a command 40 ms for its complaint; 10 ms when the question has waited too long already)
			while (!qWaiting && !queries.empty() && sendWait <= 0.f && setGap <= (qFirst ? 0.03f : 0.f) && qDrain <= 0.f) {
				qNow = queries.front();
				queries.pop_front();
				if (qNow.kind == 0) {
					if (!active(qNow.s) || !has(qNow.s, qNow.f))
						continue;
					send(P::getPath(node(qNow.s) + "." + queryOf(qNow.f)));
				}
				else if (qNow.kind == 3) {
					P::Tgt t = keyTgt(qNow.s);
					if (!t.ok() || (qNow.f == M_ICON && t.type == P::T_DCA))
						continue;
					if (qNow.f >= M_LINK) {
						const char* what = qNow.f == M_LINK ? "clink" : (qNow.f == M_GRP ? "in.conn.grp" : "in.conn.in");
						send(P::getPath(P::tgtNode(t) + "." + what));
					}
					else {
						std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
						if (!lock.owns_lock()) {
							queries.push_front(qNow);
							break;
						}
						const Meta& m = meta[qNow.s];
						qNow.src = viaSource(t, m);
						const char* what = qNow.f == M_NAME ? "name" : (qNow.f == M_COL ? "col" : "icon");
						send(P::getPath(metaNode(t, m) + "." + what));
					}
				}
				else if (qNow.kind == 4) {
					if (qNow.s != eqKeySeen)
						continue;
					send(P::getPath(P::tgtNode(keyTgt(qNow.s)) + "." + (qNow.f == EQ_MDL ? std::string("eq.mdl") : eqNode(qNow.f))));
				}
				else if (qNow.kind == 6) {
					if (qNow.s != dynKeySeen)
						continue;
					std::string node = qNow.f == D_GMDL ? "gate.mdl" : (qNow.f == D_CMDL ? "dyn.mdl" : DYN[qNow.f].node);
					send(P::getPath(P::tgtNode(keyTgt(qNow.s)) + "." + node));
				}
				else if (qNow.kind == 7) {
					WingIn* im = inMod.load();
					if (!im || im->known[qNow.s])
						continue;
					int usb = clamp(im->start.load(), 1, 41) + qNow.s;
					static const char* PART[6] = {"grp", "in", "user.grp", "user.in", "user.tap", "user.lr"};
					if (qNow.f <= 1)
						send(P::getPath(string::f("io.out.USB.%d.%s", usb, PART[qNow.f])));
					else {
						bool internal;
						{
							std::unique_lock<std::mutex> lock(im->srcMutex, std::try_to_lock);
							internal = lock.owns_lock() && igInternal(im->src[qNow.s].grp);
						}
						if (!internal)
							continue;
						send(P::getPath(string::f("io.in.USR.%d.%s", qNow.s + 1, PART[qNow.f])));
					}
				}
				else if (qNow.kind == 5) {
					selAsked = false;
					send(P::getPath("$ctl.$stat.selidx"));
				}
				else if (qNow.kind == 2) {
					static const char* const SDQ[4] = {"$stat.state", "$stat.etime", "$stat.sdfree", "$stat.sdstate"};
					send(P::getPath(string::f("cards.wlive.%d.", qNow.s + 1) + SDQ[qNow.f]));
				}
				else {
					std::string what;
					if (qNow.f == FMODEL)
						what = "mdl";
					else if (qNow.f == FMIX)
						what = "fxmix";
					else if (qNow.f == FTIME) {
						const F::Model& M = F::MODELS[fxs[qNow.s].model];
						if (M.timeParam < 0)
							continue;
						what = M.all[M.timeParam].key;
					}
					else {
						const F::Param* p = fxKnob(qNow.s, qNow.f - FP1);
						if (!p)
							continue;
						what = p->key;
					}
					send(P::fxGet(qNow.s + 1, what));
				}
				qWaiting = true;
				setSinceAsk = false;
				qStarve = 0.f;
				qTimer = 0.25f;
				sendWait = 0.002f;
			}
		}
	}

	void process(const ProcessArgs& args) override {
		midi::Message msg;
		while (midiIn.tryPop(&msg, args.frame))
			onMidi(msg);

		WingLinkX* x = xMod.load();
		dsp::Frame<MAXCH> fr = {};
		for (int s = 0; s < NT; s++) {
			Module* m = s < NS ? (Module*) this : (Module*) x;
			if (!m)
				break;
			float v = m->inputs[iid(s % NS, AUDIO)].getVoltageSum() / 10.f;
			fr.samples[s] = v;
			st[s].peak = std::max(st[s].peak, std::fabs(v));
		}
		if (port.deviceNumOutputs > 0 && !port.engineInputBuffer.full())
			port.engineInputBuffer.push(fr);
		if (WingIn* im = inMod.load()) {
			int ni = port.deviceNumInputs;
			dsp::Frame<NIN> fi = {};
			if (ni > 0 && !port.engineOutputBuffer.empty())
				fi = port.engineOutputBuffer.shift();
			for (int c = 0; c < NIN; c++)
				im->audio[c].store(c < ni ? fi.samples[c] : 0.f, std::memory_order_relaxed);
			im->hasAudio = ni > 0;
			// (not more than ~2 blocks behind)
			while ((int) port.engineOutputBuffer.size() > 4096)
				port.engineOutputBuffer.shift();
		}
		processTempo(args.sampleTime);
		if (ctl.process())
			control(args.sampleTime * ctl.getDivision());
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		// (the control tick: 1500 times a second at 48 kHz, and no more often at 96 or 192 kHz)
		ctl.setDivision(std::max(32, (int) std::round(e.sampleRate / 1500.f)));
		port.engineInputBuffer.clear();
		port.engineOutputBuffer.clear();
	}

	// ---- the panels' edits (UI thread)
	/** (UI thread) What the strip's WING channel is called, its colour and icon (a copy). */
	Meta getMeta(int s) {
		P::Tgt t = target(s);
		std::lock_guard<std::mutex> lock(metaMutex);
		auto it = meta.find(metaKey(t));
		return it == meta.end() ? Meta() : it->second;
	}
	std::string getName(int s) { return getMeta(s).name; }
	/** (UI thread) The same, fetched once per frame and strip (the strip's widgets all show it). */
	int uiFrame = 0;  // (counted by the panel)
	Meta uiMeta[NT];
	int uiMetaFrame[NT] = {}, uiMetaKey[NT] = {};
	const Meta& getMetaUi(int s) {
		int k = metaKey(target(s));
		if (uiMetaFrame[s] != uiFrame || uiMetaKey[s] != k) {
			uiMeta[s] = getMeta(s);
			uiMetaFrame[s] = uiFrame;
			uiMetaKey[s] = k;
		}
		return uiMeta[s];
	}
	void editMeta(int s, int part, std::function<void(Meta&)> f, float wait) {
		P::Tgt t = target(s);
		if (!t.ok())
			return;
		for (int i = 0; i < NT; i++)
			uiMetaKey[i] = -1;  // (what the panel fetched this frame is old now)
		std::lock_guard<std::mutex> lock(metaMutex);
		Meta& m = meta[metaKey(t)];
		f(m);
		m.known = true;
		m.dirty |= part;
		m.wait = wait;
		m.hold = wait + 2.f;
	}
	void setName(int s, const std::string& n) {
		if (getName(s) == n)
			return;
		editMeta(s, M_NAME, [&](Meta& m) { m.name = n; }, 0.6f);
	}
	void nameNow(int s) {
		P::Tgt t = target(s);
		std::lock_guard<std::mutex> lock(metaMutex);
		auto it = meta.find(metaKey(t));
		if (it != meta.end())
			it->second.wait = 0.f;
	}
	void setColour(int s, int c) { editMeta(s, M_COL, [&](Meta& m) { m.colour = c; }, 0.2f); }
	void setIcon(int s, int i) { editMeta(s, M_ICON, [&](Meta& m) { m.icon = i; }, 0.2f); }

	void setTarget(int s, P::Tgt t) {
		st[s].tType = t.ok() ? t.type : 0;
		st[s].tNum = t.ok() ? t.n : 0;
		wantReset = true;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "wingStart", json_integer(wingStart));
		json_object_set_new(root, "usbMode", json_integer(usbMode));
		json_object_set_new(root, "fxBus1", json_integer(fxBus[0]));
		json_object_set_new(root, "fxBus2", json_integer(fxBus[1]));
		json_object_set_new(root, "autoMidi", json_boolean(autoMidi));
		json_object_set_new(root, "autoAudio", json_boolean(autoAudio));
		json_object_set_new(root, "chStep", json_integer(chStep));
		json_object_set_new(root, "midiIn", midiIn.toJson());
		json_object_set_new(root, "midiOut", midiOut.toJson());
		json_object_set_new(root, "audio", port.toJson());
		json_object_set_new(root, "audioStart", json_integer(audioStart));
		json_t* strips = json_array();
		for (int s = 0; s < NT; s++) {
			json_t* o = json_object();
			json_object_set_new(o, "type", json_integer(st[s].tType));
			json_object_set_new(o, "num", json_integer(st[s].tNum));
			json_array_append_new(strips, o);
		}
		json_object_set_new(root, "strips", strips);
		json_t* names = json_array();
		{
			std::lock_guard<std::mutex> lock(metaMutex);
			for (auto& kv : meta) {
				if (!kv.second.known)
					continue;
				json_t* o = json_object();
				json_object_set_new(o, "ch", json_string(P::tgtLabel(keyTgt(kv.first)).c_str()));
				json_object_set_new(o, "key", json_integer(kv.first));
				json_object_set_new(o, "name", json_string(kv.second.name.c_str()));
				json_object_set_new(o, "colour", json_integer(kv.second.colour));
				json_object_set_new(o, "icon", json_integer(kv.second.icon));
				json_array_append_new(names, o);
			}
		}
		json_object_set_new(root, "names", names);
		json_t* fx = json_array();
		for (int k = 0; k < NSLOT; k++)
			json_array_append_new(fx, json_string(F::MODELS[fxs[k].model].mdl));
		json_object_set_new(root, "fxModels", fx);
		return root;
	}
	void dataFromJson(json_t* root) override {
		auto geti = [&](const char* k, int def) {
			json_t* j = json_object_get(root, k);
			return j ? (int) json_integer_value(j) : def;
		};
		wingStart = clamp(geti("wingStart", 1), 1, P::NUM_IN);
		fresh = false;
		usbMode = clamp(geti("usbMode", 0), 0, 48);
		fxBus[0] = clamp(geti("fxBus1", 1), 1, 16);
		fxBus[1] = clamp(geti("fxBus2", 2), 1, 16);
		chStep = geti("chStep", 8) == 1 ? 1 : 8;
		if (json_t* j = json_object_get(root, "autoMidi"))
			autoMidi = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "autoAudio"))
			autoAudio = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "midiIn"))
			midiIn.fromJson(j);
		if (json_t* j = json_object_get(root, "midiOut"))
			midiOut.fromJson(j);
		midiIn.setChannel(-1);
		midiOut.setChannel(-1);
		port.wantOffset = portOffset();
		if (json_t* j = json_object_get(root, "audio"))
			port.fromJson(j);
		audioStart = clamp(geti("audioStart", wingStart), 1, P::NUM_IN);
		if (json_t* a = json_object_get(root, "strips")) {
			std::lock_guard<std::mutex> lock(metaMutex);
			for (int s = 0; s < NT && s < (int) json_array_size(a); s++) {
				json_t* o = json_array_get(a, s);
				json_t* ty = json_object_get(o, "type");
				json_t* nu = json_object_get(o, "num");
				if (ty && nu) {
					P::Tgt t = P::tgt((int) json_integer_value(ty), (int) json_integer_value(nu));
					st[s].tType = t.ok() ? t.type : 0;
					st[s].tNum = t.ok() ? t.n : 0;
				}
				// (2.2: the names were the strips'; now they are the WING channels')
				json_t* nm = json_object_get(o, "name");
				if (nm && json_is_string(nm) && json_string_length(nm) > 0 && s < NS) {
					Meta& m = meta[metaKey(target(s))];
					m.name = json_string_value(nm);
					m.known = true;
					if (json_t* j = json_object_get(o, "colour"))
						m.colour = clamp((int) json_integer_value(j), 0, P::NUM_COLOURS);
					if (json_t* j = json_object_get(o, "icon"))
						m.icon = (int) json_integer_value(j);
				}
			}
		}
		if (json_t* a = json_object_get(root, "names")) {
			std::lock_guard<std::mutex> lock(metaMutex);
			for (size_t i = 0; i < json_array_size(a); i++) {
				json_t* o = json_array_get(a, i);
				json_t* k = json_object_get(o, "key");
				if (!k || !keyTgt((int) json_integer_value(k)).ok())
					continue;
				Meta& m = meta[(int) json_integer_value(k)];
				json_t* j = json_object_get(o, "name");
				if (j && json_is_string(j))
					m.name = std::string(json_string_value(j)).substr(0, 16);
				if ((j = json_object_get(o, "colour")))
					m.colour = clamp((int) json_integer_value(j), 0, P::NUM_COLOURS);
				if ((j = json_object_get(o, "icon")))
					m.icon = clamp((int) json_integer_value(j), -1, 999);
				m.known = true;
			}
		}
		if (json_t* a = json_object_get(root, "fxModels"))
			for (int k = 0; k < NSLOT && k < (int) json_array_size(a); k++)
				if (json_t* j = json_array_get(a, k))
					if (json_is_string(j))
						fxs[k].model = F::modelIndex(json_string_value(j));
		lastInDev = lastOutDev = -2;
	}
};

// ---------------------------------------------------------------- the panels' link
/** (the panels WING-DYN, WING-KNOBS, WING-IN) one per WING-LINK: the slot's */
template <class T>
static void dropSat(std::atomic<T*>& slot, Satellite* s) {
	if (T* t = dynamic_cast<T*>(s)) {
		T* cur = t;
		slot.compare_exchange_strong(cur, nullptr);
	}
}
template <class T>
static void takeSat(std::atomic<T*>& slot, Satellite* s, WingLink* h) {
	if (T* t = dynamic_cast<T*>(s)) {
		T* cur = slot.load();
		if (!cur || cur->host.load() != h)
			slot = t;
	}
}
template <class T>
static void staleSat(std::atomic<T*>& slot, WingLink* m) {
	if (T* t = slot.load())
		if (t->host.load() != m) {
			T* cur = t;
			slot.compare_exchange_strong(cur, nullptr);
		}
}

WingLink* Satellite::hostSafe() {
	WingLink* h = host.load();
	return hostAlive(h) ? h : nullptr;
}
void Satellite::onRemove(const RemoveEvent& e) {
	WingLink* h = host.load();
	if (hostAlive(h)) {
		WingLinkX* x = dynamic_cast<WingLinkX*>(this);
		WingFx* f = dynamic_cast<WingFx*>(this);
		if (WingEq* q = dynamic_cast<WingEq*>(this)) {
			WingEq* cur = q;
			h->eqMod.compare_exchange_strong(cur, nullptr);
		}
		dropSat(h->dynMod, this);
		dropSat(h->knobsMod, this);
		dropSat(h->inMod, this);
		if (x) {
			WingLinkX* cur = x;
			h->xMod.compare_exchange_strong(cur, nullptr);
		}
		if (f) {
			WingFx* cur = f;
			h->fxMod.compare_exchange_strong(cur, nullptr);
		}
	}
	host = nullptr;
}
void Satellite::onReset() {
	// (Initialize put this panel to its defaults: WING-LINK doesn't send them, it reads the WING's values again)
	WingLink* h = host.load();
	if (hostAlive(h))
		h->wantReset = true;
}
Satellite::~Satellite() {}
json_t* Satellite::dataToJson() {
	json_t* root = json_object();
	json_object_set_new(root, "linkId", json_integer(linkId));
	json_object_set_new(root, "lastId", json_integer(lastId));
	return root;
}
void Satellite::dataFromJson(json_t* root) {
	if (json_t* j = json_object_get(root, "linkId"))
		linkId = json_integer_value(j);
	if (json_t* j = json_object_get(root, "lastId"))
		lastId = json_integer_value(j);
}

WingLinkX::WingLinkX() {
	config(NUM_PARAMS, NS * NCI, NS * NCO, NUM_LIGHTS);
	configStrips(this, NS);
	configButton(DOWN_PARAM, "Start channel down");
	configButton(UP_PARAM, "Start channel up");
	noRandomize(this);
}
json_t* WingLinkX::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "xStart", json_integer(xStart));
	json_object_set_new(root, "xStep", json_integer(xStep));
	return root;
}
void WingLinkX::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "xStart"))
		xStart = json_integer_value(j) >= 1 && json_integer_value(j) <= 48 ? (int) json_integer_value(j) : -1;
	if (json_t* j = json_object_get(root, "xStep"))
		xStep = json_integer_value(j) == 8 ? 8 : 1;
}

/** (UI thread) Which WING-LINK a panel belongs to: docked next to it, picked by right-click, or the only one. */
static WingLink* findHost(Satellite* s) {
	Module* side = s->dockLeft() ? s->leftExpander.module : s->rightExpander.module;
	if (side && side->model == modelWingLink)
		return static_cast<WingLink*>(side);
	int64_t id = s->linkId;
	if (id >= 0) {
		Module* m = APP->engine->getModule(id);
		if (m && m->model == modelWingLink)
			return static_cast<WingLink*>(m);
		// (the chosen one is gone: as on Auto)
	}
	// the one it was linked to (also after loading the patch), else the only one
	WingLink* only = nullptr;
	WingLink* cur = s->host.load();
	WingLink* last = nullptr;
	int64_t lastId = s->lastId;
	bool several = false, curThere = false;
	for (int64_t mid : APP->engine->getModuleIds()) {
		Module* m = APP->engine->getModule(mid);
		if (m && m->model == modelWingLink) {
			if (only)
				several = true;
			only = static_cast<WingLink*>(m);
			if (m == cur)
				curThere = true;
			if (mid == lastId)
				last = static_cast<WingLink*>(m);
		}
	}
	if (curThere)
		return cur;
	if (last)
		return last;
	return several ? nullptr : only;  // several and none known: pick one by right-click
}

/** (UI thread, now and then) Link the panels: each finds its WING-LINK, each WING-LINK learns its panels. */
void linkPanels(Satellite* s) {
	WingLink* h = findHost(s);
	WingLink* old = s->host.load();
	if (old != h && hostAlive(old)) {
		if (WingLinkX* x = dynamic_cast<WingLinkX*>(s)) {
			WingLinkX* cur = x;
			old->xMod.compare_exchange_strong(cur, nullptr);
		}
		if (WingFx* f = dynamic_cast<WingFx*>(s)) {
			WingFx* cur = f;
			old->fxMod.compare_exchange_strong(cur, nullptr);
		}
		if (WingEq* q = dynamic_cast<WingEq*>(s)) {
			WingEq* cur = q;
			old->eqMod.compare_exchange_strong(cur, nullptr);
		}
		dropSat(old->dynMod, s);
		dropSat(old->knobsMod, s);
		dropSat(old->inMod, s);
	}
	s->host = h;
	if (!h)
		return;
	s->lastId = h->id;
	if (WingLinkX* x = dynamic_cast<WingLinkX*>(s)) {
		WingLinkX* cur = h->xMod.load();
		if (!cur || cur->host.load() != h)
			h->xMod = x;
	}
	if (WingFx* f = dynamic_cast<WingFx*>(s)) {
		WingFx* cur = h->fxMod.load();
		if (!cur || cur->host.load() != h)
			h->fxMod = f;
	}
	if (WingEq* q = dynamic_cast<WingEq*>(s)) {
		WingEq* cur = h->eqMod.load();
		if (!cur || cur->host.load() != h)
			h->eqMod = q;
	}
	takeSat(h->dynMod, s, h);
	takeSat(h->knobsMod, s, h);
	takeSat(h->inMod, s, h);
}

/** Right-click on a panel: which WING-LINK. */
void appendLinkMenu(Menu* menu, Satellite* s) {
	menu->addChild(new MenuSeparator);
	WingLink* h = s->hostSafe();
	menu->addChild(createSubmenuItem("Connect to WING-LINK", h ? (s->linkId >= 0 ? "chosen" : "auto") : "none",
	                                 [=](Menu* sub) {
		sub->addChild(createCheckMenuItem("Auto (docked, or the only WING-LINK)", "", [=]() { return s->linkId < 0; },
		                                  [=]() { s->linkId = -1; }));
		int n = 0;
		for (int64_t id : APP->engine->getModuleIds()) {
			Module* m = APP->engine->getModule(id);
			if (!m || m->model != modelWingLink)
				continue;
			n++;
			WingLink* w = static_cast<WingLink*>(m);
			sub->addChild(createCheckMenuItem(string::f("WING-LINK #%d  (%s …)", n, P::label(w->wingStart).c_str()), "",
			                                  [=]() { return s->linkId == id; }, [=]() { s->linkId = id; }));
		}
		if (n == 0)
			sub->addChild(createMenuLabel("(no WING-LINK in the patch)"));
	}));
}

// ---------------------------------------------------------------- widgets
struct WingFader : app::SvgSlider {
	WingFader() {
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/wing/fader_bg.svg")));
		setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/wing/fader_handle.svg")));
		float w = mm2px(wllayout::FADER_W), h = mm2px(wllayout::FADER_H), hh = mm2px(wllayout::HANDLE_H);
		setHandlePosCentered(Vec(w / 2, h - hh / 2), Vec(w / 2, hh / 2));
	}
};

template <int R, int G, int B>
using Lit = VCVLightBezelLatch<ColorLight<R, G, B>>;

static NVGcolor colourOf(int c) {
	if (c < 1 || c > P::NUM_COLOURS)
		return nvgRGB(0x4a, 0x4c, 0x57);
	const P::Colour& k = P::COLOURS[c - 1];
	return nvgRGB(k.r, k.g, k.b);
}

struct NameField : DisplayTextField {
	std::function<WingLink*()> getHost;
	int s = 0;
	NameField() {
		fontPath = asset::system("res/fonts/Nunito-Bold.ttf");
		fontSize = 10.f;
		placeholder = "name";
	}
	void step() override {
		WingLink* h = getHost ? getHost() : nullptr;
		if (h && APP->event->selectedWidget != this) {
			const std::string& n = h->getMetaUi(s).name;
			if (n != text)
				setText(n);
		}
		// long names smaller, so they fit beside the icon
		float avail = box.size.x - 2.f * textOffset.x - 2.f;
		size_t len = std::max<size_t>(1, text.size());
		fontSize = clamp(avail / (0.56f * len), 6.5f, 10.f);
		DisplayTextField::step();
	}
	void onAction(const ActionEvent& e) override {
		// Enter: send now, leave the field
		if (WingLink* h = getHost ? getHost() : nullptr)
			h->nameNow(s);
		APP->event->setSelectedWidget(NULL);
		e.consume(this);
	}
	void onChange(const ChangeEvent& e) override {
		std::string t;
		for (char c : getText())
			if ((unsigned char) c >= 0x20 && (unsigned char) c < 0x7f && c != '"')
				t += c;
		if (t.size() > 16)
			t.resize(16);
		if (t != getText()) {
			setText(t);
			return;
		}
		if (WingLink* h = getHost ? getHost() : nullptr)
			h->setName(s, t);
		DisplayTextField::onChange(e);
	}
};

/** Our own icon for each of the WING's icon groups (GENERAL, VOX, DRUMS, STRINGS, KEYS, SPEAKERS, SPECIAL). */
static void drawIcon(NVGcontext* vg, int group, float cx, float cy, float r, NVGcolor c) {
	nvgStrokeColor(vg, c);
	nvgFillColor(vg, c);
	nvgStrokeWidth(vg, std::max(1.f, r * 0.13f));
	nvgLineCap(vg, NVG_ROUND);
	nvgLineJoin(vg, NVG_ROUND);
	nvgBeginPath(vg);
	switch (group) {
		case 0:  // a knob
			nvgCircle(vg, cx, cy, r * 0.8f);
			nvgMoveTo(vg, cx, cy - r * 0.8f);
			nvgLineTo(vg, cx, cy - r * 0.35f);
			nvgStroke(vg);
			break;
		case 1:  // a microphone
			nvgRoundedRect(vg, cx - r * 0.32f, cy - r, r * 0.64f, r * 1.05f, r * 0.32f);
			nvgMoveTo(vg, cx - r * 0.6f, cy - r * 0.15f);
			nvgBezierTo(vg, cx - r * 0.6f, cy + r * 0.5f, cx + r * 0.6f, cy + r * 0.5f, cx + r * 0.6f, cy - r * 0.15f);
			nvgMoveTo(vg, cx, cy + r * 0.38f);
			nvgLineTo(vg, cx, cy + r * 0.85f);
			nvgMoveTo(vg, cx - r * 0.35f, cy + r * 0.85f);
			nvgLineTo(vg, cx + r * 0.35f, cy + r * 0.85f);
			nvgStroke(vg);
			break;
		case 2:  // a drum
			nvgEllipse(vg, cx, cy - r * 0.35f, r * 0.85f, r * 0.3f);
			nvgMoveTo(vg, cx - r * 0.85f, cy - r * 0.35f);
			nvgLineTo(vg, cx - r * 0.85f, cy + r * 0.45f);
			nvgBezierTo(vg, cx - r * 0.4f, cy + r * 0.9f, cx + r * 0.4f, cy + r * 0.9f, cx + r * 0.85f, cy + r * 0.45f);
			nvgLineTo(vg, cx + r * 0.85f, cy - r * 0.35f);
			nvgMoveTo(vg, cx - r * 0.45f, cy - r);
			nvgLineTo(vg, cx + r * 0.05f, cy - r * 0.5f);
			nvgMoveTo(vg, cx + r * 0.45f, cy - r);
			nvgLineTo(vg, cx - r * 0.05f, cy - r * 0.5f);
			nvgStroke(vg);
			break;
		case 3:  // a guitar / bass
			nvgMoveTo(vg, cx, cy - r);
			nvgLineTo(vg, cx, cy - r * 0.35f);
			nvgMoveTo(vg, cx - r * 0.25f, cy - r * 0.35f);
			nvgBezierTo(vg, cx - r * 0.7f, cy - r * 0.35f, cx - r * 0.45f, cy + r * 0.1f, cx - r * 0.65f, cy + r * 0.45f);
			nvgBezierTo(vg, cx - r * 0.75f, cy + r, cx + r * 0.75f, cy + r, cx + r * 0.65f, cy + r * 0.45f);
			nvgBezierTo(vg, cx + r * 0.45f, cy + r * 0.1f, cx + r * 0.7f, cy - r * 0.35f, cx + r * 0.25f, cy - r * 0.35f);
			nvgClosePath(vg);
			nvgStroke(vg);
			break;
		case 4: {  // keys
			nvgRect(vg, cx - r, cy - r * 0.65f, r * 2.f, r * 1.3f);
			for (int i = 1; i < 4; i++) {
				nvgMoveTo(vg, cx - r + i * r * 0.5f, cy - r * 0.65f);
				nvgLineTo(vg, cx - r + i * r * 0.5f, cy + r * 0.65f);
			}
			nvgStroke(vg);
			for (int i = 1; i < 4; i++) {
				nvgBeginPath(vg);
				nvgRect(vg, cx - r + i * r * 0.5f - r * 0.13f, cy - r * 0.65f, r * 0.26f, r * 0.75f);
				nvgFill(vg);
			}
			break;
		}
		case 5:  // a speaker
			nvgRoundedRect(vg, cx - r * 0.6f, cy - r, r * 1.2f, r * 2.f, r * 0.12f);
			nvgCircle(vg, cx, cy - r * 0.5f, r * 0.2f);
			nvgCircle(vg, cx, cy + r * 0.35f, r * 0.4f);
			nvgStroke(vg);
			break;
		default: {  // a star
			for (int i = 0; i < 10; i++) {
				float a = -1.5707963f + i * 0.62831853f, rr = (i % 2) ? r * 0.42f : r;
				if (i == 0)
					nvgMoveTo(vg, cx + rr * std::cos(a), cy + rr * std::sin(a));
				else
					nvgLineTo(vg, cx + rr * std::cos(a), cy + rr * std::sin(a));
			}
			nvgClosePath(vg);
			nvgStroke(vg);
		}
	}
}

static int iconGroup(int icon) {
	for (int g = 0; g < P::NUM_ICON_GROUPS; g++)
		if (icon >= P::ICONS[g].base && icon < P::ICONS[g].base + P::ICONS[g].count)
			return g;
	return -1;
}

/** The strip's display: colour bar (click: next colour), name, icon (click: next group), what it controls.
 * Right-click: what it controls (WING channel), all colours and icons. */
// the strip display: the icon big at the left, the name beside it
static const float ICON_X = 2.9f, ICON_Y = 5.6f, ICON_R = 2.0f, ICON_W = 5.6f;

struct ScribbleWidget : OpaqueWidget {
	std::function<WingLink*()> getHost;
	int s = 0;
	void draw(const DrawArgs& args) override {}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			WingLink* h = getHost ? getHost() : nullptr;
			NVGcontext* vg = args.vg;
			int col = 0, icon = -1;
			P::Tgt t;
			bool fixed = false;
			if (h) {
				const Meta& m = h->getMetaUi(s);
				col = m.colour;
				icon = m.icon;
				t = h->target(s);
				fixed = h->st[s].tType > 0;
			}
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, mm2px(1.3f), 1.5f);
			nvgFillColor(vg, colourOf(col));
			nvgFill(vg);
			float cx = box.size.x / 2;
			int g = iconGroup(icon);
			if (g >= 0)
				drawIcon(vg, g, mm2px(ICON_X), mm2px(ICON_Y), mm2px(ICON_R), TXT);
			else if (h)
				drawIcon(vg, 0, mm2px(ICON_X), mm2px(ICON_Y), mm2px(ICON_R), FAINT);
			std::string ch = h ? P::tgtLabel(t) : "CH 1";
			text(vg, boldFont(), cx, mm2px(10.4f), fixed ? ch + " •" : ch, 7.f, fixed ? TXT : FAINT, NVG_ALIGN_CENTER, 0.3f);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		WingLink* h = getHost ? getHost() : nullptr;
		if (!h || e.action != GLFW_PRESS) {
			OpaqueWidget::onButton(e);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			Meta m = h->getMeta(s);
			int col = m.colour, icon = m.icon;
			if (e.pos.y < mm2px(2.6f))
				h->setColour(s, col % P::NUM_COLOURS + 1);
			else if (e.pos.x < mm2px(ICON_W) && e.pos.y < mm2px(9.f))
				h->setIcon(s, P::nextIconGroup(icon));
			e.consume(this);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int ss = s;
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Strip %d", s + 1)));
			menu->addChild(createSubmenuItem("WING channel", P::tgtLabel(h->target(s)), [=](ui::Menu* sub) {
				sub->addChild(createCheckMenuItem("Auto (follows START CH)", "", [=]() { return h->st[ss].tType == 0; },
				                                  [=]() { h->setTarget(ss, P::Tgt()); }));
				sub->addChild(new MenuSeparator);
				for (int ty = P::T_CH; ty < P::NUM_TGT; ty++)
					sub->addChild(createSubmenuItem(P::TGT_NAME[ty], "", [=](ui::Menu* sub2) {
						for (int n = 1; n <= P::TGT_COUNT[ty]; n++)
							sub2->addChild(createCheckMenuItem(P::tgtLabel(P::tgt(ty, n)), "",
							                                   [=]() { return h->st[ss].tType == ty && h->st[ss].tNum == n; },
							                                   [=]() { h->setTarget(ss, P::tgt(ty, n)); }));
					}));
				sub->addChild(new MenuSeparator);
				sub->addChild(createMenuLabel("A fixed strip stays when START CH moves (• in its display)"));
				sub->addChild(createMenuLabel("Bus, main, matrix: no FX sends · DCA: fader, mute, name, colour"));
			}));
			menu->addChild(createSubmenuItem("Colour", "", [=](ui::Menu* sub) {
				sub->addChild(createMenuItem("None (the WING keeps its own)", "", [=]() { h->setColour(ss, 0); }));
				for (int c = 1; c <= P::NUM_COLOURS; c++)
					sub->addChild(createMenuItem(string::f("%d  %s", c, P::COLOURS[c - 1].name), "",
					                             [=]() { h->setColour(ss, c); }));
			}));
			menu->addChild(createSubmenuItem("Icon", "", [=](ui::Menu* sub) {
				sub->addChild(createMenuItem("None (the WING keeps its own)", "", [=]() { h->setIcon(ss, -1); }));
				for (int g = 0; g < P::NUM_ICON_GROUPS; g++) {
					const P::IconGroup& grp = P::ICONS[g];
					sub->addChild(createSubmenuItem(grp.name, "", [=](ui::Menu* sub2) {
						for (int i = 0; i < grp.count; i++)
							sub2->addChild(createMenuItem(string::f("%s %d  (WING icon %d)", grp.tag, i, grp.base + i), "",
							                              [=]() { h->setIcon(ss, grp.base + i); }));
					}));
				}
			}));
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

/** Over a knob or button that means nothing for what the strip controls: greyed out. */
struct GreyWidget : TransparentWidget {
	std::function<WingLink*()> getHost;
	int s = 0, f = 0;
	void draw(const DrawArgs& args) override {
		WingLink* h = getHost ? getHost() : nullptr;
		if (!h || h->has(s, f))
			return;
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, box.size.x / 2, box.size.y / 2, box.size.x / 2);
		nvgFillColor(args.vg, nvgRGBA(0x21, 0x23, 0x29, 0xd0));
		nvgFill(args.vg);
	}
};

/** M / S on the mute and solo buttons. */
struct LetterWidget : TransparentWidget {
	std::string letter;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			text(args.vg, boldFont(), box.size.x / 2, box.size.y / 2 + 3.6f, letter, 10.f, nvgRGBA(0xf2, 0xf3, 0xf7, 0xe0),
			     NVG_ALIGN_CENTER, 0.f);
		Widget::drawLayer(args, layer);
	}
};

/** Under FX 1 / FX 2 in the label column: the bus the send goes to. */
struct BusLabel : Widget {
	std::function<WingLink*()> getHost;
	int k = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			WingLink* h = getHost ? getHost() : nullptr;
			text(args.vg, boldFont(), box.size.x, mm2px(1.8f), string::f("\xe2\x80\xba BUS %d", h ? hostFxBus(h, k) : k + 1),
			     6.f, ACCENT, NVG_ALIGN_RIGHT, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

struct MeterWidget : Widget {
	std::function<WingLink*()> getHost;
	int s = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			WingLink* h = getHost ? getHost() : nullptr;
			float env = h ? h->st[s].env : 0.f;
			float db = env > 1e-6f ? 20.f * std::log10(env) : -100.f;
			int lit = (int) std::floor((db + 48.f) / 4.5f);
			float seg = box.size.y / 12.f;
			for (int k = 0; k < 12; k++) {
				NVGcolor c = k >= 11 ? BAD : (k >= 9 ? WARN : nvgRGB(0x5d, 0xff, 0x8a));
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, mm2px(0.15f), box.size.y - (k + 1) * seg + 0.8f, box.size.x - mm2px(0.3f),
				               seg - 1.6f, 0.6f);
				nvgFillColor(args.vg, k < lit ? c : OFF);
				nvgFill(args.vg);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

static void addStrips(ModuleWidget* w, Module* module, int base, std::function<WingLink*()> getHost) {
	const float* fader[NS] = {L::FADER1, L::FADER2, L::FADER3, L::FADER4, L::FADER5, L::FADER6, L::FADER7, L::FADER8};
	const float* pan[NS] = {L::PAN1, L::PAN2, L::PAN3, L::PAN4, L::PAN5, L::PAN6, L::PAN7, L::PAN8};
	const float* fx1[NS] = {L::FX1_1, L::FX1_2, L::FX1_3, L::FX1_4, L::FX1_5, L::FX1_6, L::FX1_7, L::FX1_8};
	const float* fx2[NS] = {L::FX2_1, L::FX2_2, L::FX2_3, L::FX2_4, L::FX2_5, L::FX2_6, L::FX2_7, L::FX2_8};
	const float* mute[NS] = {L::MUTE1, L::MUTE2, L::MUTE3, L::MUTE4, L::MUTE5, L::MUTE6, L::MUTE7, L::MUTE8};
	const float* solo[NS] = {L::SOLO1, L::SOLO2, L::SOLO3, L::SOLO4, L::SOLO5, L::SOLO6, L::SOLO7, L::SOLO8};
	const float* scrib[NS] = {L::SCRIB1, L::SCRIB2, L::SCRIB3, L::SCRIB4, L::SCRIB5, L::SCRIB6, L::SCRIB7, L::SCRIB8};
	const float* vu[NS] = {L::VU1, L::VU2, L::VU3, L::VU4, L::VU5, L::VU6, L::VU7, L::VU8};
	const float* audio[NS] = {L::AUDIO1, L::AUDIO2, L::AUDIO3, L::AUDIO4, L::AUDIO5, L::AUDIO6, L::AUDIO7, L::AUDIO8};
	const float* cvf[NS] = {L::CV_FDR1, L::CV_FDR2, L::CV_FDR3, L::CV_FDR4, L::CV_FDR5, L::CV_FDR6, L::CV_FDR7, L::CV_FDR8};
	const float* cvp[NS] = {L::CV_PAN1, L::CV_PAN2, L::CV_PAN3, L::CV_PAN4, L::CV_PAN5, L::CV_PAN6, L::CV_PAN7, L::CV_PAN8};
	const float* cv1[NS] = {L::CV_FX1_1, L::CV_FX1_2, L::CV_FX1_3, L::CV_FX1_4, L::CV_FX1_5, L::CV_FX1_6, L::CV_FX1_7, L::CV_FX1_8};
	const float* cv2[NS] = {L::CV_FX2_1, L::CV_FX2_2, L::CV_FX2_3, L::CV_FX2_4, L::CV_FX2_5, L::CV_FX2_6, L::CV_FX2_7, L::CV_FX2_8};
	const float* of[NS] = {L::OUT_FDR1, L::OUT_FDR2, L::OUT_FDR3, L::OUT_FDR4, L::OUT_FDR5, L::OUT_FDR6, L::OUT_FDR7, L::OUT_FDR8};
	const float* om[NS] = {L::OUT_MUTE1, L::OUT_MUTE2, L::OUT_MUTE3, L::OUT_MUTE4, L::OUT_MUTE5, L::OUT_MUTE6, L::OUT_MUTE7, L::OUT_MUTE8};
	for (int j = 0; j < NS; j++) {
		w->addParam(createParamCentered<WingFader>(mm(fader[j]), module, pid(j, FADER)));
		w->addParam(createParamCentered<GSmallKnob>(mm(pan[j]), module, pid(j, PAN)));
		w->addParam(createParamCentered<GSmallKnob>(mm(fx1[j]), module, pid(j, FX1)));
		w->addParam(createParamCentered<GSmallKnob>(mm(fx2[j]), module, pid(j, FX2)));
		w->addParam(createLightParamCentered<Lit<0xff, 0x6b, 0x5b>>(mm(mute[j]), module, pid(j, MUTE), j));
		w->addParam(createLightParamCentered<Lit<0xff, 0xd8, 0x4a>>(mm(solo[j]), module, pid(j, SOLO), NS + j));
		w->addInput(createInputCentered<PJ301MPort>(mm(audio[j]), module, iid(j, AUDIO)));
		w->addInput(createInputCentered<PJ301MPort>(mm(cvf[j]), module, iid(j, CV_FDR)));
		w->addInput(createInputCentered<PJ301MPort>(mm(cvp[j]), module, iid(j, CV_PAN)));
		w->addInput(createInputCentered<PJ301MPort>(mm(cv1[j]), module, iid(j, CV_FX1)));
		w->addInput(createInputCentered<PJ301MPort>(mm(cv2[j]), module, iid(j, CV_FX2)));
		w->addOutput(createOutputCentered<PJ301MPort>(mm(of[j]), module, oid(j, OUT_FDR)));
		w->addOutput(createOutputCentered<PJ301MPort>(mm(om[j]), module, oid(j, OUT_MUTE)));

		ScribbleWidget* sc = new ScribbleWidget;
		sc->box.pos = mm2px(Vec(scrib[j][0], scrib[j][1]));
		sc->box.size = mm2px(Vec(scrib[j][2], scrib[j][3]));
		sc->s = base + j;
		sc->getHost = getHost;
		w->addChild(sc);
		NameField* nf = new NameField;
		nf->box.pos = mm2px(Vec(scrib[j][0] + 5.4f, scrib[j][1] + 3.4f));
		nf->box.size = mm2px(Vec(scrib[j][2] - 5.8f, 4.4f));
		nf->s = base + j;
		nf->getHost = getHost;
		w->addChild(nf);
		MeterWidget* mw = new MeterWidget;
		mw->box.pos = mm2px(Vec(vu[j][0], vu[j][1]));
		mw->box.size = mm2px(Vec(vu[j][2], vu[j][3]));
		mw->s = base + j;
		mw->getHost = getHost;
		w->addChild(mw);
		// greyed when it means nothing for what the strip controls
		auto grey = [&](const float* p, int f, float r) {
			GreyWidget* g = new GreyWidget;
			g->box.size = mm2px(Vec(2 * r, 2 * r));
			g->box.pos = mm(p).minus(g->box.size.div(2));
			g->s = base + j;
			g->f = f;
			g->getHost = getHost;
			w->addChild(g);
		};
		grey(pan[j], PAN, 4.4f);
		grey(fx1[j], FX1, 4.4f);
		grey(fx2[j], FX2, 4.4f);
		grey(solo[j], SOLO, 4.0f);
		for (int k = 0; k < 2; k++) {
			LetterWidget* l = new LetterWidget;
			l->letter = k ? "S" : "M";
			l->box.size = mm2px(Vec(6.f, 6.f));
			l->box.pos = mm(k ? solo[j] : mute[j]).minus(l->box.size.div(2));
			w->addChild(l);
		}
	}
	const float* bl[2] = {L::BUSLBL1, L::BUSLBL2};
	for (int k = 0; k < 2; k++) {
		BusLabel* b = new BusLabel;
		b->box.pos = mm2px(Vec(bl[k][0], bl[k][1]));
		b->box.size = mm2px(Vec(bl[k][2], bl[k][3]));
		b->k = k;
		b->getHost = getHost;
		w->addChild(b);
	}
}

void addScrews(ModuleWidget* w) {
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
}

struct StatusDisplay : Widget {
	WingLink* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float x = mm2px(3.f);
			if (!module) {
				text(vg, boldFont(), x, mm2px(4.6f), "CH 1 – 8", 12.f, WHITE, NVG_ALIGN_LEFT, 0.5f);
				Widget::drawLayer(args, layer);
				return;
			}
			int a = module->wingStart, b = std::min(P::NUM_IN, a + NS - 1);
			std::string chs = P::label(a) + " – " + P::label(b);
			if (module->xMod.load() && a + NS <= P::NUM_IN)
				chs += "  +  " + P::label(a + NS) + " – " + P::label(std::min(P::NUM_IN, a + NT - 1));
			text(vg, boldFont(), x, mm2px(4.6f), chs, 11.f, WHITE, NVG_ALIGN_LEFT, 0.4f);
			std::string stx;
			NVGcolor c;
			if (module->midiOut.getDeviceId() < 0) {
				stx = "●  NO WING (MIDI) · right-click: MIDI port";
				c = BAD;
			}
			else if (module->sysexOk) {
				stx = "●  WING · MIDI OK · SYSEX OK";
				if (module->fxMod.load())
					stx += " · FX";
				if (module->eqMod.load())
					stx += " · EQ";
				if (module->dynMod.load())
					stx += " · DYN";
				if (module->knobsMod.load())
					stx += " · KNOBS";
				if (module->inMod.load())
					stx += " · IN";
				c = OK;
				std::lock_guard<std::mutex> lock(module->errMutex);
				if (module->errorAge < 20.f) {
					stx = "●  WING: " + module->lastError;
					c = WARN;
				}
				else if (module->noAnswer >= 5) {
					stx = "●  WING: NO ANSWERS TO QUESTIONS";
					c = WARN;
				}
				else if (module->routeWrong) {
					stx = "●  WING ROUTING ≠ RACK · PRESS SYNC";
					c = WARN;
				}
				if (module->logLeft > 0.f)
					stx = string::f("●  LOGGING MIDI … %.0f s", module->logLeft.load()), c = ACCENT;
			}
			else if (module->midiSeen) {
				stx = "●  NO SYSEX: WING › SETUP › REMOTE › MIDI";
				c = WARN;
			}
			else {
				stx = "●  WAITING FOR THE WING … (SETUP › REMOTE)";
				c = WARN;
			}
			text(vg, boldFont(), x, mm2px(8.1f), stx.substr(0, 56), 6.4f, c, NVG_ALIGN_LEFT, 0.3f);
			int u = module->usbStart();
			std::string au;
			int nOut = module->port.deviceNumOutputs;
			if (module->port.getDeviceId() < 0)
				au = "AUDIO: none · right-click › Audio to the WING";
			else if (nOut <= 0)
				au = string::f("AUDIO: no USB %d on the device (48 USB ch.)", u);
			else if (module->smallDevice()) {
				// the DAW (Rack as a plugin): its outputs 1–16; the DAW routes them to the WING's USB channels
				int n = std::min(module->xMod.load() ? NT : NS, nOut);
				const std::string& dn = module->driverName;
				au = string::f("AUDIO → %s 1–%d (to WING USB %d–%d IN)", dn.find("DAW") != std::string::npos ? "DAW" : "OUT", n, u,
				               u + n - 1);
			}
			else
				au = string::f("AUDIO → USB %d–%d IN · %.0f kHz · %s", u, u + (module->xMod.load() ? NT : NS) - 1,
				               module->port.deviceSampleRate / 1000.f, module->audioName.c_str());
			text(vg, boldFont(), x, mm2px(10.2f), au.substr(0, 52), 6.f, DIM, NVG_ALIGN_LEFT, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** A small display with a number; a click switches between two values. */
struct StepWidget : OpaqueWidget {
	std::atomic<int>* value = nullptr;
	int a = 1, b = 8;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			text(args.vg, boldFont(), box.size.x / 2, box.size.y / 2 + 5.f, string::f("%d", value ? value->load() : a), 14.f,
			     WHITE, NVG_ALIGN_CENTER, 0.f);
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (value && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			*value = value->load() == a ? b : a;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

/** An SD card of the WING-LIVE recorder: lit while it records; a click starts or stops it. */
struct SdWidget : OpaqueWidget {
	WingLink* module = nullptr;
	int c = 0;
	static std::string hms(float ms, bool hours) {
		int t = (int) (ms / 1000.f);
		return hours ? string::f("%d:%02d h", t / 3600, (t / 60) % 60) : string::f("%d:%02d", t / 60, t % 60);
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			int state = module ? module->sd[c].state.load() : (c == 0 ? 3 : 0);
			int card = module ? module->sd[c].card.load() : 1;
			bool rec = state == 3;
			float w = box.size.x, h = box.size.y, cut = mm2px(2.6f), r = mm2px(0.6f);
			nvgBeginPath(vg);
			nvgMoveTo(vg, r, 0);
			nvgLineTo(vg, w - cut, 0);
			nvgLineTo(vg, w, cut);
			nvgLineTo(vg, w, h - r);
			nvgQuadTo(vg, w, h, w - r, h);
			nvgLineTo(vg, r, h);
			nvgQuadTo(vg, 0, h, 0, h - r);
			nvgLineTo(vg, 0, r);
			nvgQuadTo(vg, 0, 0, r, 0);
			nvgFillColor(vg, nvgRGB(0x2a, 0x2c, 0x33));
			nvgFill(vg);
			nvgStrokeColor(vg, rec ? BAD : nvgRGB(0x4a, 0x4c, 0x57));
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
			for (int q = 0; q < 4; q++) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, mm2px(1.0f + q * 1.35f), mm2px(1.0f), mm2px(0.9f), mm2px(1.8f), 0.6f);
				nvgFillColor(vg, nvgRGB(0x4a, 0x4c, 0x57));
				nvgFill(vg);
			}
			nvgBeginPath(vg);
			nvgCircle(vg, w / 2, mm2px(6.f), mm2px(1.7f));
			nvgFillColor(vg, rec ? BAD : (state == 1 ? WARN : nvgRGB(0x3a, 0x3c, 0x44)));
			nvgFill(vg);
			std::string t;
			NVGcolor tc = TXT;
			if (rec) {
				t = module ? hms(module->sd[c].etime, false) : "12:04";
				tc = BAD;
			}
			else if (card == 0)
				t = "NO CARD", tc = FAINT;
			else if (card >= 2)
				t = card == 2 ? "LOCKED" : "ERROR", tc = WARN;
			else if (card == 1)
				t = module ? hms(module->sd[c].sdfree, true) : "3:05 h";
			else
				t = "–", tc = FAINT;
			text(vg, boldFont(), w / 2, mm2px(9.6f), t, 5.4f, tc, NVG_ALIGN_CENTER, 0.2f);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			module->sd[c].toggle = true;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

/** 8 or 16 tracks for the SD recorder: two lamps. */
struct TracksWidget : app::ParamWidget {
	float dx = 0.f;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			int v = getParamQuantity() ? (int) std::round(getParamQuantity()->getValue()) : 0;
			for (int i = 0; i < 2; i++) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, box.size.y / 2 + i * dx, box.size.y / 2, mm2px(1.35f));
				nvgFillColor(args.vg, i == v ? ACCENT : nvgRGB(0x0d, 0x0e, 0x11));
				nvgFill(args.vg);
			}
		}
		ParamWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (engine::ParamQuantity* pq = getParamQuantity())
				pq->setValue(e.pos.x > box.size.y / 2 + dx / 2 ? 1.f : 0.f);
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}
};

static int scoreMidi(const std::string& n0) {
	std::string n = string::uppercase(n0);
	if (n.find("WING") == std::string::npos)
		return -1;
	int s = 1;
	if (n.find("DAW 2") != std::string::npos || n.find("PORT 2") != std::string::npos ||
	    n.find("MIDI 2") != std::string::npos)
		s += 4;
	if (n.find("CONTROL") != std::string::npos)
		s += 3;
	return s;
}

struct WingLinkWidget : ModuleWidget {
	float scan = 0.f;
	bool audioTried = false;

	WingLinkWidget(WingLink* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingLink.svg")));
		addScrews(this);
		addParam(createParamCentered<VCVBezel>(mm(L::DOWN), module, WingLink::DOWN_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::UP), module, WingLink::UP_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<ColorLight<0xff, 0x6b, 0x5b>>>(mm(L::SYNC), module,
		                                                                               WingLink::SYNC_PARAM,
		                                                                               WingLink::SYNC_LIGHT));
		StatusDisplay* d = new StatusDisplay;
		d->module = module;
		d->box.pos = mm2px(Vec(L::DISPLAY[0], L::DISPLAY[1]));
		d->box.size = mm2px(Vec(L::DISPLAY[2], L::DISPLAY[3]));
		addChild(d);
		StepWidget* sw = new StepWidget;
		sw->value = module ? &module->chStep : nullptr;
		sw->a = 1;
		sw->b = 8;
		sw->box.pos = mm2px(Vec(L::STEP[0], L::STEP[1]));
		sw->box.size = mm2px(Vec(L::STEP[2], L::STEP[3]));
		addChild(sw);
		const float* sdp[2] = {L::SD1, L::SD2};
		for (int c = 0; c < 2; c++) {
			SdWidget* w = new SdWidget;
			w->module = module;
			w->c = c;
			w->box.pos = mm2px(Vec(sdp[c][0], sdp[c][1]));
			w->box.size = mm2px(Vec(sdp[c][2], sdp[c][3]));
			addChild(w);
		}
		TracksWidget* tw = createParam<TracksWidget>(Vec(0, 0), module, WingLink::TRACKS_PARAM);
		tw->dx = mm2px(L::TRACKS[2]);
		tw->box.size = mm2px(Vec(L::TRACKS[2] + 2.8f, 2.8f));
		tw->box.pos = mm2px(Vec(L::TRACKS[0] - 1.4f, L::TRACKS[1] - 1.4f));
		addParam(tw);
		addStrips(this, module, 0, [module]() { return module; });
	}

	void step() override {
		if (WingLink* m0 = dynamic_cast<WingLink*>(module))
			m0->uiFrame++;  // (a new frame: the strips fetch their names, colours, icons once)
		ModuleWidget::step();
		WingLink* m = dynamic_cast<WingLink*>(module);
		if (!m)
			return;
		// the MIDI log: write what the engine noted
		if (m->logLeft > 0.f || m->logPending) {
			float left = m->logLeft - frameDt();
			m->logLeft = std::max(0.f, left);
			std::vector<std::string> lines;
			{
				std::lock_guard<std::mutex> lock(m->logMutex);
				lines.swap(m->logLines);
				m->logPending = false;
			}
			if (!lines.empty() || left <= 0.f) {
				if (FILE* f = std::fopen(m->logPath.c_str(), "a")) {
					for (const std::string& l : lines)
						std::fprintf(f, "%s\n", l.c_str());
					if (left <= 0.f)
						std::fprintf(f, "\n(end of log)\n");
					std::fclose(f);
				}
			}
		}
		scan -= frameDt();
		if (scan > 0.f)
			return;
		scan = 2.f;
		m->audioName = m->port.getDeviceId() >= 0 ? m->port.getDeviceName(m->port.getDeviceId()) : "";
		m->driverName = m->port.getDriverName();
		// panels that no longer point here
		if (WingLinkX* x = m->xMod.load())
			if (x->host.load() != m) {
				WingLinkX* cur = x;
				m->xMod.compare_exchange_strong(cur, nullptr);
			}
		if (WingFx* f = m->fxMod.load())
			if (f->host.load() != m) {
				WingFx* cur = f;
				m->fxMod.compare_exchange_strong(cur, nullptr);
			}
		if (WingEq* q = m->eqMod.load())
			if (q->host.load() != m) {
				WingEq* cur = q;
				m->eqMod.compare_exchange_strong(cur, nullptr);
			}
		staleSat(m->dynMod, m);
		staleSat(m->knobsMod, m);
		staleSat(m->inMod, m);
		if (m->autoMidi) {
			auto pick = [&](midi::Port* port, bool in) {
				int cur = port->getDeviceId();
				if (cur >= 0) {
					std::vector<int> ids = port->getDeviceIds();
					if (std::find(ids.begin(), ids.end(), cur) != ids.end())
						return;
				}
				int bestDrv = -1, bestDev = -1, best = 0;
				for (int drv : midi::getDriverIds()) {
					midi::Driver* d = midi::getDriver(drv);
					if (!d)
						continue;
					std::vector<int> ids = in ? d->getInputDeviceIds() : d->getOutputDeviceIds();
					for (int id : ids) {
						int sc = scoreMidi(in ? d->getInputDeviceName(id) : d->getOutputDeviceName(id));
						if (sc > best) {
							best = sc;
							bestDrv = drv;
							bestDev = id;
						}
					}
				}
				if (bestDrv >= 0) {
					port->setDriverId(bestDrv);
					port->setDeviceId(bestDev);
					port->setChannel(-1);
				}
			};
			pick(&m->midiIn, true);
			pick(&m->midiOut, false);
		}
		if (m->autoAudio && !audioTried && m->port.getDeviceId() < 0) {
			audioTried = true;
			std::vector<int> drivers = audio::getDriverIds();
			std::stable_sort(drivers.begin(), drivers.end(), [](int a, int b) {
				audio::Driver* da = audio::getDriver(a);
				audio::Driver* db = audio::getDriver(b);
				bool aa = da && string::uppercase(da->getName()).find("ASIO") != std::string::npos;
				bool bb = db && string::uppercase(db->getName()).find("ASIO") != std::string::npos;
				return aa && !bb;
			});
			for (int drv : drivers) {
				audio::Driver* d = audio::getDriver(drv);
				if (!d)
					continue;
				for (int id : d->getDeviceIds()) {
					if (string::uppercase(d->getDeviceName(id)).find("WING") != std::string::npos &&
					    d->getDeviceNumOutputs(id) > 0) {
						m->port.setDriverId(drv);
						m->port.setDeviceId(id);
						return;
					}
				}
			}
		}
	}

	void appendContextMenu(Menu* menu) override {
		WingLink* m = dynamic_cast<WingLink*>(module);
		if (!m)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Audio to the WING (USB)", m->audioName.empty() ? "none" : m->audioName,
		                                 [=](Menu* sub) { app::appendAudioMenu(sub, &m->port); }));
		menu->addChild(createSubmenuItem("USB channels", m->usbMode.load() == 0 ? "same as the WING channels" :
		                                 string::f("from USB %d", m->usbMode.load()), [=](Menu* sub) {
			sub->addChild(createCheckMenuItem("Same as the WING channels (CH 1 ← USB 1)", "",
			                                  [=]() { return m->usbMode.load() == 0; }, [=]() { m->usbMode = 0; }));
			for (int u = 1; u <= 41; u += 8)
				sub->addChild(createCheckMenuItem(string::f("From USB %d", u), "", [=]() { return m->usbMode.load() == u; },
				                                  [=]() { m->usbMode = u; }));
			sub->addChild(new MenuSeparator);
			sub->addChild(createMenuLabel("SYNC routes the WING channels to these USB inputs"));
		}));
		menu->addChild(createSubmenuItem("MIDI from the WING", m->midiIn.getDeviceId() >= 0 ?
		                                 m->midiIn.getDeviceName(m->midiIn.getDeviceId()) : "none",
		                                 [=](Menu* sub) { app::appendMidiMenu(sub, &m->midiIn); }));
		menu->addChild(createSubmenuItem("MIDI to the WING", m->midiOut.getDeviceId() >= 0 ?
		                                 m->midiOut.getDeviceName(m->midiOut.getDeviceId()) : "none",
		                                 [=](Menu* sub) { app::appendMidiMenu(sub, &m->midiOut); }));
		menu->addChild(createCheckMenuItem("Find the WING by itself (MIDI)", "", [=]() { return m->autoMidi.load(); },
		                                   [=]() { m->autoMidi = !m->autoMidi; }));
		menu->addChild(createSubmenuItem("START CH steps", string::f("%d", m->chStep.load()), [=](Menu* sub) {
			for (int v : {1, 8})
				sub->addChild(createCheckMenuItem(v == 1 ? "1 (one channel)" : "8 (CH 1–8, 9–16 …)", "",
				                                  [=]() { return m->chStep.load() == v; }, [=]() { m->chStep = v; }));
		}));
		for (int k = 0; k < 2; k++)
			menu->addChild(createSubmenuItem(string::f("FX %d sends to", k + 1), string::f("Bus %d", m->fxBus[k].load()),
			                                 [=](Menu* sub) {
				                                 for (int b = 1; b <= 16; b++)
					                                 sub->addChild(createCheckMenuItem(string::f("Bus %d", b), "",
					                                                                   [=]() { return m->fxBus[k].load() == b; },
					                                                                   [=]() {
						                                                                   m->fxBus[k] = b;
						                                                                   m->wantReset = true;
					                                                                   }));
			                                 }));
		menu->addChild(createMenuItem("Read everything from the WING again", "", [=]() { m->wantReset = true; }));
		menu->addChild(createMenuItem("Log MIDI to file (60 s)", m->logLeft > 0.f ? "logging …" : "", [=]() {
			{
				std::lock_guard<std::mutex> lock(m->logMutex);
				m->logLines.clear();
			}
			m->logPath = asset::user("WingLink-MIDI-log.txt");
			FILE* f = std::fopen(m->logPath.c_str(), "w");
			if (f) {
				std::fprintf(f, "WING-LINK %s  MIDI log  (OUT: to the WING, IN: from the WING)\n", pluginInstance->version.c_str());
				std::fprintf(f, "MIDI in: %s\nMIDI out: %s\nSTART CH %d, audio from CH %d\n\n",
				             m->midiIn.getDeviceId() >= 0 ? m->midiIn.getDeviceName(m->midiIn.getDeviceId()).c_str() : "-",
				             m->midiOut.getDeviceId() >= 0 ? m->midiOut.getDeviceName(m->midiOut.getDeviceId()).c_str() : "-",
				             m->wingStart.load(), m->audioStart.load());
				std::fclose(f);
			}
			m->logLeft = 60.f;
		}));
		if (!m->logPath.empty())
			menu->addChild(createMenuLabel("Log: " + m->logPath));
		std::string id;
		{
			std::lock_guard<std::mutex> lock(m->identMutex);
			id = m->wingIdent;
		}
		if (!id.empty())
			menu->addChild(createMenuLabel(id.substr(0, 60)));
		{
			std::lock_guard<std::mutex> lock(m->errMutex);
			if (!m->lastError.empty())
				menu->addChild(createMenuLabel("Last WING complaint: " + m->lastError.substr(0, 70)));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Click a strip's colour bar or icon to change it, type its name"));
		menu->addChild(createMenuLabel("WING-LINK X (8 more channels) and WING-FX link by themselves"));
	}
};

struct XDisplay : Widget {
	WingLinkX* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float x = mm2px(3.f);
			WingLink* h = module ? module->hostSafe() : nullptr;
			if (!h) {
				text(vg, boldFont(), x, mm2px(4.6f), module ? "NOT LINKED" : "CH 9 – 16", 11.f, module ? WARN : WHITE,
				     NVG_ALIGN_LEFT, 0.4f);
				if (module)
					text(vg, boldFont(), x, mm2px(8.1f), "●  right-click › Connect to WING-LINK", 6.4f, WARN, NVG_ALIGN_LEFT, 0.3f);
			}
			else {
				int a = h->xStartNow();
				std::string chs = a > P::NUM_IN ? "NO CHANNELS LEFT"
				                                : P::label(a) + " – " + P::label(std::min(P::NUM_IN, a + NS - 1));
				text(vg, boldFont(), x, mm2px(4.6f), chs, 11.f, WHITE, NVG_ALIGN_LEFT, 0.4f);
				bool live = h->xMod.load() == module;
				std::string st = !live ? "●  WING-LINK already has a WING-LINK X"
				                       : (module->xStart > 0 ? "●  LINKED · OWN START CH" : "●  LINKED · FOLLOWS WING-LINK");
				text(vg, boldFont(), x, mm2px(8.1f), st, 6.4f, live ? OK : WARN, NVG_ALIGN_LEFT, 0.3f);
				int u = h->usbStart() + NS;
				std::string au = h->smallDevice() ? string::f("AUDIO → %s %d–%d (to WING USB %d–%d IN)",
				                                              h->driverName.find("DAW") != std::string::npos ? "DAW" : "OUT",
				                                              NS + 1, NT, u, u + NS - 1)
				                                  : string::f("AUDIO → USB %d–%d IN", u, u + NS - 1);
				text(vg, boldFont(), x, mm2px(10.2f), au, 6.f, DIM, NVG_ALIGN_LEFT, 0.3f);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

struct WingLinkXWidget : ModuleWidget {
	float scan = 0.f;
	WingLinkXWidget(WingLinkX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingLinkX.svg")));
		addScrews(this);
		XDisplay* d = new XDisplay;
		d->module = module;
		d->box.pos = mm2px(Vec(wllayout::wlx::DISPLAY[0], wllayout::wlx::DISPLAY[1]));
		d->box.size = mm2px(Vec(wllayout::wlx::DISPLAY[2], wllayout::wlx::DISPLAY[3]));
		addChild(d);
		addParam(createParamCentered<VCVBezel>(mm(wllayout::wlx::DOWN), module, WingLinkX::DOWN_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(wllayout::wlx::UP), module, WingLinkX::UP_PARAM));
		StepWidget* sw = new StepWidget;
		sw->value = module ? &module->xStep : nullptr;
		sw->a = 1;
		sw->b = 8;
		sw->box.pos = mm2px(Vec(wllayout::wlx::STEP[0], wllayout::wlx::STEP[1]));
		sw->box.size = mm2px(Vec(wllayout::wlx::STEP[2], wllayout::wlx::STEP[3]));
		addChild(sw);
		addStrips(this, module, NS, [module]() { return module ? module->hostSafe() : nullptr; });
	}
	void step() override {
		ModuleWidget::step();
		scan -= frameDt();
		if (module && scan <= 0.f) {
			scan = 0.5f;
			linkPanels(static_cast<Satellite*>(module));
		}
	}
	void appendContextMenu(Menu* menu) override {
		WingLinkX* m = dynamic_cast<WingLinkX*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(createCheckMenuItem("Follow WING-LINK (its START CH + 8)", "", [=]() { return m->xStart < 0; },
		                                   [=]() {
			                                   m->xStart = -1;
			                                   if (WingLink* h = m->hostSafe())
				                                   h->wantReset = true;
		                                   }));
		menu->addChild(createSubmenuItem("START CH steps", string::f("%d", m->xStep.load()), [=](Menu* sub) {
			for (int v : {1, 8})
				sub->addChild(createCheckMenuItem(v == 1 ? "1 (one channel)" : "8 (CH 1–8, 9–16 …)", "",
				                                  [=]() { return m->xStep.load() == v; }, [=]() { m->xStep = v; }));
		}));
	}
};

// ---------------------------------------------------------------- for WING-FX's panel (src/WingFx.cpp)
FxSlot* hostFx(WingLink* h, int slot) { return h ? &h->fxs[slot] : nullptr; }
int hostFxBus(WingLink* h, int k) { return h ? h->fxBus[k].load() : k + 1; }
void requestFxModel(WingLink* h, int slot, int model) {
	if (h)
		h->fxModelReq[slot] = model;
}
float hostRackBpm(WingLink* h) { return h ? h->rackBpm.load() : 0.f; }
float hostWingBpm(WingLink* h, int* slot) {
	if (!h)
		return 0.f;
	*slot = h->wingBpmSlot;
	return h->wingBpm;
}
bool hostLinked(WingLink* h, WingFx* f) { return h && h->fxMod.load() == f; }

// ---------------------------------------------------------------- for WING-EQ's panel (src/WingEq.cpp)
bool hostLinkedEq(WingLink* h, WingEq* e) { return h && h->eqMod.load() == e; }
bool hostLinkedSat(WingLink* h, Satellite* s) {
	if (!h || !s)
		return false;
	return (Satellite*) h->dynMod.load() == s || (Satellite*) h->knobsMod.load() == s || (Satellite*) h->inMod.load() == s;
}
std::string hostAudioName(WingLink* h) { return h ? h->audioName : ""; }
float hostDeviceRate(WingLink* h) { return h ? h->port.deviceSampleRate : 0.f; }
bool hostSysexOk(WingLink* h) { return h && h->sysexOk; }
Meta hostMetaOf(WingLink* h, int key) {
	if (!h)
		return Meta();
	std::lock_guard<std::mutex> lock(h->metaMutex);
	auto it = h->meta.find(key);
	return it == h->meta.end() ? Meta() : it->second;
}
std::string hostStripLabel(WingLink* h, int s) { return h ? P::tgtLabel(h->target(s)) : ""; }
bool hostHasX(WingLink* h) { return h && h->xMod.load(); }

}  // namespace winglink

Model* modelWingLink = createModel<winglink::WingLink, winglink::WingLinkWidget>("WingLink");
Model* modelWingLinkX = createModel<winglink::WingLinkX, winglink::WingLinkXWidget>("WingLinkX");

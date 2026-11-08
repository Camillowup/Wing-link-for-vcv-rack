// SPDX-License-Identifier: GPL-3.0-only
// WING-LINK – a bridge from VCV Rack to a Behringer WING over one USB cable, in the look of the GENRA plugin.
//
//  * Audio: WING-LINK is an audio port of its own (as Rack's Audio module): its 8 AUDIO inputs, and the expander's 8,
//    go to the WING's USB inputs (by default USB 33… for CH 33…, so they never clash with an Audio module on 1–16).
//  * Control: USB MIDI (the WING's remote-control port, src/WingProto.hpp): faders, pan, mute, solo and two bus sends
//    both ways; name, colour and icon of every channel from the panel; SYNC also routes the channels to their USB
//    inputs. The WING sends faders / mutes / pans as CCs when they move; the rest (and exact values) is asked for.
//  * WING-LINK X docks right of WING-LINK: 8 more channels. WING-LINK does all the work (the expander is its panel).
// Panel: tools/winglink_panel.py (res/WingLink*.svg + src/WingLinkLayout.hpp).
#include "plugin.hpp"
#include "Ui.hpp"
#include "WingProto.hpp"
#include "WingLinkLayout.hpp"
#include <atomic>
#include <deque>
#include <mutex>

namespace winglink {

namespace L = wllayout::wl;
namespace P = wingproto;
using namespace wlui;

static const int NS = 8;        // strips per module
static const int NT = 2 * NS;   // strips with the expander

// ---------------------------------------------------------------- the panel's parameters (both modules)
enum Field { FADER, PAN, FX1, FX2, MUTE, SOLO, NF };
enum ChInput { AUDIO, CV_FDR, CV_PAN, CV_FX1, CV_FX2, NCI };
enum ChOutput { OUT_FDR, OUT_MUTE, NCO };

static int pid(int j, int f) { return j * NF + f; }
static int iid(int j, int k) { return j * NCI + k; }
static int oid(int j, int k) { return j * NCO + k; }

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

/** The strips of one module: 8 × (fader, pan, FX 1, FX 2, mute, solo), the jacks; WING-LINK adds its buttons. */
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
	float deviceSampleRate = 0.f;
	int requestedEngineFrames = 0;

	WingAudioPort(Module* m) : module(m) {
		maxInputs = 0;
		maxOutputs = MAXCH;
		inputSrc.setQuality(6);
	}
	bool isMaster() { return APP->engine->getMasterModule() == module; }

	void processInput(const float* input, int inputStride, int frames) override {
		deviceNumOutputs = std::min(getNumOutputs(), MAXCH);
		deviceSampleRate = getSampleRate();
		if (!APP->engine->getMasterModule())
			APP->engine->setMasterModule(module);
		if (isMaster())
			APP->engine->setSuggestedSampleRate(deviceSampleRate);
		float ratio = APP->engine->getSampleRate() / deviceSampleRate;
		requestedEngineFrames = std::max((int) std::ceil(frames * ratio) - (int) engineInputBuffer.size(), 0);
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
	void onStartStream() override { engineInputBuffer.clear(); }
	void onStopStream() override {
		deviceNumOutputs = 0;
		deviceSampleRate = 0.f;
		engineInputBuffer.clear();
		if (APP->engine->getMasterModule() == module)
			APP->engine->setMasterModule_NoLock(NULL);
	}
};

// ---------------------------------------------------------------- one strip's two-way state
struct Sync {
	float wing = NAN;   // what the WING has (as far as we know)
	float local = NAN;  // what the panel had at the last look
	float hold = 0.f;   // after a change here: the WING's echoes are ignored this long (s)
	bool dirty = false; // to send
};

struct Strip {
	Sync f[NF];
	float peak = 0.f, env = 0.f;
	// name, colour (1..12), icon: set on the panel, sent to the WING (debounced)
	std::string name;
	int colour = 0;  // 0 = not set (the WING keeps its own)
	int icon = -1;   // -1 = not set
	bool metaDirty = false;
	float metaWait = 0.f;
};

struct WingLinkX;

struct WingLink : Module {
	enum ParamId { DOWN_PARAM = NS * NF, UP_PARAM, SYNC_PARAM, NUM_PARAMS };
	enum InputId { NUM_INPUTS = NS * NCI };
	enum OutputId { NUM_OUTPUTS = NS * NCO };
	enum LightId { MUTE_LIGHT, SOLO_LIGHT = MUTE_LIGHT + NS, SYNC_LIGHT = SOLO_LIGHT + NS, NUM_LIGHTS };

	// ---- settings (saved)
	std::atomic<int> wingStart{33};   // the WING input of strip 1 (1..48; 41–48 = AUX 1–8)
	std::atomic<int> usbMode{0};      // 0: USB n = WING channel n, else: the first USB channel
	std::atomic<int> fxBus[2] = {{1}, {2}};
	std::atomic<bool> autoMidi{true}, autoAudio{true};
	Strip st[NT];
	std::mutex metaMutex;  // names (the UI writes them, the engine sends them)

	// ---- connection
	midi::InputQueue midiIn;
	midi::Output midiOut;
	WingAudioPort port;
	std::atomic<bool> sysexOk{false}, midiSeen{false};
	std::string wingIdent;  // the WING's answer to ident (UI shows it)
	std::mutex identMutex;
	int lastInDev = -2, lastOutDev = -2;
	float identTimer = 0.f;
	bool identWaiting = false;

	// one question at a time: (strip, field)
	struct Query {
		int s, f;
	};
	std::deque<Query> queries;
	bool qWaiting = false;
	Query qNow{0, 0};
	float qTimer = 0.f, pollTimer = 0.f;
	std::atomic<bool> pullAll{false}, syncNow{false};
	int sendRR = 0;
	float sendWait = 0.f;
	dsp::ClockDivider ctl;
	dsp::BooleanTrigger downT, upT, syncT;
	float syncFlash = 0.f;
	std::atomic<bool> wantReset{false};  // (the menu asks; the engine does it)
	std::string audioName;               // (UI thread: the audio device's name, looked up now and then)

	WingLink() : port(this) {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configStrips(this, 0);
		configButton(DOWN_PARAM, "Start channel down");
		configButton(UP_PARAM, "Start channel up");
		configButton(SYNC_PARAM, "SYNC: names, colours, icons and USB routing to the WING, then read its faders back");
		ctl.setDivision(32);
		midiOut.setChannel(-1);
		midiIn.setChannel(-1);
		for (int s = 0; s < NT; s++)
			st[s].name = "";
	}
	~WingLink() { port.setDriverId(-1); }

	void onReset() override {
		wingStart = 33;
		usbMode = 0;
		fxBus[0] = 1;
		fxBus[1] = 2;
		std::lock_guard<std::mutex> lock(metaMutex);
		for (int s = 0; s < NT; s++) {
			st[s].name = "";
			st[s].colour = 0;
			st[s].icon = -1;
		}
		port.outputOffset = usbStart() - 1;
	}

	// ---- helpers
	Module* expander() {
		Module* m = rightExpander.module;
		return (m && m->model == modelWingLinkX) ? m : nullptr;
	}
	/** The module and its strip number for strip s (0..15), null if the expander isn't there. */
	Module* owner(int s, int* j) {
		*j = s % NS;
		return s < NS ? this : expander();
	}
	int input(int s) { return wingStart + s; }  // the WING input (1..48), > 48: none
	bool active(int s) {
		int j;
		return input(s) <= P::NUM_IN && owner(s, &j);
	}
	int usbStart() { return usbMode.load() > 0 ? usbMode.load() : wingStart.load(); }
	int usbOf(int s) { return usbStart() + s; }

	void send(const std::vector<uint8_t>& bytes) {
		midi::Message m;
		m.bytes = bytes;
		midiOut.sendMessage(m);
	}
	void sendCc(int ch, int cc, int v) {
		midi::Message m;
		m.bytes = {(uint8_t) (0xB0 | ch), (uint8_t) cc, (uint8_t) clamp(v, 0, 127)};
		midiOut.sendMessage(m);
	}

	/** After the channels changed (start, connect, load): take the panel as it is, ask the WING for its values. */
	void resetSync() {
		for (int s = 0; s < NT; s++) {
			int j;
			Module* m = owner(s, &j);
			for (int f = 0; f < NF; f++) {
				Sync& y = st[s].f[f];
				y.local = m ? m->params[pid(j, f)].getValue() : NAN;
				y.wing = y.local;
				y.dirty = false;
				y.hold = 0.f;
			}
		}
		queries.clear();
		qWaiting = false;
		pullAll = true;
	}

	/** A value from the WING: onto the panel, unless it was just changed here (or a CV drives it). */
	void fromWing(int s, int f, float v) {
		int j;
		Module* m = owner(s, &j);
		if (!m)
			return;
		Sync& y = st[s].f[f];
		if (y.hold > 0.f)
			return;
		y.wing = v;
		static const int CVOF[NF] = {CV_FDR, CV_PAN, CV_FX1, CV_FX2, -1, -1};
		if (CVOF[f] >= 0 && m->inputs[iid(j, CVOF[f])].isConnected())
			return;
		m->params[pid(j, f)].setValue(v);
		y.local = v;
		y.dirty = false;
	}

	// ---- MIDI in
	void onMidi(const midi::Message& msg) {
		if (msg.bytes.empty())
			return;
		if (msg.bytes[0] == 0xF0) {
			std::string text;
			int cmd = P::parse(msg.bytes, &text);
			if (cmd < 0)
				return;
			midiSeen = true;
			if (cmd == P::IDENT_REPLY) {
				sysexOk = true;
				identWaiting = false;
				std::lock_guard<std::mutex> lock(identMutex);
				wingIdent = text;
				return;
			}
			if (cmd == P::GET_REPLY || cmd == P::ERROR_REPLY) {
				sysexOk = true;
				if (qWaiting) {
					qWaiting = false;
					float v;
					if (cmd == P::GET_REPLY && P::number(text, &v)) {
						int f = qNow.f;
						if (f == FADER || f == FX1 || f == FX2)
							v = P::dbToPos(v);
						else if (f == PAN)
							v = clamp(v / 100.f, -1.f, 1.f);
						else
							v = v >= 0.5f ? 1.f : 0.f;
						fromWing(qNow.s, f, v);
					}
				}
			}
			return;
		}
		if (msg.getStatus() != 0xB)
			return;
		midiSeen = true;
		int ch = msg.getChannel(), k = P::inputOfCc(msg.getNote()), val = msg.getValue();
		if (k == 0 || ch > 2)
			return;
		int s = k - wingStart;
		if (s < 0 || s >= NT || !active(s))
			return;
		if (ch == 0) {
			// 7 bit: keep the finer value we have when it rounds to the same step
			float now = st[s].f[FADER].wing;
			float v = val / 127.f;
			if (!std::isnan(now) && (int) std::round(now * 127.f) == val)
				return;
			fromWing(s, FADER, v);
		}
		else if (ch == 1)
			fromWing(s, MUTE, val >= 64 ? 1.f : 0.f);
		else {
			float now = st[s].f[PAN].wing;
			if (!std::isnan(now) && (int) std::round((now + 1.f) * 63.5f) == val)
				return;
			fromWing(s, PAN, val == 64 ? 0.f : clamp((val - 64) / 63.f, -1.f, 1.f));
		}
	}

	// ---- the messages for one field
	void sendField(int s, int f, float v) {
		int k = input(s);
		bool sx = sysexOk;
		switch (f) {
			case FADER:
				if (sx)
					send(P::set(k, "fdr=" + P::dbText(P::posToDb(v))));
				else
					sendCc(0, P::ccOf(k), (int) std::round(v * 127.f));
				break;
			case PAN:
				if (sx)
					send(P::set(k, string::f("pan=%d", (int) std::round(v * 100.f))));
				else
					sendCc(2, P::ccOf(k), (int) std::round((v + 1.f) * 63.5f));
				break;
			case MUTE:
				if (sx)
					send(P::set(k, v > 0.5f ? "mute=1" : "mute=0"));
				else
					sendCc(1, P::ccOf(k), v > 0.5f ? 127 : 0);
				break;
			case SOLO:
				send(P::set(k, v > 0.5f ? "$solo=1" : "$solo=0"));
				break;
			case FX1:
			case FX2:
				send(P::set(k, string::f("send.%d.on=1,lvl=", fxBus[f == FX2].load()) + P::dbText(P::posToDb(v))));
				break;
		}
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
	bool sendMeta(int s, bool route) {
		std::string what;
		{
			std::unique_lock<std::mutex> lock(metaMutex, std::try_to_lock);
			if (!lock.owns_lock())
				return false;
			Strip& t = st[s];
			if (!t.name.empty())
				what += "name=" + P::nameText(t.name);
			if (t.colour > 0)
				what += std::string(what.empty() ? "" : ",") + string::f("col=%d", t.colour);
			if (t.icon >= 0)
				what += std::string(what.empty() ? "" : ",") + string::f("icon=%d", t.icon);
		}
		if (route)
			what += std::string(what.empty() ? "" : ",") + string::f("in.conn.grp=USB,in=%d", usbOf(s));
		if (!what.empty())
			send(P::set(input(s), what));
		return true;
	}

	// ---- the control tick (every 32 samples)
	void control(float dt) {
		midiOut.channel = -1;  // (a pick in the MIDI menu may set one: keep our own channels)
		midiIn.channel = -1;
		port.outputOffset = clamp(usbStart() - 1, 0, 47);

		// connection: on a new device, ask who's there
		int inDev = midiIn.getDeviceId(), outDev = midiOut.getDeviceId();
		if (inDev != lastInDev || outDev != lastOutDev) {
			lastInDev = inDev;
			lastOutDev = outDev;
			sysexOk = false;
			midiSeen = false;
			identTimer = 0.f;
			identWaiting = false;
			resetSync();
		}
		bool connected = outDev >= 0;
		if (connected && !sysexOk) {
			identTimer -= dt;
			if (identTimer <= 0.f) {
				send(P::sysex(P::IDENT, ""));
				identTimer = 2.f;
				identWaiting = true;
			}
		}

		if (wantReset.exchange(false))
			resetSync();
		// the buttons
		if (downT.process(params[DOWN_PARAM].getValue() > 0.f) && wingStart > 1) {
			wingStart--;
			resetSync();
		}
		if (upT.process(params[UP_PARAM].getValue() > 0.f) && wingStart < P::NUM_IN) {
			wingStart++;
			resetSync();
		}
		if (syncT.process(params[SYNC_PARAM].getValue() > 0.f))
			syncNow = true;
		syncFlash = std::max(0.f, syncFlash - dt);
		lights[SYNC_LIGHT].setBrightness(syncFlash > 0.f ? 1.f : 0.f);

		// the strips
		for (int s = 0; s < NT; s++) {
			int j;
			Module* m = owner(s, &j);
			Strip& t = st[s];
			t.env = std::max(t.peak, t.env * std::pow(0.5f, dt / 0.12f));
			t.peak = 0.f;
			if (!m)
				continue;
			bool on = input(s) <= P::NUM_IN;
			// CV moves the panel
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
					if (on && connected && !(std::fabs(v - y.wing) < 1e-4f)) {
						y.dirty = true;
						y.hold = 0.4f;
					}
				}
				y.hold = std::max(0.f, y.hold - dt);
			}
			float fd = m->params[pid(j, FADER)].getValue();
			bool mu = m->params[pid(j, MUTE)].getValue() > 0.5f;
			m->outputs[oid(j, OUT_FDR)].setVoltage(fd * 10.f);
			m->outputs[oid(j, OUT_MUTE)].setVoltage(mu ? 10.f : 0.f);
			if (m == this) {
				lights[MUTE_LIGHT + j].setBrightness(mu ? 1.f : 0.f);
				lights[SOLO_LIGHT + j].setBrightness(m->params[pid(j, SOLO)].getValue() > 0.5f ? 1.f : 0.f);
			}
			else {
				m->lights[j].setBrightness(mu ? 1.f : 0.f);
				m->lights[NS + j].setBrightness(m->params[pid(j, SOLO)].getValue() > 0.5f ? 1.f : 0.f);
			}
			if (t.metaDirty) {
				t.metaWait -= dt;
			}
		}
		if (!connected)
			return;

		// SYNC: names, colours, icons and the USB routing for every strip, then read the WING back
		if (syncNow) {
			bool all = true;
			for (int s = 0; s < NT; s++)
				if (active(s))
					all &= sendMeta(s, true);
			if (all) {
				syncNow = false;
				syncFlash = 0.3f;
				for (int s = 0; s < NT; s++)
					st[s].metaDirty = false;
				resetSync();
			}
			return;
		}

		// send: one message every 3 ms at most (round robin over the strips and fields)
		sendWait -= dt;
		if (sendWait <= 0.f) {
			for (int n = 0; n < NT * (NF + 1); n++) {
				int i = (sendRR + n) % (NT * (NF + 1));
				int s = i / (NF + 1), f = i % (NF + 1);
				if (!active(s))
					continue;
				if (f == NF) {
					if (st[s].metaDirty && st[s].metaWait <= 0.f && sendMeta(s, false)) {
						st[s].metaDirty = false;
						sendRR = i + 1;
						sendWait = 0.003f;
						break;
					}
					continue;
				}
				Sync& y = st[s].f[f];
				if (!y.dirty)
					continue;
				if (f == SOLO && !sysexOk) {
					y.dirty = false;
					continue;
				}
				if ((f == FX1 || f == FX2) && !sysexOk) {
					y.dirty = false;
					continue;
				}
				sendField(s, f, y.local);
				y.wing = y.local;
				y.dirty = false;
				y.hold = 0.4f;
				sendRR = i + 1;
				sendWait = 0.003f;
				break;
			}
		}

		// ask: after a connect / SYNC everything, then solo and the sends now and then (they have no CC)
		if (sysexOk) {
			if (pullAll) {
				pullAll = false;
				queries.clear();
				for (int s = 0; s < NT; s++)
					if (active(s))
						for (int f = 0; f < NF; f++)
							queries.push_back({s, f});
			}
			pollTimer -= dt;
			if (pollTimer <= 0.f && queries.empty()) {
				pollTimer = 1.f;
				for (int s = 0; s < NT; s++)
					if (active(s)) {
						queries.push_back({s, SOLO});
						queries.push_back({s, FX1});
						queries.push_back({s, FX2});
					}
			}
			if (qWaiting) {
				qTimer -= dt;
				if (qTimer <= 0.f)
					qWaiting = false;  // no answer: on with the next
			}
			if (!qWaiting && !queries.empty() && sendWait <= 0.f) {
				qNow = queries.front();
				queries.pop_front();
				if (active(qNow.s)) {
					send(P::get(input(qNow.s), queryOf(qNow.f)));
					qWaiting = true;
					qTimer = 0.25f;
					sendWait = 0.002f;
				}
			}
		}
	}

	void process(const ProcessArgs& args) override {
		midi::Message msg;
		while (midiIn.tryPop(&msg, args.frame))
			onMidi(msg);

		// audio to the WING
		Module* x = expander();
		dsp::Frame<MAXCH> fr = {};
		for (int s = 0; s < NT; s++) {
			Module* m = s < NS ? this : x;
			if (!m)
				break;
			float v = m->inputs[iid(s % NS, AUDIO)].getVoltageSum() / 10.f;
			fr.samples[s] = v;
			st[s].peak = std::max(st[s].peak, std::fabs(v));
		}
		if (port.deviceNumOutputs > 0 && !port.engineInputBuffer.full())
			port.engineInputBuffer.push(fr);

		if (ctl.process())
			control(args.sampleTime * ctl.getDivision());
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override { port.engineInputBuffer.clear(); }

	// ---- the panel's edits (UI thread)
	void setName(int s, const std::string& n) {
		std::lock_guard<std::mutex> lock(metaMutex);
		if (st[s].name == n)
			return;
		st[s].name = n;
		st[s].metaDirty = true;
		st[s].metaWait = 0.6f;
	}
	std::string getName(int s) {
		std::lock_guard<std::mutex> lock(metaMutex);
		return st[s].name;
	}
	void setColour(int s, int c) {
		std::lock_guard<std::mutex> lock(metaMutex);
		st[s].colour = c;
		st[s].metaDirty = true;
		st[s].metaWait = 0.2f;
	}
	void setIcon(int s, int i) {
		std::lock_guard<std::mutex> lock(metaMutex);
		st[s].icon = i;
		st[s].metaDirty = true;
		st[s].metaWait = 0.2f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "wingStart", json_integer(wingStart));
		json_object_set_new(root, "usbMode", json_integer(usbMode));
		json_object_set_new(root, "fxBus1", json_integer(fxBus[0]));
		json_object_set_new(root, "fxBus2", json_integer(fxBus[1]));
		json_object_set_new(root, "autoMidi", json_boolean(autoMidi));
		json_object_set_new(root, "autoAudio", json_boolean(autoAudio));
		json_object_set_new(root, "midiIn", midiIn.toJson());
		json_object_set_new(root, "midiOut", midiOut.toJson());
		json_object_set_new(root, "audio", port.toJson());
		json_t* strips = json_array();
		{
			std::lock_guard<std::mutex> lock(metaMutex);
			for (int s = 0; s < NT; s++) {
				json_t* o = json_object();
				json_object_set_new(o, "name", json_string(st[s].name.c_str()));
				json_object_set_new(o, "colour", json_integer(st[s].colour));
				json_object_set_new(o, "icon", json_integer(st[s].icon));
				json_array_append_new(strips, o);
			}
		}
		json_object_set_new(root, "strips", strips);
		return root;
	}
	void dataFromJson(json_t* root) override {
		auto geti = [&](const char* k, int def) {
			json_t* j = json_object_get(root, k);
			return j ? (int) json_integer_value(j) : def;
		};
		wingStart = clamp(geti("wingStart", 33), 1, P::NUM_IN);
		usbMode = clamp(geti("usbMode", 0), 0, 48);
		fxBus[0] = clamp(geti("fxBus1", 1), 1, 16);
		fxBus[1] = clamp(geti("fxBus2", 2), 1, 16);
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
		port.outputOffset = clamp(usbStart() - 1, 0, 47);
		if (json_t* j = json_object_get(root, "audio"))
			port.fromJson(j);
		if (json_t* a = json_object_get(root, "strips")) {
			std::lock_guard<std::mutex> lock(metaMutex);
			for (int s = 0; s < NT && s < (int) json_array_size(a); s++) {
				json_t* o = json_array_get(a, s);
				if (json_t* j = json_object_get(o, "name"))
					st[s].name = json_string_value(j);
				if (json_t* j = json_object_get(o, "colour"))
					st[s].colour = clamp((int) json_integer_value(j), 0, P::NUM_COLOURS);
				if (json_t* j = json_object_get(o, "icon"))
					st[s].icon = (int) json_integer_value(j);
			}
		}
		lastInDev = lastOutDev = -2;
	}
};

struct WingLinkX : Module {
	enum InputId { NUM_INPUTS = NS * NCI };
	enum OutputId { NUM_OUTPUTS = NS * NCO };
	enum LightId { MUTE_LIGHT, SOLO_LIGHT = MUTE_LIGHT + NS, NUM_LIGHTS = SOLO_LIGHT + NS };
	WingLinkX() {
		config(NS * NF, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configStrips(this, NS);
	}
	WingLink* host() {
		Module* m = leftExpander.module;
		return (m && m->model == modelWingLink) ? static_cast<WingLink*>(m) : nullptr;
	}
	// (WING-LINK does the work: reads this panel, writes its outputs and lights)
};

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

/** The strip's name, typed straight into its display. */
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
			std::string n = h->getName(s);
			if (n != text)
				setText(n);
		}
		DisplayTextField::step();
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

/** The strip's display: colour bar (click: next colour), name, icon (click: next group), WING channel. Right-click:
 * all colours and icons. */
struct ScribbleWidget : OpaqueWidget {
	std::function<WingLink*()> getHost;
	int s = 0;
	void draw(const DrawArgs& args) override {}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			WingLink* h = getHost ? getHost() : nullptr;
			NVGcontext* vg = args.vg;
			int col = 0, icon = -1, k = 0;
			if (h) {
				std::lock_guard<std::mutex> lock(h->metaMutex);
				col = h->st[s].colour;
				icon = h->st[s].icon;
				k = h->input(s);
			}
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, mm2px(1.3f), 1.5f);
			nvgFillColor(vg, colourOf(col));
			nvgFill(vg);
			float cx = box.size.x / 2;
			std::string it = icon >= 0 ? P::iconText(icon) : "no icon";
			text(vg, boldFont(), cx, mm2px(8.4f), it, 7.f, icon >= 0 ? DIM : FAINT, NVG_ALIGN_CENTER, 0.3f);
			std::string ch = !h ? "–" : (k > P::NUM_IN ? "–" : P::label(k));
			text(vg, boldFont(), cx, mm2px(10.4f), ch, 7.f, FAINT, NVG_ALIGN_CENTER, 0.3f);
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
			int col, icon;
			{
				std::lock_guard<std::mutex> lock(h->metaMutex);
				col = h->st[s].colour;
				icon = h->st[s].icon;
			}
			if (e.pos.y < mm2px(2.6f))
				h->setColour(s, col % P::NUM_COLOURS + 1);
			else if (e.pos.y > mm2px(6.6f) && e.pos.y < mm2px(8.9f))
				h->setIcon(s, P::nextIconGroup(icon));
			e.consume(this);
			return;
		}
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int ss = s;
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Strip %d", s + 1)));
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

/** 12 segments, green / yellow from −6 dB / red from 0 dB (of the signal going to the WING). */
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
		nf->box.pos = mm2px(Vec(scrib[j][0] + 0.4f, scrib[j][1] + 2.2f));
		nf->box.size = mm2px(Vec(scrib[j][2] - 0.8f, 4.2f));
		nf->s = base + j;
		nf->getHost = getHost;
		w->addChild(nf);
		MeterWidget* mw = new MeterWidget;
		mw->box.pos = mm2px(Vec(vu[j][0], vu[j][1]));
		mw->box.size = mm2px(Vec(vu[j][2], vu[j][3]));
		mw->s = base + j;
		mw->getHost = getHost;
		w->addChild(mw);
	}
}

static void addScrews(ModuleWidget* w) {
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
}

/** The main display: channels, the link's state, audio. */
struct StatusDisplay : Widget {
	WingLink* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float x = mm2px(3.f);
			if (!module) {
				text(vg, boldFont(), x, mm2px(4.6f), "CH 33–40", 13.f, WHITE);
				Widget::drawLayer(args, layer);
				return;
			}
			int a = module->wingStart, b = std::min(P::NUM_IN, a + NS - 1);
			std::string chs = P::label(a) + " – " + P::label(b);
			if (module->expander() && a + NS <= P::NUM_IN)
				chs += "   +  " + P::label(a + NS) + " – " + P::label(std::min(P::NUM_IN, a + NT - 1));
			text(vg, boldFont(), x, mm2px(4.6f), chs, 12.f, WHITE, NVG_ALIGN_LEFT, 0.5f);
			std::string st;
			NVGcolor c;
			if (module->midiOut.getDeviceId() < 0) {
				st = "●  NO WING (MIDI): plug the WING in by USB, or pick its MIDI port (right-click)";
				c = BAD;
			}
			else if (module->sysexOk) {
				st = "●  WING  ·  MIDI OK  ·  SYSEX OK";
				c = OK;
			}
			else if (module->midiSeen) {
				st = "●  WING  ·  MIDI OK  ·  NO SYSEX: WING › SETUP › REMOTE › MIDI: allow USB SYSEX";
				c = WARN;
			}
			else {
				st = "●  WAITING FOR THE WING …  (WING › SETUP › REMOTE › MIDI: USB SYSEX + CHANNELS)";
				c = WARN;
			}
			text(vg, boldFont(), x, mm2px(8.1f), st, 6.6f, c, NVG_ALIGN_LEFT, 0.3f);
			int u = module->usbStart();
			std::string au;
			int nOut = module->port.deviceNumOutputs;
			if (module->port.getDeviceId() < 0)
				au = "AUDIO: none (right-click › Audio to the WING)";
			else if (nOut <= 0)
				au = string::f("AUDIO: USB %d+ not on the device (set the WING to 48 USB channels)", u);
			else
				au = string::f("AUDIO → USB %d–%d  ·  %s  ·  %.0f kHz", u, u + (module->expander() ? NT : NS) - 1,
				               module->audioName.c_str(),
				               module->port.deviceSampleRate / 1000.f);
			text(vg, boldFont(), x, mm2px(10.2f), au, 6.2f, DIM, NVG_ALIGN_LEFT, 0.3f);
		}
		Widget::drawLayer(args, layer);
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
		addStrips(this, module, 0, [module]() { return module; });
	}

	/** Find the WING by itself: its MIDI ports (the remote-control one) and its audio device (ASIO first). */
	void step() override {
		ModuleWidget::step();
		WingLink* m = dynamic_cast<WingLink*>(module);
		if (!m)
			return;
		scan -= APP->window->getLastFrameDuration();
		if (scan > 0.f)
			return;
		scan = 2.f;
		m->audioName = m->port.getDeviceId() >= 0 ? m->port.getDeviceName(m->port.getDeviceId()) : "";
		if (m->autoMidi) {
			auto pick = [&](midi::Port* port, bool in) {
				int cur = port->getDeviceId();
				if (cur >= 0) {
					std::vector<int> ids = port->getDeviceIds();
					if (std::find(ids.begin(), ids.end(), cur) != ids.end())
						return;  // still there
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
		menu->addChild(createSubmenuItem("Audio to the WING (USB)", m->port.getDeviceId() >= 0 ?
		                                 m->port.getDeviceName(m->port.getDeviceId()) : "none",
		                                 [=](Menu* sub) { app::appendAudioMenu(sub, &m->port); }));
		menu->addChild(createSubmenuItem("USB channels", m->usbMode.load() == 0 ? "same as the WING channels" :
		                                 string::f("from USB %d", m->usbMode.load()), [=](Menu* sub) {
			sub->addChild(createCheckMenuItem("Same as the WING channels (CH 33 ← USB 33)", "",
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
		std::string id;
		{
			std::lock_guard<std::mutex> lock(m->identMutex);
			id = m->wingIdent;
		}
		if (!id.empty())
			menu->addChild(createMenuLabel(id.substr(0, 60)));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Click a strip's colour bar or icon to change it, type its name"));
		menu->addChild(createMenuLabel("WING-LINK X docks on the right: 8 more channels"));
	}
};

struct XDisplay : Widget {
	WingLinkX* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float x = mm2px(3.f);
			WingLink* h = module ? module->host() : nullptr;
			if (!h) {
				text(vg, boldFont(), x, mm2px(4.6f), module ? "NOT LINKED" : "CH 41–48", 12.f, module ? WARN : WHITE);
				if (module)
					text(vg, boldFont(), x, mm2px(8.1f), "●  place me right next to WING-LINK", 6.6f, WARN, NVG_ALIGN_LEFT, 0.3f);
			}
			else {
				int a = h->wingStart + NS;
				std::string chs = a > P::NUM_IN ? "NO CHANNELS LEFT"
				                                : P::label(a) + " – " + P::label(std::min(P::NUM_IN, a + NS - 1));
				text(vg, boldFont(), x, mm2px(4.6f), chs, 12.f, WHITE, NVG_ALIGN_LEFT, 0.5f);
				text(vg, boldFont(), x, mm2px(8.1f), "●  LINKED TO WING-LINK", 6.6f, ACCENT, NVG_ALIGN_LEFT, 0.3f);
				int u = h->usbStart() + NS;
				text(vg, boldFont(), x, mm2px(10.2f), string::f("AUDIO → USB %d–%d", u, u + NS - 1), 6.2f, DIM,
				     NVG_ALIGN_LEFT, 0.3f);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

struct WingLinkXWidget : ModuleWidget {
	WingLinkXWidget(WingLinkX* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingLinkX.svg")));
		addScrews(this);
		XDisplay* d = new XDisplay;
		d->module = module;
		d->box.pos = mm2px(Vec(wllayout::wlx::DISPLAY[0], wllayout::wlx::DISPLAY[1]));
		d->box.size = mm2px(Vec(wllayout::wlx::DISPLAY[2], wllayout::wlx::DISPLAY[3]));
		addChild(d);
		addStrips(this, module, NS, [module]() { return module ? module->host() : nullptr; });
	}
};

}  // namespace winglink

Model* modelWingLink = createModel<winglink::WingLink, winglink::WingLinkWidget>("WingLink");
Model* modelWingLinkX = createModel<winglink::WingLinkX, winglink::WingLinkXWidget>("WingLinkX");

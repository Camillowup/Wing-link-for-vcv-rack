// SPDX-License-Identifier: GPL-3.0-only
// WING-REC – records every track of WING-LINK, WING-LINK X and WING-IN as WAV files in Rack (one folder per take, with
// a marker list and an Ableton Live set), and drives the WING's SD recorder (WING-LIVE): REC for both at once, the
// cards' transport, markers, sessions; SPLIT cuts an old SD session (multichannel WAVs) into one WAV per track.
//
// The engine fills a ring buffer (24 tracks per frame); a thread of its own writes the files. SYNC: REC only arms,
// the take starts with GENRA's RUN (or at its next bar when it already runs) and ends with its STOP; without GENRA the
// RUN and CLOCK inputs do it. ABLAUF's rows become markers (their names: the UI reads ABLAUF's song).
// Panel: tools/panels/wingrec_panel.py. The Live set: res/wingrec/ (tools/wingrec_als_template.py).
#include "WingLink.hpp"
#include "WingRecLayout.hpp"
#include "RecFiles.hpp"
#include <osdialog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace winglink {

namespace L = reclayout;
using namespace wlui;

static const char* const SECTIONS[6] = {"INTRO", "VERSE", "BUILD", "DROP", "BREAK", "OUTRO"};
static const char* const SD_MODES[3] = {"PLAY", "A->B", "LOOP"};
static const int SD_TRACKS[3] = {8, 16, 32};

using namespace recfiles;
static std::vector<recfiles::Marker> plainMarks(const std::vector<RecMarker>& m) {
	std::vector<recfiles::Marker> o;
	for (const RecMarker& r : m) {
		recfiles::Marker x;
		x.frame = r.frame;
		x.name = r.name;
		o.push_back(x);
	}
	return o;
}
static bool writeAlsFile(const std::string& path, const std::vector<RecFile>& files, const std::vector<RecMarker>& marks,
                         double bpm, int rate, int64_t frames) {
	return recfiles::writeAlsFile(path, readText(asset::plugin(pluginInstance, "res/wingrec/live12.xml")),
	                              readText(asset::plugin(pluginInstance, "res/wingrec/live12_track.xml")), files,
	                              plainMarks(marks), bpm, rate, frames);
}

// ---------------------------------------------------------------- settings of this computer (the folder …)
static std::string settingsPath() { return asset::user("WingLink-REC.json"); }
static void loadSettings(WingRec* m) {
	std::string s = readText(settingsPath());
	json_error_t err;
	json_t* j = s.empty() ? nullptr : json_loads(s.c_str(), 0, &err);
	std::lock_guard<std::mutex> lock(m->planMutex);
	m->rootDir = asset::user("WING-REC");
	if (!j)
		return;
	if (json_t* v = json_object_get(j, "root"))
		if (json_is_string(v) && *json_string_value(v))
			m->rootDir = json_string_value(v);
	if (json_t* v = json_object_get(j, "bits"))
		m->bits = json_integer_value(v) == 32 ? 32 : 24;
	if (json_t* v = json_object_get(j, "als"))
		m->writeAls = json_is_true(v);
	if (json_t* v = json_object_get(j, "autoMarks"))
		m->autoMarks = json_is_true(v);
	json_decref(j);
}
static void saveSettings(WingRec* m) {
	json_t* j = json_object();
	{
		std::lock_guard<std::mutex> lock(m->planMutex);
		json_object_set_new(j, "root", json_string(m->rootDir.c_str()));
		json_object_set_new(j, "bits", json_integer(m->bits));
		json_object_set_new(j, "als", json_boolean(m->writeAls));
		json_object_set_new(j, "autoMarks", json_boolean(m->autoMarks));
	}
	char* s = json_dumps(j, JSON_INDENT(2));
	if (s) {
		writeText(settingsPath(), s);
		std::free(s);
	}
	json_decref(j);
}

// ---------------------------------------------------------------- the module
WingRec::WingRec() {
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "SYNC", {"Off: REC starts at once", "On: REC arms, GENRA's RUN starts the take"});
	configButton(REC_PARAM, "REC (again: stop)");
	configButton(STOP_PARAM, "STOP");
	configButton(MARK_PARAM, "MARK (a marker now)");
	static const char* const TG[3] = {"Record in Rack (one WAV per track)", "Record on SD card 1", "Record on SD card 2"};
	for (int i = 0; i < 3; i++)
		configSwitch(TGT_PARAM + i, 0.f, 1.f, i == 0 ? 1.f : 0.f, TG[i], {"Off", "On"});
	configSwitch(CARD_PARAM, 0.f, 2.f, 0.f, "SD transport for", {"Card 1", "Card 2", "Both cards"});
	configButton(PLAY_PARAM, "SD PLAY");
	configButton(PAUSE_PARAM, "SD PAUSE");
	configButton(SDSTOP_PARAM, "SD STOP");
	configButton(MPREV_PARAM, "SD marker ◀ (go to)");
	configButton(MNEXT_PARAM, "SD marker ▶ (go to)");
	configButton(SPLIT_PARAM, "SPLIT an SD session into one WAV per track");
	configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "SD play mode", {"PLAY", "A→B", "LOOP"});
	configSwitch(TRACKS_PARAM, 0.f, 2.f, 1.f, "SD tracks", {"8", "16", "32"});
	for (int p = 0; p < NPAIR; p++)
		configSwitch(ST_PARAM + p, 0.f, 1.f, 0.f, string::f("Tracks %d + %d as one stereo WAV", 2 * p + 1, 2 * p + 2),
		             {"Off (two mono WAVs)", "On (stereo)"});
	for (int t = 0; t < NREC; t++) {
		configSwitch(ARM_PARAM + t, 0.f, 1.f, 1.f, string::f("Record track %d", t + 1), {"Off", "On"});
		peak[t] = 0.f;
	}
	for (int t = 0; t < NT; t++)
		configInput(IN_INPUT + t, string::f("IN %d (a cable takes track %d over)", t + 1, t + 1));
	configInput(TRIG_REC_INPUT, "REC (trigger)");
	configInput(TRIG_STOP_INPUT, "STOP (trigger)");
	configInput(TRIG_MARK_INPUT, "MARK (trigger)");
	configInput(TRIG_SDPLAY_INPUT, "SD PLAY / STOP (trigger)");
	configInput(CLOCK_INPUT, "Clock without GENRA (16ths: a bar every 16 pulses)");
	configInput(RUN_INPUT, "Run without GENRA (gate: high = running)");
	configOutput(GATE_REC_OUTPUT, "REC (gate while recording)");
	configOutput(GATE_SDPLAY_OUTPUT, "SD PLAY (gate while a card plays)");
	for (int c = 0; c < 2; c++)
		sdOn[c] = false;
	for (int i = 0; i < NT / 2; i++)
		stereoSeen[i] = -1;
	lightDiv.setDivision(256);
	sampleRate = APP->engine->getSampleRate();
	loadSettings(this);
	writer = std::thread([this]() { writerLoop(); });
}
WingRec::~WingRec() {
	quit = true;
	if (writer.joinable())
		writer.join();
}

void WingRec::sdAll(const std::string& cmd, bool onlyTake) {
	WingLink* h = host.load();
	if (onlyTake) {
		for (int c = 0; c < 2; c++)
			if (sdOn[c])
				hostSdCommand(h, c, cmd);
		return;
	}
	int sel = (int) params[CARD_PARAM].getValue();
	for (int c = 0; c < 2; c++)
		if (sel == 2 || sel == c)
			hostSdCommand(h, c, cmd);
}

void WingRec::startTake() {
	bool rack = params[TGT_PARAM + 0].getValue() > 0.5f;
	bool sd1 = params[TGT_PARAM + 1].getValue() > 0.5f, sd2 = params[TGT_PARAM + 2].getValue() > 0.5f;
	if (rack && writerBusy)  // the last take is still being written
		rack = false;
	if (!rack && !sd1 && !sd2) {
		state = IDLE;
		return;
	}
	rackOn = rack;
	sdOn[0] = sd1;
	sdOn[1] = sd2;
	frames = 0;
	bar = 1;
	clocks = 0;
	dropped = 0;
	{
		std::lock_guard<std::mutex> lock(markMutex);
		marks.clear();
	}
	markCount = 0;
	if (rack) {
		writerBusy = true;
		wantStart++;
	}
	int tr = SD_TRACKS[clamp((int) params[TRACKS_PARAM].getValue(), 0, 2)];
	sdAll(string::f("cfg.rectracks=%d", tr), true);
	sdAll("$ctl.control=REC", true);
	state = RECORDING;
}
void WingRec::stopTake() {
	if (state == RECORDING) {
		sdAll("$ctl.control=STOP", true);
		if (sdOn[0] || sdOn[1])
			namePending = true;  // (the UI names the SD session in a moment)
		if (rackOn)
			wantStop++;
	}
	state = IDLE;
}
void WingRec::addMark(bool ablauf, float pos) {
	if (state != RECORDING)
		return;
	RecMarker m;
	m.frame = frames;
	m.bar = bar;
	m.pos = ablauf ? pos : -1.f;
	int n = ++markCount;
	if (!ablauf)
		m.name = string::f("MARK %d", n);
	{
		std::lock_guard<std::mutex> lock(markMutex);
		if (marks.size() < 999)
			marks.push_back(m);
	}
	sdAll("$ctl.setmarker=1", true);
}

void WingRec::process(const ProcessArgs& args) {
	WingLink* h = host.load();
	// ---- the transport: GENRA (or RUN / CLOCK in)
	engine::Engine* e = eng;
	Module* g = nullptr;
	Module* ab = nullptr;
	if (e) {
		int64_t gid = genraId, aid = ablaufId;
		g = gid >= 0 ? e->getModule_NoLock(gid) : nullptr;
		ab = aid >= 0 ? e->getModule_NoLock(aid) : nullptr;
	}
	int gr = genraRun, gb = genraBar, ar = ablaufRow, ap = ablaufPos;
	bool haveRun = false, run = false, barPulse = false;
	if (g && gr >= 0 && gr < (int) g->outputs.size() && gb >= 0 && gb < (int) g->outputs.size()) {
		haveRun = true;
		run = g->outputs[gr].getVoltage() > 1.f;
		barPulse = barT.process(g->outputs[gb].getVoltage(), 0.1f, 1.f);
	}
	else {
		if (inputs[RUN_INPUT].isConnected()) {
			haveRun = true;
			run = inputs[RUN_INPUT].getVoltage() > 1.f;
		}
		if (clockT.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (clocks % 16 == 0)
				barPulse = true;
			clocks++;
		}
		if (!run)
			clocks = 0;
	}
	bool runRise = run && !runWas, runFall = !run && runWas;
	runWas = run;
	bool sync = params[SYNC_PARAM].getValue() > 0.5f;

	// ---- REC / STOP / MARK
	bool rec = recB.process(params[REC_PARAM].getValue() > 0.5f) | uiRec.exchange(false) |
	           recT.process(inputs[TRIG_REC_INPUT].getVoltage(), 0.1f, 1.f);
	bool stop = stopB.process(params[STOP_PARAM].getValue() > 0.5f) |
	            stopT.process(inputs[TRIG_STOP_INPUT].getVoltage(), 0.1f, 1.f);
	bool mark = markB.process(params[MARK_PARAM].getValue() > 0.5f) |
	            markT.process(inputs[TRIG_MARK_INPUT].getVoltage(), 0.1f, 1.f);
	int st = state;
	if (rec) {
		if (st == IDLE) {
			if (sync && haveRun)
				state = ARMED;
			else
				startTake();
		}
		else if (st == ARMED)
			state = IDLE;
		else
			stopTake();
	}
	if (stop) {
		if (state == ARMED)
			state = IDLE;
		else if (state == RECORDING)
			stopTake();
	}
	if (state == ARMED && (runRise || (run && barPulse)))
		startTake();
	else if (state == RECORDING && sync && haveRun && runFall)
		stopTake();

	// ---- the take
	bool rowRise = false;
	float rowPos = -1.f;
	if (ab && ar >= 0 && ar < (int) ab->outputs.size() && rowT.process(ab->outputs[ar].getVoltage(), 0.1f, 1.f)) {
		rowRise = true;
		rowPos = (ap >= 0 && ap < (int) ab->outputs.size()) ? ab->outputs[ap].getVoltage() : -1.f;
		rowAge = 0.f;
		lastRowPos = rowPos;
	}
	rowAge += args.sampleTime;
	if (state == RECORDING) {
		if (frames == 0 && rowAge < 0.05f && autoMarks && !rowRise)
			addMark(true, lastRowPos);  // (ABLAUF's first row came just before RUN)
		if (barPulse && frames > (int64_t) (0.1f * args.sampleRate))
			bar++;
		if (mark)
			addMark(false, -1.f);
		if (rowRise && autoMarks)
			addMark(true, rowPos);
	}
	RecFrame fr;
	WingIn* im = hostInMod(h);
	for (int t = 0; t < NREC; t++) {
		float v = 0.f;
		if (t < NT) {
			if (inputs[IN_INPUT + t].isConnected())
				v = inputs[IN_INPUT + t].getVoltageSum() / 10.f;
			else
				v = hostStripAudio(h, t);
		}
		else if (im)
			v = im->audio[t - NT].load(std::memory_order_relaxed);
		fr.s[t] = v;
		float p = std::max(std::fabs(v), peak[t].load(std::memory_order_relaxed) * peakDecay);
		peak[t].store(p, std::memory_order_relaxed);
	}
	if (args.sampleTime > 0.f)
		peakDecay = std::exp(-args.sampleTime / 0.25f);
	if (state == RECORDING) {
		if (rackOn) {
			if (!ring.full())
				ring.push(fr);
			else
				dropped++;
		}
		frames++;
	}

	// ---- the SD cards' transport
	int sel = (int) params[CARD_PARAM].getValue();
	auto playing = [&]() {
		for (int c = 0; c < 2; c++)
			if ((sel == 2 || sel == c) && hostSdInfo(h, c).state == 2)
				return true;
		return false;
	};
	if (playB.process(params[PLAY_PARAM].getValue() > 0.5f))
		sdAll("$ctl.control=PLAY", false);
	if (pauseB.process(params[PAUSE_PARAM].getValue() > 0.5f))
		sdAll("$ctl.control=PPAUSE", false);
	if (sdStopB.process(params[SDSTOP_PARAM].getValue() > 0.5f))
		sdAll("$ctl.control=STOP", false);
	if (sdPlayT.process(inputs[TRIG_SDPLAY_INPUT].getVoltage(), 0.1f, 1.f))
		sdAll(playing() ? "$ctl.control=STOP" : "$ctl.control=PLAY", false);
	bool prev = prevB.process(params[MPREV_PARAM].getValue() > 0.5f);
	bool next = nextB.process(params[MNEXT_PARAM].getValue() > 0.5f);
	if (prev || next) {
		int mx = 100;
		SdInfo i = hostSdInfo(h, sel == 1 ? 1 : 0);
		if (i.markers > 0)
			mx = i.markers;
		int n = clamp(sdMark + (next ? 1 : -1), 1, mx);
		sdMark = n;
		sdAll(string::f("$ctl.gotomarker=%d", n), false);
	}
	if (splitB.process(params[SPLIT_PARAM].getValue() > 0.5f))
		splitClick = true;

	outputs[GATE_REC_OUTPUT].setVoltage(state == RECORDING ? 10.f : 0.f);
	if (lightDiv.process()) {
		outputs[GATE_SDPLAY_OUTPUT].setVoltage(playing() ? 10.f : 0.f);
		lights[SYNC_LIGHT].setBrightness(sync ? 1.f : 0.f);
		for (int p = 0; p < NPAIR; p++)
			lights[ST_LIGHT + p].setBrightness(params[ST_PARAM + p].getValue() > 0.5f ? 1.f : 0.f);
	}
}

json_t* WingRec::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "sdMark", json_integer(sdMark));
	return root;
}
void WingRec::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "sdMark"))
		sdMark = clamp((int) json_integer_value(j), 1, 100);
}

// ---------------------------------------------------------------- the writer thread
/** SPLIT: an SD session's multichannel WAVs (00000001.WAV …) → one mono WAV per track. */
static std::string splitSession(WingRec* m, const std::string& dir, const std::string& outRoot) {
	std::vector<std::string> wavs;
	for (const std::string& p : system::getEntries(dir)) {
		std::string ext = string::lowercase(system::getExtension(p));
		if (ext == ".wav" && !system::isDirectory(p))
			wavs.push_back(p);
	}
	std::sort(wavs.begin(), wavs.end());
	if (wavs.empty())
		return "SPLIT: NO WAV FILES IN THAT FOLDER";
	std::string out = system::join(outRoot, safeName(system::getFilename(dir)) + " SD");
	system::createDirectories(out);
	std::vector<RecFile> files;
	int channels = 0, rate = 48000;
	uint64_t total = 0;
	for (size_t w = 0; w < wavs.size(); w++) {
		FILE* f = openFile(wavs[w], false);
		if (!f)
			continue;
		char id[4];
		uint32_t size = 0;
		unsigned char b4[4];
		if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4) || std::fread(b4, 1, 4, f) != 4 ||
		    std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4)) {
			std::fclose(f);
			continue;
		}
		int ch = 0, bits = 0, fmt = 1;
		bool data = false;
		while (std::fread(id, 1, 4, f) == 4 && std::fread(b4, 1, 4, f) == 4) {
			size = b4[0] | b4[1] << 8 | b4[2] << 16 | (uint32_t) b4[3] << 24;
			if (!std::memcmp(id, "fmt ", 4)) {
				unsigned char fm[40] = {};
				size_t n = std::min<uint32_t>(size, 40);
				if (std::fread(fm, 1, n, f) != n)
					break;
				fmt = fm[0] | fm[1] << 8;
				ch = fm[2] | fm[3] << 8;
				rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
				bits = fm[14] | fm[15] << 8;
				if (fmt == 0xfffe && n >= 26)
					fmt = fm[24] | fm[25] << 8;
				if (size > n)
					std::fseek(f, (long) (size - n + (size & 1)), SEEK_CUR);
				else if (size & 1)
					std::fseek(f, 1, SEEK_CUR);
			}
			else if (!std::memcmp(id, "data", 4)) {
				data = true;
				break;
			}
			else
				std::fseek(f, (long) (size + (size & 1)), SEEK_CUR);
		}
		if (!data || ch <= 0 || (bits != 16 && bits != 24 && bits != 32)) {
			std::fclose(f);
			continue;
		}
		if (files.empty()) {
			channels = ch;
			for (int c = 0; c < ch; c++) {
				RecFile r;
				r.a = c;
				r.name = string::f("%02d SD %d", c + 1, c + 1);
				r.path = system::join(out, r.name + ".wav");
				r.f = openFile(r.path, true);
				if (r.f) {
					std::string hd = wavHeader(1, rate, 24, 0);
					std::fwrite(hd.data(), 1, hd.size(), r.f);
				}
				files.push_back(r);
			}
		}
		if (ch != channels) {
			std::fclose(f);
			continue;
		}
		int bps = bits / 8;
		size_t frameBytes = (size_t) ch * bps;
		std::vector<unsigned char> buf(frameBytes * 4096);
		uint64_t left = size;
		while (left >= frameBytes && !m->quit) {
			size_t want = (size_t) std::min<uint64_t>(left / frameBytes, 4096) * frameBytes;
			size_t got = std::fread(buf.data(), 1, want, f);
			got -= got % frameBytes;
			if (got == 0)
				break;
			left -= got;
			size_t nfr = got / frameBytes;
			for (int c = 0; c < ch; c++) {
				RecFile& r = files[c];
				r.buf.clear();
				for (size_t i = 0; i < nfr; i++) {
					const unsigned char* s = &buf[i * frameBytes + c * bps];
					float v;
					if (bits == 32 && fmt == 3) {
						std::memcpy(&v, s, 4);
					}
					else if (bits == 32)
						v = (int32_t) (s[0] | s[1] << 8 | s[2] << 16 | (uint32_t) s[3] << 24) / 2147483648.f;
					else if (bits == 24)
						v = (int32_t) ((s[0] << 8 | s[1] << 16 | (uint32_t) s[2] << 24)) / 2147483648.f;
					else
						v = (int16_t) (s[0] | s[1] << 8) / 32768.f;
					sample(r.buf, v, 24);
				}
				if (r.f)
					std::fwrite(r.buf.data(), 1, r.buf.size(), r.f);
				r.bytes += r.buf.size();
				r.buf.clear();
			}
			total += nfr;
			if ((total & 0xffff) < 4096) {
				std::lock_guard<std::mutex> lock(m->statusMutex);
				m->splitMsg = string::f("SPLIT: %d TRACKS · %d:%02d", channels, (int) (total / rate / 60),
				                        (int) (total / rate % 60));
			}
		}
		std::fclose(f);
	}
	std::vector<recfiles::Marker> none;
	for (RecFile& r : files)
		wavFinish(r, rate, 24, none);
	if (files.empty())
		return "SPLIT: NO READABLE SD SESSION THERE";
	return string::f("SPLIT DONE: %d TRACKS → %s", channels, system::getFilename(out).c_str());
}

void WingRec::writerLoop() {
	int startsDone = 0, stopsDone = 0;
	std::vector<RecFile> files;
	std::string dir;
	int rate = 48000, depth = 24;
	bool als = true;
	double takeBpm = 0.0;
	while (!quit) {
		int ws = wantStart, wp = wantStop;
		// a take begins: its folder, its files
		if (ws != startsDone && files.empty() && dir.empty()) {
			startsDone = ws;
			std::vector<RecPlan> pl;
			std::string root, gen;
			{
				std::lock_guard<std::mutex> lock(planMutex);
				pl = plan;
				root = rootDir;
				gen = genre;
				depth = bits;
				als = writeAls;
			}
			rate = (int) std::round(sampleRate.load());
			takeBpm = bpm;
			std::time_t now = std::time(nullptr);
			char ts[32];
			std::strftime(ts, sizeof ts, "%Y-%m-%d %H-%M", std::localtime(&now));
			std::string base = std::string(ts) + (gen.empty() ? "" : " " + safeName(gen));
			dir = system::join(root, base);
			for (int n = 2; system::exists(dir) && n < 100; n++)
				dir = system::join(root, base + string::f(" (%d)", n));
			system::createDirectories(dir);
			std::string err;
			for (const RecPlan& p : pl) {
				RecFile r;
				r.a = p.a;
				r.b = p.b;
				r.ch = p.b >= 0 ? 2 : 1;
				r.name = p.name;
				r.colour = p.colour;
				r.path = system::join(dir, safeName(p.name) + ".wav");
				r.f = openFile(r.path, true);
				if (!r.f) {
					err = "CAN'T WRITE " + system::getFilename(r.path);
					continue;
				}
				std::string hd = wavHeader(r.ch, rate, depth, 0);
				std::fwrite(hd.data(), 1, hd.size(), r.f);
				files.push_back(r);
			}
			std::lock_guard<std::mutex> lock(statusMutex);
			takeDir = dir;
			lastError = files.empty() ? (err.empty() ? "NO TRACKS TO RECORD" : err) : err;
		}
		bool stopNow = wp != stopsDone;
		// the frames
		while (!ring.empty()) {
			RecFrame fr = ring.shift();
			for (RecFile& r : files) {
				sample(r.buf, fr.s[r.a], depth);
				if (r.b >= 0)
					sample(r.buf, fr.s[r.b], depth);
			}
			for (RecFile& r : files)
				if (r.buf.size() >= 65536) {
					if (std::fwrite(r.buf.data(), 1, r.buf.size(), r.f) != r.buf.size()) {
						std::lock_guard<std::mutex> lock(statusMutex);
						lastError = "DISK FULL? WRITING FAILED";
					}
					r.bytes += r.buf.size();
					r.buf.clear();
				}
		}
		// the take ends: the markers, the list, the Live set
		if (stopNow && ws == startsDone) {
			stopsDone = wp;
			for (RecFile& r : files) {
				r.bytes += r.buf.size();
			}
			std::vector<RecMarker> mk;
			{
				std::lock_guard<std::mutex> lock(markMutex);
				mk = marks;
			}
			for (size_t i = 0; i < mk.size(); i++)
				if (mk[i].name.empty())
					mk[i].name = string::f("PART %d", (int) i + 1);
			int64_t len = 0;
			for (RecFile& r : files) {
				int bpf = r.ch * depth / 8;
				len = std::max<int64_t>(len, (int64_t) (r.bytes / bpf));
				wavFinish(r, rate, depth, plainMarks(mk));
			}
			if (!dir.empty() && !files.empty()) {
				std::string t = "WING-REC  ·  " + system::getFilename(dir) + "\n";
				t += string::f("%d Hz · %d bit%s · %.2f BPM · %d:%02d\n\n", rate, depth, depth == 32 ? " float" : "",
				               takeBpm, (int) (len / rate / 60), (int) (len / rate % 60));
				t += "Markers (bar · time · name)\n";
				for (const RecMarker& m : mk) {
					double s = (double) m.frame / rate;
					t += string::f("Bar %4d   %2d:%06.3f   %s\n", m.bar, (int) (s / 60), std::fmod(s, 60.0), m.name.c_str());
				}
				if (mk.empty())
					t += "(none)\n";
				t += "\nFiles\n";
				for (RecFile& r : files)
					t += "  " + system::getFilename(r.path) + (r.ch == 2 ? "  (stereo)\n" : "\n");
				writeText(system::join(dir, "Markers.txt"), t);
				if (als)
					writeAlsFile(system::join(dir, system::getFilename(dir) + ".als"), files, mk, takeBpm, rate, len);
			}
			files.clear();
			dir.clear();
			writerBusy = false;
		}
		// SPLIT (between takes)
		if (files.empty()) {
			std::string req, root;
			{
				std::lock_guard<std::mutex> lock(statusMutex);
				req.swap(splitReq);
			}
			if (!req.empty()) {
				{
					std::lock_guard<std::mutex> lock(planMutex);
					root = rootDir;
				}
				busySplit = true;
				std::string msg = splitSession(this, req, root);
				busySplit = false;
				std::lock_guard<std::mutex> lock(statusMutex);
				splitMsg = msg;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	for (RecFile& r : files) {
		std::vector<recfiles::Marker> none;
		wavFinish(r, rate, depth, none);
	}
}

// ---------------------------------------------------------------- the UI
static std::string hms(double s) { return string::f("%02d:%02d", (int) (s / 60), (int) std::fmod(s, 60.0)); }
static NVGcolor wingCol(int c, NVGcolor def) {
	if (c >= 1 && c <= P::NUM_COLOURS)
		return nvgRGB(P::COLOURS[c - 1].r, P::COLOURS[c - 1].g, P::COLOURS[c - 1].b);
	return def;
}
static const NVGcolor RECRED = nvgRGB(0xff, 0x3b, 0x3b);

/** A track's name as the take will call it (the UI): its WING channel, or IN n. */
static std::string trackName(WingRec* m, WingLink* h, int t, int* colour, bool* there) {
	*colour = 0;
	*there = false;
	if (t < NT) {
		bool cable = m->inputs[WingRec::IN_INPUT + t].isConnected();
		if (hostStripActive(h, t)) {
			*there = true;
			Meta mt = hostStripMeta(h, t);
			*colour = mt.colour;
			if (!mt.name.empty())
				return mt.name;
			return hostStripLabel(h, t);
		}
		if (cable) {
			*there = true;
			return string::f("IN %d", t + 1);
		}
		return "";
	}
	WingIn* im = hostInMod(h);
	if (!im)
		return "";
	*there = true;
	InSource s;
	{
		std::lock_guard<std::mutex> lock(im->srcMutex);
		s = im->src[t - NT];
	}
	int key = inSourceKey(s);
	if (key >= 0) {
		Meta mt = hostMetaOf(h, key);
		*colour = mt.colour;
		if (!mt.name.empty())
			return mt.name;
	}
	return s.grp == IG_OFF ? string::f("USB OUT %d", t - NT + 1) : inSourceLabel(s);
}

struct RecDisplay : Widget {
	WingRec* module = nullptr;
	float blink = 0.f;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			blink += frameDt();
			float x = mm2px(2.f);
			int st = module ? module->state.load() : WingRec::RECORDING;
			float sr = module ? std::max(1.f, module->sampleRate.load()) : 48000.f;
			double secs = module ? module->frames.load() / sr : 192.0;
			std::string l1;
			NVGcolor c1 = WHITE;
			if (st == WingRec::RECORDING) {
				l1 = "●  REC  " + hms(secs);
				c1 = RECRED;
			}
			else if (st == WingRec::ARMED) {
				l1 = "●  ARMED · WAITING FOR RUN";
				c1 = std::fmod(blink, 0.8f) < 0.4f ? RECRED : FAINT;
			}
			else
				l1 = "READY";
			text(vg, boldFont(), x, mm2px(4.9f), l1, 12.f, c1, NVG_ALIGN_LEFT, 0.6f);
			if (st == WingRec::RECORDING || !module) {
				std::string name;
				if (module) {
					std::lock_guard<std::mutex> lock(module->markMutex);
					if (!module->marks.empty())
						name = module->marks.back().name;
				}
				else
					name = "DROP 1";
				std::string b = string::f("BAR %d", module ? module->bar.load() : 49) + (name.empty() ? "" : " · " + name);
				text(vg, boldFont(), mm2px(30.f), mm2px(4.9f), b, 8.5f, WHITE, NVG_ALIGN_LEFT, 0.4f);
			}
			// the status line
			std::string s;
			NVGcolor sc = OK;
			WingLink* h = module ? module->hostSafe() : nullptr;
			std::string err, split, dir;
			if (module) {
				std::lock_guard<std::mutex> lock(module->statusMutex);
				err = module->lastError;
				split = module->splitMsg;
				dir = module->takeDir;
			}
			if (!module)
				s = "●  SYNC · GENRA 132 BPM · 22 TRACKS · 6 MARKERS";
			else if (module->dropped > 0) {
				s = string::f("●  DISK TOO SLOW: %lld FRAMES LOST", (long long) module->dropped.load());
				sc = BAD;
			}
			else if (!err.empty()) {
				s = "●  " + err;
				sc = WARN;
			}
			else if (module->busySplit || !split.empty()) {
				s = "●  " + (split.empty() ? std::string("SPLIT …") : split);
				sc = module->busySplit ? WARN : OK;
			}
			else if (!h) {
				s = "●  right-click › Connect to WING-LINK";
				sc = WARN;
			}
			else if (!hostLinkedSat(h, module)) {
				s = "●  WING-LINK already has a WING-REC";
				sc = WARN;
			}
			else {
				bool sync = module->params[WingRec::SYNC_PARAM].getValue() > 0.5f;
				bool genra = module->genraId >= 0;
				s = "●  ";
				s += sync ? (genra ? "SYNC · GENRA" : (module->inputs[WingRec::RUN_INPUT].isConnected() ? "SYNC · RUN IN"
				                                                                                          : "SYNC: NO GENRA / RUN → AT ONCE"))
				          : "SYNC OFF";
				if (module->bpm > 0.f)
					s += string::f(" %.0f BPM", module->bpm.load());
				size_t n;
				{
					std::lock_guard<std::mutex> lock(module->planMutex);
					n = module->plan.size();
				}
				s += string::f(" · %d FILES · %d MARKERS", (int) n, module->markCount.load());
			}
			text(vg, boldFont(), x, mm2px(8.0f), s, 5.6f, sc, NVG_ALIGN_LEFT, 0.3f);
			if (module && dir.empty()) {
				std::lock_guard<std::mutex> lock(module->planMutex);
				dir = module->rootDir;
			}
			if (!module)
				dir = "D:\\Recordings\\2026-11-22 17-46 UK Garage";
			text(vg, boldFont(), x, mm2px(10.2f), dir, 5.f, DIM, NVG_ALIGN_LEFT, 0.1f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** RACK / SD 1 / SD 2: on = this take records there (a click). */
struct TargetTile : OpaqueWidget {
	WingRec* module = nullptr;
	int i = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			bool on = module ? module->params[WingRec::TGT_PARAM + i].getValue() > 0.5f : i < 2;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 2.f);
			nvgFillColor(vg, on ? nvgRGB(0x3a, 0x1d, 0x1d) : DARK);
			nvgFill(vg);
			nvgStrokeWidth(vg, 1.2f);
			nvgStrokeColor(vg, on ? RECRED : nvgRGB(0x34, 0x36, 0x3f));
			nvgStroke(vg);
			static const char* const N[3] = {"RACK", "SD 1", "SD 2"};
			float cx = box.size.x / 2;
			text(vg, boldFont(), cx, mm2px(4.6f), N[i], 7.f, on ? WHITE : DIM, NVG_ALIGN_CENTER, 0.3f);
			std::string sub;
			NVGcolor sc = on ? nvgRGB(0xff, 0xb0, 0xa8) : FAINT;
			if (!module)
				sub = i == 0 ? "24 TRK" : (i == 1 ? "1:42 h" : "3:05 h");
			else if (i == 0) {
				std::lock_guard<std::mutex> lock(module->planMutex);
				sub = string::f("%d FILES", (int) module->plan.size());
			}
			else {
				SdInfo s = hostSdInfo(module->hostSafe(), i - 1);
				if (s.card == 0)
					sub = "NO CARD";
				else if (s.state == 3)
					sub = hms(s.etime / 1000.0), sc = RECRED;
				else if (s.state == 2)
					sub = "PLAY";
				else if (s.sdfree > 0.f)
					sub = string::f("%d:%02d h", (int) (s.sdfree / 3.6e6f), (int) std::fmod(s.sdfree / 6e4f, 60.f));
				else
					sub = "–";
			}
			text(vg, boldFont(), cx, mm2px(8.6f), sub, 5.2f, sc, NVG_ALIGN_CENTER, 0.1f);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			Param& p = module->params[WingRec::TGT_PARAM + i];
			p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

/** The big REC button. */
struct RecButton : OpaqueWidget {
	WingRec* module = nullptr;
	float blink = 0.f;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			blink += frameDt();
			int st = module ? module->state.load() : WingRec::RECORDING;
			float cx = box.size.x / 2, cy = box.size.y / 2, r = std::min(cx, cy);
			nvgBeginPath(vg);
			nvgCircle(vg, cx, cy, r * 0.98f);
			nvgFillColor(vg, nvgRGB(0x2a, 0x2c, 0x33));
			nvgFill(vg);
			nvgStrokeWidth(vg, 1.2f);
			nvgStrokeColor(vg, nvgRGB(0x4a, 0x4c, 0x57));
			nvgStroke(vg);
			bool lit = st == WingRec::RECORDING || (st == WingRec::ARMED && std::fmod(blink, 0.8f) < 0.4f);
			nvgBeginPath(vg);
			nvgCircle(vg, cx, cy, r * 0.7f);
			nvgFillColor(vg, lit ? RECRED : nvgRGB(0x68, 0x22, 0x22));
			nvgFill(vg);
			if (lit) {
				nvgBeginPath(vg);
				nvgCircle(vg, cx, cy, r * 0.86f);
				nvgStrokeWidth(vg, 1.4f);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0x3b, 0x3b, 0x70));
				nvgStroke(vg);
			}
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			module->uiRec = true;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

/** A field with a menu (CARD, MODE, SESSION, TRACKS). */
struct MenuField : OpaqueWidget {
	WingRec* module = nullptr;
	int kind = 0;  // 0 CARD, 1 MODE, 2 SESSION, 3 TRACKS, 4 the SD marker
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 2.f);
			nvgFillColor(vg, DARK);
			nvgFill(vg);
			std::string s;
			NVGcolor c = WHITE;
			if (kind == 0) {
				static const char* const C[3] = {"CARD 1", "CARD 2", "BOTH"};
				s = C[module ? clamp((int) module->params[WingRec::CARD_PARAM].getValue(), 0, 2) : 0];
			}
			else if (kind == 1) {
				static const char* const M[3] = {"PLAY", "A→B", "LOOP"};
				s = M[module ? clamp((int) module->params[WingRec::MODE_PARAM].getValue(), 0, 2) : 2];
			}
			else if (kind == 2) {
				if (!module)
					s = "S07 / 12";
				else {
					int sel = (int) module->params[WingRec::CARD_PARAM].getValue();
					SdInfo i = hostSdInfo(module->hostSafe(), sel == 1 ? 1 : 0);
					s = i.sessions > 0 ? string::f("S%02d / %d", std::max(1, i.sessionPos), i.sessions)
					                   : (i.card == 0 ? "NO CARD" : "SESSION –");
				}
			}
			else if (kind == 3)
				s = std::to_string(SD_TRACKS[module ? clamp((int) module->params[WingRec::TRACKS_PARAM].getValue(), 0, 2) : 1]);
			else {
				s = string::f("M %d", module ? module->sdMark.load() : 6);
				c = ACCENT;
			}
			text(vg, boldFont(), box.size.x / 2, box.size.y / 2 + 2.6f, s, 7.f, c, NVG_ALIGN_CENTER, 0.3f);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module || kind == 4) {
			OpaqueWidget::onButton(e);
			return;
		}
		WingRec* m = module;
		ui::Menu* menu = createMenu();
		auto param = [=](int id, const char* label, int v, std::function<void()> also) {
			return createCheckMenuItem(label, "", [=]() { return (int) m->params[id].getValue() == v; }, [=]() {
				m->params[id].setValue((float) v);
				if (also)
					also();
			});
		};
		if (kind == 0) {
			menu->addChild(createMenuLabel("SD transport (PLAY, PAUSE, STOP, markers, session) for"));
			menu->addChild(param(WingRec::CARD_PARAM, "Card 1", 0, nullptr));
			menu->addChild(param(WingRec::CARD_PARAM, "Card 2", 1, nullptr));
			menu->addChild(param(WingRec::CARD_PARAM, "Both cards", 2, nullptr));
		}
		else if (kind == 1) {
			menu->addChild(createMenuLabel("SD play mode"));
			static const char* const M[3] = {"PLAY", "A→B (between the two points set on the WING)", "LOOP"};
			for (int i = 0; i < 3; i++)
				menu->addChild(param(WingRec::MODE_PARAM, M[i], i,
				                     [=]() { m->sdAll(std::string("cfg.playmode=") + SD_MODES[i], false); }));
		}
		else if (kind == 2) {
			int sel = (int) m->params[WingRec::CARD_PARAM].getValue();
			SdInfo i = hostSdInfo(m->hostSafe(), sel == 1 ? 1 : 0);
			menu->addChild(createMenuLabel("Open an SD session (the card must be stopped)"));
			if (i.sessions <= 0)
				menu->addChild(createMenuLabel("(no sessions known)"));
			for (int n = 1; n <= std::min(i.sessions, 100); n++)
				menu->addChild(createCheckMenuItem(string::f("Session %d", n), "", [=]() { return i.sessionPos == n; },
				                                   [=]() { m->sdAll(string::f("$ctl.opensession=%d", n), false); }));
		}
		else if (kind == 3) {
			menu->addChild(createMenuLabel("Tracks the SD cards record"));
			for (int k = 0; k < 3; k++)
				menu->addChild(param(WingRec::TRACKS_PARAM, std::to_string(SD_TRACKS[k]).c_str(), k, nullptr));
		}
		e.consume(this);
	}
};

/** A track: its WING colour and name, its level, the red dot (a click: record it or not). */
struct TrackTile : OpaqueWidget {
	WingRec* module = nullptr;
	int t = 0;
	std::string name;
	int colour = 0;
	bool there = false;
	void step() override {
		if (module) {
			WingLink* h = module->hostSafe();
			name = trackName(module, h, t, &colour, &there);
		}
		OpaqueWidget::step();
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 2.f);
			nvgFillColor(vg, DARK);
			nvgFill(vg);
			std::string nm = name;
			bool th = there;
			int col = colour;
			if (!module) {
				static const char* const N[8] = {"KICK", "SNARE", "HAT CL", "HAT OP", "BASS", "SUB", "PAD L", "PAD R"};
				nm = t < 8 ? N[t] : "";
				th = t < 8;
				col = 0;
			}
			float w = box.size.x;
			if (!th) {
				text(vg, boldFont(), w / 2, mm2px(5.4f), "—", 7.f, nvgRGB(0x3d, 0x40, 0x49), NVG_ALIGN_CENTER, 0.f);
			}
			else {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, mm2px(0.8f), mm2px(0.8f), w - mm2px(1.6f), mm2px(0.7f), 1.f);
				nvgFillColor(vg, wingCol(col, ACCENT));
				nvgFill(vg);
				text(vg, boldFont(), mm2px(1.2f), mm2px(4.4f), t < NT ? string::f("%02d", t + 1) : string::f("I%d", t - NT + 1),
				     5.f, FAINT, NVG_ALIGN_LEFT, 0.f);
				text(vg, boldFont(), w / 2 + mm2px(1.0f), mm2px(4.6f), string::uppercase(nm).substr(0, 9), 6.6f, WHITE,
				     NVG_ALIGN_CENTER, 0.2f);
				bool arm = module ? module->params[WingRec::ARM_PARAM + t].getValue() > 0.5f : true;
				nvgBeginPath(vg);
				nvgCircle(vg, w - mm2px(1.6f), mm2px(3.9f), mm2px(0.75f));
				nvgFillColor(vg, arm ? RECRED : nvgRGB(0x3d, 0x40, 0x49));
				nvgFill(vg);
				float p = module ? module->peak[t].load() : 0.5f;
				float db = p > 1e-6f ? 20.f * std::log10(p) : -100.f;
				float f = clamp((db + 60.f) / 60.f, 0.f, 1.f);
				float mw = w - mm2px(2.4f);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, mm2px(1.2f), mm2px(6.6f), mw, mm2px(1.3f), 1.f);
				nvgFillColor(vg, nvgRGB(0x1b, 0x1d, 0x22));
				nvgFill(vg);
				if (f > 0.f) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, mm2px(1.2f), mm2px(6.6f), mw * f, mm2px(1.3f), 1.f);
					nvgFillColor(vg, db > -0.5f ? ACCENT : (db > -12.f ? WARN : nvgRGB(0x7e, 0xe0, 0x8a)));
					nvgFill(vg);
				}
			}
			// a stereo pair: one frame round both
			if (module && (t % 2 == 0) && module->params[WingRec::ST_PARAM + t / 2].getValue() > 0.5f) {
				float gap = mm2px(0.8f);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, -1.f, -1.f, 2 * w + gap + 2.f, box.size.y + 2.f, 2.5f);
				nvgStrokeWidth(vg, 1.f);
				nvgStrokeColor(vg, nvgRGBA(0xff, 0x6b, 0x5b, 0xc0));
				nvgStroke(vg);
			}
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			Param& p = module->params[WingRec::ARM_PARAM + t];
			p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

template <int R, int G, int B>
using RecLatch = VCVLightLatch<MediumSimpleLight<ColorLight<R, G, B>>>;

static void chooseRoot(WingRec* m) {
	std::string start;
	{
		std::lock_guard<std::mutex> lock(m->planMutex);
		start = m->rootDir;
	}
	system::createDirectories(start);
	char* p = osdialog_file(OSDIALOG_OPEN_DIR, start.empty() ? nullptr : start.c_str(), nullptr, nullptr);
	if (!p)
		return;
	std::string r = p;
	std::free(p);
	while (r.size() > 3 && (r.back() == '\\' || r.back() == '/'))
		r.pop_back();
	{
		std::lock_guard<std::mutex> lock(m->planMutex);
		m->rootDir = r;
	}
	saveSettings(m);
}
static void chooseSplit(WingRec* m) {
	char* p = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
	if (!p)
		return;
	std::string r = p;
	std::free(p);
	std::lock_guard<std::mutex> lock(m->statusMutex);
	m->splitReq = r;
	m->splitMsg = "SPLIT …";
}

/** ABLAUF's row (from the song position at its start) → the row's name ("DROP 1"). */
static std::string ablaufName(Module* a, float pos) {
	if (!a || pos < 0.f)
		return "";
	json_t* j = a->dataToJson();
	if (!j)
		return "";
	std::string out;
	json_t* rows = json_object_get(j, "rows");
	json_t* slots = json_object_get(j, "slots");
	std::map<int, std::pair<std::string, int>> used;  // slot → name, tag
	if (slots && json_is_array(slots))
		for (size_t i = 0; i < json_array_size(slots); i++) {
			json_t* o = json_array_get(slots, i);
			json_t* n = json_object_get(o, "name");
			json_t* tg = json_object_get(o, "tag");
			json_t* ix = json_object_get(o, "i");
			int k = ix ? (int) json_integer_value(ix) : (int) i;
			used[k] = {n && json_is_string(n) ? json_string_value(n) : "", tg ? (int) json_integer_value(tg) : -1};
		}
	static const int BARS[6] = {1, 2, 4, 8, 16, 32};
	struct R {
		int bars, sec, slot;
	};
	std::vector<R> rs;
	int total = 0;
	if (rows && json_is_array(rows))
		for (size_t i = 0; i < json_array_size(rows); i++) {
			json_t* o = json_array_get(rows, i);
			int slot = (int) json_integer_value(json_object_get(o, "slot"));
			if (!used.count(slot))
				continue;
			R r;
			r.slot = slot;
			r.bars = BARS[clamp((int) json_integer_value(json_object_get(o, "bars")), 0, 5)];
			r.sec = (int) json_integer_value(json_object_get(o, "section"));
			rs.push_back(r);
			total += r.bars;
		}
	if (total > 0) {
		int target = (int) std::lround(pos / 10.f * total);
		int before = 0;
		for (const R& r : rs) {
			if (before >= target) {
				out = used[r.slot].first;
				if (out.empty())
					out = SECTIONS[clamp(r.sec, 0, 5)];
				break;
			}
			before += r.bars;
		}
	}
	json_decref(j);
	return out;
}

struct WingRecWidget : ModuleWidget {
	float scan = 0.f;
	WingRecWidget(WingRec* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingRec.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		RecDisplay* d = new RecDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		const float* tg[3] = {L::T_RACK, L::T_SD1, L::T_SD2};
		for (int i = 0; i < 3; i++) {
			TargetTile* t = new TargetTile;
			t->module = module;
			t->i = i;
			area(t, tg[i]);
		}
		RecButton* rb = new RecButton;
		rb->module = module;
		area(rb, L::RECBIG);
		addParam(createLightParamCentered<VCVLightBezelLatch<ColorLight<0xb8, 0xf2, 0x3a>>>(mm(L::SYNC), module,
		                                                                                     WingRec::SYNC_PARAM, WingRec::SYNC_LIGHT));
		addParam(createParamCentered<VCVBezel>(mm(L::STOP), module, WingRec::STOP_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::MARK), module, WingRec::MARK_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::PLAY), module, WingRec::PLAY_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::PAUSE), module, WingRec::PAUSE_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::SDSTOP), module, WingRec::SDSTOP_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::MPREV), module, WingRec::MPREV_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::MNEXT), module, WingRec::MNEXT_PARAM));
		addParam(createParamCentered<VCVButton>(mm(L::SPLIT), module, WingRec::SPLIT_PARAM));
		const float* fields[5] = {L::CARD, L::MODE, L::SESSION, L::TRACKS, L::MNUM};
		for (int k = 0; k < 5; k++) {
			MenuField* f = new MenuField;
			f->module = module;
			f->kind = k;
			area(f, fields[k]);
		}
		const float* tr[NREC] = {L::TR1,  L::TR2,  L::TR3,  L::TR4,  L::TR5,  L::TR6,  L::TR7,  L::TR8,
		                         L::TR9,  L::TR10, L::TR11, L::TR12, L::TR13, L::TR14, L::TR15, L::TR16,
		                         L::TR17, L::TR18, L::TR19, L::TR20, L::TR21, L::TR22, L::TR23, L::TR24};
		for (int t = 0; t < NREC; t++) {
			TrackTile* w = new TrackTile;
			w->module = module;
			w->t = t;
			area(w, tr[t]);
		}
		const float* sts[NPAIR] = {L::ST1, L::ST2, L::ST3, L::ST4, L::ST5, L::ST6,
		                           L::ST7, L::ST8, L::ST9, L::ST10, L::ST11, L::ST12};
		for (int p = 0; p < NPAIR; p++)
			addParam(createLightParamCentered<RecLatch<0xff, 0x6b, 0x5b>>(mm(sts[p]), module, WingRec::ST_PARAM + p,
			                                                             WingRec::ST_LIGHT + p));
		const float* ins[NT] = {L::IN1, L::IN2,  L::IN3,  L::IN4,  L::IN5,  L::IN6,  L::IN7,  L::IN8,
		                        L::IN9, L::IN10, L::IN11, L::IN12, L::IN13, L::IN14, L::IN15, L::IN16};
		for (int t = 0; t < NT; t++)
			addInput(createInputCentered<PJ301MPort>(mm(ins[t]), module, WingRec::IN_INPUT + t));
		addInput(createInputCentered<PJ301MPort>(mm(L::TRIG_REC), module, WingRec::TRIG_REC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm(L::TRIG_STOP), module, WingRec::TRIG_STOP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm(L::TRIG_MARK), module, WingRec::TRIG_MARK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm(L::TRIG_SDPLAY), module, WingRec::TRIG_SDPLAY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm(L::IN_CLOCK), module, WingRec::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm(L::IN_RUN), module, WingRec::RUN_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm(L::GATE_REC), module, WingRec::GATE_REC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm(L::GATE_SDPLAY), module, WingRec::GATE_SDPLAY_OUTPUT));
	}

	/** GENRA and ABLAUF in the patch (the GENRA plugin): their ids and the outputs WING-REC reads. */
	void findTransport(WingRec* m) {
		int64_t gid = -1, aid = -1;
		int gr = -1, gb = -1, ar = -1, ap = -1;
		float bpm = 0.f;
		std::string genre;
		for (int64_t id : APP->engine->getModuleIds()) {
			Module* x = APP->engine->getModule(id);
			if (!x || !x->model || !x->model->plugin || x->model->plugin->slug != "Genra")
				continue;
			auto out = [&](const char* prefix) {
				for (int i = 0; i < (int) x->outputInfos.size(); i++)
					if (x->outputInfos[i] && string::startsWith(x->outputInfos[i]->name, prefix))
						return i;
				return -1;
			};
			if (x->model->slug == "Genra" && gid < 0) {
				gid = id;
				gr = out("Run (");
				gb = out("Bar (");
				for (ParamQuantity* q : x->paramQuantities) {
					if (!q)
						continue;
					if (q->name == "Tempo (internal clock)")
						bpm = q->getValue();
					else if (q->name == "Genre (global)")
						genre = q->getDisplayValueString();
				}
			}
			else if (x->model->slug == "GenraAblauf" && aid < 0) {
				aid = id;
				ar = out("Row (");
				ap = out("Song position");
			}
		}
		m->genraRun = gr;
		m->genraBar = gb;
		m->genraId = gr >= 0 && gb >= 0 ? gid : -1;
		m->ablaufRow = ar;
		m->ablaufPos = ap;
		m->ablaufId = ar >= 0 ? aid : -1;
		if (gid >= 0)
			m->bpm = bpm;
		std::lock_guard<std::mutex> lock(m->planMutex);
		m->genre = genre;
	}
	/** The files the next take writes (names, colours, stereo pairs). */
	void makePlan(WingRec* m, WingLink* h) {
		std::vector<RecPlan> pl;
		std::string name[NREC];
		int col[NREC];
		bool there[NREC];
		for (int t = 0; t < NREC; t++) {
			name[t] = trackName(m, h, t, &col[t], &there[t]);
			there[t] = there[t] && m->params[WingRec::ARM_PARAM + t].getValue() > 0.5f;
		}
		for (int t = 0; t < NREC; t++) {
			if (!there[t])
				continue;
			RecPlan p;
			p.a = t;
			p.colour = col[t];
			std::string n = name[t];
			if (t % 2 == 0 && t + 1 < NREC && there[t + 1] && m->params[WingRec::ST_PARAM + t / 2].getValue() > 0.5f) {
				p.b = t + 1;
				// "PAD L" + "PAD R" → "PAD"
				std::string a = n, b = name[t + 1];
				if (a.size() > 2 && b.size() > 2 && a.substr(0, a.size() - 1) == b.substr(0, b.size() - 1) &&
				    (a.back() == 'L' || a.back() == 'l'))
					n = a.substr(0, a.size() - 1);
				while (!n.empty() && (n.back() == ' ' || n.back() == '_' || n.back() == '-'))
					n.pop_back();
				if (n.empty())
					n = a;
			}
			p.name = string::f("%02d ", t + 1) + n;
			pl.push_back(p);
			if (p.b >= 0)
				t++;
		}
		std::lock_guard<std::mutex> lock(m->planMutex);
		m->plan = pl;
	}
	/** The WING's stereo sources: a pair takes it when the WING changes it (ST stays as set here otherwise). */
	void stereoFromWing(WingRec* m, WingLink* h) {
		for (int p = 0; p < NT / 2; p++) {
			if (!hostStripActive(h, 2 * p))
				continue;
			int s = hostStripMeta(h, 2 * p).stereo;
			if (s < 0 || s == m->stereoSeen[p])
				continue;
			m->stereoSeen[p] = s;
			m->params[WingRec::ST_PARAM + p].setValue((float) s);
		}
	}
	/** ABLAUF markers: their names. */
	void nameMarks(WingRec* m) {
		std::vector<std::pair<size_t, float>> todo;
		{
			std::lock_guard<std::mutex> lock(m->markMutex);
			for (size_t i = 0; i < m->marks.size(); i++)
				if (m->marks[i].name.empty())
					todo.push_back({i, m->marks[i].pos});
		}
		if (todo.empty())
			return;
		Module* a = m->ablaufId >= 0 ? APP->engine->getModule(m->ablaufId) : nullptr;
		for (auto& t : todo) {
			std::string n = ablaufName(a, t.second);
			std::lock_guard<std::mutex> lock(m->markMutex);
			if (t.first < m->marks.size() && m->marks[t.first].name.empty())
				m->marks[t.first].name = n.empty() ? string::f("PART %d", (int) t.first + 1) : n;
		}
	}
	void step() override {
		ModuleWidget::step();
		WingRec* m = dynamic_cast<WingRec*>(module);
		if (!m)
			return;
		m->eng = APP->engine;
		float dt = frameDt();
		nameMarks(m);
		if (m->splitClick.exchange(false))
			chooseSplit(m);
		// the SD session's name, a moment after STOP (the WING names it only when stopped)
		if (m->namePending.exchange(false)) {
			m->nameLater = 1.5f;
			std::string g;
			{
				std::lock_guard<std::mutex> lock(m->planMutex);
				g = m->genre;
			}
			std::time_t now = std::time(nullptr);
			char ts[16];
			std::strftime(ts, sizeof ts, "%m%d %H%M", std::localtime(&now));
			m->nameNext = (std::string(ts) + (g.empty() ? "" : " " + g)).substr(0, 19);
		}
		if (m->nameLater > 0.f) {
			m->nameLater = m->nameLater - dt;
			if (m->nameLater <= 0.f) {
				m->nameLater = -1.f;
				m->sdAll("$ctl.namesession=" + m->nameNext, true);
			}
		}
		scan -= dt;
		if (scan > 0.f)
			return;
		scan = 0.25f;
		linkPanels(static_cast<Satellite*>(m));
		WingLink* h = m->hostSafe();
		findTransport(m);
		stereoFromWing(m, h);
		if (m->state != WingRec::RECORDING)
			makePlan(m, h);
	}
	void appendContextMenu(Menu* menu) override {
		WingRec* m = dynamic_cast<WingRec*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(new MenuSeparator);
		std::string root, dir;
		int bits;
		bool als, autoM;
		{
			std::lock_guard<std::mutex> lock(m->planMutex);
			root = m->rootDir;
			bits = m->bits;
			als = m->writeAls;
			autoM = m->autoMarks;
		}
		{
			std::lock_guard<std::mutex> lock(m->statusMutex);
			dir = m->takeDir;
		}
		menu->addChild(createMenuLabel("Recordings: " + root));
		menu->addChild(createMenuItem("Choose the recordings folder…", "", [=]() { chooseRoot(m); }));
		menu->addChild(createMenuItem("Open the recordings folder", "", [=]() {
			system::createDirectories(root);
			system::openDirectory(root);
		}));
		if (!dir.empty())
			menu->addChild(createMenuItem("Open the last take", "", [=]() { system::openDirectory(dir); }));
		menu->addChild(createSubmenuItem("WAV format", bits == 32 ? "32 bit float" : "24 bit", [=](Menu* sub) {
			for (int b : {24, 32})
				sub->addChild(createCheckMenuItem(b == 32 ? "32 bit float" : "24 bit", "", [=]() { return m->bits == b; }, [=]() {
					{
						std::lock_guard<std::mutex> lock(m->planMutex);
						m->bits = b;
					}
					saveSettings(m);
				}));
		}));
		menu->addChild(createBoolMenuItem("Markers at every ABLAUF row", "", [=]() { return m->autoMarks.load(); }, [=](bool v) {
			{
				std::lock_guard<std::mutex> lock(m->planMutex);
				m->autoMarks = v;
			}
			saveSettings(m);
		}));
		menu->addChild(createBoolMenuItem("Write an Ableton Live set (Live 12)", "", [=]() { return m->writeAls.load(); }, [=](bool v) {
			{
				std::lock_guard<std::mutex> lock(m->planMutex);
				m->writeAls = v;
			}
			saveSettings(m);
		}));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("SPLIT an SD session… (its folder)", "", [=]() { chooseSplit(m); }));
		(void) als;
		(void) autoM;
	}
};

}  // namespace winglink

Model* modelWingRec = createModel<winglink::WingRec, winglink::WingRecWidget>("WingRec");

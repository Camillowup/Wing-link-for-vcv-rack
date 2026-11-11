// SPDX-License-Identifier: GPL-3.0-only
// WING-IN – audio from the WING into Rack: 8 of the WING's USB outputs (“USB n OUT”, from the WING to the computer)
// as 8 strips. Each strip says what its USB output takes on the WING: a physical input (LCL, AUX, A, B, C …) or a
// channel, aux, bus, main or matrix (through the user signal USR 1–8 of its strip). SYNC writes that routing to the
// WING; WING-LINK reads it back when the patch loads. The audio comes from WING-LINK's audio device (its inputs).
// Panel: tools/wingin_panel.py.
#include "WingLink.hpp"
#include "WingInLayout.hpp"

namespace winglink {

namespace L = inlayout;
using namespace wlui;

std::string inSourceLabel(const InSource& s) {
	if (s.grp <= IG_OFF || s.grp >= NIG)
		return "OFF";
	return string::f("%s %d", IG_NAMES[s.grp], s.n);
}

WingIn::WingIn() {
	config(NUM_PARAMS, 0, NUM_OUTPUTS, NUM_LIGHTS);
	for (int i = 0; i < NIN; i++) {
		known[i] = false;
		audio[i] = 0.f;
		peak[i] = 0.f;
		configParam(LVL_PARAM + i, 0.f, 2.f, 1.f, string::f("USB OUT %d level", i + 1), " dB", -10.f, 20.f);
		configOutput(OUTPUT + i, string::f("Audio %d (±5 V)", i + 1));
	}
	configButton(DOWN_PARAM, "USB outputs − 8");
	configButton(UP_PARAM, "USB outputs + 8");
	configButton(SYNC_PARAM, "SYNC: write the sources to the WING");
}

void WingIn::process(const ProcessArgs& args) {
	if (args.sampleTime != decayTime) {
		decayTime = args.sampleTime;
		decay = std::exp(-args.sampleTime / 0.25f);
	}
	for (int i = 0; i < NIN; i++) {
		float a = audio[i].load(std::memory_order_relaxed) * params[LVL_PARAM + i].getValue();
		outputs[OUTPUT + i].setVoltage(5.f * a);
		float p = std::max(std::fabs(a), peak[i].load(std::memory_order_relaxed) * decay);
		peak[i].store(p, std::memory_order_relaxed);
	}
}

json_t* WingIn::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "start", json_integer(start));
	json_t* a = json_array();
	{
		std::lock_guard<std::mutex> lock(srcMutex);
		for (int i = 0; i < NIN; i++) {
			json_t* o = json_object();
			json_object_set_new(o, "grp", json_integer(src[i].grp));
			json_object_set_new(o, "n", json_integer(src[i].n));
			json_object_set_new(o, "tap", json_integer(src[i].tap));
			json_object_set_new(o, "lr", json_integer(src[i].lr));
			json_array_append_new(a, o);
		}
	}
	json_object_set_new(root, "sources", a);
	return root;
}
void WingIn::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "start"))
		start = clamp((int) json_integer_value(j), 1, 41);
	json_t* a = json_object_get(root, "sources");
	if (!a || !json_is_array(a))
		return;
	std::lock_guard<std::mutex> lock(srcMutex);
	for (int i = 0; i < NIN && i < (int) json_array_size(a); i++) {
		json_t* o = json_array_get(a, i);
		InSource s;
		if (json_t* j = json_object_get(o, "grp"))
			s.grp = clamp((int) json_integer_value(j), 0, NIG - 1);
		if (json_t* j = json_object_get(o, "n"))
			s.n = clamp((int) json_integer_value(j), 1, std::max(1, igMax(s.grp)));
		if (json_t* j = json_object_get(o, "tap"))
			s.tap = clamp((int) json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(o, "lr"))
			s.lr = clamp((int) json_integer_value(j), 0, 2);
		src[i] = s;
	}
}

static InSource inGet(WingIn* m, int i) {
	std::lock_guard<std::mutex> lock(m->srcMutex);
	return m->src[i];
}
static void inSet(WingIn* m, int i, const InSource& s) {
	{
		std::lock_guard<std::mutex> lock(m->srcMutex);
		m->src[i] = s;
	}
	m->known[i] = true;
	m->pending = true;
}

// ---------------------------------------------------------------- the display
struct InDisplay : Widget {
	WingIn* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			WingLink* h = module ? module->hostSafe() : nullptr;
			int st = module ? module->start.load() : 1;
			float x = mm2px(2.6f);
			text(vg, boldFont(), x, mm2px(5.5f), string::f("AUDIO  ←  USB %d–%d OUT", st, st + NIN - 1), 12.f, WHITE,
			     NVG_ALIGN_LEFT, 0.6f);
			std::string s;
			NVGcolor sc = OK;
			if (!module)
				s = "●  WING → RACK · 48 kHz";
			else if (!h) {
				s = "●  right-click › Connect to WING-LINK";
				sc = WARN;
			}
			else if (!hostLinkedSat(h, module)) {
				s = "●  WING-LINK already has a WING-IN";
				sc = WARN;
			}
			else if (!module->hasAudio) {
				s = "●  NO INPUTS FROM THE AUDIO DEVICE OF WING-LINK";
				sc = WARN;
			}
			else if (module->pending) {
				s = "●  CHANGED HERE · PRESS SYNC TO SEND TO THE WING";
				sc = WARN;
			}
			else if (!hostSysexOk(h)) {
				s = "●  AUDIO OK · ROUTING NOT READ (NO SYSEX)";
				sc = WARN;
			}
			else {
				float r = hostDeviceRate(h);
				s = "●  WING → RACK";
				if (r > 0.f)
					s += string::f(" · %.0f kHz", r / 1000.f);
			}
			text(vg, boldFont(), x, mm2px(8.9f), s, 6.2f, sc, NVG_ALIGN_LEFT, 0.4f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** The first USB output (between ◀ and ▶). */
struct InStart : Widget {
	WingIn* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			int st = module ? module->start.load() : 1;
			text(args.vg, boldFont(), box.size.x / 2, box.size.y / 2 + 2.5f, string::f("%d OUT", st), 8.f, WHITE,
			     NVG_ALIGN_CENTER, 0.4f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** A strip's source: what, its name and colour on the WING, PRE/POST and L/R; a click picks it. */
struct InSrc : OpaqueWidget {
	WingIn* module = nullptr;
	int i = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			InSource s;
			bool known = true;
			if (module) {
				s = inGet(module, i);
				known = module->known[i];
			}
			else {
				static const int G[NIN] = {IG_LCL, IG_LCL, IG_CH, IG_CH, IG_BUS, IG_MAIN, IG_OFF, IG_OFF};
				static const int N[NIN] = {1, 2, 1, 2, 1, 1, 1, 1};
				s.grp = G[i];
				s.n = N[i];
			}
			WingLink* h = module ? module->hostSafe() : nullptr;
			int key = inSourceKey(s);
			Meta m = (h && key >= 0) ? hostMetaOf(h, key) : Meta();
			NVGcolor col = s.grp == IG_OFF ? OFF : (igInternal(s.grp) ? ACCENT : DIM);
			if (m.colour >= 1 && m.colour <= P::NUM_COLOURS)
				col = nvgRGB(P::COLOURS[m.colour - 1].r, P::COLOURS[m.colour - 1].g, P::COLOURS[m.colour - 1].b);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, mm2px(0.6f), mm2px(0.6f), box.size.x - mm2px(1.2f), mm2px(1.1f), 1.f);
			nvgFillColor(vg, col);
			nvgFill(vg);
			float cx = box.size.x / 2;
			text(vg, boldFont(), cx, mm2px(4.8f), known ? inSourceLabel(s) : "…", 7.2f, s.grp == IG_OFF ? FAINT : WHITE,
			     NVG_ALIGN_CENTER, 0.3f);
			std::string name = m.name;
			if (!module && i == 2)
				name = "Kick";
			if (!module && i == 3)
				name = "Snare";
			if (!name.empty())
				text(vg, boldFont(), cx, mm2px(8.f), string::uppercase(name).substr(0, 8), 5.6f, TXT, NVG_ALIGN_CENTER,
				     0.2f);
			if (igInternal(s.grp)) {
				static const char* LR[3] = {"L+R", "L", "R"};
				text(vg, boldFont(), cx, mm2px(11.4f), string::f("%s · %s", s.tap ? "POST" : "PRE", LR[clamp(s.lr, 0, 2)]),
				     5.f, FAINT, NVG_ALIGN_CENTER, 0.2f);
			}
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module) {
			OpaqueWidget::onButton(e);
			return;
		}
		WingIn* m = module;
		int k = i;
		InSource cur = inGet(m, k);
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("USB %d OUT  ·  source", clamp(m->start.load(), 1, 41) + k)));
		menu->addChild(createCheckMenuItem("OFF", "", [=]() { return inGet(m, k).grp == IG_OFF; }, [=]() {
			InSource s = inGet(m, k);
			s.grp = IG_OFF;
			inSet(m, k, s);
		}));
		auto pick = [=](int g, int from, int to) {
			return [=](ui::Menu* sub) {
				for (int n = from; n <= to; n++)
					sub->addChild(createCheckMenuItem(string::f("%s %d", IG_NAMES[g], n), "", [=]() {
						InSource s = inGet(m, k);
						return s.grp == g && s.n == n;
					}, [=]() {
						InSource s = inGet(m, k);
						s.grp = g;
						s.n = n;
						inSet(m, k, s);
					}));
			};
		};
		auto item = [&](const char* label, int g, int from, int to) {
			bool here = cur.grp == g && cur.n >= from && cur.n <= to;
			menu->addChild(createSubmenuItem(label, here ? inSourceLabel(cur) : "", pick(g, from, to)));
		};
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("An input of the WING"));
		item("LOCAL 1–24", IG_LCL, 1, 24);
		item("AUX IN 1–8", IG_AUX, 1, 8);
		item("A 1–48", IG_A, 1, 48);
		item("B 1–48", IG_B, 1, 48);
		item("C 1–48", IG_C, 1, 48);
		item("SC 1–32", IG_SC, 1, 32);
		item("USB IN 1–48 (from the computer)", IG_USB, 1, 48);
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(string::f("A signal of the WING (through USR %d)", k + 1)));
		item("CH 1–20", IG_CH, 1, 20);
		item("CH 21–40", IG_CH, 21, 40);
		item("AUX 1–8", IG_AUXCH, 1, 8);
		item("BUS 1–16", IG_BUS, 1, 16);
		item("MAIN 1–4", IG_MAIN, 1, 4);
		item("MATRIX 1–8", IG_MTX, 1, 8);
		if (igInternal(cur.grp)) {
			menu->addChild(new MenuSeparator);
			auto tap = [=](const char* l, int t) {
				return createCheckMenuItem(l, "", [=]() { return inGet(m, k).tap == t; }, [=]() {
					InSource s = inGet(m, k);
					s.tap = t;
					inSet(m, k, s);
				});
			};
			auto lr = [=](const char* l, int v) {
				return createCheckMenuItem(l, "", [=]() { return inGet(m, k).lr == v; }, [=]() {
					InSource s = inGet(m, k);
					s.lr = v;
					inSet(m, k, s);
				});
			};
			menu->addChild(tap("PRE (before the fader)", 0));
			menu->addChild(tap("POST (after the fader)", 1));
			menu->addChild(lr("L+R (mono sum)", 0));
			menu->addChild(lr("L", 1));
			menu->addChild(lr("R", 2));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Then SYNC sends it to the WING"));
		e.consume(this);
	}
};

/** A strip's meter (its peak, dB). */
struct InMeter : Widget {
	WingIn* module = nullptr;
	int i = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float p = module ? module->peak[i].load() : 0.3f + 0.08f * i;
			float db = p > 1e-6f ? 20.f * std::log10(p) : -100.f;
			float f = clamp((db + 60.f) / 60.f, 0.f, 1.f);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 1.f);
			nvgFillColor(vg, DARK);
			nvgFill(vg);
			if (f > 0.f) {
				float hgt = box.size.y * f;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, 0, box.size.y - hgt, box.size.x, hgt, 1.f);
				nvgFillColor(vg, db > -0.5f ? ACCENT : (db > -12.f ? WARN : OK));
				nvgFill(vg);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

struct WingInWidget : ModuleWidget {
	float scan = 0.f;
	WingInWidget(WingIn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingIn.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		InDisplay* d = new InDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		InStart* st = new InStart;
		st->module = module;
		area(st, L::START);
		addParam(createParamCentered<VCVBezel>(mm(L::DOWN), module, WingIn::DOWN_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::UP), module, WingIn::UP_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<ColorLight<0xb8, 0xf2, 0x3a>>>(mm(L::SYNC), module,
		                                                                                WingIn::SYNC_PARAM, WingIn::SYNC_LIGHT));
		const float* srcs[NIN] = {L::SRC1, L::SRC2, L::SRC3, L::SRC4, L::SRC5, L::SRC6, L::SRC7, L::SRC8};
		const float* meters[NIN] = {L::METER1, L::METER2, L::METER3, L::METER4, L::METER5, L::METER6, L::METER7, L::METER8};
		const float* lvls[NIN] = {L::LVL1, L::LVL2, L::LVL3, L::LVL4, L::LVL5, L::LVL6, L::LVL7, L::LVL8};
		const float* outs[NIN] = {L::OUT1, L::OUT2, L::OUT3, L::OUT4, L::OUT5, L::OUT6, L::OUT7, L::OUT8};
		for (int i = 0; i < NIN; i++) {
			InSrc* s = new InSrc;
			s->module = module;
			s->i = i;
			area(s, srcs[i]);
			InMeter* me = new InMeter;
			me->module = module;
			me->i = i;
			area(me, meters[i]);
			addParam(createParamCentered<GSmallKnob>(mm(lvls[i]), module, WingIn::LVL_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm(outs[i]), module, WingIn::OUTPUT + i));
		}
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
		WingIn* m = dynamic_cast<WingIn*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(createMenuLabel("Click a strip's source to choose it, then SYNC"));
		menu->addChild(createMenuLabel("WING-LINK's audio device needs inputs (the WING's USB OUT)"));
	}
};

}  // namespace winglink

Model* modelWingIn = createModel<winglink::WingIn, winglink::WingInWidget>("WingIn");

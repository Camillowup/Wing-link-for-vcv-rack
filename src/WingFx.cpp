// SPDX-License-Identifier: GPL-3.0-only
// WING-FX – the WING's 16 effects, 4 at a time, as a panel of WING-LINK (which does the talking: src/WingLink.cpp).
// Per slot: ON, the model (a click: PREMIUM / STANDARD), MIX and four parameters that come with the model, CV for
// MIX and the four. The tempo: SYNC on → the delays take Rack's tempo (CLOCK in or TAP); SYNC off → the WING's own
// (the first delay's time), and CLOCK out sends the tempo in use. Panel: tools/wingfx_panel.py.
#include "WingLink.hpp"
#include "WingFxLayout.hpp"
#include <sstream>

namespace winglink {

namespace L = fxlayout;
namespace F = wingfx;
using namespace wlui;

static const NVGcolor SLOT_COLOURS[8] = {nvgRGB(0xff, 0x6b, 0x5b), nvgRGB(0x3d, 0xc9, 0xff), nvgRGB(0xe0, 0x77, 0x9e),
                                         nvgRGB(0xb8, 0xf2, 0x3a), nvgRGB(0x7b, 0x6c, 0xff), nvgRGB(0xff, 0xd8, 0x4a),
                                         nvgRGB(0x5d, 0xd9, 0xa4), nvgRGB(0xc9, 0xcc, 0xd4)};

/** A knob's value as the effect's parameter (the model of the slot shown). */
struct FxQuantity : ParamQuantity {
	int j = 0, f = FMIX;
	const F::Param* param() {
		WingFx* m = dynamic_cast<WingFx*>(module);
		WingLink* h = m ? m->hostSafe() : nullptr;
		FxSlot* x = hostFx(h, m ? m->first + j : 0);
		if (!x)
			return nullptr;
		return m->knob(x->model, f - FP1);
	}
	std::string getLabel() override {
		if (f == FMIX)
			return string::f("FX %d mix", j + 1);
		const F::Param* p = param();
		return p ? std::string(p->label) : "(not used by this effect)";
	}
	std::string getDisplayValueString() override {
		if (f == FMIX)
			return string::f("%.0f", getValue() * 100.f);
		const F::Param* p = param();
		return p ? F::panelText(*p, F::toValue(*p, getValue())) : "–";
	}
	std::string getUnit() override { return f == FMIX ? " %" : ""; }
};

WingFx::WingFx() {
	for (int m = 0; m < F::NUM_MODELS; m++)
		for (int k = 0; k < 4; k++)
			choice[m][k] = -1;
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	for (int j = 0; j < NPAGE; j++) {
		configSwitch(fxPid(j, FON), 0.f, 1.f, 1.f, string::f("Slot %d on", j + 1), {"Off", "On"});
		FxQuantity* q = configParam<FxQuantity>(fxPid(j, FMIX), 0.f, 1.f, 1.f, "");
		q->j = j;
		q->f = FMIX;
		for (int k = 0; k < 4; k++) {
			FxQuantity* p = configParam<FxQuantity>(fxPid(j, FP1 + k), 0.f, 1.f, 0.5f, "");
			p->j = j;
			p->f = FP1 + k;
		}
		const char* cvn[5] = {"MIX", "P1", "P2", "P3", "P4"};
		for (int k = 0; k < 5; k++)
			configInput(fxCv(j, k), string::f("Slot %d %s CV (0–10 V)", j + 1, cvn[k]));
	}
	configButton(PAGE_DOWN_PARAM, "Slots back");
	configButton(PAGE_UP_PARAM, "Slots on");
	configButton(TAP_PARAM, "TAP tempo");
	configSwitch(SYNC_PARAM, 0.f, 1.f, 1.f, "SYNC", {"Off: the WING's tempo (tap on the WING)", "On: the delays take Rack's tempo"});
	configInput(CLOCK_INPUT, "Clock (Rack's tempo for the delays)");
	configOutput(CLOCK_OUTPUT, "Clock (the tempo in use)");
}

json_t* WingFx::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "first", json_integer(first));
	json_object_set_new(root, "slotStep", json_integer(slotStep));
	json_t* ch = json_object();
	for (int m = 1; m < F::NUM_MODELS; m++) {
		bool any = false;
		json_t* a = json_array();
		for (int k = 0; k < 4; k++) {
			int c = choice[m][k];
			any |= c >= 0;
			json_array_append_new(a, c >= 0 ? json_string(F::MODELS[m].all[c].key) : json_null());
		}
		if (any)
			json_object_set_new(ch, F::MODELS[m].mdl, a);
		else
			json_decref(a);
	}
	json_object_set_new(root, "knobs", ch);
	json_object_set_new(root, "ppqIn", json_integer(ppqIn));
	json_object_set_new(root, "ppqOut", json_integer(ppqOut));
	return root;
}
void WingFx::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "first"))
		first = clamp((int) json_integer_value(j), 0, NSLOT - NPAGE);
	if (json_t* j = json_object_get(root, "slotStep"))
		slotStep = json_integer_value(j) == 1 ? 1 : 4;
	if (json_t* ch = json_object_get(root, "knobs")) {
		const char* key;
		json_t* a;
		json_object_foreach(ch, key, a) {
			int m = F::modelIndex(key);
			if (m <= 0)
				continue;
			for (int k = 0; k < 4 && k < (int) json_array_size(a); k++) {
				json_t* v = json_array_get(a, k);
				choice[m][k] = json_is_string(v) ? F::findKey(F::MODELS[m], json_string_value(v)) : -1;
			}
		}
	}
	if (json_t* j = json_object_get(root, "ppqIn"))
		ppqIn = clamp((int) json_integer_value(j), 1, 24);
	if (json_t* j = json_object_get(root, "ppqOut"))
		ppqOut = clamp((int) json_integer_value(j), 1, 24);
}

// ---------------------------------------------------------------- widgets
static int slotOf(WingFx* m, int j) { return (m ? m->first.load() : 0) + j; }

/** The slot's head: colour bar, FX n, PREMIUM / STANDARD. */
struct HeadWidget : Widget {
	WingFx* module = nullptr;
	int j = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			int slot = slotOf(module, j);
			NVGcolor c = SLOT_COLOURS[slot % 8];
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, 0, 0, box.size.x, mm2px(0.9f), 1.2f);
			nvgFillColor(args.vg, c);
			nvgFill(args.vg);
			float adv = text(args.vg, boldFont(), mm2px(1.4f), mm2px(4.4f), string::f("FX %d", slot + 1), 9.f, c,
			                 NVG_ALIGN_LEFT, 1.5f);
			bool prem = slot < 8;
			text(args.vg, boldFont(), mm2px(1.4f) + adv + mm2px(1.6f), mm2px(4.2f), prem ? "PREMIUM" : "STANDARD", 5.6f,
			     prem ? WARN : DIM, NVG_ALIGN_LEFT, 0.8f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** The model: its name; a click opens the list (PREMIUM: only FX 1–8). */
struct ModelWidget : OpaqueWidget {
	WingFx* module = nullptr;
	int j = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			WingLink* h = module ? module->hostSafe() : nullptr;
			int slot = slotOf(module, j);
			FxSlot* x = hostFx(h, slot);
			std::string name = !module ? "HALL" : (x ? F::MODELS[x->model].mdl : "–");
			text(args.vg, boldFont(), mm2px(1.5f), mm2px(4.3f), name, 10.f, WHITE, NVG_ALIGN_LEFT, 0.8f);
			float ax = box.size.x - mm2px(3.4f), ay = mm2px(2.5f);
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, ax, ay);
			nvgLineTo(args.vg, ax + mm2px(2.2f), ay);
			nvgLineTo(args.vg, ax + mm2px(1.1f), ay + mm2px(1.4f));
			nvgClosePath(args.vg);
			nvgFillColor(args.vg, FAINT);
			nvgFill(args.vg);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module) {
			OpaqueWidget::onButton(e);
			return;
		}
		WingLink* h = module->hostSafe();
		int slot = slotOf(module, j);
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("FX %d  ·  effect", slot + 1)));
		if (!h) {
			menu->addChild(createMenuLabel("not linked to WING-LINK"));
			e.consume(this);
			return;
		}
		FxSlot* x = hostFx(h, slot);
		auto tab = [=](bool premium) {
			return [=](ui::Menu* sub) {
				if (premium && slot >= 8) {
					sub->addChild(createMenuLabel("Premium effects run on FX 1–8 only"));
					return;
				}
				for (int g = 0; g < F::NUM_GROUPS; g++) {
					if (F::GROUPS[g].premium != premium)
						continue;
					sub->addChild(createMenuLabel(F::GROUPS[g].name));
					std::istringstream ss(F::GROUPS[g].models);
					std::string mname;
					while (ss >> mname) {
						int idx = F::modelIndex(mname);
						if (idx <= 0)
							continue;
						std::string right = F::MODELS[idx].name;
						sub->addChild(createCheckMenuItem(F::MODELS[idx].mdl, right, [=]() { return x->model == idx; },
						                                  [=]() { requestFxModel(h, slot, idx); }));
					}
				}
			};
		};
		menu->addChild(createSubmenuItem("PREMIUM", slot < 8 ? "reverb, delay, modulation, pitch" : "FX 1–8 only", tab(true)));
		menu->addChild(createSubmenuItem("STANDARD", "EQ, dynamics, amps, more", tab(false)));
		menu->addChild(new MenuSeparator);
		menu->addChild(createCheckMenuItem("None (empty slot)", "", [=]() { return x->model == 0; },
		                                   [=]() { requestFxModel(h, slot, 0); }));
		e.consume(this);
	}
};

/** A parameter's name and value under its knob. */
struct LabelWidget : Widget {
	WingFx* module = nullptr;
	int j = 0, k = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			std::string name = "–", value;
			static const char* MOCK[4] = {"DECAY", "PRE", "SIZE", "DAMP"};
			if (!module)
				name = MOCK[k];
			else if (FxSlot* x = hostFx(module->hostSafe(), slotOf(module, j))) {
				if (const F::Param* p = module->knob(x->model, k)) {
					name = p->label;
					value = F::panelText(*p, F::toValue(*p, module->params[fxPid(j, FP1 + k)].getValue()));
				}
			}
			float cx = box.size.x / 2;
			text(args.vg, boldFont(), cx, mm2px(1.4f), name, 5.8f, value.empty() ? FAINT : TXT, NVG_ALIGN_CENTER, 0.5f);
			if (!value.empty())
				text(args.vg, boldFont(), cx, mm2px(3.7f), value, 5.6f, DIM, NVG_ALIGN_CENTER, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

struct FxDisplay : Widget {
	WingFx* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			float x = mm2px(3.f);
			int p = module ? module->first.load() : 0;
			text(vg, boldFont(), x, mm2px(4.6f), string::f("FX %d – %d", p + 1, p + NPAGE), 12.f, WHITE, NVG_ALIGN_LEFT, 0.5f);
			if (!module) {
				Widget::drawLayer(args, layer);
				return;
			}
			WingLink* h = module->hostSafe();
			bool sync = module->params[WingFx::SYNC_PARAM].getValue() > 0.5f;
			if (!h)
				text(vg, boldFont(), x, mm2px(8.1f), "●  right-click › Connect to WING-LINK", 6.4f, WARN, NVG_ALIGN_LEFT, 0.3f);
			else if (!hostLinked(h, module))
				text(vg, boldFont(), x, mm2px(8.1f), "●  WING-LINK already has a WING-FX", 6.4f, WARN, NVG_ALIGN_LEFT, 0.3f);
			else
				text(vg, boldFont(), x, mm2px(8.1f),
				     sync ? "●  LINKED TO WING-LINK  ·  SYNC: RACK TEMPO" : "●  LINKED TO WING-LINK  ·  SYNC OFF: WING TEMPO",
				     6.4f, ACCENT, NVG_ALIGN_LEFT, 0.3f);
			std::string info;
			static const char* RES[] = {"", "QUARTERS", "8THS", "", "16THS"};
			int pin = module->ppqIn, pout = module->ppqOut;
			std::string rin = pin == 24 ? "24 PPQN" : (pin <= 4 ? RES[pin] : string::f("%d PPQN", pin));
			std::string rout = pout == 24 ? "24 PPQN" : (pout <= 4 ? RES[pout] : string::f("%d PPQN", pout));
			if (sync)
				info = module->inputs[WingFx::CLOCK_INPUT].isConnected() ? "CLOCK IN " + rin : "TAP (or a clock in CLOCK IN)";
			else {
				int ws = -1;
				float wb = hostWingBpm(h, &ws);
				info = wb > 0.f ? string::f("FROM FX %d's TIME", ws + 1) : "NO DELAY ON THE WING";
			}
			info += "  ·  CLOCK OUT " + rout;
			text(vg, boldFont(), x, mm2px(10.2f), info, 6.f, DIM, NVG_ALIGN_LEFT, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** SLOTS step: 1 or 4 (a click switches). */
struct FxStep : OpaqueWidget {
	WingFx* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			text(args.vg, boldFont(), box.size.x / 2, box.size.y / 2 + 5.f, string::f("%d", module ? module->slotStep.load() : 4),
			     14.f, WHITE, NVG_ALIGN_CENTER, 0.f);
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			module->slotStep = module->slotStep == 4 ? 1 : 4;
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}
};

struct TempoWidget : Widget {
	WingFx* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			float bpm = 124.f;
			if (module) {
				WingLink* h = module->hostSafe();
				int ws;
				bpm = module->params[WingFx::SYNC_PARAM].getValue() > 0.5f ? hostRackBpm(h) : hostWingBpm(h, &ws);
			}
			text(args.vg, boldFont(), box.size.x / 2, mm2px(4.8f), bpm > 0.f ? string::f("%.1f", bpm) : "– – –", 10.f,
			     WHITE, NVG_ALIGN_CENTER, 0.6f);
		}
		Widget::drawLayer(args, layer);
	}
};

/** A parameter knob: right-click picks which of the effect's parameters it turns (kept per effect). */
struct FxKnob : GSmallKnob {
	int j = 0, k = 0;
	void appendContextMenu(Menu* menu) override {
		WingFx* m = dynamic_cast<WingFx*>(module);
		WingLink* h = m ? m->hostSafe() : nullptr;
		FxSlot* x = hostFx(h, m ? m->first + j : 0);
		if (!m || !x)
			return;
		int model = x->model, kk = k;
		const F::Model& M = F::MODELS[model];
		if (M.nAll == 0)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem(string::f("Knob %d turns", k + 1), m->knob(model, k) ? m->knob(model, k)->label : "–",
		                                 [=](Menu* sub) {
			sub->addChild(createMenuLabel(string::f("%s (kept for every %s)", M.name, M.mdl)));
			int def = M.show[kk];
			sub->addChild(createCheckMenuItem(def >= 0 ? string::f("Default: %s", M.all[def].label) : "Default: –", "",
			                                  [=]() { return m->choice[model][kk] < 0; }, [=]() {
				                                  m->choice[model][kk] = -1;
				                                  m->refresh = true;
			                                  }));
			sub->addChild(new MenuSeparator);
			for (int i = 0; i < M.nAll; i++)
				sub->addChild(createCheckMenuItem(M.all[i].label, M.all[i].unit, [=]() { return m->choice[model][kk] == i; },
				                                  [=]() {
					                                  m->choice[model][kk] = i;
					                                  m->refresh = true;
				                                  }));
		}));
	}
};

struct GMediumKnob : RoundKnob {
	GMediumKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium.svg")));
		bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium_bg.svg")));
	}
};

template <int R, int G, int B>
using LitLatch = VCVLightBezelLatch<ColorLight<R, G, B>>;

struct WingFxWidget : ModuleWidget {
	float scan = 0.f;
	WingFxWidget(WingFx* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingFx.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		FxDisplay* d = new FxDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		addParam(createParamCentered<VCVBezel>(mm(L::PAGE_DOWN), module, WingFx::PAGE_DOWN_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::PAGE_UP), module, WingFx::PAGE_UP_PARAM));
		FxStep* st = new FxStep;
		st->module = module;
		area(st, L::STEP);
		const float* on[NPAGE] = {L::ON1, L::ON2, L::ON3, L::ON4};
		const float* mix[NPAGE] = {L::MIX1, L::MIX2, L::MIX3, L::MIX4};
		const float* head[NPAGE] = {L::HEAD1, L::HEAD2, L::HEAD3, L::HEAD4};
		const float* model[NPAGE] = {L::MODEL1, L::MODEL2, L::MODEL3, L::MODEL4};
		const float* pk[NPAGE][4] = {{L::P1_1, L::P2_1, L::P3_1, L::P4_1}, {L::P1_2, L::P2_2, L::P3_2, L::P4_2},
		                             {L::P1_3, L::P2_3, L::P3_3, L::P4_3}, {L::P1_4, L::P2_4, L::P3_4, L::P4_4}};
		const float* lb[NPAGE][4] = {{L::LBL1_1, L::LBL2_1, L::LBL3_1, L::LBL4_1}, {L::LBL1_2, L::LBL2_2, L::LBL3_2, L::LBL4_2},
		                             {L::LBL1_3, L::LBL2_3, L::LBL3_3, L::LBL4_3}, {L::LBL1_4, L::LBL2_4, L::LBL3_4, L::LBL4_4}};
		const float* cv[NPAGE][5] = {{L::CV0_1, L::CV1_1, L::CV2_1, L::CV3_1, L::CV4_1},
		                             {L::CV0_2, L::CV1_2, L::CV2_2, L::CV3_2, L::CV4_2},
		                             {L::CV0_3, L::CV1_3, L::CV2_3, L::CV3_3, L::CV4_3},
		                             {L::CV0_4, L::CV1_4, L::CV2_4, L::CV3_4, L::CV4_4}};
		for (int j = 0; j < NPAGE; j++) {
			HeadWidget* hw = new HeadWidget;
			hw->module = module;
			hw->j = j;
			area(hw, head[j]);
			addParam(createLightParamCentered<LitLatch<0xff, 0x6b, 0x5b>>(mm(on[j]), module, fxPid(j, FON),
			                                                              WingFx::ON_LIGHT + j));
			ModelWidget* mw = new ModelWidget;
			mw->module = module;
			mw->j = j;
			area(mw, model[j]);
			addParam(createParamCentered<GMediumKnob>(mm(mix[j]), module, fxPid(j, FMIX)));
			for (int k = 0; k < 4; k++) {
				FxKnob* kn = createParamCentered<FxKnob>(mm(pk[j][k]), module, fxPid(j, FP1 + k));
				kn->j = j;
				kn->k = k;
				addParam(kn);
				LabelWidget* l = new LabelWidget;
				l->module = module;
				l->j = j;
				l->k = k;
				area(l, lb[j][k]);
			}
			for (int k = 0; k < 5; k++)
				addInput(createInputCentered<PJ301MPort>(mm(cv[j][k]), module, fxCv(j, k)));
		}
		addInput(createInputCentered<PJ301MPort>(mm(L::CLOCK_IN), module, WingFx::CLOCK_INPUT));
		addParam(createLightParamCentered<VCVLightBezel<ColorLight<0xff, 0x6b, 0x5b>>>(mm(L::TAP), module,
		                                                                               WingFx::TAP_PARAM, WingFx::TAP_LIGHT));
		addParam(createLightParamCentered<LitLatch<0xff, 0x6b, 0x5b>>(mm(L::SYNC), module, WingFx::SYNC_PARAM,
		                                                              WingFx::SYNC_LIGHT));
		TempoWidget* t = new TempoWidget;
		t->module = module;
		area(t, L::TEMPO);
		addOutput(createOutputCentered<PJ301MPort>(mm(L::CLOCK_OUT), module, WingFx::CLOCK_OUTPUT));
	}
	void step() override {
		ModuleWidget::step();
		scan -= APP->window->getLastFrameDuration();
		if (module && scan <= 0.f) {
			scan = 0.5f;
			linkPanels(static_cast<Satellite*>(module));
		}
	}
	void appendContextMenu(Menu* menu) override {
		WingFx* m = dynamic_cast<WingFx*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		auto res = [=](const char* title, std::atomic<int>* v) {
			menu->addChild(createSubmenuItem(title, "", [=](Menu* sub) {
				const int vals[4] = {1, 2, 4, 24};
				const char* names[4] = {"Quarters", "8ths", "16ths", "24 PPQN"};
				for (int i = 0; i < 4; i++) {
					int val = vals[i];
					sub->addChild(createCheckMenuItem(names[i], "", [=]() { return v->load() == val; }, [=]() { *v = val; }));
				}
			}));
		};
		menu->addChild(createSubmenuItem("SLOTS steps", string::f("%d", m->slotStep.load()), [=](Menu* sub) {
			for (int v : {1, 4})
				sub->addChild(createCheckMenuItem(v == 1 ? "1 (one slot)" : "4 (FX 1–4, 5–8 …)", "",
				                                  [=]() { return m->slotStep.load() == v; }, [=]() { m->slotStep = v; }));
		}));
		res("CLOCK IN resolution", &m->ppqIn);
		res("CLOCK OUT resolution", &m->ppqOut);
		menu->addChild(createMenuLabel("SYNC on: the delays take Rack's tempo (CLOCK IN or TAP)"));
		menu->addChild(createMenuLabel("SYNC off: tap on the WING; CLOCK OUT follows it"));
	}
};

}  // namespace winglink

Model* modelWingFx = createModel<winglink::WingFx, winglink::WingFxWidget>("WingFx");

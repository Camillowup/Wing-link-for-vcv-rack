// SPDX-License-Identifier: GPL-3.0-only
// WING-KNOBS – the WING's custom controls (its knobs and buttons, set to send MIDI) as CV. Each of 8 knob slots and 8
// button slots learns one control: click the slot, turn the knob / press the button on the WING. A knob gives
// 0..10 V × LEVEL, a button 10 V × LEVEL while it is on. WING-LINK hands it the MIDI (src/WingLink.cpp onMidi → feed).
// Learning takes a CC on MIDI channel 6–8 (those WING-LINK does not use itself) or a note on any channel.
// Panel: tools/wingknobs_panel.py.
#include "WingLink.hpp"
#include "WingKnobsLayout.hpp"

namespace winglink {

namespace L = knobslayout;
using namespace wlui;

static const NVGcolor KNOB_C = nvgRGB(0x3d, 0xc9, 0xff), BUTTON_C = nvgRGB(0xb8, 0xf2, 0x3a);

WingKnobs::WingKnobs() {
	config(NUM_PARAMS, 0, NUM_OUTPUTS, NUM_LIGHTS);
	for (int i = 0; i < 2 * NKNOB; i++) {
		learned[i] = -1;
		value[i] = 0;
	}
	for (int i = 0; i < NKNOB; i++) {
		configParam(KLVL_PARAM + i, 0.f, 1.f, 1.f, string::f("Knob %d level", i + 1), "%", 0.f, 100.f);
		configParam(BLVL_PARAM + i, 0.f, 1.f, 1.f, string::f("Button %d level", i + 1), "%", 0.f, 100.f);
		configOutput(K_OUTPUT + i, string::f("Knob %d (0–10 V)", i + 1));
		configOutput(B_OUTPUT + i, string::f("Button %d (gate)", i + 1));
	}
}

bool WingKnobs::feed(int ch, int num, int val, bool note) {
	int code = ch * 256 + num + (note ? 128 : 0);
	int l = learning.load();
	if (l >= 0 && (note || (ch >= 5 && ch <= 7))) {
		// a button learns a press (not a release), a knob any move
		if (!note || val > 0) {
			for (int i = 0; i < 2 * NKNOB; i++)
				if (i != l && learned[i] == code)
					learned[i] = -1;
			learned[l] = code;
			value[l] = val;
			learning = -1;
			return true;
		}
	}
	bool took = false;
	for (int i = 0; i < 2 * NKNOB; i++)
		if (learned[i] == code) {
			value[i] = val;
			took = true;
		}
	return took;
}

void WingKnobs::process(const ProcessArgs& args) {
	for (int i = 0; i < NKNOB; i++) {
		outputs[K_OUTPUT + i].setVoltage(value[i].load() / 127.f * 10.f * params[KLVL_PARAM + i].getValue());
		outputs[B_OUTPUT + i].setVoltage(value[NKNOB + i].load() >= 64 ? 10.f * params[BLVL_PARAM + i].getValue() : 0.f);
	}
}

json_t* WingKnobs::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_t* a = json_array();
	for (int i = 0; i < 2 * NKNOB; i++) {
		json_t* o = json_object();
		json_object_set_new(o, "learned", json_integer(learned[i]));
		json_object_set_new(o, "value", json_integer(value[i]));
		json_object_set_new(o, "name", json_string(names[i].c_str()));
		json_array_append_new(a, o);
	}
	json_object_set_new(root, "slots", a);
	return root;
}
void WingKnobs::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	json_t* a = json_object_get(root, "slots");
	if (!a || !json_is_array(a))
		return;
	for (int i = 0; i < 2 * NKNOB && i < (int) json_array_size(a); i++) {
		json_t* o = json_array_get(a, i);
		if (json_t* j = json_object_get(o, "learned"))
			learned[i] = (int) json_integer_value(j);
		if (json_t* j = json_object_get(o, "value"))
			value[i] = clamp((int) json_integer_value(j), 0, 127);
		if (json_t* j = json_object_get(o, "name"))
			if (const char* s = json_string_value(j))
				names[i] = s;
	}
}

static std::string learnedLabel(int code) {
	if (code < 0)
		return "—";
	int ch = code / 256 + 1, n = code % 256;
	return n >= 128 ? string::f("NOTE %d · %d", n - 128, ch) : string::f("CC %d · %d", n, ch);
}

// ---------------------------------------------------------------- the display
struct KnobsDisplay : Widget {
	WingKnobs* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			WingLink* h = module ? module->hostSafe() : nullptr;
			float x = mm2px(2.6f);
			text(vg, boldFont(), x, mm2px(5.5f), "CUSTOM CONTROLS", 12.f, WHITE, NVG_ALIGN_LEFT, 0.6f);
			std::string st;
			NVGcolor sc = OK;
			int l = module ? module->learning.load() : -1;
			if (!module)
				st = "●  6 LEARNED · CLICK A SLOT TO LEARN";
			else if (!h) {
				st = "●  right-click › Connect to WING-LINK";
				sc = WARN;
			}
			else if (!hostLinkedSat(h, module)) {
				st = "●  WING-LINK already has a WING-KNOBS";
				sc = WARN;
			}
			else if (l >= 0) {
				st = string::f("●  LEARNING %s %d · MOVE IT ON THE WING (MIDI CH 6–8)", l < NKNOB ? "KNOB" : "BUTTON",
				               l % NKNOB + 1);
				sc = WARN;
			}
			else {
				int n = 0;
				for (int i = 0; i < 2 * NKNOB; i++)
					n += module->learned[i] >= 0;
				st = string::f("●  %d LEARNED · CLICK A SLOT TO LEARN", n);
			}
			text(vg, boldFont(), x, mm2px(8.9f), st, 6.2f, sc, NVG_ALIGN_LEFT, 0.4f);
		}
		Widget::drawLayer(args, layer);
	}
};

struct KnobNameField : ui::TextField {
	WingKnobs* module = nullptr;
	int k = 0;
	void onChange(const ChangeEvent& e) override {
		module->names[k] = getText().substr(0, 16);
		ui::TextField::onChange(e);
	}
};

/** A slot: its name (or what it learned) and its value; a click learns, a right-click forgets or renames. */
struct KnobSlot : OpaqueWidget {
	WingKnobs* module = nullptr;
	int i = 0;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			bool button = i >= NKNOB;
			NVGcolor c = button ? BUTTON_C : KNOB_C;
			int code = module ? module->learned[i].load() : (i % NKNOB < 6 ? 1 : -1);
			bool learning = module && module->learning == i;
			int v = module ? module->value[i].load() : (button ? (i % 2) * 127 : 20 + i * 14);
			std::string name = module ? module->names[i] : "";
			if (name.empty())
				name = code >= 0 ? (module ? learnedLabel(code) : string::f("CC %d · 7", i % NKNOB + 1))
				                 : string::f("%s %d", button ? "BUTTON" : "KNOB", i % NKNOB + 1);
			if (learning && std::fmod(system::getTime(), 0.8) < 0.4) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 2.f);
				nvgStrokeColor(vg, WARN);
				nvgStrokeWidth(vg, 1.f);
				nvgStroke(vg);
			}
			text(vg, boldFont(), mm2px(1.f), mm2px(2.6f), learning ? "LEARN…" : string::uppercase(name), 6.f,
			     learning ? WARN : (code >= 0 ? WHITE : FAINT), NVG_ALIGN_LEFT, 0.3f);
			float y = mm2px(4.6f), w = box.size.x - mm2px(2.f);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, mm2px(1.f), y, w, mm2px(1.2f), 1.f);
			nvgFillColor(vg, OFF);
			nvgFill(vg);
			if (code >= 0) {
				float f = button ? (v >= 64 ? 1.f : 0.f) : v / 127.f;
				if (f > 0.f) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, mm2px(1.f), y, w * f, mm2px(1.2f), 1.f);
					nvgFillColor(vg, c);
					nvgFill(vg);
				}
			}
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module) {
			OpaqueWidget::onButton(e);
			return;
		}
		WingKnobs* m = module;
		int k = i;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT)
			m->learning = m->learning == k ? -1 : k;
		else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("%s %d  ·  %s", k < NKNOB ? "KNOB" : "BUTTON", k % NKNOB + 1,
			                                         learnedLabel(m->learned[k]).c_str())));
			menu->addChild(createMenuItem("Learn", "", [=]() { m->learning = k; }));
			menu->addChild(createMenuItem("Forget", "", [=]() {
				m->learned[k] = -1;
				m->value[k] = 0;
				if (m->learning == k)
					m->learning = -1;
			}));
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel("Name"));
			KnobNameField* tf = new KnobNameField;
			tf->module = m;
			tf->k = k;
			tf->box.size.x = 160.f;
			tf->setText(m->names[k]);
			menu->addChild(tf);
		}
		e.consume(this);
	}
};

struct WingKnobsWidget : ModuleWidget {
	float scan = 0.f;
	WingKnobsWidget(WingKnobs* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingKnobs.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		KnobsDisplay* d = new KnobsDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		const float* ks[NKNOB] = {L::K1, L::K2, L::K3, L::K4, L::K5, L::K6, L::K7, L::K8};
		const float* bs[NKNOB] = {L::B1, L::B2, L::B3, L::B4, L::B5, L::B6, L::B7, L::B8};
		const float* kl[NKNOB] = {L::LVL_K1, L::LVL_K2, L::LVL_K3, L::LVL_K4, L::LVL_K5, L::LVL_K6, L::LVL_K7, L::LVL_K8};
		const float* bl[NKNOB] = {L::LVL_B1, L::LVL_B2, L::LVL_B3, L::LVL_B4, L::LVL_B5, L::LVL_B6, L::LVL_B7, L::LVL_B8};
		const float* ko[NKNOB] = {L::OUT_K1, L::OUT_K2, L::OUT_K3, L::OUT_K4, L::OUT_K5, L::OUT_K6, L::OUT_K7, L::OUT_K8};
		const float* bo[NKNOB] = {L::OUT_B1, L::OUT_B2, L::OUT_B3, L::OUT_B4, L::OUT_B5, L::OUT_B6, L::OUT_B7, L::OUT_B8};
		for (int i = 0; i < NKNOB; i++) {
			KnobSlot* s = new KnobSlot;
			s->module = module;
			s->i = i;
			area(s, ks[i]);
			KnobSlot* b = new KnobSlot;
			b->module = module;
			b->i = NKNOB + i;
			area(b, bs[i]);
			addParam(createParamCentered<GSmallKnob>(mm(kl[i]), module, WingKnobs::KLVL_PARAM + i));
			addParam(createParamCentered<GSmallKnob>(mm(bl[i]), module, WingKnobs::BLVL_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm(ko[i]), module, WingKnobs::K_OUTPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm(bo[i]), module, WingKnobs::B_OUTPUT + i));
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
		WingKnobs* m = dynamic_cast<WingKnobs*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(createMenuItem("Forget all", "", [=]() {
			for (int i = 0; i < 2 * NKNOB; i++) {
				m->learned[i] = -1;
				m->value[i] = 0;
			}
			m->learning = -1;
		}));
		menu->addChild(createMenuLabel("On the WING: a custom control → MIDI CC on channel 6, 7 or 8"));
		menu->addChild(createMenuLabel("(a button may also send a note)"));
	}
};

}  // namespace winglink

Model* modelWingKnobs = createModel<winglink::WingKnobs, winglink::WingKnobsWidget>("WingKnobs");

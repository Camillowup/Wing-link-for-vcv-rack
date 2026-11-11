// SPDX-License-Identifier: GPL-3.0-only
// WING-DYN – the gate and the compressor (the STD models GATE and COMP) of one WING channel, as a panel of WING-LINK
// (which does the talking: src/WingLink.cpp controlDyn). The channel as WING-EQ chooses it: a strip of WING-LINK, a
// fixed one (◀ ▶), or SELECT on the WING. Two pictures: the curve in → out (both thresholds, the ratio) and the
// envelopes in time (attack, hold, release), both drawn from the knobs. Panel: tools/wingdyn_panel.py.
#include "WingLink.hpp"
#include "WingDynLayout.hpp"

namespace winglink {

namespace L = dynlayout;
using namespace wlui;

static const NVGcolor GATE_C = nvgRGB(0x3d, 0xc9, 0xff), COMP_C = nvgRGB(0xff, 0x9a, 0x3e);

struct DynQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		int f = paramId;
		if (f == D_CRATIO)
			return std::string(DYN_RATIOS[clamp((int) std::round(v), 0, NRATIO - 1)]) + ":1";
		if (DYN[f].logr)
			return string::f("%.0f", dynRelMs(v));
		return ParamQuantity::getDisplayValueString();
	}
	std::string getUnit() override { return DYN[paramId].logr ? " ms" : ParamQuantity::getUnit(); }
};

WingDyn::WingDyn() {
	config(NUM_PARAMS, NUM_INPUTS, 0, NUM_LIGHTS);
	static const char* const NAMES[NDYN] = {"Gate", "Gate threshold", "Gate range", "Gate attack", "Gate hold",
	                                        "Gate release", "Gate accent", "Compressor", "Compressor threshold",
	                                        "Compressor ratio", "Compressor gain", "Compressor mix", "Compressor knee",
	                                        "Compressor attack", "Compressor hold", "Compressor release",
	                                        "Compressor detector", "Compressor envelope", "Compressor auto"};
	static const char* const UNITS[NDYN] = {"", " dB", " dB", " ms", " ms", " ms", " %", "", " dB", "", " dB", " %",
	                                        "", " ms", " ms", " ms", "", "", ""};
	for (int f = 0; f < NDYN; f++) {
		const DynSpec& d = DYN[f];
		if (f == D_CDET)
			configSwitch(f, 0.f, 1.f, d.def, NAMES[f], {"PEAK", "RMS"});
		else if (f == D_CENV)
			configSwitch(f, 0.f, 1.f, d.def, NAMES[f], {"LIN", "LOG"});
		else if (d.sw)
			configSwitch(f, 0.f, 1.f, d.def, NAMES[f], {"Off", "On"});
		else {
			ParamQuantity* q = configParam<DynQuantity>(f, d.lo, d.hi, d.def, NAMES[f], UNITS[f]);
			if (f == D_CRATIO || f == D_CKNEE)
				q->snapEnabled = true;
		}
	}
	configButton(PREV_PARAM, "Channel before");
	configButton(NEXT_PARAM, "Next channel");
	configInput(CV_G_THR, "Gate threshold CV (0–10 V)");
	configInput(CV_G_RANGE, "Gate range CV (0–10 V)");
	configInput(CV_G_ON, "Gate on (gate)");
	configInput(CV_G_ATT, "Gate attack CV (0–10 V)");
	configInput(CV_G_REL, "Gate release CV (0–10 V)");
	configInput(CV_C_THR, "Compressor threshold CV (0–10 V)");
	configInput(CV_C_RATIO, "Compressor ratio CV (0–10 V)");
	configInput(CV_C_GAIN, "Compressor gain CV (0–10 V)");
	configInput(CV_C_MIX, "Compressor mix CV (0–10 V)");
	configInput(CV_C_KNEE, "Compressor knee CV (0–10 V)");
	configInput(CV_C_ATT, "Compressor attack CV (0–10 V)");
	configInput(CV_C_REL, "Compressor release CV (0–10 V)");
	configInput(CV_C_ON, "Compressor on (gate)");
}

void WingDyn::process(const ProcessArgs& args) {
	lights[GON_LIGHT].setBrightness(params[D_GON].getValue() > 0.5f ? 1.f : 0.f);
	lights[CON_LIGHT].setBrightness(params[D_CON].getValue() > 0.5f ? 1.f : 0.f);
	lights[DET_LIGHT].setBrightness(params[D_CDET].getValue() > 0.5f ? 1.f : 0.f);
	lights[ENV_LIGHT].setBrightness(params[D_CENV].getValue() > 0.5f ? 1.f : 0.f);
	lights[AUTO_LIGHT].setBrightness(params[D_CAUTO].getValue() > 0.5f ? 1.f : 0.f);
}

json_t* WingDyn::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "mode", json_integer(mode));
	json_object_set_new(root, "strip", json_integer(strip));
	json_object_set_new(root, "fixedKey", json_integer(fixedKey));
	return root;
}
void WingDyn::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "mode"))
		mode = clamp((int) json_integer_value(j), 0, 2);
	if (json_t* j = json_object_get(root, "strip"))
		strip = clamp((int) json_integer_value(j), 0, NT - 1);
	if (json_t* j = json_object_get(root, "fixedKey"))
		fixedKey = (int) json_integer_value(j);
}

static std::string dynKeyLabel(int key) { return wingproto::tgtLabel(wingproto::tgt(key / 100, key % 100)); }

// ---------------------------------------------------------------- the pictures
/** in → out: below the gate's threshold down by its range, above the compressor's by its ratio (and its knee). */
struct DynCurveWidget : Widget {
	WingDyn* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		float gthr = -42.f, range = 40.f, cthr = -18.f, ratio = 4.f, knee = 2.f, gain = 0.f;
		bool gon = true, con = true;
		if (module) {
			gthr = module->params[D_GTHR].getValue();
			range = module->params[D_GRANGE].getValue();
			cthr = module->params[D_CTHR].getValue();
			ratio = (float) std::atof(DYN_RATIOS[clamp((int) std::round(module->params[D_CRATIO].getValue()), 0, NRATIO - 1)]);
			knee = module->params[D_CKNEE].getValue();
			gain = module->params[D_CGAIN].getValue();
			gon = module->params[D_GON].getValue() > 0.5f;
			con = module->params[D_CON].getValue() > 0.5f;
		}
		float x0 = mm2px(4.f), y0 = box.size.y - mm2px(3.f), w = box.size.x - mm2px(8.f), h = box.size.y - mm2px(6.f);
		auto X = [&](float db) { return x0 + (db + 80.f) / 80.f * w; };
		auto Y = [&](float db) { return y0 - clamp((db + 80.f) / 80.f, 0.f, 1.f) * h; };
		nvgStrokeWidth(vg, 0.7f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, X(-80.f), Y(-80.f));
		nvgLineTo(vg, X(0.f), Y(0.f));
		nvgStrokeColor(vg, nvgRGB(0x34, 0x36, 0x3f));
		nvgStroke(vg);
		nvgBeginPath(vg);
		for (int k = 0; k <= 80; k++) {
			float in = -80.f + k, out = in;
			if (gon && in < gthr)
				out = in - range;
			if (con) {
				float over = in - cthr, kw = knee * 3.f;
				if (kw > 0.f && std::fabs(over) <= kw / 2.f)
					out += (1.f / ratio - 1.f) * (over + kw / 2.f) * (over + kw / 2.f) / (2.f * kw);
				else if (over > kw / 2.f)
					out += (1.f / ratio - 1.f) * over;
				out += gain;
			}
			k ? nvgLineTo(vg, X(in), Y(out)) : nvgMoveTo(vg, X(in), Y(out));
		}
		nvgStrokeColor(vg, WHITE);
		nvgStrokeWidth(vg, 1.5f);
		nvgStroke(vg);
		auto vline = [&](float db, NVGcolor c, bool on) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, X(db), mm2px(2.f));
			nvgLineTo(vg, X(db), y0);
			if (!on)
				c.a = 0.3f;
			nvgStrokeColor(vg, c);
			nvgStrokeWidth(vg, 0.9f);
			nvgStroke(vg);
		};
		vline(gthr, GATE_C, gon);
		vline(cthr, COMP_C, con);
		text(vg, boldFont(), X(cthr) + 3.f, mm2px(6.f),
		     std::string(module ? DYN_RATIOS[clamp((int) std::round(module->params[D_CRATIO].getValue()), 0, NRATIO - 1)] : "4.0") + ":1",
		     5.8f, COMP_C, NVG_ALIGN_LEFT, 0.3f);
		text(vg, boldFont(), mm2px(1.4f), mm2px(3.f), "OUT", 5.2f, FAINT, NVG_ALIGN_LEFT, 0.3f);
		text(vg, boldFont(), box.size.x - mm2px(1.4f), box.size.y - mm2px(1.f), "IN", 5.2f, FAINT, NVG_ALIGN_RIGHT, 0.3f);
		Widget::drawLayer(args, layer);
	}
};

/** in time: the gate opening (attack), staying (hold), closing (release); the compressor pulling down and letting go. */
struct DynEnvWidget : Widget {
	WingDyn* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		float ga = 2.f, gh = 40.f, gr = 120.f, ca = 10.f, ch = 20.f, cr = 80.f, red = 6.f;
		bool gon = true, con = true;
		if (module) {
			ga = module->params[D_GATT].getValue();
			gh = module->params[D_GHOLD].getValue();
			gr = dynRelMs(module->params[D_GREL].getValue());
			ca = module->params[D_CATT].getValue();
			ch = module->params[D_CHOLD].getValue();
			cr = dynRelMs(module->params[D_CREL].getValue());
			float r = (float) std::atof(DYN_RATIOS[clamp((int) std::round(module->params[D_CRATIO].getValue()), 0, NRATIO - 1)]);
			red = clamp(12.f * (1.f - 1.f / r), 0.f, 12.f);
			gon = module->params[D_GON].getValue() > 0.5f;
			con = module->params[D_CON].getValue() > 0.5f;
		}
		// the time axis: a note of 300 ms, then its release; logarithmic-ish so 1 ms and 4 s both show
		float x0 = mm2px(3.f), w = box.size.x - mm2px(6.f), top = mm2px(5.f), bot = box.size.y - mm2px(5.f);
		const float NOTE = 300.f, SPAN = 300.f + 1500.f;
		auto T = [&](float ms) { return x0 + clamp(ms / SPAN, 0.f, 1.f) * w; };
		// gate: opens over its attack, stays (the note + hold), closes over its release
		{
			NVGcolor c = gon ? GATE_C : FAINT;
			float hi = top + mm2px(9.f);
			nvgBeginPath(vg);
			nvgMoveTo(vg, T(0), bot);
			nvgLineTo(vg, T(ga), hi);
			nvgLineTo(vg, T(NOTE + gh), hi);
			for (int k = 1; k <= 20; k++) {
				float t = gr * k / 20.f;
				nvgLineTo(vg, T(NOTE + gh + t), hi + (bot - hi) * (1.f - std::exp(-3.f * k / 20.f)) / (1.f - std::exp(-3.f)));
			}
			nvgLineTo(vg, T(SPAN), bot);
			nvgStrokeColor(vg, c);
			nvgStrokeWidth(vg, 1.3f);
			nvgStroke(vg);
		}
		// compressor: gain down by its reduction over the attack, back over the release
		{
			NVGcolor c = con ? COMP_C : FAINT;
			float down = top + red * mm2px(0.8f);
			nvgBeginPath(vg);
			nvgMoveTo(vg, T(0), top);
			for (int k = 1; k <= 12; k++)
				nvgLineTo(vg, T(ca * k / 12.f * 3.f), top + (down - top) * (1.f - std::exp(-3.f * k / 12.f)));
			nvgLineTo(vg, T(NOTE + ch), down);
			for (int k = 1; k <= 20; k++)
				nvgLineTo(vg, T(NOTE + ch + cr * k / 20.f * 3.f), down + (top - down) * (1.f - std::exp(-3.f * k / 20.f)));
			nvgLineTo(vg, T(SPAN), top);
			nvgStrokeColor(vg, c);
			nvgStrokeWidth(vg, 1.3f);
			nvgStroke(vg);
		}
		text(vg, boldFont(), x0, box.size.y - mm2px(1.2f), string::f("GATE  %.0f · %.0f · %.0f ms", ga, gh, gr), 5.4f,
		     gon ? GATE_C : FAINT, NVG_ALIGN_LEFT, 0.3f);
		text(vg, boldFont(), box.size.x - mm2px(3.f), mm2px(3.2f), string::f("COMP  −%.0f dB · %.0f · %.0f ms", red, ca, cr),
		     5.4f, con ? COMP_C : FAINT, NVG_ALIGN_RIGHT, 0.3f);
		Widget::drawLayer(args, layer);
	}
};

// ---------------------------------------------------------------- the display: the channel (a click picks it)
struct DynDisplay : OpaqueWidget {
	WingDyn* module = nullptr;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			NVGcontext* vg = args.vg;
			WingLink* h = module ? module->hostSafe() : nullptr;
			int key = module ? module->shownKey.load() : 101;
			Meta m = (h && key >= 0) ? hostMetaOf(h, key) : Meta();
			NVGcolor col = FAINT;
			if (m.colour >= 1 && m.colour <= P::NUM_COLOURS)
				col = nvgRGB(P::COLOURS[m.colour - 1].r, P::COLOURS[m.colour - 1].g, P::COLOURS[m.colour - 1].b);
			if (!module)
				col = ACCENT;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, mm2px(2.6f), mm2px(1.8f), mm2px(1.1f), mm2px(7.4f), 1.2f);
			nvgFillColor(vg, col);
			nvgFill(vg);
			float x = mm2px(5.4f);
			std::string title = !module ? "CH 1 · KICK" : (key >= 0 ? dynKeyLabel(key) : "NO CHANNEL");
			if (module && !m.name.empty())
				title += " · " + string::uppercase(m.name);
			text(vg, boldFont(), x, mm2px(5.5f), title, 12.f, WHITE, NVG_ALIGN_LEFT, 0.6f);
			std::string st;
			NVGcolor sc = OK;
			if (!module)
				st = "●  STRIP 1 · GATE ON · COMP ON";
			else if (!h) {
				st = "●  right-click › Connect to WING-LINK";
				sc = WARN;
			}
			else if (!hostLinkedSat(h, module)) {
				st = "●  WING-LINK already has a WING-DYN";
				sc = WARN;
			}
			else if (module->gateModel == 0 || module->compModel == 0) {
				st = string::f("●  MODELS %s / %s ON THE WING: STD ONLY (GATE / COMP)", module->gateMdl, module->compMdl);
				sc = WARN;
			}
			else {
				int mo = module->mode;
				st = mo == WingEq::M_STRIP ? string::f("●  STRIP %d", module->strip.load() + 1)
				                           : (mo == WingEq::M_SELECT ? "●  SELECT" : "●  FIXED · ◀ ▶");
				st += module->params[D_GON].getValue() > 0.5f ? " · GATE ON" : " · GATE OFF";
				st += module->params[D_CON].getValue() > 0.5f ? " · COMP ON" : " · COMP OFF";
			}
			text(vg, boldFont(), x, mm2px(8.9f), st, 6.2f, sc, NVG_ALIGN_LEFT, 0.4f);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module) {
			OpaqueWidget::onButton(e);
			return;
		}
		WingDyn* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("WING-DYN  ·  channel"));
		menu->addChild(createSubmenuItem("Follow a strip of WING-LINK", m->mode == WingEq::M_STRIP ? string::f("strip %d", m->strip.load() + 1) : "",
		                                 [=](ui::Menu* sub) {
			WingLink* h = m->hostSafe();
			int n = hostHasX(h) ? NT : NS;
			for (int s = 0; s < n; s++)
				sub->addChild(createCheckMenuItem(string::f("Strip %d", s + 1), hostStripLabel(h, s),
				                                  [=]() { return m->mode == WingEq::M_STRIP && m->strip == s; }, [=]() {
					                                  m->strip = s;
					                                  m->mode = WingEq::M_STRIP;
				                                  }));
		}));
		menu->addChild(createCheckMenuItem("Follow SELECT on the WING", "", [=]() { return m->mode == WingEq::M_SELECT; },
		                                   [=]() { m->mode = WingEq::M_SELECT; }));
		auto fixed = [=](int type, int from, int to) {
			return [=](ui::Menu* sub) {
				for (int n = from; n <= to; n++) {
					int k = type * 100 + n;
					sub->addChild(createCheckMenuItem(dynKeyLabel(k), "", [=]() { return m->mode == WingEq::M_FIXED && m->fixedKey == k; },
					                                  [=]() {
						                                  m->fixedKey = k;
						                                  m->mode = WingEq::M_FIXED;
					                                  }));
				}
			};
		};
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("A fixed channel (◀ ▶ page through them)"));
		menu->addChild(createSubmenuItem("CH 1–20", "", fixed(P::T_CH, 1, 20)));
		menu->addChild(createSubmenuItem("CH 21–40", "", fixed(P::T_CH, 21, 40)));
		menu->addChild(createSubmenuItem("AUX 1–8", "", fixed(P::T_AUX, 1, 8)));
		e.consume(this);
	}
};

/** A knob's value under it. */
struct DynValue : Widget {
	WingDyn* module = nullptr;
	int f = 0;
	std::string mock;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			std::string s = mock;
			if (module) {
				float v = module->params[f].getValue();
				switch (f) {
					case D_GTHR: case D_CTHR: case D_GRANGE: s = string::f("%.0f dB", v); break;
					case D_CGAIN: s = string::f("%+.1f dB", v); break;
					case D_GACC: case D_CMIX: s = string::f("%.0f%%", v); break;
					case D_CRATIO: s = std::string(DYN_RATIOS[clamp((int) std::round(v), 0, NRATIO - 1)]) + ":1"; break;
					case D_CKNEE: s = string::f("%d", (int) std::round(v)); break;
					case D_GREL: case D_CREL: s = string::f("%.0f ms", dynRelMs(v)); break;
					default: s = string::f("%.0f ms", v); break;
				}
			}
			text(args.vg, boldFont(), box.size.x / 2, mm2px(1.6f), s, 5.6f, DIM, NVG_ALIGN_CENTER, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

struct DynMediumKnob : RoundKnob {
	DynMediumKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium.svg")));
		bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium_bg.svg")));
	}
};

template <int R, int G, int B>
using DynLatch = VCVLightLatch<MediumSimpleLight<ColorLight<R, G, B>>>;

struct WingDynWidget : ModuleWidget {
	float scan = 0.f;
	WingDynWidget(WingDyn* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingDyn.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		DynDisplay* d = new DynDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		DynCurveWidget* c = new DynCurveWidget;
		c->module = module;
		area(c, L::CURVE);
		DynEnvWidget* ev = new DynEnvWidget;
		ev->module = module;
		area(ev, L::ENV);
		addParam(createParamCentered<VCVBezel>(mm(L::PREV), module, WingDyn::PREV_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::NEXT), module, WingDyn::NEXT_PARAM));
		addParam(createLightParamCentered<VCVLightBezelLatch<ColorLight<0x3d, 0xc9, 0xff>>>(mm(L::GATE_ON), module, D_GON,
		                                                                                     WingDyn::GON_LIGHT));
		addParam(createLightParamCentered<VCVLightBezelLatch<ColorLight<0xff, 0x9a, 0x3e>>>(mm(L::COMP_ON), module, D_CON,
		                                                                                     WingDyn::CON_LIGHT));
		addParam(createLightParamCentered<DynLatch<0xff, 0x9a, 0x3e>>(mm(L::C_DET), module, D_CDET, WingDyn::DET_LIGHT));
		addParam(createLightParamCentered<DynLatch<0xff, 0x9a, 0x3e>>(mm(L::C_ENV), module, D_CENV, WingDyn::ENV_LIGHT));
		addParam(createLightParamCentered<DynLatch<0xff, 0x9a, 0x3e>>(mm(L::C_AUTO), module, D_CAUTO, WingDyn::AUTO_LIGHT));
		auto value = [&](const float* a, int f, const char* mk) {
			DynValue* v = new DynValue;
			v->module = module;
			v->f = f;
			v->mock = mk;
			area(v, a);
		};
		// medium knobs (THRESH, RANGE, RATIO, GAIN, MIX), small ones (the times, ACC, KNEE)
		addParam(createParamCentered<DynMediumKnob>(mm(L::G_THR), module, D_GTHR));
		addParam(createParamCentered<DynMediumKnob>(mm(L::G_RANGE), module, D_GRANGE));
		addParam(createParamCentered<GSmallKnob>(mm(L::G_ATT), module, D_GATT));
		addParam(createParamCentered<GSmallKnob>(mm(L::G_HOLD), module, D_GHOLD));
		addParam(createParamCentered<GSmallKnob>(mm(L::G_REL), module, D_GREL));
		addParam(createParamCentered<GSmallKnob>(mm(L::G_ACC), module, D_GACC));
		addParam(createParamCentered<DynMediumKnob>(mm(L::C_THR), module, D_CTHR));
		addParam(createParamCentered<DynMediumKnob>(mm(L::C_RATIO), module, D_CRATIO));
		addParam(createParamCentered<DynMediumKnob>(mm(L::C_GAIN), module, D_CGAIN));
		addParam(createParamCentered<DynMediumKnob>(mm(L::C_MIX), module, D_CMIX));
		addParam(createParamCentered<GSmallKnob>(mm(L::C_KNEE), module, D_CKNEE));
		addParam(createParamCentered<GSmallKnob>(mm(L::C_ATT), module, D_CATT));
		addParam(createParamCentered<GSmallKnob>(mm(L::C_HOLD), module, D_CHOLD));
		addParam(createParamCentered<GSmallKnob>(mm(L::C_REL), module, D_CREL));
		value(L::VG_THR, D_GTHR, "-42 dB");
		value(L::VG_RANGE, D_GRANGE, "40 dB");
		value(L::VG_ATT, D_GATT, "2 ms");
		value(L::VG_HOLD, D_GHOLD, "40 ms");
		value(L::VG_REL, D_GREL, "120 ms");
		value(L::VG_ACC, D_GACC, "50%");
		value(L::VC_THR, D_CTHR, "-18 dB");
		value(L::VC_RATIO, D_CRATIO, "4.0:1");
		value(L::VC_GAIN, D_CGAIN, "+3.0 dB");
		value(L::VC_MIX, D_CMIX, "100%");
		value(L::VC_KNEE, D_CKNEE, "2");
		value(L::VC_ATT, D_CATT, "10 ms");
		value(L::VC_HOLD, D_CHOLD, "20 ms");
		value(L::VC_REL, D_CREL, "80 ms");
		const float* jacks[WingDyn::NUM_INPUTS] = {L::CV_G_THR, L::CV_G_RANGE, L::CV_G_ON, L::CV_G_ATT, L::CV_G_REL,
		                                           L::CV_C_THR, L::CV_C_RATIO, L::CV_C_GAIN, L::CV_C_MIX, L::CV_C_KNEE,
		                                           L::CV_C_ATT, L::CV_C_REL, L::CV_C_ON};
		for (int i = 0; i < WingDyn::NUM_INPUTS; i++)
			addInput(createInputCentered<PJ301MPort>(mm(jacks[i]), module, i));
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
		WingDyn* m = dynamic_cast<WingDyn*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(createMenuLabel("The channel: click the display, or ◀ ▶"));
		menu->addChild(createMenuLabel("WING-DYN plays the STD models GATE and COMP"));
	}
};

}  // namespace winglink

Model* modelWingDyn = createModel<winglink::WingDyn, winglink::WingDynWidget>("WingDyn");

// SPDX-License-Identifier: GPL-3.0-only
// WING-EQ – the STD EQ and the filters of one WING channel, as a panel of WING-LINK (which does the talking:
// src/WingLink.cpp). The channel: a strip of WING-LINK, a fixed one (◀ ▶ page through CH 1–40 and AUX 1–8, a click on
// the display picks one) or the one selected on the WING. Six bands with GAIN, FREQ, Q and CV, LOW / HIGH as shelf or
// peak, LO CUT and HI CUT, the curve drawn from the knobs. Panel: tools/wingeq_panel.py.
#include "WingLink.hpp"
#include "WingEqLayout.hpp"
#include <complex>

namespace winglink {

namespace L = eqlayout;
using namespace wlui;

static const NVGcolor BAND_COLOURS[NBAND] = {nvgRGB(0xff, 0x6b, 0x5b), nvgRGB(0xff, 0x9a, 0x3e), nvgRGB(0xf0, 0xd0, 0x40),
                                             nvgRGB(0x7e, 0xe0, 0x8a), nvgRGB(0x3d, 0xc9, 0xff), nvgRGB(0xb4, 0x8c, 0xff)};
static const double TWO_PI = 6.283185307179586;
static const char* const BAND_NAMES[NBAND] = {"Low", "Band 1", "Band 2", "Band 3", "Band 4", "High"};

WingEq::WingEq() {
	config(NUM_PARAMS, NUM_INPUTS, 0, NUM_LIGHTS);
	configSwitch(EQ_ON, 0.f, 1.f, 1.f, "EQ", {"Off", "On"});
	const float defF[NBAND] = {80.f, 200.f, 600.f, 2000.f, 6000.f, 8000.f};
	for (int b = 0; b < NBAND; b++) {
		std::string n = BAND_NAMES[b];
		configParam(eqG(b), -15.f, 15.f, 0.f, n + " gain", " dB");
		float lo, hi;
		eqLogRange(eqF(b), &lo, &hi);
		configParam(eqF(b), 0.f, 1.f, eqParam(eqF(b), defF[b]), n + " frequency", " Hz", hi / lo, lo);
		eqLogRange(eqQ(b), &lo, &hi);
		configParam(eqQ(b), 0.f, 1.f, eqParam(eqQ(b), (b == 0 || b == 5) ? 0.7f : 1.f), n + " Q", "", hi / lo, lo);
		configInput(b * 3 + 0, n + " gain CV (±5 V = ±15 dB)");
		configInput(b * 3 + 1, n + " frequency CV (0–10 V)");
		configInput(b * 3 + 2, n + " Q CV (0–10 V)");
	}
	configSwitch(EQ_LSHV, 0.f, 1.f, 1.f, "Low band", {"Peak (PEQ)", "Shelf (SHV)"});
	configSwitch(EQ_HSHV, 0.f, 1.f, 1.f, "High band", {"Peak (PEQ)", "Shelf (SHV)"});
	configSwitch(EQ_LC, 0.f, 1.f, 0.f, "Low cut", {"Off", "On"});
	configParam(EQ_LCF, 0.f, 1.f, eqParam(EQ_LCF, 80.f), "Low cut frequency", " Hz", 100.f, 20.f);
	configSwitch(EQ_HC, 0.f, 1.f, 0.f, "High cut", {"Off", "On"});
	configParam(EQ_HCF, 0.f, 1.f, eqParam(EQ_HCF, 12000.f), "High cut frequency", " Hz", 400.f, 50.f);
	configButton(PREV_PARAM, "Channel before");
	configButton(NEXT_PARAM, "Next channel");
	configButton(FLAT_PARAM, "FLAT: every gain to 0 dB");
	configInput(CV_LC, "Low cut frequency CV (0–10 V)");
	configInput(CV_HC, "High cut frequency CV (0–10 V)");
	configInput(CV_ON, "EQ on (gate)");
}

void WingEq::process(const ProcessArgs& args) {
	// the lit buttons (also without a WING-LINK)
	lights[ON_LIGHT].setBrightness(params[EQ_ON].getValue() > 0.5f ? 1.f : 0.f);
	lights[LSHV_LIGHT].setBrightness(params[EQ_LSHV].getValue() > 0.5f ? 1.f : 0.f);
	lights[HSHV_LIGHT].setBrightness(params[EQ_HSHV].getValue() > 0.5f ? 1.f : 0.f);
	lights[LC_LIGHT].setBrightness(params[EQ_LC].getValue() > 0.5f ? 1.f : 0.f);
	lights[HC_LIGHT].setBrightness(params[EQ_HC].getValue() > 0.5f ? 1.f : 0.f);
}

json_t* WingEq::dataToJson() {
	json_t* root = Satellite::dataToJson();
	json_object_set_new(root, "mode", json_integer(mode));
	json_object_set_new(root, "strip", json_integer(strip));
	json_object_set_new(root, "fixedKey", json_integer(fixedKey));
	return root;
}
void WingEq::dataFromJson(json_t* root) {
	Satellite::dataFromJson(root);
	if (json_t* j = json_object_get(root, "mode"))
		mode = clamp((int) json_integer_value(j), 0, 2);
	if (json_t* j = json_object_get(root, "strip"))
		strip = clamp((int) json_integer_value(j), 0, NT - 1);
	if (json_t* j = json_object_get(root, "fixedKey"))
		fixedKey = (int) json_integer_value(j);
}

// ---------------------------------------------------------------- the curve
/** The magnitude (dB) of a biquad at w (radians per sample). */
static float biquadDb(const double* b, const double* a, double w) {
	std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
	std::complex<double> h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2);
	return (float) (20.0 * std::log10(std::max(std::abs(h), 1e-6)));
}
enum Shape { PEAK, LOSHELF, HISHELF, HIPASS, LOPASS };
/** RBJ cookbook coefficients. */
static void coeffs(Shape s, double f, double g, double q, double* b, double* a) {
	const double fs = 96000.0;
	double w0 = TWO_PI * std::min(f, fs * 0.45) / fs, c = std::cos(w0), sn = std::sin(w0);
	double A = std::pow(10.0, g / 40.0), al = sn / (2.0 * q), sa = 2.0 * std::sqrt(A) * al;
	switch (s) {
		case PEAK:
			b[0] = 1 + al * A; b[1] = -2 * c; b[2] = 1 - al * A;
			a[0] = 1 + al / A; a[1] = -2 * c; a[2] = 1 - al / A;
			break;
		case LOSHELF:
			b[0] = A * ((A + 1) - (A - 1) * c + sa); b[1] = 2 * A * ((A - 1) - (A + 1) * c); b[2] = A * ((A + 1) - (A - 1) * c - sa);
			a[0] = (A + 1) + (A - 1) * c + sa; a[1] = -2 * ((A - 1) + (A + 1) * c); a[2] = (A + 1) + (A - 1) * c - sa;
			break;
		case HISHELF:
			b[0] = A * ((A + 1) + (A - 1) * c + sa); b[1] = -2 * A * ((A - 1) + (A + 1) * c); b[2] = A * ((A + 1) + (A - 1) * c - sa);
			a[0] = (A + 1) - (A - 1) * c + sa; a[1] = 2 * ((A - 1) - (A + 1) * c); a[2] = (A + 1) - (A - 1) * c - sa;
			break;
		case HIPASS:
			b[0] = (1 + c) / 2; b[1] = -(1 + c); b[2] = (1 + c) / 2;
			a[0] = 1 + al; a[1] = -2 * c; a[2] = 1 - al;
			break;
		case LOPASS:
			b[0] = (1 - c) / 2; b[1] = 1 - c; b[2] = (1 - c) / 2;
			a[0] = 1 + al; a[1] = -2 * c; a[2] = 1 - al;
			break;
	}
}

struct CurveWidget : Widget {
	WingEq* module = nullptr;
	static const int NP = 160;
	float fx(float f, float x0, float x1) { return x0 + std::log10(f / 20.f) / 3.f * (x1 - x0); }
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		float x0 = mm2px(6.f), x1 = box.size.x - mm2px(2.5f), top = mm2px(2.f), bot = box.size.y - mm2px(4.f);
		float mid = (top + bot) / 2, dbs = (bot - top) / 2 / 16.f;
		// grid
		nvgStrokeWidth(vg, 0.7f);
		for (float f : {20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f}) {
			float x = fx(f, x0, x1);
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, top);
			nvgLineTo(vg, x, bot);
			nvgStrokeColor(vg, nvgRGB(0x1d, 0x1f, 0x25));
			nvgStroke(vg);
		}
		const char* fl[] = {"20", "100", "500", "1k", "5k", "10k", "20k"};
		const float fv[] = {20.f, 100.f, 500.f, 1000.f, 5000.f, 10000.f, 20000.f};
		for (int i = 0; i < 7; i++)
			text(vg, boldFont(), fx(fv[i], x0, x1), box.size.y - mm2px(1.2f), fl[i], 5.5f, FAINT, NVG_ALIGN_CENTER, 0.3f);
		for (int d : {-12, -6, 0, 6, 12}) {
			float y = mid - d * dbs;
			nvgBeginPath(vg);
			nvgMoveTo(vg, x0, y);
			nvgLineTo(vg, x1, y);
			nvgStrokeColor(vg, d == 0 ? nvgRGB(0x34, 0x36, 0x3f) : nvgRGB(0x1d, 0x1f, 0x25));
			nvgStroke(vg);
			text(vg, boldFont(), x0 - mm2px(0.8f), y + 2.f, d == 0 ? "0" : string::f("%+d", d), 5.2f, FAINT, NVG_ALIGN_RIGHT, 0.f);
		}
		// the bands from the knobs (the mock values in the module browser)
		double bb[NBAND + 2][3], aa[NBAND + 2][3];
		bool use[NBAND + 2] = {};
		float pf[NBAND];
		bool on = true;
		const float MG[NBAND] = {4.f, -3.f, -4.5f, 2.5f, 3.5f, 2.f}, MF[NBAND] = {80, 200, 600, 2500, 5000, 10000},
		            MQ[NBAND] = {0.7f, 1.2f, 2.f, 1.f, 1.5f, 0.7f};
		for (int b = 0; b < NBAND; b++) {
			float g = module ? module->params[eqG(b)].getValue() : MG[b];
			float f = module ? eqValue(eqF(b), module->params[eqF(b)].getValue()) : MF[b];
			float q = module ? eqValue(eqQ(b), module->params[eqQ(b)].getValue()) : MQ[b];
			Shape s = PEAK;
			if (b == 0 && (!module || module->params[EQ_LSHV].getValue() > 0.5f))
				s = LOSHELF;
			if (b == 5 && (!module || module->params[EQ_HSHV].getValue() > 0.5f))
				s = HISHELF;
			coeffs(s, f, g, q, bb[b], aa[b]);
			use[b] = true;
			pf[b] = f;
		}
		if (module) {
			on = module->params[EQ_ON].getValue() > 0.5f;
			if (module->params[EQ_LC].getValue() > 0.5f) {
				coeffs(HIPASS, eqValue(EQ_LCF, module->params[EQ_LCF].getValue()), 0, 0.707, bb[NBAND], aa[NBAND]);
				use[NBAND] = true;
			}
			if (module->params[EQ_HC].getValue() > 0.5f) {
				coeffs(LOPASS, eqValue(EQ_HCF, module->params[EQ_HCF].getValue()), 0, 0.707, bb[NBAND + 1], aa[NBAND + 1]);
				use[NBAND + 1] = true;
			}
		}
		auto total = [&](float f) {
			double w = TWO_PI * f / 96000.0;
			float d = 0.f;
			for (int i = 0; i < NBAND + 2; i++)
				if (use[i] && (on || i >= NBAND))
					d += biquadDb(bb[i], aa[i], w);
			return clamp(d, -16.f, 16.f);
		};
		nvgSave(vg);
		nvgScissor(vg, x0, top - 2.f, x1 - x0, bot - top + 4.f);
		// each band, thin, in its colour
		if (on)
			for (int b = 0; b < NBAND; b++) {
				nvgBeginPath(vg);
				for (int k = 0; k <= NP; k++) {
					float f = 20.f * std::pow(1000.f, k / (float) NP);
					float d = clamp(biquadDb(bb[b], aa[b], TWO_PI * f / 96000.0), -16.f, 16.f);
					float x = fx(f, x0, x1), y = mid - d * dbs;
					k ? nvgLineTo(vg, x, y) : nvgMoveTo(vg, x, y);
				}
				NVGcolor c = BAND_COLOURS[b];
				c.a = 0.45f;
				nvgStrokeColor(vg, c);
				nvgStrokeWidth(vg, 0.8f);
				nvgStroke(vg);
			}
		// the sum
		nvgBeginPath(vg);
		nvgMoveTo(vg, x0, mid);
		for (int k = 0; k <= NP; k++) {
			float f = 20.f * std::pow(1000.f, k / (float) NP);
			nvgLineTo(vg, fx(f, x0, x1), mid - total(f) * dbs);
		}
		nvgLineTo(vg, x1, mid);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0xf2, 0xf3, 0xf7, on ? 0x16 : 0x08));
		nvgFill(vg);
		nvgBeginPath(vg);
		for (int k = 0; k <= NP; k++) {
			float f = 20.f * std::pow(1000.f, k / (float) NP);
			float x = fx(f, x0, x1), y = mid - total(f) * dbs;
			k ? nvgLineTo(vg, x, y) : nvgMoveTo(vg, x, y);
		}
		nvgStrokeColor(vg, on ? WHITE : FAINT);
		nvgStrokeWidth(vg, 1.6f);
		nvgStroke(vg);
		nvgRestore(vg);
		// the bands' points
		if (on) {
			const char* lb[NBAND] = {"L", "1", "2", "3", "4", "H"};
			for (int b = 0; b < NBAND; b++) {
				float x = fx(clamp(pf[b], 20.f, 20000.f), x0, x1), y = mid - total(pf[b]) * dbs;
				nvgBeginPath(vg);
				nvgCircle(vg, x, y, mm2px(1.5f));
				nvgFillColor(vg, BAND_COLOURS[b]);
				nvgFill(vg);
				text(vg, boldFont(), x, y + 2.3f, lb[b], 6.2f, DARK, NVG_ALIGN_CENTER, 0.f);
			}
		}
		int md = module ? module->model.load() : -1;
		if (md > 0 && md < 7)
			text(vg, boldFont(), box.size.x / 2, mid - mm2px(9.f),
			     string::f("EQ MODEL %s ON THE WING – WING-EQ SHOWS THE STD EQ", EQ_MODELS[md]), 7.f, WARN,
			     NVG_ALIGN_CENTER, 0.4f);
		else if (module && !on)
			text(vg, boldFont(), box.size.x / 2, mid - mm2px(9.f), "EQ OFF", 8.f, FAINT, NVG_ALIGN_CENTER, 0.8f);
		Widget::drawLayer(args, layer);
	}
};

// ---------------------------------------------------------------- the display: the channel (a click picks it)
static std::string keyLabel(int key) { return wingproto::tgtLabel(wingproto::tgt(key / 100, key % 100)); }

struct EqDisplay : OpaqueWidget {
	WingEq* module = nullptr;
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
			std::string title = !module ? "CH 1 · KICK" : (key >= 0 ? keyLabel(key) : "NO CHANNEL");
			if (module && !m.name.empty())
				title += " · " + string::uppercase(m.name);
			text(vg, boldFont(), x, mm2px(5.5f), title, 12.f, WHITE, NVG_ALIGN_LEFT, 0.6f);
			std::string st;
			NVGcolor sc = OK;
			if (!module)
				st = "●  FOLLOWS STRIP 1 OF WING-LINK  ·  EQ ON  ·  STD";
			else if (!h) {
				st = "●  right-click › Connect to WING-LINK";
				sc = WARN;
			}
			else if (!hostLinkedEq(h, module)) {
				st = "●  WING-LINK already has a WING-EQ";
				sc = WARN;
			}
			else {
				int mo = module->mode;
				if (mo == WingEq::M_STRIP)
					st = string::f("●  FOLLOWS STRIP %d OF WING-LINK", module->strip.load() + 1);
				else if (mo == WingEq::M_SELECT)
					st = "●  FOLLOWS SELECT ON THE WING";
				else
					st = "●  FIXED  ·  ◀ ▶ PAGE";
				st += module->params[EQ_ON].getValue() > 0.5f ? "  ·  EQ ON" : "  ·  EQ OFF";
				int md = module->model;
				st += (md >= 0 && md < 7) ? std::string("  ·  ") + EQ_MODELS[md] : "";
				if (md > 0)
					sc = WARN;
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
		WingEq* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("WING-EQ  ·  channel"));
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
					sub->addChild(createCheckMenuItem(keyLabel(k), "", [=]() { return m->mode == WingEq::M_FIXED && m->fixedKey == k; },
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

/** A knob's value under it (GAIN in dB, FREQ, Q, a filter's frequency). */
struct ValueWidget : Widget {
	WingEq* module = nullptr;
	int f = 0;
	std::string mock;
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			std::string s = mock;
			if (module) {
				float p = module->params[f].getValue();
				if (eqGain(f))
					s = string::f("%+.1f dB", p);
				else if ((f - EQ_BAND) % 3 == 2 && f < EQ_LSHV)
					s = string::f("%.2f", eqValue(f, p));
				else
					s = freqText(eqValue(f, p));
				if ((f == EQ_LCF && module->params[EQ_LC].getValue() < 0.5f) ||
				    (f == EQ_HCF && module->params[EQ_HC].getValue() < 0.5f))
					s = "OFF";
			}
			text(args.vg, boldFont(), box.size.x / 2, mm2px(1.6f), s, 5.6f, DIM, NVG_ALIGN_CENTER, 0.3f);
		}
		Widget::drawLayer(args, layer);
	}
};

struct EqMediumKnob : RoundKnob {
	EqMediumKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium.svg")));
		bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/medium_bg.svg")));
	}
};

template <int R, int G, int B>
using SmallLatch = VCVLightLatch<MediumSimpleLight<ColorLight<R, G, B>>>;

struct WingEqWidget : ModuleWidget {
	float scan = 0.f;
	WingEqWidget(WingEq* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WingEq.svg")));
		addScrews(this);
		auto area = [&](Widget* w, const float* a) {
			w->box.pos = mm2px(Vec(a[0], a[1]));
			w->box.size = mm2px(Vec(a[2], a[3]));
			addChild(w);
		};
		EqDisplay* d = new EqDisplay;
		d->module = module;
		area(d, L::DISPLAY);
		CurveWidget* c = new CurveWidget;
		c->module = module;
		area(c, L::CURVE);
		addParam(createParamCentered<VCVBezel>(mm(L::PREV), module, WingEq::PREV_PARAM));
		addParam(createParamCentered<VCVBezel>(mm(L::NEXT), module, WingEq::NEXT_PARAM));
		addParam(createLightParamCentered<VCVLightBezelLatch<ColorLight<0xb8, 0xf2, 0x3a>>>(mm(L::EQ_ON), module, EQ_ON,
		                                                                                     WingEq::ON_LIGHT));
		addParam(createParamCentered<VCVBezel>(mm(L::FLAT), module, WingEq::FLAT_PARAM));
		const float* gain[NBAND] = {L::GAIN1, L::GAIN2, L::GAIN3, L::GAIN4, L::GAIN5, L::GAIN6};
		const float* freq[NBAND] = {L::FREQ1, L::FREQ2, L::FREQ3, L::FREQ4, L::FREQ5, L::FREQ6};
		const float* q[NBAND] = {L::Q1, L::Q2, L::Q3, L::Q4, L::Q5, L::Q6};
		const float* vg[NBAND] = {L::VGAIN1, L::VGAIN2, L::VGAIN3, L::VGAIN4, L::VGAIN5, L::VGAIN6};
		const float* vf[NBAND] = {L::VFREQ1, L::VFREQ2, L::VFREQ3, L::VFREQ4, L::VFREQ5, L::VFREQ6};
		const float* vq[NBAND] = {L::VQ1, L::VQ2, L::VQ3, L::VQ4, L::VQ5, L::VQ6};
		const float* cg[NBAND] = {L::CV_GAIN1, L::CV_GAIN2, L::CV_GAIN3, L::CV_GAIN4, L::CV_GAIN5, L::CV_GAIN6};
		const float* cf[NBAND] = {L::CV_FREQ1, L::CV_FREQ2, L::CV_FREQ3, L::CV_FREQ4, L::CV_FREQ5, L::CV_FREQ6};
		const float* cq[NBAND] = {L::CV_Q1, L::CV_Q2, L::CV_Q3, L::CV_Q4, L::CV_Q5, L::CV_Q6};
		const char* MG[NBAND] = {"+4.0 dB", "-3.0 dB", "-4.5 dB", "+2.5 dB", "+3.5 dB", "+2.0 dB"};
		const char* MF[NBAND] = {"80 Hz", "200 Hz", "600 Hz", "2k5", "5k0", "10k"};
		const char* MQ[NBAND] = {"0.70", "1.20", "2.00", "1.00", "1.50", "0.70"};
		auto value = [&](const float* a, int f, const char* mk) {
			ValueWidget* v = new ValueWidget;
			v->module = module;
			v->f = f;
			v->mock = mk;
			area(v, a);
		};
		for (int b = 0; b < NBAND; b++) {
			addParam(createParamCentered<EqMediumKnob>(mm(gain[b]), module, eqG(b)));
			addParam(createParamCentered<GSmallKnob>(mm(freq[b]), module, eqF(b)));
			addParam(createParamCentered<GSmallKnob>(mm(q[b]), module, eqQ(b)));
			value(vg[b], eqG(b), MG[b]);
			value(vf[b], eqF(b), MF[b]);
			value(vq[b], eqQ(b), MQ[b]);
			addInput(createInputCentered<PJ301MPort>(mm(cg[b]), module, b * 3 + 0));
			addInput(createInputCentered<PJ301MPort>(mm(cf[b]), module, b * 3 + 1));
			addInput(createInputCentered<PJ301MPort>(mm(cq[b]), module, b * 3 + 2));
		}
		addParam(createLightParamCentered<SmallLatch<0xff, 0x6b, 0x5b>>(mm(L::SHV1), module, EQ_LSHV, WingEq::LSHV_LIGHT));
		addParam(createLightParamCentered<SmallLatch<0xb4, 0x8c, 0xff>>(mm(L::SHV6), module, EQ_HSHV, WingEq::HSHV_LIGHT));
		addParam(createLightParamCentered<SmallLatch<0xb8, 0xf2, 0x3a>>(mm(L::LC_ON), module, EQ_LC, WingEq::LC_LIGHT));
		addParam(createLightParamCentered<SmallLatch<0xb8, 0xf2, 0x3a>>(mm(L::HC_ON), module, EQ_HC, WingEq::HC_LIGHT));
		addParam(createParamCentered<GSmallKnob>(mm(L::LCF), module, EQ_LCF));
		addParam(createParamCentered<GSmallKnob>(mm(L::HCF), module, EQ_HCF));
		value(L::VLCF, EQ_LCF, "30 Hz");
		value(L::VHCF, EQ_HCF, "OFF");
		addInput(createInputCentered<PJ301MPort>(mm(L::CV_LC), module, WingEq::CV_LC));
		addInput(createInputCentered<PJ301MPort>(mm(L::CV_HC), module, WingEq::CV_HC));
		addInput(createInputCentered<PJ301MPort>(mm(L::CV_ON), module, WingEq::CV_ON));
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
		WingEq* m = dynamic_cast<WingEq*>(module);
		if (!m)
			return;
		appendLinkMenu(menu, m);
		menu->addChild(createMenuLabel("The channel: click the display, or ◀ ▶"));
		menu->addChild(createMenuLabel("WING-EQ shows the WING's STD EQ (other EQ models: switch to STD on the WING)"));
	}
};

}  // namespace winglink

Model* modelWingEq = createModel<winglink::WingEq, winglink::WingEqWidget>("WingEq");

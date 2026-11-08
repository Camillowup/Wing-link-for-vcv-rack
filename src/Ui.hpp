// SPDX-License-Identifier: GPL-3.0-only
// The look of the GENRA plugin (colours, fonts, text, lit buttons, a text field in a display), for WING-LINK.
#pragma once
#include "plugin.hpp"
#include <blendish.h>

namespace wlui {

static const NVGcolor WHITE = nvgRGB(0xf2, 0xf3, 0xf7), TXT = nvgRGB(0xd9, 0xdb, 0xe2), DIM = nvgRGB(0x8a, 0x8d, 0x99),
                      FAINT = nvgRGB(0x5d, 0x60, 0x6b), OFF = nvgRGB(0x2a, 0x2b, 0x33), DARK = nvgRGB(0x07, 0x08, 0x0a),
                      ACCENT = nvgRGB(0xff, 0x6b, 0x5b), OK = nvgRGB(0xb8, 0xf2, 0x3a), WARN = nvgRGB(0xff, 0xd8, 0x4a),
                      BAD = nvgRGB(0xff, 0x4f, 0x4f);

inline Vec mm(const float* p) { return mm2px(Vec(p[0], p[1])); }

inline std::shared_ptr<window::Font> boldFont() {
	return APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
}
inline std::shared_ptr<window::Font> monoFont() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

/** Text at a baseline; align NVG_ALIGN_LEFT / CENTER / RIGHT. Returns the advance (px). */
inline float text(NVGcontext* vg, const std::shared_ptr<window::Font>& f, float x, float y, const std::string& s,
                  float size, NVGcolor c, int align = NVG_ALIGN_LEFT, float spacing = 0.4f) {
	if (!f || f->handle < 0 || s.empty())
		return 0.f;
	nvgFontFaceId(vg, f->handle);
	nvgFontSize(vg, size);
	nvgTextLetterSpacing(vg, spacing);
	nvgTextAlign(vg, align | NVG_ALIGN_BASELINE);
	nvgFillColor(vg, c);
	return nvgText(vg, x, y, s.c_str(), nullptr) - x;
}

/** The light of a lit button, in a fixed colour. */
template <int R, int G, int B>
struct ColorLight : GrayModuleLightWidget {
	ColorLight() { addBaseColor(nvgRGB(R, G, B)); }
};

/** A text field drawn into a dark display: no box, the display's font, a colour of its own. */
struct DisplayTextField : app::LedDisplayTextField {
	float fontSize = 8.f;
	int align = BND_CENTER;
	DisplayTextField() {
		bgColor = nvgRGBA(0, 0, 0, 0);
		color = WHITE;
		textOffset = Vec(0, -3.5f);  // blendish puts the baseline 14 px below y and 4 px in
		multiline = false;
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		nvgScissor(args.vg, RECT_ARGS(args.clipBox));
		if (layer == 1) {
			std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
			if (font && font->handle >= 0) {
				bndSetFont(font->handle);
				NVGcolor hl = color;
				hl.a = 0.5f;
				int begin = std::min(cursor, selection);
				int end = (this == APP->event->selectedWidget) ? std::max(cursor, selection) : -1;
				if (text.empty() && this != APP->event->selectedWidget)
					bndIconLabelCaret(args.vg, textOffset.x, textOffset.y, box.size.x - 2 * textOffset.x, box.size.y,
					                  -1, FAINT, fontSize, placeholder.c_str(), hl, -1, -1);
				else
					bndIconLabelCaret(args.vg, textOffset.x, textOffset.y, box.size.x - 2 * textOffset.x, box.size.y,
					                  -1, color, fontSize, text.c_str(), hl, begin, end);
				bndSetFont(APP->window->uiFont->handle);
			}
		}
		Widget::drawLayer(args, layer);
		nvgResetScissor(args.vg);
	}
	int getTextPosition(math::Vec mousePos) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
		if (!font || font->handle < 0)
			return 0;
		bndSetFont(font->handle);
		int pos = bndIconLabelTextPosition(APP->window->vg, textOffset.x, textOffset.y, box.size.x - 2 * textOffset.x,
		                                   box.size.y, -1, fontSize, text.c_str(), mousePos.x, mousePos.y);
		bndSetFont(APP->window->uiFont->handle);
		return pos;
	}
};

}  // namespace wlui

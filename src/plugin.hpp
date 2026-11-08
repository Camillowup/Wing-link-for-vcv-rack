// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rack.hpp>

using namespace rack;

extern Plugin* pluginInstance;
extern Model* modelWingLink;
extern Model* modelWingLinkX;
extern Model* modelWingFx;

/** The dark knob with the white pointer of the GENRA plugin (res/knob/small*.svg). */
struct GSmallKnob : RoundKnob {
	GSmallKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/small.svg")));
		bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob/small_bg.svg")));
	}
};

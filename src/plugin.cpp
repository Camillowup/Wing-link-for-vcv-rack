// SPDX-License-Identifier: GPL-3.0-only
#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelWingLink);
	p->addModel(modelWingLinkX);
	p->addModel(modelWingFx);
	p->addModel(modelWingEq);
	p->addModel(modelWingDyn);
	p->addModel(modelWingKnobs);
	p->addModel(modelWingIn);
}

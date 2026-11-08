// SPDX-License-Identifier: GPL-3.0-only
#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelWingLink);
	p->addModel(modelWingLinkX);
}

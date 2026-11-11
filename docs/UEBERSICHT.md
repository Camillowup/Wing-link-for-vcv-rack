# Übersicht: wo liegt was

Plugin **WING-LINK** (Slug `WingLink`), eigenes Plugin im GENRA-Look. Alle Module liegen flach in `src/`.

| Modul (Slug) | Code (`src/`) | Panel-Skript (`tools/panels/`) | Grafik (`res/`) | Bild (`docs/panels/`) |
| --- | --- | --- | --- | --- |
| WING-LINK (`WingLink`), WING-LINK X (`WingLinkX`) | `WingLink.cpp/.hpp`, `WingLinkLayout.hpp` | `winglink_panel.py` | `WingLink.svg`, `WingLinkX.svg`, `wing/` (Fader) | `winglink.png` |
| WING-FX (`WingFx`) | `WingFx.cpp`, `WingFxLayout.hpp`, `WingFxMap.hpp`, `WingFxModels.hpp` | `wingfx_panel.py` | `WingFx.svg` | `wingfx.png` |
| WING-EQ (`WingEq`) | `WingEq.cpp`, `WingEqLayout.hpp` | `wingeq_panel.py` | `WingEq.svg` | `wingeq.png` |
| WING-DYN (`WingDyn`) | `WingDyn.cpp`, `WingDynLayout.hpp` | `wingdyn_panel.py` | `WingDyn.svg` | `wingdyn.png` |
| WING-KNOBS (`WingKnobs`) | `WingKnobs.cpp`, `WingKnobsLayout.hpp` | `wingknobs_panel.py` | `WingKnobs.svg` | `wingknobs.png` |
| WING-IN (`WingIn`) | `WingIn.cpp`, `WingInLayout.hpp` | `wingin_panel.py` | `WingIn.svg` | `wingin.png` |

| Datei / Ordner | Inhalt |
| --- | --- |
| `src/WingProto.hpp` | das WING-Protokoll (Sysex/MIDI), Test `test/test_proto.cpp` (ohne Rack) |
| `src/Ui.hpp`, `src/plugin.*` | gemeinsame Bedienteile, Knöpfe (`res/knob/`), Modell-Liste |
| `tools/panels/` | Panel-Skripte (`<modul>_panel.py real` schreibt `res/*.svg` + `src/*Layout.hpp`, `preview OUT.png RACK` ein Bild); gemeinsame Teile `panelkit.py`, `satpanel.py` |
| `tools/fx_models.py`, `tools/data/` | schreibt `src/WingFxModels.hpp` aus `tools/data/wing_fx_params.json` |
| `docs/MERKLISTE.md` | offene Wünsche; Erledigtes und alte Entwürfe in `docs/archiv/` |

Branches: `main` = Stand (jeder Push baut Windows + Mac), `release` = ein Push dorthin veröffentlicht den Release
(nur auf ausdrücklichen Wunsch).

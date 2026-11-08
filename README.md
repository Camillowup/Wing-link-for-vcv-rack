# WING-LINK für VCV Rack

Eine Brücke von VCV Rack zum Behringer **WING** (gebaut und getestet für die WING COMPACT) über **ein USB-Kabel**,
im Look des GENRA-Plugins.

![Panel](docs/panel.png)

- **WING-LINK**: 8 Spuren. **WING-LINK X**: 8 Spuren mehr. **WING-FX**: die 16 Effekte der WING.
- WING-LINK X und WING-FX verbinden sich **von selbst**, wenn es nur ein WING-LINK im Patch gibt. Sonst per
  Rechtsklick › *Connect to WING-LINK*, oder angedockt (X rechts neben WING-LINK, FX links daneben).
- **Audio**: Was in die AUDIO-Buchsen geht, landet direkt auf den USB-Eingängen der WING. WING-LINK ist dafür
  selbst ein Audio-Ausgang (wie Racks Audio-Modul), ein Audio-16-Modul braucht es nicht.
- **Steuerung** über USB-MIDI, in beide Richtungen: Fader, Pan, Mute, Solo und zwei Sends (FX 1 / FX 2 auf frei
  wählbare Busse).
- **Name, Farbe und Icon** jeder Spur stellt man auf dem Panel ein, sie gehen sofort an den WING-Kanal.
- **CV**: Fader, Pan und Sends per CV fahren (die Fader bewegen sich mit, auch an der WING). Ausgänge: Fader als
  0–10 V und Mute als Gate.

## Einrichten (einmal)

1. **Treiber**: Den Windows-ASIO-Treiber der WING von behringer.com installieren, PC neu starten.
2. **WING per USB** an den PC.
3. **An der WING**:
   - *Setup › USB-Audio*: **48 Kanäle** (WING-LINK schickt standardmässig CH 33 → USB 33 usw.).
   - *Setup › Remote › MIDI Remote Control*: für **USB** die Kanal-Steuerung (Fader/Mute/Pan) und **SYSEX**
     erlauben. Ohne Sysex gehen nur Fader, Mute und Pan (gröber), Namen/Farben/Icons/Solo/Sends nicht.
4. **In Rack**: *Engine › Sample rate* auf **48 kHz**. WING-LINK ins Rack ziehen: Es sucht die WING selbst
   (MIDI und Audio, ASIO zuerst). Das Display zeigt, ob alles verbunden ist.
5. Spuren in die **AUDIO**-Buchsen stecken, Namen tippen, Farben und Icons wählen, dann **SYNC** drücken:
   WING-LINK benennt die WING-Kanäle und stellt ihre Quelle auf die passenden USB-Eingänge.

## Bedienung

- **START CH − / +**: Ab welchem WING-Kanal die Spuren liegen (Standard CH 33). CH 41–48 sind an der WING die
  AUX-Kanäle 1–8.
- **Farbbalken** einer Spur anklicken: nächste Farbe. **Icon** anklicken: nächste Icon-Gruppe. Rechtsklick auf die
  Spur: alle Farben und Icons. **Name**: direkt hineinklicken und tippen (max. 16 Zeichen, ohne Umlaute).
- **SYNC**: schickt Namen, Farben, Icons und das USB-Routing an die WING und liest danach Fader, Pan, Mute, Solo
  und Sends von der WING zurück.
- Beim Verbinden übernimmt WING-LINK die Stellungen der WING (die WING gilt).
- Rechtsklick aufs Modul: Audio-Gerät, USB-Kanäle, MIDI-Ports von Hand, Busse für FX 1 / FX 2, alles neu lesen.

## WING-FX

![WING-FX](docs/panel_fx.png)

- Zeigt 4 FX-Plätze, **SLOTS − / +** blättert durch FX 1–16.
- **Effekt wählen**: aufs Modell klicken › *PREMIUM* (Hall, Delay, Modulation, Pitch – nur auf FX 1–8) oder
  *STANDARD* (EQ, Dynamik, Amps …).
- **ON**, **MIX** und vier Parameter je Effekt (die Namen wechseln mit dem Effekt), alles in beide Richtungen und
  per CV (0–10 V).
- **Tempo**: *SYNC an*: die Delays übernehmen Racks Tempo (CLOCK IN oder TAP). *SYNC aus*: das Tempo kommt von der
  WING (dort tappen), CLOCK OUT gibt es an Rack weiter. Auflösung von CLOCK IN / OUT per Rechtsklick (Standard 16tel).
- An der WING zusätzlich unter *Setup › Remote › MIDI Remote Control* die **FX-Steuerung** für USB erlauben.

## Gut zu wissen

- Läuft schon ein Audio-Modul von Rack auf der WING (z. B. zum Abhören auf USB 1–2), stört das nicht, solange es
  andere USB-Kanäle benutzt als WING-LINK.
- Ändert man an der WING die USB-Kanalzahl oder die Samplerate, trennt sich USB kurz; WING-LINK verbindet sich
  von selbst wieder.
- Noch nicht an der echten WING geprüft: die Reihenfolge der Farben, Solo per Fernsteuerung, der Name des
  MIDI-Ports der WING COMPACT, ON der Effekte (läuft über den Insert-Schalter der WING). Rückmeldungen dazu bitte in die Issues.

## Bauen

Über GitHub Actions (Windows), siehe `.github/workflows/main.yml`. Panels: `tools/winglink_panel.py real` und
`tools/wingfx_panel.py real` (schreiben `res/*.svg` und die Layout-Header), Vorschau mit `preview OUT.png RACK`.
Die Effekt-Liste: `tools/fx_models.py` schreibt `src/WingFxModels.hpp` aus `tools/data/wing_fx_params.json`.

Lizenz: GPL-3.0. Protokoll nach Patrick-Gilles Maillots „WING Remote Protocols“.

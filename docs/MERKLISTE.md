# WING-LINK – Merkliste (offen)

Nur Offenes; gebaut wird erst auf „bauen“. Erledigtes steht in `docs/archiv/ERLEDIGT.md`.

Gesammelt für den nächsten Bau (12. Nov. 2026, aus dem Workspace):
- **FEHLER WING-LINK X: zieht beim Einfügen die Fader an der WING hoch.** Beim Dazukommen von X alles neu von der
  WING lesen; eine neu erscheinende Spur übernimmt den Wert der WING, statt ihren Startwert (0 dB) zu senden.
- **WING-EQ: Punkte im Display mit der Maus ziehen** – links/rechts FREQ, hoch/runter GAIN, Mausrad Q, Doppelklick
  0 dB, Shift fein, Strg+Z; beim Ziehen Wert-Anzeige (z. B. BAND 2 · 1.2 kHz · +4.5 dB). Steckt CV am Band: dort nicht.
- **WING-EQ: SHV-Taster weg, LOW/HIGH fest Shelf** (Peak nur per Rechtsklick), FILTER-Spalte bleibt. Panel vorher zeigen.
- **WING-EQ + WING-DYN: SELECT als Standard** – neu eingefügte folgen dem an der WING angewählten Kanal; Spur folgen /
  fester Kanal per Klick aufs Display; Statuszeile zeigt den Kanal; gespeicherte Patches behalten ihre Einstellung.

Zu prüfen / Fehler (gemeldet 19. Nov. 2026):
- **WING-LINK X übernimmt Name und Farbe der WING nicht.** Verdacht: bei START CH 33 liegt X auf 41–48 = AUX 1–8
  (nicht auf Kanälen). Sonst MIDI-Log (Rechtsklick WING-LINK › Log MIDI to file).
- **Spur per Rechtsklick auf einen anderen Kanal (z. B. 15): Name / Farbe nicht an der WING.** Klären: kommt auch ein
  danach in Rack getippter Name nicht an (Fehler), oder war erwartet, dass der Name mit der Spur wandert (Regel ändern?).
- **WING-REC** (2.8.0) an der WING testen: SD-Befehle (Marker, Sessions, Play-Modus `A->B`, Session-Namen), Stereo-
  Erkennung (wie ist Stereo an der WING eingestellt: Quelle ST oder zwei verlinkte Kanäle?), Ableton-Set in Live 12.
- SPLIT: Spurnamen aus dem Routing der WING (WLIVE REC) statt `SD 1 …`.

Später:
- **WING-AUTO** – Fader-, Mute- und FX-Bewegungen aufnehmen und pro Song-Teil abspielen (synchron zu GENRAs Clock).
- **WING-FX VIEW** – eigene Grafik je Effektart und alle Parameter eines FX-Platzes.
- **WING-SNAP** – Szenen von Rack aus aufrufen: geht per MIDI (CC0 Bank-Select + Program Change auf Kanal 7 bzw. 8,
  dazu GO / Zurück / Weiter; an der WING SETUP › MIDI REMOTE CONTROL › SCENE CHANGE bzw. SHOW CONTROL).
  **Beim Bau WING-KNOBS auf Kanal 6 beschränken** (Kanal 7/8 kollidieren sonst mit dem Szenenaufruf).
- Pegel der WING als CV – nur, falls es einen Weg über USB gibt.

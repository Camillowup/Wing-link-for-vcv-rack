# WING-LINK – Änderungen

## 2.8.0

Neues Modul **WING-REC** (Rekorder): jede Spur von WING-LINK, X und WING-IN (oder IN 1–16) als eigene WAV in Rack und
die SD-Karten der WING (WING-LIVE) mit einem REC; SYNC zu GENRA (RUN/STOP), Marker per MARK und bei jedem ABLAUF-Song-
Teil, Stereo-Paare (ST, von der WING übernommen), pro Aufnahme ein Ordner mit `Markers.txt` und Ableton-Live-12-Set;
SD-Transport, Marker, Sessions, Play-Modus; SPLIT zerlegt eine SD-Session in eine WAV pro Spur.
Noch nicht an der WING geprüft.

WING-LINK: fragt für WING-REC zusätzlich ab, ob die Quelle eines Kanals stereo ist, und mehr vom SD-Rekorder
(Marker, Sessions).

## 2.7.0

Fehler behoben und Rechenzeit gespart; an Bedienung und Aussehen ändert sich nichts. Noch nicht an der WING getestet.

Fehler:
- **Initialize / Randomize** schicken nichts mehr an die WING: Randomize lässt alle Regler, die zur WING gehören, in
  Ruhe (WING-LINK, X, FX, EQ, DYN); nach Initialize werden die Werte der WING neu gelesen statt 0 dB / Mute aus gesendet.
- **CV auf einem Fader, EQ- oder FX-Regler** hält die Abfragen nicht mehr auf (Namen, Solo, Sends, FX, SD-Status):
  ein Wert geht nur noch hinaus, wenn er sich – gerundet wie die WING ihn nimmt (0,1 dB …) – geändert hat, und eine
  Abfrage, die 100 ms wartet, kommt zuerst dran.
- Eine Fehlermeldung der WING landet bei der Abfrage, zu der sie gehört (vorher manchmal beim letzten Befehl).
- WING-IN: nach dem Entfernen und Wieder-Einfügen spielt kein alter Rest aus dem Puffer.
- Zahlen an die WING immer mit Dezimalpunkt, auch wenn ein DAW-Host (Rack als Plugin) auf Komma umstellt.
- Audio-Thread: keine Texte mehr für das MIDI-Log, solange nicht geloggt wird; kein Warten auf die Anzeige (Mutex);
  keine neuen Einträge in der Namensliste; zwei Datenrennen zwischen Engine und Anzeige beseitigt (Log, Modellnamen
  von WING-DYN).

Rechenzeit:
- Namen, Farben, Icons und Quellen der Kanäle werden alle 6 s abgefragt (vorher alle 1,5 s); Fader, Sends, Solo, FX,
  EQ, DYN wie bisher.
- Der Steuertakt richtet sich nach der Abtastrate (bei 96 / 192 kHz nicht mehr doppelt / vierfach so oft).
- Lichter von WING-FX, -EQ, -DYN im Steuertakt statt pro Sample; kleinere Ersparnisse im Steuertakt und bei WING-IN.
- Anzeige: die EQ-Kurve wird nur neu gerechnet, wenn sich ein Regler bewegt hat; Name / Farbe / Icon je Kanalzug
  einmal pro Bild geholt.
- Beim Verbinden werden die FX-Plätze nicht mehr mehrfach abgefragt.

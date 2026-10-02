# AxiDraw- und NextDraw-Unterstützung (Recherche)

Stand: 30.09.2026. Nur anhand der Dokumentation recherchiert, noch nicht mit echter Hardware getestet.

## Hardware

- AxiDraw (V3, SE, MiniKit usw.) und der Nachfolger Bantam Tools NextDraw nutzen dasselbe **EiBotBoard (EBB)**:
  PIC18F46J50 mit zwei Schrittmotortreibern. Am USB meldet es sich als serieller Port (CDC). Der generische
  CDC-Pfad in `usb_serial.cpp` sollte es öffnen.
- Mechanik H-Bot/CoreXY: Motor 1 fährt `x + y` Schritte, Motor 2 `x − y`.
- Auflösung: 2032 Schritte/Zoll = **80 Schritte/mm** bei 1/16-Mikroschritt.
- Firmware: NextDraw-Software verlangt EBB-Firmware ≥ 3.0.2. Ältere AxiDraws laufen evtl. noch mit 2.x.
  `LM` gibt es ab 2.5.3.

## Protokoll

| Zweck | Befehl |
|---|---|
| Firmwareversion abfragen | `V` |
| Antwortformat vereinheitlichen (ab FW 3) | `CU,10,1` |
| Motoren an, 1/16-Mikroschritt | `EM,1,1` |
| Motoren aus | `EM,0,0` |
| Bewegung mit Beschleunigung (ab FW 2.5.3) | `LM,Rate1,Steps1,Accel1,Rate2,Steps2,Accel2[,Clear]` |
| Bewegung mit fester Dauer (Rückfall) | `XM,Dauer_ms,A,B` |
| Stift-Servo stellen | `S2,Position,Pin,Rate,Delay` |
| Status (Befehl läuft, Motoren, FIFO) | `QG`, bei älterer FW `QM` |
| Not-Aus: stoppen, Warteschlange leeren | `ES` |

- Zeilenende beim Senden: `\r`.
- Handshake: Jeder Befehl wird mit `OK\r\n` bestätigt (Legacy-Format), Abfragen liefern stattdessen Daten.
  Keine Zeilennummern, keine Prüfsummen, kein `Resend`, kein `M110`.
- Die Warteschlange ist standardmäßig einen Befehl tief (vergrößerbar mit `CU,4,n`). Ist sie voll, blockiert
  das EBB die Antwort, bis wieder Platz ist. Das entspricht dem Prinzip „eine Zeile unterwegs, auf ok warten“.
- `LM`: Rate und Beschleunigung werden alle 40 µs auf einen 31-Bit-Akkumulator addiert.
  Rate ≈ Schritte/s × 2³¹ / 25000. Höchstens 25 000 Schritte/s.
- Stift: Klassisches AxiDraw und NextDraw steuern den Servo über **verschiedene Pins und Wertebereiche**
  (siehe saxi-Issue #340). Deshalb braucht es Maschinenprofile.

## Taste zum Pausieren (Stiftwechsel)

- Das EBB hat eine Taste **PRG**, beim AxiDraw die Pause-Taste. Die Firmware reagiert nicht selbst darauf:
  Bewegungen laufen weiter, und jeder Befehl wird weiter mit `OK` bestätigt. Sie merkt sich nur den Druck.
- Der Host muss abfragen. In der AxiDraw-Software pausiert der Rechner: Warteschlange abarbeiten, Stift hoch,
  Position merken. Fortgesetzt wird am Rechner.
- Abfrage:
  - FW 2.x: `QB` → `1`, wenn seit der letzten Abfrage gedrückt (gespeichert)
  - FW 3.x: `QB` gilt als veraltet, stattdessen Bit 5 von `QG`. Ob dieses Bit den Druck ebenfalls speichert,
    ist nicht eindeutig dokumentiert.
- Die zweite Taste **RST** setzt das Board hart zurück. Die USB-Verbindung bricht ab, µprint sieht
  „Drucker getrennt“.

Umsetzung in µprint: Zwischen den Bewegungsbefehlen die Taste abfragen. Bei einem Druck das aktuelle Segment zu
Ende fahren, Stift hoch, Position merken, „Pausiert“ im Web-UI anzeigen. Fortsetzen per Web-UI **oder erneutem
Tastendruck**: zurück zur gemerkten Position, Stift runter, weiter. Die Motoren bleiben in der Pause an, damit
die Position stimmt. Die bestehende Pause-Logik wird dafür erweitert.

## Ansatz: G-Code auf dem ESP32 übersetzen

µplot erzeugt bereits Plotter-G-Code (`G0`/`G1`, Stift über Z-Höhen, sortierte Pfade über vpype). µprint
übersetzt einen einfachen G-Code-Teil in EBB-Befehle:

- `G0`/`G1 X Y F` → `LM` mit Beschleunigungsrampe
- Z-Wechsel (oder ein eigener Befehl) → `S2` (Stift hoch/runter)
- `M84` → `EM,0,0`

Hochladen, Starten, Pause, Abbrechen und Fortschritt funktionieren dann wie beim Drucker. Die Planung besteht
aus einer Trapez- bzw. Dreiecksrampe pro Segment mit etwas Vorausschau für die Eckgeschwindigkeit.

Verworfene Alternative: SVG im Browser planen (wie saxi) und fertige EBB-Befehle hochladen. Das doppelt
µplot und schreibt ein eigenes Dateiformat vor.

saxi steht unter AGPL-3.0, daher darf kein Code übernommen werden, nur das Prinzip.

## Aufteilung zwischen µplot und µprint

Nullpunkt, Achsrichtung und Zeichenfläche regelt **µplot** über ein AxiDraw-Profil. Das AxiDraw hat seinen
Nullpunkt oben links (Stiftposition beim Einschalten), Y zeigt vom Gerät weg, und es gibt keinen Düsen-Offset.
µprint übersetzt nur und rechnet nichts um. So stimmt der G-Code mit der µplot-Vorschau überein.

## Testmodus (Alpha)

Bevor der Plotter-Modus entsteht, gibt es in µprint einen Test, der ein echtes Gerät kennenlernt:

- Einstellungen → Gerätetyp „AxiDraw / NextDraw“. µprint verbindet neu, sendet kein `M110` und fragt nur `V` ab.
  Drucken ist in diesem Modus gesperrt.
- Karte „AxiDraw-Test“ → „Test starten“. Der Ablauf (etwa eine Minute) läuft in `main/axidraw.c`:
  1. Verbindung: 1 s auf unaufgeforderte Meldungen warten
  2. Firmware und Status: `V`, `QT`, `QC`, `QE`, `QG`, `QM`, `QS`, `QB`, ab FW 3 `QU,2`
  3. Stift: `SP,1` / `SP,0` / `SP,1`, jeweils danach `QG`
  4. Optional Bewegung: `XM`-Quadrat (10 mm), `LM` konstant hin und zurück, `LM` mit Rampe hin und zurück,
     danach jeweils `QS` (erwartet 0,0)
  5. PRG-Taste live: 15 s lang `QG` alle 100 ms, protokolliert werden nur Änderungen
  6. PRG-Taste gespeichert? 8 s keine Abfrage, dabei einmal drücken, danach `QG` und `QB`
- Jede gesendete und empfangene Zeile landet mit Zeitstempel im Log. Im Browser kreuzt man an, was man
  gesehen hat (Stift, Quadrat, LM-Fahrten, zurück am Start), und lädt alles als `.txt` herunter.

Ablauf für einen Tester:

1. ESP32-S3 über den Web-Flasher einrichten (Umschalter auf „Alpha“, neueste Version) und ins WLAN bringen. Ein schon
   eingerichtetes Gerät bekommt die Alpha als `uprint-…-ota.bin` (Vorab-Release) unter Einstellungen → Firmware
2. AxiDraw anschließen (USB und Netzteil), Gerätetyp umstellen, Test starten, den Anweisungen folgen
3. Log herunterladen und schicken

Tags mit Bindestrich (z. B. `v0.4.0-alpha.1`) werden als Vorab-Release gebaut (nicht „latest“). Der Web-Flasher
listet alle Releases; der Umschalter „Release / Alpha“ filtert das Versions-Dropdown.

## Aufgaben

1. ~~Gerätetyp in den Einstellungen~~ (erledigt für den Testmodus)
2. ~~Testmodus mit Log~~ (erledigt, wartet auf den ersten Hardwaretest)
3. Hardwaretest beim Bekannten, Log auswerten
4. Protokollschicht für den Plotter-Modus: Handshake auf `OK`, bei FW 3 `CU,10,1`
5. Einstellungen für den Plotter: Maschinenprofil (AxiDraw/NextDraw), Stifthöhen, Geschwindigkeit, Beschleunigung
6. Übersetzer G-Code → `LM`/`S2` mit Vorausschau zum Abbremsen an Ecken
7. Abbrechen über `ES`, danach Stift hoch und Motoren aus
8. Pause über die PRG-Taste: regelmäßig abfragen, Stift hoch, Position merken, Fortsetzen per Web-UI oder
   erneutem Tastendruck
9. Temperaturen und andere Druckerfelder im Plotter-Modus ausblenden
10. µplot: AxiDraw-Profil mit Zeichenfläche (A4/A3), Offset 0, Nullpunkt oben links, Y gespiegelt;
    Stift weiter über Z, µprint wertet den Z-Wechsel aus
11. Gesamttest beim Bekannten: SVG → µplot → µprint → AxiDraw, mit Stiftwechsel über die Taste

## Offen

- Erkennt der ESP32-S3 das EBB sauber als CDC? Welche VID/PID meldet es?
- Stift-Timing und Servowerte je Maschine. Der Test nutzt `SP` mit den Werten aus der Firmware; `S2` mit
  eigenen Werten und der NextDraw-Pin kommen später
- Speichert Bit 5 von `QG` (FW 3.x) einen Tastendruck bis zur nächsten Abfrage, oder zeigt es nur den
  aktuellen Zustand? Testbar am Gerät des Bekannten: Taste kurz drücken, danach `QG` senden.
  Welche Firmware hat sein Gerät (`V`)?
- Test auf echter Hardware

## Quellen

- [EBB Command Set, Firmware v3.0+](https://evil-mad.github.io/EggBot/ebb.html)
- [EBB (EiBotBoard) – Hardware](http://www.schmalzhaus.com/EBB/)
- [saxi](https://github.com/nornagon/saxi) (AGPL-3.0), [Issue #340: NextDraw-Stiftservo](https://github.com/alexrudd2/saxi/issues/340)
- [Bantam Tools NextDraw Firmware](https://support.bantamtools.com/hc/en-us/articles/28809123473043-Bantam-Tools-NextDraw-Firmware)
- [AxiDraw V3 – 2032 Schritte/Zoll](https://shop.evilmadscientist.com/productsmenu/846)
- [µplot](https://github.com/dmyrenne/uplot)

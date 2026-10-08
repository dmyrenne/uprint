# AxiDraw- und NextDraw-Unterstützung (Recherche)

Stand: 08.10.2026. Erster Hardwaretest mit einem AxiDraw mit **EBB-Firmware 2.8.1** (Log vom 08.10.2026,
µprint 0.4.0-alpha.4). Alles zu FW 3.x stammt nur aus der Dokumentation.

> **Wir können nur mit FW 2.x testen.** Unterstützung für FW 3.x (u. a. NextDraw) ist **Beta**: µprint erkennt
> die Version und erwartet das dokumentierte Antwortformat, geprüft hat das niemand. Wer ein Gerät mit FW 3.x
> anschließt, bekommt in der Statuszeile den Hinweis, den AxiDraw-Test laufen zu lassen und das Log zu schicken.

## Hardware

- AxiDraw (V3, SE, MiniKit usw.) und der Nachfolger Bantam Tools NextDraw nutzen dasselbe **EiBotBoard (EBB)**:
  PIC18F46J50 mit zwei Schrittmotortreibern. Am USB meldet es sich als serieller Port (CDC) mit
  **VID 0x04D8, PID 0xFD92**. Der generische CDC-Pfad in `usb_serial.cpp` öffnet es (getestet). Nach dem
  Verbinden kommen keine unaufgeforderten Meldungen.
- Mechanik H-Bot/CoreXY: Motor 1 fährt `x + y` Schritte, Motor 2 `x − y`. Getestet: 800 Schritte auf beiden
  Motoren fahren rein in +X, also nach rechts.
- Auflösung: 2032 Schritte/Zoll = **80 Schritte/mm** bei 1/16-Mikroschritt.
- Firmware: NextDraw-Software verlangt EBB-Firmware ≥ 3.0.2. Ältere AxiDraws laufen noch mit 2.x (das
  Testgerät: 2.8.1). `LM` gibt es ab 2.7.0, `QG` ab 2.6.2, `QE` ab 2.8.0.
- Ob 80 Schritte/mm stimmen, ist noch nicht nachgemessen. Der Test zeigt nur, dass keine Schritte verloren gehen.

## Protokoll

| Zweck | Befehl |
|---|---|
| Firmwareversion abfragen | `V` |
| Antwortformat vereinheitlichen (ab FW 3) | `CU,10,1` |
| Motoren an, 1/16-Mikroschritt | `EM,1,1` |
| Motoren aus | `EM,0,0` |
| Bewegung mit Beschleunigung (ab FW 2.7.0) | `LM,Rate1,Steps1,Accel1,Rate2,Steps2,Accel2[,Clear]` |
| Bewegung mit fester Dauer (Rückfall) | `XM,Dauer_ms,A,B` |
| Stift-Servo stellen | `S2,Position,Pin,Rate,Delay` |
| Status (Befehl läuft, Motoren, FIFO) | `QG`, bei älterer FW `QM` |
| Not-Aus: stoppen, Warteschlange leeren | `ES` |

- Zeilenende beim Senden: `\r`.
- Keine Zeilennummern, keine Prüfsummen, kein `Resend`, kein `M110`.
- Die Warteschlange ist standardmäßig einen Befehl tief (vergrößerbar mit `CU,4,n`). Ist sie voll, blockiert
  das EBB die Antwort, bis wieder Platz ist. Getestet: Die ersten beiden `XM` werden sofort bestätigt, ab dem
  dritten kommt das `OK` erst, wenn die vorige Bewegung fertig ist. Das entspricht dem Prinzip „eine Zeile
  unterwegs, auf ok warten“.

### Antwortformate

Wie eine Antwort endet, hängt von der Firmware ab. µprint fragt nach jeder Verbindung `V` ab und legt danach
fest, worauf es wartet (`ebb_syntax_t` in `main/axidraw.c`). So muss es nie auf einen Zeitablauf warten.

| Befehl | FW 2.x, Legacy (getestet mit 2.8.1) | FW 3.x nach `CU,10,1` (Beta, nur Doku) |
|---|---|---|
| `V` | Datenzeile, **kein `OK`** | `V,<Text>` |
| `QG`, `QM` | Datenzeile, **kein `OK`** | `QG,<Hex>` bzw. `QM,…` |
| `QB`, `QC`, `QE`, `QS`, `QT`, `QU`, `ES` | Datenzeile, dann `OK` | `<Befehl>,<Daten>` |
| alle anderen (`SP`, `EM`, `CS`, `XM`, `LM`, …) | `OK` | `<Befehl>` |
| Fehler | `!<Code> Err: …`, evtl. gefolgt von `OK` | `<Befehl>,!<Code> Err: …` |

- FW 2.x: kein `CU,10,1` (gibt es erst ab 3.0), µprint bleibt im Legacy-Format.
- FW 3.x: µprint sendet `CU,10,1` und prüft mit `QT`, ob die Antwort jetzt mit `QT` beginnt. Wenn nicht, bleibt
  es bei Legacy. Steht das Board noch von einer früheren Verbindung auf `CU,10,1`, erkennt µprint das an `V,…`.
- Firmware nicht erkannt: wie bisher bis `OK`, Fehler oder 250 ms Stille sammeln.
- Im ersten Test (alpha.4) wartete µprint nach `V`, `QG` und `QM` jedes Mal 250–300 ms auf ein `OK`, das nie
  kam. In der Plotter-Schleife hätte das jede Tastenabfrage so viel Zeit gekostet.
- `LM`: Rate und Beschleunigung werden alle 40 µs auf einen 31-Bit-Akkumulator addiert.
  Rate ≈ Schritte/s × 2³¹ / 25000. Höchstens 25 000 Schritte/s.
- `LM`-Rate getestet: 1600 Schritte/s → `137438953`, Rampe 0 → 1600 → 0 über 800 Schritte mit Accel `10995`.
  Beide Fahrten liefen sauber, `QS` stand danach wieder auf `0,0`.
- Stift: Klassisches AxiDraw und NextDraw steuern den Servo über **verschiedene Pins und Wertebereiche**
  (siehe saxi-Issue #340). Deshalb braucht es Maschinenprofile. `SP,0`/`SP,1` mit den Firmware-Standardwerten
  funktionieren auf dem Testgerät.

### QG-Statusbyte

| Bit | Bedeutung |
|---|---|
| 7 | FW 2.x: Pin RB5; FW 3.x: Endschalter ausgelöst (wird durch `QG` gelöscht) |
| 6 | FW 2.x: Pin RB2; FW 3.x: Spannung war weg (wird durch `QG` gelöscht) |
| 5 | PRG-Taste seit der letzten `QG`- oder `QB`-Abfrage gedrückt |
| 4 | Stift oben (1) / unten (0) |
| 3–0 | Befehl läuft, Motor 1 bzw. 2 bewegt sich, FIFO nicht leer |

Auf dem Testgerät stehen Bit 6 und 7 immer auf 1 (`D0` = Stift oben, `C0` = Stift unten). Für Zustandsvergleiche
werden sie ausmaskiert (`QG_STATE`).

## Taste zum Pausieren (Stiftwechsel)

- Das EBB hat eine Taste **PRG**, beim AxiDraw die Pause-Taste. Die Firmware reagiert nicht selbst darauf:
  Bewegungen laufen weiter, und jeder Befehl wird weiter mit `OK` bestätigt. Sie merkt sich nur den Druck.
- Der Host muss abfragen. In der AxiDraw-Software pausiert der Rechner: Warteschlange abarbeiten, Stift hoch,
  Position merken. Fortgesetzt wird am Rechner.
- Abfrage über Bit 5 von `QG` (ab FW 2.6.2; `QB` gilt ab FW 3 als veraltet). **Getestet mit FW 2.8.1:** Das Bit
  merkt sich einen Druck bis zur nächsten Abfrage (8 s ohne Abfrage, dann `F0`), die Abfrage löscht es (direkt
  danach wieder `D0`).
- `QG` und `QB` teilen sich den Merker. Im Test lieferte `QB` direkt nach einem `QG` mit gedrückter Taste `0`.
  Deshalb nur `QG` verwenden, nie beide gemischt.
- Solange die Taste gehalten wird, liefert jede Abfrage wieder Bit 5 (im Test bis zu 3,2 s lang `F0`). Ein
  Tastendruck zählt deshalb erst wieder, nachdem einmal ein `QG` ohne Bit 5 kam.
- Die zweite Taste **RST** setzt das Board hart zurück. Die USB-Verbindung bricht ab, µprint sieht
  „Drucker getrennt“.

Umsetzung in µprint: Zwischen den Bewegungsbefehlen die Taste abfragen. Bei einem Druck das aktuelle Segment zu
Ende fahren, Stift hoch, Position merken, „Pausiert“ im Web-UI anzeigen. Fortsetzen per Web-UI **oder erneutem
Tastendruck**: zurück zur gemerkten Position, Stift runter, weiter. Ein Druck zum Fortsetzen zählt erst, wenn
die Taste nach dem Pausieren einmal losgelassen war, sonst fährt ein langer Druck sofort wieder los. Die Motoren
bleiben in der Pause an, damit die Position stimmt. Die bestehende Pause-Logik wird dafür erweitert.

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
  2. Firmware und Status: `V` und Wahl des Antwortformats (ab FW 3 mit `CU,10,1`), dann `QT`, `QC`, `QE`,
     `QG`, `QM`, `QS`, `QB`, ab FW 3 `QU,2`
  3. Stift: `SP,1` / `SP,0` / `SP,1`, jeweils danach `QG`
  4. Optional Bewegung: `XM`-Quadrat (10 mm), `LM` konstant hin und zurück, `LM` mit Rampe hin und zurück,
     danach jeweils `QS` (erwartet 0,0)
  5. PRG-Taste live: 15 s lang `QG` alle 100 ms, protokolliert werden nur Änderungen
  6. PRG-Taste gespeichert? 8 s keine Abfrage, dabei einmal drücken, danach zweimal `QG` (erwartet: erst
     gedrückt, dann gelöscht). `QB` wird hier nicht mehr abgefragt, weil es sich den Merker mit `QG` teilt.
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
3. ~~Hardwaretest beim Bekannten, Log auswerten~~ (erledigt: FW 2.8.1, Log vom 08.10.2026)
4. Protokollschicht für den Plotter-Modus: Antwortformat je Firmware (für den Testmodus erledigt, `ebb_cmd`
   und `ebb_detect` in `main/axidraw.c`). Zweiter Test mit alpha.5 bestätigt das für FW 2.x, FW 3.x bleibt Beta
5. Einstellungen für den Plotter: Maschinenprofil (AxiDraw/NextDraw), Stifthöhen, Geschwindigkeit, Beschleunigung
6. Übersetzer G-Code → `LM`/`S2` mit Vorausschau zum Abbremsen an Ecken
7. Abbrechen über `ES`, danach Stift hoch und Motoren aus
8. Pause über die PRG-Taste: regelmäßig `QG` abfragen (nicht `QB`), Stift hoch, Position merken, Fortsetzen
   per Web-UI oder erneutem Tastendruck (erst nach Loslassen)
9. Temperaturen und andere Druckerfelder im Plotter-Modus ausblenden
10. µplot: AxiDraw-Profil mit Zeichenfläche (A4/A3), Offset 0, Nullpunkt oben links, Y gespiegelt;
    Stift weiter über Z, µprint wertet den Z-Wechsel aus
11. Gesamttest beim Bekannten: SVG → µplot → µprint → AxiDraw, mit Stiftwechsel über die Taste

## Offen

- Zweiter Test mit alpha.5: Kommen alle Antworten ohne Zeitablauf und ohne „Antwort unvollständig“ an?
- Stimmen 80 Schritte/mm? Beim nächsten Test die 10-mm-Fahrt mit dem Lineal nachmessen
- Stift-Timing und Servowerte je Maschine. Der Test nutzt `SP` mit den Werten aus der Firmware; `S2` mit
  eigenen Werten und der NextDraw-Pin kommen später
- FW 3.x komplett ungetestet (Antwortformat nach `CU,10,1`, Bit 6/7 von `QG`, Tasten-Merker). Wir haben nur ein
  Gerät mit FW 2.8.1; wer FW 3.x hat, soll bitte ein Log schicken

## Quellen

- [EBB Command Set, Firmware v3.0+](https://evil-mad.github.io/EggBot/ebb.html)
- [EBB (EiBotBoard) – Hardware](http://www.schmalzhaus.com/EBB/)
- [saxi](https://github.com/nornagon/saxi) (AGPL-3.0), [Issue #340: NextDraw-Stiftservo](https://github.com/alexrudd2/saxi/issues/340)
- [Bantam Tools NextDraw Firmware](https://support.bantamtools.com/hc/en-us/articles/28809123473043-Bantam-Tools-NextDraw-Firmware)
- [AxiDraw V3 – 2032 Schritte/Zoll](https://shop.evilmadscientist.com/productsmenu/846)
- [µplot](https://github.com/dmyrenne/uplot)

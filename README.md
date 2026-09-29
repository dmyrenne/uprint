# μprint

Druckserver für Marlin-3D-Drucker auf einem ESP32-S3. G-Code wird über eine Weboberfläche im WLAN auf eine
microSD-Karte oder in den internen Flash des ESP geladen. Von dort schickt µprint ihn über den USB-Host-Port an den
Drucker. Ein PC muss während des Drucks nicht laufen. Getestet mit einem Prusa MK3S, gedacht für jeden Drucker
mit Marlin- oder Prusa-Firmware und USB-Anschluss.

Die Oberfläche passt zu [µplot](https://github.com/dmyrenne/uplot) und [µgen](https://github.com/dmyrenne/ugen) und
druckt deren G-Code genauso wie den eines Slicers.

## Installation

### Das brauchst du

| Teil | Hinweis |
| --- | --- |
| ESP32-S3-Board | mit **16 MB Flash und 8 MB Octal-PSRAM** (Modul N16R8). Andere Varianten starten mit dem fertigen Image nicht. |
| USB-Adapter | USB-C- bzw. Micro-USB-Stecker auf USB-A-Buchse (OTG) für den nativen USB-Port des ESP |
| Netzteil 5 V | für den ESP, getrennt vom Drucker |
| microSD-Modul (SPI) | optional, mit einer FAT32-formatierten Karte |

### Firmware installieren (einmalig)

**Mit dem Web-Flasher (empfohlen):** [dmyrenne.github.io/uprint](https://dmyrenne.github.io/uprint/) in Chrome, Edge
oder Opera am Computer öffnen, das Board per USB anschließen und auf „Installieren“ klicken. Das Flashen läuft über
WebSerial direkt im Browser, ohne weitere Software.

**Mit esptool:** die Datei `uprint-<version>-factory.bin` von den [Releases](https://github.com/dmyrenne/uprint/releases)
laden und ab Adresse 0 schreiben:

    pip install esptool
    esptool --chip esp32s3 erase-flash
    esptool --chip esp32s3 write-flash 0x0 uprint-<version>-factory.bin

Hinweise für beide Wege:

- Hat das Board zwei USB-Buchsen, die zum Programmieren nehmen, meist mit `UART` oder `COM` beschriftet.
- Findet der Computer das Board nicht: `BOOT` gedrückt halten, kurz `RESET` (bzw. `RST`/`EN`) drücken, `BOOT`
  loslassen. Nach dem Flashen einmal `RESET` drücken.
- Das Löschen bei der Erstinstallation entfernt alte Daten. Bei späteren Updates bleiben Einstellungen, WLAN-Daten
  und Dateien erhalten.

### Einrichten

1. Nach dem Start öffnet µprint den WLAN-Access-Point **uprint**, Passwort `uprint123`.
2. Mit diesem WLAN verbinden und http://192.168.4.1 öffnen.
3. Unter **04 WLAN** das eigene Netz wählen, Passwort eingeben, **Verbinden**. Die Seite zeigt danach die neue
   IP-Adresse an. Der Access Point schließt sich nach etwa 2 Minuten, µprint ist dann unter http://uprint.local
   erreichbar.
4. Den Drucker per USB an den USB-Host-Port anschließen (siehe [USB-Anschluss](#usb-anschluss)). Sobald er
   verbunden ist, wird der Status-Chip oben rechts blau.

Findet µprint das eingerichtete WLAN 30 s lang nicht, etwa nach einem Routerwechsel, öffnet es den Access Point
erneut. Hostname, Access-Point-Passwort und alle anderen Werte lassen sich unter **05 Einstellungen** ändern.

### Aktualisieren

Updates brauchen kein Kabel: `uprint-<version>-ota.bin` von den
[Releases](https://github.com/dmyrenne/uprint/releases) laden und unter **05 Einstellungen → Firmware** installieren.
µprint schreibt die Datei in die freie App-Partition, prüft sie und startet neu. Angenommen wird nur Firmware mit
dem Projektnamen `uprint`. Während eines Drucks ist kein Update möglich.

## Hardware

### USB-Anschluss

- Der Drucker hängt am **nativen USB-Port** des ESP32-S3 (GPIO19/20), meist mit `USB` oder `OTG` beschriftet.
  Dieser Port arbeitet als Host. Die Logausgabe läuft deshalb über den UART-Port bzw. GPIO43/44.
- Ein USB-Host muss 5 V auf VBUS liefern. Viele Boards tun das am nativen Port nicht von sich aus: Bei manchen ist eine
  Lötbrücke zu schließen (oft mit `USB-OTG` oder `IN-OUT` beschriftet), bei anderen muss VBUS extern eingespeist werden.
  Ohne 5 V erkennen viele Drucker die Verbindung nicht.
- Manche Drucker versorgen einen Teil ihrer Elektronik über USB mit, der Prusa MK3S zum Beispiel sein Display. Das
  Netzteil des ESP muss diesen Strom liefern können.
- Erkannt werden USB-CDC-Geräte (z. B. Prusa MK3S mit ATmega32U2, Boards mit nativem USB wie STM32 oder LPC) und
  USB-Seriell-Wandler mit CH340/CH341, CP210x oder FTDI.

### microSD (optional)

Standardbelegung für ein SPI-Modul, änderbar unter **05 Einstellungen → microSD**:

| SD | ESP32-S3 |
| --- | --- |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| SCLK | GPIO12 |
| CS | GPIO10 |
| VCC / GND | je nach Modul 5 V oder 3,3 V / GND |

Module mit eigenem Spannungsregler und Pegelwandler gehören an 5 V, reine Breakout-Platinen an 3,3 V. Die Karte muss
FAT32-formatiert sein. Karten über 32 GB sind ab Werk meist exFAT und müssen umformatiert werden. Ohne SD-Karte nutzt
µprint nur den internen Speicher.

### Speicheraufteilung (16 MB Flash, `partitions.csv`)

| Partition | Größe | Zweck |
| --- | --- | --- |
| nvs | 24 KB | Einstellungen und WLAN-Zugangsdaten |
| ota_0 / ota_1 | je 2 MB | Firmware, abwechselnd beschrieben bei Updates |
| storage | ~11,9 MB | Dateispeicher für G-Code (FAT mit Wear-Levelling), wird beim ersten Start formatiert |

## Bedienung

- **01 Druck:** zeigt die ausgewählte Datei bzw. den laufenden Druck mit Fortschritt und Restzeit. Buttons
  **Drucken**, **Pausieren**/**Fortsetzen** und **Abbrechen**.
- **02 Hochladen:** G-Code hineinziehen oder auswählen, Speicherort wählen. Reicht der Platz nicht, ist der
  Upload-Button gesperrt.
- **03 Dateien:** Datei anklicken, um sie auszuwählen. Download und Löschen direkt in der Zeile.
- **04 WLAN** und **05 Einstellungen:** eingeklappt, siehe oben und unten.
- Oben: Fortschritt, Laufzeit, Restzeit, Temperaturen von Düse und Bett, Sprache (Deutsch/Englisch) und der
  Druckerstatus.

### Pausieren

µprint hält den Datenstrom an und hebt die Düse an (Standard 5 mm, 0 = aus). Beim Fortsetzen senkt es die Düse auf
die alte Höhe und stellt den vorherigen Vorschub wieder her. Dafür verfolgt µprint Z-Position und Vorschub im gesendeten
G-Code mit. So kommt es ohne `G91`/`G90` aus, das bei Marlin einen relativen Extruder (`M83`, z. B. bei PrusaSlicer)
zurücksetzen würde. Ist Z unbekannt, etwa direkt nach `G28`, pausiert µprint ohne Anheben. Die Heizungen bleiben an.

### Abbrechen

Heizungen und Lüfter gehen immer aus. Danach:

- **Dateien aus µplot:** µprint führt den Block `; -- shutdown` vom Ende der Datei aus: Stift auf die Sicherheitshöhe,
  dann zur Parkposition, Motoren aus. Damit gelten die Werte aus dem µplot-Profil.
- **Alle anderen Dateien:** Düse anheben (Standard 10 mm), dann zur Parkposition (Standard X0 Y200, beim Prusa MK3S
  fährt damit das Bett nach vorn), Motoren aus.

### Einstellungen

Alle Einstellungen liegen im NVS des ESP und überstehen Firmware-Updates.

| Einstellung | Standard | wirksam |
| --- | --- | --- |
| Baudrate | 115200 | ab der nächsten Verbindung mit dem Drucker |
| Anheben beim Pausieren | 5 mm | sofort |
| Anheben beim Abbrechen, Parkposition | 10 mm, X0 Y200 | sofort |
| Hostname, Access-Point-Passwort | `uprint`, `uprint123` | nach Neustart |
| SD-Pins MOSI/MISO/SCLK/CS | 11/13/12/10 | nach Neustart |

## Selbst bauen

Voraussetzung ist [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v6.1
(ab v5.3 sollte es ebenfalls gehen). Die Komponenten für USB-Host und mDNS lädt der IDF Component Manager beim ersten
Build herunter.

    git clone https://github.com/dmyrenne/uprint.git
    cd uprint
    idf.py set-target esp32s3
    idf.py build
    idf.py -p <port> flash monitor      # z. B. /dev/ttyUSB0, /dev/cu.usbserial-…, COM3

`build/uprint.bin` ist das Image für Updates über die Weboberfläche. `idf.py merge-bin -o uprint-factory.bin` erzeugt
das Gesamt-Image für die Erstinstallation (`build/uprint-factory.bin`, ab Adresse 0). Die Firmware-Version ermittelt
ESP-IDF mit `git describe`.

Für Boards mit anderem Flash oder ohne Octal-PSRAM müssen `partitions.csv` und `sdkconfig.defaults` angepasst werden.

### Firmware-Builds und Releases

Der Workflow `.github/workflows/firmware.yml` baut die Firmware mit ESP-IDF im offiziellen Docker-Image:

- Push auf `main` und Pull Requests: bauen, die `.bin`-Dateien hängen als Artefakt am Workflow-Lauf.
- Tag `v1.2.3` (`git tag v1.2.3 && git push --tags`): zusätzlich ein GitHub-Release mit
  `uprint-1.2.3-factory.bin` und `uprint-1.2.3-ota.bin`. Der Web-Flasher auf GitHub Pages wird auf diese Version
  aktualisiert.

Einmalig im Repository einzurichten:

1. **Settings → Pages → Build and deployment → Source:** „GitHub Actions“.
2. **Settings → Environments → github-pages → Deployment branches and tags:** eine Regel für Tags `v*` hinzufügen.
   Ohne sie verweigert GitHub die Veröffentlichung von einem Tag aus.

Die Seite des Web-Flashers liegt in `flasher/index.html`. Der Workflow ergänzt Manifest, Schriften und Image.

## Funktionsweise

µprint sendet G-Code nach dem Marlin-Host-Protokoll, wie OctoPrint:

1. Nach dem Verbinden wartet es auf `start`. Die meisten Boards starten neu, sobald der Port geöffnet wird. Danach folgen
   `M110 N0` und `M155 S2` (Temperaturmeldungen alle 2 s).
2. Jede Zeile geht als `N<n> <befehl>*<prüfsumme>` an den Drucker. Die nächste folgt erst nach dessen `ok`.
3. Auf `Resend: <n>` sendet es ab Zeile n erneut (Verlauf von 32 Zeilen).
4. Kommentare und Leerzeilen entfernt es vorher.

## Einschränkungen

- Keine Anmeldung für die Weboberfläche: nur in vertrauenswürdigen Netzen betreiben.
- Ein Abbruch während `M109`/`M190` (Aufheizen) greift erst, wenn der Drucker die Zieltemperatur erreicht hat.
- Kein Fortsetzen nach Stromausfall.
- Der Webserver bearbeitet eine Anfrage nach der anderen. Während eines großen Uploads, Downloads oder Updates
  aktualisiert sich die Statusanzeige erst danach. Der Druck selbst läuft ungestört weiter.
- Kein automatisches Zurückschalten auf die alte Firmware, falls eine neue nicht startet. Dann hilft die
  Erstinstallation über USB.
- Drucker mit Klipper werden nicht unterstützt, deren Mainboard nimmt keinen G-Code über USB an.

## Lizenzen

Die Schriften Space Grotesk und JetBrains Mono stehen unter der SIL Open Font License (`main/www/fonts/OFL-*.txt`).

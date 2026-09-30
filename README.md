# μprint

μprint macht aus einem ESP32-S3 einen kleinen Druckserver für deinen 3D-Drucker. Du lädst G-Code im Browser hoch,
μprint speichert ihn auf einer microSD-Karte oder im internen Speicher und schickt ihn über USB an den Drucker.
Starten, Pausieren und Abbrechen erledigst du ebenfalls im Browser, am Rechner oder am Handy. Ein PC muss dafür nicht
laufen. μprint funktioniert mit Druckern mit Marlin- oder Prusa-Firmware und USB-Anschluss, zum Beispiel dem Prusa MK3S.

![μprint während eines Drucks](docs/screenshot.png)

## Was du brauchst

- Ein **ESP32-S3-Board** mit mindestens 4 MB Flash.
- Einen **USB-OTG-Adapter** von der USB-Buchse des Boards auf USB-A, damit du das Druckerkabel anstecken kannst.
- Ein **5-V-Netzteil** für das Board.
- Ein **microSD-Modul** mit FAT32-formatierter Karte für deine Dateien.

Für die meisten Boards gibt es die Firmware **Basic**, Dateien liegen dann auf der microSD-Karte. Hast du ein Board
mit der Bezeichnung **N16R8** (16 MB Flash, 8 MB PSRAM), nimm die gleichnamige Variante: Sie bietet zusätzlich
knapp 12 MB internen Speicher, dort ist die SD-Karte optional.

Das SD-Modul kommt standardmäßig an diese Pins, andere lassen sich später in den Einstellungen wählen:

| SD-Modul | MOSI | MISO | SCLK | CS |
| --- | --- | --- | --- | --- |
| ESP32-S3 | GPIO11 | GPIO13 | GPIO12 | GPIO10 |

Die meisten Boards haben zwei USB-Buchsen. Die eine (meist `UART` oder `COM`) nutzt du zum Aufspielen der Firmware,
an die andere (meist `USB` oder `OTG`) kommt später der Drucker. Damit der Drucker dort erkannt wird, muss die Buchse
5 V liefern. Bei vielen Boards ist dafür eine kleine Lötbrücke zu schließen, oft mit `USB-OTG` beschriftet.

## Firmware aufspielen

Am einfachsten geht das mit dem Web-Flasher direkt im Browser, ganz ohne Software-Installation:

1. Öffne [dmyrenne.github.io/uprint](https://dmyrenne.github.io/uprint/) in **Chrome oder Edge** am Computer.
   Firefox und Safari unterstützen das nötige WebSerial leider nicht.
2. Schließe das Board mit einem USB-Datenkabel an die `UART`- bzw. `COM`-Buchse an.
3. Wähle dein Board (**Basic** oder **N16R8**), klicke auf **Installieren**, wähle im Dialog den seriellen Port des
   Boards und bestätige beim ersten Mal das Löschen des Geräts. Nach etwa einer Minute ist die Firmware drauf.

Findet der Browser das Board nicht, halte die Taste `BOOT` gedrückt, drücke kurz `RESET` und lass `BOOT` wieder los.
Danach klappt die Verbindung. Nach dem Aufspielen einmal `RESET` drücken.

## Einrichten

Nach dem Start öffnet μprint ein eigenes WLAN namens **uprint** (Passwort `uprint123`). Verbinde dich damit und öffne
http://192.168.4.1. Unter **WLAN** wählst du dein Heimnetz aus und gibst das Passwort ein. Die Seite zeigt dir danach
die neue Adresse an, und ab dann erreichst du μprint in deinem Netz unter http://uprint.local.

Jetzt noch den Drucker per USB an das Board anstecken. Sobald er verbunden ist, wird der Status oben rechts blau.
Datei hochladen, in der Liste anklicken, auf **Drucken** – fertig.

Unter **Einstellungen** findest du alles Weitere: einen eigenen Namen für das Gerät (praktisch, wenn du mehrere
hast), wie weit die Düse beim Pausieren und Abbrechen angehoben wird,
wohin der Kopf danach fährt, Hostname, WLAN-Passwort des Access Points und die Pins der SD-Karte.

## Aus dem Slicer drucken

μprint versteht die Schnittstellen von PrusaLink und OctoPrint. Damit schickt dein Slicer den G-Code direkt an den
Drucker. In **PrusaSlicer** legst du dazu einen physischen Drucker an: Host-Typ **PrusaLink**, Hostname
`uprint.local` und als API-Schlüssel den Wert aus μprint unter **Einstellungen → Slicer**. Über „Durchsuchen“ findet
PrusaSlicer μprint auch selbst im Netz. **OrcaSlicer** und andere Slicer nutzen den Host-Typ **OctoPrint** mit
denselben Angaben. Beim Hochladen kannst du wählen, ob der Druck gleich startet.

Binären G-Code (`.bgcode`) kann μprint nicht drucken. Falls dein Druckerprofil ihn nutzt, schalte ihn in den
Druckereinstellungen des Slicers ab.

## Aktualisieren

Neue Versionen erscheinen unter [Releases](https://github.com/dmyrenne/uprint/releases). Zum Aktualisieren brauchst du
kein Kabel: Lade die Datei `uprint-…-ota.bin` für deine Variante herunter (`basic` oder `n16r8` im Namen) und
installiere sie in μprint unter **Einstellungen → Firmware**. Welche Variante installiert ist, steht dort ebenfalls. μprint startet danach neu, deine Einstellungen, das WLAN und alle Dateien bleiben
erhalten. Alternativ kannst du auch einfach den Web-Flasher noch einmal benutzen, dann aber ohne das Gerät zu löschen.

Im Web-Flasher lässt sich auch eine ältere Version wählen, falls du zurückgehen möchtest. Vorabversionen zum Testen
neuer Funktionen erscheinen als *Pre-release* und stehen dort unter **Alpha**. Für den normalen Betrieb ist
**Release** gedacht.

## Selbst bauen

Wer die Firmware selbst kompilieren möchte, braucht [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
v6.1. Die übrigen Abhängigkeiten lädt der Build automatisch:

    git clone https://github.com/dmyrenne/uprint.git
    cd uprint
    idf.py -DUPRINT_VARIANT=basic -B build-basic build          # oder n16r8
    idf.py -DUPRINT_VARIANT=basic -B build-basic -p <port> flash

`build-basic/uprint.bin` lässt sich anschließend auch über **Einstellungen → Firmware** einspielen. Jeder Tag `v*` im Repo
baut über GitHub Actions automatisch ein Release und aktualisiert den Web-Flasher.

## Lizenz

μprint steht unter der [MIT-Lizenz](LICENSE).

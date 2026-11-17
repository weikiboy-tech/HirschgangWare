# Spielerleben (HP 0–100)

Der aktuelle DMA-Datenpfad liefert für die getestete Multiplayer-Sitzung
keinen verifizierten numerischen Lebenswert. Der Schalter aktiviert die
Anzeige. Ohne eine Quelle bleibt sie leer. Das Todesflag enthält keinen
Prozentwert. Bohemias
[DayZ-HUD](https://github.com/BohemiaInteractive/DayZ-Script-Diff/blob/main/scripts/5_mission/gui/ingamehud.c)
fragt exakte HP in diesem Codepfad nur im Einzelspieler direkt ab. Das
[synchronisierte `m_HealthLevel`](https://github.com/BohemiaInteractive/DayZ-Script-Diff/blob/main/scripts/4_world/entities/manbase/playerbase.c)
ist eine Zustandsstufe.

[Mambos `Memory.cs`](https://github.com/Mambo-Noob/MamboDMA/blob/main/Memory.cs)
stellt allgemeine VMM-Leseoperationen bereit.
[`GetHealth` im verglichenen DayZ-DMA-Projekt](https://github.com/KoreanLandGiant/DayZ-DMA/blob/main/SDK.h#L2166)
liest das Feld `OFF_Quality`, das dort auch für Zustandsstufen verwendet
wird. Der Name der Funktion liefert daher keinen Beleg für exakte HP.

Die Diagnose in `tools/dayz-health-probe.cpp` verwendet denselben
DMA-Backendpfad und dieselbe RTTI-Klasse `DayZPlayer` wie das Radar.
Am 17. November 2026 waren in der laufenden Deathmatch-Sitzung 16 lebende
Spielerobjekte lesbar. Das geprüfte Qualitätsfeld bei `+0x194` war bei
allen 16 gleich 0. Der Verweis auf den eigenen Spieler war für die
separate Probe nicht auflösbar. Es gab keinen kontrollierten HP-Wechsel.
Ein Treffer bei zwei bekannten HP-Werten
wäre ein Kandidat für einen dritten Messpunkt und einen separaten Test
an einem anderen Spieler. Der Build der Probe allein bestätigt kein Feld.

Die Anzeige kann mit einem autorisierten Serverfeed betrieben werden. Ein
Serverexport muss den echten aktuellen Wert und das Maximum für den
Gesundheitskanal ermitteln, etwa über `GetHealth("", "Health")` und
`GetMaxHealth("", "Health")`. Der Export muss die Peer-ID mit der im
Radar aus Entität und Spielerliste gelesenen ID abgleichen. Diese Zuordnung
ist vor Einsatz auf einem jeweiligen Server einmal live zu prüfen.
[Bohemias Beispiel für den Gesundheitskanal](https://github.com/BohemiaInteractive/DayZ-Script-Diff/blob/main/scripts/4_world/entities/manbase/playerbase.c)

Der Feed-Adapter liest `player-health.tsv` neben der EXE. Der Produzent
soll die Datei etwa zweimal pro Sekunde atomar ersetzen. Die Kopfzeile
enthält Unixzeit in Millisekunden, Spielkarten-ID und den exakten
Servernamen aus dem HirschgangWare-Launcher. Jede folgende Zeile enthält
Peer-ID, X, Z, aktuelle HP und maximale HP mit Dezimalpunkt. Beispiel für
das Format mit synthetischen Werten:

```text
HIRSCHGANG-HP-1 1794900000000 chernarusplus Private Test
42 500 1000 37.5 50
```

Das ergibt `HP 75`. Scans verwerfen einen Feed nach 2,5 Sekunden, bei
falschem Server oder Karte, doppelten Peer-IDs und ungültigen Werten. Ein
Marker muss außerdem innerhalb von 50 Metern zur Feed-Position liegen.
Die Datei wird nicht über den Webserver bereitgestellt.

## Privater Deathmatch-Testserver

1. Der Serverbetreiber legt `tools/dayz-health-export.c` neben die
   bestehende Missionsdatei `init.c`. Diese bindet die Datei mit
   `#include "dayz-health-export.c"` ein und ruft
   `HirschgangStartHealthExport();` einmal aus der vorhandenen
   `main()` auf. Der Export enthält nur Peer-ID, Position und HP.
2. Der Server erzeugt nach dem Missionsstart zweimal pro Sekunde
   `HirschgangHealth.tsv` im Profilverzeichnis. Der Radar-PC braucht
   lesenden Zugriff auf diese Datei. Beide Rechner brauchen synchronisierte
   Uhren, und bei einer Dateispiegelung muss die Änderungszeit erhalten bleiben.
3. Starte genau eine Bridge mit dem Quellpfad, dem Ordner der laufenden
   HirschgangWare-EXE und dem exakten Servernamen aus dem Launcher.

```powershell
node tools/bridge-player-health.mjs "Z:\DayZProfile\HirschgangHealth.tsv" "build\health-test" "Private Deathmatch"
```

Die Bridge ersetzt den Radar-Feed nur bei vollständigen, frischen Snapshots.
Bei einer Unterbrechung blenden die Anzeigen die Werte nach spätestens 2,5 s
ab Erfassungszeit aus. Zum Abgleich im privaten Test zuerst genau einen
bekannten Spieler mit vollen HP prüfen. Dann serverseitig für denselben
Spieler kontrolliert den Wert ändern und die drei Anzeigen vergleichen.
Bis der Peer-ID-Abgleich auf dem jeweiligen Server bestätigt ist, bleibt
die Webfreigabe ausgeschaltet.

- **ESP Overlay → Health (0–100)** zeigt HP am Spieler im Overlay.
- **Players & World → Health (0–100)** schaltet die native Radaranzeige.
- **Map → Webmap → Player health** gibt die HP für Browser frei. Dort
  schaltet **Ebenen → Leben auf Karte (0–100)** die eigene Anzeige.

Ein fehlender Wert bleibt ausgeblendet. Testdaten werden nur in der
Testumgebung verwendet.

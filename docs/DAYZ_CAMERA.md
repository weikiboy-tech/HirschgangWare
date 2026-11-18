# DayZ-Kamera, Optik und Fuser

Das Overlay liest den Zeiger bei `World + 0x1B0`. Die Kameradaten enthalten die
Basis und Position ab `+0x08` sowie die Halbwinkel-Tangenten für X bei
`+0xD0` und Y bei `+0xE0`. Der DMA-Kamerathread prüft den Zeiger jetzt in
derselben Scatter-Anfrage wie die Kameradaten und übernimmt einen gewechselten
Zeiger im nächsten Durchlauf. Er veröffentlicht Pose, Projektion, Welt und
Zeitstempel gemeinsam. Der Renderpfad verwendet diesen Snapshot; er führt
dafür keine DMA-Lesevorgänge aus. Eine Scatter-Anfrage ist kein atomarer
Spiel-Renderframe; die Zeitmessung erfasst die Erfassung am Zweit-PC.

Die frühere Prüfung verwarf Werte unter oder gleich 0,05 und ersetzte beide
Achsen durch 90° horizontal bei 16:9. Damit konnte ein engeres gültiges
Sichtfeld schlagartig als Weitwinkel erscheinen. Der frühere Viewport wurde
außerdem je nach Verhältnis der Tangenten neu eingepasst. Jetzt bleiben
Projektionsparameter und Ausgabegröße getrennt: normierte Bildkoordinaten
werden auf den Clientbereich der Overlay-Ausgabe abgebildet. Das setzt für
den Fuser zwei vollständig überlagerte Bilder voraus. Getrenntes Cropping,
Letterboxing oder eine abweichende Optik-Teilansicht sind nicht ermittelt.

Ungültige Projektion, bekannte Kamerawechsel und Samples ab 35 ms werden
nicht als gültige ESP-Positionen ausgegeben. Bei Änderungen der Projektion
beginnt die Bewegungsvorhersage für die Kamera neu. Für Radarpositionen ist
keine gültige Projektion nötig. Die Kameraauswertung nutzt keine Eingaben.
Bohemias [Optik-API](https://github.com/BohemiaInteractive/DayZ-Script-Diff/blob/main/scripts/4_world/entities/core/inherited/itemoptics.c)
führt Zoomstufe und Zeroing getrennt. Der [Optikkamera-Code](https://github.com/BohemiaInteractive/DayZ-Script-Diff/blob/main/scripts/4_world/entities/manbase/dayzplayer/dayzplayercameraironsights.c)
berechnet einen momentanen Kamera-FOV aus der Zoomstufe und setzt für
vergrößernde Optiken ggf. separate Masken- und Linsenparameter für die
Nachbearbeitung. Der veröffentlichte Spielcode belegt weder die nativen
DMA-Adressen noch, dass der sichtbare Ausschnitt eines Server-Mods allein
durch diese Kamera beschrieben wird.

„Target under crosshair“ zeigt den nächstgelegenen zulässigen projizierten
Knochen im eingestellten Radius. Die Anzeige funktioniert ohne MAKCU und
bei deaktivierter Klickausgabe. Die Live-Anzeige und der aktive Trigger
fordern Knochen auch bei ausgeschalteter Skelettzeichnung an. Die Anzeige
ist keine Aussage über Sichtlinien, Wanddeckung oder ballistische Treffer.

## Prüfen

`tests\build_dayz_camera_test.bat` prüft mit MSVC künstliche Kamerasamples
für Eintritt, Rückkehr und Zoomstufen, darunter gültige Werte unter dem
früheren Grenzwert, wechselnde Auflösung, gedrehte Kamera, ungültige
Samples, Wechsel des Kameraobjekts und Trefferanzeige ohne Eingabegerät.
`build.bat x64` erstellt den Testbuild. In älteren Spiel-Logs wechselte
der horizontale Tangent bei derselben Kameraadresse. Im zuletzt laufenden
Build (`webmap-feed-fix`, 10:51:35–10:51:41) lag die Spanne in mehreren
aufeinanderfolgenden Messfenstern bei 0,150 bis 0,698, bei höchstens 0,3°
Orientierungsänderung pro Kamerasample. Der alte 0,05-Grenzwert erklärt
diese Messung nicht: Die Kamera meldet bereits deutlich unterschiedliche
Projektionen. Aus den Sekundenstatistiken ohne Bildbezug lässt sich nicht ableiten,
welche der gelesenen Projektionen im jeweiligen sichtbaren Frame verwendet
wurde oder ob eine zweite Projektionsfläche beteiligt war.

Für die Hardwareabnahme: Launcher starten, selbst **Start** drücken. Die
Klickausgabe deaktiviert lassen und das MAKCU-Modul für die Live-Anzeige
öffnen. Auf dem Fuser-Monitor eine Aufnahme mit dem gleichen stationären
Testspieler und eingeblendeten Skelettpunkten machen. Für einen
Geometrie-Vergleich die sichtbare Stelle am Kopf und am Rumpf jeweils auch
außermittig aufnehmen, damit eine verkehrte Vergrößerung auffällt:

1. Normale Ansicht, Kimme/Korn und Mosin als bisher funktionierende Kontrolle.
2. Dieselbe Folge mit einer bisher fehlerhaften Optik. Jede Stufe einzeln
   durchgehen, zweimal hoch und runter zoomen, bewegen und wieder aussteigen.
3. Nur die Einschussentfernung ändern, dann außerhalb der Optik scrollen.
4. Mit einer zweiten Auflösung wiederholen, sofern beide Fuser-Eingänge
   denselben sichtbaren Bildbereich abdecken.

Erwartet: Der projizierte Punkt liegt bei ruhendem Bild bis auf die
Ableseungenauigkeit am gewählten Referenzpunkt (Ziel: höchstens 3 px bei
1080p). Mehr als ein Aufnahmeframe systematischer Versatz während eines
Wechsels ist ein Befund. Die Anzeige wechselt zu „yes“, wenn ein
freigegebener Knochen unter dem Fadenkreuz liegt, und wieder zu „no“,
sobald er den Radius verlässt. Bei Zeroing ist keine Änderung der
Vergrößerung zu erwarten, solange das Spiel selbst den Bildausschnitt
nicht ändert. In den `[cam]`-Zeilen stehen beide FOV-Tangenten, Wechsel
und Lesefehler; `render` enthält Projektionsaussetzer und maximales
Samplealter. Für einen verbleibenden Optikfehler werden Waffen- und
Optikname, Zoomstufe, ein kurzer Clip und der entsprechende Logabschnitt
benötigt. Bleibt die Kamera bei sichtbarer Vergrößerung unverändert
oder passt der Bildrand bei korrekter Mitte nicht, muss die zusätzliche
Optikprojektion beziehungsweise Nachbearbeitung separat identifiziert
werden. Ohne diese Daten bleibt die Zielfernrohrdarstellung am Fuser
unverifiziert.

### Rumpfbreite (2026-11-18)

Die aktuelle Zielauswertung verwendet projizierte Körperregionen statt nur
Knochenpunkte. Brust und Bauch verwenden jetzt einen Kapselradius von 35 %
statt 30 % des gemessenen Schulterabstands (rund 17 % breiter). Kopf, Hals,
Arme, Beine und die zusätzliche Pixeltoleranz bleiben unverändert. Das ist
eine anatomische Näherung, keine ausgelesene Engine-Kollisionsgeometrie.
Der Test prüft beide Seitenränder ohne Pixeltoleranz bei drei Zoomstufen
und drei Auflösungen sowie den unveränderten Kopfbereich. Der reale Rand
muss mit sichtbaren Rumpfregionen im Spiel geprüft werden; ein Monitorfoto
allein liefert dafür keine präzise geometrische Messung.

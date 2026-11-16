# MamboDMA und der PCIe-Eingabepfad

Stand 16. November 2026. Die Messung verwendet die vorhandene FPGA-Karte, die
mitgelieferten MemProcFS-DLLs und den laufenden Haupt-PC. Der Prüfmodus arbeitet lesend. Der Produktionspfad teilt sich inzwischen den Resolver mit ihm.

## Vergleich

| Teil | MamboDMA | Konsequenz für HirschgangWare |
| --- | --- | --- |
| VMM | `Memory.cs` initialisiert eine VmmSharpEx-Sitzung und reicht PID, VA, Flags und Byteanzahl über einen Adapter durch. | Dieselbe Art Speicherzugriff steht mit der vorhandenen C-API bereit. Ein Wechsel auf C# oder VmmSharpEx löst das Adress- oder Herkunftsproblem nicht. |
| Adresse | `InputManager.cs` liest den Windows-Build, sucht in einem Kernel-View von `csrss` nach dem globalen Session-Slot und entnimmt aus Code in `win32kbase.sys` einen Offset. Es liest den ersten plausibel wirkenden Slot und liest die berechnete Adresse anschließend in einem per Name gefundenen `winlogon`. | Der Codepfad ist eine sinnvollere Hypothese als die frühere Daten-Differenzsuche oder ein festes RVA. Der erste Signaturtreffer, ein Kernel-ähnlicher Zeiger und ein vollständiger Read sagen noch nichts über Session, Inhalt oder Herkunft aus. Mambos Adapter rät bei fehlendem PE-Header die Bildgröße und prüft keine Treffer-Eindeutigkeit. |
| Eingabe | Der Worker pollt ein 64-Byte-VK-Bitmap und erkennt Flanken zwischen zwei Samples. Bei Maus-VKs mischt er MAKCU-Zustand ein. `IsReady` setzt ein erfolgreicher Erstread, und Lesefehler im Worker löschen die Bereitschaft nicht. | Das Bitmap enthält VK-Zustände ohne Geräte-ID. Zwei Tastaturen in der Zielsession können dort dieselbe E-Flanke erzeugen. Ein Tap zwischen zwei Polls kann fehlen. Ein Trigger braucht Provenienz und einen sicheren Zustand bei Ausfall. |
| Frühere HirschgangWare-Diagnosen | – | Das in `INPUT_RESEARCH.md` §12 zunächst als bestätigt dokumentierte RVA ist durch die späteren Leerlauf- und Mauskontrollen in `INPUT_DEBUG_HISTORY.md` §5.10 widerlegt. `INPUT_DIAGNOSIS.md` dokumentiert die erwartete 2-Bit-Geometrie und den separat behobenen VK-Anzeigefehler. |

[InputManager.cs, Blob 3a50cb1](https://github.com/Mambo-Noob/MamboDMA/blob/main/Input/InputManager.cs)
· [Memory.cs, Blob f6cc3d6](https://github.com/Mambo-Noob/MamboDMA/blob/main/Memory.cs)
· [memflow-win32-Referenz](https://github.com/memflow/memflow-win32/blob/main/memflow-win32/src/win32/keyboard.rs)
· [Windows Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input)

## Reale Messung

- Das Ziel meldete diesmal **Windows 11 Build 26200.9711**. Die älteren
  Berichte betreffen Build 26100. Die aktuelle Messung bestätigt 26100 nicht.
- `win32ksgd.sys` war im Kernel-View der ausgewählten interaktiven Session
  nicht auflösbar. In `win32k.sys` ergab Mambos erstes Muster neun Treffer
  mit identischem RIP-Ziel. Das zweite Muster und das engere memflow-Muster
  trafen dort je einmal und adressierten denselben Zeiger.
- Mambos Offset-Muster und das erste memflow-Offset-Muster lieferten je
  einen Treffer mit **0x3BFC**. Das zweite memflow-Offset-Muster hatte keinen
  Treffer. Der alte Export `gafAsyncKeyState` ließ sich nicht auflösen.
  Der PDB-Fallback wurde in diesem Lauf nicht geprüft.
- Der Slot-0-Kandidat war in `winlogon | KERNELMEMORY` vollständig als 64
  Bytes lesbar. Die erste 8-Sekunden-Probe beobachtete null Tastatur-VK-Flanken
  und acht Maus-VK-Flanken ohne kontrollierten Mausstimulus. Die
  Tastatur-Positivprobe und die kontrollierten Quellen-Negativproben wurden
  mangels Bedienung am Haupt-PC nicht ausgeführt. Die beobachteten Maus-Bits
  allein belegen keine Zuordnung zu einer realen Mausaktion.

## Entscheidung und reproduzierbarer Test

Die erste Prüfung oben war eine Strukturdiagnose. Der spätere kontrollierte Zwei-PC-Lauf bestätigte den Kandidaten für E und W. Für die geforderte
Unterscheidung physischer Geräte reicht selbst ein korrektes Bitmap nicht aus.
Ein geeigneter PCIe-Transport muss ein Ereignis mit Quellgerät und Flanke
erhalten. Das Quellgerät muss auf der Erfassungsseite bestimmt werden, solange
der Gerätekontext noch vorhanden ist. Der FPGA-Lesepfad selbst liefert diese Information nicht.

Der vorhandene MAKCU ist eine fertige Alternative für eine andere Verkabelung.
Die V4-API trennt Tastatur und Maus im typisierten `INPUT_CHANGE`-Frame. Die
Tastatur muss dafür an die Box geroutet werden und die Ereignisse kommen am
seriellen USB-Port an. Die installierte Firmware und der Tastaturstream wurden
hier nicht geprüft. [MAKCU-Protokoll](https://makcu.com/api/versions)
· [Verkabelung](https://makcu.com/en/setup)

Auf dem Zweit-PC andere DMA-Clients schließen. Im Repository
`tools\build_keyboard_probe.bat` ausführen und dann aus PowerShell starten:

```powershell
.\build\keyboard_probe.exe (Resolve-Path .\dma\vmm.dll).Path 1 --verify
```

Das Argument `1` bezeichnet die interaktive Windows-Session auf dem Haupt-PC.
Ohne `--verify` endet der Lauf nach der Strukturprüfung. Der interaktive Lauf
führt nacheinander durch Ruhe, zweimal E und zweimal W an der Haupt-PC-Tastatur,
Mausaktivität am Haupt-PC und zweimal E am Zweit-PC. Bei beiden positiven
Phasen werden genau zwei steigende und zwei fallende Flanken erwartet. Maus und
Zweit-PC dürfen keine Tastaturflanke im Zielbitmap auslösen. Die letzte Phase
zählt die E-Flanken am Zweit-PC separat, damit ein ausgelassener Kontrollreiz
nicht als Erfolg zählt. Falls eine zweite Tastatur am Haupt-PC verfügbar ist,
kann deren gleiche E-Flanke durch Wiederholung der E-Phase geprüft werden.
Auch ein bestandener Lauf belegt weder Geräteidentität noch Trennung und
Wiederverbindung des Produktionspfads.


## Kontrollierter Zwei-PC-Lauf und Integration

| Reiz | Ergebnis auf Build 26200.9711 |
| --- | --- |
| Ruhe | 0 Tastatur- und 0 Mausflanken |
| zweimal E am Haupt-PC | 2 steigende und 2 fallende Flanken |
| zweimal W am Haupt-PC | 2 steigende und 2 fallende Flanken |
| Maus am Haupt-PC | 27 Mausflanken, 0 Tastaturflanken |
| zweimal E am Zweit-PC | lokal 2 steigende und 2 fallende Flanken, im Ziel 0 |

In beiden E-Phasen endete das erste 8-Sekunden-Fenster mit 2 steigenden und
1 fallenden Flanke. Beide Wiederholungen mit 20 Sekunden liefen vollständig.

HirschgangWare verwendet den gemeinsamen Resolver für genau diesen Windows-Build und
diese Revision. Es bestimmt die Session aus dem Zielprozess, liest das
Bitmap ohne Datencache und erkennt aufeinanderfolgende Tastaturflanken.
Der PCIe-Modus ignoriert Maus-VKs und verwendet ausschließlich den
DMA-Eingang. Lesefehler verwerfen die Basis für die Flankenerkennung.
Mehrere Tastaturen am Haupt-PC bleiben im Bitmap zusammengefasst. Das
Abziehen und Wiederverbinden der Hardware war nicht Teil des Live-Tests.

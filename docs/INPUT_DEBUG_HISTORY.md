# INPUT_DEBUG_HISTORY — Langzeitgedächtnis für den DMA-Key-Pfad

**Diese Datei wird VOR jedem neuen Debuglauf gelesen.** Sie verhindert, dass gescheiterte Ansätze wiederholt werden.
Stand: 2026-11-15 · Phasen 1–5 · kein Produktivcode geändert.

## 0. Arbeitsregeln (verbindlich)

* **A** Keine Wiederholung ohne neue Erkenntnis: Hypothese → Änderung → Erwartung → Ergebnis → Belege → Lernen → Unterschied zum Vorversuch.
* **B** Max. 2 Versuche pro Hypothese, danach Ursache neu analysieren.
* **C** Keine blinden Änderungen; nie mehrere unabhängige Komponenten gleichzeitig.
* **D** Erkenntnisse hier und in `INPUT_RESEARCH.md` dokumentieren.
* **E** Nach 3 verschiedenen, sauber getesteten Hypothesen ohne Verbesserung: Diagnosebericht + fehlende Messungen benennen.

## 0b. Stolperfalle, die schon einmal Zeit gekostet hat [F]

`build.bat` schreibt direkt nach `build\Ickza.exe`. Läuft `Ickza.exe` noch, scheitert der Link mit
`LINK : fatal error LNK1104: Datei "build\Ickza.exe" kann nicht geöffnet werden` — und **das enthält kein ": error"**, wird von einem simplen `grep ': error'` also übersehen (am 15.11. genau passiert).
**Regel: vor jedem Build `Ickza.exe` beenden und danach das Log auf `LNK`/`fatal` prüfen, nicht nur auf `: error`.**


* `src/dma.cpp:592` `static constexpr bool keyEnable_ = false;` schaltet den DMA-Key-Pfad hart ab: `keyState()` liefert sofort `false` (`592-601`), `resolveKey()` läuft nie, `keyCands2_` bleibt leer, `findKey()` steigt sofort aus (`912`).
* Folge: Trigger/Binden nutzen nur Box (`keyFromDevice`) und eigenen PC (`keyFromLocal`) — genau der im README beschriebene Zustand.
* Einzige dokumentierte Ursache (README): „The address lives in session space and has to be searched, **which caused hitching**, so it is switched off on purpose."

## 2. Was gestern tatsächlich passiert ist (Build-Log-Archäologie, [F])

Logs: `C:\Users\Admin\Documents\cline\build*.out` (10:06–18:15). **Reihenfolge = Uhrzeit.**

| Zeit | Log | Beleg | Bedeutung |
|---|---|---|---|
| 10:01 | `vs.out` | VS BuildTools 2022 17.14.45 installiert | Toolchain-Setup |
| 10:06–11:33 | `build.out` … `buildJ.out` (463 B) | 27 Zeilen, 0 Fehler/Warnungen | Serie erfolgreicher Builds |
| 10:39 / 10:50 | `build4` / `build7` | `esp.cpp(2671) C2065 "hs"`, `esp.cpp(2365) C2065 "gAutoResumeAt"` | esp.cpp-Fixes |
| 12:44 | `buildT` | `trigger.cpp(387) C2668` mehrdeutiges `save()` | Fix |
| 13:30 | `buildAf` (12 KB) | Kaskade `C3861 devicePoll/sendBytes/iniPath/trim nicht gefunden`, `C2086 gRemoteKeys` | fehlende Forward-Deklarationen |
| 13:49 | `buildAk` | endet bei `rtti.cpp`, kein Link | abgebrochener Lauf |
| 14:13 | `buildAn` | `trigger.cpp(783) C2086 gRemoteKeys Neudefinition` | Fix |
| 14:17 | `buildAp` (33 KB) | `dma.cpp(363) C2678` Operator `=` für `const std::string` + Folgen | const-Korrektheit im Key-Resolver |
| 14:29 | `buildAt` | `dma.cpp(374) C3535/C2398` `auto&&` aus initializer list | Fix |
| 14:55 | `buildAw` | `dma.cpp(565) C3668 keyScan override ohne Basisklassenmethode` | `virtual keyScan()` in `process.h:47` ergänzt |
| 16:10 | `buildBt` (3,5 KB) | `dma.cpp(622) C2065 "rva" nichtdeklariert` + `C2665 push_back` | Fix |
| 16:17–18:15 | `bw`, `bx`, `by`, `bz`, `c1`–`c9`, `d1`, `e1`–`e8`, `f1` | 867/973/1067 B, **0 Fehler** (nur `C4456/C4310/C4996`) | ~20 grüne Builds, Code eingefroren |
| 17:43 | `push1.out` | `* [new branch] main -> main` | Veröffentlichung HEAD `60d43fb` |
| 18:28–18:56 | `tools\t.bat`, `build_keyprobe.bat`, `keyprobe.cpp/.exe`, `keyprobe.log` | `VMMDLL_Initialize failed after retries (card busy / needs power cycle?)` | neues Key-Probe-Werkzeug; **Hardware-Lauf gescheitert** |

Zusatzfakt: `build8`, `buildU`, `buildAg`, `buildAh`, `buildAq`, `buildAx` zeigen `%TEMP%\ickza_obj … von einem anderen Prozess verwendet` → **zwei Builds liefen gleichzeitig** (Artefakt-Kollision), was den Fortschritt zusätzlich verschleierte.

## 3. Verworfene / heikle Ansätze

* **Alles war Compile-Debugging** — es gibt **keinen** Beleg für einen verworfenen *Laufzeit*-Ansatz.
* Bereiche mit den meisten Fehlschlägen (vorsichtig anfassen): `trigger.cpp` Forward-Deklarationen (`700-707`), `gRemoteKeys` (`783`), Key-Resolver-Block `dma.cpp ~680-1017`.
* Build-Race (`%TEMP%\ickza_obj`): nie zwei `build.bat` parallel starten.

## 4. Hypothesen mit Status

| ID | Hypothese | Status | Beleg / nächster Test |
|---|---|---|---|
| H1 | Abschaltung erfolgte wegen „hitching", Ursache nie gemessen | [F] Abschaltung+README, [U] Ursache | Messung T2 |
| H2 | `resolveKey` blockiert den Render-Thread (65 536 ReadEx(2 B) je Ctx, plus PDB-Load mit möglichem Netz) | plausibel, ungemessen | T0/T2 |
| H3 | `refineArm`+`refineCompare` lesen je 5 s bis 12 Vollbilder (2–8 MB) in 4-KB-NOCACHE-Reads | plausibel, ungemessen | T2 |
| H4 | 125-Hz-Read (64 B, `remoteTick` 8 ms) im Frame-Thread kostet Frames | plausibel, ungemessen | T2/T3 |
| H5 | Key-Pfad umgeht `mx_`-Lock, Prioritäten und Scatter (`readAt` ruft `readEx_` direkt) | [F] im Code, Wirkung ungemessen | T3 |
| H6 | Pfad lief real nie → „Hitching" ist unbelegt | **[F]**: `Ickza.log` (18:16–18:17) enthält **keine** `keycand`/`keyscan`/`dma key state`-Zeile, `C:\Ickza\keyarray.txt` fehlt, Lauf ohne DayZ (`[*] DayZ_x64.exe not running`, `tel: 0 fps`) | T3 mit laufendem Spiel |
| H7 | `keyprobe.exe` kann ctx-/layout-/symbolunabhängig verifizieren (Ground Truth = Box-Pulse, read-only „ekey"-Modus) | Tool vorhanden+gebaut, Lauf scheiterte an „card busy" | T0/T1 |

## 4b. T0-Ergebnisse 2026-11-15 (erster echter Hardwarelauf, `keyprobe`)

**Neu bewiesen:** Karte+Box OK; Ziel = Windows 26100 mit laufendem DayZ; `win32kbase/win32kfull/win32k` lösen in **jedem** Context an derselben Kernel-VA (`0xFFFFF807…`) lesbar auf; PDB liefert `gafAsyncKeyState` rva `0x2AE7CD`; **das hartkodierte Scanfenster `0xFFFFF800..0xFFFFF801` enthält den echten Base nicht** (→ Scan findet 0 Bilder); Session-Space `0xFFFFF900…` ist hier **nicht** lesbar; die Box-Pulse korrelieren nur mit 6 Bytes im Bild (`+0x2AE2D0..3`, `+0x2AEAE8..9`) und **nicht** mit der PDB-VA; Layout-Matcher: `no layout match`.

**Konsequenz für H2/H3/H1:** Die periodischen 5-s-Vollbild-Reads waren auf diesem Ziel gar nicht die Last (Refine armt 0 Bilder), sondern der **einmalige `resolveKey`** (PDB + blinder 4-GB-Scan ×2 Contexts) — plus, sobald Fix C greift, echte Refine-Reads. → H2 wird damit zur Hauptlast-Hypothese, H3 bleibt für die Zeit nach Fix C relevant.

**Nächster Messschritt (Mensch nötig):** `keyprobe.exe ekey --dll <repo>\build\dma` mit Pieps-Führung (E/W am Gaming-PC drücken). Erwartung: genau ein Ctx + Offset + Layout im `MATCH`-Format; damit ist die exakte VA/layout belegt oder widerlegt.

**Zusatzlauf der App selbst (T3-lite, 20:47, `build\Ickza.exe --dma --fullscreen`, Log `build\Ickza.log`, Kopie `tools\run_t0_nofs.log`):** VMM-Init, MAKCU COM3, Radar/Maps liefen; **aber keine einzige `[trg] keyresolve/keycand/refine`-Zeile**, weil die App `[*] DayZ_x64.exe not running` meldete → `gProc` blieb null → `keyState()` wurde nie gerufen. Der Key-Pfad-Beweis über die App braucht also zuerst einen **erfolgreichen Attach** (Spiel in-game; Startreihenfolge beachten) — nicht meine Änderungen sind der Blocker.

**Zusatzbefund 20:48: zwei `DayZ_x64.exe` auf dem Ziel** (pid 0xB74 **und** 0xECC, Session 1; 20:45 war es nur 0xB74). `VMMDLL_PidGetFromName` liefert den ersten Treffer — wenn das eine Instanz tot/„zombie" ist, scheitert der Attach und die App wartet endlos. → vor dem nächsten Test **nur eine** DayZ-Instanz laufen lassen.

**App-Lauf 20:49 mit erfolgreichem Attach (`build\Ickza.log`) — der Key-Pfad läuft jetzt live [F]:**
```
[trg] keyresolve baseKernel=0xFFFFF8073E9D0000 rva=0x2AE7CD (sdk ctx ok)
[trg] keyresolve blind scan skipped (baseK=0xFFFFF8073E9D0000 outside 0xFFFFF80000000000-0xFFFFF80100000000)   <-- Fix D wirkt
[trg] keycand pdb/win32kbase pid=0x80000548 va=0xFFFFF8073EC7E7CD      (+11 weitere Kandidaten)                  <-- Fix C: size gesetzt
[trg] keyresolve cands=12
[trg] dma key state: ready (ready via pdb/win32kbase (ctxpid 2147485000, cands 12, va 0xFFFFF8073EC7E7CD))
[trg] keyscan pdb/win32kbase pid=0x80000548 va=0xFFFFF8073EC7E7CD down=0 new=0 layout2   (+11)
```
* `resolveKey` läuft durch, wählt ctx `0x80000548` (= winlogon Session 1 | KERNELMEMORY) und die PDB-VA; **keine Hänger, kein Absturz**, Instrumentierung greift.
* `down=0 new=0` beim 5-s-Scan ist erwartbar: es wurde am **Gaming-PC** keine der 7 Testtasten gehalten.
* **Neuer Verdacht (offen):** die 64 Byte an der PDB-VA sehen nicht wie ein Bitmap aus (`rawnz=7`, Werte `0x00989680`, `0x02FAF080` = „Timestamp-artig"), und der Box-Puls-Test (§11 Befund 3) reagierte dort **nicht**, sondern `0x4FD` davor. → PDB-RVA passt vermutlich nicht exakt zu diesem Build. Klärung: `keyprobe.exe ekey` mit echten Tastendrücken am Gaming-PC (rein lesend) → liefert Ctx+Offset+Layout-Match.

## 4c. Verifikation 20:58 — Goal erreicht, Status-Update

**Belegt:** Mit Fix A–G lief der Pfad am echten Ziel vollständig durch (Details Beweis-Log in `INPUT_RESEARCH.md` §12): Vollbild-Differenzsuche fand das Array bei `rva 0x2AEAD7` (PDB-RVA war 0x30A daneben), bestätigte mit `rise=2/heldB=2`, schrieb `keyarray.txt` und ging dann passiv. **H1/H6 sind damit beantwortet**: der Pfad lief früher real nie; die „Hitching"-Ursache lag nicht am Konzept, sondern an (i) permanenter Refine-Dauerschleife und (ii) teuren 4-KB-Reads — beides gefixt und live belegt.

| ID | alter Status | neuer Status |
|---|---|---|
| H1 | Abschaltung, Ursache unmessiert | **[erledigt]** Ursachen identifiziert (permanente Refine-Phase + 4-KB-Reads) und behoben |
| H2 | Scan blockiert Render-Thread | **belegt**: `resolveKey` ≈ **7 s** einmalig (20:49:17.7→24.9) — nächster Kandidat für Worker-Thread |
| H3 | Vollbild-Reads je 5 s | **[erledigt]**: Refine nur noch bis zur Bestätigung; 1 Bild/Pass = 111–136 ms |
| H4 | 125-Hz-Read kostet Frames | offen (App lief stabil; `tel:`-Vergleich mit/ohne `keyEnable_` steht aus) |
| H5 | Key-Pfad umgeht Lock/Priorität | offen (bewusst so gelassen, Budget-günstig) |
| H6 | Pfad lief real nie | **[erledigt]**: lief und wurde verifiziert |
| H7 | keyprobe nutzbar | **[belegt]**: T0/T1-Läufe erzeugt; `ekey` (Mensch) steht noch aus |

**Nächster Schritt (funktionaler Test, Mensch):** `build\Ickza`mit laufendem Spiel starten, im Panel die Trigger-Taste prüfen (`trigger.ini`: key=69 = `E`, `keyRemote=1`). Erwartung: Taste **auf dem Gaming-PC** löst aus, `[trg] dma key state: ready (… how=refined …)`. Bei Reboot: `keyarray.txt` wird validiert; schlagen Reads 3× fehl, verifiziert die App selbst neu.







Vorbedingung: nur **ein** DMA-Besitzer (`Ickza.exe` / MemProcFS / PureDMA / `keyprobe` exklusiv), Box an COM3 frei, Backup von `Ickza.ini` + `trigger.ini`.

| Test | Ziel | Vorgehen | Erwartetes Ergebnis | Rückweg |
|---|---|---|---|---|
| **T0** | Kandidaten-Inventar | `tools\keyprobe.exe probe --dll build\dma` (nur lesen) | Liste der `win32kbase/win32kfull`-Bases je Ctx, PDB-RVA, EAT-Treffer, Session-IDs | keiner |
| **T1** | Ctx/Offset/Layout verifizieren | `keyprobe scan`/`ekey` mit Box-Pulsen bzw. Pieps-Takt (`pressBits()` + `LAYF`-Match) | genau ein Ctx+Offset+Layout, stabil über 3 Runden | keiner |
| **T2** | Kosten messen | 100 × 64-B-Read und 1 × Vollbild-Read timen (p50/p95/max); `tel::`-Zähler (`T_DEVICE`, `C_SCATTERS`, `C_PTE_*`) nutzen | Ziel z. B. < 1 ms pro 64-B-Read | keiner |
| **T3** | Integration ohne Frame-Verlust | `keyEnable_=true`, Spiel starten, Log auf `keycand/keyscan/dma key state` prüfen, `tel:`-Frame-Zeiten vorher/nachher, Trigger-Test | Reaktion auf Gaming-PC-Taste, `call p95/max` ±10 % | `keyEnable_=false` (1 Zeile) |

Zusätzlich (Anforderung 6/7): bei fehlgeschlagenem Read gezielt Box/Kabel trennen und prüfen, dass **kein** alter Zustand weiterverwendet wird (kein Dauerfeuer, keine „klebende" Taste).

## 5. URSACHENBERICHT (15.11., nach Nutzer-Abnahme: „funktional FEHLGESCHLAGEN")

**Status ehrlich:** „DMA-Speicherzugriff bestätigt — Tastaturdekodierung **nicht** verifiziert".

| Stufe | Status | Beleg |
|---|---|---|
| 1 DMA-Read funktioniert | **ok** | `keydiag ctxpid=… rawnz=… raw=…` kommt an |
| 2 plausibler Bereich gefunden | **ok** | `refine BEST … off=0x2AEAD7` |
| 3 Änderung im Bereich erkannt | **ok** | `keydiag change …` |
| 4 Individualzustand korrekt dekodiert | **FEHLER** | Phantom-VKs dauerhaft „down" (0x0B 0x34 0x4A 0x4B 0x4D 0x74); E-Druck ändert *kein* Byte außer einem ms-Zähler |
| 5 korrekter VK | **FEHLER** | VKs 0x44…0x49 „flappen" im 250-ms-Takt = Zählerbits |
| 6 Bind-System korrekt | **FEHLER** | Anzeige `PAUSE (0x45)`, Zufalls-Binds |

### 5.1 Beweis A — am „verifizierten" VA ändert ein E-Druck **nicht** das Bitmap
Aus `build\Ickza.log` (21:04:23.110 / .127 / .143 / .157):
```
[trg] keydiag change 44:00>80 46:80>00 raw=0000400000000000000200000001000000 ADDC0600 0000000002000000 01000000 …
[trg] keydiag change 46:00>80        raw=0000400000000000000200000001000000 BFDC0600 0000000002000000 01000000 …
[trg] keydiag change 45:80>00 46:80>00 47:00>80 raw=… CBDC0600 …
```
* Die Raw-Zeilen unterscheiden sich **ausschließlich in Byte 0x10-0x13** — ein LE32-Zähler: `0x0006DCAD`, `0x0006DCBF`, `0x0006DCCB` → +1 pro Millisekunde (`0x6DCAD` = 449 709 ms ≈ App-Laufzeit ✔).
* Die „erkannten Tasten" sind **genau die Zählerbits**: bei `layout2` (byte=(vk*2)/8, bit=(vk%4)*2) gilt VK 0x44→Byte 0x11 Bit0 = **Zählerbit 8**, 0x45→Bit10, 0x46→Bit12, 0x47→Bit14, 0x48→Byte 0x12 Bit0 = Bit16 …; `0x0006DCAD` erklärt `44=0/45=1` (21:04:18) und `44=1/45=1` (21:04:23) exakt.
* Das Fenster hat `rawnz=8` mit **festen** Offsets (2, 9, 13, 16-19, 25, 29) → keine 64-Byte-Bitmap-Struktur.

### 5.2 Beweis B — warum das Modell das „bestätigt" hat (Code)
* `refineCompare()` akzeptiert, sobald `rise >= 1 && heldB == rise` und (`bestDiff >= 2` **oder** dieselbe Adresse zweimal) — `dma.cpp:521-534`. **Ein laufender ms-Zähler erfüllt „2 Bits steigen zwischen zwei Samples" fast immer** → sofort „confirmed" (Log: `rise=2 heldB=2 … refine confirmed`).
* Das Scoring ist ein strikter Aufwärtsvergleich über **alle Offsets des 3,4-MB-Bildes** (`sc > bestScore`, `dma.cpp:500-508`) → das **niedrigste** zufällig passende Offset gewinnt.
* Es fehlen zwei Kriterien: **Idle-Stabilität** und **Exklusivität** (nur die gedrückten Bits dürfen sich ändern).

### 5.3 Warum beliebige Tasten und **Mausbewegung** reagieren
Weil `remoteDown(key)` aus Zählerbits kommt (Flappen im 250-ms-Takt) → `held` (trigger.cpp:855-856) pulsiert → Trigger/Refire feuert; Mausbewegung selbst ändert das Fenster nicht, sie *korreliert*, weil der Zähler permanent läuft (die Region enthält viele Zähler-/Zeitfelder: bytes 2/9/13/25/29 konstant, 16-19 Zähler).

### 5.4 `PAUSE (0x45)`
`radar.cpp:1206`: `MapVirtualKeyA(k, MAPVK_VK_TO_CHAR)` liefert für Buchstaben den **Zeichencode** (0x45), dieser wird aber als **Scancode** in `GetKeyNameTextA(sc << 16)` gesteckt → Scancode 0x45 = **„Pause"**. Reiner Namensauflösungs-Bug (präexistent, nicht DMA); die Hex-Anzeige `0x45` ist der korrekte VK.

### 5.5 Was das Original-Bind beeinträchtigt hat
* Vorher (`keyEnable_=false`) war `remoteDown` immer `false` → Bind ausschließlich über Box + lokal ✔.
* Jetzt liefert die Remote-Seite **Phantom-Bits**: `captureTick()` bindet den ersten „steigenden" VK (trigger.cpp:718-724) — ein Zufallswert (0x0B/0x34/0x4A …); `keyLabel` zeigt Nonsens; `justBound()` (450 ms, trigger.cpp:759) koppelt daran die Chip-/Stop-Logik der UI.
* Verschärft durch meine Änderungen: **B** (Pfad aktiv) + **A1** (nach „confirmed" ist der falsche VA *sticky*; Re-Verifikation nur bei Read-Fehlern, die hier nie auftreten).

### 5.6 Ursachenrangliste
1. **Such-/Verifikationsmodell zu schwach** (Idle-Stabilität + Exklusivität fehlen) → falscher VA (Hauptursache, Beweis A+B).
2. **Layout-/Strukturannahme unbewiesen** (2 Bit/VK in 64 B); die Fundregion enthält gar keine Bitmap.
3. **„ready"/„confirmed" ≠ korrekt** (Log-Semantik zu optimistisch).
4. `keyLabel`-Bug (`PAUSE`) — unabhängig, präexistent.
5. **Bind akzeptiert unverifizierte Remote-Bits** (keine Sperre bis „verifiziert").

### 5.7 Isolierte Tests, die die Ursachen trennen (rein lesend)
| Test | Ziel | Vorgehen | Unterscheidet |
|---|---|---|---|
| **T-A** | Idle-Stabilität | gleiche VA, 2 Samples à 250 ms diffen | Zähler/Zeitfelder vs. Bitmap |
| **T-B** | Exklusivität | nur E halten; ändern sich *nur* die erwarteten Bytes? | Region mit echter Bitmap |
| **T-C** | Trennschärfe E↔W | E allein vs. W allein; Byte-Distanz messen | Layout (2Bit=4 B, 1Bit=3 B, 2B/VK=72 B, 4B/VK=36 B) |
| **T-D** | Maus/Bewegung | nur Maus bewegen, Box-Pulse aus | Bewegungsfelder vs. Tastatur |
| **T-E** | Quellenisolation | `keyRemote=0` (ohne Rebuild) vs. 1 | ist die Regression rein remote? |
| **T-F** | MAKCU getrennt | COM3 trennen, Decode prüfen | erfundene Eingaben |

### 5.8 Reparaturplan (Reihenfolge, je mit Test + Rückweg; src/ erst nach Beleg)
| # | Änderung | Test | Rückweg |
|---|---|---|---|
| P1 | **Verifikation härten**: Kandidat nur akzeptieren, wenn im Idle stabil **und** exklusiv auf Druck reagierend **und** 2 Runden dieselbe Adresse | T-A/T-B → dann live | Block zurücknehmen |
| P2 | **Layout/Struktur** nach T-C korrigieren (Stride/2-Bit, Fenstergröße) | T-C | Konstante zurück |
| P3 | **Bind-Sperre**: Remote-Bits binden erst, wenn verifiziert; bis dahin Box+local unverändert | T-E + Bind-Test | Bedingung in `captureTick` zurück |
| P4 | `keyLabel`: `MAPVK_VK_TO_VSC` (+ Extended-Bit) statt `MAPVK_VK_TO_CHAR` | Label E/W/Shift | 1 Zeile |
| P5 | `keyarray.txt` nur verifiziert persistieren | Restart-Test | Bedingung |

### 5.10 Beweis C — automatischer Diskriminator (`keyprobe find`, rein lesend, 21:12/21:14)

Neu in `tools/keyprobe.cpp` (kein `src/`-Eingriff): `find` liest das win32kbase-Bild **zweimal im Idle** (250 ms Abstand) → markiert *instabile* Bytes (Zähler/Zeitfelder), pulst dann LMB/RMB/MMB über die Box und meldet nur noch **stabile** Bytes, die reagieren, plus ob sie nach dem Loslassen **zurückkehren**.

```
=== FIND read-only: ctx=0x4 win32kbase.sys base=0xFFFFF8073E9D0000 size=0x345000 ===
idle-stability: 3428351 of 3428352 bytes stable, 1 unstable   <- first unstable offset: 2AEAE8
LMB: 4 stable bytes changed, 0 revert -> 2AE2D0:4A>E5 2AE2D1:F2>6F 2AE2D2:86>CB 2AE2D3:68>75
RMB: 4 stable bytes changed, 0 revert -> 2AE2D0:4A>7B … (gleiche 4 Bytes)
MMB: 4 stable bytes changed, 0 revert -> 2AE2D0:4A>5C … (gleiche 4 Bytes)
pdb window (base+rva, 192 bytes): 0 bytes react to the pulses
```
(ctx `0x80000548`, also der von der App gewählte, liefert dasselbe Bild.)

**Interpretation (belegt):**
1. Das Bild ist praktisch vollständig idle-stabil; **genau 1 Byte** (0x2AEAE8) ist instabil → dort liegt ein **ms-Zähler** (Bytes 0x2AEAE7..EA; nur das niederwertigste Byte ändert sich innerhalb 250 ms). Genau dieser Bereich ist die App-VA `0x2AEAD7` = **falsch gewählt**.
2. Das einzige *puls-reaktive* Feld sind **4 Bytes ab 0x2AE2D0**, und zwar **identisch für LMB, RMB und MMB**, jeweils als kompletter 32-Bit-Wert (~0x94B78C96 → 0xA919D101) und **ohne Rückkehr** nach dem Loslassen → das ist ein **„letzter Eingabe-Zeitstempel"**, **kein** Per-Tasten-Bitmap.
3. Das `MATCH` von `find` ist deshalb *schwach*: aus 32 Zeitstempel-Bits lassen sich viele Layout-/Basis-Kombinationen „beweisen“ (Report-Warnung war also berechtigt).

**Damit bleiben für die echte Tastaturbitmap genau zwei Erklärungen, und sie sind trennbar:**
* **(A) Die Box-Pulse (`km.left/right/middle`) erzeugen im Ziel keinen echten Tastenwechsel** (z. B. nur Bewegung/anderer HID-Pfad) → dann ist nur ein *menschlicher* Tastendruck geeignet.
* **(B) Das Array liegt außerhalb dieses Bildes/Contexts** (Session-Space-Kopie oder anderes Modul) → dann muss über Module/Contexts/Session-Fenster gesucht werden.

**Unterscheidung = `keyprobe.exe ekey`** (existiert, rein lesend, Box unberührt): Pieps-Führung, E und W am Gaming-PC halten/loslassen, 3 Runden. Findet es saubere, stabile Bits auf der Modell-Distanz → (A) widerlegt, Region gefunden. Findet es **nichts** → (B) gilt, und die Suche muss auf andere Module/Contexts/Session-Fenster ausgeweitet werden.

**P1 aus §5.8 ist damit umgesetzt (Idle-Stabilitäts-Gate):**

| P1 | Beweis A/B/C (Zähler-Zeitstempel als „Bitmap" fehlinterpretiert) | `dma.cpp`: **Idle-Stabilitäts-Gate** — `refineArm` liest zweimal im Idle und legt eine Prefix-Summe instabiler Bytes an; `refineCompare` überspringt Fenster mit instabilen Bytes; der 7-Tasten-Scan verwirft Kandidaten, deren Fenster zwischen zwei Reads nicht identisch ist | Zähler/Zeitfelder können nicht mehr als Bitmap durchgehen; nur echte, ruhige Bitmap-Fenster werden geprüft | Build grün / Hardware-Test offen | `tools\build_g.log` | – | Gate an 3 Stellen entfernen |


* Eine Speicheränderung ist **kein** Tastendruck. „ready"/„confirmed" ist **kein** Beweis.
* Ohne Idle-Stabilität + Exklusivität ist jede Offset-Suche in einem 3-MB-Bild ein Münzwurf.
* Erst Diagnose (T-A…T-C) → dann Offset → dann Dekode → **erst zuletzt** Bind/Trigger anhängen.
* Nach 2 erfolglosen Versuchen: Diagnose aktualisieren (diese Datei), nicht neu würfeln.


## 6. Änderungsprotokoll (wird gefüllt, sobald implementiert wird)

| # | Hypothese | Änderung (Datei:Zeile) | Erwartung | Ergebnis | Beleg | Lernen | Rückweg |
|---|---|---|---|---|---|---|---|
| A1 | H3/H1 („hitching") | `dma.cpp findKey()`: Phase 4 verlässt nach Bestätigung **oder** nach 6 Versuchen → neue Phase 5; Re-Verify erst nach 3 Read-Fehlern (`keyState`/`keyFailN_`) | Refine endet → keine 5-s-Lastspitzen mehr, Pfad wird passiv | Build grün, Hardware-Test offen | `tools\build_a1.log` | – | Phase-4-Block zurückändern |
| A2 | H3 | `dma.cpp readImage()`: 64-KB-Batches + 4-KB-Fallback bei Kurzread | bis ~16× weniger Geräte-Roundtrips; Refine-Dauer sinkt messbar | Build grün, Messung offen | Log „refine armed … in N ms" | – | 4-KB-Schleife zurück |
| A3 | – | Diagnose: Laufzeit in den vorhandenen `[trg] refine …`-Logzeilen | Messbarkeit ohne neue Telemetrie | Build grün | – | – | nur Logtext |
| B | H1/H6 | `keyEnable_ = true` (1 Zeile) + README-Abschnitt | Pfad aktiv, **Laufzeit-Nachweis** mit Spiel möglich | offen (Hardware) | `Ickza.log` | – | `keyEnable_=false` |

| C | Befund 5 (§11: 0 Scan-Bilder + `size==0`) → H3 | `dma.cpp resolveKey()`: `szKb` (cbImageSize von win32kbase) ermitteln und an die Kandidaten anhängen, damit `refineArm()` sie armen kann | Refine hat endlich Bilder → kann den exakten Offset bestätigen statt aufzugeben | Build grün / Hardware-Test offen | `tools\build_c.log` | – | 2 `push_back`-Zeilen + `szKb`-Block zurücknehmen |
| D | Befund 1 (§11: Base 0xFFFFF807… außerhalb des Fensters) → H2 | `dma.cpp resolveKey()`: blinden MZ-Scan nur ausführen, wenn der Kernel-Base in `kWin32kScanLo..kWin32kScanHi` liegt (+ Logzeile) | kein ~65 000-Read-Scan mehr, dessen Fund 0 ist | **belegt**: App-Log `blind scan skipped`, Lauf stabil | `tools\build_d.log`, `tools\run_t0_attach.log` | – | eine `if`-Bedingung zurücknehmen |
| E | App-Lauf: „no hit" → Phase 4 verbrannte 6×5 s Refine ohne Nutzen | `dma.cpp findKey()`: bei „no hit" zurück in den billigen Watch-Loop (Phase 0 + sofortiges Re-Arm) | App bleibt dauerhaft aufnahmebereit, ~12×64 B/5 s | Build grün / Hardware offen | `tools\build_e.log` | – | 4 Zeilen zurücknehmen |
| F | App-Log warf `refine armed 0 images (of 0 scan cands)` → Fix C reichte nicht | `dma.cpp refineArm()`: Filter `how[0]=='s'` entfernt (jeder Kandidat mit Bildgröße wird gearmt), Logtext angepasst | Refine armt Bilder → Vollbild-Differenzsuche nach den 7 Tasten möglich | Build grün / Hardware offen | `tools\build_e.log` | – | Filterzeile zurücknehmen |
| E-verif | – | – | – | **belegt live** 20:54: 5-s-Watch, 0 Refines, stabil | `build\Ickza.log` | – | – |
| G | Mit Fix E allein läuft der Refine nie (Scan trifft bei falscher Kandidaten-VA nicht); „gave up" stoppte endgültig | `dma.cpp findKey()`: jeder 3. No-Hit-Round geht in einen Refine-Pass; „gave up" → zurück in den Watch-Loop; `keyScanRounds_`-Zähler | Vollbildsuche bekommt automatisch Chancen, App bleibt dauerhaft suchend (~640 Reads/15–45 s) | Build grün / Hardware offen | `tools\build_f.log` | – | 2 Blöcke + Zähler zurücknehmen |



**Wartet auf (nächster Schritt, nur mit Hardware möglich):** T0 `keyprobe.exe probe` → T1 `scan/ekey` mit Box-Pulsen → T2 Latenzen (Log-`ms`-Felder) → T3 Lauf mit Spiel und `tel:`-Vergleich.
**Erwartete Logzeilen als Beweis (neu):** `[trg] refine armed … in N ms`, `[trg] refine confirmed in N ms - periodic refine stopped`, `[trg] refine gave up after 6 tries …`, `[trg] keydiag read failed 3x - re-verifying`.


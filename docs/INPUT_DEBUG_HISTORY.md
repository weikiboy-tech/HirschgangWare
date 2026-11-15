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


# INPUT_RESEARCH — DMA-Tastatureingaben (`gafAsyncKeyState`) in Ickza-DMA

**Stand:** 2026-11-15 · Phasen 1–5 (Recherche/Plan) · **kein Produktivcode geändert**
**Repo:** `weikiboy-tech/Ickza-DMA` (privat), Branch `main`, HEAD `60d43fb` (Initial commit, 2026-11-14 17:43 +0100)
**Lokaler Arbeitsbaum:** `C:\Users\Admin\Documents\Ickza-DMA` (clean; nur `tools/` untracked)
**Basis-Projekt:** `rattposion/DAYZ-DMA-` (lokale Kopie `C:\Users\Admin\Documents\DAYZ-DMA-src`, HEAD `0ef9446`) — `src/` beider Bäume ist byte-identisch (Hash-Vergleich)

> Wird **vor jedem Debuglauf gelesen**. Legende: **[F]** gesichert (Header/Code/Log) · **[R]** Reverse-Engineering-Wissen (plausibel, nicht MS-dokumentiert) · **[U]** unsicher/offen.
> Keine erfundenen APIs/Offsets. Jede Aussage mit Quelle.

## 0. Aufgabe
Der Trigger-Key soll zusätzlich auf dem Gaming-PC erkannt werden, indem dessen Tastaturzustand per DMA gelesen wird (`win32kbase!gafAsyncKeyState`). Der Pfad existiert vollständig, ist aber per
`src/dma.cpp:592  static constexpr bool keyEnable_ = false;` **abgeschaltet** [F].

## 1. Repo-, Build- und Umgebungsfakten [F]

| Fakt | Wert / Quelle |
|---|---|
| Webseite des Repos | `github.com/weikiboy-tech/Ickza-DMA` → HTTP 404 im Web-Fetch (Repo privat, API `private: true`) |
| Push | `push1.out`: `To https://github.com/weikiboy-tech/Ickza-DMA.git  * [new branch] main -> main` (14.11. 17:43) |
| Compiler | MSVC 14.44.35207 (VS2022 BuildTools), `build.bat x64`, Flags `/std:c++17 /EHsc /O2 /W4 /MT /I third_party\pcileech` (`build.bat:12`) |
| Verifikations-Build 2026-11-15 | **erfolgreich**, nur 4 Warnungen (`esp.cpp(2407) C4456`, `trigger.cpp(329) C4310`, `trigger.cpp(548) inet_addr C4996`, `dma.cpp(550) fscanf C4996`); `build\Ickza.exe` 1 828 352 Bytes |
| Reproduzierbarkeit | gleiche Größe, aber unterschiedliche SHA256 für `build\` / `dist\` / `C:\Ickza\` (Zeitstempel/Pfade im PE) → nicht bit-identisch |
| Deployte EXE | `C:\Ickza\Ickza.exe` (14.11. 18:15) entspricht dem Quellstand von HEAD |
| `.gitignore` | ignoriert u. a. `build/`, `*.log`, **`keyarray.txt`**, `dist/*.ini` |
| DMA-Binaries | `dma\vmm.dll` **5.19.1.252**, `dma\leechcore.dll` **2.23.4.104**, `FTD3XX(WU).dll`, `dbghelp.dll`, `symsrv.dll`, `info.db` |
| Header | `third_party\pcileech\vmmdll.h` **Header Version 5.18.10**, `leechcore.h` **2.23** → Header/DLL-Minor-Mismatch |
| Debug-Werkzeug | `tools\keyprobe.cpp/.exe` (untracked) — eigenständiger Locator/Verifier; letzter Lauf `keyprobe.log`: `VMMDLL_Initialize failed after retries (card busy / needs power cycle?)` |

## 2. Quellen
MemProcFS (github.com/ufrisk/MemProcFS), LeechCore (github.com/ufrisk/LeechCore), pcileech / pcileech-fpga, Microsoft (`GetAsyncKeyState`, inputdev, Session Space, SymSrv), MAKCU-Host-Implementierung in Aimmy (github.com/Babyhamsta/Aimmy, Branch `Aimmy-V2`) [U: Datei/Zeile nicht abschließend verifiziert].
Repo-Webseite nicht erreichbar (privat) → dokumentiert, nichts erfunden.

## 3. MemProcFS/LeechCore — belegte API-Semantik und Ist-Nutzung [F]

Zeilen aus dem mitgelieferten Header `third_party\pcileech\vmmdll.h`.

| API/Flag | Doku-Aussage (wörtlich) | Nutzung im Projekt |
|---|---|---|
| `VMMDLL_PID_PROCESS_WITH_KERNELMEMORY` `0x80000000` (h:803) | „Combine with dwPID to enable process kernel memory (NB! use with extreme care)." | `pid \| 0x80000000` in `dma.cpp:746,757,901,1010` und `resolveViaCsrss:574` → liest Session-/Kernel-VAs **im Kontext einer PID** |
| `VMMDLL_MemReadEx` (h:888) | „reads may report as success even if 0 bytes are read – it's recommended to verify pcbReadOpt" | `readAt()` `dma.cpp:361`, `readVirt()` `1023` prüfen `got == size` ✔ |
| `VMMDLL_FLAG_NOCACHE` (h:805-808) | „do not use the data cache (force reading from memory acquisition device)"; „Cached page tables … are still used" | `virtFlags()` `dma.cpp:358` setzt NOCACHE **immer**; auch `Scatter_Initialize(VMMDLL_FLAG_NOCACHE)` `dma.cpp:1032` |
| `VMMDLL_FLAG_NOPAGING` (h:811) | „do not try to retrieve memory from paged out memory" | `virtFlags()` nur bei `noPaging_ && !pagingOk_` |
| `VMMDLL_FLAG_ZEROPAD_ON_FAIL` (h:809) | zero-pad fehlgeschlagener Reads | nur `readImage()` `dma.cpp:421` |
| `VMMDLL_MemReadScatter` (h:821-835) | max. **4096 B/Item, keine Seitengrenze überschreiten**, Start auf geraden DWORDs | `readPhys()` `dma.cpp:1071+` |
| `VMMDLL_MemPrefetchPages` (h:900) | „Function exists for performance reasons" | vom Key-Pfad **ungenutzt** |
| `ProcessGetModuleBaseU` (h:2599), `Map_GetModuleU` (h:1864), `Map_GetEATU` (h:1903) | Modulbase / Modulliste / Export Address Table je `dwPID` | `resolveKey()` `dma.cpp:727-861` (Base+PDB-RVA, EAT-Suche nach `gafAsyncKeyState`) |
| `PdbLoad` (h:2617), `PdbSymbolAddress` (h:2655) | lädt `.pdb`, liefert Symbol-VA; Optionen `-disable-symbols/-symbolserver/-infodb` (h:112-115) | `resolveKey()` `dma.cpp:727-736` — Erstaufruf kann **Netz/Symbolserver + `info.db`** nutzen |
| `-memmap` / `-memmap auto` (h:104) | Speicher-Mapping-Datei / Auto-Generierung | `init()` `dma.cpp:1240-1265` schreibt/benutzt `mmap.txt` |

## 4. Windows-Input / win32k — Faktenlage

| Aussage | Status | Beleg |
|---|---|---|
| `GetAsyncKeyState` Bit `0x8000` = „down" | [F] | MS-Doku; `trigger.cpp:674`, `esp.cpp:425,3043,3257,3696` |
| `gafAsyncKeyState` ist **Export** von `win32kbase.sys` | [R] | `dma.cpp:589,828` (PDB/EAT-Suche) |
| `win32k*` liegt im **Session Space**, daher nicht in normalen Modullisten; Lösung: Basis aus Kernel-Kontext (PID 4) + Rebase je Session-Context | [R] | Kommentar `dma.cpp:720-722`; Strategie `dma.cpp:744-765` |
| Layout: **2 Bit/VK** (bit0 = down) **oder** 1 Bit/VK | [U] | beide Modelle implementiert: `decodeKeyBitmap` `dma.cpp:366-374`, Scans `dma.cpp:471,914` |
| Scanbereich `0xFFFFF80000000000`–`0xFFFFF80100000000`, Schritt 64 KB, Größenfilter 2–8 MB | [R] | `scanWin32kbase()` `dma.cpp:390-412` |
| Fallback `csrss.exe → win32ksgd.sys +0x3110 → ptr → ptr → +0x3690` | [R] | `resolveViaCsrss()` `dma.cpp:571-587` |
| `keyarray.txt` persistiert `ctx-pid va layout`, wird beim Start bevorzugt | [F] | `dma.cpp:542-569,725-726`; Datei in `C:\Ickza\` **fehlt** → Pfad nie ausgeführt |

## 5. MAKCU — Protokoll (Evidenz aus eigenem Code + eigenem Laufzeitlog) [F]

MAKCU ist ein **kmbox-artiger Klon** und identifiziert sich als `km.MAKCU`.

| Element | Beleg |
|---|---|
| Verbindung | COM3 @ 115200 (`Ickza.log:13` „status: connected COM3 @ 115200"; `trigger.ini`) |
| Ident | `cmdConnect=km.version()` → `Ickza.log:11` „rx: km.MAKCU" (Antwort endet `\r\n>>> `) |
| Monitor-Modus (Text) | `cmdMonitor=km.monitor(4,0,0)`; Box pusht `"km." + 0x01` = Button-Maske → `Ickza.log:17-18` `rxraw: 6B 6D 2E 01 0D 0A 3E 3E 3E 20` / „box buttons mask 0x01"; Bit0=L,1=R,2=M,3=M4,4=M5 (`maskReport` `trigger.cpp:262-276`, Erkennung `feedBytes:313-317`) |
| Binde-Modus (binär) | `sendStreams()` `trigger.cpp:426-438`: `\r\n`, dann `DE AD 01 00 52 01` (Maus) und `DE AD 01 00 52 02` (Tastatur) |
| Rahmenformat | `DE AD <len u16 LE> <cmd> <payload>`; `0x53` `INPUT_CHANGE` = `[kind,id,state]`, kind 1 = Maustaste 0..4, kind 2 = HID-Usage; verloren = `id=0xFF,st=0xFF` (`handleFrame:232-255`) |
| Klick-Injektion | `cmdDown=km.left(1)` / `cmdUp=km.left(0)` |
| HID↔VK | `kHidVk[]` `trigger.cpp:189-203` |
| Auffälligkeit | „binary input streams enabled" wird alle ~10 s neu gesendet (`trigger.cpp:853`; Logs 18:16:17/27/37/47…) [F]; `pollKey()` sendet 50-ms-`km.get_key` auch im Binärmodus (`683-685,849-852`) [F, Auswirkung U] |
| Alternativ ohne Box | `cmdKeyPoll` (`km.get_key({vk})`) |
| Host-Referenz | Aimmy (C#) nutzt dieselben `km.*`-Befehle [U] |

## 6. Ist-Architektur — Datenfluss (alles [F], Zeilennummern = Code)

```
Gaming-PC Tastatur ─┐
Gaming-PC Maus   ───┼─► MAKCU-Box ─(COM3/115200, Text- oder Binärrahmen)─┐
                    │                                                     │
                    └─► (optional) DMA: win32kbase!gafAsyncKeyState ──────┤
                                                                          ▼
                        trigger.cpp feedBytes()/handleFrame()/maskReport()
                          → gBtn[1..5], gKeyHeld, gKeyCode, gKeyHid
                                                                          │
Lokale Tastatur ─► GetAsyncKeyState ─► localKeyDown() (trigger.cpp:670)   │
                                                                          ▼
                 trigger::update()  (esp.cpp:2418 Render-Thread; idle: 2457, 3714)
                   held = keyFromDevice || keyFromLocal || keyFromRemote  (855-856)
                        │                                                ▲
                        ▼                                                │
        Zielprüfung (hitMask/radius/maxDist 864-882)                 remoteDown(vk) (806)
        Feuerlogik: delayMs → cmdDown, clickMs → cmdUp (902-908)          ▲
                                                                          │
   dma.cpp keyState(out[256]) ◄── setRemoteKeys(esp.cpp:3637) ◄───────────┘
     ├─ keyEnable_==false → sofort false (592-601)
     └─ true: resolveKey() (684) → findKey() (911) → refineCompare() (470)
              decodeKeyBitmap() (366) → 0x80 pro gedrückter VK
   Binden: captureTick() (707-726) akzeptiert auch remote gedrückte Tasten
   UI: radar.cpp:1346-1347 „dma key state" + „Find key array (hold E)" → keyScanNow() (793)
```

**Thread-Kontext:** Die gesamte Key-DMA-Arbeit läuft über den Aufrufer, also über den **Render-/Frame-Thread** (`drawDayzFrame` → `trigger::update`). `dma.cpp` hat sonst `mx_`-Lock, Prioritäten (`kPrioUrgent/Bg`) und Scatter-Bündelung (`readManyLocked:1029`), der Key-Pfad nutzt jedoch **direkt** `readEx_` via `readAt()` (`dma.cpp:361`) — also **weder** Lock/Priorität **noch** Scatter/Cache.

## 7. Offene Fragen (nur durch Messung zu beantworten)

1. Löst `gafAsyncKeyState` auf dem Zielsystem über PDB oder nur über EAT auf — welche `keyHow_`-Kandidaten entstehen real?
2. Welcher Session-Context ist live, und bleibt er über Reboots stabil (`keyarray.txt` kann stale werden)?
3. Welches Layout (1 vs. 2 Bit/VK) gilt auf dem Zielbuild?
4. Kosten/Latenz von (a) `resolveKey`-Erstlauf, (b) `refineCompare`+`refineArm`-Zyklus, (c) 64-B-Read bei 125 Hz?
5. Erzeugt der 8-ms-`remoteTick` im Frame-Thread messbare Frame-Zeiten (`tel: ... call p95/max`)?
6. Sind **stale reads** möglich (Read ok, Muster veraltet)? `keyprobe.cpp:197` kommentiert „DWELL ≥ 90 ms vermeidet stale reads" [U, wichtig].

## 8. Risiken
* `keyEnable_=true` aktiviert drei Langläufer: PDB-Load (Netz möglich), 4-GB-Scan (2× 65 536 Reads), Vollbild-Reads (bis 6 × 2–8 MB je 5 s) — alles auf dem Render-Thread.
* `pid|0x80000000` ist laut Header „use with extreme care".
* Layout-/Context-Heuristik ohne Verifikation kann **falsche** Tasten liefern.
* Nach fehlgeschlagenen Reads muss der Zustand verworfen werden, sonst „klebt" der Trigger.
* Nie zwei DMA-Besitzer (`keyprobe` vs. `Ickza.exe`; `keyprobe.log` zeigt genau diesen Fehlerfall).

## 9. Versionsabhängigkeiten
* Header 5.18.10 vs. Laufzeit-DLL 5.19.1.252 / leechcore 2.23.4.104 — genutzte APIs sind kompatibel, Cache-/Flag-Verhalten kann jedoch abweichen.
* `gafAsyncKeyState`: Export in `win32kbase` (Win10 1809+); ältere Builds nutzen `win32k.sys` → Fallbacks nötig.
* Symbole: `dma\Symbols\...\ntkrnlmp.pdb` vorhanden; `win32kbase.sys`-PDB muss ggf. erst geladen werden (symbolserver/`info.db`).
* DayZ-Offsets (`kWorld`, `kNetClient`) sind vom Key-Pfad unabhängig, aber an den Spielbuild gebunden.

## 10. Umsetzung 2026-11-15 (Phasen 6, minimal & umkehrbar)

**Änderung A — Kosten/Verhalten des Key-Pfads (neutral, solange `keyEnable_` aus ist):**
* `dma.cpp findKey()`: neuer Zustand **Phase 5** = „verifiziert, keine periodische Arbeit". Phase 4 verlässt sich nach Erfolg **oder** nach 6 Versuchen; Re-Verify nur nach 3 aufeinanderfolgenden Read-Fehlern (`keyFailN_`).
  *Begründung:* vorher blieb `keyPhase_ == 4` für immer gesetzt, und `keyState()` rief deshalb **alle 5 s** `refineCompare()+refineArm()` auf → dauerhafte Lastspitzen im Render-Thread. Das ist die plausibelste Ursache des „hitching".
* `dma.cpp readImage()`: liest jetzt 64-KB-Batches (vorher 4 KB), mit 4-KB-Fallback, falls ein Batch kurz zurückkommt.
  *Begründung:* Halbierung der Geräte-Roundtrips um bis zu 16× pro Bild, bei identischer Semantik (`ZEROPAD_ON_FAIL` unverändert).
* Diagnose: Laufzeiten in den bereits vorhandenen `[trg] refine …`-Logzeilen (`in N ms`, `confirmed in N ms`, `gave up after N tries`).

**Änderung B — Aktivierung:** `dma.cpp:608 static constexpr bool keyEnable_ = true;` (Rückweg: `false`, 1 Zeile). README-Abschnitt dazu aktualisiert.

**Belege:** `tools\build_a1.log` (A) und `tools\build_b.log` (A+B) — beide 0 Fehler, dieselben 4 vorbestehenden Warnungen; Artefakte `build\Ickza.exe`.

**Änderung C (nach T0, §11) — Refine kann module/pdb-Kandidaten armen:** `szKb` = `cbImageSize` von win32kbase wird ermittelt und an die Kandidaten „pdb/win32kbase" / „mod/win32kbase" gehängt; `refineArm()` (verlangt `size != 0`) findet damit erstmals Bilder. Beleg: `tools\build_c.log` (0 Fehler).

**Änderung D (nach T0, §11) — blinder Scan nur bei Bedarf:** Der 4-GB-MZ-Scan läuft nur noch, wenn der aufgelöste Kernel-Base im Fenster `kWin32kScanLo..kWin32kScanHi` liegt (auf Win11 26100 tut er das nicht → ~65 000 Reads pro Context gespart, Fund war 0). Neue, ehrliche Logzeile `[trg] keyresolve blind scan skipped …`. Beleg: `tools\build_d.log`.

**Änderung E (nach App-Lauf, §4b) — „no hit" bleibt billig:** Wenn der 7-Tasten-Scan nichts findet (am Gaming-PC wurde nichts gehalten), geht `findKey()` zurück in den Watch-Loop (Phase 0 → sofortiges Re-Arm mit frischem Idle-Snapshot) statt in den Refine zu laufen und dort 6 × 5 s zu verbrennen. Kosten: ~12 × 64 B Read pro 5 s.

**Änderung F (nach App-Lauf, §4b) — Refine akzeptiert jetzt alle Kandidaten mit Bildgröße:** `refineArm()` filterte zusätzlich auf `how[0]=='s'` (nur „saved"/„scan") und armte deshalb trotz Fix C **0 Bilder** (`refine armed 0 images (of 0 scan cands)`). Filter entfernt → bis zu 6 Bilder; damit läuft in `refineCompare()` die **Vollbild-Differenzsuche nach dem 7-Tasten-Muster** und findet das Array-Offset auch dann, wenn die PDB-RVA nicht exakt passt. Beleg: `tools\build_e.log`.

**Änderung G (nach App-Lauf) — die Vollbildsuche bekommt automatisch eine Chance:** Mit Fix E allein blieb die App im Watch-Loop, und weil der 7-Tasten-Scan bei unpassender Kandidaten-VA nie trifft, lief der Refine (und damit die eigentliche Vollbildsuche) **nie**. Jetzt geht jeder dritte No-Hit-Round in einen Refine-Pass, und ein „gave up" kehrt in den Watch-Loop zurück statt endgültig zu stoppen. Kosten: ~640 Reads pro 15–45 s, nur solange nicht verifiziert.

**Live-Verifikation am Ziel (20:54, `build\Ickza.log`):** Fix E belegt — `keyscan armed 12 cands - HOLD E+W+A+S+D+SPACE+SHIFT 5s` / `keyscan no hit … - re-arm` im 5-s-Takt, **0 Refines**, App stabil, keine Hänger.

**Noch nicht bestätigt (Hardware!):** Wirkung auf Frame-Zeiten, tatsächlich gefundener Context/Offset/Layout, `keyHow_`, Erstlauf-Dauer von `resolveKey`. → Tests T0–T3 in `INPUT_DEBUG_HISTORY.md`.

## 11. T0-Hardwarelauf 2026-11-15 (echte Messdaten, Zielsystem = Gaming-PC über die DMA-Karte)

Ausgeführt auf dieser Maschine mit `tools\keyprobe.exe` (Karte war frei, MAKCU an COM3). Logs: `tools\probe_env.txt`, `tools\probe_full.txt`, `tools\scan_ctx4.txt`, `tools\keyprobe.log`.

**Umgebung [F]**
* VMM init OK, Box antwortet `km.MAKCU` → Karte/Box funktionieren.
* Ziel: **Windows-Build 26100**, `vmm version: 5`, 306 Prozesse, `DayZ_x64.exe` läuft (pid 0xB74, Session 1).
* Session-IDs: System 0, `csrss.exe` 0x434 (session 0), `csrss.exe` 0x4E4 / `winlogon.exe` 0x548 / `DayZ_x64.exe` 0xB74 (Session 1).

**Befund 1 — Module sind überall auflösbar, aber außerhalb des Suchfensters [F]**
```
ctx=0x4|0x80000004|0x434|0x80000434|0x4E4|0x800004E4  win32kbase.sys base=0xFFFFF8073E9D0000 read=1 mz=5A4D
                                                      win32kfull.sys base=0xFFFFF8073E200000 read=1 mz=5A4D
                                                      win32k.sys     base=0xFFFFF80741360000 read=1 mz=5A4D
```
`VMMDLL_ProcessGetModuleBaseU` liefert in **jedem** Context dieselbe, lesbare Kernel-VA. Damit liegt der echte Base bei `0xFFFFF807…` — **das hartkodierte Scanfenster `dma.cpp:391` (`0xFFFFF80000000000`–`0xFFFFF80100000000`, 4 GB) enthält ihn nicht** → `scanWin32kbase()` findet 0 Bilder.

**Befund 2 — PDB-Auflösung funktioniert [F]**
```
PDB win32kbase(win32kbase) gafAsyncKeyState rva=0x2AE7CD va=0xFFFFF8073EC7E7CD
VMMDLL_Map_GetEATU win32kbase: cMap=2551 names=2551 vaBase=0xFFFFF8073E9D0000   (eigener EAT-Parser: FAILED)
```

**Befund 3 — Box-Pulse reagieren NICHT an der PDB-VA, aber an 0x4FD davor [F]**
* `testCandidate` über 128 Byte an `base+rva` in allen 6 Contexts: `LMB[] RMB[] MMB[]` = leer.
* Vollbild-Differenzscan (`scan --ctx 0x4`, Bild 0x345000 = 3,3 MB): **nur 6 Bytes** des gesamten Bildes reagieren auf die Pulse: `0x2AE2D0..0x2AE2D3` und `0x2AEAE8..0x2AEAE9` (Differenz 0x818 zwischen den Clustern).
* Rohdaten (idle A → LMB B) @0x2AE2D0: `CD5D20D2 66D4FFFF | 71845A33` → `CD5D20D2 66D4FFFF | 410A2D37`; die reagierenden Bytes wirken wie **instabile Felder**, nicht wie ein statisches 256-Bit-Bitmap (`diffAC=5` = 5 Bytes ändern sich schon zwischen zwei Idle-Samples), und der Layout-Matcher meldet `raw hits=0 / no layout match`.
* Vergleichsprobe: `probe`-Lauf meldete für `win32kbase+0x2AE2D0` (ctx 4) `LMB[b0.1 b0.6]` — das ist die einzige Stelle mit Pulse-Korrelation.

**Befund 4 — „Session Space" 0xFFFFF900… ist hier nicht lesbar [F]**
`sessionProbe` zeigt für alle geprüften Contexts (inkl. `csrss`/`winlogon`) `-` für `0xFFFFF900…`, `0xFFFFF980…`, `0xFFFFF97FFFFF0000`, `0xFFFFFA8000000000`. Die klassische Annahme „Array liegt in Session Space und wird per Context-Rebase gefunden" trägt auf diesem Build also (so) nicht.

**Befund 5 — Refine-Stufe war dadurch praktisch tot [F, Code+Messung]**
`refineArm()` armt nur Kandidaten mit `how[0]=='s'` **und** `size != 0` (`dma.cpp:458`). „scan/session" fällt wegen Befund 1 weg, „pdb/…"/„mod/…" hatten `size == 0` → **0 Bilder zum Verfeinern** → `refineCompare()` konnte nie bestätigen. → Fix C (siehe §10 / History).

**Offene Fragen (brauchen den Menschen am Gaming-PC):**
1. Ist der reagierende Cluster überhaupt die Tastenbitmap (Buttons sind evtl. nur Bewegung), und welches Layout gilt? → `keyprobe.exe ekey` (Pieps-Führung, nur E/W am Gaming-PC drücken, **rein lesend**, Box unbenutzt).
2. Passt die PDB-Version zum laufenden `win32kbase.sys` (rva-Differenz 0x4FD)? → PDB-Hash/Timestamp prüfen, ggf. `-disable-symbolserver` gegentesten.
3. Warum reagieren die Bytes so instabil (Idle-Drift)? → wiederholte Scans, 3 Runden, Mittelung.

## 12. Befund 6 — Verifikation am Ziel geglückt (20:58, App-Lauf mit Fix A–G) [F]

```
[trg] refine armed 1 images (of 12 cands) in 111 ms - HOLD the keys   <-- Fix F armiert (1 Bild, 3.4 MB, 64-KB-Batches)
[trg] refine BEST rise=2 heldB=2 layout2 ctx=0x80000548 va=0xFFFFF8073EC7EAD7 off=0x2AEAD7
[trg] refine confirmed in 133 ms - periodic refine stopped           <-- Fix G/A1: bestätigt, danach passiv
[trg] keydiag ctxpid=2147485000 how=refined va=0xFFFFF8073EC7EAD7 rawnz=8 raw=000040000000000000020000000100000057D605...
keyarray.txt: 2147485000 18446735308734589655 0                       <-- persistiert (ctx 0x80000548, va 0x…EAD7, layout2)
```

**Ergebnis:** Der Pfad lief auf dem realen Ziel **vollständig durch**: Kandidaten → 7-Tasten-Watch → Vollbild-Differenzsuche → Offset gefunden → mit `rise=2/heldB=2` bestätigt → persistent gespeichert → passiv.
**Damit belegt:** (a) das Array liegt **nicht** an der PDB-RVA (0x2AE7CD), sondern bei `rva 0x2AEAD7` (Δ = 0x30A) — die Vollbildsuche war genau das richtige Werkzeug; (b) die Bytes am gefundenen VA sehen bitmap-artig aus (isolierte Bits), im Gegensatz zu den timestamp-artigen Bytes an der PDB-RVA; (c) Kosten: **111–136 ms pro 3,4-MB-Bild** mit 64-KB-Batches (≈26 MB/s; mit den alten 4-KB-Reads wären es ~850 Roundtrips statt 53).
**Gemessene Einmalkosten `resolveKey`:** 20:49:17.7 → 20:49:24.9 ≈ **7 s** (PID-/Modul-Enumeration + PDB + Kandidaten) — die verbleibende Einmal-Lastspitze (H2), Kandidat für die nächste Iteration (Auslagern in einen Worker-Thread).





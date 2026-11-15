# INPUT_DIAGNOSIS — Referenzabgleich (memflow-win32), Bereichstrennung, Testplan

**Stand:** 2026-11-15 · Diagnosebericht **vor** jeder weiteren Codeänderung · manuelle E-Sync-Tests sind gestoppt
**Referenz:** `memflow-win32` `Win32Keyboard` — https://docs.rs/memflow-win32/latest/memflow_win32/win32/keyboard/ · Quelltext `memflow-win32/src/win32/keyboard.rs` (master)

> Status ehrlich: **Speicherzugriff ja, Tasten nein** — „Offset-Erkennung ist NICHT verifiziert."

## 0. Drei getrennte Fehlerbereiche (Auftrag)

| # | Bereich | Status | Beleglage |
|---|---|---|---|
| 1 | Speicherzugriff / Datenvalidität | **teilweise**: Reads laufen, Zieladresse aber ungeprüft falsch verdrahtet | Reads fehlerfrei; reagierende Bytes = Zähler/Zeitstempel (Beweise A/B/C) |
| 2 | Dekodierung einzelner Tastenzustände | **NICHT verifiziert** — Formel deckt sich mit der Referenz, nie an einem reagierenden Byte bestätigt | memflow-Formel = unsere Formel (§2); nie ein „2 Werte / 4+ Wechsel"-Byte gefunden |
| 3 | VK-/HID-Zuordnung + Bind-Anzeige | **FEHLER** (eigenständig, präexistent) | `PAUSE (0x45)` (§4.4), Zufalls-Binds durch unverifizierte Remote-Bits (§4.5) |

## 1. Was die Referenz dokumentiert (Zitate)

Modulkopf von `keyboard.rs`:
* „The `gafAsyncKeyState` array contains the current Keyboard state on Windows targets."
* „Although the `gafAsyncKeyState` array is exported by the `win32kbase.sys` kernel module **it is only properly mapped into user mode processes**. Therefor the Keyboard will by default find the `winlogon.exe` or `wininit.exe` process and use it as a proxy to read the data."
* Beispiel nutzt einen **User-Mode-Prozess** als Proxy (kein Kernel-Flag).

Konstanten im Quelltext (mit Versionszuordnung dokumentiert):
```
SIG_GAF_WIN10         = "48 8B 05 $ { ' } 48 89 81 ? ? 00 00 48 8B 8F"  // win32kbase.sys -> RVA gafAsyncKeyState
SIG_GSGS_WIN11_23H2   = "48 8B 05 $ { ' } 48 8B 04 C8 48 83 C4 ? C3"  // win32ksgd.sys  -> gSessionGlobalSlots
SIG_GSGS_WIN11_24H2   = "48 8B 05 $ { ' } FF C9 48 8B 04 C8 C3"       // win32k.sys     -> gSessionGlobalSlots
SIG_KEYOFF_WIN11_24H2 = "B9 00 80 FF FF ? 22 B4 ? u4 41"              // Offset in _GetAsyncKeyState (z. B. 0x37F8)
SIG_KEYOFF_WIN11_25H2 = "40 0F B6 ? 48 C1 E9 02 8A 94 08 u4"          // Offset in _GetAsyncKeyState (z. B. 0x37F0)
```
* Signatur werden per **PE-Code-Scan** gesucht; ein Treffer muss **genau einmal** auftreten, sonst harter Fehler. Kommentar: *„Ambiguous (0 or >1 matches) is a hard error … rather than a silently-wrong keyboard buffer read."*
* Layout im Code: `byte = vk * 2 / 8`, down-Bit `1 << ((vk % 4) * 2)`, Puffer `[u8; 256 * 2 / 8]` = **64 Byte**.

## 2. Abgleich mit unserem Projekt

| Aspekt | Referenz (dokumentiert) | Unser Code | Bewertung |
|---|---|---|---|
| Datenlayout | 2 Bit/VK, 64 B, down-Bit `1<<((vk%4)*2)` | `dma.cpp decodeKeyBitmap()` (oneBit=false) | **deckungsgleich** |
| Proxy-Prozess | `winlogon`/`wininit` als **User-Mode**-Proxy | Kandidaten enthalten `pid\|0x80000000` **und** plain pid (`resolveKey`); gewählt wurde `0x80000548` (kernel-flagged) | teils abweichend |
| Fundort/Locator | **Code-Signaturen je Build**: Win10 win32kbase; 23H2 win32ksgd; **24H2+ win32k.sys + `_GetAsyncKeyState`-Offset** | **PDB-Symbol** `gafAsyncKeyState` + `ProcessGetModuleBaseU` + Rebase; Fallback-Kette `+0x3110/+0x3690` | **veraltet**; PDB liefert *immer* eine VA – auch bei Build-Mismatch |
| Fehlerverhalten | 0/mehrdeutig = harter Fehler | PDB-Treffer = „Adresse gefunden" | erklärt die **stille** Fehladresse |

## 3. Ursachen (belegt)

* **U1 (Hauptursache):** Ziel ist **Win11 Build 26100 (24H2)**. Auf 24H2 liegt die Live-Tastaturstruktur nicht am bisher genutzten win32kbase-Pfad, sondern hinter `gSessionGlobalSlots` + Code-Offset. Was wir im win32kbase-Bild sahen (ms-Zähler, „last input"-Feld), war **Nachbarschaft**, nicht das Array.
* **U2:** Das Prüfkriterium hatte keine Idle-Stabilität/Exklusivität → ein Zähler wurde als Bitmap „bestätigt" (Beweise A/B). Idle-Gate ist jetzt eingebaut, greift aber am falschen Ort.
* **U3:** Die manuellen E-Drücke waren **nie** ein Timing-Problem, sondern ein **Adress**-Problem → Methode nicht wiederholen.
* **U4:** `PAUSE (0x45)` = eigener Mapping-Bug (unten).
* **U5:** `captureTick()` bindet unverifizierte Remote-Bits.

### 4.4 `PAUSE (0x45)` — eigenständiger Mapping-Fehler
* Dokumentierte VK-Referenz: `VK_E = 0x45` (Taste **E**), `VK_PAUSE = 0x13` (memflow `vkey.rs`: `VK_PAUSE = VKEY(19)`).
* Unser Fehler: `radar.cpp:1206` `MapVirtualKeyA(k, MAPVK_VK_TO_CHAR)` liefert den **Zeichencode** (0x45 = 'E'), wird aber als **Scancode** in `GetKeyNameTextA(sc << 16)` gesteckt → Scancode 0x45 wird als **„Pause"** benannt. Die Hex-Anzeige (`0x45`) ist korrekt.
* Folge: für Buchstaben wird immer der falsche Name angezeigt. **Offline testbar, DMA-unabhängig.**

### 4.5 Bind-Regression
Vorher (`keyEnable_=false`) war `remoteDown` immer false → Bind kam ausschließlich von Box/lokal. Jetzt bindet `captureTick()` (trigger.cpp:718-724) den ersten „steigenden" Zufalls-VK; `justBound()` hängt an der UI. → Remote darf erst binden, wenn die Struktur belegt ist (P3).

## 5. Reproduzierbarer Testplan **ohne menschliches Timing**

Kern: **Die Box erzeugt die Referenzereignisse** — LMB/RMB/MMB sind VKs **1/2/4 im selben Array** und exakt zeitsteuerbar. Der Mensch wird nur einmal gebraucht (T4) und auch dort timingfrei.

| Test | Ziel | Vorgehen (automatisch protokolliert) | Hartes Kriterium |
|---|---|---|---|
| **T1** | Struktur/Adresse finden | Box toggelt LMB 5×, dann RMB, dann MMB; je Phase Fenster abtasten, Wechsel zählen | Byte mit **genau 2 Werten**, **≥4 Wechsel**, kehrt zurück; Distanzen VK1/2/4 (/4, /2, kein Abstand) identifizieren Layout + Base |
| **T2** | Kandidatenmatrix | T1 für (a) `win32kbase`, (b) `win32k.sys`, (c) `win32ksgd.sys`; je Proxy `winlogon` (user-mode) **und** `winlogon\|0x80000000` | genau **ein** (Modul, Ctx) zeigt das Muster |
| **T3** | Datenvalidität | 2 Idle-Reads identisch? 3× reproduzierbar? nur ≤2 Bytes betroffen? Lesefehlerrate | 0 Abweichung im Idle, 3/3 reproduzierbar |
| **T4** | E-Bestätigung (timingfrei) | 1 Signal, dann E mehrfach tippen (10 s Fenster) | Toggle-Muster; Zuordnung über Geometrie zu T1 (2Bit: E-Byte = VK1-Byte + 17) |
| **T5** | Mapping offline (kein DMA) | VK→Anzeigename/HID gegen dokumentierte Listen; PAUSE als Testfall | E→„E", 0x13→„Pause", Tab/Shift korrekt |

**Warum das ohne Sync funktioniert:** Nicht die *Zeit* wird korreliert, sondern das *Muster* (2 Werte + viele Wechsel) und die *Geometrie* (Byte-Abstände der VKs). Ein Zähler/Zeitstempel fällt durch das Muster, ein falscher Offset durch die Geometrie.

## 6. Dokumentierte Alternative (falls DMA unzuverlässig bleibt)
* **MAKCU/Box:** binärer `0x53`-Stream, `kind 2` = HID-Usage (`handleFrame`, trigger.cpp:232-255) sowie `km.get_key(vk)` — **kein DMA, kein Offset, kein Timing**; funktioniert, sobald die Tastatur durch die Box läuft.
* **memflow** selbst (Rust) als unabhängige Referenzimplementierung zum Gegenprüfen derselben Maschine.

## 7. Reparaturplan (src/ erst nach Freigabe)

| # | Änderung | Abhängig von | Test | Rückweg |
|---|---|---|---|---|
| **P1** | Anzeige/Mapping: `MAPVK_VK_TO_VSC` (+Extended-Bit) statt `MAPVK_VK_TO_CHAR` | nichts (offline, risikolos) | T5 | 1 Zeile |
| **P2** | Locator: Signaturweg wie dokumentiert (Win11 24H2: `gSessionGlobalSlots` in `win32k.sys` + `_GetAsyncKeyState`-Offset), alten win32ksgd-Fallback sauber dokumentieren | T1/T2 | T1–T3 | Block; `keyEnable_=false` |
| **P3** | Bind-Sperre: Remote bindet erst nach belegter Struktur (bis dahin Box+local wie früher) | P2 | T1/T4 + Bind | Bedingung zurück |
| **P4** | Verifikation (Idle-Stabilität + Exklusivität + 2 Runden) beibehalten | – | T1/T3 | Gate zurück |

## 8. Ausdrücklich **nicht** verifiziert
* Adresse/Offset — „Offset-Erkennung gilt NICHT als funktional verifiziert"
* dass ein Tastendruck überhaupt ein Byte im gelesenen Bereich ändert
* Layout: nur **referenz-deckungsgleich**, **nicht** live bestätigt
* Live-Zuordnung VK/Funktion (nur die Formel stimmt mit der Referenz überein)
* keine Erfolgsaussage ohne T1–T4.



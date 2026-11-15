# PROMPT FÜR CHATGPT — PCIe/DMA-Tastaturerkennung (Ziel: Win11 24H2, Build 26100)

*Dieser Text ist die Vorlage für die Browser-Seite der LAN-Bridge. Bitte komplett kopieren.*

---

## 1. Setup (fix, nicht verhandelbar)

* Zwei PCs. **Gaming-PC**: DayZ, Windows 11 **Build 26100 (24H2)**. **Laptop**: unsere Software, hängt per USB an einer
  PCIe-FPGA-DMA-Karte (FTDI FT601/FTD3XX) im Gaming-PC. Übertragen wird ausschließlich gelesen.
* Stack: **MemProcFS `vmm.dll` 5.19.1.252**, **LeechCore `leechcore.dll` 2.23.4.104**, Header `vmmdll.h` 5.18.10.
  Genutzte APIs: `VMMDLL_MemReadEx`, `VMMDLL_ProcessGetModuleBaseU`, `VMMDLL_PidGetFromName`, `VMMDLL_PidList`,
  `VMMDLL_Map_GetModuleFromNameU`, `VMMDLL_PdbLoad`, `VMMDLL_PdbSymbolAddress`, `VMMDLL_MemVirt2Phys`.
  Flags bisher immer `VMMDLL_FLAG_NOCACHE`; PID manchmal `pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY (0x80000000)`.
* Als Referenz diente uns **memflow-win32 `Win32Keyboard`** (`memflow-win32/src/win32/keyboard.rs`). Dessen Doku/Kommentare
  sagen: `gafAsyncKeyState`, 2 Bit/VK, 64 Byte, Proxy `winlogon/wininit`, und ab 24H2: `gSessionGlobalSlots` liegt in
  `win32k.sys`, Array = „find our session's gSessionGlobalSlot, **dereference the pointer three times**, and add the
  **0x3690** hardcoded offset", Key-Test `*(vk>>2 + *SGDGetSessionState() + 0x3690) & (1 << (2*(vk&3)))`, Index `session-1`.
* Eingaben am Gaming-PC erzeugen wir derzeit automatisch mit einer **MAKCU/kmbox-Box** (`km.left(1)`, `km.right(1)`,
  `km.middle(1)`), also *Maus*-Buttons. Zusätzlich kann ein Mensch Tasten am Gaming-PC tippen.

## 2. Ziel

Trigger-Taste soll **auf dem Gaming-PC** erkannt werden, indem dessen Tastaturzustand per DMA gelesen wird. Kein
Schreibzugriff. Später soll das ohne Mensch und ohne Timing auskommen (Muster-/Geometrie-Erkennung).

## 3. Was funktioniert (verifiziert)

1. DMA-Kanal: Reads laufen fehlerfrei (ganze Modulbilder à 3,4 MB lesbar).
2. `VMMDLL_ProcessGetModuleBaseU` liefert `win32kbase.sys`, `win32kfull.sys`, `win32k.sys` in **allen** Contexts
   (z. B. `win32kbase.sys base=0xFFFFF8073E9D0000`, `MZ` gültig) — auch mit unserm 24H2-Scanfenster, das aber
   0xFFFFF80000000000..0xFFFFF80100000000 ist und die echten Bases (`0xFFFFF807…`) **nicht** enthält.
3. PDB-Auflösung: `VMMDLL_PdbLoad` + `VMMDLL_PdbSymbolAddress` liefern `gafAsyncKeyState rva=0x2AE7CD`
   (VA 0xFFFFF8073EC7E7CD) im Zielbuild 26100.
4. Signatur `48 8B 05 ?? ?? ?? ?? FF C9 48 8B 04 C8 C3` (= `gSessionGlobalSlots`-Zugriff) trifft in **`win32k.sys`**
   eindeutig: site-rva 0x16118, disp 0x71BC1 → Ziel-VA **0xFFFFF807413E7CE0**. Der RIP-Displacement wird korrekt
   gerechnet (site+7+disp).
5. Von dort lesbare Kette (ctx=System pid 4 bzw. csrss Session 1):
   `*(gsgs)=0xFFFFAE02C79FCFB0` → `*(*gsgs)=0xFFFFAE02CAE852D0` → `*(*(gsgs)+i*8)` usw. — alles lesbare Pool-Adressen.
6. Offline-Test der Anzeige-Namensauflösung (eigener Trigger-Key) ist korrigiert und 12/12 grün.

## 4. Was NICHT funktioniert (Kernproblem)

**Wir haben bis heute kein einziges Byte gefunden, das sich bei einer Eingabe wie ein Tastenbitmap verhält.**

Konkret gemessen:

* Am PDB-VA `0xFFFFF8073EC7E7CD` (win32kbase) liegt **kein** Bitmap: die 64 Byte dort sind praktisch leer, aber es
  gibt in der Nachbarschaft Felder, die sich wie **Zähler/Zeitstempel** verhalten (LE32 `0x0006DCAD → 0x0006DCBF`
  = +1/ms; ein zweites 4-Byte-Feld springt bei jedem Box-Puls um ~3e8 und **kommt nicht zurück**).
* Vollbild-Differenzsuche über `win32kbase.sys` (3,4 MB) und `win32kfull.sys` (4,4 MB): **kein** Byte mit
  „genau 2 Werten + vielen Wechseln + Rückkehr" (Kriterium: getipptes Byte). Nur Zähler/Zeitfelder reagieren.
* Fenster-Burst an den Ketten-Kandidaten (`*(*(gsgs))+0x3690 = 0xFFFFAE02CAE88960` usw.): Fenster ist idle-stabil,
  reagiert aber **nicht** auf `km.left/right/middle`-Pulse. `*(*(gsgs))+0x3690` war 0/64 Bytes ≠ 0 (leer),
  `*(gsgs[0][0])+0x3690 = 0xFFFFAE02D2E8C690` war 64/64 ≠ 0 (komplett belegt — eher Datenfeld, kein Bitmap).
* Ein Blatt **Maus-Buttons als Referenz** scheint grundsätzlich fraglich: es ist unklar, ob `VK_LBUTTON/RBUTTON/MBUTTON`
  überhaupt in derselben Struktur wie die Tastatur-VKs liegen (bei uns hat *nie* ein Puls ein sauberes Bit gesetzt).
## 5. Was wurde probiert — und warum ist es gescheitert (nicht wiederholen)

| # | Ansatz | Ergebnis / warum gescheitert |
|---|---|---|
| 1 | PDB-Symbol `gafAsyncKeyState` (win32kbase) + 64-B-Window, 2 Bit/VK | Adresse stimmt (Symbol), aber dort liegt kein reagierendes Bitmap — nur Nachbarfelder |
| 2 | Blinder MZ-Scan über 4 GB (64-KB-Schritte) im Fenster `0xFFFFF800..0xFFFFF801` | findet 0 Bilder, weil die echten Bases bei `0xFFFFF807…` liegen |
| 3 | 7-Tasten-Scan über Kandidaten + „Diff-Refine" über das ganze Bild | hat zugestimmt, weil ein **ms-Zähler** das Kriterium erfüllt (False Positive). Danach: Idle-Stabilitäts-Gate eingebaut (2 Idle-Reads, instabile Bytes ausgeschlossen) — dadurch kein Fehlalarm mehr, aber auch **kein** Treffer |
| 4 | `win32ksgd.sys`-Kette `+0x3110 → ptr → ptr → +0x3690` (unser Alt-Code) | auf 26100 existiert `win32ksgd.sys` nicht mehr (kein Modul); `win32k.sys+0x3110` liefert 0x12F83E828EC8348 (kein Kernelzeiger) |
| 5 | `gSessionGlobalSlots`-Sig + 1/2/3 Derefs + `+0x3690` (Index `session`, `session-1`, ctx 4/Proxy/csrss) | Sig trifft, Kette ist lesbar, aber kein Kandidat reagiert auf Maus-Pulse |
| 6 | Offset aus `_GetAsyncKeyState` per memflow-Sig (`B9 00 80 FF FF ? 22 B4 ? u4 41` bzw. `40 0F B6 ? 48 C1 E9 02 8A 94 08 u4`) in `win32kfull/win32k/win32kbase` | **kein Treffer** — also Offset-/Modul-Annahme für 26100 falsch oder Pattern falsch |
| 7 | Mensch tippt E/Space, Muster „2 Werte + viele Wechsel" | kein Treffer, aber bisher nur an Adressen getestet, die selbst unbestätigt sind |

## 6. Wo genau wir technisch hängen

1. **Fundort unbekannt**: Wir haben weder über PDB noch über Signatur eine Adresse, an der ein Tastendruck ein Bit setzt.
2. **Testvalidität**: Unsere automatischen Referenzereignisse sind Maus-Buttons — unklar, ob die im selben Bitmap landen.
3. **Offset-Versionierung**: `+0x3690` stammt aus älteren Builds; memflow liest den Offset *per Signatur aus dem Code*.
   Für 26100 haben wir den Offset nicht.
4. **Session-/Context-Semantik**: unklar, welcher Prozess (winlogon/dwm/explorer/csrss/system), welcher Session-Index und
   welches PID-Flag bei `VMMDLL_MemReadEx` richtig ist, um *session space* der interaktiven Session zu sehen.

## 7. Konkrete Fragen an ChatGPT

1. Wie lautet auf **Win11 24H2 / Build 26100** der *richtige* Weg, das Keyboard-Async-Bitmap von außen (MemProcFS/vmmdll)
   zu finden — und stimmt die Kette `gSessionGlobalSlots → 3 Derefs → +0x3690` so, wie wir sie lesen (Array = RIP-Ziel,
   Index `sessionId-1`, +0x3690)? Welche **genauen Offsets/Signaturen** gelten für 26100?
2. Enthält das Bitmap auf 24H2 überhaupt `VK_LBUTTON/RBUTTON/MBUTTON`, oder nur Tastatur-VKs? Ist der Maus-Puls-Test
   damit aussagekräftig oder irreführend?
3. In welchem Modul liegt `_GetAsyncKeyState` auf 26100 (win32kfull/win32kbase/win32k) und wie lautet das korrekte
   Byte-Pattern für den Array-Offset (`0x37F8/0x3800/…`)? Oder: wie leitet man den Offset robust ab?
4. Ist ein **komplett leeres** 64-Byte-Fenster an der Kettenadresse (0/64 ≠ 0) plausibel (nichts gedrückt) oder
   zeigt es einen falschen Pointer? Wonach soll man im Speicher suchen, um den richtigen Ort zu bestätigen?
5. Welcher Prozess ist auf 26100 der richtige Proxy und mit welchem Flag (plain `pid` vs. `pid|0x80000000`)? Was genau
   macht `VMMDLL_PID_PROCESS_WITH_KERNELMEMORY` bei Session-Space-Reads — und wie liest man Session-Space (`0xFFFFF9…`,
   bei uns in allen Contexts „nicht lesbar") korrekt mit vmmdll?
6. Wie injizieren wir mit **MAKCU/kmbox** automatisch eine **Tastatur**-Taste (exakter Befehl), damit wir Tastatur-
   Referenzereignisse ohne Mensch haben? Alternativ: bekommt man über die Box die HID-Reports der physischen Tastatur?
7. Welche **minimal-invasive** Validierungsprozedur empfiehlst du, die ohne menschliches Timing auskommt und
   eindeutig „richtige Adresse + richtiges Layout" beweist (Kriterium/Idee)?
8. Falls der externe Weg auf 26100 unzuverlässig bleibt: welche *dokumentierte* Alternative auf dem Gaming-PC
   (z. B. Box-Tastatur-Stream, `km.get_key`, MemProcFS-Kernelmodul) würdest du nehmen?

## 8. Was wir liefern können

Read-only Experimente auf zwei echten PCs (beide online), Logs zu jedem Test, und wir setzen Vorschläge sofort um und
reporten die Messwerte zurück. Bitte antworte so, dass wir es **ohne Trial-and-Error** nachvollziehen können:
konkrete Adressen/Signaturen/Offsets für 26100, die richtige Context-/Session-Regel, und ein Validierungsrezept.


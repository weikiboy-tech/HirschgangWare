# Ickza-DMA

External **DayZ** cheat that reads memory over a **DMA card** (PCILeech / MemProcFS) from a second PC.
ESP overlay + radar on the second monitor + triggerbot via **kmbox / MAKCU** (pure input injection, no memory writes).

> Runs on the *second* PC (the "cheat PC"). The game runs on the first PC, communication is DMA only.

## Features

- **ESP overlay** on the main monitor: boxes, skeletons, names, distance, held item, dead/alive, kind (player / zombie / animal), loot
- **Radar** on the second monitor: satellite map (Chernarus + others), players/zombies/vehicles/loot markers, zoom, place names, toggle with `F8`
- **Mini radar** in the corner of the overlay (adjustable size)
- **Friend marking**: middle-click a player on the radar -> marker + ESP turn green (persisted in `friends.txt`)
- **Loot filter**: category based, driven by `dayz_loot_cats.txt` / `dayz_rare.txt`
- **Triggerbot over kmbox / MAKCU**: connect the box, bind a trigger key (mouse 4/5/left/right/middle or a key), hold to shoot.
  Configurable delay, hit radius, hitboxes, targeting distance, players-only. The box injects while the physical mouse chain stays intact for the game.
- Bind capture accepts input **from the box** and **from this PC**
- All settings auto-saved (`Ickza.ini`, `trigger.ini`, `friends.txt` next to the exe)

## Layout

```
src/            cheat source (dma.cpp = VMM read layer, esp.cpp, radar.cpp, trigger.cpp, ...)
third_party/    pcileech headers (vmmdll.h)
dma/            runtime DLLs for the DMA device (vmm.dll, leechcore.dll, FTD3XX, dbghelp, info.db)
dist/           ready-to-run build output (Ickza.exe + dma/ + maps/ + txt data)
laptop/         helper files copied into a build (run script, loot tables)
docs/           notes
build.bat       MSVC build (needs "Desktop development with C++"; run: build.bat x64)
```

## Build

```
build.bat x64
```

Needs Visual Studio / MSVC Build Tools with the C++ workload (uses `vswhere` + `vcvarsall`).
Output: `build\Ickza.exe` (plus `build\dma`, `build\maps`, loot tables).

## Run

```
dist\Ickza.exe --dma --fullscreen
```

`run_ickza.bat` does the same. **Only one DMA tool may own the card at a time** (close PureDMA / pcileech / SSZ).

Keys: `F8` radar on/off · `Tab` collapse/expand settings sidebar · `MMB` on a radar player = mark/unmark friend · `END` exit.

## Configuration files (auto-created next to the exe)

| file | content |
|---|---|
| `Ickza.ini` | ESP/radar settings, range, minimap size, sidebar state |
| `trigger.ini` | kmbox/MAKCU device, baud, commands, trigger key, delay, hitbox mask |
| `friends.txt` | friend list (names, case-insensitive) |
| `keyarray.txt` | found DMA key address (written once the key path verified it) |

## Offsets

DayZ updates move a few globals; currently patched for the latest build:

| thing | value |
|---|---|
| `World` | `0x1834B80` |
| `NetworkClient` (roster/names) | `0x4A8E4D0` |

If it says "waiting for DayZ" after a game update, these are the two values to find again.

## DMA keyboard path

A trigger key pressed on the **game PC** can also be read over DMA (`win32k` `gafAsyncKeyState`), so the
trigger works even when the keyboard is not routed through the box. The address lives in session space and
has to be searched, which used to cause hitching - the path is therefore bounded now:

* resolve runs **once**, the found address is kept in `keyarray.txt` (delete that file to force a fresh search)
* verification (7 test keys, then a differential refine inside the win32k image) runs at most 6 attempts and
  then stops - no periodic full-image reads any more (`refine confirmed ... periodic refine stopped`)
* afterwards the path is passive: one 64 byte read per tick
* if the read fails 3x (reboot / session moved) it re-verifies by itself

Off-switch (exactly the old behaviour: box + this PC only): `src/dma.cpp` -> `static constexpr bool keyEnable_ = false;`
Details, evidence and measurements: `docs/INPUT_RESEARCH.md`, `docs/INPUT_DEBUG_HISTORY.md`.
Standalone hardware check without the cheat: `tools\keyprobe.exe probe` (DMA card must be free).

## Credits

- Base project: `rattposion/DAYZ-DMA-`
- DMA access: PCILeech / MemProcFS (`vmm.dll`, `leechcore.dll`)
- UI: Dear ImGui + DirectX 11 backend

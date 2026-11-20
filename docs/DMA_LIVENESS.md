# DMA capture stopping with a live game

## Evidence on 2026-11-18

At 20:35:41.630 and 20:36:51.334 the log reported four failed process-alive
checks, then `Game or DMA connection lost` and `stopped`. The native Windows
process remained running. No matching Windows Application Error (1000) event
was returned for the inspected two-hour interval. This identifies a controlled
capture shutdown for these occurrences, not a proven application exception.

The same captures reported `cannot read PE header` during attach, while camera
samples/lens validation and gameplay/loot reads succeeded. The last recorded
camera sample immediately before each shutdown had zero misses and zero invalid
lenses. These observations contradict using PE-header readability as a necessary
condition for keeping capture alive. They do not establish that every earlier
reported crash has this cause, or why the header becomes unreadable.

Previously `DmaProcess::alive()` tried the image's MZ header and immutable code
probes. `captureAliveProbes()` itself needs that PE header. If it cannot read the
header at attach, there are no probes, and four failed header checks stop capture
after roughly two seconds even with working gameplay reads. This path predates
Clutch Mode and was also present during the removed Smart Alerts feature.

## Correction

The existing VMM process-information API captures the attached PID/EPROCESS
identity. On the existing 500 ms scanner-thread check, a specific-process refresh
updates that process's kernel record, including with `-norefresh`. The result
must identify the same PID and EPROCESS and have active process state. A complete
16-byte uncached, non-paged read of that process object through System PID 4 is
also required: cached process metadata alone must never hide a disconnected
device. API/refresh failures, short reads, inactive processes and changed
identities fail the check. The original four-failure stop threshold remains.

This uses the existing VMM handle and DMA lock in the scanner thread, not the
render thread. It does not add a separate connection, offsets, input resolver,
or search through other processes. Refresh timings use existing telemetry.
Older providers without a captured kernel identity retain the legacy PE/probe
route and explicitly report that limitation in the log.

`[dma] liveness:` reports the check method or failure reason on transitions and
on the first/fourth failure. A failed read is no longer logged as proof the game
process disappeared. Snapshot freshness and the actual stop path are unchanged;
the fix does not keep stale frames falsely live.

API grounding: the bundled `third_party/pcileech/vmmdll.h` documents process
information and `VMMDLL_OPT_REFRESH_SPECIFIC_PROCESS`. The upstream
[VMMDLL implementation](https://github.com/ufrisk/MemProcFS/blob/master/vmm/vmmdll.c)
dispatches that option to a single-process refresh; the
[process implementation](https://github.com/ufrisk/MemProcFS/blob/master/vmm/vmm.c)
uses state zero for the active-process path. Runtime compatibility/performance
with the user's particular bundled DLL still requires live testing.

## Verification

`tests\build_dma_liveness_test.bat` builds an isolated mock `vmm.dll` under
`build/dma-liveness-test` and runs the actual production `DmaProcess` through its
normal DLL-loading/attach/read/alive methods. It never replaces `build/dma/vmm.dll`
and opens no FPGA or mouse device. It covers:

- Unreadable PE header with readable game data: capture liveness succeeds beyond
  the old four-failure threshold.
- Disconnected transport with cached active process metadata: fails.
- Exited process, changed PID/EPROCESS, failed query/refresh, short kernel read:
  fail; subsequent healthy samples recover.
- Missing kernel identity: no metadata-only success; original PE fallback works
  when a real header read is available and fails when disconnected.

The full native build is produced separately as
`build/HirschgangWare-liveness-test.exe`. It is not automatically installed or
started while the user's current session is working. Live acceptance requires
starting that build from its launcher, observing sustained healthy liveness
without capture shutdown, then testing a real disconnect and target-game exit.
The first must still stop capture after four failed checks; game exit must be
reported as inactive/changed/unavailable identity. Record refresh durations and
camera/frame timings before calling the fix verified on hardware.

# Player-name association

The saved test configuration had PlayerNames (bit 256) disabled. Player
markers and other enabled layers could be delivered through Cloudflare while
the protocol correctly omitted every name. The test configuration now enables
that release explicitly. Browser filters still operate independently.

Further defects found in the shared scan:

- Near/far entity lists were concatenated without removing duplicate pointers.
- Entity network IDs were cached until a ten-second sweep, including zero IDs.
- Invalid roster counts were replaced by the maximum capacity, allowing reads
  beyond the populated list.
- Failed roster reads retained previous associations indefinitely.
- Character model names were used when no player identity was resolved.
- Text validation rejected numeric and non-ASCII player names and accepted
  printable prefixes of truncated or malformed strings.

The scan now deduplicates entities, rereads their IDs each scan and reads the
roster every 500 ms. It checks row ID/name pointers again after the text read,
and checks the roster header again before publication. Failed reads invalidate
the association. A network ID claimed by multiple entities or a repeated case-insensitive name is ambiguous
and receives no personal name or name-derived friend color. Local identity is
included in collision checks. Names use bounded, validated UTF-8. Character
model/profile names are not substituted for an unresolved network identity.
Tracking IDs change when a known entity address receives a different network
ID. All views use the resulting shared snapshot.

The protocol reports `namesShared`; the browser explains missing host release,
disabled labels or a missing resolved name. Enabling player names enables
labels too. Revoking the host release still removes the names from the wire.

Validation: fixture cases cover address reuse, zero/missing IDs, duplicate ID
claims, case-insensitive duplicate names, conflicting roster rows, numeric and
UTF-8 names and malformed/truncated text. Server tests check the actual name
release and its revocation; browser tests check names and label activation.
These are synthetic checks, not proof of current live-game offsets.

Live acceptance: after Start, identify two players with different known names
(one friend), compare native overlay/radar with the public WebRadar, then have
one disappear/respawn/rejoin. Neither marker may inherit the other's name or
friend color. Duplicate rows must not create a second marker. Check roster log
named/visible counts if a name is missing. A server-supplied anonymous identity
or a player absent from client memory cannot yield a verified real name. A
newly changed name may take up to the next 500 ms roster read to appear.

## Friends and trigger candidates

Host-marked green friends remain visible on the maps and overlay. Their point
and body-region candidates are removed from the per-frame trigger list before
target evaluation. Key capture, click timing and device output are unchanged.
If an active target becomes a friend, the resulting empty target list causes
the existing output logic to release its button. This is exclusion, not an
occlusion test: an overlapping opponent behind a friend can still be selected.
The geometry/output test uses an isolated UDP receiver to check that excluded
friends cause no down packets while the key is held, nonfriends still do, and
friend marking ends an already active press.

## Player-name availability

### Observed 2026-11-18

Native Names and Radar Names were enabled and the host's PlayerNames release
was set. The normal scanner repeatedly reported an empty roster and zero named
survivors. The existing diagnostic sweep independently found PlayerIdentity
records at NetworkClient +0x18, count +0x20 (159 in that capture), name +0xF8,
and the existing identity +0x30 / entity +0x664 ID relationship. Thus the reported
absence already occurred before native/WebRadar rendering. This is separate
from Clutch deliberately suppressing player names on its overlay.

The sweep can accept partial reads; the previous production path required an
entire contiguous roster block, fixed 80-byte name reads and an unchanged total
count. One failed array page or membership-count change discarded the entire
roster. It also rejected duplicate ID/name pairs even when they agreed exactly.
These failure cases are reproduced offline, but which one caused the observed
live empty roster is not yet established by the older log's limited counters.

### Changes

`src/player_roster.h` reads the existing documented-in-project fields, using
the existing Process/DMA interface at the existing 500 ms roster cadence:

- Read each array slot through the batched reader; discard inaccessible slots.
- Read ID/name pointer and bounded UTF-8 text. String reads stop at a real NUL,
  use small chunks and never cross a page boundary unnecessarily. Unterminated,
  malformed or incompletely read names are rejected, not displayed as prefixes.
- Re-read every slot, ID and name pointer. Only unchanged identities still in
  the current array/count can publish. Root/array replacement clears the result.
- Changes in total count do not invalidate independently verified old slots.
  Newly added slots wait until the next complete pass; removed slots are omitted.
- Identical repeated ID/name pairs are idempotent. Conflicting names for one ID
  remain rejected. Existing multiple-entity and duplicate-name ambiguity checks
  remain active, including conflicts with the local player.

No last-known name is blindly reapplied to a reused entity pointer. No model
name is substituted for a player's identity. Native ESP, radar and WebRadar
continue receiving the same resolved snapshot names; permissions, friend colors,
tracking profiles and browser filters retain their existing behavior.

`[names]` logs status, count, readable slots, valid fields, valid texts, verified
rows, conflicting IDs and published count on status changes/at most every 10 s
for an unchanged status. It does not dump player identifiers or names. These
counters distinguish root/count, table-read, text-read and identity failures.

### Tests and remaining live verification

Run `tests\build_player_roster_test.bat`. The production reader runs against a
deterministic Process adapter: partial array, boundary strings, UTF-8, truncated
names, consistent duplicates, conflicting IDs, changed slot/ID/name pointer,
replaced root, count growth/shrinkage, invalid/empty count and recovery.

Build the native app using the existing MSVC build. The name test build also
includes the separately tested DMA liveness correction documented in
`DMA_LIVENESS.md`; it does not restore Smart Alerts.

Live acceptance: after starting capture, inspect `[names] ready` and a nonzero
published count, then verify named/visible players through the native overlay,
radar and released WebSocket packet. Confirm names against actual known players,
especially friends. Repeat after a join/leave, death/respawn, reconnect and
session change. Offline tests and an HTTP fixture do not prove that all current
Ground Zero identities can be resolved. If names still fail, retain the new
`[names]` line instead of repeatedly restarting or loosening identity validation.

#### First hardware observation after deployment

At 21:50:15 the new running build published 96 validated names from count=159,
98 valid slots, 96 valid field/text pairs, and zero conflicting IDs. Subsequent
passes published 95 names as membership changed; the native log named the
visible survivor. An eight-second read of the actual local WebSocket captured
62/62 live packets; all three transmitted players had nonempty names in every
packet and the own-player name was present. The kernel liveness check reported
zero failed checks. This verifies real name acquisition and local WebSocket
delivery for that interval, not every identity's in-game correctness, browser
pixels, an external Cloudflare connection, or long-term reconnect stability.

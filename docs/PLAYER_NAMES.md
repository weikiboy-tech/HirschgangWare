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
the association. A repeated network ID or case-insensitive name is ambiguous
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

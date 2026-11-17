# Webmap data flow

The Visual Studio project and property sheet are named HirschgangWare after
commit 9eeb446. The entry point is main in src/esp.cpp. It owns the event loop,
rendering, settings and calls to syncRadarScreen. runFrame calls it after the
game overlay renders. The idle branch calls it as well. syncRadarScreen creates
a local radar::Frame and calls radar::sync only when the local radar is wanted
or visible. The existing local window and that call remain independent.

DMA scan, camera and loot threads publish their existing snapshots. drawDayzFrame
takes local copies under the existing snapshot locks. The Webmap takes its
release-filtered copy from this path, using radar::Frame and radar::Dot. It
does no DMA itself. The own marker requires snap.haveMe because the local
renderer can fall back to camera coordinates. No network callback enters the
render thread or calls into Process. A single server worker owns its sockets,
map inventory and JSON serialization. Publication replaces the previous
immutable snapshot. It does not queue packets for slow clients.

The position feed uses the latest scan independently of camera reads. The
selected native map is used while automatic world detection has no result.
A brief camera gap can reuse the last heading within the freshness window.

The local renderer maps x to u = x / worldSize and z to v = 1 - z / worldSize.
The browser uses the same geometry with x east and z north. It draws the entire
raster into a world-sized square regardless of image pixel dimensions. The
bundled Chernarus map uses 15360 metres and three files under dist/maps.
Additional map images and sizes are read from the same maps directory at runtime.

## Build and run

Run `build.bat x64` with the Visual Studio C++ workload and Windows SDK.
The Visual Studio project and runtime copy target also include the new files.
From the build directory start `HirschgangWare.exe --webmap` for the standalone
browser map, or `HirschgangWare.exe --dma --webmap` to preselect DMA and enable the
Webmap. Click **Start** in the launcher to connect to the game. The launcher
remains open after a disconnect and waits for another manual start. Open http://127.0.0.1:8765/. No network service is started by default.
The existing local Radar → Map → Webmap panel enables/disables the service,
edits the port and update rate, shows its status, and opens the browser.
Closing the radar window leaves the configured service running. The same
settings can be configured with the application closed in
`HirschgangWare.ini`, under `[HirschgangWare]`:

```ini
webmap_enabled=1
webmap_port=8765
webmap_hz=8
webmap_release=0
webmap_public_host=
```

Release bits are 1 current map, 2 own position, 4 own heading, 8 player
positions with headings and friend markers, 16 infected, 32 animals, 64 vehicles,
128 loot, 256 player names, 512 held items, and 1024 events. Existing bit
values keep their meaning. For example,
`webmap_release=7` publishes the current map and own marker with heading.
The map bit is required for all live data, and own position is required for
heading. Loot also requires the local loot reader and respects its category
and name filters. The local infected, animal and vehicle visibility settings also apply to their released layers. Browser filters only hide already released markers.
Server positions outside the selected world's bounds and nonfinite positions
are discarded. The own position is derived from the game snapshot. Friend
markers use the same name-based list as the local radar and update in all
connected browsers. A valid heading is drawn as a short line. **Own heading**
enables that line and Heading-Up for the own marker.

**Player names** shares available in-game names with authorized browsers for
the Tracking dropdown and marker captions. **Held items** independently shares
the same held-item names used by the desktop radar. The browser's **Labels**
switch controls all captions. **Spielernamen auf Karte** and **Waffen auf
Karte** independently control the two player lines and are saved per browser.
The tracking dropdown still uses the names shared by the host. A hint in the
sidebar identifies a missing **Held items** host release when weapon labels are
selected. The first line adds horizontal distance
when Own position is shared, and the second line disappears with empty hands.
Revoking either host release omits that field from subsequent packets. Browsers
can select any visible released player,
including when the own position is unavailable. Players without a released name
have a numbered entry. Tracking uses an opaque session ID. The chosen target
stays selected if it temporarily disappears, and the camera resumes tracking
when the same target returns. Loadouts, server details, credentials, process
identifiers and memory addresses are omitted.

Clicking a player marker selects it and enables Follow. The GPS button enables
Follow for the selected target. Dragging disables Follow and leaves rotation
enabled. Zooming preserves Follow. Follow, rotation and smoothing are
independent settings. Waypoints, filters and tracking preferences are specific
to the browser and origin. The target selection is specific to the tab.

### Shared map events

Use **Display → Events** to enable event markers on the desktop map.
For browser guests, enable **Map → Webmap → Events**, then use the browser's
**Events** layer switch. It is possible to share events with the desktop
radar window closed and with the ordinary loot layer turned off. Both
maps consume the same event snapshot and project its world X/Z coordinates
using their existing map scale.

Named events match exact class names from the observed loot and near/far
object tables. Expansion's documented Airdrop container variants produce
**Airdrop**. The Central Economy wrecks `Wreck_UH1Y` and
`Wreck_Mi8_Crashed` produce **Helikopterabsturz**. This requires an observed
readable class and position. Catalog or mission configuration alone cannot
locate an active event. Named markers follow subsequent position samples and
expire after three seconds without a fresh observation.

**Loot-Häufung** is a heuristic based on the complete loot snapshots before
web filtering and the per-layer cap. At least eight new objects must appear
within eight seconds and 25 metres of a candidate in a consistently observed
area near the player. Initial snapshots and repeated identities add no
new objects. Snapshots with unreadable loot tables cannot create a cluster.
A nearby confirmed event replaces the cluster marker. Clusters expire when
fewer than four contributing objects remain observed, or after 90 seconds.
Confirmed and heuristic markers reset on a session, world or selected map change.

The local session logs include `chernarusplus`, with a bundled 15360 m map.
Ground Zero's exact custom event mapping still needs a capture from that
server with the world ID, timestamp, exact crate or event class, world X/Z,
and which observed object table contained it. A before/during/after sample
at a known drop location would also validate detection and removal.

- [DayZ Expansion AirdropSettings](https://github.com/salutesh/DayZ-Expansion-Scripts/wiki/%5BServer-Hosting%5D-AirdropSettings)
- [Bohemia Chernarus Central Economy events](https://github.com/BohemiaInteractive/DayZ-Central-Economy/blob/master/dayzOffline.chernarusplus/db/events.xml)
- [Bohemia Livonia Central Economy events](https://github.com/BohemiaInteractive/DayZ-Central-Economy/blob/master/dayzOffline.enoch/db/events.xml)

Static maps are available independently of live releases. The map list
contains local PNG/JPEG files whose basenames are valid map IDs and have
an accompanying numeric `.size` file in metres. A map ID can have a
`_topo` image fallback. Use `<id>_names.txt` with one line per place in
`x z category name` format to include place names. An image is limited to
64 MiB. The build and package ship the existing Chernarus image and metadata.
The server rechecks the inventory every three seconds; Refresh reloads the
browser list. A removed or unreadable map yields an error rather than a
fallback to an unrelated world.

## Transport and access

The server listens only on IPv4 loopback. Routes are read-only and allowlisted.
HTTP serves `/`, its local assets, `/api/status`, `/api/maps`,
`/api/places/<id>`, and `/maps/<id>`. The WebSocket at `/ws` sends
versioned snapshots and handles close and ping control frames. Client
application messages are rejected. Only the newest snapshot is retained.
One worker handles up to 16 simultaneous connections with bounded headers,
payloads and file chunks. Partial transfers time out. Disabling, changing the
port, or changing release settings closes existing connections. Browsers
clear live markers when disconnected and reconnect with bounded backoff.
The static map and browser waypoints remain usable offline while the page
is open. No CDN assets are needed.

The Canvas 2D renderer is the CRS.Simple-equivalent for this single-raster
world map. It uses Pointer Events for pinch/drag and requestAnimationFrame
for interpolation. Server dependencies are the C++ standard library and
Windows Winsock, BCrypt, and Crypt32. No third-party map or networking
package is added.

### Optional Cloudflare Tunnel

For a test without a domain, use a recent `cloudflared` with `--allowed-mail`
support and Node.js. Start the current build, enable the Webmap, then run
`node tools/webmap-cloudflare.mjs 8765` (use the port shown in the app). Set
`HIRSCHGANG_TEST_EMAIL` to the invited address and, if needed,
`HIRSCHGANG_CLOUDFLARED` to the full path of `cloudflared.exe` before running
the command. Open the printed URL and complete the email-code login.
This forwards the running Webmap's assets and WebSocket data, including its
current release settings. The Webmap and relay each listen on loopback.
Close the tunnel with Ctrl+C when the test is finished.

`--public-test` opens an unauthenticated test link for 30 minutes. The
Webmap release settings determine which live data everyone with the link can see.

For a fixed hostname, use a named tunnel.

1. Install cloudflared using the official Windows instructions and add the
   intended domain to your Cloudflare account. Run `cloudflared tunnel login`
   and `cloudflared tunnel create hirschgangware-webmap`.
2. In Zero Trust → Access → Applications create a self-hosted application
   for **the whole hostname** `webmap.example.com`, including `/ws`. Add an
   Allow policy only for the intended users. Copy the application AUD.
3. Copy [the example YAML](cloudflared-webmap.example.yml) to your local
   cloudflared config. Fill in the tunnel UUID, credentials file, team and
   AUD. Set `webmap_public_host` to that exact lowercase hostname in
   `HirschgangWare.ini`, then restart HirschgangWare. Keep the service URL
   on loopback and the port in sync with the app.
4. Run `cloudflared tunnel route dns hirschgangware-webmap webmap.example.com`,
   then `cloudflared tunnel --config config.yml ingress validate` and
   `cloudflared tunnel --config config.yml run hirschgangware-webmap`.
5. On a different network, open `https://webmap.example.com/` in a private
   browser window. Access must require sign-in. Before sign-in, verify both
   `/api/maps` and a `/ws` upgrade fail. After sign-in verify the page,
   map image, and a WebSocket HTTP 101 in the browser Network panel. Stop
   the tunnel and check for Offline; restart it and confirm reconnect.

The hostname route requires an Access assertion header, but the origin does
not validate its signature itself. The example uses cloudflared's
`originRequest.access.required` with the correct AUD to enforce validation.
Keep that enabled on the whole hostname. Do not open a router port, set up
another reverse proxy that injects the header. Store the tunnel credential
outside the repository.

[Cloudflare Tunnel setup](https://developers.cloudflare.com/tunnel/features/locally-managed-tunnels/create-local-tunnel/)
· [Access for public hostnames](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/self-hosted-public-app/)
· [Origin token validation](https://developers.cloudflare.com/tunnel/reference/origin-parameters/#access)
· [WebSocket protocol](https://www.rfc-editor.org/rfc/rfc6455.html)

## Changed files

- `src/webmap.h`, `src/webmap_state.h`, `src/webmap.cpp` implement the public
  settings, immutable snapshot and local transport.
- `src/esp.cpp`, `src/esp_launcher.h/.cpp`, `src/esp_panel.cpp`, and `src/radar.h/.cpp` wire
  the current game snapshot, lifecycle, persisted settings and ImGui controls.
- `src/trigger.cpp` releases an active output before closing the box when the session stops.
- `web/index.html`, `web/style.css`, `web/app.js`, `web/map.js`, and
  `web/live.js` provide the browser UI, camera and WebSocket client.
- `README.md` links the new startup and configuration instructions.
- `build.bat`, `HirschgangWare.vcxproj`, `HirschgangWare.props`,
  `package.bat`, and `deploy.bat` include the Webmap and bundled map assets.
- Both `run_hirschgangware.bat` scripts open the launcher.
- `tests/` contains camera/protocol tests and a real loopback server fixture.
- `docs/WEBMAP.md` and `docs/cloudflared-webmap.example.yml` document this build.


## Verification

Run `tests\build_webmap_test.bat` and `node tests/webmap_browser.mjs` from the
repository root for the Tracking browser test. Set `WEBMAP_TEST_CHROME` to the
Chrome executable if it is installed elsewhere. The test opens two isolated
headless browser profiles against the local fixture. It checks the dropdown,
map names and held-item changes during Follow, event positions, layer and
label switches, release revocation, map changes, heading lines, reconnect,
and friend color changes using simulated snapshots. A live game session is
required to verify the corresponding DMA data.

- x64 build via the Windows SDK and MSVC build script.
- `node tests/webmap.mjs` checks image/world conversion in both directions,
  map boundaries and resolutions, 0/90/180/270-degree projection, Follow
  activation, drag and zoom, 359-to-1-degree tracking, interpolation settling,
  invalid messages and bounded reconnect.
- `tests\build_webmap_test.bat` followed by `python tests/webmap_server.py`
  runs the actual C++ server on a disposable loopback port. It checks map
  and place routes, a missing image, traversal attempts, invalid Host and
  Origin, rejected unauthenticated public-host requests, the RFC handshake,
  invalid control data, concurrent coherent snapshots, revocation of a
  release, and listener stop/restart/shutdown.
- Browser smoke tests used the bundled Chernarus raster with the integrated
  app in standalone web mode. The same UI was also verified using a synthetic
  marker on an independent fixture. Search, zoom, desktop and narrow layout,
  Follow through zoom, Heading-Up during drag, and the visible Offline state
  were exercised.

- `python tests/launcher_smoke.py` checks Launcher-only startup with saved radar
  settings, the previous DMA/fullscreen flags, and the optional independent Webmap.
- The Visual Studio CopyRuntime target was checked using the installed v143 toolset.

Live game snapshot publication on the DMA hardware, physical touch/pinch
gestures, and an authenticated external Cloudflare tunnel still need an
end-to-end test. The MSVC batch build was used on this machine.

The launcher scripts now open the launcher directly. An in-flight attach must release the previous DMA session before Start becomes available again.

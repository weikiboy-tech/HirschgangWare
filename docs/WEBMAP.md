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
positions, 16 infected, 32 animals, 64 vehicles, 128 loot. For example,
`webmap_release=7` publishes the current map and own marker with heading.
The map bit is required for all live data, and own position is required for
heading. Loot also requires the local loot reader and respects its category
and name filters. The local infected, animal and vehicle visibility settings also apply to their released layers. Browser filters only hide already released markers.
Server positions outside the selected world's bounds and nonfinite positions
are discarded. The own position is derived from the game snapshot, not from
the fallback camera location. No player names, loadouts, server information,
credentials, process identifiers or memory addresses are published. Entity
labels in the browser are generic. The player layer includes a direction
arrow when a valid facing direction is available. Clicking the own marker or the GPS button
enables Follow. Dragging disables Follow and leaves rotation enabled. Zooming
preserves Follow. Waypoints, filters and tracking preferences are specific
to the browser and origin.

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
another reverse proxy that injects the header, or use a Quick Tunnel for
released data. Store the tunnel credential outside the repository.

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

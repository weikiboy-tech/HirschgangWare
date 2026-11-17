import { Camera, clamp, headingOnMap, radians, screenPoint, worldPoint } from "./map.js";
import { LiveConnection, layers, playerLines, trackingTargets, validPlayerId } from "./live.js";

const $ = id => document.getElementById(id);
const canvas = $("canvas"), ctx = canvas.getContext("2d");
const defaults = { follow: false, rotate: false, smooth: true, duration: 120, places: true, labels: true,
  playerNames: true, heldItems: true,
  grid: false, waypoints: true, markerSize: 7, autoMap: true, sidebar: null, map: "", filters: {}, points: [] };
const storageKey = "hirschgangware.webmap.v1";
const targetKey = storageKey + ".target";
function savedSettings() {
  try { return JSON.parse(localStorage.getItem(storageKey)) ?? {}; } catch { return {}; }
}
const stored = savedSettings(), prefs = { ...defaults };
for (const key of ["follow", "rotate", "smooth", "places", "labels", "playerNames", "heldItems", "grid", "waypoints", "autoMap", "sidebar"]) {
  if (typeof stored[key] === "boolean") prefs[key] = stored[key];
}
if (Number.isFinite(stored.duration)) prefs.duration = clamp(stored.duration, 40, 600);
if (Number.isFinite(stored.markerSize)) prefs.markerSize = clamp(stored.markerSize, 3, 16);
if (typeof stored.map === "string") prefs.map = stored.map;
for (const id of layers) if (typeof stored.filters?.[id] === "boolean") prefs.filters[id] = stored.filters[id];
if (Array.isArray(stored.points)) prefs.points = stored.points.filter(p =>
  p && typeof p.map === "string" && typeof p.name === "string" && p.name.length <= 60 &&
  Number.isFinite(p.x) && Number.isFinite(p.z)).slice(0, 100);
function save() {
  try { localStorage.setItem(storageKey, JSON.stringify(prefs)); } catch { /* Storage may be disabled. */ }
}

let maps = [], selected = null, raster = null, places = [], live = null, status = "connecting";
let width = 0, height = 0, ratio = 1, raf = null, lastDraw = 0, adding = false;
let loading = 0, catalogPromise = null, lastCatalog = 0, firstFit = true;
let trackingId = "own", trackedMarkerId = null, targets = new Map(), targetOptions = "";
try {
  const savedTarget = sessionStorage.getItem(targetKey);
  if (validPlayerId(savedTarget)) trackingId = savedTarget;
} catch { /* Storage may be disabled. */ }
const camera = new Camera(value => { $("follow").checked = prefs.follow = value; save(); redraw(); });
camera.follow = prefs.follow;
camera.rotate = prefs.rotate;
camera.smooth = prefs.smooth;
camera.milliseconds = prefs.duration;

const layerNames = { players: "Spieler", infected: "Infizierte", animals: "Tiere", vehicles: "Fahrzeuge", loot: "Beute", events: "Events" };
const colors = { players: "#fb8d9d", infected: "#faaf67", animals: "#d5b1fc", vehicles: "#a8c8fe", loot: "#e7d28c" };
const friendColor = "#3cff8c";
const trackingOrder = new Intl.Collator("de", { numeric: true });
const matchLive = () => live && selected && live.map === selected.id;
const pointsOnMap = () => prefs.points.filter(p => p.map === selected?.id);
function trackingLabel(id) {
  const player = targets.get(id);
  if (id === "own") return player?.name ? player.name + " (eigene Position)" : "Eigene Position";
  return player?.name || "Spieler " + id.split("-")[1];
}

function bounds() {
  const min = selected && width && height ? Math.min(width, height) / selected.size * .4 : .001;
  return [min, Math.max(min, 3)];
}
function fit() {
  if (!selected || !width || !height) return;
  camera.x = camera.z = selected.size / 2;
  camera.scale = Math.min(width, height) / selected.size * .92;
  firstFit = false;
}
function resize() {
  const box = canvas.getBoundingClientRect();
  width = box.width;
  height = box.height;
  ratio = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.max(1, Math.round(width * ratio));
  canvas.height = Math.max(1, Math.round(height * ratio));
  if (firstFit) fit();
  const [min, max] = bounds();
  camera.scale = clamp(camera.scale, min, max);
  redraw();
}

function redraw() {
  if (raf === null) {
    lastDraw = performance.now();
    raf = requestAnimationFrame(draw);
  }
}

function worldTransform() {
  ctx.translate(width / 2, height / 2);
  ctx.rotate(radians(-camera.angle));
  ctx.scale(camera.scale, camera.scale);
  ctx.translate(-camera.x, camera.z);
}

function drawGrid() {
  const step = [100, 250, 500, 1000, 2500, 5000, 10000].find(x => x * camera.scale >= 45) ?? 10000;
  const span = Math.hypot(width, height) / camera.scale / 2;
  const loX = Math.max(0, Math.floor((camera.x - span) / step) * step);
  const hiX = Math.min(selected.size, camera.x + span);
  const loZ = Math.max(0, Math.floor((camera.z - span) / step) * step);
  const hiZ = Math.min(selected.size, camera.z + span);
  if (loX > hiX || loZ > hiZ) return;
  ctx.strokeStyle = "#d4f9ed55";
  ctx.lineWidth = 1 / camera.scale;
  ctx.beginPath();
  for (let x = loX; x <= hiX; x += step) { ctx.moveTo(x, -loZ); ctx.lineTo(x, -hiZ); }
  for (let z = loZ; z <= hiZ; z += step) { ctx.moveTo(loX, -z); ctx.lineTo(hiX, -z); }
  ctx.stroke();
}

const labelBoxes = [];
function label(name, x, y, color = "#ecf3f3", declutter = false) {
  ctx.font = '600 12px "Segoe UI", sans-serif';
  ctx.textAlign = "center";
  if (declutter) {
    const half = ctx.measureText(name).width / 2 + 3;
    const box = { left: x - half, right: x + half, top: y - 13, bottom: y + 4 };
    if (labelBoxes.some(b => b.right > box.left && b.left < box.right && b.bottom > box.top && b.top < box.bottom)) return;
    labelBoxes.push(box);
  }
  ctx.lineWidth = 3;
  ctx.strokeStyle = "#07141a";
  ctx.strokeText(name, x, y);
  ctx.fillStyle = color;
  ctx.fillText(name, x, y);
}

function marker(p, radius, color, name, tracked = false) {
  const s = screenPoint(p, camera, width, height);
  if (s.x < -60 || s.y < -60 || s.x > width + 60 || s.y > height + 60) return;
  const direction = Number.isFinite(p.heading) ? radians(headingOnMap(p.heading, camera.angle)) : null;
  ctx.save();
  ctx.translate(s.x, s.y);
  if (direction !== null) {
    ctx.save();
    ctx.rotate(direction);
    ctx.beginPath();
    ctx.moveTo(0, -radius - 2);
    ctx.lineTo(0, -radius - 19);
    ctx.lineCap = "round";
    ctx.strokeStyle = "#09141b";
    ctx.lineWidth = 5;
    ctx.stroke();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2.5;
    ctx.stroke();
    ctx.restore();
  }
  ctx.strokeStyle = tracked ? "#eafcf8" : "#09141b";
  ctx.lineWidth = 2;
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.arc(0, 0, radius, 0, Math.PI * 2);
  ctx.fill(); ctx.stroke(); ctx.restore();
  if (name && prefs.labels) {
    const below = direction !== null && Math.cos(direction) > 0;
    const lines = Array.isArray(name) ? name : [name];
    const y = s.y + (below ? radius + 15 : -radius - 5 - (lines.length - 1) * 14);
    lines.forEach((line, index) => label(line, s.x, y + index * 14, color));
  }
}

function eventMarker(event) {
  const p = screenPoint(event, camera, width, height);
  if (p.x < -24 || p.y < -24 || p.x > width + 24 || p.y > height + 24) return;
  const r = Math.max(6, prefs.markerSize);
  const color = event.kind === "loot-cluster" ? "#ffd380" : "#ffa869";
  ctx.beginPath();
  ctx.moveTo(p.x, p.y - r);
  ctx.lineTo(p.x + r, p.y);
  ctx.lineTo(p.x, p.y + r);
  ctx.lineTo(p.x - r, p.y);
  ctx.closePath();
  ctx.fillStyle = color;
  ctx.strokeStyle = "#09141b";
  ctx.lineWidth = 2;
  ctx.fill();
  ctx.stroke();
  if (prefs.labels) label(event.label, p.x, p.y - r - 5, color);
}

function compass() {
  const s = { x: width - 39, y: height - 85 };
  ctx.fillStyle = "#10202aee";
  ctx.beginPath(); ctx.arc(s.x, s.y, 22, 0, Math.PI * 2); ctx.fill();
  ctx.save();
  ctx.translate(s.x, s.y);
  ctx.rotate(radians(-camera.angle));
  ctx.fillStyle = "#6ff4d8";
  ctx.beginPath(); ctx.moveTo(0, -14); ctx.lineTo(4, 4); ctx.lineTo(-4, 4); ctx.closePath(); ctx.fill();
  ctx.restore();
  label("N", s.x, s.y + 14);
}

function draw(now) {
  const moving = camera.step(now - lastDraw);
  lastDraw = now;
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  if (selected && width && height) {
    ctx.save();
    worldTransform();
    ctx.fillStyle = "#182830";
    ctx.fillRect(0, -selected.size, selected.size, selected.size);
    if (raster) ctx.drawImage(raster, 0, -selected.size, selected.size, selected.size);
    if (prefs.grid) drawGrid();
    ctx.restore();

    labelBoxes.length = 0;
    if (prefs.places) {
      for (const place of places) {
        const s = screenPoint(place, camera, width, height);
        if (s.x < -60 || s.y < -60 || s.x > width + 60 || s.y > height + 60) continue;
        if (prefs.labels) label(place.name, s.x, s.y, "#ecf3f3", true);
        else marker(place, 2.5, "#eef4ef");
      }
    }
    if (prefs.waypoints) for (const p of pointsOnMap()) marker(p, 5, "#f7d466", p.name);
    if (matchLive()) {
      for (const [id, dots] of Object.entries(live.layers)) {
        if (id === "events" || prefs.filters[id] === false) continue;
        const radius = prefs.markerSize * .65;
        for (const p of dots) {
          const tracked = !!p.id && p.id === trackedMarkerId;
          marker(tracked ? camera.marker : p, radius, p.friend ? friendColor : colors[id],
            id === "players" ? playerLines(p, live.me, prefs) : layerNames[id], tracked);
        }
      }
      if (live.me) marker(trackedMarkerId === "own" ? camera.marker : live.me, prefs.markerSize, "#65f1d3",
        playerLines(live.me, null, prefs));
      if (prefs.filters.events !== false) for (const event of live.layers.events ?? []) eventMarker(event);
    }
    if (width > 130 && height > 180) compass();
  }
  raf = moving ? requestAnimationFrame(draw) : null;
}

function syncTracking() {
  targets = trackingTargets(matchLive() ? live : null, prefs.filters.players !== false);
  const target = targets.get(trackingId);
  const id = target ? trackingId : null;
  if (trackedMarkerId !== id) camera.updateMarker(null);
  trackedMarkerId = id;
  camera.updateMarker(target ?? null);

  const select = $("tracking-target");
  const ids = ["own", ...[...targets.keys()].filter(id => id !== "own").sort((a, b) =>
    trackingOrder.compare(trackingLabel(a), trackingLabel(b)) || a.localeCompare(b))];
  const nameCounts = new Map();
  for (const id of ids) {
    const label = trackingLabel(id);
    nameCounts.set(label, (nameCounts.get(label) ?? 0) + 1);
  }
  if (!ids.includes(trackingId)) ids.push(trackingId);
  const options = ids.map(id => {
    const player = targets.get(id), name = trackingLabel(id);
    const label = name + (nameCounts.get(name) > 1 ? " #" + id.split("-")[1] : "") +
      (player?.friend ? " · Freund" : "") + (!player ? " (nicht sichtbar)" : "");
    return { id, label, disabled: !player };
  });
  const stamp = JSON.stringify(options);
  if (stamp !== targetOptions) {
    targetOptions = stamp;
    select.replaceChildren(...options.map(({id, label, disabled}) => {
      const option = document.createElement("option");
      option.value = id;
      option.disabled = disabled;
      option.textContent = label;
      return option;
    }));
  }
  select.value = trackingId;
  $("center").disabled = !target;
  const oriented = Number.isFinite(target?.heading);
  $("orientation").textContent = prefs.rotate && oriented ? "Heading-Up" : "North-Up";
  $("tracking-status").textContent = prefs.autoMap && live?.map && !maps.some(m => m.id === live.map)
    ? "Spielkarte " + live.map + " fehlt im Kartenordner." : !target ? "Tracking-Ziel nicht sichtbar." :
    prefs.rotate && !oriented ? (trackingId === "own" ? "Eigene Blickrichtung fehlt. Am Host „Own heading“ prüfen." :
      "Blickrichtung des ausgewählten Spielers fehlt.") : "";
}

function pickTarget(id) {
  trackingId = id;
  try { sessionStorage.setItem(targetKey, id); } catch { /* Storage may be disabled. */ }
  syncTracking();
  redraw();
}

function selectedWaypoints() {
  const list = $("waypoint-list");
  list.replaceChildren();
  for (const p of pointsOnMap()) {
    const li = document.createElement("li"), jump = document.createElement("button"), remove = document.createElement("button");
    jump.textContent = p.name;
    jump.onclick = () => moveTo(p);
    remove.textContent = "×";
    remove.title = "Wegpunkt löschen";
    remove.setAttribute("aria-label", p.name + " löschen");
    remove.onclick = () => { prefs.points = prefs.points.filter(v => v !== p); save(); selectedWaypoints(); redraw(); };
    li.append(jump, remove);
    list.append(li);
  }
}

function moveTo(p) {
  camera.setFollow(false);
  camera.x = p.x; camera.z = p.z;
  redraw();
}

function matchPlaces() {
  const query = $("place-search").value.trim().toLocaleLowerCase(), list = $("results");
  list.replaceChildren();
  if (!query) return;
  for (const place of places.filter(p => p.name.toLocaleLowerCase().includes(query)).slice(0, 12)) {
    const li = document.createElement("li"), button = document.createElement("button");
    button.textContent = place.name;
    button.onclick = () => { moveTo(place); if (openMobile.matches) sidebar(false); };
    li.append(button); list.append(li);
  }
  if (!list.children.length) {
    const li = document.createElement("li"); li.textContent = "Keine Treffer"; list.append(li);
  }
}

async function getJson(path) {
  const response = await fetch(path, { cache: "no-store" });
  if (!response.ok) throw new Error("HTTP " + response.status);
  return response.json();
}

function setMap(id) {
  const map = maps.find(m => m.id === id) ?? null;
  if (map && map.id === selected?.id && map.size === selected.size) return;
  selected = map;
  raster = null; places = []; adding = false;
  $("add-waypoint").setAttribute("aria-pressed", "false");
  canvas.classList.remove("placing");
  firstFit = true;
  fit();
  camera.updateMarker(null);
  $("map-title").textContent = map?.id ?? "Karten";
  $("map-id").textContent = map?.id ?? "–";
  $("map-select").value = map?.id ?? "";
  $("map-message").textContent = map ? "Kartenbild wird geladen …" : "Keine Karte installiert.";
  $("add-waypoint").disabled = !map;
  syncTracking(); matchPlaces(); selectedWaypoints(); redraw();
  const revision = ++loading;
  if (!map) return;
  prefs.map = map.id; save();
  const image = new Image();
  image.onload = () => {
    if (revision !== loading) return;
    raster = image;
    $("map-message").textContent = "";
    redraw();
  };
  image.onerror = () => {
    if (revision !== loading) return;
    $("map-message").textContent = "Kartenbild fehlt oder kann nicht gelesen werden.";
    redraw();
  };
  image.src = "/maps/" + map.id;
  getJson("/api/places/" + map.id).then(list => {
    if (revision !== loading || !Array.isArray(list)) return;
    places = list.slice(0, 512).filter(p => p && Number.isFinite(p.x) && Number.isFinite(p.z) &&
      typeof p.name === "string" && p.name.length <= 128);
    matchPlaces(); redraw();
  }).catch(() => {});
}

function catalog(force = false) {
  if (catalogPromise) return catalogPromise;
  if (!force && Date.now() - lastCatalog < 4000) return;
  lastCatalog = Date.now();
  catalogPromise = getJson("/api/maps").then(entries => {
    if (!Array.isArray(entries)) throw new Error("Invalid catalog");
    maps = entries.slice(0, 64).filter(m => m && typeof m.id === "string" && /^[a-z0-9_-]{1,48}$/.test(m.id) &&
      Number.isFinite(m.size) && m.size >= 256 && m.size <= 100000);
    const select = $("map-select");
    select.replaceChildren();
    for (const map of maps) {
      const opt = document.createElement("option");
      opt.value = opt.textContent = map.id;
      select.append(opt);
    }
    const auto = prefs.autoMap && maps.find(m => m.id === live?.map);
    const wasSelected = maps.find(m => m.id === prefs.map);
    if (force && selected && !raster) selected = null;
    setMap(auto?.id ?? wasSelected?.id ?? maps[0]?.id ?? "");
    select.value = selected?.id ?? "";
  }).catch(() => {
    if (!selected) $("map-message").textContent = "Kartenliste konnte nicht geladen werden.";
  }).finally(() => { catalogPromise = null; });
  return catalogPromise;
}

function updateLayers() {
  const held = $("held-status");
  const hasPlayers = live?.me || live?.layers.players;
  const missing = live?.heldShared === false;
  const legacy = hasPlayers && live.heldShared === undefined &&
    !live.me?.held && !live.layers.players?.some(player => player.held);
  held.hidden = !(prefs.labels && prefs.heldItems && hasPlayers && (missing || legacy));
  held.textContent = held.hidden ? "" : missing
    ? "Waffenanzeige: Am Host unter Map → Webmap „Held items“ freigeben."
    : "Falls Waffen fehlen: Am Host unter Map → Webmap „Held items“ prüfen.";
  const box = $("released"), list = Object.keys(live?.layers ?? {});
  const stamp = list.join(",");
  if (box.dataset.layers === stamp) return;
  box.dataset.layers = stamp;
  box.replaceChildren();
  for (const id of list) {
    const label = document.createElement("label"), span = document.createElement("span"), input = document.createElement("input");
    label.className = "toggle";
    span.textContent = layerNames[id];
    input.type = "checkbox";
    input.checked = prefs.filters[id] !== false;
    input.onchange = () => { prefs.filters[id] = input.checked; syncTracking(); save(); redraw(); };
    label.append(span, input); box.append(label);
  }
}

let lastPacket = "", error = "";
function connectionState(state, reason) {
  if (state === "online") error = "";
  else if (reason) error = reason;
  const changed = state !== status;
  status = state;
  const title = { online: "Online", offline: "Offline", connecting: "Verbindet" }[state];
  $("connection").textContent = $("badge").textContent = title;
  $("badge").dataset.state = state;
  $("connection-error").textContent = error;
  if (state === "online" && changed) catalog(true);
  if (state !== "online" && changed) {
    live = null; lastPacket = "";
    syncTracking(); updateLayers(); redraw();
  }
}

function packet(update) {
  $("updated").textContent = new Date().toLocaleTimeString("de-DE");
  const stamp = JSON.stringify(update);
  if (stamp === lastPacket) return;
  lastPacket = stamp;
  live = update;
  if (prefs.autoMap && update.map && update.map !== selected?.id) {
    if (maps.some(m => m.id === update.map)) setMap(update.map);
    else catalog();
  }
  syncTracking(); updateLayers(); redraw();
}

function adjustZoom(factor, at = { x: width / 2, y: height / 2 }) {
  camera.zoom(factor, at, width, height, ...bounds());
  redraw();
}

const openMobile = matchMedia("(max-width: 700px)");
function sidebar(open) {
  $("layout").classList.toggle("collapsed", !open);
  $("open-sidebar").setAttribute("aria-expanded", String(open));
  prefs.sidebar = open;
  save();
}
sidebar(prefs.sidebar ?? !openMobile.matches);
$("open-sidebar").onclick = () => sidebar($("layout").classList.contains("collapsed"));
$("close-sidebar").onclick = () => { sidebar(false); $("open-sidebar").focus(); };
$("refresh").onclick = () => catalog(true);
$("zoom-in").onclick = () => adjustZoom(1.35);
$("zoom-out").onclick = () => adjustZoom(1 / 1.35);
$("center").onclick = () => { if (camera.marker) camera.setFollow(true); };
$("tracking-target").onchange = () => pickTarget($("tracking-target").value);
$("fullscreen").onclick = async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await $("layout").requestFullscreen();
  } catch { $("connection-error").textContent = "Vollbild hier nicht verfügbar."; }
};
$("map-select").onchange = () => {
  $("auto-map").checked = prefs.autoMap = false;
  setMap($("map-select").value);
  save();
};
$("place-search").oninput = matchPlaces;
$("add-waypoint").onclick = () => {
  if (!selected) return;
  adding = !adding;
  $("add-waypoint").setAttribute("aria-pressed", String(adding));
  canvas.classList.toggle("placing", adding);
  if (adding && openMobile.matches) sidebar(false);
};

for (const [id, key] of Object.entries({
  "auto-map": "autoMap", follow: "follow", rotate: "rotate", smooth: "smooth", places: "places",
  labels: "labels", "player-names": "playerNames", "held-items": "heldItems", grid: "grid", waypoints: "waypoints"
})) {
  $(id).checked = prefs[key];
  $(id).onchange = () => {
    prefs[key] = $(id).checked;
    if (key === "follow") camera.setFollow(prefs.follow);
    if (key === "rotate") camera.rotate = prefs.rotate;
    if (key === "smooth") camera.smooth = prefs.smooth;
    if (key === "autoMap" && prefs.autoMap && live?.map) {
      if (maps.some(m => m.id === live.map)) setMap(live.map);
      else catalog(true);
    }
    syncTracking(); updateLayers(); save(); redraw();
  };
}
$("duration").value = prefs.duration;
$("duration-value").textContent = prefs.duration + " ms";
$("duration").oninput = () => {
  camera.milliseconds = prefs.duration = Number($("duration").value);
  $("duration-value").textContent = prefs.duration + " ms";
  save(); redraw();
};
$("marker-size").value = prefs.markerSize;
$("marker-size").oninput = () => { prefs.markerSize = Number($("marker-size").value); save(); redraw(); };

const pointers = new Map();
let drag = null, pinch = null;
function eventPoint(e) {
  const rect = canvas.getBoundingClientRect();
  return { x: e.clientX - rect.left, y: e.clientY - rect.top };
}
function pair() {
  const [a, b] = pointers.values();
  return { at: { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 }, distance: Math.hypot(a.x - b.x, a.y - b.y) };
}
canvas.addEventListener("pointerdown", e => {
  if ((e.pointerType === "mouse" && e.button !== 0) || pointers.size >= 2) return;
  const p = eventPoint(e);
  canvas.setPointerCapture(e.pointerId);
  pointers.set(e.pointerId, p);
  if (pointers.size === 1) { drag = { start: p, last: p, moved: false }; pinch = null; }
  else { pinch = pair(); drag = null; }
});
canvas.addEventListener("pointermove", e => {
  const p = eventPoint(e);
  if (selected) {
    const pos = worldPoint(p, camera, width, height);
    $("coords").textContent = "X " + Math.round(pos.x) + " · Z " + Math.round(pos.z);
  }
  if (!pointers.has(e.pointerId)) return;
  pointers.set(e.pointerId, p);
  if (pointers.size === 2) {
    const next = pair();
    if (pinch?.distance > 4 && next.distance > 4) {
      if (!camera.follow) camera.pan(next.at.x - pinch.at.x, next.at.y - pinch.at.y);
      adjustZoom(next.distance / pinch.distance, next.at);
    }
    pinch = next;
  } else if (drag) {
    if (!drag.moved && Math.hypot(p.x - drag.start.x, p.y - drag.start.y) <= 4) return;
    drag.moved = true;
    canvas.classList.add("dragging");
    camera.pan(p.x - drag.last.x, p.y - drag.last.y);
    drag.last = p;
    redraw();
  }
});

function clickAt(p) {
  if (!selected) return;
  if (adding) {
    const pos = worldPoint(p, camera, width, height);
    if (pos.x < 0 || pos.z < 0 || pos.x > selected.size || pos.z > selected.size) return;
    const name = prompt("Name des Wegpunkts", "Wegpunkt");
    adding = false; canvas.classList.remove("placing"); $("add-waypoint").setAttribute("aria-pressed", "false");
    if (name?.trim()) {
      prefs.points = [...prefs.points.slice(-99), { map: selected.id, name: name.trim().slice(0, 60), ...pos }];
      $("waypoints").checked = prefs.waypoints = true;
      save(); selectedWaypoints(); redraw();
    }
  } else {
    let nearest = null, distance = Math.max(16, prefs.markerSize * 2);
    for (const [id, target] of targets) {
      const s = screenPoint(id === trackedMarkerId ? camera.marker : target, camera, width, height);
      const gap = Math.hypot(s.x - p.x, s.y - p.y);
      if (gap < distance) { nearest = id; distance = gap; }
    }
    if (nearest) {
      pickTarget(nearest);
      camera.setFollow(true);
    }
  }
}
function endPointer(e) {
  if (!pointers.delete(e.pointerId)) return;
  if (!pointers.size && drag && !drag.moved && e.type === "pointerup") clickAt(eventPoint(e));
  pinch = null; drag = null; canvas.classList.remove("dragging");
}
canvas.addEventListener("pointerup", endPointer);
canvas.addEventListener("pointercancel", endPointer);
canvas.addEventListener("lostpointercapture", endPointer);
canvas.addEventListener("wheel", e => {
  e.preventDefault();
  adjustZoom(Math.exp(-clamp(e.deltaY, -100, 100) * .004), eventPoint(e));
}, { passive: false });
canvas.addEventListener("keydown", e => {
  const d = { ArrowUp: [0, 30], ArrowDown: [0, -30], ArrowLeft: [30, 0], ArrowRight: [-30, 0] }[e.key];
  if (d) { e.preventDefault(); camera.pan(...d); redraw(); }
  if (e.key === "+" || e.key === "-") { e.preventDefault(); adjustZoom(e.key === "+" ? 1.25 : .8); }
});
new ResizeObserver(resize).observe($("stage"));
syncTracking();
catalog(true);
const url = new URL("/ws", location.href);
url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
const connection = new LiveConnection(url.href, connectionState, packet);
const connect = () => connection.start();
addEventListener("pagehide", () => connection.stop());
addEventListener("pageshow", connect);
connect();

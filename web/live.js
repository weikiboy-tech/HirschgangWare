export const layers = ["players", "infected", "animals", "vehicles", "loot", "events", "corpses", "stashLoot", "highValues"];
const stashLootKinds = ["backpack", "vehicle-key", "stash"];
const corpseLifetime = 7 * 60 * 1000;
const eventKinds = ["airdrop", "helicopter-crash", "loot-cluster"];
const validId = id => typeof id === "string" && /^[a-z0-9_-]{1,48}$/.test(id);
const validPoint = p => p && Number.isFinite(p.x) && Number.isFinite(p.z) &&
  p.x >= 0 && p.z >= 0 && p.x <= 100000 && p.z <= 100000;

const validHeading = value => value === undefined || (Number.isFinite(value) && Math.abs(value) <= 360);
export const validPlayerId = id => typeof id === "string" && /^[0-9a-f]{16}-[1-9][0-9]{0,9}$/.test(id);
const validHealth = value => Number.isInteger(value) && value >= 0 && value <= 100;
const shortText = value => typeof value === "string" && value.length > 0 && value.length <= 80 ? value : undefined;

export const healthLine = (player, enabled) => enabled && validHealth(player.health) ? "HP " + player.health : "";

export function playerLines(player, me, { playerNames = true, heldItems = true } = {}) {
  const name = playerNames ? player.name : undefined;
  const distance = name && me ? "  " + Math.round(Math.hypot(player.x - me.x, player.z - me.z)) + "m" : "";
  return [name ? name + distance : undefined, heldItems ? player.held : undefined].filter(Boolean);
}

export function trackingTargets(snapshot, showPlayers) {
  const players = showPlayers ? snapshot?.layers.players ?? [] : [];
  return new Map([
    ...(snapshot?.me ? [["own", snapshot.me]] : []),
    ...players.filter(p => p.id).map(p => [p.id, p]),
  ]);
}

export function savedPlayer(value) {
  if (!value || (value.id !== "own" && !validPlayerId(value.id))) return null;
  return {id: value.id, name: shortText(value.name)};
}

export function resolveSavedPlayer(saved, targets) {
  if (!saved) return null;
  if (targets.has(saved.id)) return saved.id;
  if (saved.id === "own" || !saved.name) return null;
  const matches = [...targets].filter(([id, player]) => id !== "own" && player.name === saved.name);
  return matches.length === 1 ? matches[0][0] : null;
}

export function decode(raw) {
  if (typeof raw !== "string" || raw.length > 320 * 1024) return null;
  let p;
  try { p = JSON.parse(raw); } catch { return null; }
  if (!p || p.v !== 1 || typeof p.live !== "boolean" || (p.map !== null && !validId(p.map)) ||
      !p.layers || Array.isArray(p.layers) || typeof p.layers !== "object") return null;
  if (p.me !== null && (!validPoint(p.me) || !validHeading(p.me.heading))) return null;
  if ((!p.live || p.map === null) && (p.me !== null || Object.keys(p.layers).length)) return null;
  const safe = {};
  for (const key of Object.keys(p.layers)) {
    const dots = p.layers[key];
    if (!layers.includes(key) || !Array.isArray(dots) || dots.length > 512 || !dots.every(validPoint)) return null;
    if (key === "stashLoot") {
      if (!dots.every(p => stashLootKinds.includes(p.kind))) return null;
      safe.stashLoot = dots.map(({x, z, kind}) => ({x, z, kind}));
      continue;
    }
    if (key === "events") {
      if (dots.length > 128 || !dots.every(e => eventKinds.includes(e.kind) && shortText(e.label))) return null;
      safe.events = dots.map(({x, z, kind, label}) => ({x, z, kind, label}));
      continue;
    }
    if (key === "corpses") {
      const ids = new Set();
      if (dots.length > 128) return null;
      for (const {id, ttlMs} of dots) {
        if (!validPlayerId(id) || ids.has(id) || !Number.isInteger(ttlMs) || ttlMs <= 0 || ttlMs > corpseLifetime) return null;
        ids.add(id);
      }
      safe.corpses = dots.map(({x, z, id, ttlMs}) => ({x, z, id, ttlMs}));
      continue;
    }
    if (key === "vehicles") {
      if (!dots.every(p => p.covered === undefined || typeof p.covered === "boolean")) return null;
      safe.vehicles = dots.map(({x, z, covered}) => ({x, z, covered: covered === true}));
      continue;
    }
    if (key === "highValues") {
      if (!dots.every(p => p.bitcoin === undefined || typeof p.bitcoin === "boolean")) return null;
      safe.highValues = dots.map(({x, z, bitcoin}) => ({x, z, ...(bitcoin === true ? {bitcoin:true} : {})}));
      continue;
    }
    const playerIds = new Set();
    safe[key] = dots.map(({x, z, category, heading, id, name, held, health, friend}) => ({
      x, z, category: Number.isInteger(category) ? category : null,
      heading: key === "players" ? heading : undefined,
      id: key === "players" ? id : undefined,
      name: key === "players" ? shortText(name) : undefined,
      held: key === "players" ? shortText(held) : undefined,
      health: key === "players" && validHealth(health) ? health : undefined,
      friend: key === "players" && friend === true,
    }));
    if (key === "players") {
      for (const {id, heading} of safe[key]) {
        if (!validHeading(heading)) return null;
        if (id === undefined) continue;
        if (!validPlayerId(id) || playerIds.has(id)) return null;
        playerIds.add(id);
      }
    }
  }
  const me = p.me === null ? null : { x: p.me.x, z: p.me.z, heading: p.me.heading,
    name: shortText(p.me.name), held: shortText(p.me.held), health: validHealth(p.me.health) ? p.me.health : undefined };
  const heldShared = typeof p.heldShared === "boolean" ? p.heldShared : undefined;
  const healthShared = typeof p.healthShared === "boolean" ? p.healthShared : undefined;
  const namesShared = typeof p.namesShared === "boolean" ? p.namesShared : undefined;
  return { live: p.live, map: p.map, me, layers: safe, heldShared, healthShared, namesShared };
}

export class CorpseVisibility {
  constructor(hidden = []) {
    this.hidden = new Set(Array.isArray(hidden) ? hidden.filter(validPlayerId).slice(-128) : []);
    this.deadlines = new Map();
    this.firstClick = null;
  }

  update(markers, now) {
    const next = new Map();
    for (const {id, ttlMs} of markers) {
      next.set(id, Math.min(this.deadlines.get(id) ?? Infinity, now + ttlMs));
    }
    this.deadlines = next;
  }

  visible(marker, now) {
    return !this.hidden.has(marker.id) && (this.deadlines.get(marker.id) ?? 0) > now;
  }

  click(id, now) {
    if (id && this.firstClick?.id === id && now - this.firstClick.at <= 400) {
      this.hidden.add(id);
      if (this.hidden.size > 128) this.hidden.delete(this.hidden.values().next().value);
      this.firstClick = null;
      return true;
    }
    this.firstClick = id ? {id, at: now} : null;
    return false;
  }
}

export class LiveConnection {
  constructor(url, onState, onPacket, env = globalThis) {
    Object.assign(this, { url, onState, onPacket, env });
    this.delay = 500;
    this.stopped = true;
  }

  start() {
    if (!this.stopped) return;
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    this.env.clearTimeout(this.retry);
    this.env.clearTimeout(this.watch);
    this.socket?.close();
    this.socket = null;
  }

  connect() {
    this.onState("connecting");
    let socket;
    try { socket = new this.env.WebSocket(this.url); } catch { this.schedule(); return; }
    this.socket = socket;
    const monitor = () => {
      this.env.clearTimeout(this.watch);
      this.watch = this.env.setTimeout(() => {
        if (this.socket === socket) socket.close();
      }, 8000);
    };
    monitor();
    socket.onmessage = ({ data }) => {
      if (this.socket !== socket) return;
      const update = decode(data);
      if (!update) {
        this.onState("offline", "Invalid WebSocket update");
        socket.close();
        return;
      }
      monitor();
      this.delay = 500;
      this.onState("online");
      this.onPacket(update);
    };
    socket.onerror = () => { if (this.socket === socket) socket.close(); };
    socket.onclose = () => {
      if (this.socket !== socket || this.stopped) return;
      this.env.clearTimeout(this.watch);
      this.socket = null;
      this.schedule();
    };
  }

  schedule() {
    if (this.stopped) return;
    this.onState("offline");
    this.retry = this.env.setTimeout(() => this.connect(), this.delay);
    this.delay = Math.min(10000, this.delay * 2);
  }
}

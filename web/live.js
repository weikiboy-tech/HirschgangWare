export const layers = ["players", "infected", "animals", "vehicles", "loot"];
const validId = id => typeof id === "string" && /^[a-z0-9_-]{1,48}$/.test(id);
const validPoint = p => p && Number.isFinite(p.x) && Number.isFinite(p.z) &&
  p.x >= 0 && p.z >= 0 && p.x <= 100000 && p.z <= 100000;

const validHeading = value => value === undefined || (Number.isFinite(value) && Math.abs(value) <= 360);

export function decode(raw) {
  if (typeof raw !== "string" || raw.length > 192 * 1024) return null;
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
    if (key === "players" && !dots.every(p => validHeading(p.heading))) return null;
    safe[key] = dots.map(({x, z, category, heading}) => ({
      x, z, category: Number.isInteger(category) ? category : null,
      heading: key === "players" ? heading : undefined,
    }));
  }
  const me = p.me === null ? null : { x: p.me.x, z: p.me.z, heading: p.me.heading };
  return { live: p.live, map: p.map, me, layers: safe };
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

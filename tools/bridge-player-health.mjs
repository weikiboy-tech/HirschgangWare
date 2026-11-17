import { open, rename, rm, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { setTimeout as pause } from "node:timers/promises";

const limit = 48 * 1024;
const maxAge = 2500;
const format = "HIRSCHGANG-HP-SERVER-1";
const [, , sourceArg, dirArg, serverName] = process.argv;
if (!sourceArg || !dirArg || !serverName?.trim() || /[\r\n]/.test(serverName)) {
  console.error('Usage: node tools/bridge-player-health.mjs <server-profile-export.tsv> <radar-directory> "Exact server name"');
  process.exit(2);
}
const source = resolve(sourceArg);
const destination = resolve(dirArg, "player-health.tsv");
if (source.toLowerCase() === destination.toLowerCase()) throw Error("Source and destination must differ");
const temporary = destination + "." + process.pid + ".tmp";
let lastStamp = -1, state = "";

async function snapshot() {
  const file = await open(source, "r");
  try {
    const before = await file.stat();
    const stamp = Math.floor(before.mtimeMs);
    const age = Date.now() - stamp;
    if (stamp <= lastStamp || age < 0 || age >= maxAge || before.size <= 0 || before.size >= limit) return;
    const bytes = Buffer.alloc(before.size);
    const {bytesRead} = await file.read(bytes, 0, bytes.length, 0);
    const after = await file.stat();
    if (bytesRead !== bytes.length || after.size !== before.size || after.mtimeMs !== before.mtimeMs) return;
    const lines = bytes.toString("utf8").trimEnd().split(/\r?\n/);
    const header = lines.shift()?.match(/^HIRSCHGANG-HP-SERVER-1 ([a-z0-9_-]{1,48})$/i);
    const footer = lines.pop();
    if (!header || footer !== "END " + lines.length || lines.length > 512) return;
    const ids = new Set();
    for (const line of lines) {
      const fields = line.trim().split(/\s+/);
      if (fields.length !== 5 || fields.some(v => !/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i.test(v))) return;
      const [id, x, z, hp, maximum] = fields.map(Number);
      if (!/^\d+$/.test(fields[0]) || id <= 0 || id > 2147483647 || ids.has(id) ||
          !(x >= 0 && x <= 100000 && z >= 0 && z <= 100000) ||
          !Number.isFinite(maximum) || !(maximum > 0 && hp >= 0 && hp <= maximum)) return;
      ids.add(id);
    }
    const data = ["HIRSCHGANG-HP-1 " + stamp + " " + header[1].toLowerCase() + " " + serverName.trim(), ...lines, ""].join("\n");
    if (Buffer.byteLength(data) >= limit) return;
    await writeFile(temporary, data, {flag: "wx"});
    await rename(temporary, destination);
    lastStamp = stamp;
    return lines.length;
  } finally {
    await file.close();
    await rm(temporary, {force: true});
  }
}

for (;;) {
  try {
    const count = await snapshot();
    if (count !== undefined && state !== "live") {
      state = "live";
      console.log(format + ": forwarding fresh snapshots (" + count + " players)");
    }
  } catch (error) {
    const message = error.code ?? "read or write failed";
    if (state !== message) console.error("Health bridge: " + message);
    state = message;
  }
  await pause(250);
}

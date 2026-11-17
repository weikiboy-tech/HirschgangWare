import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { cp, mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import http from "node:http";
import { createServer } from "node:net";
import { resolve } from "node:path";
import { createRelay } from "../tools/webmap-cloudflare.mjs";

const pause = ms => new Promise(done => setTimeout(done, ms));
async function until(fn, description, maxAttempts = 100) {
  for (let i = 0; i < maxAttempts; i++) {
    const result = await fn().catch(() => null);
    if (result) return result;
    await pause(75);
  }
  throw Error(description);
}
const stage = await mkdtemp(resolve("build/webmap-test/tracking-"));
await cp("web", resolve(stage, "web"), { recursive: true });
await mkdir(resolve(stage, "maps"));
await writeFile(resolve(stage, "maps/chernarusplus.size"), "15360");
const image = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+rH8sAAAAASUVORK5CYII=", "base64");
await writeFile(resolve(stage, "maps/chernarusplus.png"), image);
await writeFile(resolve(stage, "maps/different.png"), image);
await writeFile(resolve(stage, "maps/different.size"), "8192");
const probe = createServer();
await new Promise(done => probe.listen(0, "127.0.0.1", done));
const base = "http://127.0.0.1:" + probe.address().port;
const port = probe.address().port;
await new Promise(done => probe.close(done));
const fixture = spawn(resolve("build/webmap-test/server.exe"), [stage, String(port)], { windowsHide: true });
const profile = resolve(stage, "chrome");
let chrome, socket, relay;
const waiters = new Map();
let sequence = 0;
let response;
try {
  await until(async () => (await fetch(base + "/api/status")).ok, "fixture HTTP");
  fixture.stdin.write("events\n");
  const hostname = "hirschgang-test.trycloudflare.com";
  relay = createRelay(port, () => hostname);
  await new Promise(done => relay.listen(0, "127.0.0.1", done));
  const proxy = "http://127.0.0.1:" + relay.address().port;
  const request = (path, host = hostname) => new Promise((done, reject) => {
    http.get(proxy + path, {headers: {Host: host}}, reply => {
      const chunks = [];
      reply.on("data", chunk => chunks.push(chunk));
      reply.on("end", () => done({status: reply.statusCode, body: Buffer.concat(chunks)}));
      reply.on("error", reject);
    }).on("error", reject);
  });
  for (const path of ["/hirschgang-dont-press-me", "/app.js", "/hirschgang-logo.png", "/maps/chernarusplus"]) {
    const throughTunnel = await request(path);
    assert.equal(throughTunnel.status, 200);
    assert.deepEqual(throughTunnel.body, Buffer.from(await (await fetch(base + path)).arrayBuffer()));
  }
  assert.equal((await request("/", "wrong.trycloudflare.com")).status, 403);
  const websocket = origin => new Promise((done, reject) => {
    const ws = http.request(proxy + "/ws", {timeout: 3000, headers: {
      Host: hostname, Origin: origin, Upgrade: "websocket", Connection: "Upgrade",
      "Sec-WebSocket-Version": "13", "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
    }});
    ws.on("response", reply => { reply.resume(); done(reply.statusCode); });
    ws.on("upgrade", (_reply, client, first) => {
      let body = first.toString();
      const verify = () => {
        if (body.includes('"name":"private-player"') && body.includes('"friend":true') &&
            body.includes('"vehicles":[{"x":490,"z":1080},{"x":710,"z":1080,"covered":true}]') &&
            body.includes('"stashLoot":[{"x":450,"z":960,"kind":"backpack"},{"x":560,"z":960,"kind":"vehicle-key"},{"x":670,"z":960,"kind":"stash"}]')) {
          done(101); client.destroy();
        }
      };
      client.setTimeout(3000, () => { client.destroy(); reject(Error("Relayed WebSocket data timeout")); });
      client.on("data", chunk => { body += chunk.toString(); verify(); });
      client.on("error", reject);
      verify();
    });
    ws.on("timeout", () => ws.destroy(Error("WebSocket handshake timeout")));
    ws.on("error", reject);
    ws.end();
  });
  assert.equal(await websocket("https://wrong.trycloudflare.com"), 403);
  assert.equal(await websocket("https://" + hostname), 101);
  const chromePath = process.env.WEBMAP_TEST_CHROME ?? "C:/Program Files/Google/Chrome/Application/chrome.exe";
  chrome = spawn(chromePath, [
    "--headless", "--disable-gpu", "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check",
    "--user-data-dir=" + profile, "about:blank"
  ], { windowsHide: true });
  chrome.on("error", error => console.error(error.message));
  const active = await until(() => readFile(resolve(profile, "DevToolsActivePort"), "utf8"), "Chrome DevTools", 600);
  const [debugPort, debugPath] = active.trim().split(/\r?\n/);
  socket = new WebSocket("ws://127.0.0.1:" + debugPort + debugPath);
  await new Promise((done, reject) => { socket.onopen = done; socket.onerror = reject; });
  socket.onmessage = ({data}) => {
    const reply = JSON.parse(data);
    const pending = waiters.get(reply.id);
    if (!pending) return;
    waiters.delete(reply.id);
    if (reply.error) pending.reject(Error(reply.error.message));
    else pending.done(reply.result);
  };
  const call = (method, params = {}, sessionId) => new Promise((done, reject) => {
    const id = ++sequence;
    waiters.set(id, {done, reject});
    socket.send(JSON.stringify({id, method, params, sessionId}));
  });
  async function browser() {
    const {browserContextId} = await call("Target.createBrowserContext");
    const {targetId} = await call("Target.createTarget", {url: "about:blank", browserContextId});
    const {sessionId} = await call("Target.attachToTarget", {targetId, flatten: true});
    await call("Page.enable", {}, sessionId);
    await call("Page.addScriptToEvaluateOnNewDocument", {source: `
      const p = CanvasRenderingContext2D.prototype;
      window.paint = { strokes: [], names: [] };
      for (const name of ["clearRect", "arc", "moveTo", "lineTo", "stroke", "fillText", "fill"]) {
        const original = p[name];
        p[name] = function (...args) {
          if (name === "clearRect") window.paint = { strokes: [], names: [], fills: [], events: [], corpses: [], stashIcons: [], cars: [] };
          if (name === "arc") this.lastArc = args;
          if (name === "moveTo") { this.segment = null; this.start = args; }
          if (name === "lineTo") this.segment = args;
          if (name === "stroke" && this.segment?.[0] === 0 && this.segment?.[1] < -19) {
            const m = this.getTransform();
            window.paint.strokes.push({color: this.strokeStyle, angle: Math.round(Math.atan2(m.b,m.a)*180/Math.PI), x: m.e, y: m.f});
          }
          if (name === "stroke" && args[0] instanceof Path2D &&
              ["#e0a377", "#ffd264", "#6edcac"].includes(this.strokeStyle)) {
            const m = this.getTransform();
            window.paint.stashIcons.push({color: this.strokeStyle, x: m.e, y: m.f});
          }
          if (name === "stroke" && args[0] instanceof Path2D && this.strokeStyle === "#a8c8fe") {
            const m = this.getTransform();
            window.paint.cars.push({x: m.e, y: m.f});
          }
          if (name === "fillText") window.paint.names.push(args[0]);
          if (name === "fill") {
            window.paint.fills?.push(this.fillStyle);
            if (["#ffd380", "#ffa869"].includes(this.fillStyle))
              window.paint.events?.push({ color: this.fillStyle, x: this.start[0], y: this.start[1] });
            if (this.fillStyle === "#faf0d6" && this.lastArc?.[2] === 5.3)
              window.paint.corpses?.push({ x: this.lastArc[0], y: this.lastArc[1] + 1 });
          }
          return original.apply(this, args);
        };
      }
    `}, sessionId);
    await call("Page.navigate", {url: base + "/hirschgang-dont-press-me"}, sessionId);
    const evaluate = async expression => {
      const state = await call("Runtime.evaluate", {expression, returnByValue: true}, sessionId);
      if (state.exceptionDetails) throw Error(state.exceptionDetails.text);
      return state.result.value;
    };
    evaluate.click = async ({x, y}) => {
      for (const type of ["mousePressed", "mouseReleased"])
        await call("Input.dispatchMouseEvent", {type, x, y, button: "left", clickCount: 1}, sessionId);
    };
    return evaluate;
  }
  const host = await browser(), guest = await browser();
  const opts = "[...document.querySelector('#tracking-target').options].map(o => o.textContent)";
  for (const evaluate of [host, guest]) {
    await until(() => evaluate("document.querySelector('.brand-mark')?.naturalWidth === 256"), "Original Hirschgang logo loaded");
    await until(async () => (await evaluate(opts)).includes("private-player · Freund"), "Name and friend in tracking dropdown");
    response = await until(() => evaluate("paint.strokes.some(s=>s.color === '#3cff8c' && s.angle === 90) && paint.strokes.some(s=>s.color === '#65f1d3' && s.angle === 90) && paint.fills.includes('#fb8d9d')"), "North-Up line on friend and own marker, red other player");
    assert(response);
    await until(() => evaluate("paint.names.some(t => /^private-player  \\d+m$/.test(t)) && paint.names.includes('GZ_TestRifle') && paint.names.includes('private-name')"), "player names, distance and held item on map");
    await until(() => evaluate("paint.events.length === 3 && paint.names.includes('Airdrop') && paint.names.includes('Loot-Häufung') && paint.names.includes('Helikopterabsturz')"), "three named event markers");
  }
  const eventPositions = (await host("paint.events")).sort((a, b) => a.x - b.x);
  const vehicles = await until(() => host("paint.cars.length === 2 && paint.cars"), "normal and covered vehicle icons");
  assert.ok(vehicles[0].x < vehicles[1].x && vehicles[0].y === vehicles[1].y);
  assert.equal(await host("paint.names.some(name => /Fahrzeuge|TestCar|Motorbike_Red_Cover/.test(name))"), false);
  await host("document.querySelector('#vehicle-layer').click()");
  await until(() => host("paint.cars.length === 0"), "vehicle switch hides both vehicle icons");
  assert.equal(await guest("paint.cars.length"), 2);
  assert.equal(await host("JSON.parse(localStorage.getItem('hirschgangware.webmap.v1')).filters.vehicles"), false);
  await host("document.querySelector('#vehicle-layer').click()");
  await until(() => host("paint.cars.length === 2"), "vehicle icons restored");
  const stashIcons = await until(() => host("paint.stashIcons.length === 3 && paint.stashIcons"), "three stash loot icons on the map");
  assert.deepEqual(stashIcons.map(icon => icon.color), ["#e0a377", "#ffd264", "#6edcac"]);
  assert.ok(stashIcons[0].x < stashIcons[1].x && stashIcons[1].x < stashIcons[2].x);
  assert.ok(stashIcons.every(icon => icon.y === stashIcons[0].y));
  assert.equal(await host("paint.names.some(name => /HuntingBag|ExpansionCarKey|UndergroundStash|Rucksack|Fahrzeugschlüssel|^Stash$/.test(name))"), false);
  await host("document.querySelector('#stash-loot-layer').click()");
  await until(() => host("paint.stashIcons.length === 0"), "stash loot switch removes all three icons");
  assert.equal(await host("JSON.parse(localStorage.getItem('hirschgangware.webmap.v1')).filters.stashLoot"), false);
  assert.equal(await guest("paint.stashIcons.length"), 3);
  await host("document.querySelector('#stash-loot-layer').click()");
  await until(() => host("paint.stashIcons.length === 3"), "stash loot enabled again");
  assert.equal(await host("paint.names.includes('HP 75')"), false);
  await host("document.querySelector('#health').click()");
  await until(() => host("paint.names.includes('HP 75') && paint.names.includes('HP 100')"), "valid health values visible when enabled");
  assert.equal(await guest("paint.names.includes('HP 75')"), false);
  await host("document.querySelector('#health').click()");
  await until(() => host("!paint.names.includes('HP 75') && !paint.names.includes('HP 100')"), "HP toggle off removes both numbers");
  await host("document.querySelector('#health').click()");
  const corpse = await until(() => host(`(() => {
    const p = paint.corpses[0];
    if (!p) return null;
    const rect = document.querySelector("#canvas").getBoundingClientRect();
    return {x: rect.left + p.x, y: rect.top + p.y};
  })()`), "corpse skull visible");
  await host.click(corpse);
  assert.equal(await host("paint.corpses.length"), 1);
  await host.click(corpse);
  await until(() => host("paint.corpses.length === 0"), "double click hides skull");
  assert.equal(await guest("paint.corpses.length"), 1);
  assert.deepEqual(eventPositions.map(event => event.color), ["#ffa869", "#ffd380", "#ffa869"]);
  assert.ok(Math.abs(eventPositions[0].y - eventPositions[2].y) < .001);
  assert.ok(Math.abs((eventPositions[1].x - eventPositions[0].x) / (eventPositions[2].x - eventPositions[0].x) - 130 / 250) < .001);
  const toggleEvents = "[...document.querySelectorAll('#released label')].find(l=>l.textContent.trim()==='Events').querySelector('input').click()";
  await host(toggleEvents);
  await host("document.querySelector('#corpse-layer').click()");
  assert.equal(await host("document.querySelector('#corpse-layer').checked"), false);
  await host("document.querySelector('#corpse-layer').click()");
  assert.equal(await host("document.querySelector('#corpse-layer').checked"), true);
  await until(() => host("paint.events.length === 0"), "events hidden locally");
  assert.equal(await guest("paint.events.length"), 3);
  await host(toggleEvents);
  await host("document.querySelector('#labels').click()");
  await until(() => host("!paint.names.some(t => /private|GZ_TestRifle/.test(t)) && paint.events.length === 3 && paint.names.includes('HP 75')"), "HP stays on when other labels are off");
  await host("document.querySelector('#labels').click()");
  await host("document.querySelector('#player-names').click()");
  await until(() => host("paint.names.includes('GZ_TestRifle') && !paint.names.some(t => t.startsWith('private-player'))"), "hide player name only");
  assert.ok((await host(opts)).includes("private-player · Freund"));
  assert.equal(await host("JSON.parse(localStorage.getItem('hirschgangware.webmap.v1')).playerNames"), false);
  await host("document.querySelector('#player-names').click()");
  await host("document.querySelector('#held-items').click()");
  await until(() => host("!paint.names.includes('GZ_TestRifle') && paint.names.some(t => t.startsWith('private-player'))"), "hide held item only");
  assert.equal(await host("JSON.parse(localStorage.getItem('hirschgangware.webmap.v1')).heldItems"), false);
  assert.equal(await guest("paint.names.includes('GZ_TestRifle')"), true);
  await host("document.querySelector('#held-items').click()");
  await guest(`
    const select = document.querySelector('#tracking-target');
    select.selectedIndex = [...select.options].findIndex(o => o.textContent.startsWith('private-player'));
    select.dispatchEvent(new Event('change'));
    document.querySelector('#smooth').click();
    document.querySelector('#follow').click();
    document.querySelector('#rotate').click();
    true;
  `);
  const aligned = `(() => {
    const box = document.querySelector('canvas').getBoundingClientRect();
    const dpr = Math.min(devicePixelRatio, 2);
    return paint.strokes.some(s => s.color === '#3cff8c' && s.angle === 0 &&
      Math.abs(s.x/dpr-box.width/2)<1 && Math.abs(s.y/dpr-box.height/2)<1);
  })()`;
  await until(() => guest(aligned), "guest follow plus Heading-Up centered on selected player");
  fixture.stdin.write("swap\n");
  await until(() => guest("paint.names.includes('GZ_SecondRifle') && !paint.names.includes('GZ_TestRifle')"), "held item changes during Follow");
  assert.equal(await guest("document.querySelector('#follow').checked"), true);
  fixture.stdin.write("empty\n");
  await until(() => guest("paint.names.some(t => t.startsWith('private-player')) && !paint.names.includes('GZ_SecondRifle')"), "empty hands remove second line");
  fixture.stdin.write("swap\nnames\n");
  await until(() => guest("paint.names.some(t => t.startsWith('private-player')) && !paint.names.includes('GZ_SecondRifle') && paint.events.length === 0 && paint.corpses.length === 0"), "held, event and corpse releases withdrawn");
  await until(() => guest("!document.querySelector('#held-status').hidden && document.querySelector('#held-status').textContent.includes('Held items')"), "host release guidance");
  await until(() => host("!paint.names.includes('HP 75') && !paint.names.includes('HP 100') && document.querySelector('#health-status').textContent.includes('freigeben')"), "HP removed when host revokes release");
  await until(() => guest("!document.querySelector('#corpse-status').hidden && document.querySelector('#corpse-status').textContent.includes('Corpses') && document.querySelector('#corpse-layer').checked"), "corpse toggle and missing host release guidance");
  await until(() => guest("paint.stashIcons.length === 0 && !document.querySelector('#stash-loot-status').hidden"), "stash icons removed when host revokes release");
  await until(() => guest("paint.cars.length === 0 && !document.querySelector('#vehicle-status').hidden"), "vehicle icons removed when host revokes release");
  fixture.stdin.write("held\n");
  await until(() => guest("paint.names.includes('GZ_SecondRifle') && !paint.names.some(t => t.startsWith('private-player'))"), "name release withdrawn independently");
  fixture.stdin.write("events\n");
  await until(() => guest(aligned), "follow preserved after release changes");
  await until(() => guest("paint.corpses.length === 1"), "corpse release restored on guest");
  assert.equal(await guest("document.querySelector('#corpse-status').hidden"), true);
  assert.equal(await host("paint.corpses.length"), 0);
  assert.equal(await host("document.querySelector('#rotate').checked"), false);
  assert.equal(await host("document.querySelector('#follow').checked"), false);
  await guest("document.querySelector('#zoom-in').click()");
  await until(() => guest(aligned), "zoom preserves guest Follow");
  await guest(`document.querySelector('canvas').dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowRight'}))`);
  assert.equal(await guest("document.querySelector('#follow').checked"), false);
  assert.equal(await guest("document.querySelector('#rotate').checked"), true);
  await host("document.querySelector('#rotate').click()");
  await until(() => host("paint.strokes.some(s=>s.color === '#65f1d3' && s.angle === 0)"), "own Heading-Up");
  fixture.stdin.write("guests\n");
  await until(async () => {
    const options = await guest(opts);
    const selected = await guest("document.querySelector('#tracking-target').selectedOptions[0]?.textContent");
    return options.includes("Eigene Position (nicht sichtbar)") && /^private-player/.test(selected);
  }, "guest reconnects and retains target while own marker is absent");
  fixture.stdin.write("unfriend\n");
  await until(() => guest("paint.strokes.some(s=>s.color === '#fb8d9d' && s.angle === 0) && !paint.fills.includes('#3cff8c')"), "friend marker updates to red");
  fixture.stdin.write("events\n");
  await until(() => host("paint.events.length === 3"), "events ready before map change");
  fixture.stdin.write("mapchange\n");
  await until(() => guest("document.querySelector('#map-select').value === 'different' && paint.events.length === 0 && paint.corpses.length === 0"), "events and corpses cleared on map change");
  console.log("PASS two isolated Chrome clients: live player labels, held item, event positions, corpse dismissal, layer releases, map switch, follow and friend colors");
} finally {
  relay?.close();
  socket?.close();
  chrome?.kill();
  fixture.stdin.end("quit\n");
}

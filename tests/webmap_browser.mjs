import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { cp, mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import http from "node:http";
import { createServer } from "node:net";
import { resolve } from "node:path";
import { createRelay } from "../tools/webmap-cloudflare.mjs";

const pause = ms => new Promise(done => setTimeout(done, ms));
async function until(fn, description) {
  for (let i = 0; i < 100; i++) {
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
await writeFile(resolve(stage, "maps/chernarusplus.png"),
  Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+rH8sAAAAASUVORK5CYII=", "base64"));
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
  fixture.stdin.write("names\n");
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
        if (body.includes('"name":"private-player"') && body.includes('"friend":true')) {
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
  const active = await until(() => readFile(resolve(profile, "DevToolsActivePort"), "utf8"), "Chrome DevTools");
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
      for (const name of ["clearRect", "moveTo", "lineTo", "stroke", "fillText", "fill"]) {
        const original = p[name];
        p[name] = function (...args) {
          if (name === "clearRect") window.paint = { strokes: [], names: [], fills: [] };
          if (name === "moveTo") this.segment = null;
          if (name === "lineTo") this.segment = args;
          if (name === "stroke" && this.segment?.[0] === 0 && this.segment?.[1] < -19) {
            const m = this.getTransform();
            window.paint.strokes.push({color: this.strokeStyle, angle: Math.round(Math.atan2(m.b,m.a)*180/Math.PI), x: m.e, y: m.f});
          }
          if (name === "fillText") window.paint.names.push(args[0]);
          if (name === "fill") window.paint.fills?.push(this.fillStyle);
          return original.apply(this, args);
        };
      }
    `}, sessionId);
    await call("Page.navigate", {url: base + "/hirschgang-dont-press-me"}, sessionId);
    return async expression => {
      const state = await call("Runtime.evaluate", {expression, returnByValue: true}, sessionId);
      if (state.exceptionDetails) throw Error(state.exceptionDetails.text);
      return state.result.value;
    };
  }
  const host = await browser(), guest = await browser();
  const opts = "[...document.querySelector('#tracking-target').options].map(o => o.textContent)";
  for (const evaluate of [host, guest]) {
    await until(() => evaluate("document.querySelector('.brand-mark')?.naturalWidth === 256"), "Original Hirschgang logo loaded");
    await until(async () => (await evaluate(opts)).includes("private-player · Freund"), "Name and friend in tracking dropdown");
    response = await until(() => evaluate("paint.strokes.some(s=>s.color === '#3cff8c' && s.angle === 90) && paint.strokes.some(s=>s.color === '#65f1d3' && s.angle === 90) && paint.fills.includes('#fb8d9d')"), "North-Up line on friend and own marker, red other player");
    assert(response);
    assert.deepEqual((await evaluate("paint.names")).filter(name => name.includes("private") || name.includes("Second")), []);
  }
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
  console.log("PASS two isolated Chrome clients: dropdown, no map names, direction lines, friend color, follow, rotation, zoom, independent settings, reconnect and live unmark");
} finally {
  relay?.close();
  socket?.close();
  chrome?.kill();
  fixture.stdin.end("quit\n");
}

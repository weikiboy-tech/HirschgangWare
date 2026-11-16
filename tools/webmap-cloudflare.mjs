import { spawn } from "node:child_process";
import http from "node:http";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const entry = "/hirschgang-dont-press-me";
const routes = ["/", entry, "/hirschgang-logo.png", "/app.js", "/map.js", "/live.js", "/style.css",
  "/api/status", "/api/maps"];

export function createRelay(port, getHostname) {
  const local = "http://127.0.0.1:" + port;
  const allowed = req => getHostname() && req.headers.host === getHostname() && req.method === "GET";
  const server = http.createServer({ requestTimeout: 8000 }, (req, res) => {
    if (!allowed(req)) { res.writeHead(403).end(); return; }
    if (!routes.includes(req.url) && !/^\/(?:api\/places|maps)\/[a-z0-9_-]{1,48}$/.test(req.url)) {
      res.writeHead(404).end();
      return;
    }
    const upstream = http.get(local + req.url, { timeout: 8000 }, reply => {
      res.writeHead(reply.statusCode, reply.headers);
      reply.pipe(res);
    });
    upstream.on("timeout", () => upstream.destroy());
    upstream.on("error", () => {
      if (res.headersSent) res.destroy();
      else res.writeHead(502).end();
    });
    res.on("close", () => upstream.destroy());
  });
  server.on("upgrade", (req, client, head) => {
    const key = req.headers["sec-websocket-key"];
    if (!allowed(req) || req.url !== "/ws" || req.headers.origin !== "https://" + getHostname() ||
        req.headers.upgrade?.toLowerCase() !== "websocket" || req.headers["sec-websocket-version"] !== "13" ||
        !/^[A-Za-z0-9+/]{22}==$/.test(key ?? "")) {
      client.end("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
      return;
    }
    const upstream = http.request(local + "/ws", { timeout: 5000, headers: {
      Origin: local, Upgrade: "websocket", Connection: "Upgrade",
      "Sec-WebSocket-Version": "13", "Sec-WebSocket-Key": key,
    } });
    upstream.on("upgrade", (reply, socket, firstFrame) => {
      client.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
        "Sec-WebSocket-Accept: " + reply.headers["sec-websocket-accept"] + "\r\n\r\n");
      if (firstFrame.length) client.write(firstFrame);
      if (head.length) socket.write(head);
      socket.on("error", () => client.destroy());
      socket.on("close", () => client.destroy());
      client.on("close", () => socket.destroy());
      client.pipe(socket).pipe(client);
    });
    upstream.on("response", reply => { reply.resume(); client.destroy(); });
    upstream.on("timeout", () => upstream.destroy());
    upstream.on("error", () => client.destroy());
    client.on("error", () => upstream.destroy());
    client.on("close", () => upstream.destroy());
    upstream.end();
  });
  server.maxConnections = 24;
  return server;
}

async function start() {
  const port = Number(process.argv[2] ?? 8765);
  const publicLink = process.argv[3] === "--public-test";
  if (!Number.isInteger(port) || port < 1024 || port > 65535) throw Error("Invalid local Webmap port.");
  if (process.argv[3] && !publicLink) throw Error("Unknown tunnel option.");
  const { HIRSCHGANG_TEST_EMAIL, ...env } = process.env;
  const email = HIRSCHGANG_TEST_EMAIL?.trim();
  if (!publicLink && (!email || !/^[^\s@*,]+@[^\s@*,]+\.[^\s@*,]+$/.test(email))) {
    throw Error("Set HIRSCHGANG_TEST_EMAIL to the email address invited to this Webmap.");
  }
  const localPage = await fetch("http://127.0.0.1:" + port + entry, { signal: AbortSignal.timeout(3500) });
  if (!localPage.ok) throw Error("Start and enable the current Webmap build on the selected port.");
  await localPage.body?.cancel();

  let hostname = "";
  const relay = createRelay(port, () => hostname);
  const sockets = new Set();
  relay.on("connection", socket => {
    sockets.add(socket);
    socket.once("close", () => sockets.delete(socket));
  });
  await new Promise((done, reject) => {
    relay.once("error", reject);
    relay.listen(0, "127.0.0.1", done);
  });
  const args = ["tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:" + relay.address().port];
  if (!publicLink) args.push("--allowed-mail", email);
  const connector = spawn(process.env.HIRSCHGANG_CLOUDFLARED ?? "cloudflared.exe", args,
    { env, stdio: ["ignore", "ignore", "pipe"], windowsHide: true });
  const startup = setTimeout(() => {
    if (!hostname) { console.error("Cloudflare did not create the tunnel."); connector.kill(); }
  }, 45000);
  const expiry = publicLink ? setTimeout(() => connector.kill(), 30 * 60 * 1000) : null;
  let output = "";
  connector.stderr.setEncoding("utf8");
  connector.stderr.on("data", part => {
    output = (output + part).slice(-16000);
    const host = output.match(/https:\/\/([a-z0-9-]+\.trycloudflare\.com)\b/i)?.[1];
    const provisioned = publicLink ? /your quick tunnel has been created/i : /your protected quick tunnel has been created/i;
    if (!hostname && host && provisioned.test(output)) {
      hostname = host;
      clearTimeout(startup);
      console.log("https://" + hostname + entry);
    }
    if (part.includes(" ERR ")) console.error((email ? part.replaceAll(email, "[invited address]") : part).trim());
  });
  const close = code => {
    clearTimeout(startup);
    clearTimeout(expiry);
    for (const socket of sockets) socket.destroy();
    relay.close(() => { process.exitCode = code; });
  };
  connector.once("error", error => { console.error(error.message); close(1); });
  connector.once("exit", code => close(code ?? 0));
  for (const signal of ["SIGINT", "SIGTERM"]) process.once(signal, () => connector.kill());
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await start();
}

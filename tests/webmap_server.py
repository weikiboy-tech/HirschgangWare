from concurrent.futures import ThreadPoolExecutor
import base64
import http.client as http_client
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "build" / "webmap-test" / ("case-" + uuid.uuid4().hex[:8])
MAPS = STAGE / "maps"
MAPS.mkdir(parents=True)
shutil.copytree(ROOT / "web", STAGE / "web")
MAPS.joinpath("chernarusplus.jpg").write_bytes(b"test-jpeg-fragment")
MAPS.joinpath("chernarusplus.size").write_text("15360\n")
MAPS.joinpath("chernarusplus_names.txt").write_text("20 30 City Good Place\n-5 4 City Outside\n")
MAPS.joinpath("different.png").write_bytes(b"test-png-fragment")
MAPS.joinpath("different.size").write_text("8192\n")
MAPS.joinpath("missing.size").write_text("1000\n")
STAGE.joinpath("private.txt").write_text("never returned")

with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    PORT = probe.getsockname()[1]
HOST = f"127.0.0.1:{PORT}"
ORIGIN = f"http://{HOST}"


def http(path, method="GET", headers=None):
    with_http = http_client.HTTPConnection("127.0.0.1", PORT, timeout=2)
    try:
        with_http.request(method, path, headers=headers or {})
        response = with_http.getresponse()
        return response.status, response.read()
    finally:
        with_http.close()


def wait_http():
    for _ in range(30):
        try:
            if http("/api/status")[0] == 200:
                return
        except OSError:
            pass
        time.sleep(.1)
    raise AssertionError("listener did not start")


def websocket(origin=ORIGIN, target_host=HOST, version=13, extra=""):
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
    sock.settimeout(2)
    # RFC 6455 worked example nonce.
    key = base64.b64encode(b"the sample nonce").decode()
    sock.sendall((
        f"GET /ws HTTP/1.1\r\nHost: {target_host}\r\nOrigin: {origin}\r\n"
        "Connection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n"
        f"Sec-WebSocket-Version: {version}\r\nSec-WebSocket-Key: {key}\r\n"
        + extra + "\r\n").encode())
    stream = sock.makefile("rb")
    status = stream.readline()
    if b" 101 " not in status:
        stream.close()
        sock.close()
        return status, None, None
    headers = b""
    while line := stream.readline():
        if line == b"\r\n":
            break
        headers += line
    assert b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in headers
    return status, sock, stream


def frame(stream):
    header = stream.read(2)
    assert len(header) == 2
    op, length = header
    assert not length & 128
    if length == 126:
        length = struct.unpack("!H", stream.read(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", stream.read(8))[0]
    body = stream.read(length)
    assert len(body) == length
    return op & 15, body


def verify_snapshot(ws_number):
    status, sock, stream = websocket()
    assert b" 101 " in status
    with sock, stream:
        verified = 0
        for _ in range(20):
            op, body = frame(stream)
            assert op == 1
            snap = json.loads(body)
            if snap["me"] is None and verified == 0:
                continue
            assert snap["me"]["heading"] == 90
            assert snap["me"]["x"] * 2 == snap["me"]["z"]
            assert set(snap["layers"]) == {"loot"}
            assert snap["layers"]["loot"][0]["x"] == snap["me"]["x"]
            assert b"private" not in body and b"player" not in body
            verified += 1
            if verified == 12:
                break
        assert verified == 12
    return ws_number


def wire(payload, opcode):
    return bytes([0x80 | opcode, 128 | len(payload)]) + b"mask" + bytes(
        byte ^ b"mask"[i % 4] for i, byte in enumerate(payload))


server = subprocess.Popen([str(ROOT / "build/webmap-test/server.exe"), str(STAGE), str(PORT)],
                          stdin=subprocess.PIPE, text=True)
try:
    wait_http()
    assert http("/")[0] == 200
    assert http("/map.js")[0] == 200
    assert {m["id"]: m["size"] for m in json.loads(http("/api/maps")[1])} == {
        "chernarusplus": 15360, "different": 8192}
    assert http("/maps/chernarusplus") == (200, b"test-jpeg-fragment")
    assert json.loads(http("/api/places/chernarusplus")[1]) == [{"x": 20, "z": 30, "name": "Good Place"}]
    assert http("/maps/missing")[0] == 404
    MAPS.joinpath("different.png").unlink()
    assert http("/maps/different")[0] == 404
    for path in ["/maps/../private.txt", "/maps/%2e%2e%2fprivate", "/maps/chernarusplus/../private", "/private.txt"]:
        status, content = http(path)
        assert status in (400, 404) and b"never returned" not in content
    assert http("/api/maps", "POST")[0] == 405
    assert http("/api/maps", headers={"Host": "evil.example"})[0] == 403
    assert http("/api/maps", headers={"Host": "webmap.example.com"})[0] == 403
    assert websocket(origin="https://evil.example")[0].startswith(b"HTTP/1.1 403")
    assert websocket(version=12)[0].startswith(b"HTTP/1.1 403")
    assert websocket(origin="https://webmap.example.com", target_host="webmap.example.com")[0].startswith(b"HTTP/1.1 403")
    with ThreadPoolExecutor(4) as pool:
        assert list(pool.map(verify_snapshot, range(4))) == list(range(4))

    status, sock, stream = websocket()
    with sock, stream:
        sock.sendall(wire(b"ping", 9))
        for _ in range(3):
            if frame(stream) == (10, b"ping"):
                break
        else:
            raise AssertionError("missing pong")
        sock.sendall(b"\x81\x01x")  # unmasked and unsupported
        for _ in range(3):
            op, body = frame(stream)
            if op == 8:
                assert body[:2] == struct.pack("!H", 1002)
                break
        else:
            raise AssertionError("missing protocol-error close")

    _, sock, stream = websocket()
    with sock, stream:
        frame(stream)
        server.stdin.write("hide\n")
        server.stdin.flush()
        for _ in range(4):
            if stream.peek(1) == b"":
                break
            frame(stream)
        else:
            raise AssertionError("release change did not close old subscriber")
    wait_http()
    _, sock, stream = websocket()
    with sock, stream:
        op, body = frame(stream)
        snap = json.loads(body)
        assert op == 1 and snap["live"] is False and snap["map"] is None and snap["me"] is None and snap["layers"] == {}

    server.stdin.write("off\n")
    server.stdin.flush()
    for _ in range(30):
        try:
            http("/api/status")
        except OSError:
            break
        time.sleep(.07)
    else:
        raise AssertionError("listener did not stop")
    server.stdin.write("on\n")
    server.stdin.flush()
    wait_http()
    verify_snapshot(0)
finally:
    if server.poll() is None:
        server.stdin.write("quit\n")
        server.stdin.flush()
        try:
            server.wait(4)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
            raise AssertionError("shutdown blocked")
assert server.returncode == 0
print("PASS: HTTP assets, map limits, paths, origin/host, WebSocket framing, concurrent snapshots, release and start/stop")

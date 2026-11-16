import ctypes as c
from ctypes import wintypes as w
import http.client
from pathlib import Path
import shutil
import socket
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "build" / ("launcher-test-" + uuid.uuid4().hex[:8])
STAGE.mkdir()
APP = STAGE / "HirschgangWare.exe"
shutil.copyfile(ROOT / "build/HirschgangWare.exe", APP)
shutil.copytree(ROOT / "web", STAGE / "web")

user = c.WinDLL("user32")
enumeration = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = (enumeration, w.LPARAM)
user.GetWindowThreadProcessId.argtypes = (w.HWND, c.POINTER(w.DWORD))
user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
user.PostMessageW.argtypes = (w.HWND, w.UINT, w.WPARAM, w.LPARAM)


def classes(pid):
    handles = {}

    @enumeration
    def visit(hwnd, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            buf = c.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, buf, len(buf))
            if buf.value.startswith("hirschgangware_"):
                handles[buf.value] = hwnd
        return True

    user.EnumWindows(visit, 0)
    return handles


def reachable(port):
    client = http.client.HTTPConnection("127.0.0.1", port, timeout=.4)
    try:
        client.request("GET", "/api/status")
        return client.getresponse().status == 200
    except OSError:
        return False
    finally:
        client.close()


def smoke(flags, expect_webmap):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    (STAGE / "HirschgangWare.ini").write_text(
        "[HirschgangWare]\ndma=1\nfullscreen=1\ndz_radar_screen=1\n"
        f"webmap_enabled=0\nwebmap_port={port}\n")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    proc = subprocess.Popen([str(APP), *flags], cwd=STAGE, startupinfo=startup)
    try:
        for _ in range(40):
            if "hirschgangware_panel" in classes(proc.pid):
                break
            time.sleep(.1)
        else:
            raise AssertionError("launcher not found")

        time.sleep(1.5)
        assert proc.poll() is None
        assert set(classes(proc.pid)) == {"hirschgangware_panel"}
        log = (STAGE / "HirschgangWare.log").read_text(encoding="utf-8", errors="replace")
        assert " launcher" in log
        assert "waiting for DayZ" not in log
        assert "attach" not in log.lower()
        assert "COM ports present" not in log
        assert reachable(port) == expect_webmap
    finally:
        if proc.poll() is None:
            panel = classes(proc.pid).get("hirschgangware_panel")
            if panel:
                user.PostMessageW(panel, 0x10, 0, 0)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise AssertionError("launcher did not close")
    assert proc.returncode == 0


smoke(["--dma", "--fullscreen"], False)
smoke(["--dma", "--fullscreen", "--webmap"], True)
print("PASS: launcher alone with saved radar flag and legacy switches; independent Webmap and graceful shutdown")

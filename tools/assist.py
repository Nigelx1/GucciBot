"""Claude's client for test runs over Assistant Access (Settings > Assistant Access).

Starts Geometry Dash with safe mode forced on, drives GucciBot's tools, takes
screenshots of the window, and closes the game it started. Python 3, standard
library only, Windows.

    import assist
    assist.launch()                    # refuses if GD is already running
    assist.ensure_safe()
    assist.menu(open=True, page="Settings", theme="SolarBot", effects=False)
    png = assist.shot("solar_settings")  # a path; the Read tool shows it
    assist.menu(theme="")              # the user's own theme back (never saved anyway)
    assist.close()                     # only a game launch() started

Or from a shell:

    python tools/assist.py call gucci_get_state
    python tools/assist.py call gucci_menu open=true "page=JMF Trainer"
    python tools/assist.py shot solar_settings 3
    python tools/assist.py launch | ours | safe | close

Values after key= are read as JSON when they parse (true, 3, "x") and as
text otherwise.

The game plays on the owner's real account, so launch() starts it with
GUCCIBOT_FORCE_SAFE_MODE=1: safe mode on for that launch only, never saved
(hacks/safemode.hpp). Check it with ensure_safe() before any run. A restart
through gucci_restart_game starts a new process, which ours() won't know, and
may not keep the variable: check ensure_safe() again, and gucci_force_safe_mode
turns it on for the rest of that launch.
"""
import csv
import ctypes
import http.client
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
from ctypes import wintypes

URL = "http://127.0.0.1:8790/"
GD_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Geode Geometry Dash"
GD_EXE = os.path.join(GD_DIR, "GeometryDash.exe")
PID_FILE = os.path.join(os.environ.get("TEMP") or tempfile.gettempdir(), "guccibot_assist_pid.txt")

_id = 0


def call(tool, /, **args):
    """Calls one of GucciBot's tools and returns its JSON. Raises on a tool error.

    `tool` is positional-only, so tools with an argument called `name`
    (gucci_load_macro, gucci_open_level, gucci_screenshot) can be given it.
    """
    global _id
    _id += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id, "method": "tools/call",
                       "params": {"name": tool, "arguments": args}}).encode()
    req = urllib.request.Request(URL, body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        res = json.loads(r.read())["result"]
    text = res["content"][0]["text"]
    if res.get("isError"):
        raise RuntimeError(f"{tool}: {text}")
    return json.loads(text)


# ---- processes ---------------------------------------------------------------

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD)]
_k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
_k32.CloseHandle.argtypes = [wintypes.HANDLE]

_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259


def _process(pid):
    """(exe name, creation time) of a running process, or None."""
    h = _k32.OpenProcess(_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        code = wintypes.DWORD()
        if not _k32.GetExitCodeProcess(h, ctypes.byref(code)) or code.value != _STILL_ACTIVE:
            return None
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(len(buf))
        if not _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return None
        times = [wintypes.FILETIME() for _ in range(4)]
        if not _k32.GetProcessTimes(h, *[ctypes.byref(t) for t in times]):
            return None
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return os.path.basename(buf.value), created
    finally:
        _k32.CloseHandle(h)


def _running(exe):
    """PIDs of every running process with this exe name."""
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="replace").stdout
    return [int(row[1]) for row in csv.reader(out.splitlines())
            if len(row) > 1 and row[0].lower() == exe.lower() and row[1].isdigit()]


def _recorded():
    """(pid, creation time) launch() wrote down, or None."""
    try:
        with open(PID_FILE) as f:
            pid, created = f.read().split()
        return int(pid), int(created)
    except (OSError, ValueError):
        return None


def ours():
    """True while the game launch() started is still running.

    The PID it recorded is alive, is GeometryDash.exe, and was started when
    it was then (so a later process given the same number doesn't count).
    """
    rec = _recorded()
    if not rec:
        return False
    now = _process(rec[0])
    return bool(now) and now[0].lower() == os.path.basename(GD_EXE).lower() and now[1] == rec[1]


# ---- the game ----------------------------------------------------------------

def _answers():
    """Assistant Access is up and the game's main thread runs its tools."""
    try:
        call("gucci_get_state")
        return True
    except (OSError, http.client.HTTPException, RuntimeError, ValueError, KeyError):
        return False


def launch(force_safe=True, timeout=180, settle=20):
    """Starts Geometry Dash (the Geode copy) and waits until Assistant Access answers.

    Refuses if any GeometryDash.exe is running already (it may be Nigel's), or
    if Steam isn't (the game can't start without it, and starting Steam is
    Nigel's call). With force_safe, the game's environment has
    GUCCIBOT_FORCE_SAFE_MODE=1. Waits `settle` more seconds once it answers:
    opening a level while Mega Hack is still loading crashed it once.
    Assistant Access has to be on already (it's remembered across launches).
    """
    if _running(os.path.basename(GD_EXE)):
        raise RuntimeError("Geometry Dash is already running; close it first (assist.close() only "
                           "closes a game launch() started)")
    if not _running("steam.exe"):
        raise RuntimeError("Steam isn't running, and the game can't start without it. Starting "
                           "Steam is Nigel's call.")
    if not os.path.exists(GD_EXE):
        raise RuntimeError(f"no game at {GD_EXE}")

    env = dict(os.environ)
    if force_safe:
        env["GUCCIBOT_FORCE_SAFE_MODE"] = "1"
    else:
        env.pop("GUCCIBOT_FORCE_SAFE_MODE", None)
    # The game outlives this script: its own process group, no console, and out
    # of any job this shell runs in where that's allowed.
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    devnull = subprocess.DEVNULL
    try:
        proc = subprocess.Popen([GD_EXE], cwd=GD_DIR, env=env, stdin=devnull, stdout=devnull, stderr=devnull,
                                creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB)
    except OSError:
        proc = subprocess.Popen([GD_EXE], cwd=GD_DIR, env=env, stdin=devnull, stdout=devnull, stderr=devnull,
                                creationflags=flags)
    info = _process(proc.pid)
    if not info:
        raise RuntimeError("the game closed right after starting")
    with open(PID_FILE, "w") as f:
        f.write(f"{proc.pid} {info[1]}")

    start = time.time()
    while not _answers():
        if not ours():
            raise RuntimeError("the game closed while starting (Steam not signed in? Geode restarting "
                               "it for an update? Then the new one isn't launch()'s)")
        if time.time() - start > timeout:
            raise TimeoutError(f"Assistant Access didn't answer within {timeout} s: is it on "
                               "(Settings > Assistant Access)?")
        time.sleep(1.0)
    time.sleep(settle)
    return proc.pid


def ensure_safe():
    """Raises unless safe mode is forced or switched on: nothing a test run does may count."""
    sm = call("gucci_get_state")["safe_mode"]
    if not (sm.get("forced") or sm.get("switch")):
        raise RuntimeError(f"safe mode is off ({sm}); call gucci_force_safe_mode, or launch() with "
                           "force_safe=True")
    return sm


def shot(name, frames=3, timeout=10):
    """A screenshot of the game window; returns the PNG's path once it's on disk."""
    path = call("gucci_screenshot", name=name, frames=frames)["path"]
    start = time.time()
    while time.time() - start < timeout:
        s = call("gucci_screenshot", status=True)
        if s.get("path") == path and s.get("done"):
            if not s.get("ok"):
                raise RuntimeError(f"screenshot {name}: {s.get('error')}")
            return path
        time.sleep(0.1)
    raise TimeoutError(f"screenshot {name} wasn't taken within {timeout} s (is the game minimized?)")


def menu(**kw):
    """gucci_menu: open, page, theme (a test's, never saved; "" ends it), effects."""
    return call("gucci_menu", **kw)


def close(timeout=60):
    """Closes the game launch() started, the way its window's close button does (it saves).

    Refuses for any other game. Waits for it to exit; never kills it.
    """
    if not ours():
        raise RuntimeError("no game launch() started is running; a game someone else started is "
                           "theirs to close")
    pid = _recorded()[0]
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"(Get-Process -Id {pid}).CloseMainWindow() | Out-Null"],
                   capture_output=True, text=True)
    start = time.time()
    while ours():
        if time.time() - start > timeout:
            raise TimeoutError(f"the game didn't close within {timeout} s (a dialog open?)")
        time.sleep(0.5)
    try:
        os.remove(PID_FILE)
    except OSError:
        pass


# ---- command line ------------------------------------------------------------

def _value(text):
    try:
        return json.loads(text)
    except ValueError:
        return text


def _pairs(items):
    out = {}
    for item in items:
        key, eq, value = item.partition("=")
        if not eq:
            raise SystemExit(f"expected key=value, got {item!r}")
        out[key] = _value(value)
    return out


def main(argv):
    usage = ("usage: assist.py call TOOL [key=value...] | menu [key=value...] | shot NAME [FRAMES] | "
             "launch [--no-safe] | ensure_safe | ours | close")
    if not argv:
        raise SystemExit(usage)
    cmd, rest = argv[0], argv[1:]
    if cmd == "call" and rest:
        result = call(rest[0], **_pairs(rest[1:]))
    elif cmd == "menu":
        result = menu(**_pairs(rest))
    elif cmd == "shot" and rest:
        result = shot(rest[0], int(rest[1]) if len(rest) > 1 else 3)
    elif cmd == "launch":
        result = {"pid": launch(force_safe="--no-safe" not in rest)}
    elif cmd in ("ensure_safe", "safe"):
        result = ensure_safe()
    elif cmd == "ours":
        result = ours()
    elif cmd == "close":
        close()
        result = {"closed": True}
    else:
        raise SystemExit(usage)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main(sys.argv[1:])

"""Claude's client for test runs over Assistant Access (Settings > Assistant Access).

Starts Geometry Dash with safe mode forced on, drives GucciBot's tools, takes
screenshots of the window, and closes the game it started. Python 3, standard
library only, Windows.

    import assist
    assist.launch()                    # refuses if GD is already running
    try:
        assist.ensure_safe()
        assist.menu(open=True, page="Settings", theme="SolarBot", effects=False)
        png = assist.shot("solar_settings")  # a path; the Read tool shows it
        assist.menu(theme="")          # the user's own theme back (never saved anyway)
    finally:
        assist.close()                 # only a game launch() started

Or from a shell:

    python tools/assist.py call gucci_get_state
    python tools/assist.py call gucci_menu open=true "page=JMF Trainer"
    python tools/assist.py shot solar_settings 3
    python tools/assist.py launch | restart | ours | safe | close

Values after key= are read as JSON when they parse (true, 3, "x") and as
text otherwise.

The game plays on the owner's real account, so launch() starts it with
GUCCIBOT_FORCE_SAFE_MODE=1 (safe mode on for that launch only, never saved;
hacks/safemode.hpp) and only returns once the game says safe mode is forced.
Every call goes to that game alone (ours()): whatever else answers on the
port may be Nigel's own game, started from Steam while a test still runs, and
call() refuses it. call(tool, _any_game=True) asks it anyway, for a
supervised look with tools that only read.

A new build loads through restart(), not a bare gucci_restart_game: the game
comes back as a new process, which restart() follows (checking safe mode is
still forced) so ours() and close() know it. Close in a finally: a forced game
left open is where Nigel might play next, and his runs wouldn't count.
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
GD_NAME = os.path.basename(GD_EXE).lower()
PID_FILE = os.path.join(os.environ.get("TEMP") or tempfile.gettempdir(), "guccibot_assist_pid.txt")

_id = 0


def call(tool, /, _any_game=False, **args):
    """Calls one of GucciBot's tools and returns its JSON. Raises on a tool error.

    Only in the game launch() started (ours()): anything else answering on
    the port may be Nigel's own game, on his real account. `_any_game=True`
    asks whichever game answers, for a supervised look with tools that only
    read. gucci_restart_game goes through restart(), which follows the game
    into its new process.

    `tool` is positional-only, so tools with an argument called `name`
    (gucci_load_macro, gucci_open_level, gucci_screenshot) can be given it.
    """
    if not _any_game:
        if tool == "gucci_restart_game":
            raise RuntimeError("restart the game with restart(): it follows the game into its new process")
        if not ours():
            raise RuntimeError(f"{tool}: no game launch() started is running, and a game that answers anyway "
                               "may be Nigel's (_any_game=True asks it, for a supervised look that only reads)")
    return _rpc(tool, args)


def _rpc(tool, args):
    """One tools/call to whatever answers on the port."""
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


class _ProcessEntry(ctypes.Structure):  # PROCESSENTRY32W
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260)]


_k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
_k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
_k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]

_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259
_SNAPPROCESS = 0x2
_INVALID_HANDLE = wintypes.HANDLE(-1).value


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


def _processes():
    """{pid: (parent pid, exe name)} of every running process."""
    snap = _k32.CreateToolhelp32Snapshot(_SNAPPROCESS, 0)
    if not snap or snap == _INVALID_HANDLE:
        return {}
    try:
        entry = _ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        out = {}
        more = _k32.Process32FirstW(snap, ctypes.byref(entry))
        while more:
            out[entry.th32ProcessID] = (entry.th32ParentProcessID, entry.szExeFile)
            more = _k32.Process32NextW(snap, ctypes.byref(entry))
        return out
    finally:
        _k32.CloseHandle(snap)


def _recorded():
    """(pid, creation time) launch() (or restart()) wrote down, or None."""
    try:
        with open(PID_FILE) as f:
            pid, created = f.read().split()
        return int(pid), int(created)
    except (OSError, ValueError):
        return None


def _record(pid, created):
    with open(PID_FILE, "w") as f:
        f.write(f"{pid} {created}")


def _alive(rec):
    """The process (pid, creation time) is still running as the GeometryDash.exe it was."""
    now = _process(rec[0])
    return bool(now) and now[0].lower() == GD_NAME and now[1] == rec[1]


def ours():
    """True while the game launch() started (or restart() followed) is still running.

    The PID it recorded is alive, is GeometryDash.exe, and was started when
    it was then (so a later process given the same number doesn't count).
    """
    rec = _recorded()
    return bool(rec) and _alive(rec)


# ---- the game ----------------------------------------------------------------

def _answers():
    """Assistant Access is up in our game and its main thread runs its tools."""
    try:
        call("gucci_get_state")
        return True
    except (OSError, http.client.HTTPException, RuntimeError, ValueError, KeyError):
        return False


def _wait_for_tools(start, timeout):
    """Until our game's Assistant Access answers; `start` is when the wait began (time.time())."""
    while not _answers():
        if not ours():
            raise RuntimeError("the game closed while starting (Steam not signed in? Geode restarting "
                               "it for an update? Then the new one isn't launch()'s)")
        if time.time() - start > timeout:
            raise TimeoutError(f"Assistant Access didn't answer within {timeout} s: is it on "
                               "(Settings > Assistant Access)?")
        time.sleep(1.0)


def _require_forced():
    """Raises unless our game has safe mode forced, forcing it first if the variable didn't reach it."""
    if call("gucci_get_state")["safe_mode"].get("forced"):
        return
    call("gucci_force_safe_mode")
    if not call("gucci_get_state")["safe_mode"].get("forced"):
        raise RuntimeError("safe mode isn't forced in the game, and gucci_force_safe_mode didn't turn it on")


def launch(force_safe=True, timeout=180, settle=20):
    """Starts Geometry Dash (the Geode copy) and waits until Assistant Access answers.

    Refuses if any GeometryDash.exe is running already (it may be Nigel's), or
    if Steam isn't (the game can't start without it, and starting Steam is
    Nigel's call). With force_safe, the game's environment has
    GUCCIBOT_FORCE_SAFE_MODE=1, and launch() doesn't return before the game
    says safe mode is forced. Waits `settle` more seconds once it answers:
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
    _record(proc.pid, info[1])

    _wait_for_tools(time.time(), timeout)
    time.sleep(settle)
    if force_safe:
        _require_forced()
    return proc.pid


def restart(timeout=180, settle=20):
    """Restarts our game (gucci_restart_game, so a new build loads) and follows it.

    Geode restarts through GeodeUpdater.exe: the old game starts it and quits,
    and it starts the new game once the old one has closed. Only a game
    started down that line counts. The new process keeps the old one's
    environment, so safe mode stays forced; restart() checks that (forcing it
    again if it didn't), records the new process for ours() and close(), and
    waits for it as launch() does. Returns the new PID.
    """
    if not ours():
        raise RuntimeError("no game launch() started is running")
    old = _recorded()
    _rpc("gucci_restart_game", {})
    start = time.time()
    line = {old[0]}  # the old game and what it starts: GeodeUpdater.exe while it runs
    while True:
        procs = _processes()
        line |= {pid for pid, (parent, _) in procs.items() if parent in line}
        if not _alive(old):
            for pid, (parent, exe) in procs.items():
                info = _process(pid) if exe.lower() == GD_NAME and parent in line else None
                if info and info[1] > old[1]:
                    _record(pid, info[1])
                    _wait_for_tools(start, timeout)
                    time.sleep(settle)
                    _require_forced()
                    return pid
        if time.time() - start > timeout:
            raise TimeoutError(f"the game didn't come back within {timeout} s of gucci_restart_game")
        time.sleep(0.25)


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
    usage = ("usage: assist.py call [--any-game] TOOL [key=value...] | menu [key=value...] | "
             "shot NAME [FRAMES] | launch [--no-safe] | restart | ensure_safe | ours | close")
    if not argv:
        raise SystemExit(usage)
    cmd, rest = argv[0], argv[1:]
    any_game = cmd == "call" and rest[:1] == ["--any-game"]
    if any_game:
        rest = rest[1:]
    if cmd == "call" and rest:
        result = call(rest[0], _any_game=any_game, **_pairs(rest[1:]))
    elif cmd == "menu":
        result = menu(**_pairs(rest))
    elif cmd == "shot" and rest:
        result = shot(rest[0], int(rest[1]) if len(rest) > 1 else 3)
    elif cmd == "launch":
        result = {"pid": launch(force_safe="--no-safe" not in rest)}
    elif cmd == "restart":
        result = {"pid": restart()}
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

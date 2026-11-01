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

A test batch (the 2026-11-01 test plans) also proves it left Nigel's files
alone:

    before = assist.snapshot()         # saved.json, GD's saves, the mod's folders
    assist.copy_macro("Clubstep 2", "zz_gbtest_cs")   # tests use copies only
    assist.launch()
    try:
        assist.ensure_safe()
        assist.test_set("test_mute", True)             # gucci_test_set, never saved
        mark = assist.log_mark("guccibot_fw.log")
        ...
        lines = assist.log_since(mark, grep="[fw]")
    finally:
        assist.close()                 # gucci_test_set reset first
    assist.sweep()                     # every zz_gbtest_ file, wherever it went
    assist.check(before, assist.snapshot())   # raises SavesChanged

check() fails on any change outside the allowlist, which learn() builds from a
no-op session (launch, ensure_safe, close): GD rewrites its saves on every
close, so only the keys a no-op session changes may change.
"""
import base64
import contextlib
import csv
import ctypes
import fnmatch
import gzip
import hashlib
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import xml.etree.ElementTree as ET
import zlib
from ctypes import wintypes

URL = "http://127.0.0.1:8790/"
GD_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Geode Geometry Dash"
GD_EXE = os.path.join(GD_DIR, "GeometryDash.exe")
GD_NAME = os.path.basename(GD_EXE).lower()
PID_FILE = os.path.join(os.environ.get("TEMP") or tempfile.gettempdir(), "guccibot_assist_pid.txt")
# Geode's own records, only ever read: its mod list (which mods load) and its logs.
GEODE_MOD_LIST = os.path.join(os.environ.get("LOCALAPPDATA", ""), "GeometryDash", "geode", "mods",
                              "geode.loader", "saved.json")
GEODE_LOGS = os.path.join(GD_DIR, "geode", "logs")
MOD_ID = "nigelx1.guccibot"

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


def _switched_off():
    """GucciBot is switched off in Geode's mod list, so the game would start without it."""
    try:
        with open(GEODE_MOD_LIST, encoding="utf-8") as f:
            return json.load(f).get(f"should-load-{MOD_ID}") is False
    except (OSError, ValueError, AttributeError):
        return False


def _why_silent(since):
    """Why Assistant Access hasn't answered in a game started at `since` (time.time())."""
    try:
        logs = [os.path.join(GEODE_LOGS, n) for n in os.listdir(GEODE_LOGS) if n.endswith(".log")]
        newest = max(logs, key=os.path.getmtime)
        if os.path.getmtime(newest) >= since:
            with open(newest, encoding="utf-8", errors="replace") as f:
                if "[GucciBot] BUILD:" not in f.read():
                    return (f"GucciBot didn't load in this launch (no '[GucciBot] BUILD:' line in "
                            f"{os.path.basename(newest)}): switched off in Geode, or it failed to load")
    except (OSError, ValueError):
        pass
    return "is it on (Settings > Assistant Access)?"


def _wait_for_tools(start, timeout):
    """Until our game's Assistant Access answers; `start` is when the wait began (time.time())."""
    while not _answers():
        if not ours():
            raise RuntimeError("the game closed while starting (Steam not signed in? Geode restarting "
                               "it for an update? Then the new one isn't launch()'s)")
        if time.time() - start > timeout:
            raise TimeoutError(f"Assistant Access didn't answer within {timeout} s: {_why_silent(start)}")
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

    Refuses if any GeometryDash.exe is running already (it may be Nigel's), if
    Steam isn't (the game can't start without it, and starting Steam is
    Nigel's call), or if GucciBot is switched off in Geode's mod list (which
    only Nigel changes). Raises if the game closes or restarts before it has
    settled. With force_safe, the game's environment has
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
    if _switched_off():
        raise RuntimeError("GucciBot is switched off in Geode's mod list (geode.loader/saved.json), so the "
                           "game would start without it. Switching it on is Nigel's call.")

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
    # Still the game it started, still answering, once settled: one that
    # closed or restarted meanwhile (a restart from inside it, an update)
    # isn't launch()'s, and whatever runs now is left alone.
    end = time.time() + settle
    while time.time() < end and ours():
        time.sleep(0.5)
    if not ours() or not _answers():
        now = _running(os.path.basename(GD_EXE))
        raise RuntimeError("the game closed or restarted while settling" +
                           (f"; the Geometry Dash running now (PID {', '.join(map(str, now))}) isn't one "
                            "launch() started, so leave it alone" if now else ""))
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


def test_set(key=None, value=None, reset=False, list=False):
    """gucci_test_set: one setting for this launch only, never saved (refused unless safe mode is forced).

    test_set("hack_force_platformer", True); test_set(reset=True); test_set(list=True).
    """
    if reset:
        return call("gucci_test_set", reset=True)
    if list:
        return call("gucci_test_set", list=True)
    return call("gucci_test_set", key=key, value=value)


# Set by close(): how its gucci_test_set reset went (None when it answered fine).
last_reset_error = None


def close(timeout=60):
    """Closes the game launch() started, the way its window's close button does (it saves).

    Puts every gucci_test_set setting back first (they never reach the disk
    anyway; this keeps the game's memory clean too). Refuses for any other
    game. Waits for it to exit; never kills it.
    """
    global last_reset_error
    if not ours():
        raise RuntimeError("no game launch() started is running; a game someone else started is "
                           "theirs to close")
    last_reset_error = None
    try:
        call("gucci_test_set", reset=True)
    except (OSError, http.client.HTTPException, RuntimeError, ValueError, KeyError) as e:
        last_reset_error = str(e)  # an older build has no gucci_test_set: nothing to put back
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


# ---- Nigel's files: snapshots, the allowlist, test copies, logs --------------
#
# Only ever read, except the zz_gbtest_ files a test makes itself.

GD_SAVE_DIR = os.path.join(os.environ.get("LOCALAPPDATA", ""), "GeometryDash")
MOD_SAVE_DIR = os.path.join(GD_SAVE_DIR, "geode", "mods", MOD_ID)
MOD_CONFIG_DIR = os.path.join(GD_SAVE_DIR, "geode", "config", MOD_ID)
SAVED_JSON = os.path.join(MOD_SAVE_DIR, "saved.json")
REPLAYS = os.path.join(MOD_SAVE_DIR, "replays")
GD_SAVES = ("CCGameManager.dat", "CCGameManager2.dat", "CCLocalLevels.dat", "CCLocalLevels2.dat")
TEST_PREFIX = "zz_gbtest_"
# Learned from a no-op session on this PC, so it lives with the PID file, not in the repo.
ALLOWLIST = os.path.join(os.path.dirname(PID_FILE), "guccibot_assist_allowlist.json")
# The mod folder's files a snapshot leaves out: logs grow, shots are the test's own.
_TREE_SKIP = ("*.log", "screenshots/*", "saved.json")


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def gd_decode(path):
    """A GD save (.dat) as its XML text: XOR 11, base64, gzip. Read only."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw.lstrip().startswith(b"<?xml"):
        return raw.decode("utf-8", "replace")
    x = bytes(b ^ 11 for b in raw).rstrip(b"\x00").strip()
    x = x.replace(b"-", b"+").replace(b"_", b"/")
    data = base64.b64decode(x + b"=" * (-len(x) % 4))
    try:
        return gzip.decompress(data).decode("utf-8", "replace")
    except (OSError, EOFError, zlib.error):
        return zlib.decompress(data[10:], -15).decode("utf-8", "replace")


def gd_flatten(xml_text):
    """{key path: value} from GD's plist (<k>key</k> then <s>/<i>/<r>/<t/>/<d>...).

    Paths join dict keys with '/': GLM_03/12345/k18. Values over 64 characters
    (level strings) are kept as a hash, so a snapshot stays small.
    """
    root = ET.fromstring(xml_text)
    top = root[0] if len(root) else root
    out = {}

    def walk(d, prefix):
        children = list(d)
        i = 0
        while i < len(children):
            k = children[i]
            if k.tag != "k":
                i += 1
                continue
            path = prefix + (k.text or "")
            v = children[i + 1] if i + 1 < len(children) else None
            i += 2
            if v is None:
                out[path] = None
            elif v.tag in ("d", "dict"):
                walk(v, path + "/")
            elif v.tag in ("t", "true"):
                out[path] = "true"
            elif v.tag in ("f", "false"):
                out[path] = "false"
            else:
                text = v.text or ""
                out[path] = text if len(text) <= 64 else "sha:" + _sha(text.encode("utf-8", "replace"))[:16]

    walk(top, "")
    return out


def _tree(base):
    """{relative path: sha256} of every file under base (but _TREE_SKIP)."""
    out = {}
    if not os.path.isdir(base):
        return out
    for dirpath, _, files in os.walk(base):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, base).replace("\\", "/")
            if any(fnmatch.fnmatch(rel, p) for p in _TREE_SKIP):
                continue
            try:
                h = hashlib.sha256()
                with open(full, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                out[rel] = h.hexdigest()
            except OSError as e:
                out[rel] = f"unreadable: {e}"
    return out


def snapshot():
    """Everything a test must leave as it was, read only: saved.json by key, GD's saves by
    key (decoded), and every file in GucciBot's save and config folders by hash."""
    snap = {"time": time.time(), "saved_json": None, "gd": {}, "gd_sha": {},
            "mod_files": _tree(MOD_SAVE_DIR), "config_files": _tree(MOD_CONFIG_DIR)}
    try:
        with open(SAVED_JSON, encoding="utf-8") as f:
            snap["saved_json"] = {k: json.dumps(v, sort_keys=True) for k, v in json.load(f).items()}
    except (OSError, ValueError) as e:
        snap["saved_json_error"] = str(e)
    for name in GD_SAVES:
        path = os.path.join(GD_SAVE_DIR, name)
        if not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            snap["gd_sha"][name] = _sha(f.read())
        try:
            snap["gd"][name] = gd_flatten(gd_decode(path))
        except (ValueError, OSError, zlib.error, ET.ParseError) as e:
            snap["gd"][name] = None  # compared by bytes only
            snap.setdefault("gd_errors", {})[name] = str(e)
    return snap


def _keydiff(a, b):
    a, b = a or {}, b or {}
    return sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k) or (k in a) != (k in b))


def diff(before, after):
    """What changed between two snapshots, as lists of names:
    {"saved_json": [keys], "gd": {file: [key paths]}, "mod_files": [...], "config_files": [...]}."""
    out = {"saved_json": _keydiff(before.get("saved_json"), after.get("saved_json")), "gd": {},
           "mod_files": _keydiff(before.get("mod_files"), after.get("mod_files")),
           "config_files": _keydiff(before.get("config_files"), after.get("config_files"))}
    for name in sorted(set(before.get("gd_sha", {})) | set(after.get("gd_sha", {}))):
        a, b = before.get("gd", {}).get(name), after.get("gd", {}).get(name)
        if a is not None and b is not None:
            changed = _keydiff(a, b)
        else:  # one side couldn't be decoded: the whole file, by bytes
            changed = ["<bytes>"] if before.get("gd_sha", {}).get(name) != after.get("gd_sha", {}).get(name) else []
        if changed:
            out["gd"][name] = changed
    return out


def _flat(d):
    """diff()'s result as 'area:name' strings, the allowlist's form."""
    items = [f"saved_json:{k}" for k in d["saved_json"]]
    items += [f"gd:{name}:{k}" for name, keys in d["gd"].items() for k in keys]
    items += [f"mod_files:{k}" for k in d["mod_files"]]
    items += [f"config_files:{k}" for k in d["config_files"]]
    return items


def load_allowlist(path=ALLOWLIST):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def learn(before, after, path=ALLOWLIST, fresh=False):
    """Adds what a no-op session changed (launch, ensure_safe, close: no test) to the allowlist.

    Entries are 'area:name' and may hold fnmatch wildcards added by hand, e.g.
    'gd:CCGameManager.dat:GS_value/*'. Returns the allowlist.
    """
    allow = [] if fresh else load_allowlist(path)
    for item in _flat(diff(before, after)):
        if item not in allow:
            allow.append(item)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(sorted(allow), f, indent=1)
    return sorted(allow)


class SavesChanged(RuntimeError):
    """A batch changed Nigel's files outside the allowlist; `.unexpected` lists what."""

    def __init__(self, unexpected):
        self.unexpected = unexpected
        more = f" (+{len(unexpected) - 20} more)" if len(unexpected) > 20 else ""
        super().__init__("changed outside the allowlist: " + ", ".join(unexpected[:20]) + more)


def check(before, after, allow=None):
    """Raises SavesChanged on any change outside the allowlist (default: load_allowlist()).

    Fails with no allowlist at all: learn() one from a no-op session first.
    Returns the changes it allowed.
    """
    if allow is None:
        allow = load_allowlist()
        if not allow and not os.path.exists(ALLOWLIST):
            raise RuntimeError(f"no allowlist at {ALLOWLIST}: learn() one from a no-op session first")
    items = _flat(diff(before, after))
    unexpected = [i for i in items if not any(fnmatch.fnmatchcase(i, p) for p in allow)]
    if unexpected:
        raise SavesChanged(unexpected)
    return items


def _test_only(path):
    """Raises unless the file's own name is a test's (zz_gbtest_...)."""
    if TEST_PREFIX not in os.path.basename(path):
        raise RuntimeError(f"not a test file: {path} (test files are named {TEST_PREFIX}...)")
    return path


def copy_macro(source, test_name):
    """Copies replays/<source>.<ext> (the macro file only, no sidecars) to replays/<test_name>.<ext>.

    test_name must start with zz_gbtest_. Returns the copy's path.
    """
    if not test_name.startswith(TEST_PREFIX):
        raise RuntimeError(f"test copies are named {TEST_PREFIX}...: {test_name!r}")
    found = [n for n in os.listdir(REPLAYS)
             if os.path.isfile(os.path.join(REPLAYS, n)) and os.path.splitext(n)[0] == source
             and not n.endswith(".bak")]
    if len(found) != 1:
        raise RuntimeError(f"no single macro called {source!r} in {REPLAYS}: {found}")
    dst = _test_only(os.path.join(REPLAYS, test_name + os.path.splitext(found[0])[1]))
    shutil.copyfile(os.path.join(REPLAYS, found[0]), dst)
    return dst


def test_path(*parts):
    """A path under GucciBot's save folder for a test's own file or folder (a click pack,
    a custom theme): the last part must start with zz_gbtest_. Makes the parent folders."""
    path = os.path.join(MOD_SAVE_DIR, *parts)
    if not parts[-1].startswith(TEST_PREFIX):
        raise RuntimeError(f"test files are named {TEST_PREFIX}...: {parts[-1]!r}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def sweep():
    """Deletes every test file (zz_gbtest_ in its name) wherever GucciBot puts copies:
    replays and its sidecars, converted, imported and exports folders, click packs,
    custom themes, renders and render presets. Returns what it removed."""
    places = [REPLAYS] + [os.path.join(REPLAYS, d) for d in ("sidecars", "converted", "imported", "exports")]
    places += [os.path.join(MOD_SAVE_DIR, d) for d in ("clickpacks", "customthemes", "renders", "presets")]
    places += [os.path.join(MOD_CONFIG_DIR, "render_presets")]
    removed = []
    for place in places:
        if not os.path.isdir(place):
            continue
        for name in os.listdir(place):
            if TEST_PREFIX not in name:
                continue
            path = _test_only(os.path.join(place, name))
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
            removed.append(path)
    return removed


def log_path(name="geode"):
    """'geode' is Geode's newest log; anything else is a bare .log name in GucciBot's save folder."""
    if name == "geode":
        logs = [os.path.join(GEODE_LOGS, n) for n in os.listdir(GEODE_LOGS) if n.endswith(".log")]
        return max(logs, key=os.path.getmtime)
    if os.path.basename(name) != name or not name.endswith(".log"):
        raise RuntimeError(f"a bare .log name: {name!r}")
    return os.path.join(MOD_SAVE_DIR, name)


def log_mark(name="geode"):
    """Where a log ends now: (path, byte offset). Read what comes after with log_since()."""
    path = log_path(name)
    try:
        return path, os.path.getsize(path)
    except OSError:
        return path, 0


def log_since(mark, grep=None, max_bytes=16 << 20):
    """The lines a log gained since log_mark() (read from disk, not through the game:
    guccibot_fw.log runs to hundreds of MB). Only lines containing `grep`, if given;
    at most the last max_bytes. A log that shrank (a new file) is read from the start."""
    path, offset = mark
    try:
        size = os.path.getsize(path)
    except OSError:
        return []
    if size < offset:
        offset = 0
    offset = max(offset, size - max_bytes)
    with open(path, "rb") as f:
        f.seek(offset)
        text = f.read(size - offset).decode("utf-8", "replace")
    lines = text.splitlines()
    return [ln for ln in lines if grep in ln] if grep else lines


@contextlib.contextmanager
def batch(macros=(), mute=True, allow=None):
    """One test batch's frame: sweep, snapshot, copy the test macros, launch, ensure_safe,
    mute (gucci_test_set test_mute); then always close (reset first), sweep and check().

    macros: (source name, zz_gbtest_ name) pairs. Raises SavesChanged after the
    batch if anything outside the allowlist changed.
    """
    sweep()
    before = snapshot()
    for source, test_name in macros:
        copy_macro(source, test_name)
    try:
        launch()
        ensure_safe()
        if mute:
            test_set("test_mute", True)
        yield before
    finally:
        if ours():  # also a game launch() started and then gave up on
            close()
        sweep()
    check(before, snapshot(), allow)


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
             "shot NAME [FRAMES] | launch [--no-safe] | restart | ensure_safe | ours | close | "
             "snapshot FILE | diff BEFORE AFTER | learn BEFORE AFTER | check BEFORE AFTER | sweep | "
             "copy SOURCE ZZ_GBTEST_NAME | test_set KEY VALUE | test_set reset | test_set list")
    if argv and argv[0] in ("snapshot", "diff", "learn", "check"):
        def read(p):
            with open(p, encoding="utf-8") as f:
                return json.load(f)
        if argv[0] == "snapshot" and len(argv) == 2:
            with open(argv[1], "w", encoding="utf-8") as f:
                json.dump(snapshot(), f)
            print(json.dumps({"saved": argv[1]}))
            return
        if len(argv) != 3:
            raise SystemExit(usage)
        before, after = read(argv[1]), read(argv[2])
        if argv[0] == "diff":
            print(json.dumps(diff(before, after), indent=2))
        elif argv[0] == "learn":
            print(json.dumps({"allowlist": ALLOWLIST, "entries": len(learn(before, after))}))
        else:
            print(json.dumps({"ok": True, "allowed": check(before, after)}, indent=2))
        return
    if argv and argv[0] == "sweep":
        print(json.dumps({"removed": sweep()}, indent=2))
        return
    if argv and argv[0] == "copy" and len(argv) == 3:
        print(json.dumps({"copied": copy_macro(argv[1], argv[2])}))
        return
    if argv and argv[0] == "test_set" and len(argv) >= 2:
        if argv[1] in ("reset", "list") and len(argv) == 2:
            result = test_set(**{argv[1]: True})
        elif len(argv) == 3:
            result = test_set(argv[1], _value(argv[2]))
        else:
            raise SystemExit(usage)
        print(json.dumps(result, indent=2))
        return
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

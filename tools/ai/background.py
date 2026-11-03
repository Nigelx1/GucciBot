r"""Training in the background on Nigel's PC (TOOLCHAIN.md 4.4, tested 2026-11-02).

- Every trainer lowers its own priority to BelowNormal (--priority below, the
  default), so the game and the desktop come first.
- Threads: --threads auto is 3 while Geometry Dash runs (Nigel plays; 3 keeps
  83-85% of the 6-thread speed and leaves 3 cores for GD) and 6 when it
  doesn't. Never 12: SMT buys nothing here.
- progress.json is rewritten atomically (write, then rename) every few
  seconds; checkpoints are written the same way, during an epoch too
  (--ckpt-every-s), so a crash or a reboot loses minutes, not an epoch.
- A STOP file in the run's output folder makes the trainer checkpoint and exit
  cleanly (progress state "stopped"); it deletes the STOP file, so running the
  same command again resumes where it stopped.
- launch() starts a command with Invoke-CimMethod Win32_Process Create: the
  process is created by WMI, outside any job object and outside Claude's
  process tree, so it outlives the session that started it.

    python tools/ai/background.py launch --log RUN.log -- tools/ai/train_pilot.py --out DIR --threads auto
    python tools/ai/background.py stop DIR          # asks the run writing DIR\progress.json to stop
    python tools/ai/background.py status DIR        # its progress.json, briefly
    python tools/ai/background.py run --log RUN.log -- SCRIPT ARGS...   (what launch starts)

Datasets and runs belong outside OneDrive (the repo lives in OneDrive, which
is already the PC's biggest CPU and memory user): the trainers write under
AI_ROOT\train\ (%LOCALAPPDATA%), not the repo.
"""
import argparse
import ctypes
import json
import os
import runpy
import subprocess
import sys
import time

STOP_NAME = "STOP"
GAME_EXE = "GeometryDash.exe"
# Real interpreter on Nigel's PC; `python` on PATH is the install manager's shim (TOOLCHAIN.md section 0).
KNOWN_PYTHON = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Python", "pythoncore-3.14-64", "python.exe")


class StopRequested(Exception):
    """A STOP file asked the run to checkpoint and exit."""


def set_below_normal_priority():
    """This process to BelowNormal priority (nice 10 off Windows). True when it worked."""
    try:
        if os.name == "nt":
            import ctypes.wintypes as wt
            k32 = ctypes.windll.kernel32
            # Without these, ctypes passes the pseudo-handle -1 as a 32-bit int and the call fails.
            k32.GetCurrentProcess.restype = wt.HANDLE
            k32.SetPriorityClass.argtypes = [wt.HANDLE, wt.DWORD]
            k32.SetPriorityClass.restype = wt.BOOL
            return bool(k32.SetPriorityClass(k32.GetCurrentProcess(), 0x00004000))  # BELOW_NORMAL_PRIORITY_CLASS
        os.nice(10)
        return True
    except (OSError, AttributeError):
        return False


def game_running():
    """True while any Geometry Dash process runs (Windows; False elsewhere or when it can't tell)."""
    if os.name != "nt":
        return False
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME_EXE}", "/NH", "/FO", "CSV"],
                             capture_output=True, text=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return GAME_EXE.lower() in out.lower()


def resolve_threads(value):
    """--threads: a number, or 'auto' (3 while Geometry Dash runs, else 6)."""
    if str(value).strip().lower() == "auto":
        return 3 if game_running() else 6
    n = int(value)
    if n < 1:
        raise ValueError(f"--threads {value}: at least 1")
    return n


def stop_path(out):
    return os.path.join(out, STOP_NAME)


def stop_requested(out):
    return bool(out) and os.path.exists(stop_path(out))


def clear_stop(out):
    try:
        os.remove(stop_path(out))
    except FileNotFoundError:
        pass


def request_stop(out):
    """Creates the STOP file; the trainer checkpoints and exits at its next step."""
    os.makedirs(out, exist_ok=True)
    with open(stop_path(out), "w", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%dT%H:%M:%S") + "\n")
    return stop_path(out)


def setup(args, log=print):
    """What every trainer does first: priority, threads (torch and numpy's BLAS). Returns the threads."""
    import models  # torch is only imported by the trainers
    if getattr(args, "priority", "below") == "below":
        if not set_below_normal_priority():
            log("NOTE: couldn't lower this process's priority")
    n = resolve_threads(getattr(args, "threads", 6))
    models.set_threads(n)
    args.threads = n
    return n


def add_args(ap):
    """The background options every trainer takes."""
    ap.add_argument("--threads", default="auto", help="CPU threads, or auto: 3 while Geometry Dash runs, else 6")
    ap.add_argument("--priority", choices=["below", "normal"], default="below",
                    help="process priority (below = BelowNormal, so the game comes first)")
    ap.add_argument("--ckpt-every-s", type=float, default=300.0,
                    help="also checkpoint mid-epoch this often (seconds; 0 = only at epoch ends)")


def python_exe():
    """The real interpreter (a detached process can't go through the PATH shim)."""
    exe = getattr(sys, "_base_executable", None) or sys.executable
    if os.path.basename(os.path.dirname(exe)).lower() == "bin" and os.path.exists(KNOWN_PYTHON):
        return KNOWN_PYTHON
    return exe


def _quote(a):
    a = str(a)
    return '"' + a.replace('"', '\\"') + '"' if (not a or any(c in a for c in ' \t"')) else a


def launch(script_args, log_path, cwd=None, dry_run=False):
    """Starts `python background.py run --log LOG -- script_args...` outside any job (Invoke-CimMethod
    Win32_Process Create). Returns {"pid", "command"}; dry_run returns the command only."""
    if os.name != "nt":
        raise RuntimeError("launch() is for Windows (Invoke-CimMethod); elsewhere use nohup")
    here = os.path.abspath(__file__)
    cmd = " ".join(_quote(a) for a in [python_exe(), here, "run", "--log", os.path.abspath(log_path), "--"]
                   + [os.path.abspath(script_args[0])] + list(script_args[1:]))
    cwd = os.path.abspath(cwd or os.path.dirname(here))
    if dry_run:
        return {"pid": None, "command": cmd, "cwd": cwd}
    ps_cmd = cmd.replace("'", "''")
    ps = ("$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
          f"@{{CommandLine = '{ps_cmd}'; CurrentDirectory = '{cwd.replace(chr(39), chr(39) * 2)}'}}; "
          "Write-Output ('{0} {1}' -f $r.ReturnValue, $r.ProcessId)")
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True,
                         text=True, timeout=60)
    parts = (out.stdout or "").split()
    if out.returncode != 0 or len(parts) < 2 or parts[0] != "0":
        raise RuntimeError(f"Invoke-CimMethod failed: {out.stdout.strip()} {out.stderr.strip()}")
    return {"pid": int(parts[1]), "command": cmd, "cwd": cwd}


def run(log_path, script, script_argv):
    """Runs a trainer script in this process with stdout/stderr appended to log_path (what launch starts)."""
    os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
    f = open(log_path, "a", encoding="utf-8", buffering=1)
    sys.stdout = sys.stderr = f
    print(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} pid {os.getpid()}: {script} {' '.join(script_argv)}")
    set_below_normal_priority()
    sys.argv = [script] + list(script_argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
    code = 0
    try:
        runpy.run_path(script, run_name="__main__")
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
        if e.code not in (None, 0) and not isinstance(e.code, int):
            print(e.code)
    except BaseException:
        import traceback
        traceback.print_exc()
        code = 1
    print(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} exit {code}")
    return code


def main(argv=None):
    ap = argparse.ArgumentParser(description="Run GucciBot AI training in the background.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("launch", help="start a trainer detached (outlives this session)")
    p.add_argument("--log", required=True)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("rest", nargs=argparse.REMAINDER)
    p = sub.add_parser("run", help="run a trainer here with output to a log (what launch starts)")
    p.add_argument("--log", required=True)
    p.add_argument("rest", nargs=argparse.REMAINDER)
    p = sub.add_parser("stop", help="ask a run to checkpoint and exit")
    p.add_argument("out")
    p = sub.add_parser("status", help="a run's progress.json")
    p.add_argument("out")
    a = ap.parse_args(argv)
    rest = list(getattr(a, "rest", []) or [])
    if rest and rest[0] == "--":
        rest = rest[1:]
    if a.cmd == "launch":
        if not rest:
            ap.error("launch needs a script after --")
        print(json.dumps(launch(rest, a.log, dry_run=a.dry_run)))
        return 0
    if a.cmd == "run":
        if not rest:
            ap.error("run needs a script after --")
        return run(a.log, rest[0], rest[1:])
    if a.cmd == "stop":
        print(f"asked to stop: {request_stop(a.out)}")
        return 0
    path = os.path.join(a.out, "progress.json")
    try:
        with open(path, encoding="utf-8") as f:
            p = json.load(f)
    except (OSError, ValueError) as e:
        print(f"no progress at {path}: {e}")
        return 1
    print(json.dumps({k: p.get(k) for k in ("model", "state", "pid", "epoch", "epochs", "step", "train_loss",
                                            "val_loss", "eta", "threads", "updated", "message")}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

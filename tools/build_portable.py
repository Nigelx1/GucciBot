"""TEST ONLY: builds GucciBot for Windows with the off-Windows engine (2026-11-15).

    python tools/build_portable.py [source dir] [--lock <lock file>] [--deps <dir with *-src folders>]

CMake option GB_FORCE_PORTABLE=ON makes GB_NATIVE_ENGINE 0 on Windows (src/core/platform.hpp): the tick
runs through GJBaseGameLayer::processCommands (GBPortableTick), TPS through portable_tps, checkpoint
placement and teleports through the portable hooks, and the World (trigger model) stays off, as on macOS
and Android. A hidden Windows test game can then exercise those paths. It is NOT the Mac or Android
build: the compiler, the standard library and GD's own code are still Windows', so it shows logic, not
platform behavior.

- Builds into <source>/build-portable, never <source>/build, so the normal build is untouched.
- -DGEODE_DONT_INSTALL_MODS=ON: it never installs into Geometry Dash. Install the .geode into a test
  copy only, and never ship it (the mod logs "TEST build" at startup).
- --lock: a lock file to hold while building (one build at a time on a PC that shares C:\\GeodeSDK\\build).
- --deps: build FetchContent dependencies offline from an existing build's _deps folder.
Prints "Build complete: <path>" on success.
"""
import glob
import os
import subprocess
import sys
import tempfile
import time


def main():
    args = sys.argv[1:]
    lock = deps = None
    src = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    i = 0
    while i < len(args):
        if args[i] == "--lock":
            lock, i = args[i + 1], i + 2
        elif args[i] == "--deps":
            deps, i = args[i + 1], i + 2
        else:
            src, i = os.path.abspath(args[i]), i + 1
    build = os.path.join(src, "build-portable")
    defs = ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DGEODE_DONT_INSTALL_MODS=ON", "-DGB_FORCE_PORTABLE=ON"]
    if deps:
        defs.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        for d in sorted(glob.glob(os.path.join(deps, "*-src"))):
            name = os.path.basename(d)[:-4].upper()
            defs.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={d.replace(os.sep, '/')}")
        os.makedirs(os.path.join(build, "cmake"), exist_ok=True)
        for f in glob.glob(os.path.join(os.path.dirname(deps), "cmake", "CPM_*.cmake")):
            dst = os.path.join(build, "cmake", os.path.basename(f))
            if not os.path.exists(dst):
                with open(f, "rb") as a, open(dst, "wb") as b:
                    b.write(a.read())
    lines = ["@echo off", "setlocal EnableDelayedExpansion",
             'if not defined GEODE_SDK set "GEODE_SDK=C:\\GeodeSDK"',
             'set "VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe"',
             'for /f "delims=" %%i in (\'"%VSWHERE%" -latest -property installationPath\') do set "VS_PATH=%%i"',
             'call "!VS_PATH!\\VC\\Auxiliary\\Build\\vcvarsall.bat" amd64 >nul 2>&1',
             f'cd /d "{src}" || exit /b 1',
             'cmake -S . -B build-portable -G Ninja ' + " ".join(f'"{x}"' for x in defs),
             'if errorlevel 1 (echo CMake configure failed. & exit /b 1)',
             'cmake --build build-portable --config RelWithDebInfo -- -k 0',  # every error at once
             'if errorlevel 1 (echo Build failed. & exit /b 1)',
             'exit /b 0']
    bat = os.path.join(tempfile.gettempdir(), f"gb_portable_build_{os.getpid()}.bat")
    with open(bat, "w") as f:
        f.write("\r\n".join(lines) + "\r\n")
    if lock:
        while True:
            try:
                fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, f"build-portable {os.getpid()} {time.strftime('%H:%M:%S')}\n".encode())
                os.close(fd)
                break
            except FileExistsError:
                try:
                    if time.time() - os.path.getmtime(lock) > 90 * 60:
                        os.remove(lock)
                        continue
                except OSError:
                    pass
                time.sleep(5)
    try:
        t0 = time.time()
        r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True, errors="replace")
        out = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0:
            errs = [ln for ln in out.splitlines() if " error " in ln or "error C" in ln or "FAILED:" in ln][:80]
            print("BUILD FAILED after %.0f s\n%s\n--- last lines ---\n%s"
                  % (time.time() - t0, "\n".join(errs), "\n".join(out.splitlines()[-40:])))
            return 1
        geodes = sorted(glob.glob(os.path.join(build, "**", "*.geode"), recursive=True), key=os.path.getmtime)
        if not geodes:
            print("no .geode produced\n" + "\n".join(out.splitlines()[-40:]))
            return 1
        print("Build complete: %s (%.0f s)" % (geodes[-1], time.time() - t0))
        return 0
    finally:
        if lock:
            try:
                os.remove(lock)
            except OSError:
                pass
        try:
            os.remove(bat)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())

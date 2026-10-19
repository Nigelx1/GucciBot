"""Play macros in the real game over Assistant Access and report where each dies.

Checks Calculate against the game: make copies of a macro with one click moved
(tools/shift_click.cpp), then

    python tools/calc_check.py --end-frame 3760 VSC "VSC c81 m1" "VSC c81 p1"

Needs the level open in the game and Assistant Access on (Settings). Each
macro is played from the start; the level is restarted twice, because the
first attempt after restarting straight out of a finished replay can desync
(2026-10-19, open bug). The first macro named is loaded again at the end.
"""
import argparse
import json
import time
import urllib.request

URL = "http://127.0.0.1:8790/"
_id = 0


def call(name, **args):
    global _id
    _id += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(URL, body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        res = json.loads(r.read())["result"]
    text = res["content"][0]["text"]
    if res.get("isError"):
        raise RuntimeError(f"{name}: {text}")
    return json.loads(text)


def play(name, end_frame, limit_s):
    call("gucci_set_paused", paused=False)
    call("gucci_set_mode", mode="idle")
    call("gucci_load_macro", name=name)
    call("gucci_set_mode", mode="play")
    call("gucci_restart_level")
    time.sleep(1.0)
    call("gucci_restart_level")
    t0 = time.time()
    last, started = -1, False
    while time.time() - t0 < limit_s:
        s = call("gucci_get_state")
        f, p = s["frame"], s["player1"]
        if f < 50:
            started = True
        if started and p.get("dead"):
            return f"died at frame {f}, x={p['x']:.1f} ({p['percent']:.1f}%)"
        if started and last > 100 and f < last - 50:
            return f"restarted (died) after frame {last}"
        if started and f >= end_frame:
            return f"survived to frame {f}"
        last = f
        time.sleep(0.03)
    return f"timed out at frame {last}"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--end-frame", type=int, required=True, help="a frame just past the level's end")
    ap.add_argument("--limit", type=float, default=60.0, help="seconds per run")
    ap.add_argument("macros", nargs="+")
    a = ap.parse_args()
    for m in a.macros:
        print(f"{m:28} -> {play(m, a.end_frame, a.limit)}", flush=True)
    call("gucci_set_mode", mode="idle")
    call("gucci_load_macro", name=a.macros[0])

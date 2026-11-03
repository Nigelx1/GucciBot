"""The whole AI loop, driven through the game: record the human macros, train, install the
weights, let the AI play the showcase levels, and report.

Every step that needs the game goes through tools/assist.py: it launches its
own Geometry Dash with safe mode forced (refusing while any Geometry Dash is
running, since that one may be Nigel's), checks safe mode is on, mutes it
(gucci_test_set test_mute, never saved) and closes it in a finally. It never
talks to a game it didn't start. Training runs with the game closed.

    python tools/ai/orchestrate.py status                 # what's recorded, trained, installed
    python tools/ai/orchestrate.py record [--manifest FILE] [--only TEXT] [--redo]
    python tools/ai/orchestrate.py train [--fold k/8 | all] [--cv] [--pilot-epochs N] [--threads auto] [--background]
    python tools/ai/orchestrate.py install                # train\\<model>\\<model>.gbnn -> weights\\
    python tools/ai/orchestrate.py play [--levels ID,ID] [--max-attempts N] [--max-seconds S]
    python tools/ai/orchestrate.py all                    # record, train, install, play

record takes every manifest entry marked use_for_training (and bar-level human
recordings nothing excludes; the manifest defaults to AI_ROOT\\manifest.json):
it finds the level id (gucci_ai_find_level for a name-only entry), calls
gucci_ai_record, and when the simulation didn't complete the macro, plays it in
the real game (gucci_ai_validate_real) and keeps the verdict in
AI_ROOT\\validation\\. Resumable (AI_ROOT\\train\\record_progress.json); it
brings a crashed game back; DATA_REPORT.md goes next to the manifest. Held-out
exam levels are recorded too (for checking), never trained on. play opens each
showcase level (gucci_open_level), reloads the weights (gucci_ai_reload) and
runs gucci_ai_play, then writes a report to AI_ROOT\\train\\reports\\.

train runs each trainer with the game closed (BelowNormal priority, --threads
auto = 3 while Geometry Dash runs, else 6). --fold all (the default here)
trains the models to ship on every level but the showcase ones; --cv first
trains the pilot on each of the 8 level folds (train\\pilot_folds\\fold_k\\,
each with its held-out report) and writes train\\pilot_folds\\cv_summary.json,
the honest numbers. --background launches the whole train step detached
(background.py: it outlives this session; `orchestrate.py status` polls it,
`background.py stop DIR` stops a trainer at its next step, and the same
command resumes).

The AI tools (contract v2 s8) are new on the C++ side. Before anything starts,
the game's tool list (MCP tools/list) is checked; a build without them stops
with a message naming what's missing.
"""
import argparse
import http.client
import json
import os
import subprocess
import sys
import time
import urllib.request

import background
import gbai

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(HERE)  # tools\, where assist.py lives
MODELS = ("pilot", "reader", "critic")
RECORD_TOOLS = ("gucci_ai_record", "gucci_ai_status")
OTHER_GAME_POLL = 300.0  # seconds between checks while a game this script didn't start runs (BUILD.md: 5 minutes)
OTHER_GAME_WAIT = 6 * 3600.0
SETTLE_SECONDS = 3.0  # after a level opens, before the AI plays it
PLAY_TOOLS = ("gucci_ai_reload", "gucci_ai_play", "gucci_ai_status", "gucci_open_level", "gucci_quit_level",
              "gucci_get_state")


class MissingTools(RuntimeError):
    """The game's GucciBot build doesn't have the tools a step needs."""


def require(game, names):
    """Raises MissingTools unless the game has every tool in `names`."""
    missing = [n for n in names if n not in game.tools]
    if missing:
        raise MissingTools(f"this GucciBot build has no {', '.join(missing)} (contract v2 s8; the C++ side, src/ai/, "
                           "isn't built into it yet). Build and install one that has them, then run this again.")


class AssistGame:
    """Our own game, through tools/assist.py: launched with safe mode forced, checked, muted; closed on exit.

    Optionally proves it left Nigel's files alone (assist's snapshot/check,
    when an allowlist has been learned): the AI's own folder (ai\\) is left
    out of the snapshot, since recording writes there on purpose.
    """

    def __init__(self, log=print, check_saves=True):
        self.log = log
        self.check_saves = check_saves
        self.tools = set()
        self.before = None
        self.saves_changed = None

    def __enter__(self):
        if TOOLS_DIR not in sys.path:
            sys.path.insert(0, TOOLS_DIR)
        import assist
        self.assist = assist
        if self.check_saves and os.path.exists(assist.ALLOWLIST):
            assist._TREE_SKIP = tuple(assist._TREE_SKIP) + ("ai/*",)
            self.before = assist.snapshot()
        elif self.check_saves:
            self.log(f"NOTE: no allowlist at {assist.ALLOWLIST}, so this session can't prove it left the saves "
                     "alone (assist.learn() makes one from a no-op session)")
        try:
            # Refuses while any Geometry Dash runs (it may be Nigel's), before starting anything;
            # inside the try so a game it did start and then gave up on still gets closed.
            assist.launch()
            self._setup()
        except BaseException:
            self._close()
            raise
        return self

    def __exit__(self, *exc):
        self._close()
        if self.before is not None:
            try:
                self.assist.check(self.before, self.assist.snapshot())
            except self.assist.SavesChanged as e:
                self.saves_changed = e.unexpected
                self.log(f"WARNING: the session changed files outside the allowlist: {e}")
        return False

    def _close(self):
        if self.assist.ours():
            self.assist.close()

    def alive(self):
        return self.assist.ours()

    def _setup(self):
        self.assist.ensure_safe()
        self.assist.test_set("test_mute", True)
        self.tools = self._tool_list()

    def recover(self, other_poll=OTHER_GAME_POLL, other_wait=OTHER_GAME_WAIT):
        """Brings our game back after a crash or a hang: restart() when it still answers (it follows the game
        into its new process; safe mode stays forced), else close() it if it still runs, then launch() a new
        one. A Geometry Dash this script didn't start (Nigel's) is never touched: launch waits for it to close,
        checking every `other_poll` seconds, up to `other_wait`."""
        a = self.assist
        if a.ours():
            if a._answers():
                try:
                    a.restart()
                    self._setup()
                    return
                except (RuntimeError, TimeoutError, OSError) as e:
                    self.log(f"  restart didn't work ({e}); closing and launching again")
            if a.ours():
                a.close()  # never kills; raises if the game won't close
        start = time.time()
        while a._running(os.path.basename(a.GD_EXE)):
            if time.time() - start > other_wait:
                raise RuntimeError(f"a Geometry Dash this script didn't start is still running after "
                                   f"{gbai.fmt_seconds(other_wait)}; stopping (run record again to carry on)")
            self.log(f"  a Geometry Dash this script didn't start is running; checking again in "
                     f"{gbai.fmt_seconds(other_poll)}")
            time.sleep(other_poll)
        a.launch()
        try:
            self._setup()
        except BaseException:
            self._close()
            raise

    def call(self, tool, **args):
        return self.assist.call(tool, **args)

    def _tool_list(self):
        """The game's tool names (MCP tools/list), asked only of the game launch() started."""
        if not self.assist.ours():
            raise RuntimeError("no game launch() started is running")
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}).encode()
        req = urllib.request.Request(self.assist.URL, body, {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=20) as r:
            res = json.loads(r.read())["result"]
        return {t["name"] for t in res.get("tools", [])}


def wait_job(game, job, timeout, label, log=print, poll=2.0, on_poll=None):
    """Polls gucci_ai_status until the job is done or failed; returns its last status.
    on_poll(status) sees every status read (the last progress text before a timeout)."""
    start, shown = time.time(), 0.0
    while True:
        st = game.call("gucci_ai_status", job=job)
        if on_poll:
            on_poll(st)
        state = st.get("state")
        if state in ("done", "failed"):
            return st
        if time.time() - start > timeout:
            raise TimeoutError(f"{label}: still {state} after {gbai.fmt_seconds(timeout)}")
        if time.time() - shown > 15:
            log(f"  {label}: {state}, progress {st.get('progress')}")
            shown = time.time()
        time.sleep(poll)


def stamp():
    return time.strftime("%Y%m%d_%H%M%S")


# ---- record ----------------------------------------------------------------------
#
# The overnight data run. For every manifest entry: the level id (gucci_ai_find_level when the
# manifest has only a name), gucci_ai_record (the mod tries the contract's offsets), and when the
# simulation didn't complete it, gucci_ai_validate_real at the offset the mod kept (4 minutes at most),
# whose answer goes to AI_ROOT\validation\<run_id>.json (gbai.validation_rule reads it). Progress is
# AI_ROOT\train\record_progress.json after every entry: a second run carries on where the first
# stopped, and a game that crashed or hung is restarted and the run goes on.

VALIDATE_TIMEOUT = 240.0  # seconds the real game may play one macro
FIND_TIMEOUT = 60.0
MAX_CRASHES_PER_ENTRY = 2  # an entry that took the game down this often is given up
MAX_RECOVERIES = 20  # restarts in one run before it stops for good
FINAL_STATES = ("done", "unresolved", "failed", "excluded")  # not retried unless --redo (failed: --retry-failed)


def manifest_path(root, given=None):
    path = given or os.path.join(root, "manifest.json")
    if not os.path.exists(path):
        raise SystemExit(f"no manifest at {path}: pass --manifest (the data agent's manifest.json)")
    return path


def _picked(e):
    """Entries the data run records: use_for_training, plus bar-level human recordings nothing excludes."""
    raw = e["raw"]
    if e["use_for_training"]:
        return True
    return e["is_bar_level"] and raw.get("origin", "human") == "human" and not raw.get("exclude_reason") \
        and not raw.get("duplicate_of")


def entries_to_record(path, only=None):
    """(entries to record, entries that can't be: no macro path, or neither a level id nor a name to find)."""
    picked = [e for e in gbai.read_manifest(path) if _picked(e)]
    for e in picked:
        e["find_name"] = str(e["raw"].get("find_name") or "").strip()
    if only:
        key = gbai.name_key(only)
        picked = [e for e in picked if key in gbai.name_key(e["level_name"] or e["find_name"]) or
                  key in gbai.name_key(os.path.basename(e["macro_path"]))]
    good = [e for e in picked if e["macro_path"] and (e["level_id"] or e["find_name"])]
    bad = [e for e in picked if not (e["macro_path"] and (e["level_id"] or e["find_name"]))]
    return good, bad


def entry_key(e):
    return os.path.normcase(os.path.abspath(str(e["macro_path"]).replace("/", os.sep)))


def _same_macro(a, b):
    return os.path.normcase(os.path.basename(str(a).replace("\\", "/"))) == \
        os.path.normcase(os.path.basename(str(b).replace("\\", "/")))


def recorded_already(entry, runs):
    return any(r.level_id == entry["level_id"] and _same_macro(r.macro_file, entry["macro_path"]) for r in runs)


def sanitize(s):
    """The mod's run_id piece (src/ai/record.cpp sanitize): over the UTF-8 bytes, ASCII letters, digits, - and .
    kept, every other byte _."""
    out = "".join(chr(c) if chr(c).isascii() and (chr(c).isalnum() or chr(c) in "-.") else "_"
                  for c in str(s).encode("utf-8"))
    return (out or "macro")[:80]


def expected_run_id(level_id, macro_path, fmt):
    stem = os.path.splitext(os.path.basename(str(macro_path).replace("\\", "/")))[0]
    return f"{level_id}__{sanitize(stem)}__{sanitize(fmt)}"


def pick_level(name, candidates):
    """(id, why) from gucci_ai_find_level's candidates: exact name matches only (case-insensitive);
    several -> a rated one first, then the most downloads; still tied -> (None, why)."""
    want = " ".join(str(name).lower().split())
    exact = [c for c in candidates or [] if " ".join(str(c.get("name", "")).lower().split()) == want
             and int(c.get("id") or 0) > 0]
    if not exact:
        names = ", ".join(repr(c.get("name")) for c in (candidates or [])[:5])
        return None, f"no level named exactly {name!r} on GD's servers" + (f" (found {names})" if names else "")
    if len(exact) == 1:
        return int(exact[0]["id"]), "the only exact match"

    def rank(c):
        downloads = c.get("downloads")
        return (1 if (c.get("stars") or 0) > 0 else 0, downloads if isinstance(downloads, (int, float)) else -1)
    exact.sort(key=rank, reverse=True)
    if rank(exact[0]) == rank(exact[1]):
        ids = ", ".join(str(c["id"]) for c in exact)
        why = "rated, same downloads" if rank(exact[0])[1] >= 0 else "no download counts to choose by"
        return None, f"ambiguous: {len(exact)} levels named {name!r} ({ids}; {why})"
    how = "rated" if rank(exact[0])[0] > rank(exact[1])[0] else "most downloads"
    return int(exact[0]["id"]), f"{len(exact)} exact matches, picked the {how} one"


def two_player_dropped(result):
    return "dropped (two-player level)" in str((result or {}).get("notes") or "")


class Progress:
    """AI_ROOT\\train\\record_progress.json: one item per manifest entry, the names found, written after each step."""

    def __init__(self, root):
        self.path = os.path.join(gbai.train_dir(root), "record_progress.json")
        data = gbai.read_json(self.path) if os.path.exists(self.path) else {}
        self.entries = data.get("entries", {})
        self.resolved = data.get("resolved", {})
        self.started = data.get("started") or gbai.now_iso()
        self.recoveries = 0

    def save(self):
        gbai.write_json_atomic(self.path, {"format": "gbai-record-progress", "started": self.started,
                                           "updated": gbai.now_iso(), "entries": self.entries,
                                           "resolved": self.resolved})


def _resolve(game, e, progress, log):
    """The entry's level id (the manifest's, or one gucci_ai_find_level found); (None, why) when none."""
    if e["level_id"]:
        return int(e["level_id"]), "manifest"
    name = e["find_name"]
    key = " ".join(name.lower().split())
    known = progress.resolved.get(key)
    if known is not None:
        return known.get("id"), known.get("why")
    if "gucci_ai_find_level" not in game.tools:
        return None, "this build has no gucci_ai_find_level"
    job = game.call("gucci_ai_find_level", name=name)["job"]
    st = wait_job(game, job, FIND_TIMEOUT, f"finding {name!r}", log)
    if st.get("state") != "done":
        raise RuntimeError(f"gucci_ai_find_level {name!r}: {st.get('error') or st.get('result')}")
    lid, why = pick_level(name, (st.get("result") or {}).get("candidates"))
    progress.resolved[key] = {"id": lid, "why": why, "when": gbai.now_iso()}
    return lid, why


def _macro_for(root, e, level_id, fmt, progress):
    """The macro path to record: the manifest's, or a copy in train\\macro_copies when another entry's run
    already has the run_id this one would get (the mod names runs by level, file stem and format)."""
    rid = expected_run_id(level_id, e["macro_path"], fmt)
    key = entry_key(e)
    taken = {v.get("run_id") for k, v in progress.entries.items() if k != key and v.get("run_id")}
    if rid not in taken:
        return e["macro_path"], rid
    stem, ext = os.path.splitext(os.path.basename(str(e["macro_path"]).replace("\\", "/")))
    folder = gbai.train_dir(root, "macro_copies")
    n = 2
    while True:
        copy = os.path.join(folder, f"{stem}__{n}{ext}")
        rid = expected_run_id(level_id, copy, fmt)
        if rid not in taken:
            break
        n += 1
    with open(e["macro_path"], "rb") as f:
        gbai.write_bytes_atomic(copy, f.read())
    return copy, rid


def _validate(game, root, run_id, header, macro_path, level_id, timeout, log):
    """gucci_ai_validate_real at the run's offset; writes the sidecar and returns it."""
    side = {"macro_path": macro_path, "level_id": level_id, "frame_offset": header.get("frame_offset"),
            "sim_completed": bool(header.get("completed")), "sim_died_x": header.get("died_x"),
            "sim_died_tick": header.get("died_tick"), "level_length_x": header.get("level_length_x")}
    job = game.call("gucci_ai_validate_real", macro_path=macro_path, level_id=level_id,
                    frame_offset=int(header.get("frame_offset") or 0))["job"]
    last = {}

    def seen(st):
        last.update(st)
    try:
        st = wait_job(game, job, timeout, f"real game {run_id}", log, on_poll=seen)
    except TimeoutError:
        if "gucci_ai_cancel" in game.tools:
            try:
                game.call("gucci_ai_cancel", job=job)
            except (RuntimeError, OSError):
                pass
        x = _progress_x(last.get("progress"))
        side.update(completed=None, died_x=None, percent=None, timed_out=True, real_x_at_timeout=x,
                    real_passed_sim_death=(x is not None and _is_num(header.get("died_x")) and
                                           x > float(header["died_x"]) + gbai.BLOCK))
        side["verdict"], side["why"] = gbai.validation_rule(header, side)
        gbai.write_validation(root, run_id, side)
        return side
    res = st.get("result") or {}
    if st.get("state") != "done":
        side.update(completed=None, died_x=None, percent=None, error=str(st.get("error") or res))
    else:
        side.update(completed=bool(res.get("completed")), died_x=res.get("died_x"), percent=res.get("percent"),
                    frame_offset=res.get("frame_offset", side["frame_offset"]), real_tps=res.get("tps"))
    side["verdict"], side["why"] = gbai.validation_rule(header, side)
    gbai.write_validation(root, run_id, side)
    return side


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _progress_x(text):
    import re
    m = re.search(r"x (-?[\d.]+)", str(text or ""))
    return float(m.group(1)) if m else None


def _record_one(game, root, e, progress, args, log):
    """Records one entry; returns its progress item."""
    item = {"macro_path": e["macro_path"], "level_name": e["level_name"] or e["find_name"],
            "format": e["raw"].get("format"), "set": e["raw"].get("set"), "manifest_level_id": e["level_id"] or None}
    lid, why = _resolve(game, e, progress, log)
    item["level_id"], item["level_id_how"] = lid, why
    if not lid:
        item.update(state="unresolved", why=why)
        return item
    fmt = e["raw"].get("format") or ""
    macro, rid = _macro_for(root, e, lid, fmt, progress)
    if macro != e["macro_path"]:
        item["recorded_from_copy"] = macro
    t0 = time.time()
    job = game.call("gucci_ai_record", macro_path=macro, level_id=lid)["job"]
    st = wait_job(game, job, args.record_timeout, f"recording {item['level_name']}", log)
    item["record_seconds"] = round(time.time() - t0, 1)
    if st.get("state") != "done":
        item.update(state="failed", why=f"gucci_ai_record: {st.get('error') or st.get('result')}")
        return item
    res = st.get("result") or {}
    rid = res.get("run_id") or rid
    item.update(run_id=rid, macro_format=res.get("macro_format") or fmt, frame_offset=res.get("frame_offset"),
                offsets_tried=res.get("offsets_tried"), sim_completed=bool(res.get("completed")),
                sim_died_x=res.get("died_x"), sim_died_tick=res.get("died_tick"), ticks=res.get("ticks"),
                tps=res.get("tps"), level_length_x=res.get("level_length_x"), notes=res.get("notes"))
    if two_player_dropped(res):
        gbai.write_validation(root, rid, {"exclude": "two-player level (player 2 isn't simulated)",
                                          "frame_offset": res.get("frame_offset")})
        item.update(state="excluded", why="two-player level", verdict="none")
        return item
    if res.get("completed"):
        stale = gbai.validation_path(root, rid)
        if os.path.exists(stale):  # from an earlier recording of the same run
            os.remove(stale)
        item.update(state="done", verdict="whole")
        return item
    if "gucci_ai_validate_real" not in game.tools:
        item.update(state="done", verdict="none", why="this build has no gucci_ai_validate_real")
        return item
    side = _validate(game, root, rid, res, macro, lid, args.validate_timeout, log)
    item.update(state="done", real=side, verdict=side.get("verdict"), why=side.get("why"))
    return item


def record(game, root, entries, redo=False, timeout=1800, log=print, args=None):
    """The data run (see above). Returns the progress items of this run's entries (also written as a report)."""
    if args is None:
        args = argparse.Namespace(record_timeout=timeout, validate_timeout=VALIDATE_TIMEOUT, retry_failed=False,
                                  report=None)
    args.record_timeout = getattr(args, "record_timeout", timeout) or timeout
    require(game, RECORD_TOOLS)
    save_layout(game, root, log)
    progress = Progress(root)
    results = []
    i = 0
    while i < len(entries):
        e = entries[i]
        key = entry_key(e)
        old = progress.entries.get(key) or {}
        label = f"[{i + 1}/{len(entries)}] {e['level_name'] or e['find_name'] or e['level_id']} " \
                f"({os.path.basename(e['macro_path'])})"
        final = old.get("state") in FINAL_STATES and not (old.get("state") == "failed" and args.retry_failed)
        if not redo and final:
            results.append(dict(old, state="skipped", was=old.get("state"), why="already " + old.get("state")))
            i += 1
            continue
        if old.get("crashes", 0) >= MAX_CRASHES_PER_ENTRY and not redo:
            item = dict(old, state="failed", why=f"the game went down {old['crashes']} times on it")
            progress.entries[key] = item
            progress.save()
            results.append(item)
            i += 1
            continue
        log(f"recording {label}")
        try:
            item = _record_one(game, root, e, progress, args, log)
        except (RuntimeError, TimeoutError, OSError, KeyError, ValueError, http.client.HTTPException) as ex:
            item = dict(old, macro_path=e["macro_path"], level_name=e["level_name"] or e["find_name"],
                        format=e["raw"].get("format"), state="error", why=str(ex))
            if not game.alive() or isinstance(ex, (TimeoutError, OSError, http.client.HTTPException)):
                item["crashes"] = old.get("crashes", 0) + 1
                progress.entries[key] = item
                progress.save()
                log(f"  ERROR: {ex}; restarting the game")
                if progress.recoveries >= MAX_RECOVERIES:
                    log(f"the game went down {progress.recoveries} times this run; stopping (run record again)")
                    results.append(item)
                    break
                progress.recoveries += 1
                try:
                    game.recover()
                except Exception as rex:  # noqa: BLE001 - whatever stopped the restart, the run stops here
                    log(f"couldn't bring the game back: {rex}; stopping (run record again to carry on)")
                    results.append(item)
                    break
                continue  # the same entry again
            log(f"  ERROR: {ex}")
        item["when"] = gbai.now_iso()
        progress.entries[key] = item
        progress.save()
        results.append(item)
        log(f"  {item['state']}: {item.get('verdict') or ''} {item.get('why') or ''}".rstrip())
        i += 1
    path = os.path.join(gbai.train_dir(root, "reports"), f"record_{stamp()}.json")
    gbai.write_json_atomic(path, {"format": "gbai-record-report", "created": gbai.now_iso(), "results": results})
    report = data_report(progress, entries).encode("utf-8")
    gbai.write_bytes_atomic(os.path.join(gbai.train_dir(root, "reports"), "DATA_REPORT.md"), report)
    rpath = getattr(args, "report", None) or os.path.join(gbai.train_dir(root, "reports"), "DATA_REPORT.md")
    gbai.write_bytes_atomic(rpath, report)
    counts = {}
    for r in results:
        counts[r["state"]] = counts.get(r["state"], 0) + 1
    log(f"record: {json.dumps(counts)}; report {path}; data report {rpath}")
    return results


def _outcome(it):
    """One word for a done item's sim-vs-real outcome."""
    if it.get("state") != "done":
        return it.get("state")
    if it.get("sim_completed"):
        return "sim completed"
    real = it.get("real") or {}
    if real.get("timed_out"):
        return "real timed out (passed the sim's death)" if real.get("real_passed_sim_death") else "real timed out"
    if real.get("error"):
        return "real check failed"
    if real.get("completed") is True:
        return "sim died, real completed"
    if real.get("completed") is False:
        return "sim died, real died there" if it.get("verdict") == "before_death" else "sim died, real died elsewhere"
    return "sim died, not checked"


def data_report(progress, entries):
    """DATA_REPORT.md: what the data run made of the manifest."""
    keys = [entry_key(e) for e in entries]
    items = [progress.entries[k] for k in keys if k in progress.entries]
    missing = len(keys) - len(items)
    lines = ["# AI training data: the data run", "",
             f"{gbai.now_iso()}. {len(entries)} manifest entries; {len(items)} tried, {missing} not yet. "
             "Made by `tools/ai/orchestrate.py record` (progress: AI_ROOT\\train\\record_progress.json; the real "
             "game's verdicts: AI_ROOT\\validation\\).", ""]

    def table(title, head, rows):
        lines.extend([f"## {title}", "", "| " + " | ".join(head) + " |", "|" + "---|" * len(head)])
        lines.extend("| " + " | ".join(str(c) for c in r) + " |" for r in rows)
        lines.append("")

    states = {}
    for it in items:
        states[it.get("state")] = states.get(it.get("state"), 0) + 1
    table("States", ["State", "Entries"], sorted(states.items(), key=lambda kv: -kv[1]))
    done = [it for it in items if it.get("state") == "done"]
    by_fmt = {}
    for it in done:
        f = it.get("macro_format") or it.get("format") or "?"
        row = by_fmt.setdefault(f, {"runs": 0, "whole": 0, "before_death": 0, "none": 0, "ticks": 0})
        row["runs"] += 1
        row[it.get("verdict") if it.get("verdict") in ("whole", "before_death") else "none"] += 1
        row["ticks"] += int(it.get("ticks") or 0)
    table("By format", ["Format", "Recorded", "Whole run usable", "Usable up to the death", "Not usable", "Ticks"],
          [(f, r["runs"], r["whole"], r["before_death"], r["none"], r["ticks"]) for f, r in sorted(by_fmt.items())])
    by_off = {}
    for it in done:
        o = it.get("frame_offset")
        row = by_off.setdefault(str(o), [0, 0])
        row[0] += 1
        row[1] += 1 if it.get("sim_completed") else 0
    table("By frame offset (the one the mod kept)", ["Offset", "Runs", "Completed in the simulation"],
          [(o, r[0], r[1]) for o, r in sorted(by_off.items())])
    outcomes = {}
    for it in done:
        k = _outcome(it)
        outcomes[k] = outcomes.get(k, 0) + 1
    table("Simulation vs the real game", ["Outcome", "Runs"], sorted(outcomes.items(), key=lambda kv: -kv[1]))
    dis = [it for it in done if _outcome(it) in ("sim died, real completed", "real timed out (passed the sim's death)",
                                                  "sim died, real died elsewhere")]
    table("Where the simulation and the real game disagree (simulator bugs to fix)",
          ["Level", "Id", "Macro", "Offset", "Sim died at x", "Real", "Real died at x"],
          [(it.get("level_name"), it.get("level_id"), os.path.basename(str(it.get("macro_path"))),
            it.get("frame_offset"), _fmt_x(it.get("sim_died_x")), _outcome(it),
            _fmt_x((it.get("real") or {}).get("died_x") or (it.get("real") or {}).get("real_x_at_timeout")))
           for it in sorted(dis, key=lambda it: str(it.get("level_name")))])
    bad = [it for it in items if it.get("state") not in ("done",)]
    table("Not recorded, and why", ["Level", "Macro", "State", "Why"],
          [(it.get("level_name"), os.path.basename(str(it.get("macro_path"))), it.get("state"),
            str(it.get("why") or "").replace("|", "/")[:200]) for it in sorted(bad, key=lambda it: str(it.get("state")))])
    real_fail = [it for it in done if (it.get("real") or {}).get("error")]
    if real_fail:
        table("Real-game checks that failed", ["Level", "Macro", "Error"],
              [(it.get("level_name"), os.path.basename(str(it.get("macro_path"))),
                str(it["real"]["error"]).replace("|", "/")[:200]) for it in real_fail])
    try:
        held = gbai.Showcase()
    except gbai.DataError as ex:
        held, lines[2:2] = None, [f"WARNING: {ex}", ""]
    if held:
        out = [it for it in items if it.get("level_id") and held.reason(it["level_id"])]
        table("Held-out exam macros (recorded for checking, never trained on)",
              ["Level", "Id", "Exam", "Macro", "Outcome"],
              [(it.get("level_name"), it.get("level_id"),
                str((held.entries.get(int(it["level_id"])) or {}).get("tier", "listed")),
                os.path.basename(str(it.get("macro_path"))), _outcome(it))
               for it in sorted(out, key=lambda it: (str(it.get("level_name")), str(it.get("macro_path"))))])
        warn = [(it, held.warning(it.get("level_id") or 0, it.get("level_name"), it.get("macro_path")))
                for it in items if it.get("level_id")]
        warn = [(it, w) for it, w in warn if w]
        if warn:
            table("Names that look like an exam level's (kept for training: only the id holds a level out)",
                  ["Level", "Id", "Macro"],
                  [(it.get("level_name"), it.get("level_id"), os.path.basename(str(it.get("macro_path"))))
                   for it, _ in warn])
    if progress.resolved:
        table("Level ids looked up in the game", ["Name", "Id", "How"],
              [(k, v.get("id"), str(v.get("why")).replace("|", "/")) for k, v in sorted(progress.resolved.items())])
    return "\n".join(lines) + "\n"


def _fmt_x(v):
    return f"{float(v):.0f}" if _is_num(v) else ""


def save_layout(game, root, log=print):
    """gucci_ai_layout's answer, kept as AI_ROOT\\train\\layout.json (train uses its hashes)."""
    if "gucci_ai_layout" not in game.tools:
        log("NOTE: no gucci_ai_layout in this build; training will use whatever layout the data has")
        return None
    layout = game.call("gucci_ai_layout")
    gbai.write_json_atomic(os.path.join(gbai.train_dir(root), "layout.json"), layout)
    return layout


# ---- train -----------------------------------------------------------------------

def layout_hashes(layout):
    """(layout hash, tape hash) from gucci_ai_layout's answer. The contract names "the layout hash"
    without fixing its field name (CONTRACT_ISSUES.md), so a few likely ones are read; None where
    there's none. Without a tape hash the reader gets the layout hash too."""
    def pick(*keys):
        for k in keys:
            v = layout.get(k)
            if isinstance(v, str) and gbai.HASH_RE.match(v):
                return v
        return None
    return pick("layout_hash", "obs_layout_hash", "hash"), pick("tape_layout_hash", "tape_hash")


def trainer_steps(root, args, fold=None, out=None, models=None):
    """[(model, argv)] for the trainers that have data to learn from. fold overrides args.fold;
    out maps a model to its output folder; models limits which ones."""
    layout = gbai.read_json(os.path.join(root, "train", "layout.json")) \
        if os.path.exists(os.path.join(root, "train", "layout.json")) else {}
    common = ["--root", root, "--threads", str(args.threads), "--fold", fold or args.fold]
    # The same manifest record used (AI_ROOT\manifest.json unless --manifest), so its
    # is_bar_level entries stay out of training even when --manifest wasn't typed.
    manifest = args.manifest or os.path.join(root, "manifest.json")
    if args.manifest or os.path.exists(manifest):
        common += ["--manifest", manifest]
    if args.fresh:
        common += ["--fresh"]
    obs_hash, tape_hash = layout_hashes(layout)
    steps = []

    def add(model, argv, epochs):
        if models is not None and model not in models:
            return
        if epochs:
            argv = argv + ["--epochs", str(epochs)]
        if out and model in out:
            argv = argv + ["--out", out[model]]
        steps.append((model, argv))
    add("pilot", common + (["--layout-hash", obs_hash] if obs_hash else []), args.pilot_epochs)
    tapes, _ = gbai.list_tapes(root)
    if tapes:
        tape = tape_hash or obs_hash
        add("reader", common + (["--layout-hash", tape] if tape else []), args.reader_epochs)
    attempts, _ = gbai.list_practice(root)
    if attempts:
        add("critic", common + (["--layout-hash", obs_hash] if obs_hash else []), args.critic_epochs)
    return steps


def _run_trainer(model, argv, log, in_process):
    log(f"training the {model}: {' '.join(argv)}")
    if in_process:
        import importlib
        mod = importlib.import_module(f"train_{model}")
        try:
            return mod.main(argv, log=log)
        except (gbai.DataError, SystemExit) as e:
            log(f"  the {model} didn't train: {e}")
            return None
    code = subprocess.run([background.python_exe(), os.path.join(HERE, f"train_{model}.py")] + argv).returncode
    if code:
        log(f"  the {model} trainer exited with {code}")
    return None


def cross_validate(root, args, log=print, in_process=False):
    """The pilot on each of the 8 level folds (train\\pilot_folds\\fold_k), and cv_summary.json: per fold,
    its validation and showcase numbers. Every level is held out exactly once."""
    base = gbai.train_dir(root, "pilot_folds")
    k_max = gbai.DEFAULT_FOLDS
    folds = []
    for k in range(k_max):
        out = os.path.join(base, f"fold_{k}")
        steps = trainer_steps(root, args, fold=f"{k}/{k_max}", out={"pilot": out}, models={"pilot"})
        for model, argv in steps:
            _run_trainer(model, argv, log, in_process)
        rep = os.path.join(out, "eval_report.json")
        split = os.path.join(out, "split.json")
        entry = {"fold": f"{k}/{k_max}", "out": out}
        if os.path.exists(split):
            sp = gbai.read_json(split)
            entry["fold_used"] = sp.get("fold")
            entry["val_levels"] = sp.get("val_levels")
        if os.path.exists(rep):
            groups = gbai.read_json(rep).get("groups", {})
            entry["validation"] = groups.get("validation", {}).get("model")
            entry["showcase"] = groups.get("showcase", {}).get("model")
        folds.append(entry)
        if entry.get("fold_used") and int(entry["fold_used"].split("/")[1]) <= k + 1:
            break  # fewer levels than folds: leave-one-out ran out of folds
    summary = {"format": "gbai-cv", "created": gbai.now_iso(), "folds": folds,
               "note": "each fold's model validated on levels it never trained on; showcase levels are held out "
                       "of every fold"}
    gbai.write_json_atomic(os.path.join(base, "cv_summary.json"), summary)
    log(f"cross-validation: {len(folds)} folds, summary {os.path.join(base, 'cv_summary.json')}")
    return summary


def train(root, args, log=print, in_process=False):
    """Runs each trainer (in its own process unless in_process) and returns {model: progress}
    (plus "cv" with --cv)."""
    out = {}
    if getattr(args, "cv", False):
        out["cv"] = cross_validate(root, args, log, in_process)
    for model, argv in trainer_steps(root, args):
        _run_trainer(model, argv, log, in_process)
        out[model] = gbai.read_progress(os.path.join(root, "train", model, "progress.json"))
    return out


def launch_train(root, argv, log=print, dry_run=False):
    """`orchestrate.py train ...` (without --background) started detached: it outlives this session."""
    rest = [a for a in argv if a != "--background"]
    log_path = os.path.join(gbai.train_dir(root, "logs"), f"train_{stamp()}.log")
    info = background.launch([os.path.abspath(__file__)] + rest, log_path, dry_run=dry_run)
    info["log"] = log_path
    log(f"training launched in the background (pid {info['pid']}); log {log_path}. "
        "Poll it with `orchestrate.py status`; stop a trainer with `background.py stop <its train folder>`.")
    return info


# ---- install ---------------------------------------------------------------------

def install(root, log=print):
    """Copies each trained <model>.gbnn into AI_ROOT\\weights after checking it reads back and
    reproduces its golden vectors. Returns [{model, layout_hash, sha256}]."""
    import export
    wdir = os.path.join(root, "weights")
    os.makedirs(wdir, exist_ok=True)
    layout_path = os.path.join(root, "train", "layout.json")
    layout = gbai.read_json(layout_path) if os.path.exists(layout_path) else {}
    done = []
    for m in MODELS:
        src = os.path.join(root, "train", m, f"{m}.gbnn")
        if not os.path.exists(src):
            log(f"no trained {m} ({src}); nothing to install")
            continue
        net = gbai.read_gbnn(src)
        if os.path.exists(export.golden_path(src)):
            ok, worst, n = export.check_golden(src)
            if not ok:
                raise RuntimeError(f"{src} doesn't reproduce its own golden vectors (worst {worst:.3g}); not installed")
        want = layout_hashes(layout)[1 if m == "reader" else 0]
        if want and want != net.layout_hash:
            log(f"WARNING: {m}.gbnn is for layout {net.layout_hash} but the game's is {want}: the mod will refuse it")
        dst = os.path.join(wdir, f"{m}.gbnn")
        with open(src, "rb") as f:
            gbai.write_bytes_atomic(dst, f.read())
        done.append({"model": m, "layout_hash": net.layout_hash, "sha256": gbai.sha256_file(dst), "path": dst})
        log(f"installed {dst} (layout {net.layout_hash})")
    return done


# ---- play ------------------------------------------------------------------------

def showcase_levels(manifest=None, only=None):
    """[(level_id, name)]: the showcase ids gbai knows, plus the manifest's is_bar_level entries."""
    levels = dict(gbai.SHOWCASE_IDS)
    if manifest and os.path.exists(manifest):
        for e in gbai.read_manifest(manifest):
            if e["is_bar_level"] and e["level_id"]:
                levels[e["level_id"]] = e["level_name"] or levels.get(e["level_id"], "")
    if only:
        want = {int(v) for v in str(only).split(",") if v.strip()}
        levels = {k: v for k, v in levels.items() if k in want}
        for k in want - set(levels):
            levels[k] = ""
    return sorted(levels.items(), key=lambda kv: kv[1].lower())


def _state(game):
    return game.call("gucci_get_state")


def _wait(game, cond, timeout, what):
    start = time.time()
    while True:
        st = _state(game)
        if cond(st):
            return st
        if time.time() - start > timeout:
            raise TimeoutError(f"{what} didn't happen within {timeout} s")
        time.sleep(1.0)


def leave_level(game, timeout=30):
    if _state(game).get("in_level"):
        game.call("gucci_quit_level")
        _wait(game, lambda s: not s.get("in_level"), timeout, "leaving the level")


def open_level(game, level_id, timeout=180, settle=None):
    """Opens a level: RobTop's (1..22) by `main`, an online one by `id` (downloading it first if needed)."""
    leave_level(game)
    args = {"main": level_id} if 1 <= level_id <= 22 else {"id": level_id}
    start = time.time()
    while game.call("gucci_open_level", **args).get("downloading"):
        if time.time() - start > timeout:
            raise TimeoutError(f"level {level_id} didn't download within {timeout} s")
        time.sleep(3.0)
    _wait(game, lambda s: s.get("in_level") and int((s.get("level") or {}).get("id") or 0) == level_id,
          timeout, f"opening level {level_id}")
    time.sleep(SETTLE_SECONDS if settle is None else settle)


def play(game, root, levels, args, log=print):
    """gucci_ai_play on each level; returns the report dict (also written as JSON + markdown)."""
    require(game, PLAY_TOOLS)
    reload = game.call("gucci_ai_reload")
    log(f"weights reloaded: {json.dumps(reload)}")
    results = []
    for lid, name in levels:
        item = {"level_id": lid, "level_name": name}
        log(f"playing {name or lid}")
        t0 = time.time()
        try:
            open_level(game, lid)
            call = {"max_attempts": args.max_attempts, "max_seconds": args.max_seconds}  # contract v2 s8
            if args.save_macros:
                call["save_macro"] = f"AI {name or lid}"
            if args.log_practice:
                call["log_practice"] = True
            job = game.call("gucci_ai_play", **call)["job"]
            st = wait_job(game, job, args.max_seconds + 300, name or str(lid), log)
            results.append(dict(item, state=st.get("state"), result=st.get("result"),
                                seconds=round(time.time() - t0, 1)))
        except (RuntimeError, TimeoutError, OSError, KeyError, ValueError) as ex:
            results.append(dict(item, state="error", why=str(ex)))
            log(f"  ERROR: {ex}")
            if not game.alive():
                log("the game closed; stopping here")
                break
        finally:
            try:
                if game.alive():
                    leave_level(game)
            except (RuntimeError, TimeoutError, OSError):
                pass
    report = {"format": "gbai-play-report", "created": gbai.now_iso(), "reload": reload,
              "settings": {"max_attempts": args.max_attempts, "max_seconds": args.max_seconds,
                           "log_practice": bool(args.log_practice)}, "results": results}
    base = os.path.join(gbai.train_dir(root, "reports"), f"play_{stamp()}")
    gbai.write_json_atomic(base + ".json", report)
    gbai.write_bytes_atomic(base + ".md", play_markdown(report).encode("utf-8"))
    log(f"report: {base}.json and .md")
    return report


def play_markdown(report):
    lines = ["# AI play report", "", f"{report['created']}. Showcase levels: the models never trained on them.", "",
             "| Level | Result | Best | Attempts | Deaths | Most common cause |", "|---|---|---|---|---|---|"]
    for r in report["results"]:
        res = r.get("result") or {}
        if r.get("state") != "done":
            lines.append(f"| {r['level_name'] or r['level_id']} | {r.get('state')}: {r.get('why') or res} | | | | |")
            continue
        deaths = res.get("deaths") or []
        causes = {}
        for d in deaths:
            causes[d.get("cause") or "?"] = causes.get(d.get("cause") or "?", 0) + 1
        top = max(causes.items(), key=lambda kv: kv[1])[0] if causes else ""
        best = res.get("best_percent")
        lines.append(f"| {r['level_name'] or r['level_id']} | {'completed' if res.get('completed') else 'not yet'} | "
                     f"{'' if best is None else f'{best:.1f}%'} | {res.get('attempts', '')} | {len(deaths)} | {top} |")
    return "\n".join(lines) + "\n"


# ---- status ----------------------------------------------------------------------

def status(root, manifest=None, log=print):
    log(f"AI_ROOT: {root}")
    runs, problems = gbai.list_runs(root, source=None)
    tapes, _ = gbai.list_tapes(root)
    attempts, _ = gbai.list_practice(root)
    log(f"data: {len(runs)} runs ({len({gbai.level_key(r) for r in runs})} levels), {len(tapes)} tapes, "
        f"{len(attempts)} practice attempts, {len(problems)} files that don't follow the contract")
    mpath = manifest or os.path.join(root, "manifest.json")
    if os.path.exists(mpath):
        good, bad = entries_to_record(mpath)
        missing = [e for e in good if not recorded_already(e, runs)]
        log(f"manifest: {len(good)} entries to record, {len(good) - len(missing)} recorded, {len(missing)} not yet, "
            f"{len(bad)} without a macro path or level id")
    for m in MODELS:
        p = gbai.read_progress(os.path.join(root, "train", m, "progress.json"))
        if p:
            log(f"{m}: {p.get('state')}, epoch {p.get('epoch')}/{p.get('epochs')}, eta {p.get('eta')}, "
                f"pid {p.get('pid')}, {p.get('threads')} threads, updated {p.get('updated')}, "
                f"{p.get('message') or ''}")
        w = os.path.join(root, "weights", f"{m}.gbnn")
        if os.path.exists(w):
            try:
                log(f"  installed: layout {gbai.read_gbnn(w).layout_hash}")
            except gbai.DataError as e:
                log(f"  installed file is broken: {e}")


# ---- command line ----------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(description="Record, train, install and play the GucciBot AI through the game.")
    ap.add_argument("command", choices=["status", "record", "train", "install", "play", "all"])
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--manifest", help="the training-data manifest (default: AI_ROOT\\manifest.json)")
    ap.add_argument("--only", help="record: only entries whose level or macro name holds this")
    ap.add_argument("--redo", action="store_true", help="record: record entries again even if already recorded")
    ap.add_argument("--record-timeout", type=float, default=1800, help="seconds one recording may take")
    ap.add_argument("--validate-timeout", type=float, default=VALIDATE_TIMEOUT,
                    help="record: seconds the real game may play one macro (gucci_ai_validate_real)")
    ap.add_argument("--retry-failed", action="store_true", help="record: try entries that failed before again")
    ap.add_argument("--report", help="record: where DATA_REPORT.md goes (default: next to the manifest)")
    ap.add_argument("--fold", default="all",
                    help="train: k/8 holds out fold k of the levels; all (default) trains the models to ship")
    ap.add_argument("--cv", action="store_true",
                    help="train: first the pilot on each of the 8 level folds, for honest held-out numbers")
    ap.add_argument("--background", action="store_true",
                    help="train: launch it detached (it outlives this session) and return")
    ap.add_argument("--pilot-epochs", type=int)
    ap.add_argument("--reader-epochs", type=int)
    ap.add_argument("--critic-epochs", type=int)
    ap.add_argument("--threads", default="auto", help="train: CPU threads, or auto (3 while Geometry Dash runs, "
                                                      "else 6)")
    ap.add_argument("--fresh", action="store_true", help="train: start the trainers over")
    ap.add_argument("--levels", help="play: level ids, comma separated (default: every showcase level)")
    ap.add_argument("--max-attempts", type=int, default=200)
    ap.add_argument("--max-seconds", type=int, default=900)
    ap.add_argument("--log-practice", action="store_true", help="play: log the attempts (contract s6)")
    ap.add_argument("--save-macros", action="store_true", help="play: save each finished run as a macro")
    ap.add_argument("--no-save-check", action="store_true",
                    help="skip proving the session left Nigel's files alone (assist snapshot/check)")
    return ap.parse_args(argv)


def main(argv=None, log=print, game_factory=None):
    """game_factory: () -> a context manager giving the game (tests pass a fake one)."""
    args = parse_args(argv)
    root = gbai.ai_root(args.root)
    make_game = game_factory or (lambda: AssistGame(log, check_saves=not args.no_save_check))
    if args.command == "status":
        status(root, args.manifest, log)
        return None
    if args.command == "install":
        return install(root, log)
    if args.command == "train":
        if args.background:
            return launch_train(root, list(argv if argv is not None else sys.argv[1:]), log)
        return train(root, args, log, in_process=game_factory is not None)
    out = {}
    if args.command in ("record", "all"):
        mpath = manifest_path(root, args.manifest)
        entries, bad = entries_to_record(mpath, args.only)
        for e in bad:
            log(f"can't record {e['macro_path'] or e['raw']}: it needs a macro path and a level id or a name to find")
        if not args.report:
            args.report = os.path.join(os.path.dirname(os.path.abspath(mpath)), "DATA_REPORT.md")
        with make_game() as game:
            out["record"] = record(game, root, entries, args.redo, args.record_timeout, log, args)
    if args.command == "all":
        out["train"] = train(root, args, log, in_process=game_factory is not None)
        out["install"] = install(root, log)
    if args.command in ("play", "all"):
        levels = showcase_levels(args.manifest or os.path.join(root, "manifest.json"), args.levels)
        with make_game() as game:
            out["play"] = play(game, root, levels, args, log)
    return out


if __name__ == "__main__":
    try:
        main()
    except (MissingTools, gbai.DataError) as e:
        sys.exit(f"orchestrate: {e}")

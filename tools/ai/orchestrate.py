"""The whole AI loop, driven through the game: record the human macros, train, install the
weights, let the AI play the showcase levels, and report.

Every step that needs the game goes through tools/assist.py: it launches its
own Geometry Dash with safe mode forced (refusing while any Geometry Dash is
running, since that one may be Nigel's), checks safe mode is on, mutes it
(gucci_test_set test_mute, never saved) and closes it in a finally. It never
talks to a game it didn't start. Training runs with the game closed.

    python tools/ai/orchestrate.py status                 # what's recorded, trained, installed
    python tools/ai/orchestrate.py record [--manifest FILE] [--only TEXT] [--redo]
    python tools/ai/orchestrate.py train [--fold k/K] [--pilot-epochs N] [--threads 6]
    python tools/ai/orchestrate.py install                # train\\<model>\\<model>.gbnn -> weights\\
    python tools/ai/orchestrate.py play [--levels ID,ID] [--max-attempts N] [--max-seconds S]
    python tools/ai/orchestrate.py all                    # record, train, install, play

record takes every manifest entry marked use_for_training or is_bar_level
(the manifest defaults to AI_ROOT\\manifest.json) and calls gucci_ai_record
for each one not recorded yet, polling gucci_ai_status. play opens each
showcase level (gucci_open_level), reloads the weights (gucci_ai_reload) and
runs gucci_ai_play, then writes a report to AI_ROOT\\train\\reports\\.

The AI tools (contract s6) are new on the C++ side. Before anything starts,
the game's tool list (MCP tools/list) is checked; a build without them stops
with a message naming what's missing.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

import gbai

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(HERE)  # tools\, where assist.py lives
MODELS = ("pilot", "reader", "critic")
RECORD_TOOLS = ("gucci_ai_record", "gucci_ai_status")
SETTLE_SECONDS = 3.0  # after a level opens, before the AI plays it
PLAY_TOOLS = ("gucci_ai_reload", "gucci_ai_play", "gucci_ai_status", "gucci_open_level", "gucci_quit_level",
              "gucci_get_state")


class MissingTools(RuntimeError):
    """The game's GucciBot build doesn't have the tools a step needs."""


def require(game, names):
    """Raises MissingTools unless the game has every tool in `names`."""
    missing = [n for n in names if n not in game.tools]
    if missing:
        raise MissingTools(f"this GucciBot build has no {', '.join(missing)} (contract s6; the C++ side, src/ai/, "
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
            assist.ensure_safe()
            assist.test_set("test_mute", True)
            self.tools = self._tool_list()
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


def wait_job(game, job, timeout, label, log=print, poll=2.0):
    """Polls gucci_ai_status until the job is done or failed; returns its last status."""
    start, shown = time.time(), 0.0
    while True:
        st = game.call("gucci_ai_status", job=job)
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

def manifest_path(root, given=None):
    path = given or os.path.join(root, "manifest.json")
    if not os.path.exists(path):
        raise SystemExit(f"no manifest at {path}: pass --manifest (the data agent's manifest.json)")
    return path


def entries_to_record(path, only=None):
    """(entries to record, entries that can't be: no macro path or no level id)."""
    picked = [e for e in gbai.read_manifest(path) if e["use_for_training"] or e["is_bar_level"]]
    if only:
        key = gbai.name_key(only)
        picked = [e for e in picked if key in gbai.name_key(e["level_name"]) or
                  key in gbai.name_key(os.path.basename(e["macro_path"]))]
    good = [e for e in picked if e["macro_path"] and e["level_id"]]
    bad = [e for e in picked if not (e["macro_path"] and e["level_id"])]
    return good, bad


def _same_macro(a, b):
    return os.path.normcase(os.path.basename(str(a).replace("\\", "/"))) == \
        os.path.normcase(os.path.basename(str(b).replace("\\", "/")))


def recorded_already(entry, runs):
    return any(r.level_id == entry["level_id"] and _same_macro(r.macro_file, entry["macro_path"]) for r in runs)


def save_layout(game, root, log=print):
    """gucci_ai_layout's answer, kept as AI_ROOT\\train\\layout.json (train uses its hashes)."""
    if "gucci_ai_layout" not in game.tools:
        log("NOTE: no gucci_ai_layout in this build; training will use whatever layout the data has")
        return None
    layout = game.call("gucci_ai_layout")
    gbai.write_json_atomic(os.path.join(gbai.train_dir(root), "layout.json"), layout)
    return layout


def record(game, root, entries, redo=False, timeout=1800, log=print):
    """gucci_ai_record for each entry not recorded yet. Returns a list of results (also written as a report)."""
    require(game, RECORD_TOOLS)
    save_layout(game, root, log)
    runs, _ = gbai.list_runs(root, source=None)
    results = []
    for i, e in enumerate(entries):
        label = f"[{i + 1}/{len(entries)}] {e['level_name'] or e['level_id']} ({os.path.basename(e['macro_path'])})"
        item = {"level_id": e["level_id"], "level_name": e["level_name"], "macro_path": e["macro_path"]}
        if not redo and recorded_already(e, runs):
            results.append(dict(item, state="skipped", why="already recorded"))
            continue
        log(f"recording {label}")
        t0 = time.time()
        try:
            job = game.call("gucci_ai_record", macro_path=e["macro_path"], level_id=e["level_id"])["job"]
            st = wait_job(game, job, timeout, label, log)
            results.append(dict(item, state=st.get("state"), result=st.get("result"),
                                seconds=round(time.time() - t0, 1)))
            if st.get("state") != "done":
                log(f"  FAILED: {st.get('result')}")
        except (RuntimeError, TimeoutError, OSError, KeyError, ValueError) as ex:
            results.append(dict(item, state="error", why=str(ex)))
            log(f"  ERROR: {ex}")
            if not game.alive():
                log("the game closed; stopping here (run record again to carry on)")
                break
    path = os.path.join(gbai.train_dir(root, "reports"), f"record_{stamp()}.json")
    gbai.write_json_atomic(path, {"format": "gbai-record-report", "created": gbai.now_iso(), "results": results})
    done = sum(1 for r in results if r["state"] == "done")
    log(f"recorded {done}, skipped {sum(1 for r in results if r['state'] == 'skipped')}, "
        f"failed {len(results) - done - sum(1 for r in results if r['state'] == 'skipped')}; report {path}")
    return results


# ---- train -----------------------------------------------------------------------

def layout_hashes(layout):
    """(obs hash, tape hash) from gucci_ai_layout's answer. The contract doesn't fix its field names
    (CONTRACT_ISSUES.md), so a few likely ones are read; None where there's none."""
    def pick(*keys):
        for k in keys:
            v = layout.get(k)
            if isinstance(v, str) and gbai.HASH_RE.match(v):
                return v
        return None
    return pick("obs_layout_hash", "layout_hash", "hash"), pick("tape_layout_hash", "tape_hash")


def trainer_steps(root, args):
    """[(model, argv)] for the trainers that have data to learn from."""
    layout = gbai.read_json(os.path.join(root, "train", "layout.json")) \
        if os.path.exists(os.path.join(root, "train", "layout.json")) else {}
    common = ["--root", root, "--threads", str(args.threads)]
    if args.fold:
        common += ["--fold", args.fold]
    # The same manifest record used (AI_ROOT\manifest.json unless --manifest), so its
    # is_bar_level entries stay out of training even when --manifest wasn't typed.
    manifest = args.manifest or os.path.join(root, "manifest.json")
    if args.manifest or os.path.exists(manifest):
        common += ["--manifest", manifest]
    if args.fresh:
        common += ["--fresh"]
    obs_hash, tape_hash = layout_hashes(layout)
    steps = []
    pilot = common + (["--layout-hash", obs_hash] if obs_hash else [])
    if args.pilot_epochs:
        pilot += ["--epochs", str(args.pilot_epochs)]
    steps.append(("pilot", pilot))
    tapes, _ = gbai.list_tapes(root)
    if tapes:
        reader = common + (["--layout-hash", tape_hash] if tape_hash else [])
        if args.reader_epochs:
            reader += ["--epochs", str(args.reader_epochs)]
        steps.append(("reader", reader))
    attempts, _ = gbai.list_practice(root)
    if attempts:
        critic = common + (["--layout-hash", obs_hash] if obs_hash else [])
        if args.critic_epochs:
            critic += ["--epochs", str(args.critic_epochs)]
        steps.append(("critic", critic))
    return steps


def train(root, args, log=print, in_process=False):
    """Runs each trainer (in its own process unless in_process) and returns {model: progress}."""
    out = {}
    for model, argv in trainer_steps(root, args):
        log(f"training the {model}: {' '.join(argv)}")
        script = os.path.join(HERE, f"train_{model}.py")
        if in_process:
            import importlib
            mod = importlib.import_module(f"train_{model}")
            try:
                mod.main(argv, log=log)
            except (gbai.DataError, SystemExit) as e:
                log(f"  the {model} didn't train: {e}")
        else:
            code = subprocess.run([sys.executable, script] + argv).returncode
            if code:
                log(f"  the {model} trainer exited with {code}")
        out[model] = gbai.read_progress(os.path.join(root, "train", model, "progress.json"))
    return out


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
            call = {"max_attempts": args.max_attempts, "max_seconds": args.max_seconds, "practice": True,
                    "route_weight": args.route_weight}
            if args.save_macros:
                call["save_macro"] = f"AI {name or lid}"
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
                           "route_weight": args.route_weight}, "results": results}
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
    ap.add_argument("--fold", help="train: k/K, hold out fold k of the levels")
    ap.add_argument("--pilot-epochs", type=int)
    ap.add_argument("--reader-epochs", type=int)
    ap.add_argument("--critic-epochs", type=int)
    ap.add_argument("--threads", type=int, default=6, help="train: CPU threads (6 = one per core)")
    ap.add_argument("--fresh", action="store_true", help="train: start the trainers over")
    ap.add_argument("--levels", help="play: level ids, comma separated (default: every showcase level)")
    ap.add_argument("--max-attempts", type=int, default=200)
    ap.add_argument("--max-seconds", type=int, default=900)
    ap.add_argument("--route-weight", type=float, default=1.0, help="play: 1 = Accurate, 0 = Cool")
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
        return train(root, args, log, in_process=game_factory is not None)
    out = {}
    if args.command in ("record", "all"):
        entries, bad = entries_to_record(manifest_path(root, args.manifest), args.only)
        for e in bad:
            log(f"can't record {e['raw']}: it needs a macro path and a level id")
        with make_game() as game:
            out["record"] = record(game, root, entries, args.redo, args.record_timeout, log)
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

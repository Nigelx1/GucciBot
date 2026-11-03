"""GucciBot AI: the trainer's side of the files the mod writes (contract v1).

The contract (CONTRACT.md, "AI pilot contract v1") fixes what the mod records
under AI_ROOT and what the trainer gives back:

    AI_ROOT\\data\\<run_id>.json + .obs.f16 .act.u8 .meta.f32       recorded runs (s2)
    AI_ROOT\\tapes\\<level_id>.json + .tape.f16                      the level as columns (s3)
    AI_ROOT\\practice\\<level_id>\\<attempt>.json + ... .alive.i32   the AI's own attempts (s4)
    AI_ROOT\\weights\\<model>.gbnn                                   trained networks (s5)

Python only ever reads data\\, tapes\\ and practice\\ (the mod owns them). It
writes weights\\ (orchestrate.py installs them there) and its own train\\
folder (checkpoints, progress files, reports), which the mod never reads.

This module reads and checks all of it (numpy memmaps for the arrays), writes
and reads GBNN files byte for byte as the contract lays them out, and holds
what the trainers share: the showcase levels (never trained on), splits by
level, and progress files another process can poll. numpy only, no torch,
so the shell commands below start fast.

    import gbai
    root = gbai.ai_root()                    # or $GUCCIBOT_AI_ROOT
    runs, problems = gbai.list_runs(root)    # human runs that passed the checks
    runs, layout_hash = gbai.pick_layout(runs)
    run = runs[0]
    run.obs[:run.usable_ticks()]             # float16 [T][N], read from disk as used

From a shell:

    python tools/ai/gbai.py list [--root DIR]     # what's recorded, per level
    python tools/ai/gbai.py check [--root DIR]    # every file against the contract, values too
    python tools/ai/gbai.py gbnn FILE             # a weights file's header and layers
"""
import argparse
import ctypes
import datetime
import glob
import hashlib
import json
import os
import re
import struct
import sys
import time

import numpy as np

MOD_ID = "nigelx1.guccibot"

# ---- the contract's constants --------------------------------------------------

BLOCK = 30.0  # GD units per block
META_FIELDS = ["x", "y", "yvel", "mode", "gravity_flipped", "mini", "speed", "dead"]
META_X, META_Y, META_YVEL, META_MODE, META_GRAVITY, META_MINI, META_SPEED, META_DEAD = range(8)
MODES = ["cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"]
SPEEDS = ["0.5x", "1x", "2x", "3x", "4x"]
# H = round(1.0 * tps): the practice log's "alive" cap (s4), and how much of a
# run that died the trainers drop before the death (s2).
SURVIVE_SECONDS = 1.0
DROP_SECONDS = 1.0

HASH_RE = re.compile(r"^[0-9a-fA-F]{16}$")


class DataError(Exception):
    """A file that doesn't follow the contract, or a dataset the trainers can't use as it is."""


def default_ai_root():
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "GeometryDash", "geode", "mods", MOD_ID, "ai")


def ai_root(path=None):
    """AI_ROOT: `path`, else $GUCCIBOT_AI_ROOT, else the mod's save folder's ai\\."""
    return os.path.abspath(path or os.environ.get("GUCCIBOT_AI_ROOT") or default_ai_root())


def train_dir(root, *parts):
    """A folder under AI_ROOT\\train\\ (Python's own; the mod never reads it). Made if missing."""
    path = os.path.join(root, "train", *parts)
    os.makedirs(path, exist_ok=True)
    return path


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def fmt_seconds(s):
    """12.3 -> '12 s', 754 -> '12 min 34 s', 4000 -> '1 h 6 min'."""
    if s is None or not np.isfinite(s):
        return "?"
    s = int(round(max(0.0, s)))
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60} s"
    return f"{s // 3600} h {(s % 3600) // 60} min"


# ---- small file helpers --------------------------------------------------------

def _replace(tmp, path, tries=40):
    """os.replace, retried: on Windows a reader that has the old file open blocks it briefly."""
    for i in range(tries):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(0.05)


def write_bytes_atomic(path, data):
    """Writes the whole file under a temporary name, then renames it into place,
    so a process polling `path` never reads half a file."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    _replace(tmp, path)


def write_json_atomic(path, obj):
    write_bytes_atomic(path, (json.dumps(obj, indent=1, default=_json_default) + "\n").encode("utf-8"))


def _json_default(o):
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.floating):
        return float(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(f"not JSON: {type(o).__name__}")


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _check_layout_list(entries, total, what):
    """[{"name","offset","size"}] inside [0, total) without overlaps. Returns error strings."""
    errors = []
    if not isinstance(entries, list) or not entries:
        return [f"{what} is not a non-empty list"]
    spans = []
    for i, e in enumerate(entries):
        if not isinstance(e, dict) or not isinstance(e.get("name"), str) or not _is_int(e.get("offset")) \
                or not _is_int(e.get("size")):
            errors.append(f"{what}[{i}] needs a string name and integer offset and size")
            continue
        if e["offset"] < 0 or e["size"] < 1 or e["offset"] + e["size"] > total:
            errors.append(f"{what}[{i}] ({e['name']}) runs outside 0..{total}")
            continue
        spans.append((e["offset"], e["offset"] + e["size"], e["name"]))
    spans.sort()
    for a, b in zip(spans, spans[1:]):
        if b[0] < a[1]:
            errors.append(f"{what}: {a[2]} and {b[2]} overlap")
    return errors


# ---- recorded runs and practice attempts (s2, s4) ------------------------------

class Run:
    """One recorded run (contract s2) or practice attempt (s4): its header and arrays.

    The arrays are read-only numpy memmaps, opened on first use. Drop every
    reference to them (or call close()) before deleting the files: Windows
    can't delete a mapped file.
    """

    def __init__(self, json_path, header):
        self.json_path = os.path.abspath(json_path)
        self.header = header
        self.base = self.json_path[:-len(".json")]
        self.run_id = os.path.basename(self.base)
        self._maps = {}

    def __repr__(self):
        return f"<Run {self.run_id} {self.level_name!r} {self.ticks} ticks>"

    # The header, by name.
    source = property(lambda self: self.header["source"])
    level_id = property(lambda self: int(self.header.get("level_id") or 0))
    level_name = property(lambda self: str(self.header.get("level_name") or ""))
    macro_file = property(lambda self: str(self.header.get("macro_file") or ""))
    tps = property(lambda self: float(self.header["tps"]))
    ticks = property(lambda self: int(self.header["ticks"]))
    obs_dim = property(lambda self: int(self.header["obs_dim"]))
    layout_hash = property(lambda self: self.header["obs_layout_hash"])
    completed = property(lambda self: bool(self.header.get("completed")))
    died_tick = property(lambda self: self.header.get("died_tick"))
    # Practice branches (s4): which attempt and tick this one flipped. first_tick
    # is the tick of row 0 (not in the contract yet; 0 when absent).
    branch_of = property(lambda self: self.header.get("branch_of"))
    first_tick = property(lambda self: int(self.header.get("first_tick") or 0))

    @property
    def horizon(self):
        """H = round(1.0 * tps) ticks (s4)."""
        return int(round(SURVIVE_SECONDS * self.tps))

    def _map(self, ext, dtype, shape):
        arr = self._maps.get(ext)
        if arr is None:
            if self.ticks == 0:
                arr = np.zeros(shape, dtype)
            else:
                arr = np.memmap(self.base + ext, dtype=dtype, mode="r", shape=shape)
            self._maps[ext] = arr
        return arr

    @property
    def obs(self):
        return self._map(".obs.f16", "<f2", (self.ticks, self.obs_dim))

    @property
    def act(self):
        return self._map(".act.u8", "<u1", (self.ticks,))

    @property
    def meta(self):
        return self._map(".meta.f32", "<f4", (self.ticks, len(META_FIELDS)))

    @property
    def alive(self):
        return self._map(".alive.i32", "<i4", (self.ticks,))

    def close(self):
        self._maps.clear()

    def usable_ticks(self):
        """How many leading ticks the trainers learn from (s2).

        All of a completed run. A run that didn't complete loses its last
        DROP_SECONDS before the death (or before its end, when it just
        stopped): those ticks led into the death. Nothing from the first tick
        the meta marks dead on, minus the same margin, ever counts.
        """
        T = self.ticks
        if T == 0:
            return 0
        margin = int(round(DROP_SECONDS * self.tps))
        end = T
        if not self.completed:
            died = self.died_tick
            end = (int(died) if died is not None else T) - margin
        dead = np.flatnonzero(np.asarray(self.meta[:, META_DEAD]) >= 0.5)
        if len(dead):
            end = min(end, int(dead[0]) - margin)
        return int(min(max(end, 0), T))


def check_run_header(h, practice=False):
    """What's wrong with a run's header (s2, s4), as a list of strings (empty: fine)."""
    errors = []
    if not isinstance(h, dict):
        return ["the header is not a JSON object"]
    if h.get("format") != "gbai-run":
        errors.append(f"format is {h.get('format')!r}, not 'gbai-run'")
    if h.get("version") != 1:
        errors.append(f"version is {h.get('version')!r}, not 1")
    if h.get("source") not in ("human", "ai"):
        errors.append(f"source is {h.get('source')!r}, not 'human' or 'ai'")
    if practice and h.get("source") != "ai":
        errors.append("a practice attempt's source must be 'ai'")
    if not _is_int(h.get("level_id")) or h["level_id"] < 0:
        errors.append("level_id is not a non-negative integer")
    if not isinstance(h.get("level_name", ""), str):
        errors.append("level_name is not a string")
    if not _is_num(h.get("tps")) or not h["tps"] > 0:
        errors.append("tps is not a positive number")
    if not _is_int(h.get("ticks")) or h["ticks"] < 0:
        errors.append("ticks is not a non-negative integer")
    if not _is_int(h.get("obs_dim")) or h["obs_dim"] < 1:
        errors.append("obs_dim is not a positive integer")
    if not isinstance(h.get("obs_layout_hash"), str) or not HASH_RE.match(h["obs_layout_hash"]):
        errors.append(f"obs_layout_hash {h.get('obs_layout_hash')!r} is not 16 hex characters")
    if _is_int(h.get("obs_dim")):
        errors += _check_layout_list(h.get("obs_layout"), h["obs_dim"], "obs_layout")
    if h.get("meta_fields") != META_FIELDS:
        errors.append(f"meta_fields is {h.get('meta_fields')!r}, not {META_FIELDS}")
    if not isinstance(h.get("completed"), bool):
        errors.append("completed is not true or false")
    died = h.get("died_tick")
    if died is not None and (not _is_int(died) or died < 0 or (_is_int(h.get("ticks")) and died > h["ticks"])):
        errors.append(f"died_tick {died!r} is not null or a tick of the run")
    branch = h.get("branch_of")
    if branch is not None:
        if not isinstance(branch, dict) or not isinstance(branch.get("attempt"), str) \
                or not _is_int(branch.get("tick")) or branch["tick"] < 0:
            errors.append("branch_of is not {\"attempt\": name, \"tick\": t}")
    first = h.get("first_tick", 0)
    if first is not None and (not _is_int(first) or first < 0):
        errors.append("first_tick is not a non-negative integer")
    return errors


def _expected_files(base, ticks, obs_dim, practice):
    files = [(".obs.f16", ticks * obs_dim * 2), (".act.u8", ticks), (".meta.f32", ticks * len(META_FIELDS) * 4)]
    if practice:
        files.append((".alive.i32", ticks * 4))
    return [(base + ext, size) for ext, size in files]


def load_run(json_path, practice=False, deep=False):
    """One run (s2) or practice attempt (s4, practice=True), checked against the contract.

    Raises DataError naming the file and every problem: header fields, each
    array's size (exactly `ticks` rows), act holding only 0 and 1. deep=True
    also reads every value (finite obs, modes 0..7, alive in 0..H).
    """
    try:
        header = read_json(json_path)
    except (OSError, ValueError) as e:
        raise DataError(f"{json_path}: can't read the header: {e}")
    errors = check_run_header(header, practice)
    if errors:
        raise DataError(f"{json_path}: " + "; ".join(errors))
    run = Run(json_path, header)
    for path, size in _expected_files(run.base, run.ticks, run.obs_dim, practice):
        try:
            have = os.path.getsize(path)
        except OSError:
            errors.append(f"{os.path.basename(path)} is missing")
            continue
        if have != size:
            errors.append(f"{os.path.basename(path)} is {have} bytes, not {size} ({run.ticks} rows)")
    if not errors and run.ticks:
        act = np.fromfile(run.base + ".act.u8", np.uint8)
        if act.max() > 1:
            errors.append(f"act holds {int(act.max())}; only 0 and 1 are allowed")
        if deep:
            errors += _deep_check(run, practice)
    if errors:
        raise DataError(f"{json_path}: " + "; ".join(errors))
    return run


def _deep_check(run, practice):
    errors = []
    bad = 0
    for a in range(0, run.ticks, 65536):
        bad += int((~np.isfinite(np.asarray(run.obs[a:a + 65536], np.float32))).sum())
    if bad:
        errors.append(f"obs holds {bad} values that aren't finite")
    meta = np.asarray(run.meta)
    if not np.isfinite(meta).all():
        errors.append("meta holds values that aren't finite")
    modes = meta[:, META_MODE]
    if ((modes < 0) | (modes > len(MODES) - 1) | (modes != np.round(modes))).any():
        errors.append("meta mode is outside 0..7")
    speeds = meta[:, META_SPEED]
    if ((speeds < 0) | (speeds > len(SPEEDS) - 1)).any():
        errors.append("meta speed is outside 0..4")
    if practice:
        alive = np.asarray(run.alive)
        if (alive < 0).any() or (alive > run.horizon).any():
            errors.append(f"alive is outside 0..{run.horizon} (H = round(1.0 * tps))")
    run.close()
    return errors


def list_runs(root, source="human", deep=False):
    """Every recorded run in <root>\\data (s2) that passes the checks, by run_id.

    source: "human", "ai" or None for both. Returns (runs, problems), problems
    being (path, why) for each file that didn't pass.
    """
    folder = os.path.join(root, "data")
    runs, problems = [], []
    if not os.path.isdir(folder):
        return runs, problems
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".json"):
            continue
        try:
            run = load_run(os.path.join(folder, name), deep=deep)
        except DataError as e:
            problems.append((os.path.join(folder, name), str(e)))
            continue
        if source is None or run.source == source:
            runs.append(run)
    return runs, problems


def list_practice(root, deep=False):
    """Every practice attempt under <root>\\practice\\<level_id>\\ (s4). Returns (attempts, problems)."""
    base = os.path.join(root, "practice")
    runs, problems = [], []
    if not os.path.isdir(base):
        return runs, problems
    for level in sorted(os.listdir(base)):
        folder = os.path.join(base, level)
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".json"):
                continue
            path = os.path.join(folder, name)
            try:
                run = load_run(path, practice=True, deep=deep)
            except DataError as e:
                problems.append((path, str(e)))
                continue
            if str(run.level_id) != level:
                problems.append((path, f"level_id {run.level_id} doesn't match its folder {level}"))
                continue
            runs.append(run)
    return runs, problems


def pick_layout(runs, want=None):
    """The runs recorded with one observation layout, and its hash.

    A network only fits the encoder it was trained for (the mod refuses
    weights whose obs_layout_hash differs from its own), so one training run
    uses one layout. want: the hash to use (gucci_ai_layout gives the mod's
    current one). Without it, every run must share one hash. Runs with the
    same hash must carry the same layout too (else the mod's hash is broken).
    """
    if not runs:
        raise DataError("no runs to train on")
    groups = {}
    for r in runs:
        groups.setdefault(r.layout_hash, []).append(r)
    if want:
        if want not in groups:
            raise DataError(f"no run was recorded with obs layout {want}; found " +
                            ", ".join(f"{h} ({len(g)} runs)" for h, g in sorted(groups.items())))
        chosen = want
    elif len(groups) > 1:
        lines = []
        for h, g in sorted(groups.items(), key=lambda kv: -max(os.path.getmtime(r.json_path) for r in kv[1])):
            newest = max(os.path.getmtime(r.json_path) for r in g)
            lines.append(f"{h}: {len(g)} runs, newest {time.strftime('%Y-%m-%d %H:%M', time.localtime(newest))}")
        raise DataError("the runs were recorded with different observation layouts (" + "; ".join(lines) +
                        "). Pass --layout-hash with the mod's current one (gucci_ai_layout) and "
                        "record the others again.")
    else:
        chosen = next(iter(groups))
    chosen_runs = groups[chosen]
    ref = chosen_runs[0]
    for r in chosen_runs[1:]:
        if r.obs_dim != ref.obs_dim or r.header["obs_layout"] != ref.header["obs_layout"]:
            raise DataError(f"{r.run_id} and {ref.run_id} share obs_layout_hash {chosen} but not the layout "
                            "itself: the mod's layout hash is broken")
    return chosen_runs, chosen


# ---- level tapes (s3) ----------------------------------------------------------

class Tape:
    """A level as columns (s3): float16 [C][K], a read-only memmap opened on first use."""

    def __init__(self, json_path, header):
        self.json_path = os.path.abspath(json_path)
        self.header = header
        self.base = self.json_path[:-len(".json")]
        self._data = None

    level_id = property(lambda self: int(self.header["level_id"]))
    columns = property(lambda self: int(self.header["columns"]))
    channels = property(lambda self: int(self.header["channels"]))
    x0 = property(lambda self: float(self.header["x0"]))
    y0 = property(lambda self: float(self.header["y0"]))
    column_width = property(lambda self: float(self.header["column_width"]))
    band_height = property(lambda self: float(self.header["band_height"]))
    channel_layout = property(lambda self: self.header["channel_layout"])
    # Not in contract v1 (see CONTRACT_ISSUES.md): the tape encoder's own hash.
    layout_hash = property(lambda self: self.header.get("layout_hash"))

    @property
    def data(self):
        if self._data is None:
            shape = (self.columns, self.channels)
            self._data = np.zeros(shape, np.float16) if self.columns == 0 else \
                np.memmap(self.base + ".tape.f16", dtype="<f2", mode="r", shape=shape)
        return self._data

    def close(self):
        self._data = None

    def column_of(self, x):
        """The column (int array) each x (GD units) falls in; may be outside 0..C-1."""
        return np.floor((np.asarray(x, np.float64) - self.x0) / self.column_width).astype(np.int64)


def check_tape_header(h, level_id=None):
    errors = []
    if not isinstance(h, dict):
        return ["the header is not a JSON object"]
    if h.get("format") != "gbai-tape":
        errors.append(f"format is {h.get('format')!r}, not 'gbai-tape'")
    if h.get("version") != 1:
        errors.append(f"version is {h.get('version')!r}, not 1")
    if not _is_int(h.get("level_id")):
        errors.append("level_id is not an integer")
    elif level_id is not None and h["level_id"] != level_id:
        errors.append(f"level_id {h['level_id']} doesn't match the file name ({level_id})")
    for key in ("column_width", "band_height"):
        if not _is_num(h.get(key)) or not h[key] > 0:
            errors.append(f"{key} is not a positive number")
    for key in ("x0", "y0"):
        if not _is_num(h.get(key)):
            errors.append(f"{key} is not a number")
    for key in ("columns", "channels"):
        if not _is_int(h.get(key)) or h[key] < (0 if key == "columns" else 1):
            errors.append(f"{key} is not a valid count")
    if _is_int(h.get("channels")):
        errors += _check_layout_list(h.get("channel_layout"), h["channels"], "channel_layout")
    lh = h.get("layout_hash")
    if lh is not None and (not isinstance(lh, str) or not HASH_RE.match(lh)):
        errors.append(f"layout_hash {lh!r} is not 16 hex characters")
    return errors


def load_tape(root, level_id, deep=False):
    """The tape of one level (s3), checked; None when it hasn't been recorded."""
    path = os.path.join(root, "tapes", f"{int(level_id)}.json")
    if not os.path.exists(path):
        return None
    return _load_tape_file(path, int(level_id), deep)


def _load_tape_file(path, level_id, deep=False):
    try:
        header = read_json(path)
    except (OSError, ValueError) as e:
        raise DataError(f"{path}: can't read the header: {e}")
    errors = check_tape_header(header, level_id)
    if not errors:
        tape = Tape(path, header)
        want = tape.columns * tape.channels * 2
        try:
            have = os.path.getsize(tape.base + ".tape.f16")
            if have != want:
                errors.append(f"{os.path.basename(tape.base)}.tape.f16 is {have} bytes, not {want}")
        except OSError:
            errors.append(f"{os.path.basename(tape.base)}.tape.f16 is missing")
        if not errors and deep and tape.columns:
            if not np.isfinite(np.asarray(tape.data, np.float32)).all():
                errors.append("the tape holds values that aren't finite")
            tape.close()
    if errors:
        raise DataError(f"{path}: " + "; ".join(errors))
    return tape


def list_tapes(root, deep=False):
    """{level_id: Tape} for every tape in <root>\\tapes, and (path, why) problems."""
    folder = os.path.join(root, "tapes")
    tapes, problems = {}, []
    if not os.path.isdir(folder):
        return tapes, problems
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".json"):
            continue
        stem = name[:-5]
        path = os.path.join(folder, name)
        if not stem.isdigit():
            problems.append((path, "a tape is named <level_id>.json"))
            continue
        try:
            tapes[int(stem)] = _load_tape_file(path, int(stem), deep)
        except DataError as e:
            problems.append((path, str(e)))
    return tapes, problems


# ---- writing synthetic data (tests only) -----------------------------------------

def _refuse_real_root(folder):
    real = os.path.normcase(os.path.abspath(default_ai_root()))
    here = os.path.normcase(os.path.abspath(folder))
    if here == real or here.startswith(real + os.sep):
        raise RuntimeError(f"refusing to write into the mod's own AI_ROOT ({real}): only the mod writes "
                           "data\\, tapes\\ and practice\\")


def write_run(folder, run_id, header, obs, act, meta, alive=None):
    """Writes one run (s2) or practice attempt (s4, with alive) in the contract's format.

    For tests and synthetic data only: the trainer never writes into AI_ROOT's
    data\\, tapes\\ or practice\\, and this refuses the mod's real AI_ROOT.
    """
    _refuse_real_root(folder)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, run_id)
    np.ascontiguousarray(obs, "<f2").tofile(base + ".obs.f16")
    np.ascontiguousarray(act, "<u1").tofile(base + ".act.u8")
    np.ascontiguousarray(meta, "<f4").tofile(base + ".meta.f32")
    if alive is not None:
        np.ascontiguousarray(alive, "<i4").tofile(base + ".alive.i32")
    write_json_atomic(base + ".json", header)
    return base + ".json"


def write_tape(folder, header, data):
    """Writes one tape (s3); tests only, like write_run()."""
    _refuse_real_root(folder)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, str(int(header["level_id"])))
    np.ascontiguousarray(data, "<f2").tofile(base + ".tape.f16")
    write_json_atomic(base + ".json", header)
    return base + ".json"


# ---- showcase levels: never trained on -------------------------------------------

# Nigel's showcase (bar) levels. Every result he sees on them must come from a
# model that never learned them, so no trainer ever takes a tick from them, in
# any macro format. Evaluation only.
SHOWCASE_LEVELS = ["Bloodbath", "The Nightmare", "Clubstep", "Theory of Everything 2", "Deadlocked", "LIMBO",
                   "ORBIT", "Heliopolis"]
# IDs known for certain. RobTop's levels keep their main-level number (Clubstep
# 14, Theory of Everything 2 18, Deadlocked 20). The online ones were confirmed
# by both the Geode census log and Nigel's save (DATA.md, "Bar-level IDs").
# Every showcase level is matched by id AND by name, so neither a recorder
# that leaves level_name empty nor a renamed copy lets one through.
SHOWCASE_IDS = {10565740: "Bloodbath", 13519: "The Nightmare", 14: "Clubstep", 18: "Theory of Everything 2",
                20: "Deadlocked", 86084399: "LIMBO", 133175713: "ORBIT", 136530685: "Heliopolis"}
# Name matches, after name_key(): anywhere in the level's name or the macro's
# file name. Generous on purpose (it also catches "Silent Clubstep" and
# "Null Clubstep", copies of Clubstep's layout): a level wrongly left out
# costs a little data, one wrongly let in costs the honesty of every result.
SHOWCASE_KEYS = ["bloodbath", "thenightmare", "clubstep", "theoryofeverything2", "toe2", "deadlocked", "limbo",
                 "orbit", "heliopolis"]


def name_key(text):
    """Lowercase letters and digits only: 'Heliopolis      ' -> 'heliopolis'."""
    return re.sub(r"[^a-z0-9]", "", str(text or "").lower())


class Showcase:
    """Which levels are showcase levels. extra_ids/extra_names add to the lists;
    a manifest adds every entry marked is_bar_level."""

    def __init__(self, extra_ids=(), extra_names=(), manifest=None):
        self.ids = dict(SHOWCASE_IDS)
        self.keys = list(SHOWCASE_KEYS)
        for i in extra_ids:
            self.ids[int(i)] = "listed"
        for n in extra_names:
            if name_key(n):
                self.keys.append(name_key(n))
        if manifest:
            for e in read_manifest(manifest):
                if e["is_bar_level"]:
                    if e["level_id"]:
                        self.ids[e["level_id"]] = e["level_name"] or "a bar level in the manifest"
                    if name_key(e["level_name"]):
                        self.keys.append(name_key(e["level_name"]))

    def reason(self, level_id=0, level_name="", macro_file=""):
        """Why this level is a showcase level, or None."""
        if level_id and int(level_id) in self.ids:
            return f"showcase level id {int(level_id)} ({self.ids[int(level_id)]})"
        stem = os.path.splitext(os.path.basename(str(macro_file or "").replace("\\", "/")))[0]
        for text, what in ((level_name, "level name"), (stem, "macro file")):
            k = name_key(text)
            for key in self.keys:
                if key and key in k:
                    return f"showcase {what} {text!r} (matches {key!r})"
        return None

    def run_reason(self, run):
        return self.reason(run.level_id, run.level_name, run.macro_file)


def read_manifest(path):
    """The training-data manifest's entries, normalized (its schema isn't in the contract).

    Accepts a list of entries or an object holding one under "entries",
    "macros" or "items". Each entry -> {"macro_path", "level_id", "level_name",
    "use_for_training", "is_bar_level", "raw"}; fields read: macro_path / path /
    file / macro_file, level_id / id, level_name / name / level.
    """
    data = read_json(path)
    if isinstance(data, dict):
        for key in ("entries", "macros", "items"):
            if isinstance(data.get(key), list):
                data = data[key]
                break
    if not isinstance(data, list):
        raise DataError(f"{path}: expected a list of entries (or an object with 'entries')")
    out = []
    for e in data:
        if not isinstance(e, dict):
            continue
        path_ = next((e[k] for k in ("macro_path", "path", "file", "macro_file") if isinstance(e.get(k), str)), "")
        lid = next((e[k] for k in ("level_id", "id") if _is_int(e.get(k))), 0)
        if not lid:
            for k in ("level_id", "id"):
                if isinstance(e.get(k), str) and e[k].strip().isdigit():
                    lid = int(e[k].strip())
                    break
        name = next((e[k] for k in ("level_name", "name", "level") if isinstance(e.get(k), str)), "")
        out.append({"macro_path": path_, "level_id": int(lid), "level_name": name,
                    "use_for_training": bool(e.get("use_for_training")), "is_bar_level": bool(e.get("is_bar_level")),
                    "raw": e})
    return out


# ---- splits by level -------------------------------------------------------------

def level_key(run_or_id, name=""):
    """A level's identity for splits: 'id:<level_id>', or 'name:<name_key>' for a level without an id."""
    if isinstance(run_or_id, (Run, Tape)):
        lid = run_or_id.level_id
        name = getattr(run_or_id, "level_name", "")
    else:
        lid = int(run_or_id or 0)
    return f"id:{lid}" if lid else f"name:{name_key(name)}"


def parse_fold(text):
    """'k/K' -> (k, K) with 0 <= k < K, K >= 2."""
    m = re.match(r"^\s*(\d+)\s*/\s*(\d+)\s*$", str(text or ""))
    if not m:
        raise ValueError(f"--fold is k/K (like 0/5), not {text!r}")
    k, n = int(m.group(1)), int(m.group(2))
    if n < 2 or not 0 <= k < n:
        raise ValueError(f"--fold {text}: needs K >= 2 and 0 <= k < K")
    return k, n


def split_levels(keys, fold=None, val_frac=0.15, seed=0):
    """(train keys, validation keys), split by level, never by tick.

    fold=(k, K): the levels, shuffled once by `seed`, dealt into K folds;
    fold k validates and the rest train (K = the number of levels is leave-
    one-out). Without a fold, about val_frac of the levels validate (at least
    one when there are two or more and val_frac > 0); val_frac=0 trains on
    them all.
    """
    keys = sorted(set(keys))
    order = [keys[i] for i in np.random.default_rng(seed).permutation(len(keys))]
    if fold is not None:
        k, n = fold
        if n > len(keys):
            raise DataError(f"--fold {k}/{n}: only {len(keys)} levels to split")
        val = [key for i, key in enumerate(order) if i % n == k]
    else:
        n_val = int(round(len(keys) * val_frac))
        if val_frac > 0 and len(keys) >= 2:
            n_val = max(1, n_val)
        n_val = min(n_val, max(0, len(keys) - 1))
        val = order[:n_val]
    train = [key for key in keys if key not in set(val)]
    return train, sorted(val)


def make_split(runs, showcase, fold=None, val_frac=0.15, seed=0, usable=None):
    """Leaves the showcase levels out, then splits the rest by level.

    usable (optional): run -> bool. A run it rejects has nothing to learn
    from (0 ticks, or a death inside its first second) and is left out
    BEFORE the split, so a level with nothing in it can't take a validation
    or fold slot: that left a fold with nothing to validate on, or training
    with nothing to train on, depending on how the levels happened to fall.

    Returns {"train": runs, "val": runs, "excluded": [(run, reason)],
    "unusable": runs, "train_levels": keys, "val_levels": keys}. A showcase
    run is never in train or val, whatever the fold.
    """
    pool, excluded, unusable = [], [], []
    for r in runs:
        why = showcase.run_reason(r)
        if why:
            excluded.append((r, why))
        elif usable is not None and not usable(r):
            unusable.append(r)
        else:
            pool.append(r)
    train_keys, val_keys = split_levels([level_key(r) for r in pool], fold, val_frac, seed)
    tk, vk = set(train_keys), set(val_keys)
    return {"train": [r for r in pool if level_key(r) in tk], "val": [r for r in pool if level_key(r) in vk],
            "excluded": excluded, "unusable": unusable, "train_levels": train_keys, "val_levels": val_keys}


def split_record(split, model, layout_hash, fold=None, val_frac=None, seed=0):
    """split.json's contents: what trained, what validated, what was left out and why.
    evaluate.py reads it to tell held-out levels from trained ones."""
    def runs(rs):
        return [{"run_id": r.run_id, "level_id": r.level_id, "level_name": r.level_name} for r in rs]
    return {"format": "gbai-split", "version": 1, "model": model, "created": now_iso(), "layout_hash": layout_hash,
            "fold": f"{fold[0]}/{fold[1]}" if fold else None, "val_frac": None if fold else val_frac, "seed": seed,
            "train_levels": split["train_levels"], "val_levels": split["val_levels"],
            "train_runs": runs(split["train"]), "val_runs": runs(split["val"]),
            "showcase_excluded": [dict(runs([r])[0], reason=why) for r, why in split["excluded"]],
            "unusable_runs": runs(split.get("unusable", []))}


# ---- per-tick helpers ------------------------------------------------------------

def before(act):
    """The button before each tick: act[t-1], released (0) before tick 0 (s1)."""
    a = np.asarray(act).astype(bool)
    h = np.zeros_like(a)
    h[1:] = a[:-1]
    return h


def transitions(act):
    """Ticks where the button changes (s1): a press at t is act[t-1]=0, act[t]=1."""
    a = np.asarray(act).astype(bool)
    return np.flatnonzero(a != before(a))


def near_mask(act, k):
    """True on every tick within +-k ticks of a press or release."""
    T = len(act)
    tr = transitions(act)
    diff = np.zeros(T + 1, np.int64)
    np.add.at(diff, np.clip(tr - k, 0, T), 1)
    np.add.at(diff, np.clip(tr + k + 1, 0, T), -1)
    return np.cumsum(diff[:-1]) > 0


def finite_rows(x, fill=0.0):
    """x with non-finite values replaced in place (float arrays); returns how many there were."""
    bad = ~np.isfinite(x)
    n = int(bad.sum())
    if n:
        x[bad] = fill
    return n


def clean_rows(x):
    """Rows read from a run (float16) as a float32 copy, values that aren't finite set to 0.

    Training does that to its rows (finite_rows), so every prediction made
    for validation or a report goes through here too. Without it the same
    broken value (a float16 overflow is inf) read as P = 0.5, a hold, in
    train_pilot's validation and as NaN, a release, in evaluate.py.
    """
    out = np.array(x, np.float32)
    finite_rows(out)
    return out


def limit_blas_threads(n):
    """Caps numpy's own BLAS at n threads; True when it found it.

    torch.set_num_threads doesn't reach it: the numpy wheels bundle their own
    OpenBLAS, which starts with one thread per logical CPU (12 on Nigel's
    PC), so the numpy forward passes (evaluate.py, the export checks) took
    the whole machine while training was told to use 6. Measured 2026-11-02:
    about 9 cores busy before, 2 after limiting it to 2. False (nothing done)
    where numpy was built against another BLAS.
    """
    n = max(1, int(n))
    folder = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(np.__file__))), "numpy.libs")
    for path in sorted(glob.glob(os.path.join(folder, "*openblas*"))):
        try:
            lib = ctypes.CDLL(path)  # the copy numpy already loaded: Windows hands back the same module
        except OSError:
            continue
        for name in ("scipy_openblas_set_num_threads64_", "scipy_openblas_set_num_threads",
                     "openblas_set_num_threads64_", "openblas_set_num_threads"):
            fn = getattr(lib, name, None)
            if fn is not None:
                fn(n)
                return True
    return False


def moments(chunks, n_in):
    """Per-feature (mean, std) over 2-D float16/float32 chunks, accumulated in float64.

    Keep chunks to ~16k rows: each becomes float64 here (430 inputs x 16k rows
    is 56 MB). std is 1 where a feature never changes, so the mod's
    (x-mean)/std never divides by zero.
    """
    total = 0
    s = np.zeros(n_in, np.float64)
    ss = np.zeros(n_in, np.float64)
    for c in chunks:
        c = np.asarray(c, np.float64)
        total += len(c)
        s += c.sum(0)
        ss += (c * c).sum(0)
    if total == 0:
        return np.zeros(n_in, np.float32), np.ones(n_in, np.float32)
    mean = s / total
    var = np.maximum(ss / total - mean * mean, 0.0)
    std = np.sqrt(var)
    std = np.where(std > 1e-6, std, 1.0)
    return mean.astype(np.float32), std.astype(np.float32)


# ---- GBNN weights (s5) -----------------------------------------------------------

GBNN_MAGIC = b"GBNN"
GBNN_VERSION = 1
KIND_MLP, KIND_CONV = 0, 1
LAYER_DENSE, LAYER_CONV1D = 0, 1
ACT_NONE, ACT_RELU, ACT_SIGMOID, ACT_TANH = 0, 1, 2, 3
ACT_NAMES = {ACT_NONE: "none", ACT_RELU: "relu", ACT_SIGMOID: "sigmoid", ACT_TANH: "tanh"}


class Layer:
    """One GBNN layer: weight float32 [out][in][kernel], bias float32 [out]."""

    def __init__(self, type, n_in, n_out, kernel, dilation, activation, weight, bias):
        self.type = int(type)
        self.n_in = int(n_in)
        self.n_out = int(n_out)
        self.kernel = int(kernel)
        self.dilation = int(dilation)
        self.activation = int(activation)
        self.weight = np.ascontiguousarray(weight, np.float32).reshape(self.n_out, self.n_in, self.kernel)
        self.bias = np.ascontiguousarray(bias, np.float32).reshape(self.n_out)

    def describe(self):
        if self.type == LAYER_DENSE:
            return f"dense {self.n_in} -> {self.n_out}, {ACT_NAMES.get(self.activation)}"
        return (f"conv1d {self.n_in} -> {self.n_out}, kernel {self.kernel}, dilation {self.dilation}, "
                f"{ACT_NAMES.get(self.activation)}")


class GBNN:
    """A network as the mod reads it: kind, layout hash, input mean/std, layers, output names."""

    def __init__(self, kind, layout_hash, mean, std, layers, out_names):
        self.kind = int(kind)
        self.layout_hash = str(layout_hash)
        self.mean = np.ascontiguousarray(mean, np.float32).reshape(-1)
        self.std = np.ascontiguousarray(std, np.float32).reshape(-1)
        self.layers = list(layers)
        self.out_names = [str(n) for n in out_names]

    n_in = property(lambda self: len(self.mean))
    n_out = property(lambda self: self.layers[-1].n_out if self.layers else 0)

    def describe(self):
        lines = [f"GBNN v{GBNN_VERSION}, {'mlp' if self.kind == KIND_MLP else 'conv1d stack'}, "
                 f"layout {self.layout_hash}, {self.n_in} inputs -> {self.n_out} outputs "
                 f"({', '.join(self.out_names)})"]
        lines += [f"  {i}: {layer.describe()}" for i, layer in enumerate(self.layers)]
        return "\n".join(lines)


def check_gbnn(net):
    """Raises DataError unless the network is one the mod can run (s5)."""
    errors = []
    if net.kind not in (KIND_MLP, KIND_CONV):
        errors.append(f"kind {net.kind} is not 0 (mlp) or 1 (conv1d stack)")
    if not HASH_RE.match(net.layout_hash or ""):
        errors.append(f"obs_layout_hash {net.layout_hash!r} is not 16 hex characters")
    if net.n_in < 1 or len(net.std) != net.n_in:
        errors.append("mean and std need one value per input")
    if not (np.isfinite(net.mean).all() and np.isfinite(net.std).all()):
        errors.append("mean or std holds values that aren't finite")
    if (net.std <= 0).any():
        errors.append("std holds values that aren't positive (the mod divides by it and refuses std <= 0)")
    if not net.layers:
        errors.append("no layers")
    width = net.n_in
    for i, L in enumerate(net.layers):
        want = LAYER_DENSE if net.kind == KIND_MLP else LAYER_CONV1D
        if L.type != want:
            errors.append(f"layer {i}: type {L.type} in a {'mlp' if net.kind == KIND_MLP else 'conv1d stack'}")
        if L.n_in != width:
            errors.append(f"layer {i}: takes {L.n_in} inputs, the one before gives {width}")
        if L.activation not in ACT_NAMES:
            errors.append(f"layer {i}: activation {L.activation} is not 0..3")
        if L.type == LAYER_DENSE and (L.kernel != 1 or L.dilation != 1):
            errors.append(f"layer {i}: a dense layer has kernel 1 and dilation 1")
        if L.type == LAYER_CONV1D and (L.kernel < 1 or L.kernel % 2 == 0 or L.dilation < 1):
            errors.append(f"layer {i}: conv1d needs an odd kernel and dilation >= 1")
        if not (np.isfinite(L.weight).all() and np.isfinite(L.bias).all()):
            errors.append(f"layer {i}: weights that aren't finite")
        width = L.n_out
    if len(net.out_names) != width:
        errors.append(f"{len(net.out_names)} output names for {width} outputs")
    for n in net.out_names:
        if not n or not n.isascii():
            errors.append(f"output name {n!r} is empty or not ASCII")
    if errors:
        raise DataError("GBNN: " + "; ".join(errors))


def gbnn_bytes(net):
    """The file's bytes, exactly as contract s5 lays them out (little-endian)."""
    check_gbnn(net)
    parts = [struct.pack("<4sII16sI", GBNN_MAGIC, GBNN_VERSION, net.kind, net.layout_hash.encode("ascii"), net.n_in),
             net.mean.astype("<f4").tobytes(), net.std.astype("<f4").tobytes(), struct.pack("<I", len(net.layers))]
    for L in net.layers:
        parts.append(struct.pack("<6I", L.type, L.n_in, L.n_out, L.kernel, L.dilation, L.activation))
        parts.append(L.weight.astype("<f4").tobytes())
        parts.append(L.bias.astype("<f4").tobytes())
    parts.append(struct.pack("<I", len(net.out_names)))
    for n in net.out_names:
        b = n.encode("ascii")
        parts.append(struct.pack("<I", len(b)) + b)
    return b"".join(parts)


def write_gbnn(path, net):
    """Writes the GBNN file (atomically); returns its bytes."""
    data = gbnn_bytes(net)
    write_bytes_atomic(path, data)
    return data


def parse_gbnn(data, what="GBNN"):
    """A GBNN from its bytes, checked (DataError otherwise; nothing may follow the names)."""
    view = memoryview(data)
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(view):
            raise DataError(f"{what}: truncated at byte {pos} (wanted {n} more of {len(view)})")
        chunk = view[pos:pos + n]
        pos += n
        return chunk

    def u32():
        return struct.unpack("<I", take(4))[0]

    def f32(n):
        return np.frombuffer(take(4 * n), "<f4").astype(np.float32)

    magic, version, kind, hash_bytes, n_in = struct.unpack("<4sII16sI", take(32))
    if magic != GBNN_MAGIC:
        raise DataError(f"{what}: not a GBNN file (starts {bytes(magic)!r})")
    if version != GBNN_VERSION:
        raise DataError(f"{what}: version {version}, this reads {GBNN_VERSION}")
    try:
        layout_hash = hash_bytes.decode("ascii")
    except UnicodeDecodeError:
        raise DataError(f"{what}: obs_layout_hash is not ASCII")
    if n_in > (len(view) - pos) // 8:
        raise DataError(f"{what}: {n_in} inputs can't fit in the file")
    mean = f32(n_in)
    std = f32(n_in)
    n_layers = u32()
    if n_layers > 4096:
        raise DataError(f"{what}: {n_layers} layers")
    layers = []
    for i in range(n_layers):
        t, li, lo, k, d, a = struct.unpack("<6I", take(24))
        count = lo * li * k
        if count > (len(view) - pos) // 4:
            raise DataError(f"{what}: layer {i}'s {count} weights run past the end of the file")
        w = f32(count)
        b = f32(lo)
        layers.append(Layer(t, li, lo, k, d, a, w, b))
    n_names = u32()
    if n_names > 65536:
        raise DataError(f"{what}: {n_names} output names")
    names = []
    for _ in range(n_names):
        n = u32()
        raw = bytes(take(n))
        try:
            names.append(raw.decode("ascii"))
        except UnicodeDecodeError:
            raise DataError(f"{what}: an output name is not ASCII")
    if pos != len(view):
        raise DataError(f"{what}: {len(view) - pos} bytes after the output names")
    net = GBNN(kind, layout_hash, mean, std, layers, names)
    check_gbnn(net)
    return net


def read_gbnn(path):
    with open(path, "rb") as f:
        return parse_gbnn(f.read(), what=path)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---- progress files another process can poll -------------------------------------

class Progress:
    """A JSON file saying where a training run is, rewritten whole (write, then rename).

    Fields: model, state (starting/loading/training/exporting/done/failed),
    pid, started, updated, epoch, epochs, step, steps_per_epoch, train_loss,
    val_loss, val (metrics), best_epoch, eta_seconds, eta, history (one entry
    per finished epoch), message, exported. set() writes at most every
    `every` seconds unless forced; finish() always writes.
    """

    def __init__(self, path, model, out_dir, every=5.0):
        self.path = path
        self.every = every
        self._last = 0.0
        self.data = {"format": "gbai-progress", "model": model, "state": "starting", "pid": os.getpid(),
                     "started": now_iso(), "updated": now_iso(), "out_dir": out_dir, "epoch": 0, "epochs": None,
                     "eta_seconds": None, "eta": "?", "history": [], "message": ""}
        self.write()

    def write(self):
        self.data["updated"] = now_iso()
        write_json_atomic(self.path, self.data)
        self._last = time.time()

    def set(self, force=False, **fields):
        self.data.update(fields)
        if "eta_seconds" in fields:
            self.data["eta"] = fmt_seconds(fields["eta_seconds"])
        if force or time.time() - self._last >= self.every:
            self.write()

    def finish(self, state, message="", **fields):
        self.data.update(fields)
        self.data["state"] = state
        self.data["message"] = message
        if state == "done":
            self.data["eta_seconds"] = 0
            self.data["eta"] = "0 s"
        self.write()


def read_progress(path):
    try:
        return read_json(path)
    except (OSError, ValueError):
        return None


# ---- command line ----------------------------------------------------------------

def _cmd_list(root):
    print(f"AI_ROOT: {root}")
    show = Showcase()
    runs, problems = list_runs(root, source=None)
    levels = {}
    for r in runs:
        levels.setdefault(level_key(r), []).append(r)
    print(f"\nRecorded runs (data\\): {len(runs)} in {len(levels)} levels")
    for key, rs in sorted(levels.items(), key=lambda kv: kv[1][0].level_name.lower()):
        r0 = rs[0]
        flag = "  SHOWCASE (evaluation only)" if show.run_reason(r0) else ""
        print(f"  {r0.level_name or '?'} ({key}){flag}")
        for r in rs:
            state = "completed" if r.completed else f"died at tick {r.died_tick}"
            print(f"    {r.run_id}: {r.source}, {r.ticks} ticks at {r.tps:g} tps ({r.usable_ticks()} usable), "
                  f"{state}, layout {r.layout_hash}")
    hashes = sorted({r.layout_hash for r in runs})
    if len(hashes) > 1:
        print(f"  NOTE: {len(hashes)} different obs layouts: {', '.join(hashes)}")
    tapes, tape_problems = list_tapes(root)
    print(f"\nTapes (tapes\\): {len(tapes)}")
    for lid, t in sorted(tapes.items()):
        print(f"  {lid}: {t.columns} columns x {t.channels} channels")
    practice, practice_problems = list_practice(root)
    per = {}
    for p in practice:
        per.setdefault(p.level_id, []).append(p)
    print(f"\nPractice attempts (practice\\): {len(practice)} in {len(per)} levels")
    for lid, ps in sorted(per.items()):
        branches = sum(1 for p in ps if p.branch_of)
        print(f"  {lid}: {len(ps)} attempts ({branches} branches), {sum(p.ticks for p in ps)} ticks")
    wdir = os.path.join(root, "weights")
    print("\nWeights (weights\\):")
    found = False
    if os.path.isdir(wdir):
        for name in sorted(os.listdir(wdir)):
            if name.endswith(".gbnn"):
                found = True
                try:
                    print("  " + name + ": " + read_gbnn(os.path.join(wdir, name)).describe().replace("\n", "\n  "))
                except DataError as e:
                    print(f"  {name}: UNREADABLE: {e}")
    if not found:
        print("  (none)")
    all_problems = problems + tape_problems + practice_problems
    if all_problems:
        print(f"\nFiles that don't follow the contract ({len(all_problems)}):")
        for path, why in all_problems:
            print(f"  {why}")
    return 0


def _cmd_check(root):
    print(f"AI_ROOT: {root}")
    runs, problems = list_runs(root, source=None, deep=True)
    tapes, tape_problems = list_tapes(root, deep=True)
    practice, practice_problems = list_practice(root, deep=True)
    for r in runs + practice:
        r.close()
    all_problems = problems + tape_problems + practice_problems
    print(f"{len(runs)} runs, {len(tapes)} tapes, {len(practice)} practice attempts passed")
    for path, why in all_problems:
        print(f"FAIL {why}")
    hashes = sorted({r.layout_hash for r in runs + practice})
    if len(hashes) > 1:
        print(f"NOTE: {len(hashes)} obs layouts in use: {', '.join(hashes)} (training uses one)")
    return 1 if all_problems else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="GucciBot AI data: list and check what the mod recorded.")
    ap.add_argument("command", choices=["list", "check", "gbnn"])
    ap.add_argument("file", nargs="?", help="gbnn: the weights file")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    args = ap.parse_args(argv)
    if args.command == "gbnn":
        if not args.file:
            ap.error("gbnn needs a FILE")
        net = read_gbnn(args.file)
        print(net.describe())
        print(f"sha256 {sha256_file(args.file)}")
        return 0
    root = ai_root(args.root)
    return _cmd_list(root) if args.command == "list" else _cmd_check(root)


if __name__ == "__main__":
    sys.exit(main())

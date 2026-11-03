r"""GucciBot AI: the trainer's side of the files the mod writes (contract v2).

The contract (CONTRACT.md, "AI pilot contract v2") fixes what the mod records
under AI_ROOT and what the trainer gives back:

    AI_ROOT\data\<run_id>.json + .state.f32 .scan.u8 .tok.u8 .act.u8 .meta.f32
                                  + .golden.f32 .golden.idx.i32       recorded runs (s3, s4)
    AI_ROOT\tapes\<level_id>.json + .tape.u8                       the level as columns (s5)
    AI_ROOT\practice\<level_id>\<attempt>.json + s3 arrays + .p.f32 .alive.i32
                                                                    the AI's own attempts (s6)
    AI_ROOT\weights\<model>.gbnn                                   trained networks (s7)
    AI_ROOT\validation\<run_id>.json                              the real game's verdict (Python's)

Each tick is stored compact (s3): 40 state floats, 160 scan bytes and 8
tokens of 6 bytes, 368 bytes in all. Every network sees the same 472-float
expansion of it (expand_np here, expand_torch for training batches), which
must match the mod's C++ bit for bit: the recorder writes golden expanded
rows next to every run, and loading a run checks them.

Python only ever reads data\, tapes\ and practice\ (the mod owns them). It
writes weights\ (orchestrate.py installs them there), its own train\
folder (checkpoints, progress files, reports) and validation\: one sidecar
per recorded run that died in the simulation, holding what
gucci_ai_validate_real said when the same macro played in the real game
(orchestrate.py record writes them; usable_ticks reads them).
Arrays are read whole with np.fromfile (random batches from a memmap are ~50x
slower on Nigel's PC, TOOLCHAIN.md 4.3), one run at a time, and dropped with
close() once copied.

    import gbai
    root = gbai.ai_root()                    # or $GUCCIBOT_AI_ROOT
    runs, problems = gbai.list_runs(root)    # human runs that passed the checks
    runs, layout_hash = gbai.pick_layout(runs)
    run = runs[0]
    run.obs[:run.usable_ticks()]             # float32 [T][472], expanded as read

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
RUN_VERSION = 2  # recorded runs and practice attempts (s4)
TAPE_VERSION = 1  # level tapes (s5)
META_FIELDS = ["x", "y", "yvel", "mode", "gravity_flipped", "mini", "speed", "dead"]
META_X, META_Y, META_YVEL, META_MODE, META_GRAVITY, META_MINI, META_SPEED, META_DEAD = range(8)
MODES = ["cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"]
SPEEDS = ["0.5x", "1x", "2x", "3x", "4x"]
VALIDATED = ("sim", "real", "failed")
# H = round(1.0 * tps): the practice log's "alive" cap (s6), and how much of a
# run that died the trainers drop before the death (s4).
SURVIVE_SECONDS = 1.0
DROP_SECONDS = 1.0

# The compact per-tick record (s3) and its expansion.
STATE_DIM = 40
SCAN_COLUMNS = 40
SCAN_DIM = 4 * SCAN_COLUMNS  # deadly above, deadly below, solid above, solid below per column
SCAN_NONE = 255  # nothing within 12 blocks
SCAN_NONE_VALUE = 12.5  # what SCAN_NONE expands to (blocks)
SCAN_MAX = 96  # 12 blocks in 1/8 block
TOKENS = 8
TOKEN_BYTES = 6  # float16 dx, float16 dy, uint8 type, uint8 0
TOK_DIM = TOKENS * TOKEN_BYTES
TOKEN_TYPES = 32  # one-hot width; index 0 (empty) unused, types 1..31
TOKEN_FLOATS = 2 + TOKEN_TYPES
OBS_DIM = STATE_DIM + SCAN_DIM + TOKENS * TOKEN_FLOATS  # 472
STATE_OFFSET, SCAN_OFFSET, TOK_OFFSET = 0, STATE_DIM, STATE_DIM + SCAN_DIM
# Named state slots the trainer reads (s3).
STATE_MODE0 = 0
STATE_GRAVITY = 8
STATE_SPEED0 = 10
STATE_HELD = 18  # held before this tick: must equal act[t-1]
STATE_INV_TPS = 23  # 60 / tps
TOKEN_NAMES = {1: "yellow orb", 2: "pink orb", 3: "red orb", 4: "blue orb", 5: "green orb", 6: "black orb",
               7: "green dash orb", 8: "pink dash orb", 9: "spider orb", 10: "teleport orb", 11: "yellow pad",
               12: "pink pad", 13: "red pad", 14: "blue pad", 15: "spider pad", 16: "gravity-down portal",
               17: "gravity-up portal", 18: "gravity-flip portal", 19: "cube portal", 20: "ship portal",
               21: "ball portal", 22: "ufo portal", 23: "wave portal", 24: "robot portal", 25: "spider portal",
               26: "swing portal", 27: "mini portal", 28: "normal-size portal", 29: "speed portal",
               30: "mirror portal", 31: "dual portal"}
# The arrays of one run (s3, s4): extension, dtype, values per tick.
RUN_ARRAYS = [(".state.f32", "<f4", STATE_DIM), (".scan.u8", "<u1", SCAN_DIM), (".tok.u8", "<u1", TOK_DIM),
              (".act.u8", "<u1", 1), (".meta.f32", "<f4", len(META_FIELDS))]
PRACTICE_ARRAYS = [(".p.f32", "<f4", 1), (".alive.i32", "<i4", 1)]

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


# ---- the per-tick record and its 472-float expansion (s3) ------------------------

def obs_name(j):
    """What value j of the expansion is, for messages: 'state[18]', 'scan col 3 solid below', 'token 2 type 11'."""
    j = int(j)
    if j < SCAN_OFFSET:
        return f"state[{j}]"
    if j < TOK_OFFSET:
        c, k = divmod(j - SCAN_OFFSET, 4)
        return f"scan col {c} " + ("deadly above", "deadly below", "solid above", "solid below")[k]
    s, k = divmod(j - TOK_OFFSET, TOKEN_FLOATS)
    return f"token {s} " + ("dx" if k == 0 else "dy" if k == 1 else f"type {k - 2}")


def expand_np(state, scan, tok):
    """The 472 floats every network sees (s3), from compact rows, in numpy.

    state float32 [n][40] (or [40]), scan uint8 [n][160], tok uint8 [n][48].
    [state as-is][scan: 255 -> 12.5, else v / 8][per token: dx, dy, one-hot(type, 32)
    with index 0 unused]; a slot whose type is 0 (or outside 1..31) is all zeros.
    Bit for bit what the mod's C++ computes (the golden rows check it).
    """
    state = np.asarray(state, np.float32)
    single = state.ndim == 1
    state = np.atleast_2d(state)
    scan = np.atleast_2d(np.asarray(scan, np.uint8))
    tok = np.atleast_2d(np.asarray(tok, np.uint8))
    n = len(state)
    out = np.zeros((n, OBS_DIM), np.float32)
    out[:, :STATE_DIM] = state
    s = scan.astype(np.float32) / np.float32(8.0)
    s[scan == SCAN_NONE] = np.float32(SCAN_NONE_VALUE)
    out[:, SCAN_OFFSET:TOK_OFFSET] = s
    t = np.ascontiguousarray(tok).reshape(n, TOKENS, TOKEN_BYTES)
    dxdy = np.ascontiguousarray(t[:, :, :4]).view("<f2").astype(np.float32)  # [n][8][2]
    typ = t[:, :, 4].astype(np.int64)
    valid = (typ >= 1) & (typ < TOKEN_TYPES)
    block = np.zeros((n, TOKENS, TOKEN_FLOATS), np.float32)
    block[:, :, :2] = np.where(valid[:, :, None], dxdy, np.float32(0.0))
    r, c = np.nonzero(valid)
    block[r, c, 2 + typ[r, c]] = 1.0
    out[:, TOK_OFFSET:] = block.reshape(n, TOKENS * TOKEN_FLOATS)
    return out[0] if single else out


def expand_torch(state, scan, tok):
    """expand_np in torch, for training batches: state float32 [B][40], scan uint8 [B][160],
    tok uint8 [B][48] tensors -> float32 [B][472], the same bits as expand_np."""
    import torch
    B = state.shape[0]
    s = scan.to(torch.float32) / 8.0
    s = torch.where(scan == SCAN_NONE, torch.full_like(s, SCAN_NONE_VALUE), s)
    t = tok.reshape(B, TOKENS, TOKEN_BYTES)
    dxdy = t[:, :, :4].contiguous().view(torch.float16).to(torch.float32)  # [B][8][2]
    typ = t[:, :, 4].to(torch.int64)
    valid = (typ >= 1) & (typ < TOKEN_TYPES)
    dxdy = torch.where(valid[:, :, None], dxdy, torch.zeros_like(dxdy))
    onehot = torch.zeros(B, TOKENS, TOKEN_TYPES, dtype=torch.float32)
    onehot.scatter_(2, typ.clamp(0, TOKEN_TYPES - 1)[:, :, None], 1.0)
    onehot = onehot * valid[:, :, None].to(torch.float32)
    tokens = torch.cat([dxdy, onehot], 2).reshape(B, TOKENS * TOKEN_FLOATS)
    return torch.cat([state.to(torch.float32), s, tokens], 1)


def clean_compact(state, tok):
    """Values that aren't finite (state floats, token dx/dy) set to 0, in place; returns how many.
    The expansion of the cleaned rows equals clean_rows() of the raw rows' expansion."""
    bad = finite_rows(state)
    if not tok.flags.c_contiguous:
        raise ValueError("tok must be C-contiguous (cleaned in place)")
    f = tok.reshape(len(tok), TOKENS, TOKEN_BYTES)[:, :, :4].view("<f2")  # a view: writes reach tok
    nf = ~np.isfinite(f)
    if nf.any():
        bad += int(nf.sum())
        f[nf] = 0
    return bad


class CompactRows:
    """Training rows kept compact in RAM (368 bytes a tick, not 472 floats = 1888), expanded per batch.

    rows(idx) -> numpy float32 [n][472]; batch(idx) -> a torch tensor, the same bits.
    Indexing (X[i], X[a:b], X[array]) is rows().
    """

    def __init__(self, n):
        n = int(n)
        try:
            self.state = np.zeros((n, STATE_DIM), np.float32)
            self.scan = np.zeros((n, SCAN_DIM), np.uint8)
            self.tok = np.zeros((n, TOK_DIM), np.uint8)
        except MemoryError:
            raise DataError(f"{n} ticks need {n * self.BYTES_PER_ROW / 2**30:.2f} GiB of free RAM, which isn't "
                            "there (close what's using it; OneDrive is often the biggest), or train on less "
                            "(--load-far-keep)")

    BYTES_PER_ROW = STATE_DIM * 4 + SCAN_DIM + TOK_DIM

    def __len__(self):
        return len(self.state)

    @property
    def nbytes(self):
        return self.state.nbytes + self.scan.nbytes + self.tok.nbytes

    def put(self, dest, run, rows):
        """Copies a run's rows (a slice or index array) to dest (slice or index array)."""
        self.state[dest] = run.state[rows]
        self.scan[dest] = run.scan[rows]
        self.tok[dest] = run.tok[rows]

    def clean(self, chunk=65536):
        bad = 0
        for a in range(0, len(self), chunk):
            bad += clean_compact(self.state[a:a + chunk], self.tok[a:a + chunk])
        return bad

    def rows(self, idx):
        return expand_np(self.state[idx], self.scan[idx], self.tok[idx])

    def batch(self, idx):
        import torch
        return expand_torch(torch.from_numpy(np.ascontiguousarray(self.state[idx])),
                            torch.from_numpy(np.ascontiguousarray(self.scan[idx])),
                            torch.from_numpy(np.ascontiguousarray(self.tok[idx])))

    def __getitem__(self, idx):
        return self.rows(idx)


class ObsView:
    """A run's expanded rows, made as they're indexed: run.obs[a:b], run.obs[t], run.obs[array].
    Values that aren't finite read as 0 (clean_rows), as in training."""

    def __init__(self, run):
        self.run = run

    def __len__(self):
        return self.run.ticks

    @property
    def shape(self):
        return (self.run.ticks, OBS_DIM)

    def __getitem__(self, idx):
        r = self.run
        return clean_rows(expand_np(r.state[idx], r.scan[idx], r.tok[idx]))


# ---- recorded runs and practice attempts (s4, s6) ------------------------------

class Run:
    """One recorded run (s4) or practice attempt (s6): its header and arrays.

    The arrays are read whole (np.fromfile) on first use and kept until
    close(): call it once a run has been copied out, so only one run's
    arrays sit in RAM at a time.
    """

    def __init__(self, json_path, header, practice=False):
        self.json_path = os.path.abspath(json_path)
        self.header = header
        self.practice = practice
        self.base = self.json_path[:-len(".json")]
        self.run_id = os.path.basename(self.base)
        self._arrays = {}

    def __repr__(self):
        return f"<Run {self.run_id} {self.level_name!r} {self.ticks} ticks>"

    # The header, by name.
    source = property(lambda self: self.header["source"])
    level_id = property(lambda self: int(self.header.get("level_id") or 0))
    level_name = property(lambda self: str(self.header.get("level_name") or ""))
    macro_file = property(lambda self: str(self.header.get("macro_file") or ""))
    tps = property(lambda self: float(self.header["tps"]))
    ticks = property(lambda self: int(self.header["ticks"]))
    layout_hash = property(lambda self: self.header["layout_hash"])
    completed = property(lambda self: bool(self.header.get("completed")))
    died_tick = property(lambda self: self.header.get("died_tick"))
    validated = property(lambda self: self.header.get("validated"))
    frame_offset = property(lambda self: self.header.get("frame_offset"))
    obs_dim = OBS_DIM
    # Practice branches (s6): which attempt and tick this one flipped. first_tick
    # is the tick of row 0 (not in the contract; 0 when absent).
    branch_of = property(lambda self: self.header.get("branch_of"))
    first_tick = property(lambda self: int(self.header.get("first_tick") or 0))

    @property
    def horizon(self):
        """H = round(1.0 * tps) ticks (s6)."""
        return int(round(SURVIVE_SECONDS * self.tps))

    def _arr(self, ext, dtype, width):
        arr = self._arrays.get(ext)
        if arr is None:
            shape = (self.ticks,) if width == 1 else (self.ticks, width)
            arr = np.zeros(shape, dtype) if self.ticks == 0 else \
                np.fromfile(self.base + ext, dtype=dtype).reshape(shape)
            self._arrays[ext] = arr
        return arr

    state = property(lambda self: self._arr(".state.f32", "<f4", STATE_DIM))
    scan = property(lambda self: self._arr(".scan.u8", "<u1", SCAN_DIM))
    tok = property(lambda self: self._arr(".tok.u8", "<u1", TOK_DIM))
    act = property(lambda self: self._arr(".act.u8", "<u1", 1))
    meta = property(lambda self: self._arr(".meta.f32", "<f4", len(META_FIELDS)))
    p = property(lambda self: self._arr(".p.f32", "<f4", 1))
    alive = property(lambda self: self._arr(".alive.i32", "<i4", 1))
    obs = property(lambda self: ObsView(self))

    def close(self):
        self._arrays.clear()

    def has_golden(self):
        return os.path.exists(self.base + ".golden.idx.i32")

    @property
    def validation(self):
        """The real game's verdict on this run (AI_ROOT\\validation\\<run_id>.json), or None."""
        if self.practice:
            return None
        if "_validation" not in self.__dict__:
            path = validation_path(os.path.dirname(os.path.dirname(self.json_path)), self.run_id)
            try:
                self._validation = read_json(path) if os.path.exists(path) else None
            except (OSError, ValueError, DataError):
                self._validation = None
        return self._validation

    def usable_ticks(self):
        """How many leading ticks the trainers learn from (s4).

        A human run (validation_rule): all of it when it completed in the
        copies (validated "sim" or "real"); when it died there, the ticks
        before died_tick - tps, and only if the real game (the validation
        sidecar) completed it (a simulator mismatch) or died within 1 block of
        the simulation's death x; nothing otherwise, nor from a run its
        sidecar excludes. A run without a sidecar whose header says
        validated "real" keeps the older reading (the real game agreed).
        A practice attempt (s6) that didn't complete loses its last second
        before the death (or before its end, when a limit stopped it). Nothing
        from the first tick the meta marks dead on, minus the same margin,
        ever counts.
        """
        T = self.ticks
        if T == 0:
            return 0
        margin = int(round(DROP_SECONDS * self.tps))
        if self.practice:
            end = T
            if not self.completed:
                died = self.died_tick
                end = (int(died) if died is not None else T) - margin
        else:
            rule, _ = validation_rule(self.header, self.validation)
            if rule == "whole":
                end = T
            elif rule == "before_death":
                end = int(self.died_tick) - margin
            else:
                return 0
        dead = np.flatnonzero(np.asarray(self.meta[:, META_DEAD]) >= 0.5)
        if len(dead):
            end = min(end, int(dead[0]) - margin)
        return int(min(max(end, 0), T))


# ---- the real game's verdict on a recorded run (AI_ROOT\validation\<run_id>.json) -----------

VALIDATION_FORMAT = "gbai-validation"
SAME_DEATH_BLOCKS = 1.0  # the real game "dies there too" within this many blocks of the simulation's death x


def validation_path(root, run_id):
    return os.path.join(root, "validation", f"{run_id}.json")


def write_validation(root, run_id, sidecar):
    """Writes AI_ROOT\\validation\\<run_id>.json (atomic). sidecar: {completed, died_x, percent, frame_offset,
    when} from gucci_ai_validate_real, plus whatever else is worth keeping (sim_died_x, exclude, ...)."""
    out = {"format": VALIDATION_FORMAT, "run_id": run_id}
    out.update(sidecar)
    out.setdefault("when", now_iso())
    path = validation_path(root, run_id)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    write_json_atomic(path, out)
    return path


def validation_rule(header, sidecar=None):
    """(rule, why): which ticks of a recorded human run the trainers may use.

    "whole": the simulation completed it. "before_death": it died in the
    simulation, and the real game either completed it (the simulator is wrong
    there) or died within SAME_DEATH_BLOCKS of the same x; the ticks before
    the simulation's death minus 1 s count. "none": anything else (no real
    game verdict, a verdict for another frame offset, a death elsewhere, a
    sidecar that excludes the run).
    """
    if sidecar and sidecar.get("exclude"):
        return "none", f"excluded: {sidecar['exclude']}"
    completed = bool(header.get("completed"))
    v = header.get("validated")
    if completed:
        if v in ("sim", "real"):
            return "whole", "completed in the simulation"
        return "none", f"completed but validated {v!r}"
    if header.get("died_tick") is None:
        return "none", "died in the simulation at no known tick"
    if not sidecar:
        if v == "real":
            return "before_death", "the header says the real game agreed"
        return "none", "died in the simulation, not checked in the real game"
    if sidecar.get("frame_offset") is not None and header.get("frame_offset") is not None and \
            int(sidecar["frame_offset"]) != int(header["frame_offset"]):
        return "none", (f"the real game played offset {sidecar['frame_offset']}, the run is offset "
                        f"{header['frame_offset']}")
    if sidecar.get("completed") is True:
        return "before_death", "the real game completed it (a simulator mismatch)"
    sim_x, real_x = header.get("died_x"), sidecar.get("died_x")
    if sidecar.get("completed") is False and _is_num(sim_x) and _is_num(real_x):
        if abs(float(real_x) - float(sim_x)) <= SAME_DEATH_BLOCKS * BLOCK:
            return "before_death", "the real game dies at the same place"
        return "none", f"the real game dies at x {float(real_x):.0f}, the simulation at x {float(sim_x):.0f}"
    return "none", "no usable real-game result"


def check_run_header(h, practice=False):
    """What's wrong with a run's header (s4, s6), as a list of strings (empty: fine)."""
    errors = []
    if not isinstance(h, dict):
        return ["the header is not a JSON object"]
    if h.get("format") != "gbai-run":
        errors.append(f"format is {h.get('format')!r}, not 'gbai-run'")
    if h.get("version") != RUN_VERSION:
        errors.append(f"version is {h.get('version')!r}, not {RUN_VERSION} (contract v2; record it again)")
    if h.get("source") not in ("human", "ai"):
        errors.append(f"source is {h.get('source')!r}, not 'human' or 'ai'")
    if practice and h.get("source") != "ai":
        errors.append("a practice attempt's source must be 'ai'")
    if not _is_int(h.get("level_id")) or h["level_id"] < 0:
        errors.append("level_id is not a non-negative integer")
    for key in ("level_name", "macro_file", "macro_format", "notes"):
        if key in h and h[key] is not None and not isinstance(h[key], str):
            errors.append(f"{key} is not a string")
    if not _is_num(h.get("tps")) or not h["tps"] > 0:
        errors.append("tps is not a positive number")
    if not _is_int(h.get("ticks")) or h["ticks"] < 0:
        errors.append("ticks is not a non-negative integer")
    if not isinstance(h.get("layout_hash"), str) or not HASH_RE.match(h["layout_hash"]):
        errors.append(f"layout_hash {h.get('layout_hash')!r} is not 16 hex characters")
    if not isinstance(h.get("completed"), bool):
        errors.append("completed is not true or false")
    died = h.get("died_tick")
    # died_tick counts from the level's start (contract s1), like first_tick (a practice branch's
    # log starts at first_tick, so its rows end at first_tick + ticks).
    first0 = h.get("first_tick") or 0
    end = h["ticks"] + (first0 if _is_int(first0) else 0) if _is_int(h.get("ticks")) else None
    if died is not None and (not _is_int(died) or died < 0 or (end is not None and died > end)):
        errors.append(f"died_tick {died!r} is not null or a tick of the run")
    for key in ("died_x", "level_length_x"):
        if h.get(key) is not None and not _is_num(h[key]):
            errors.append(f"{key} is not null or a number")
    if not practice:
        if h.get("validated") not in VALIDATED:
            errors.append(f"validated is {h.get('validated')!r}, not one of {', '.join(VALIDATED)}")
        if not _is_int(h.get("frame_offset")):
            errors.append("frame_offset is not an integer")
    elif h.get("validated") is not None and h["validated"] not in VALIDATED:
        errors.append(f"validated is {h.get('validated')!r}, not one of {', '.join(VALIDATED)}")
    tried = h.get("offsets_tried")
    if tried is not None:
        if not isinstance(tried, dict) or not all(isinstance(v, dict) for v in tried.values()):
            errors.append("offsets_tried is not {\"offset\": {...}}")
        else:
            for k in tried:
                try:
                    int(k)
                except ValueError:
                    errors.append(f"offsets_tried key {k!r} is not an integer")
    branch = h.get("branch_of")
    if branch is not None:
        if not isinstance(branch, dict) or not isinstance(branch.get("attempt"), (str, int)) \
                or isinstance(branch.get("attempt"), bool) or not _is_int(branch.get("tick")) or branch["tick"] < 0:
            errors.append("branch_of is not {\"attempt\": name, \"tick\": t}")
    forced = h.get("forced")
    if forced is not None and (not isinstance(forced, list) or not all(
            isinstance(f, list) and len(f) == 2 and _is_int(f[0]) and f[1] in (0, 1, True, False) for f in forced)):
        errors.append("forced is not [[tick, held], ...]")
    first = h.get("first_tick", 0)
    if first is not None and (not _is_int(first) or first < 0):
        errors.append("first_tick is not a non-negative integer")
    return errors


def _expected_files(base, ticks, practice):
    arrays = RUN_ARRAYS + (PRACTICE_ARRAYS if practice else [])
    return [(base + ext, ticks * width * np.dtype(dtype).itemsize) for ext, dtype, width in arrays]


def _row(path, dtype, width, t):
    """Row t of a [T][width] array file, read alone."""
    size = np.dtype(dtype).itemsize
    return np.fromfile(path, dtype=dtype, count=width, offset=int(t) * width * size)


def check_golden(run):
    """The recorder's golden rows (s3) against expand_np of the same ticks: a list of errors.

    .golden.idx.i32 [G] ticks, .golden.f32 [G][472] the mod's expansion of them.
    They must match bit for bit (a -0.0 vs 0.0 or a NaN payload counts). Reads
    only the G rows it needs.
    """
    base = run.base
    errors = []
    try:
        isize = os.path.getsize(base + ".golden.idx.i32")
        fsize = os.path.getsize(base + ".golden.f32")
    except OSError as e:
        return [f"golden file missing: {os.path.basename(e.filename or '')}"]
    if isize % 4:
        return [f"golden.idx.i32 is {isize} bytes, not a whole number of int32"]
    G = isize // 4
    if fsize != G * OBS_DIM * 4:
        return [f"golden.f32 is {fsize} bytes, not {G * OBS_DIM * 4} ({G} rows of {OBS_DIM})"]
    if G == 0:
        return [] if run.ticks == 0 else ["the golden files hold no rows"]
    idx = np.fromfile(base + ".golden.idx.i32", "<i4")
    want = np.fromfile(base + ".golden.f32", "<f4").reshape(G, OBS_DIM)
    bad_idx = idx[(idx < 0) | (idx >= run.ticks)]
    if len(bad_idx):
        return [f"golden.idx.i32 names ticks outside the run: {bad_idx[:5].tolist()}"]
    for g, t in enumerate(idx):
        got = expand_np(_row(base + ".state.f32", "<f4", STATE_DIM, t), _row(base + ".scan.u8", "<u1", SCAN_DIM, t),
                        _row(base + ".tok.u8", "<u1", TOK_DIM, t))
        diff = np.flatnonzero(got.view(np.uint32) != want[g].view(np.uint32))
        if len(diff):
            j = int(diff[0])
            errors.append(f"golden row {g} (tick {int(t)}) differs from Python's expansion at {len(diff)} values, "
                          f"first {obs_name(j)} (index {j}): the mod has {want[g, j]!r}, Python {got[j]!r}")
            if len(errors) >= 3:
                break
    return errors


def load_run(json_path, practice=False, deep=False):
    """One run (s4) or practice attempt (s6, practice=True), checked against the contract.

    Raises DataError naming the file and every problem: header fields, each
    array's size (exactly `ticks` rows), act holding only 0 and 1, the golden
    rows (required for a recorded run, checked when a practice attempt has
    them). deep=True also reads every value (finite, modes 0..7, scan bytes,
    token slots, held-before = act[t-1], alive in 0..H).
    """
    try:
        header = read_json(json_path)
    except (OSError, ValueError) as e:
        raise DataError(f"{json_path}: can't read the header: {e}")
    errors = check_run_header(header, practice)
    if errors:
        raise DataError(f"{json_path}: " + "; ".join(errors))
    run = Run(json_path, header, practice)
    for path, size in _expected_files(run.base, run.ticks, practice):
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
        if not practice or run.has_golden():
            errors += check_golden(run)
        if deep and not errors:
            errors += _deep_check(run, practice)
    if errors:
        raise DataError(f"{json_path}: " + "; ".join(errors))
    return run


def _deep_check(run, practice):
    errors = []
    state = run.state
    if not np.isfinite(state).all():
        errors.append(f"state holds {int((~np.isfinite(state)).sum())} values that aren't finite")
    scan = run.scan
    bad = (scan > SCAN_MAX) & (scan != SCAN_NONE)
    if bad.any():
        errors.append(f"scan holds {int(bad.sum())} bytes outside 0..{SCAN_MAX} and {SCAN_NONE}")
    t = run.tok.reshape(run.ticks, TOKENS, TOKEN_BYTES)
    typ = t[:, :, 4]
    if (typ >= TOKEN_TYPES).any():
        errors.append(f"a token type is {int(typ.max())}; types are 0..{TOKEN_TYPES - 1}")
    if (t[:, :, 5] != 0).any():
        errors.append("a token's 6th byte is not 0")
    if (t[typ == 0] != 0).any():
        errors.append("an empty token slot (type 0) holds bytes that aren't 0")
    f = np.ascontiguousarray(t[:, :, :4]).view("<f2")
    if not np.isfinite(f).all():
        errors.append("a token dx/dy isn't finite")
    held = state[:, STATE_HELD]
    want = before(run.act).astype(np.float32)
    if run.first_tick > 0 and len(want):
        want[0] = held[0]  # the tick before a branch log's first row is in its parent's log
    off = np.flatnonzero(held != want)
    if len(off):
        errors.append(f"state[{STATE_HELD}] (held before this tick) differs from act[t-1] on {len(off)} ticks, first "
                      f"tick {int(off[0])} (contract s1: rec[t] is what the pilot sees before step t)")
    meta = run.meta
    if not np.isfinite(meta).all():
        errors.append("meta holds values that aren't finite")
    modes = meta[:, META_MODE]
    if ((modes < 0) | (modes > len(MODES) - 1) | (modes != np.round(modes))).any():
        errors.append("meta mode is outside 0..7")
    speeds = meta[:, META_SPEED]
    if ((speeds < 0) | (speeds > len(SPEEDS) - 1)).any():
        errors.append("meta speed is outside 0..4")
    if practice:
        alive = run.alive
        if (alive < 0).any() or (alive > run.horizon).any():
            errors.append(f"alive is outside 0..{run.horizon} (H = round(1.0 * tps))")
        p = run.p
        if not (np.isfinite(p).all() and (p >= 0).all() and (p <= 1).all()):
            errors.append("p (P(hold)) is outside 0..1 or not finite")
    run.close()
    return errors


def list_runs(root, source="human", deep=False):
    """Every recorded run in <root>\\data (s4) that passes the checks, by run_id.

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
    """Every practice attempt under <root>\\practice\\<level_id>\\ (s6). Returns (attempts, problems)."""
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
    """The runs recorded with one layout hash, and the hash.

    A network only fits the encoder it was trained for (the mod refuses
    weights whose layout hash differs from its own), so one training run
    uses one layout. want: the hash to use (gucci_ai_layout gives the mod's
    current one). Without it, every run must share one hash.
    """
    if not runs:
        raise DataError("no runs to train on")
    groups = {}
    for r in runs:
        groups.setdefault(r.layout_hash, []).append(r)
    if want:
        if want not in groups:
            raise DataError(f"no run was recorded with layout {want}; found " +
                            ", ".join(f"{h} ({len(g)} runs)" for h, g in sorted(groups.items())))
        chosen = want
    elif len(groups) > 1:
        lines = []
        for h, g in sorted(groups.items(), key=lambda kv: -max(os.path.getmtime(r.json_path) for r in kv[1])):
            newest = max(os.path.getmtime(r.json_path) for r in g)
            lines.append(f"{h}: {len(g)} runs, newest {time.strftime('%Y-%m-%d %H:%M', time.localtime(newest))}")
        raise DataError("the runs were recorded with different layouts (" + "; ".join(lines) +
                        "). Pass --layout-hash with the mod's current one (gucci_ai_layout) and "
                        "record the others again.")
    else:
        chosen = next(iter(groups))
    return groups[chosen], chosen


# ---- level tapes (s5) ----------------------------------------------------------

class Tape:
    """A level as columns (s5): uint8 [C][K], read whole on first use."""

    def __init__(self, json_path, header):
        self.json_path = os.path.abspath(json_path)
        self.header = header
        self.base = self.json_path[:-len(".json")]
        self._data = None

    level_id = property(lambda self: int(self.header["level_id"]))
    columns = property(lambda self: int(self.header["columns"]))
    channels = property(lambda self: int(self.header["channels"]))
    bands = property(lambda self: int(self.header["bands"]))
    x0 = property(lambda self: float(self.header["x0"]))
    y0 = property(lambda self: float(self.header["y0"]))
    column_width = property(lambda self: float(self.header["column_width"]))
    band_height = property(lambda self: float(self.header["band_height"]))
    channel_layout = property(lambda self: self.header["channel_layout"])
    # Not in the contract (see CONTRACT_ISSUES.md): the tape encoder's own hash.
    layout_hash = property(lambda self: self.header.get("layout_hash"))

    @property
    def data(self):
        if self._data is None:
            shape = (self.columns, self.channels)
            self._data = np.zeros(shape, np.uint8) if self.columns == 0 else \
                np.fromfile(self.base + ".tape.u8", dtype=np.uint8).reshape(shape)
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
    if h.get("version") != TAPE_VERSION:
        errors.append(f"version is {h.get('version')!r}, not {TAPE_VERSION}")
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
    for key, low in (("columns", 0), ("channels", 1), ("bands", 1)):
        if not _is_int(h.get(key)) or h[key] < low:
            errors.append(f"{key} is not a valid count")
    layout = h.get("channel_layout")
    if not isinstance(layout, list) or not layout:
        errors.append("channel_layout is not a non-empty list")
    elif _is_int(h.get("channels")) and all(isinstance(e, dict) and "offset" in e and "size" in e for e in layout):
        # The contract leaves the entries' form open; when they say where they sit, check that.
        errors += _check_layout_list(layout, h["channels"], "channel_layout")
    lh = h.get("layout_hash")
    if lh is not None and (not isinstance(lh, str) or not HASH_RE.match(lh)):
        errors.append(f"layout_hash {lh!r} is not 16 hex characters")
    return errors


def load_tape(root, level_id, deep=False):
    """The tape of one level (s5), checked; None when it hasn't been recorded."""
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
        want = tape.columns * tape.channels
        try:
            have = os.path.getsize(tape.base + ".tape.u8")
            if have != want:
                errors.append(f"{os.path.basename(tape.base)}.tape.u8 is {have} bytes, not {want}")
        except OSError:
            errors.append(f"{os.path.basename(tape.base)}.tape.u8 is missing")
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


# ---- writing synthetic data (tests and golden files only) ---------------------------

def _refuse_real_root(folder):
    real = os.path.normcase(os.path.abspath(default_ai_root()))
    here = os.path.normcase(os.path.abspath(folder))
    if here == real or here.startswith(real + os.sep):
        raise RuntimeError(f"refusing to write into the mod's own AI_ROOT ({real}): only the mod writes "
                           "data\\, tapes\\ and practice\\")


def write_run(folder, run_id, header, state, scan, tok, act, meta, p=None, alive=None, golden_idx=None):
    """Writes one run (s4) or practice attempt (s6, with p and alive) in the contract's format,
    plus golden rows for the ticks in golden_idx (expanded here, as the recorder would).

    For tests and synthetic data only: the trainer never writes into AI_ROOT's
    data\\, tapes\\ or practice\\, and this refuses the mod's real AI_ROOT.
    """
    _refuse_real_root(folder)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, run_id)
    np.ascontiguousarray(state, "<f4").tofile(base + ".state.f32")
    np.ascontiguousarray(scan, "<u1").tofile(base + ".scan.u8")
    np.ascontiguousarray(tok, "<u1").tofile(base + ".tok.u8")
    np.ascontiguousarray(act, "<u1").tofile(base + ".act.u8")
    np.ascontiguousarray(meta, "<f4").tofile(base + ".meta.f32")
    if p is not None:
        np.ascontiguousarray(p, "<f4").tofile(base + ".p.f32")
    if alive is not None:
        np.ascontiguousarray(alive, "<i4").tofile(base + ".alive.i32")
    if golden_idx is not None:
        gi = np.asarray(golden_idx, np.int64)
        rows = expand_np(np.asarray(state)[gi], np.asarray(scan)[gi], np.asarray(tok)[gi]) if len(gi) else \
            np.zeros((0, OBS_DIM), np.float32)
        np.ascontiguousarray(gi, "<i4").tofile(base + ".golden.idx.i32")
        np.ascontiguousarray(rows, "<f4").tofile(base + ".golden.f32")
    write_json_atomic(base + ".json", header)
    return base + ".json"


def golden_ticks(T, G=32):
    """G ticks spread over a run of T (the recorder's choice is its own; this is the tests')."""
    if T <= 0:
        return np.zeros(0, np.int64)
    return np.unique(np.linspace(0, T - 1, min(G, T)).round().astype(np.int64))


def write_tape(folder, header, data):
    """Writes one tape (s5); tests only, like write_run()."""
    _refuse_real_root(folder)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, str(int(header["level_id"])))
    np.ascontiguousarray(data, "<u1").tofile(base + ".tape.u8")
    write_json_atomic(base + ".json", header)
    return base + ".json"


# ---- held-out exam levels: never trained on -------------------------------------

# Nigel's exams (his 10-tier ladder, the 2.0 bar levels and the bonus ones):
# tools/ai/heldout.json, a copy of the list he gave with ids (2026-11-03).
# Every result he sees on them must come from a model that never learned
# them, so no trainer ever takes a tick from them and they never sit in a
# fold's validation set either. They are matched by level_id ONLY (main
# levels by their number, 1-22): a name match only warns, since names collide
# ("Sakupen End" is not Sakupen Circles, "Clubstep" is not Silent Clubstep).
HELDOUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "heldout.json")


def read_heldout(path=None):
    """{level_id: entry} from heldout.json ({"levels": [{tier, name, level_id, ...}]}). A missing or broken
    file is an error, never an empty list: training with no exams held out would be silently wrong."""
    path = path or HELDOUT_PATH
    try:
        data = read_json(path)
    except (OSError, ValueError) as e:
        raise DataError(f"the held-out exam list {path} can't be read ({e}); training needs it") from e
    levels = data.get("levels") if isinstance(data, dict) else None
    if not isinstance(levels, list) or not levels:
        raise DataError(f"{path}: expected {{\"levels\": [...]}} with at least one level")
    out = {}
    for e in levels:
        if not isinstance(e, dict) or not _is_int(e.get("level_id")) or int(e["level_id"]) <= 0:
            raise DataError(f"{path}: every level needs a positive level_id ({e!r})")
        out[int(e["level_id"])] = e
    return out


# Kept for the play step and old callers: {level_id: name} of every exam level.
SHOWCASE_IDS = {lid: str(e.get("name") or "") for lid, e in read_heldout().items()}     if os.path.exists(HELDOUT_PATH) else {}
SHOWCASE_LEVELS = sorted(set(SHOWCASE_IDS.values()))


def name_key(text):
    """Lowercase letters and digits only: 'Heliopolis      ' -> 'heliopolis'."""
    return re.sub(r"[^a-z0-9]", "", str(text or "").lower())


class Showcase:
    """The held-out exam levels (by id). extra_ids add to them; a manifest adds its entries marked
    is_bar_level or held_out (by their ids). Names (extra_names, a level's name, a macro's file name) only
    ever give a warning (warning()), never an exclusion."""

    def __init__(self, extra_ids=(), extra_names=(), manifest=None, heldout=None):
        self.entries = read_heldout(heldout)
        self.ids = {lid: str(e.get("name") or "an exam level") for lid, e in self.entries.items()}
        for i in extra_ids:
            self.ids.setdefault(int(i), "listed")
        self.warn_keys = {name_key(e.get("name")): str(e.get("name")) for e in self.entries.values()}
        for n in extra_names:
            if name_key(n):
                self.warn_keys[name_key(n)] = str(n)
        if manifest:
            for e in read_manifest(manifest):
                if (e["is_bar_level"] or e["raw"].get("held_out")) and e["level_id"]:
                    self.ids.setdefault(e["level_id"], e["level_name"] or "a held-out level in the manifest")
        self.warn_keys.pop("", None)

    def reason(self, level_id=0, level_name="", macro_file=""):
        """Why this level is held out (by its id), or None. Names never exclude."""
        if level_id and int(level_id) in self.ids:
            e = self.entries.get(int(level_id)) or {}
            tier = f", tier {e['tier']} exam" if "tier" in e else ""
            return f"held-out level id {int(level_id)} ({self.ids[int(level_id)]}{tier})"
        return None

    def warning(self, level_id=0, level_name="", macro_file=""):
        """A note when a level that ISN'T held out has an exam level's name in its name or macro file name
        (it may be a copy of the level, or just a similar name), or None. Never an exclusion."""
        if self.reason(level_id):
            return None
        stem = os.path.splitext(os.path.basename(str(macro_file or "").replace("\\", "/")))[0]
        for text, what in ((level_name, "level name"), (stem, "macro file")):
            k = name_key(text)
            for key, name in self.warn_keys.items():
                if key in k:
                    how = "is" if key == k else "contains"
                    return (f"level id {int(level_id or 0)}: its {what} {text!r} {how} the exam level name {name!r}, "
                            "but the id isn't held out, so it is trained on (check it isn't the exam level)")
        return None

    def run_reason(self, run):
        return self.reason(run.level_id, run.level_name, run.macro_file)

    def run_warning(self, run):
        return self.warning(run.level_id, run.level_name, run.macro_file)


HeldOut = Showcase


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


DEFAULT_FOLDS = 8


def parse_fold(text):
    """--fold: 'k/K' -> (k, K); 'k' -> (k, 8); 'all' (or 'none') -> None, train on every level.

    0 <= k < K, K >= 2. The default everywhere is 0/8: eight level-grouped
    folds, each level held out exactly once over k = 0..7.
    """
    t = str(text if text is not None else "").strip().lower()
    if t in ("all", "none"):
        return None
    m = re.match(r"^(\d+)\s*(?:/\s*(\d+))?$", t)
    if not m:
        raise ValueError(f"--fold is k/K (like 0/8), k (of 8) or all, not {text!r}")
    k, n = int(m.group(1)), int(m.group(2) or DEFAULT_FOLDS)
    if n < 2 or not 0 <= k < n:
        raise ValueError(f"--fold {text}: needs K >= 2 and 0 <= k < K")
    return k, n


def fold_order(keys, seed=0):
    """The levels in the order they're dealt into folds: sorted by a hash of (seed, level), so a
    level's place doesn't depend on Python's hash seed or the order the files were listed in."""
    return sorted(set(keys), key=lambda k: hashlib.sha256(f"{seed}:{k}".encode()).hexdigest())


def split_levels(keys, fold=None, seed=0):
    """(train keys, validation keys, folds used), split by level, never by tick.

    fold=(k, K): the levels, in fold_order, dealt into K folds; fold k
    validates and the rest train. With fewer than K levels it becomes
    leave-one-out (K = the number of levels), and k must still name one.
    fold=None trains on every level and validates on none.
    """
    keys = sorted(set(keys))
    if fold is None:
        return keys, [], None
    k, n = fold
    if len(keys) < 2:
        raise DataError(f"--fold {k}/{n}: {len(keys)} level(s) to split; it takes 2 or more (or --fold all)")
    n = min(n, len(keys))
    if k >= n:
        raise DataError(f"--fold {k}/{fold[1]}: only {len(keys)} levels, so only folds 0..{n - 1}")
    val = [key for i, key in enumerate(fold_order(keys, seed)) if i % n == k]
    train = [key for key in keys if key not in set(val)]
    return train, sorted(val), (k, n)


def make_split(runs, showcase, fold=(0, DEFAULT_FOLDS), seed=0, usable=None):
    """Leaves the held-out exam levels out (by id), then splits the rest by level.

    usable (optional): run -> bool. A run it rejects has nothing to learn
    from (0 ticks, a failed validation, or a death inside its first second)
    and is left out BEFORE the split, so a level with nothing in it can't
    take a validation or fold slot.

    Returns {"train": runs, "val": runs, "excluded": [(run, reason)],
    "unusable": runs, "train_levels": keys, "val_levels": keys, "fold": (k, K)
    or None, "name_warnings": [(run, text)]}. A held-out run is never in train
    or val, whatever the fold. A run whose name only looks like an exam
    level's is kept and listed in name_warnings.
    """
    pool, excluded, unusable, warnings = [], [], [], []
    for r in runs:
        why = showcase.run_reason(r)
        if why:
            excluded.append((r, why))
            continue
        warn = showcase.run_warning(r) if hasattr(showcase, "run_warning") else None
        if warn:
            warnings.append((r, warn))
        if usable is not None and not usable(r):
            unusable.append(r)
        else:
            pool.append(r)
    train_keys, val_keys, used = split_levels([level_key(r) for r in pool], fold, seed)
    tk, vk = set(train_keys), set(val_keys)
    return {"train": [r for r in pool if level_key(r) in tk], "val": [r for r in pool if level_key(r) in vk],
            "excluded": excluded, "unusable": unusable, "name_warnings": warnings,
            "train_levels": train_keys, "val_levels": val_keys,
            "fold": used, "fold_asked": fold}


def split_record(split, model, layout_hash, seed=0):
    """split.json's contents: what trained, what validated, what was left out and why.
    evaluate.py reads it to tell held-out levels from trained ones."""
    def runs(rs):
        return [{"run_id": r.run_id, "level_id": r.level_id, "level_name": r.level_name} for r in rs]
    used, asked = split.get("fold"), split.get("fold_asked")
    return {"format": "gbai-split", "version": 2, "model": model, "created": now_iso(), "layout_hash": layout_hash,
            "fold": f"{used[0]}/{used[1]}" if used else "all",
            "fold_asked": f"{asked[0]}/{asked[1]}" if asked else "all", "seed": seed,
            "train_levels": split["train_levels"], "val_levels": split["val_levels"],
            "train_runs": runs(split["train"]), "val_runs": runs(split["val"]),
            "showcase_excluded": [dict(runs([r])[0], reason=why) for r, why in split["excluded"]],
            "name_warnings": [dict(runs([r])[0], warning=w) for r, w in split.get("name_warnings", [])],
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
    """Expanded rows as a float32 copy, values that aren't finite set to 0.

    Training does that to its rows (finite_rows), so every prediction made
    for validation or a report goes through here too. Without it the same
    broken value (an inf state float or token dx) read as P = 0.5, a hold, in
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


# ---- GBNN weights (s7) -----------------------------------------------------------

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
    """Raises DataError unless the network is one the mod can run (s7)."""
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
    """The file's bytes, exactly as contract s7 lays them out (little-endian)."""
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
        flag = "  HELD OUT (exam level, never trained on)" if show.run_reason(r0) else ""
        print(f"  {r0.level_name or '?'} ({key}){flag}")
        for r in rs:
            state = "completed" if r.completed else f"died at tick {r.died_tick}"
            print(f"    {r.run_id}: {r.source}, {r.ticks} ticks at {r.tps:g} tps ({r.usable_ticks()} usable), "
                  f"{state}, validated {r.validated}, frame offset {r.frame_offset}, layout {r.layout_hash}")
    hashes = sorted({r.layout_hash for r in runs})
    if len(hashes) > 1:
        print(f"  NOTE: {len(hashes)} different layouts: {', '.join(hashes)}")
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
        print(f"NOTE: {len(hashes)} layouts in use: {', '.join(hashes)} (training uses one)")
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

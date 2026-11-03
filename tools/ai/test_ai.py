"""Self-contained tests for the AI trainer package (tools/ai/), contract v2.

Writes a SYNTHETIC dataset in the exact contract format (compact per-tick
records, golden rows, headers) into a temporary AI_ROOT and runs the real
trainers on it. Nothing here touches the mod's own AI_ROOT, the game, or
anything outside a temp folder (and tools/ai/golden, which it only reads).

The toy: a cube runs right at GD speed (240 tps, two levels at 360). Gaps come
every 6-20 blocks, each marked by a token (type 11, a yellow pad) at its near
edge, whose float16 dx is exact; the "human" holds exactly while the next gap
is 2.5 to 1.0 blocks ahead (about 35 ticks of hold, then 100-450 ticks
released: sparse presses like a real macro). Speed portals change the speed
inside a level, so a hold's length in ticks varies. The scan shows the pits
(deadly below) and a smooth ceiling that differs per level, the state the
button held before the tick and the ticks since the last press/release, so
the pilot has to find the window; the trivial baseline (repeat the last
tick's button) never presses at all.

    python tools/ai/test_ai.py            # all tests
    python tools/ai/test_ai.py -k pilot   # the ones with "pilot" in their name
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import gbai  # noqa: E402

TPS = 240
BLOCKS_PER_SEC = 10.386  # GD's 1x speed in blocks per second
HOLD_FROM, HOLD_TO = 2.5, 1.0  # hold while the next gap is this far ahead (blocks)
GAP_WIDTH = 1.5  # blocks
GAP_TOKEN = 11  # yellow pad
TOY_HASH = "70a1b2c3d4e5f607"
TAPE_LAYOUT = [{"name": "gap", "offset": 0, "size": 1}, {"name": "deadly_band", "offset": 1, "size": 2},
               {"name": "solid_band", "offset": 3, "size": 2}, {"name": "portal_ship", "offset": 5, "size": 1}]
TAPE_K = 6
# (level_id, name, speed, tps, ticks): twelve ordinary levels and two
# held-out exam ones (Bloodbath and The Nightmare, by their ids).
TOY_LEVELS = [(1001, "Toy One", 1.0, 240, 7000), (1002, "Toy Two", 0.9, 240, 7000),
              (1003, "Toy Three", 1.1, 240, 7000), (1004, "Toy Four", 1.0, 360, 9000),
              (1005, "Toy Five", 1.25, 240, 6000), (1006, "Toy Six", 0.8, 240, 7000),
              (1007, "Toy Seven", 1.0, 240, 7000), (1008, "Toy Eight", 0.85, 240, 7000),
              (1009, "Toy Nine", 1.15, 240, 7000), (1010, "Toy Ten", 1.0, 360, 9000),
              (1011, "Toy Eleven", 0.95, 240, 7000), (1012, "Toy Twelve", 1.2, 240, 6000),
              (10565740, "BB copy", 1.0, 240, 6000), (13519, "the  NIGHTMARE", 1.0, 240, 6000)]
SHOWCASE_TOY_IDS = {10565740, 13519}


SPEED_STEPS = (0.8, 1.0, 1.25)  # speed portals: the multiplier changes every 900-2400 ticks


def scan_bytes(blocks):
    """Distances (blocks) as scan bytes: 1/8 block, 255 past 12 blocks."""
    v = np.round(np.asarray(blocks, np.float64) * 8)
    return np.where(v > gbai.SCAN_MAX, gbai.SCAN_NONE, np.clip(v, 0, gbai.SCAN_MAX)).astype(np.uint8)


def toy_arrays(seed, ticks, tps=TPS, speed=1.0, died_tick=None, ship=True):
    """(state, scan, tok, act, meta, gaps_x) for one synthetic run, per the contract's tick convention:
    rec[t] is what the pilot sees before step t, act[t] the button during it."""
    rng = np.random.default_rng(seed)
    mult = np.empty(ticks)
    t0 = 0
    while t0 < ticks:
        n = int(rng.integers(900, 2400))
        mult[t0:t0 + n] = speed * SPEED_STEPS[int(rng.integers(len(SPEED_STEPS)))]
        t0 += n
    v = BLOCKS_PER_SEC * mult / tps
    x = 2.0 + np.concatenate([[0.0], np.cumsum(v[:-1])])  # blocks, before each tick
    gaps, p = [], 8.0 + rng.uniform(0, 6)
    while p < x[-1] + 30:
        gaps.append(p)
        p += rng.uniform(6, 20)
    gaps = np.array(gaps)
    nxt = np.minimum(np.searchsorted(gaps, x, side="right"), len(gaps) - 1)
    g = (gaps[nxt] - x).astype(np.float16).astype(np.float64)  # the next gap ahead, as its token's dx says
    act = ((g >= HOLD_TO) & (g < HOLD_FROM)).astype(np.uint8)
    h = gbai.before(act).astype(np.uint8)
    t = np.arange(ticks)
    press = (act == 1) & (h == 0)
    release = (act == 0) & (h == 1)
    # Ticks since the last press/release BEFORE this tick (rec[t] is before step t).
    lp = np.maximum.accumulate(np.where(press, t, -100000))
    lr = np.maximum.accumulate(np.where(release, t, -100000))
    lp_before = np.concatenate([[-100000], lp[:-1]])
    lr_before = np.concatenate([[-100000], lr[:-1]])
    speed_index = np.searchsorted(np.array(SPEED_STEPS) * speed, mult - 1e-9).clip(0, 4)
    mode = np.zeros(ticks, np.int64)
    if ship:
        mode[int(ticks * 0.6):int(ticks * 0.7)] = 1  # a ship part, for the reader's labels
    state = np.zeros((ticks, gbai.STATE_DIM), np.float32)
    state[t, mode] = 1.0
    state[t, gbai.STATE_SPEED0 + speed_index] = 1.0
    state[:, 15] = 0.05 * np.sin(x * 1.7 + seed)
    state[:, 16] = 1.0  # on ground
    state[:, gbai.STATE_HELD] = h
    state[:, 19] = np.log1p(np.minimum(t - lp_before, 100000)) / 5
    state[:, 20] = np.log1p(np.minimum(t - lr_before, 100000)) / 5
    state[:, 21] = 0.0
    state[:, 22] = 15.0
    state[:, gbai.STATE_INV_TPS] = np.float32(60.0 / tps)
    # The scan: pits (deadly 1 block below, nothing solid) where a gap overlaps the column, the
    # ground at the feet elsewhere, and a smooth ceiling that differs per level.
    cols = -1.0 + 0.5 * np.arange(gbai.SCAN_COLUMNS)
    a = x[:, None] + cols[None, :]
    j = np.searchsorted(gaps, a + 0.5, side="left") - 1
    pit = (j >= 0) & (gaps[np.maximum(j, 0)] + GAP_WIDTH > a)
    freq, phase, depth = rng.uniform(0.05, 0.4), rng.uniform(0, 2 * np.pi), rng.uniform(3, 9)
    ceiling = depth + 2.5 * np.sin(a * freq + phase)
    scan = np.full((ticks, gbai.SCAN_COLUMNS, 4), gbai.SCAN_NONE, np.uint8)
    scan[:, :, 1] = np.where(pit, scan_bytes(1.0), gbai.SCAN_NONE)
    scan[:, :, 2] = scan_bytes(ceiling)
    scan[:, :, 3] = np.where(pit, gbai.SCAN_NONE, 0)
    scan = scan.reshape(ticks, gbai.SCAN_DIM)
    # Tokens: the gaps' pads with dx in [-1, 20], nearest first.
    tok = np.zeros((ticks, gbai.TOKENS, gbai.TOKEN_BYTES), np.uint8)
    first = np.searchsorted(gaps, x - 1.0, side="left")
    for s in range(gbai.TOKENS):
        k = first + s
        ok = k < len(gaps)
        dx = np.where(ok, gaps[np.minimum(k, len(gaps) - 1)] - x, 99.0)
        ok &= dx <= 20.0
        pair = np.zeros((ticks, 2), "<f2")
        pair[:, 0] = dx
        tok[ok, s, :4] = pair[ok].view(np.uint8).reshape(-1, 4)
        tok[ok, s, 4] = GAP_TOKEN
    tok = tok.reshape(ticks, gbai.TOK_DIM)
    meta = np.zeros((ticks, len(gbai.META_FIELDS)), np.float32)
    meta[:, gbai.META_X] = x * gbai.BLOCK
    meta[:, gbai.META_Y] = 105.0 + gbai.BLOCK * act
    meta[:, gbai.META_YVEL] = state[:, 15]
    meta[:, gbai.META_MODE] = mode
    meta[:, gbai.META_SPEED] = speed_index
    if died_tick is not None:
        meta[died_tick + 1:, gbai.META_DEAD] = 1
    return state, scan, tok, act, meta, gaps


def run_header(level_id, name, ticks, tps, meta, completed=True, died_tick=None, macro_file="", source="human",
               layout_hash=TOY_HASH, validated=None, **extra):
    h = {"format": "gbai-run", "version": 2, "source": source, "level_id": level_id, "level_name": name,
         "macro_file": macro_file, "macro_format": "slc3", "tps": tps, "ticks": ticks, "layout_hash": layout_hash,
         "frame_offset": 0, "offsets_tried": {"0": {"completed": completed,
                                                    "died_x": None if completed else 1234.5}},
         "completed": completed, "died_tick": died_tick,
         "died_x": None if died_tick is None else float(meta[died_tick, gbai.META_X]),
         "level_length_x": float(meta[-1, gbai.META_X]) + 300.0 if len(meta) else 0.0,
         "validated": validated or ("sim" if completed else "real"), "notes": "synthetic test data"}
    if source == "ai":
        h.pop("validated")
        h.pop("frame_offset")
        h.pop("offsets_tried")
    h.update(extra)
    return h


def write_arrays(folder, run_id, header, arrays, p=None, alive=None, golden=True):
    state, scan, tok, act, meta = arrays[:5]
    return gbai.write_run(folder, run_id, header, state, scan, tok, act, meta, p=p, alive=alive,
                          golden_idx=gbai.golden_ticks(len(state)) if golden else None)


def write_toy_run(root, level_id, name, speed=1.0, tps=TPS, ticks=7000, seed=0, died_tick=None, stem=None,
                  macro_file=None, validated=None):
    arrays = toy_arrays(seed, ticks, tps, speed, died_tick)
    stem = stem or name.strip().replace(" ", "_")
    run_id = f"{level_id}__{stem}"
    header = run_header(level_id, name, ticks, tps, arrays[4], died_tick is None, died_tick,
                        macro_file or f"C:/macros/{stem}.slc", validated=validated)
    write_arrays(os.path.join(root, "data"), run_id, header, arrays)
    return run_id, arrays[5], float(arrays[4][-1, gbai.META_X]) / gbai.BLOCK + 2.0


def write_toy_tape(root, level_id, gaps, length_blocks):
    """A tape for a toy level: 0.5-block columns, a 'gap' channel where a gap starts, a ship portal mark."""
    cols = int(np.ceil(length_blocks * 2)) + 4
    data = np.zeros((cols, TAPE_K), np.uint8)
    for gx in gaps:
        c = int(np.floor(gx * gbai.BLOCK / 15.0))
        if 0 <= c < cols:
            data[c, 0] = 1
            data[c, 1] = 1
    data[:, 3] = 1  # the ground
    data[cols * 6 // 10, 5] = 1
    header = {"format": "gbai-tape", "version": 1, "level_id": level_id, "column_width": 15.0, "x0": 0.0,
              "columns": cols, "bands": 2, "channels": TAPE_K, "channel_layout": TAPE_LAYOUT, "y0": 105.0,
              "band_height": 15.0}
    gbai.write_tape(os.path.join(root, "tapes"), header, data)


def make_toy_root(root, tapes=False):
    """Every toy level (one run each, two on Toy One, Toy Two's second run dies, proven in the real
    game; Toy Three's second run failed validation). Returns {run_id: level}."""
    made = {}
    for i, (lid, name, speed, tps, ticks) in enumerate(TOY_LEVELS):
        run_id, gaps, length = write_toy_run(root, lid, name, speed, tps, ticks, seed=100 + i)
        made[run_id] = lid
        if tapes:
            write_toy_tape(root, lid, gaps, length)
    rid, _, _ = write_toy_run(root, 1001, "Toy One", 1.0, TPS, 5000, seed=7, stem="Toy_One_take2")
    made[rid] = 1001
    rid, _, _ = write_toy_run(root, 1002, "Toy Two", 0.9, TPS, 5000, seed=8, died_tick=4200, stem="Toy_Two_died")
    made[rid] = 1002
    rid, _, _ = write_toy_run(root, 1003, "Toy Three", 1.1, TPS, 3000, seed=9, died_tick=2000, stem="Toy_Three_bad",
                              validated="failed")
    made[rid] = 1003
    return made


class TempRoot:
    """A temporary AI_ROOT, removed afterwards."""

    def __enter__(self):
        self.path = tempfile.mkdtemp(prefix="gbai_test_")
        return self.path

    def __exit__(self, *exc):
        import gc
        gc.collect()
        shutil.rmtree(self.path, ignore_errors=True)


def write_toy_practice(root, level_id, name, seed, ticks=3000, tps=TPS):
    """Practice attempts on one toy level (contract s6):
    a0 skips its 3rd press and dies 40 ticks later; b0 branches from a0 at that
    tick, presses, and completes (its log starts at tick 0, a copy of a0's
    before it); a1 completes; a2 stops at a limit without dying (its last H
    rows have no known outcome). Returns (H, the branch tick, row counts)."""
    state, scan, tok, act, meta, _ = toy_arrays(seed, ticks, tps, ship=False)
    H = int(round(tps))
    presses = np.flatnonzero((act == 1) & (gbai.before(act) == 0))
    tf = int(presses[2])
    folder = os.path.join(root, "practice", str(level_id))
    t = np.arange(ticks)

    def put(attempt, n, a, alive, completed, died_tick, **extra):
        header = run_header(level_id, name, n, tps, meta[:n], completed, died_tick, "", "ai", **extra)
        p = np.where(a[:n] == 1, 0.9, 0.1).astype(np.float32)
        st = state[:n].copy()
        st[:, gbai.STATE_HELD] = gbai.before(a[:n])  # what this attempt held, not the human's
        gbai.write_run(folder, attempt, header, st, scan[:n], tok[:n], a[:n], meta[:n], p=p,
                       alive=alive.astype(np.int32))

    died = tf + 40
    a0 = act.copy()
    a0[tf:] = 0
    put("a0", died + 1, a0, np.minimum(H, died - t[:died + 1]), False, died)
    put("b0", ticks, act, np.full(ticks, H), True, None, branch_of={"attempt": "a0", "tick": tf}, first_tick=0,
        forced=[[tf, 1]])
    put("a1", ticks, act, np.full(ticks, H), True, None)
    stop = 2000
    put("a2", stop, act, np.minimum(H, stop - t[:stop]), False, None)
    rows = {"a0": died + 1, "b0": ticks - max(0, tf - H), "a1": ticks, "a2": stop - H}
    return H, tf, rows


class FakeGame:
    """Stands in for the game in orchestrate.py's tests: the contract's tools (s8), answered from here.

    gucci_ai_record writes a toy run (and tape) for the macro into the temp
    AI_ROOT; gucci_ai_play answers with a made-up result. Leave tools out to
    test what happens on a build without them.
    """

    ALL = {"gucci_ai_record", "gucci_ai_status", "gucci_ai_reload", "gucci_ai_play", "gucci_ai_layout",
           "gucci_open_level", "gucci_quit_level", "gucci_get_state", "gucci_ai_find_level",
           "gucci_ai_validate_real", "gucci_ai_cancel"}
    TAPE_HASH = "00aa11bb22cc33dd"

    def __init__(self, root, tools=None, sim=None, real=None, find=None, crash=None):
        """sim: {macro stem: tick it dies at in the simulation}; real: {stem: validate_real's result, or
        "hang"}; find: {name: candidates}; crash: {stem: times gucci_ai_record takes the game down}."""
        self.root = root
        self.tools = set(self.ALL if tools is None else tools)
        self.jobs = {}
        self.level = None
        self.calls = []
        self.sim, self.real, self.find = sim or {}, real or {}, find or {}
        self.crash = dict(crash or {})
        self.up = True
        self.recoveries = 0

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def alive(self):
        return self.up

    def recover(self):
        self.up = True
        self.recoveries += 1

    def _job(self, result, state="done"):
        job = f"job{len(self.jobs)}"
        self.jobs[job] = {"polls": 0, "result": result, "state": state}
        return {"job": job}

    def call(self, tool, **args):
        self.calls.append((tool, args))
        if not self.up:
            raise ConnectionRefusedError("the game isn't running")
        if tool not in self.tools:
            raise RuntimeError(f"{tool}: no tool called {tool}")
        if tool == "gucci_ai_find_level":
            return self._job({"query": args["name"], "candidates": self.find.get(args["name"], [])})
        if tool == "gucci_ai_validate_real":
            stem = os.path.splitext(os.path.basename(args["macro_path"]))[0]
            res = self.real.get(stem, {"completed": True, "died_x": None, "percent": 100.0})
            if res == "hang":
                return self._job(None, state="running")
            return self._job(dict(res, frame_offset=args.get("frame_offset", 0)))
        if tool == "gucci_ai_cancel":
            return {"cancelled": True}
        if tool == "gucci_ai_layout":
            return {"layout_hash": TOY_HASH, "obs_dim": gbai.OBS_DIM, "tape_layout": TAPE_LAYOUT,
                    "tape_layout_hash": self.TAPE_HASH}
        if tool == "gucci_ai_record":
            lid = int(args["level_id"])
            stem = os.path.splitext(os.path.basename(args["macro_path"]))[0]
            if self.crash.get(stem):
                self.crash[stem] -= 1
                self.up = False
                raise ConnectionResetError("the game crashed")
            died = self.sim.get(stem)
            import orchestrate  # the mod's run_id: <level>__<stem>__<format>
            fmt = {".ybot": "ybot2", ".slc": "slc3", ".tcm": "tcm", ".gdr": "gdr"}.get(
                os.path.splitext(args["macro_path"])[1].lower(), "gbr6")
            run_id, gaps, length = write_toy_run(self.root, lid, f"Level {stem}", ticks=4000, seed=lid % 997,
                                                 stem=f"{orchestrate.sanitize(stem)}__{fmt}",
                                                 macro_file=args["macro_path"], died_tick=died,
                                                 validated="failed" if died else None)
            write_toy_tape(self.root, lid, gaps, length)
            header = gbai.read_json(os.path.join(self.root, "data", run_id + ".json"))
            return self._job(dict(header, run_id=run_id))
        if tool == "gucci_ai_status":
            j = self.jobs[args["job"]]
            j["polls"] += 1
            if j["state"] != "done":
                return {"state": j["state"], "progress": "playing in the real game: frame 9000, x 31000"}
            return {"state": "done", "progress": 1.0, "result": j["result"]}
        if tool == "gucci_ai_reload":
            return {"models": sorted(n[:-5] for n in os.listdir(os.path.join(self.root, "weights")))}
        if tool == "gucci_open_level":
            self.level = args.get("main") or args.get("id")
            return {"opening": str(self.level)}
        if tool == "gucci_get_state":
            return {"in_level": self.level is not None, "level": {"id": self.level} if self.level else None}
        if tool == "gucci_quit_level":
            self.level = None
            return {"leaving": True}
        if tool == "gucci_ai_play":
            if set(args) - {"max_attempts", "max_seconds", "save_macro", "log_practice"}:
                raise RuntimeError(f"gucci_ai_play: arguments the contract doesn't have: {sorted(args)}")
            return self._job({"completed": False, "best_percent": 42.5, "attempts": 3, "ticks_simulated": 9000,
                              "deaths": [{"tick": 900, "x": 2700.0, "percent": 12.0, "cause": "spike from above",
                                          "mistake_tick": 893, "mistake_was": "release",
                                          "fix": "pressed 3 ticks earlier"}]})
        raise RuntimeError(f"FakeGame has no {tool}")


def quiet(*_):
    pass


def random_compact(rng, n):
    """Random compact rows that follow the contract (incl. empty slots, scan 255, -0.0)."""
    state = rng.normal(0, 3, (n, gbai.STATE_DIM)).astype(np.float32)
    state[0, 0] = np.float32(-0.0)
    scan = rng.integers(0, gbai.SCAN_MAX + 1, (n, gbai.SCAN_DIM)).astype(np.uint8)
    scan[rng.random((n, gbai.SCAN_DIM)) < 0.3] = gbai.SCAN_NONE
    tok = np.zeros((n, gbai.TOKENS, gbai.TOKEN_BYTES), np.uint8)
    used = rng.random((n, gbai.TOKENS)) < 0.6
    dxdy = np.stack([rng.uniform(-1, 20, (n, gbai.TOKENS)), rng.uniform(-10, 10, (n, gbai.TOKENS))], 2)
    tok[:, :, :4] = dxdy.astype("<f2").view(np.uint8).reshape(n, gbai.TOKENS, 4)
    tok[:, :, 4] = rng.integers(1, gbai.TOKEN_TYPES, (n, gbai.TOKENS))
    tok[~used] = 0
    geo = rng.uniform(0, gbai.GEO_REACH, (n, gbai.GEO_DIM)).astype(np.float32)
    geo[rng.random((n, gbai.GEO_DIM)) < 0.3] = np.float32(gbai.GEO_NONE)
    geo[:, list(gbai.GEO_FLAGS)] = rng.integers(0, 2, (n, 2))
    geo[0, 0] = np.float32(-0.0)
    return state, scan, tok.reshape(n, gbai.TOK_DIM), geo


class ExpansionTests(unittest.TestCase):
    """The 494-float expansion (contract s3, layout v3): numpy, torch and the hand-written rule agree bit for bit."""

    def test_layout_by_hand(self):
        state = np.arange(gbai.STATE_DIM, dtype=np.float32)
        scan = np.full(gbai.SCAN_DIM, gbai.SCAN_NONE, np.uint8)
        scan[:4] = [0, 1, 95, 96]
        tok = np.zeros((gbai.TOKENS, gbai.TOKEN_BYTES), np.uint8)
        tok[0, :4] = np.array([2.5, -1.0], "<f2").view(np.uint8)
        tok[0, 4] = 11
        tok[1, :4] = np.array([19.75, 0.5], "<f2").view(np.uint8)
        tok[1, 4] = 31
        geo = np.arange(gbai.GEO_DIM, dtype=np.float32) / 4
        x = gbai.expand_np(state, scan, tok.reshape(-1), geo)
        self.assertEqual(x.shape, (494,))
        self.assertEqual(gbai.OBS_DIM, 494)
        self.assertEqual(gbai.GEO_OFFSET, 472)
        np.testing.assert_array_equal(x[:40], state)
        np.testing.assert_array_equal(x[40:44], [0.0, 0.125, 11.875, 12.0])
        self.assertTrue((x[44:200] == 12.5).all())
        self.assertEqual(list(x[200:202]), [2.5, -1.0])
        self.assertEqual(int(np.flatnonzero(x[202:234])[0]), 11)
        self.assertEqual(x[202:234].sum(), 1.0)
        self.assertEqual(list(x[234:236]), [19.75, 0.5])
        self.assertEqual(int(np.flatnonzero(x[236:268])[0]), 31)
        self.assertTrue((x[268:472] == 0).all())  # six empty slots
        np.testing.assert_array_equal(x[472:], geo)
        self.assertEqual(gbai.obs_name(472), "geo deadly ray -2")
        self.assertEqual(gbai.obs_name(472 + 13), "geo solid ray +0")
        self.assertEqual(gbai.obs_name(472 + 19), "geo floor_dx")
        self.assertEqual(gbai.obs_name(18), "state[18]")
        self.assertEqual(gbai.obs_name(41), "scan col 0 deadly below")
        self.assertEqual(gbai.obs_name(200 + 34 + 2 + 31), "token 1 type 31")

    def test_numpy_and_torch_match_bit_for_bit(self):
        import torch
        rng = np.random.default_rng(5)
        state, scan, tok, geo = random_compact(rng, 3000)
        a = gbai.expand_np(state, scan, tok, geo)
        b = gbai.expand_torch(torch.from_numpy(state), torch.from_numpy(scan), torch.from_numpy(tok),
                              torch.from_numpy(geo)).numpy()
        self.assertTrue(np.array_equal(a.view(np.uint32), b.view(np.uint32)))
        self.assertEqual(a[0, 0].view(np.uint32), np.float32(-0.0).view(np.uint32))
        rows = gbai.CompactRows(3000)
        rows.state[:], rows.scan[:], rows.tok[:], rows.geo[:] = state, scan, tok, geo
        idx = np.array([5, 17, 2999])
        self.assertTrue(np.array_equal(rows.batch(idx).numpy().view(np.uint32), a[idx].view(np.uint32)))
        self.assertTrue(np.array_equal(rows[idx], a[idx]))
        self.assertEqual(rows.BYTES_PER_ROW, 456)

    def test_empty_or_bad_slot_is_all_zeros(self):
        """A slot with type 0 (or a type outside 1..31) expands to zeros, whatever its other bytes hold."""
        import torch
        state = np.zeros((1, gbai.STATE_DIM), np.float32)
        scan = np.zeros((1, gbai.SCAN_DIM), np.uint8)
        tok = np.zeros((1, gbai.TOKENS, gbai.TOKEN_BYTES), np.uint8)
        tok[0, 0, :4] = np.array([3.0, 4.0], "<f2").view(np.uint8)  # type 0
        tok[0, 1, :4] = np.array([np.inf, 1.0], "<f2").view(np.uint8)
        tok[0, 1, 4] = 40  # no such type
        tok = tok.reshape(1, -1)
        geo = np.zeros((1, gbai.GEO_DIM), np.float32)
        a = gbai.expand_np(state, scan, tok, geo)
        b = gbai.expand_torch(torch.from_numpy(state), torch.from_numpy(scan), torch.from_numpy(tok),
                              torch.from_numpy(geo)).numpy()
        self.assertTrue((a[0, 200:] == 0).all())
        self.assertTrue(np.array_equal(a, b))

    def test_cleaning_compact_rows_equals_cleaning_expanded_rows(self):
        rng = np.random.default_rng(6)
        state, scan, tok, geo = random_compact(rng, 50)
        state[3, 7], state[4, 15] = np.inf, np.nan
        geo[6, 2] = np.nan
        t = tok.reshape(50, gbai.TOKENS, gbai.TOKEN_BYTES)
        t[5, 0, :4] = np.array([np.inf, -np.inf], "<f2").view(np.uint8)
        t[5, 0, 4] = 3
        want = gbai.clean_rows(gbai.expand_np(state, scan, tok, geo))
        rows = gbai.CompactRows(50)
        rows.state[:], rows.scan[:], rows.tok[:], rows.geo[:] = state, scan, tok, geo
        self.assertEqual(rows.clean(), 5)
        self.assertTrue(np.array_equal(rows[np.arange(50)], want))


class GoldenSetTests(unittest.TestCase):
    """tools/ai/golden: what stage C's C++ test loads."""

    def test_committed_golden_set_checks_out(self):
        import export
        self.assertTrue(os.path.exists(os.path.join(export.GOLDEN_DIR, "golden.json")),
                        "make it with: python tools/ai/export.py golden")
        self.assertEqual(export.check_golden_dir(), [])
        doc = gbai.read_json(os.path.join(export.GOLDEN_DIR, "golden.json"))
        self.assertEqual(doc["layout_hash"], export.GOLDEN_HASH)
        for kind in ("pilot", "critic"):
            dec = np.fromfile(os.path.join(export.GOLDEN_DIR, f"{kind}.dec.u8"), np.uint8)
            self.assertTrue(0 < dec.sum() < len(dec), kind)  # both decisions appear
        net = gbai.read_gbnn(os.path.join(export.GOLDEN_DIR, "critic.gbnn"))
        self.assertEqual(net.layers[-1].activation, gbai.ACT_NONE)  # v2: logits
        self.assertEqual(net.out_names, ["alive_hold", "alive_release"])
        self.assertLess(doc["models"]["reader"]["columns"][1], doc["models"]["reader"]["receptive_field"])

    def test_make_golden_is_repeatable(self):
        import export
        with TempRoot() as d:
            export.make_golden(os.path.join(d, "a"), log=quiet)
            export.make_golden(os.path.join(d, "b"), log=quiet)
            self.assertEqual(export.check_golden_dir(os.path.join(d, "a")), [])
            for name in os.listdir(os.path.join(d, "a")):
                if name != "golden.json":
                    with open(os.path.join(d, "a", name), "rb") as f, open(os.path.join(d, "b", name), "rb") as g:
                        self.assertEqual(f.read(), g.read(), name)


class GBNNTests(unittest.TestCase):
    def random_net(self, kind, rng):
        if kind == gbai.KIND_MLP:
            dims, layers = [5, 7, 3], []
            for i, (a, b) in enumerate(zip(dims[:-1], dims[1:])):
                layers.append(gbai.Layer(gbai.LAYER_DENSE, a, b, 1, 1, gbai.ACT_RELU if i == 0 else gbai.ACT_SIGMOID,
                                         rng.standard_normal((b, a, 1)), rng.standard_normal(b)))
            names = ["a", "b", "c"]
        else:
            layers = [gbai.Layer(gbai.LAYER_CONV1D, 5, 4, 3, 2, gbai.ACT_TANH, rng.standard_normal((4, 5, 3)),
                                 rng.standard_normal(4)),
                      gbai.Layer(gbai.LAYER_CONV1D, 4, 3, 1, 1, gbai.ACT_NONE, rng.standard_normal((3, 4, 1)),
                                 rng.standard_normal(3))]
            names = ["y_rel", "x", "hold_frac"]
        mean = rng.standard_normal(5).astype(np.float32)
        std = rng.uniform(0.1, 3, 5).astype(np.float32)
        # Awkward float32 values must survive bit for bit: -0.0, a subnormal, a huge one.
        mean[0], mean[1], mean[2] = -0.0, np.float32(1e-45), np.float32(3e38)
        layers[0].weight.reshape(-1)[0] = np.float32(-0.0)
        return gbai.GBNN(kind, "0123456789abcdef", mean, std, layers, names)

    def test_round_trip_is_bit_exact(self):
        rng = np.random.default_rng(1)
        for kind in (gbai.KIND_MLP, gbai.KIND_CONV):
            net = self.random_net(kind, rng)
            data = gbai.gbnn_bytes(net)
            back = gbai.parse_gbnn(data)
            self.assertEqual(gbai.gbnn_bytes(back), data)
            self.assertEqual(back.kind, kind)
            self.assertEqual(back.layout_hash, "0123456789abcdef")
            self.assertEqual(back.out_names, net.out_names)
            self.assertTrue(np.array_equal(back.mean.view(np.uint32), net.mean.view(np.uint32)))
            self.assertTrue(np.array_equal(back.std.view(np.uint32), net.std.view(np.uint32)))
            for a, b in zip(back.layers, net.layers):
                self.assertEqual((a.type, a.n_in, a.n_out, a.kernel, a.dilation, a.activation),
                                 (b.type, b.n_in, b.n_out, b.kernel, b.dilation, b.activation))
                self.assertTrue(np.array_equal(a.weight.view(np.uint32), b.weight.view(np.uint32)))
                self.assertTrue(np.array_equal(a.bias.view(np.uint32), b.bias.view(np.uint32)))
            with TempRoot() as d:
                path = os.path.join(d, "net.gbnn")
                gbai.write_gbnn(path, net)
                with open(path, "rb") as f:
                    self.assertEqual(f.read(), data)
                self.assertEqual(gbai.gbnn_bytes(gbai.read_gbnn(path)), data)

    def test_byte_layout_follows_contract(self):
        import struct
        L = gbai.Layer(gbai.LAYER_DENSE, 2, 1, 1, 1, gbai.ACT_NONE, [[[0.5], [-2.0]]], [0.25])
        data = gbai.gbnn_bytes(gbai.GBNN(gbai.KIND_MLP, "fedcba9876543210", [1.0, 2.0], [3.0, 4.0], [L],
                                         ["hold_logit"]))
        self.assertEqual(data[:4], b"GBNN")
        self.assertEqual(struct.unpack_from("<II", data, 4), (1, 0))  # version, kind
        self.assertEqual(data[12:28], b"fedcba9876543210")
        self.assertEqual(struct.unpack_from("<I", data, 28), (2,))  # n_in
        self.assertEqual(struct.unpack_from("<4f", data, 32), (1.0, 2.0, 3.0, 4.0))  # mean, std
        self.assertEqual(struct.unpack_from("<I", data, 48), (1,))  # n_layers
        self.assertEqual(struct.unpack_from("<6I", data, 52), (0, 2, 1, 1, 1, 0))
        self.assertEqual(struct.unpack_from("<3f", data, 76), (0.5, -2.0, 0.25))  # weight[1][2][1], bias
        self.assertEqual(struct.unpack_from("<II", data, 88), (1, 10))  # n_out_names, len
        self.assertEqual(data[96:], b"hold_logit")

    def test_rejects_broken_files(self):
        rng = np.random.default_rng(2)
        net = self.random_net(gbai.KIND_MLP, rng)
        data = gbai.gbnn_bytes(net)
        for bad in (data[:-1], data + b"\0", b"GBNX" + data[4:], data[:4] + b"\2\0\0\0" + data[8:], data[:40]):
            with self.assertRaises(gbai.DataError):
                gbai.parse_gbnn(bad)
        broken = self.random_net(gbai.KIND_CONV, rng)
        broken.layers[0] = gbai.Layer(gbai.LAYER_CONV1D, 5, 4, 2, 1, gbai.ACT_NONE, np.zeros((4, 5, 2)), np.zeros(4))
        with self.assertRaises(gbai.DataError):  # an even kernel has no center
            gbai.gbnn_bytes(broken)
        chain = self.random_net(gbai.KIND_MLP, rng)
        chain.layers = chain.layers[1:]  # the first layer takes 7 inputs, the file has 5
        with self.assertRaises(gbai.DataError):
            gbai.gbnn_bytes(chain)
        zero_std = self.random_net(gbai.KIND_MLP, rng)
        zero_std.std[0] = 0.0
        with self.assertRaises(gbai.DataError):
            gbai.gbnn_bytes(zero_std)


class ForwardTests(unittest.TestCase):
    """export.forward (numpy, what the mod runs) against PyTorch, for every model kind."""

    def setUp(self):
        import torch
        torch.manual_seed(3)
        self.rng = np.random.default_rng(3)

    def check(self, model, inputs):
        import export
        model.set_norm(self.rng.standard_normal(model.n_in), self.rng.uniform(0.3, 4.0, model.n_in))
        net = gbai.parse_gbnn(gbai.gbnn_bytes(export.to_gbnn(model, "0123456789abcdef")))  # what a file holds
        self.assertLessEqual(export.compare(net, model, inputs), 1e-5)
        return net

    def test_pilot(self):
        import models
        x = (self.rng.standard_normal((64, gbai.OBS_DIM)) * 5).astype(np.float32)
        net = self.check(models.Pilot(gbai.OBS_DIM, (256, 256)), x)
        self.assertEqual((net.out_names, net.layers[-1].activation), (["hold_logit"], gbai.ACT_NONE))

    def test_critic(self):
        import models
        x = (self.rng.standard_normal((64, gbai.OBS_DIM)) * 5).astype(np.float32)
        net = self.check(models.Critic(gbai.OBS_DIM, (256, 256)), x)
        self.assertEqual(net.layers[-1].activation, gbai.ACT_NONE)  # contract v2: logits
        self.assertEqual(net.out_names, ["alive_hold", "alive_release"])

    def test_reader(self):
        import models
        model = models.Reader(TAPE_K, 16, 5, (1, 2, 4, 8, 16, 32))
        seqs = [self.rng.integers(0, 3, (c, TAPE_K)).astype(np.float32) for c in (1, 2, 7, 40, 300)]
        net = self.check(model, seqs)
        self.assertEqual(net.out_names, models.READER_OUTPUTS)

    def test_decision_rule_is_logit_above_zero(self):
        import evaluate
        logits = np.array([-1.0, -0.0, 0.0, 1e-30, 2.0], np.float32)
        self.assertEqual(evaluate.decide(logits).tolist(), [False, False, False, True, True])  # ties release

    def test_broken_values_read_as_zero_everywhere(self):
        """A value that isn't finite trains as 0. Validation and the report must read it the same way."""
        import evaluate
        import export
        import models
        import train_pilot
        model = models.Pilot(gbai.OBS_DIM, (16,))
        model.set_norm(self.rng.standard_normal(gbai.OBS_DIM), self.rng.uniform(0.5, 2.0, gbai.OBS_DIM))
        net = gbai.parse_gbnn(gbai.gbnn_bytes(export.to_gbnn(model, TOY_HASH)))
        with TempRoot() as root:
            arrays = list(toy_arrays(4, 300))
            arrays[0][0, 3], arrays[0][1, 15] = np.inf, np.nan
            t = arrays[2].reshape(300, gbai.TOKENS, gbai.TOKEN_BYTES)
            t[2, 0, :2] = np.array([np.inf], "<f2").view(np.uint8)
            t[2, 0, 4] = GAP_TOKEN
            write_arrays(os.path.join(root, "data"), "9__broken", run_header(9, "B", 300, TPS, arrays[4]), arrays)
            run = gbai.list_runs(root)[0][0]
            clean = gbai.clean_rows(gbai.expand_np(arrays[0][:6], arrays[1][:6], arrays[2][:6], gbai.default_geo(6)))
            want = export.forward(net, clean)[:, 0]
            np.testing.assert_allclose(evaluate.pilot_logits(net, run.obs[:6]), want, atol=1e-5)
            np.testing.assert_allclose(train_pilot.predict_logits(model, run.obs[:6]), want, atol=1e-5)
            X, *_ = train_pilot.assemble([run], 8)
            np.testing.assert_allclose(export.forward(net, X[np.arange(6)])[:, 0], want, atol=1e-6)
            run.close()

    def test_conv_is_the_contracts_loop(self):
        """The contract's formula, written as plain loops, gives what export.forward gives."""
        import export
        rng = self.rng
        layers = [gbai.Layer(gbai.LAYER_CONV1D, 2, 3, 3, 2, gbai.ACT_RELU, rng.standard_normal((3, 2, 3)),
                             rng.standard_normal(3)),
                  gbai.Layer(gbai.LAYER_CONV1D, 3, 2, 5, 1, gbai.ACT_NONE, rng.standard_normal((2, 3, 5)),
                             rng.standard_normal(2))]
        net = gbai.GBNN(gbai.KIND_CONV, "0123456789abcdef", [0.5, -1.0], [2.0, 0.5], layers, ["a", "b"])
        x = rng.standard_normal((9, 2))
        h = ((x - net.mean) / net.std).T
        for L in net.layers:
            C, K = h.shape[1], L.kernel
            out = np.zeros((L.n_out, C))
            for o in range(L.n_out):
                for t in range(C):
                    acc = float(L.bias[o])
                    for i in range(L.n_in):
                        for k in range(K):
                            j = t + (k - (K - 1) // 2) * L.dilation
                            if 0 <= j < C:
                                acc += float(L.weight[o, i, k]) * h[i, j]
                    out[o, t] = acc
            h = np.maximum(out, 0) if L.activation == gbai.ACT_RELU else out
        self.assertLess(float(np.max(np.abs(export.forward(net, x) - h.T))), 1e-9)


class DataTests(unittest.TestCase):
    def test_run_checks(self):
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy One", ticks=500, seed=1)
            write_toy_run(root, 1002, "Toy Two", ticks=500, seed=2)
            runs, problems = gbai.list_runs(root, deep=True)
            self.assertEqual((len(runs), problems), (2, []))
            base = os.path.join(root, "data", "1002__Toy_Two")
            with open(base + ".act.u8", "ab") as f:
                f.write(b"\0")  # one row too many
            runs, problems = gbai.list_runs(root)
            self.assertEqual(len(runs), 1)
            self.assertIn("act.u8 is 501 bytes, not 500", problems[0][1])
            arrays = list(toy_arrays(3, 300))
            arrays[3] = arrays[3].copy()
            arrays[3][5] = 2
            write_arrays(os.path.join(root, "data"), "1003__bad", run_header(1003, "Bad", 300, TPS, arrays[4]), arrays)
            h = run_header(1004, "Bad hash", 300, TPS, arrays[4], layout_hash="xyz")
            arrays[3] = arrays[3].clip(0, 1)
            write_arrays(os.path.join(root, "data"), "1004__badhash", h, arrays)
            h = run_header(1005, "v1", 300, TPS, arrays[4])
            h["version"] = 1
            write_arrays(os.path.join(root, "data"), "1005__v1", h, arrays)
            h = run_header(1006, "No validated", 300, TPS, arrays[4])
            del h["validated"]
            write_arrays(os.path.join(root, "data"), "1006__noval", h, arrays)
            write_arrays(os.path.join(root, "data"), "1007__nogolden", run_header(1007, "NG", 300, TPS, arrays[4]),
                         arrays, golden=False)
            runs, problems = gbai.list_runs(root)
            text = " | ".join(p[1] for p in problems)
            self.assertIn("only 0 and 1", text)
            self.assertIn("not 16 hex", text)
            self.assertIn("not 2 (contract v2", text)
            self.assertIn("validated is None", text)
            self.assertIn("golden file missing", text)

    def test_golden_rows_catch_a_different_expansion(self):
        """The recorder's golden rows must equal Python's expansion bit for bit; one flipped bit fails the run."""
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy One", ticks=400, seed=1)
            base = os.path.join(root, "data", "1001__Toy_One")
            g = np.fromfile(base + ".golden.f32", "<f4").reshape(-1, gbai.OBS_DIM)
            self.assertEqual(len(g), 32)
            g[3, 40 + 9] = np.nextafter(g[3, 40 + 9], np.float32(100))
            g.tofile(base + ".golden.f32")
            runs, problems = gbai.list_runs(root)
            self.assertEqual(runs, [])
            self.assertIn("golden row 3", problems[0][1])
            self.assertIn("scan col 2 deadly below", problems[0][1])

    def test_deep_check_wants_held_before_to_be_the_last_action(self):
        with TempRoot() as root:
            arrays = list(toy_arrays(2, 600))
            arrays[0] = arrays[0].copy()
            arrays[0][:, gbai.STATE_HELD] = arrays[3]  # off by one tick: the label leaks into what it sees
            write_arrays(os.path.join(root, "data"), "5__leak", run_header(5, "Leak", 600, TPS, arrays[4]), arrays)
            self.assertEqual(len(gbai.list_runs(root)[0]), 1)  # shallow checks can't see it
            runs, problems = gbai.list_runs(root, deep=True)
            self.assertIn("differs from act[t-1]", problems[0][1])

    def test_one_layout_per_training(self):
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy One", ticks=300, seed=1)
            arrays = toy_arrays(2, 300)
            other = "aaaaaaaaaaaaaaaa"
            write_arrays(os.path.join(root, "data"), "1002__other",
                         run_header(1002, "Other", 300, TPS, arrays[4], layout_hash=other), arrays)
            runs, _ = gbai.list_runs(root)
            with self.assertRaises(gbai.DataError) as cm:
                gbai.pick_layout(runs)
            self.assertIn("--layout-hash", str(cm.exception))
            picked, h = gbai.pick_layout(runs, other)
            self.assertEqual(([r.run_id for r in picked], h), (["1002__other"], other))

    def test_usable_ticks_follow_validated(self):
        """Contract s4: all of a completed run; from one that died, the ticks before death - tps, and
        only when the real game agreed (validated real); nothing from a failed run."""
        with TempRoot() as root:
            write_toy_run(root, 1, "done sim", ticks=5000, seed=1)
            write_toy_run(root, 2, "done real", ticks=5000, seed=2, validated="real")
            write_toy_run(root, 3, "died real", ticks=5000, seed=3, died_tick=4200)
            write_toy_run(root, 4, "died failed", ticks=5000, seed=4, died_tick=4200, validated="failed")
            write_toy_run(root, 5, "died sim", ticks=5000, seed=5, died_tick=4200, validated="sim")
            write_toy_run(root, 6, "early", ticks=5000, seed=6, died_tick=100)
            write_toy_run(root, 7, "done failed", ticks=5000, seed=7, validated="failed")
            got = {r.level_id: r.usable_ticks() for r in gbai.list_runs(root)[0]}
            self.assertEqual(got, {1: 5000, 2: 5000, 3: 4200 - 240, 4: 0, 5: 0, 6: 0, 7: 0})

    def test_practice_files(self):
        with TempRoot() as root:
            H, tf, rows = write_toy_practice(root, 1001, "Toy One", seed=11)
            attempts, problems = gbai.list_practice(root, deep=True)
            self.assertEqual((len(attempts), problems), (4, []))
            os.remove(os.path.join(root, "practice", "1001", "a1.p.f32"))
            attempts, problems = gbai.list_practice(root)
            self.assertIn("a1.p.f32 is missing", problems[0][1])

    def test_tapes(self):
        with TempRoot() as root:
            write_toy_tape(root, 1001, [10.0, 20.0], 40.0)
            tapes, problems = gbai.list_tapes(root)
            self.assertEqual((sorted(tapes), problems), ([1001], []))
            self.assertEqual(tapes[1001].data.dtype, np.uint8)
            self.assertEqual(tapes[1001].bands, 2)
            with open(os.path.join(root, "tapes", "1001.tape.u8"), "ab") as f:
                f.write(b"\0")
            self.assertIn("tape.u8 is", gbai.list_tapes(root)[1][0][1])

    def test_heldout_exams_by_id_only(self):
        show = gbai.Showcase()
        listed = gbai.read_json(gbai.HELDOUT_PATH)["levels"]
        self.assertEqual(len(listed), 26)
        for e in listed:  # every exam level is held out by its id, whatever it's called
            self.assertIsNotNone(show.reason(e["level_id"], "a renamed copy", "x.slc"), e)
        for lid in (1, 5, 11, 14, 18, 20, 13519, 10565740, 100856004, 146615106):
            self.assertIsNotNone(show.reason(lid))
        # Names never exclude: same-looking names on other ids are trained on, with a warning at most.
        for lid, name, macro in [(0, "Theory of Everything 2", ""), (55, "LIMBO", ""), (59, "Silent Clubstep", ""),
                                 (60, "", "Clubstep.gdr"), (61, "Sakupen Circles", ""), (62, "Sakupen Hell", ""),
                                 (63, "Sakupen End", ""), (12, "Theory of Everything", "toe.slc"),
                                 (80517643, "Maze of Justice", "")]:
            self.assertIsNone(show.reason(lid, name, macro), (lid, name, macro))
        self.assertIsNotNone(show.warning(59, "Silent Clubstep"))
        self.assertIsNotNone(show.warning(63, "Sakupen End"))
        self.assertIsNotNone(show.warning(60, "", "C:/m/Clubstep.gdr"))
        self.assertIsNone(show.warning(61, "Sakupen Circles"))  # "Sakupen End" isn't in it
        self.assertIsNone(show.warning(62, "Sakupen Hell"))
        self.assertIsNone(show.warning(14, "Clubstep"))  # held out: an exclusion, not a warning
        show2 = gbai.Showcase(extra_ids=[777], extra_names=["Some Level"])
        self.assertIsNotNone(show2.reason(777))
        self.assertIsNone(show2.reason(778, "some level"))
        self.assertIsNotNone(show2.warning(778, "some level"))
        with TempRoot() as d:
            path = os.path.join(d, "manifest.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"entries": [{"path": "x.slc", "level_id": 777, "level_name": "Some Bar Level",
                                        "is_bar_level": True},
                                       {"path": "y.slc", "level_id": 778, "level_name": "Exam", "held_out": "tier 4"},
                                       {"path": "z.slc", "level_id": 0, "level_name": "No Id", "is_bar_level": True}]},
                          f)
            show = gbai.Showcase(manifest=path)
            self.assertIsNotNone(show.reason(777, "renamed", ""))
            self.assertIsNotNone(show.reason(778))
            self.assertIsNone(show.reason(0, "some bar level", ""))
            for bad in ("{}", '{"levels": []}', '{"levels": [{"name": "x"}]}'):
                with open(os.path.join(d, "h.json"), "w") as f:
                    f.write(bad)
                with self.assertRaises(gbai.DataError):
                    gbai.Showcase(heldout=os.path.join(d, "h.json"))
            with self.assertRaises(gbai.DataError):
                gbai.Showcase(heldout=os.path.join(d, "missing.json"))

    def test_heldout_split_keeps_lookalike_names(self):
        with TempRoot() as root:
            for lid, name in [(1001, "Sakupen Circles"), (1002, "Silent Clubstep"), (1003, "Toy"),
                              (1004, "Sakupen Hell"), (100856004, "renamed copy"), (14, ""), (1, "Stereo Madness")]:
                write_toy_run(root, lid, name, ticks=600, seed=lid % 97, stem=f"r{lid}")
            runs, problems = gbai.list_runs(root)
            self.assertEqual(problems, [])
            for k in range(4):
                split = gbai.make_split(runs, gbai.Showcase(), (k, 4), usable=lambda r: r.usable_ticks() > 0)
                used = {r.level_id for r in split["train"] + split["val"]}
                self.assertEqual(used, {1001, 1002, 1003, 1004})
                self.assertEqual({r.level_id for r, _ in split["excluded"]}, {100856004, 14, 1})
                self.assertEqual({r.level_id for r, _ in split["name_warnings"]}, {1002})
                rec = gbai.split_record(split, "pilot", TOY_HASH)
                self.assertEqual([w["level_id"] for w in rec["name_warnings"]], [1002])

    def test_levels_with_nothing_usable_take_no_split_slot(self):
        with TempRoot() as root:
            for lid in (1001, 1002, 1003):
                write_toy_run(root, lid, f"Toy {lid}", ticks=600, seed=lid)
            write_toy_run(root, 1004, "Toy Early", ticks=600, seed=4, died_tick=100, stem="early")
            write_toy_run(root, 1006, "Toy Failed", ticks=600, seed=6, validated="failed", stem="failed")
            arrays = toy_arrays(5, 10)
            write_arrays(os.path.join(root, "data"), "1005__empty", run_header(1005, "Empty", 0, TPS, arrays[4][:0]),
                         [a[:0] for a in arrays])
            runs, problems = gbai.list_runs(root)
            self.assertEqual(problems, [])
            for k in range(3):
                split = gbai.make_split(runs, gbai.Showcase(), (k, 3), usable=lambda r: r.usable_ticks() > 0)
                self.assertEqual(sorted(r.run_id for r in split["unusable"]),
                                 ["1004__early", "1005__empty", "1006__failed"])
                self.assertEqual((len(split["train"]), len(split["val"])), (2, 1))
                rec = gbai.split_record(split, "pilot", TOY_HASH)
                self.assertEqual(rec["fold"], f"{k}/3")

    def test_eight_level_folds(self):
        keys = [f"id:{i}" for i in range(1, 21)]
        seen = []
        for k in range(8):
            train, val, used = gbai.split_levels(keys, fold=(k, 8))
            self.assertEqual(used, (k, 8))
            self.assertFalse(set(train) & set(val))
            self.assertEqual(sorted(train + val), sorted(keys))
            self.assertIn(len(val), (2, 3))
            seen += val
        self.assertEqual(sorted(seen), sorted(keys))  # every level validates exactly once over the 8 folds
        # The deal doesn't depend on the order the levels were listed in.
        self.assertEqual(gbai.split_levels(list(reversed(keys)), (3, 8))[1], gbai.split_levels(keys, (3, 8))[1])
        self.assertEqual(gbai.split_levels(keys, None), (sorted(keys), [], None))
        # Fewer levels than folds: leave-one-out.
        train, val, used = gbai.split_levels(keys[:5], (4, 8))
        self.assertEqual((len(val), used), (1, (4, 5)))
        with self.assertRaises(gbai.DataError):
            gbai.split_levels(keys[:5], (6, 8))
        self.assertEqual(gbai.parse_fold("2/7"), (2, 7))
        self.assertEqual(gbai.parse_fold("3"), (3, 8))
        self.assertIsNone(gbai.parse_fold("all"))
        for bad in ("7/7", "1/1", "x", "8"):
            with self.assertRaises(ValueError):
                gbai.parse_fold(bad)

    def test_showcase_levels_are_out_of_every_fold(self):
        with TempRoot() as root:
            made = make_toy_root(root)
            runs, problems = gbai.list_runs(root)
            self.assertEqual(problems, [])
            showcase_runs = {rid for rid, lid in made.items() if lid in SHOWCASE_TOY_IDS}
            for k in range(8):
                split = gbai.make_split(runs, gbai.Showcase(), (k, 8), usable=lambda r: r.usable_ticks() > 0)
                used = {r.run_id for r in split["train"] + split["val"]}
                self.assertFalse(used & showcase_runs)
                self.assertEqual({r.run_id for r, _ in split["excluded"]}, showcase_runs)
                self.assertEqual([r.run_id for r in split["unusable"]], ["1003__Toy_Three_bad"])


class EvaluateTests(unittest.TestCase):
    def test_hand_counted_example(self):
        import evaluate
        act = np.array([0, 0, 0, 1, 1, 1, 0, 0, 0, 0])
        pred = np.array([0, 0, 1, 1, 1, 1, 1, 0, 0, 1])
        c = evaluate.transition_metrics(act, pred, 240)
        self.assertEqual((c["correct"], c["decisions"], c["false"]), (7, 2, 1))
        self.assertEqual(c["press"]["events"], 1)
        self.assertEqual(c["press"]["within"], {"1": 1, "2": 1, "4": 1})  # pressed 1 tick early
        self.assertEqual(c["release"]["missed"], 1)  # released 1 tick late: can't be seen, counts as missed
        base = evaluate.transition_metrics(act, gbai.before(act), 240)
        self.assertEqual((base["decisions"], base["false"], base["press"]["within"]["4"]), (0, 0, 0))
        self.assertEqual(base["correct"], 8)

    def test_early_and_flicker(self):
        import evaluate
        act = np.array([0] * 10 + [1] * 5 + [0] * 5)
        pred = act.copy()
        pred[7:10] = 1  # 3 ticks early
        c = evaluate.transition_metrics(act, pred, 240)
        self.assertEqual(c["press"]["within"], {"1": 0, "2": 0, "4": 1})
        pred = act.copy()
        pred[4] = 1  # a stray press, then the right one 1 tick early
        pred[9] = 1
        c = evaluate.transition_metrics(act, pred, 240)
        self.assertEqual(c["press"]["within"]["1"], 1)
        self.assertEqual(c["press"]["first_within"]["4"], 0)  # strictly, it pressed 6 ticks early
        self.assertEqual(c["false"], 1)
        r = evaluate.rates(evaluate.add_counts(c, c))
        self.assertEqual(r["presses"], 2)
        self.assertAlmostEqual(r["false_per_sec"], 2 / (40 / 240))


class PilotTests(unittest.TestCase):
    def test_pilot_learns_the_toy_and_beats_the_baseline(self):
        import export
        import models
        import train_pilot
        with TempRoot() as root:
            made = make_toy_root(root)
            res = train_pilot.main(["--root", root, "--fold", "1/6", "--epochs", "30", "--batch", "512",
                                    "--threads", "4"], log=quiet)
            out = os.path.join(root, "train", "pilot")
            split = gbai.read_json(os.path.join(out, "split.json"))
            trained = {r["run_id"] for r in split["train_runs"]} | {r["run_id"] for r in split["val_runs"]}
            showcase_runs = {rid for rid, lid in made.items() if lid in SHOWCASE_TOY_IDS}
            self.assertEqual({r["run_id"] for r in split["showcase_excluded"]}, showcase_runs)
            self.assertFalse(trained & showcase_runs)
            self.assertNotIn("1003__Toy_Three_bad", trained)  # failed validation: nothing from it
            self.assertFalse(set(split["train_levels"]) & set(split["val_levels"]))
            report = gbai.read_json(os.path.join(out, "eval_report.json"))
            self.assertEqual({r["group"] for r in report["runs"]}, {"validation", "showcase"})
            for g in ("validation", "showcase"):
                m, b = report["groups"][g]["model"], report["groups"][g]["baseline"]
                for side in ("press", "release"):
                    self.assertEqual(b[f"{side}_within_2"], 0.0)
                    self.assertGreaterEqual(m[f"{side}_within_2"], 0.55, (g, side, m))
                self.assertLessEqual(m["false_per_sec"], 0.3, (g, m))
                self.assertGreaterEqual(m["tick_accuracy"], 0.95)
            gbnn = os.path.join(out, "pilot.gbnn")
            ok, worst, n = export.check_golden(gbnn)
            self.assertTrue(ok, worst)
            self.assertGreaterEqual(n, 2)
            net = gbai.read_gbnn(gbnn)
            self.assertEqual((net.layout_hash, net.n_in), (TOY_HASH, gbai.OBS_DIM))
            model, ck = models.from_checkpoint(os.path.join(out, "best.pt"))
            run = [r for r in gbai.list_runs(root)[0] if r.level_id == 10565740][0]
            x = run.obs[:3000]  # real rows of a level it never saw
            run.close()
            self.assertLessEqual(export.compare(net, model, x), 1e-5)
            prog = gbai.read_json(os.path.join(out, "progress.json"))
            self.assertEqual((prog["state"], prog["epoch"], prog["epochs"]), ("done", 30, 30))
            self.assertEqual(res["best"]["epoch"], prog["best_epoch"])
            with open(os.path.join(out, "eval_report.md"), encoding="utf-8") as f:
                self.assertIn("Held-out exam levels (never trained on)", f.read())

    def test_same_seed_and_threads_train_the_same_network(self):
        import torch
        import train_pilot
        with TempRoot() as root:
            for lid in (1001, 1002, 1003):
                write_toy_run(root, lid, f"Toy {lid}", ticks=1500, seed=lid)
            files = []
            for i in range(2):
                out = os.path.join(root, f"out{i}")
                train_pilot.main(["--root", root, "--out", out, "--epochs", "1", "--threads", "2", "--fold", "all",
                                  "--no-eval"], log=quiet)
                self.assertEqual(torch.get_num_threads(), 2)
                with open(os.path.join(out, "pilot.gbnn"), "rb") as f:
                    files.append(f.read())
            self.assertEqual(files[0], files[1])
            self.assertIsInstance(gbai.limit_blas_threads(2), bool)

    def test_pilot_resumes_and_refuses_other_settings(self):
        import train_pilot
        with TempRoot() as root:
            make_toy_root(root)
            lines = []
            train_pilot.main(["--root", root, "--epochs", "2", "--threads", "2", "--no-eval"], log=lines.append)
            self.assertTrue(any(ln.startswith("fold 0/8:") for ln in lines), lines)
            lines.clear()
            train_pilot.main(["--root", root, "--epochs", "3", "--threads", "2", "--no-eval"], log=lines.append)
            self.assertTrue(any(ln.startswith("resuming after epoch 2") for ln in lines), lines)
            prog = gbai.read_json(os.path.join(root, "train", "pilot", "progress.json"))
            self.assertEqual((prog["epoch"], len(prog["history"])), (3, 3))
            with self.assertRaises(SystemExit) as cm:
                train_pilot.main(["--root", root, "--epochs", "3", "--threads", "2", "--batch", "256", "--no-eval"],
                                 log=quiet)
            self.assertIn("batch", str(cm.exception))
            self.assertEqual(gbai.read_json(os.path.join(root, "train", "pilot", "progress.json"))["state"], "failed")

    def test_load_far_keep_loads_less(self):
        import train_pilot
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy One", ticks=4000, seed=1)
            runs = gbai.list_runs(root)[0]
            X1, y1, _, near1, b1, _ = train_pilot.assemble(runs, 8)
            X2, y2, _, near2, b2, _ = train_pilot.assemble(runs, 8, load_far_keep=0.25)
            self.assertLess(len(X2), 0.5 * len(X1))
            self.assertEqual(int(near1.sum()), int(near2.sum()))  # every tick near a change is kept
            self.assertEqual(set(np.unique(b2).round(3)), {1.0, 4.0})


class BackgroundTests(unittest.TestCase):
    def test_stop_file_checkpoints_and_the_same_command_resumes_to_the_same_network(self):
        """A STOP file stops a run mid-epoch with a checkpoint; running the same command again finishes it,
        and the result is the network an uninterrupted run gives, byte for byte."""
        import background
        import train_pilot
        with TempRoot() as root:
            for lid in (1001, 1002, 1003, 1004):
                write_toy_run(root, lid, f"Toy {lid}", ticks=3000, seed=lid)
            common = ["--root", root, "--epochs", "2", "--threads", "2", "--batch", "512", "--fold", "0/4",
                      "--no-eval", "--priority", "normal"]
            ref = os.path.join(root, "ref")
            train_pilot.main(common + ["--out", ref], log=quiet)
            out = os.path.join(root, "stopped")
            background.request_stop(out)
            res = train_pilot.main(common + ["--out", out], log=quiet)
            self.assertTrue(res.get("stopped"))
            self.assertFalse(os.path.exists(background.stop_path(out)))  # removed, so the rerun resumes
            prog = gbai.read_json(os.path.join(out, "progress.json"))
            self.assertEqual(prog["state"], "stopped")
            self.assertEqual(prog["step"], 1)
            self.assertEqual(models_ck(out)["batch"], 1)
            lines = []
            train_pilot.main(common + ["--out", out], log=lines.append)
            self.assertTrue(any("resuming after epoch 0, batch 1" in ln for ln in lines), lines)
            with open(os.path.join(ref, "pilot.gbnn"), "rb") as a, open(os.path.join(out, "pilot.gbnn"), "rb") as b:
                self.assertEqual(a.read(), b.read())
            self.assertEqual(gbai.read_json(os.path.join(out, "progress.json"))["state"], "done")

    def test_reader_stops_and_resumes(self):
        import background
        import train_reader
        with TempRoot() as root:
            make_toy_root(root, tapes=True)
            common = ["--root", root, "--epochs", "2", "--channels", "8", "--threads", "2", "--tapes-per-step", "2",
                      "--layout-hash", "00aa11bb22cc33dd", "--priority", "normal"]
            out = os.path.join(root, "r")
            background.request_stop(out)
            self.assertTrue(train_reader.main(common + ["--out", out], log=quiet).get("stopped"))
            lines = []
            res = train_reader.main(common + ["--out", out], log=lines.append)
            self.assertTrue(any("resuming after epoch 0, batch 1" in ln for ln in lines), lines)
            self.assertEqual(len(res["history"]), 2)

    def test_threads_and_launch_command(self):
        import background
        self.assertIn(background.resolve_threads("auto"), (3, 6))
        self.assertEqual(background.resolve_threads("4"), 4)
        if os.name == "nt":
            info = background.launch([os.path.join(HERE, "train_pilot.py"), "--threads", "auto"],
                                     os.path.join(tempfile.gettempdir(), "x.log"), dry_run=True)
            self.assertIn(" run --log ", info["command"])
            self.assertIn("train_pilot.py", info["command"])
            self.assertIsNone(info["pid"])


def models_ck(out):
    import models
    return models.load_checkpoint(os.path.join(out, "last.pt"))


class ReaderTests(unittest.TestCase):
    def test_labels_per_column(self):
        import train_reader
        with TempRoot() as root:
            header = {"format": "gbai-tape", "version": 1, "level_id": 5, "column_width": 15.0, "x0": 0.0,
                      "columns": 6, "bands": 1, "channels": 2, "channel_layout": [{"name": "a", "offset": 0, "size": 2}],
                      "y0": 100.0, "band_height": 15.0}
            gbai.write_tape(os.path.join(root, "tapes"), header, np.zeros((6, 2), np.uint8))
            arrays = list(toy_arrays(1, 8, ship=False))
            meta = arrays[4]
            meta[:, gbai.META_X] = [0, 5, 14.9, 15, 29, 45, 46, 200]  # columns 0 0 0 1 1 3 3 (13: off the tape)
            meta[:, gbai.META_Y] = [100, 130, 160, 115, 145, 100, 100, 100]
            meta[:, gbai.META_MODE] = [0, 1, 1, 4, 4, 2, 2, 0]
            meta[:, gbai.META_GRAVITY] = [0, 0, 1, 1, 1, 0, 0, 0]
            arrays[3] = np.array([1, 0, 0, 1, 1, 0, 0, 1], np.uint8)
            write_arrays(os.path.join(root, "data"), "5__hand", run_header(5, "Hand", 8, TPS, meta), arrays)
            tape = gbai.load_tape(root, 5)
            run = gbai.list_runs(root)[0][0]
            lab = train_reader.column_labels(tape, run)
            self.assertEqual(lab["mask"].tolist(), [True, True, False, True, False, False])
            np.testing.assert_allclose(lab["y_rel"][[0, 1, 3]], [1.0, 1.0, 0.0], atol=1e-6)
            self.assertEqual(lab["mode"][[0, 1, 3]].tolist(), [1, 4, 2])
            np.testing.assert_allclose(lab["gravity"][[0, 1, 3]], [1 / 3, 1.0, 0.0], atol=1e-6)
            np.testing.assert_allclose(lab["hold"][[0, 1, 3]], [1 / 3, 1.0, 0.0], atol=1e-6)

    def test_reader_trains_exports_and_leaves_showcase_out(self):
        import export
        import models
        import train_reader
        with TempRoot() as root:
            made = make_toy_root(root, tapes=True)
            res = train_reader.main(["--root", root, "--epochs", "15", "--channels", "16", "--threads", "4",
                                     "--layout-hash", "00aa11bb22cc33dd"], log=quiet)
            out = os.path.join(root, "train", "reader")
            split = gbai.read_json(os.path.join(out, "split.json"))
            showcase_runs = {rid for rid, lid in made.items() if lid in SHOWCASE_TOY_IDS}
            self.assertEqual({r["run_id"] for r in split["showcase_excluded"]}, showcase_runs)
            used = {r["run_id"] for r in split["train_runs"] + split["val_runs"]}
            self.assertFalse(used & showcase_runs)
            self.assertEqual(split["fold"], "0/8")
            hist = res["history"]
            self.assertLess(hist[-1]["train_loss"], hist[0]["train_loss"])
            gbnn = os.path.join(out, "reader.gbnn")
            self.assertTrue(export.check_golden(gbnn)[0])
            net = gbai.read_gbnn(gbnn)
            self.assertEqual((net.kind, net.layout_hash, net.n_out), (gbai.KIND_CONV, "00aa11bb22cc33dd", 11))
            best = res["best"]["epoch"]
            model, _ = models.from_checkpoint(os.path.join(out, "best.pt" if best != len(hist) else "last.pt"))
            tape = np.asarray(gbai.load_tape(root, 1001).data, np.float32)[:120]
            self.assertLessEqual(export.compare(net, model, [tape]), 1e-5)


class CriticTests(unittest.TestCase):
    def test_labels_and_branch_pairs(self):
        import models
        import train_critic
        with TempRoot() as root:
            H, tf, rows = write_toy_practice(root, 1001, "Toy One", seed=11)
            attempts, problems = gbai.list_practice(root)
            self.assertEqual(problems, [])
            X, Y, M, sw, pairs, bad = train_critic.assemble(attempts, 4.0, log=quiet)
            self.assertEqual(len(X), sum(rows.values()))
            self.assertEqual(len(pairs), 1)
            p = int(pairs[0])
            hold, release = models.Critic.HOLD, models.Critic.RELEASE
            # a0 released at tf and died 40 ticks later; b0 held there and lived.
            self.assertEqual((Y[p, release], M[p, release]), (0.0, 1.0))
            self.assertEqual((Y[p, hold], M[p, hold]), (1.0, 1.0))
            self.assertEqual(sw[p], 4.0)
            labels = M.sum(1)
            self.assertEqual(int((labels == 2).sum()), 1)  # the pair
            self.assertEqual(int((labels == 0).sum()), 1)  # b0's own row at tf, now the pair's other half
            self.assertEqual(int((labels == 1).sum()), len(X) - 2)
            died = int(((Y == 0) & (M == 1)).sum())
            self.assertEqual(died, H)

    def test_critic_trains_and_exports_without_showcase(self):
        import export
        import train_critic
        with TempRoot() as root:
            for i, (lid, name) in enumerate([(1001, "Toy One"), (1002, "Toy Two"), (1003, "Toy Three"),
                                             (10565740, "Bloodbath")]):
                write_toy_practice(root, lid, name, seed=20 + i)
            res = train_critic.main(["--root", root, "--epochs", "3", "--threads", "4", "--fold", "0/3"], log=quiet)
            out = os.path.join(root, "train", "critic")
            split = gbai.read_json(os.path.join(out, "split.json"))
            self.assertTrue(split["showcase_excluded"])
            self.assertTrue(all(r["level_id"] == 10565740 for r in split["showcase_excluded"]))
            self.assertFalse([r for r in split["train_runs"] + split["val_runs"] if r["level_id"] == 10565740])
            gbnn = os.path.join(out, "critic.gbnn")
            self.assertTrue(export.check_golden(gbnn)[0])
            net = gbai.read_gbnn(gbnn)
            self.assertEqual(net.out_names, ["alive_hold", "alive_release"])
            self.assertEqual(net.layers[-1].activation, gbai.ACT_NONE)
            self.assertIsNotNone(res["val"])


class OrchestrateTests(unittest.TestCase):
    def manifest(self, root):
        entries = [{"macro_path": f"C:/macros/Train{i}.slc", "level_id": 3000 + i, "level_name": f"Train {i}",
                    "use_for_training": True} for i in range(1, 7)]
        entries.append({"macro_path": "C:/macros/Bloodbath.slc", "level_id": 10565740, "level_name": "Bloodbath",
                        "use_for_training": False, "is_bar_level": True})
        entries.append({"macro_path": "C:/macros/skip.slc", "level_id": 4000, "level_name": "Not used",
                        "use_for_training": False, "is_bar_level": False})
        entries.append({"macro_path": "", "level_id": 0, "level_name": "No path", "use_for_training": True})
        path = os.path.join(root, "manifest.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(entries, f)
        return path

    def test_all_with_a_fake_game(self):
        import orchestrate
        orchestrate.SETTLE_SECONDS = 0.0
        with TempRoot() as root:
            m = self.manifest(root)
            games = []

            def factory():
                games.append(FakeGame(root))
                return games[-1]
            out = orchestrate.main(["all", "--root", root, "--manifest", m, "--pilot-epochs", "2",
                                    "--reader-epochs", "2", "--threads", "2", "--levels", "10565740,14",
                                    "--log-practice"], log=quiet, game_factory=factory)
            recorded = [r for r in out["record"] if r["state"] == "done"]
            self.assertEqual(len(recorded), 7)  # six to train on and the bar level; not the unused one
            self.assertEqual(gbai.read_json(os.path.join(root, "train", "layout.json"))["layout_hash"], TOY_HASH)
            self.assertEqual(out["train"]["pilot"]["state"], "done")
            self.assertEqual(out["train"]["reader"]["state"], "done")
            self.assertNotIn("critic", out["train"])  # no practice attempts yet
            installed = {i["model"]: i for i in out["install"]}
            self.assertEqual(set(installed), {"pilot", "reader"})
            self.assertEqual(installed["pilot"]["layout_hash"], TOY_HASH)
            self.assertEqual(installed["reader"]["layout_hash"], FakeGame.TAPE_HASH)
            for name in ("pilot", "reader"):
                with open(os.path.join(root, "weights", f"{name}.gbnn"), "rb") as a, \
                        open(os.path.join(root, "train", name, f"{name}.gbnn"), "rb") as b:
                    self.assertEqual(a.read(), b.read())
            split = gbai.read_json(os.path.join(root, "train", "pilot", "split.json"))
            self.assertEqual(split["fold"], "all")  # the model to ship trains on every non-showcase level
            self.assertIn(10565740, {r["level_id"] for r in split["showcase_excluded"]})
            play = out["play"]
            self.assertEqual(sorted(r["level_id"] for r in play["results"]), [14, 10565740])
            self.assertTrue(all(r["state"] == "done" for r in play["results"]))
            calls = [args for tool, args in games[-1].calls if tool == "gucci_ai_play"]
            self.assertTrue(all(c.get("log_practice") for c in calls))
            opened = [args for tool, args in games[-1].calls if tool == "gucci_open_level"]
            self.assertEqual(sorted(opened, key=str), sorted([{"id": 10565740}, {"main": 14}], key=str))
            reports = os.listdir(os.path.join(root, "train", "reports"))
            self.assertTrue(any(n.startswith("play_") and n.endswith(".md") for n in reports))
            again = orchestrate.main(["record", "--root", root, "--manifest", m], log=quiet,
                                     game_factory=lambda: FakeGame(root))
            self.assertTrue(all(r["state"] == "skipped" for r in again["record"]))

    def test_cross_validation_over_level_folds(self):
        import orchestrate
        with TempRoot() as root:
            for lid in (1001, 1002, 1003):
                write_toy_run(root, lid, f"Toy {lid}", ticks=1200, seed=lid)
            write_toy_run(root, 10565740, "Bloodbath", ticks=1200, seed=9)
            out = orchestrate.main(["train", "--root", root, "--cv", "--pilot-epochs", "1", "--threads", "2"],
                                   log=quiet, game_factory=lambda: None)
            folds = out["cv"]["folds"]
            self.assertEqual([f["fold_used"] for f in folds], ["0/3", "1/3", "2/3"])  # 3 levels: leave-one-out
            held = sorted(lv for f in folds for lv in f["val_levels"])
            self.assertEqual(held, ["id:1001", "id:1002", "id:1003"])
            self.assertTrue(all(f["showcase"] for f in folds))
            self.assertTrue(os.path.exists(os.path.join(root, "train", "pilot_folds", "cv_summary.json")))
            self.assertEqual(out["pilot"]["state"], "done")

    def test_missing_tools_stop_with_a_clear_message(self):
        import orchestrate
        with TempRoot() as root:
            m = self.manifest(root)
            with self.assertRaises(orchestrate.MissingTools) as cm:
                orchestrate.main(["record", "--root", root, "--manifest", m], log=quiet,
                                 game_factory=lambda: FakeGame(root, tools={"gucci_get_state"}))
            self.assertIn("gucci_ai_record", str(cm.exception))
            self.assertIn("gucci_ai_status", str(cm.exception))
            with self.assertRaises(orchestrate.MissingTools) as cm:
                orchestrate.main(["play", "--root", root, "--levels", "14"], log=quiet,
                                 game_factory=lambda: FakeGame(root, tools=FakeGame.ALL - {"gucci_ai_play"}))
            self.assertIn("gucci_ai_play", str(cm.exception))


class RecordTests(unittest.TestCase):
    """orchestrate.py record: the overnight data run against a fake game."""

    def setUp(self):
        import orchestrate
        self.o = orchestrate

    def entries(self):
        def e(stem, lid, name, **kw):
            return dict({"file": f"C:/m/{stem}.ybot", "format": "ybot2", "level_id": lid, "level_name": name,
                         "use_for_training": True, "origin": "human"}, **kw)
        return [e("Done", 3001, "Done"),
                e("RealDone", 3002, "Real Done"),  # dies in the sim, the real game completes it
                e("SamePlace", 3003, "Same Place"),  # dies in both, within a block
                e("Elsewhere", 3004, "Elsewhere"),  # dies in both, 3 blocks apart
                e("Hangs", 3005, "Hangs"),  # the real game is still playing at the time cap
                e("Found", None, "Found", find_name="Found"),
                e("Twice", None, "Twice", find_name="Twice"),
                e("Nowhere", None, "Nowhere", find_name="Nowhere"),
                e("Crashy", 3006, "Crashy"),
                e("Bloodbath", 10565740, "Bloodbath"),  # an exam: recorded, never trained on
                e("Junk", 3007, "Junk", use_for_training=False, exclude_reason="junk"),
                e("Dup", 3001, "Done", use_for_training=False, duplicate_of="C:/m/Done.ybot")]

    def game(self, root):
        return FakeGame(root, sim={"RealDone": 2000, "SamePlace": 2000, "Elsewhere": 2000, "Hangs": 2000},
                        find={"Found": [{"id": 5555, "name": "found", "stars": 0}, {"id": 5556, "name": "Found It"}],
                              "Twice": [{"id": 1, "name": "Twice", "stars": 2}, {"id": 2, "name": "twice", "stars": 3}],
                              "Nowhere": [{"id": 9, "name": "Somewhere"}]},
                        crash={"Crashy": 1})

    def run_record(self, root, m, game, *extra):
        return self.o.main(["record", "--root", root, "--manifest", m, "--validate-timeout", "-1", *extra],
                           log=quiet, game_factory=lambda: game)

    def test_validation_rule(self):
        base = {"completed": False, "validated": "failed", "died_tick": 900, "died_x": 3000.0, "frame_offset": 1}
        rule = gbai.validation_rule
        self.assertEqual(rule(dict(base, completed=True, validated="sim", died_tick=None))[0], "whole")
        self.assertEqual(rule(base)[0], "none")  # died, never checked in the real game
        self.assertEqual(rule(base, {"completed": True, "frame_offset": 1})[0], "before_death")
        self.assertEqual(rule(base, {"completed": False, "died_x": 3029.0, "frame_offset": 1})[0], "before_death")
        self.assertEqual(rule(base, {"completed": False, "died_x": 2971.0})[0], "before_death")
        self.assertEqual(rule(base, {"completed": False, "died_x": 3031.0, "frame_offset": 1})[0], "none")
        self.assertEqual(rule(base, {"completed": True, "frame_offset": 0})[0], "none")  # another offset
        self.assertEqual(rule(base, {"completed": None, "timed_out": True})[0], "none")
        self.assertEqual(rule(base, {"exclude": "two-player level"})[0], "none")
        self.assertEqual(rule(dict(base, completed=True, validated="sim"), {"exclude": "x"})[0], "none")
        self.assertEqual(rule(dict(base, died_tick=None), {"completed": True})[0], "none")
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy", ticks=3000, seed=1, died_tick=2000, stem="died", validated="failed")
            run = gbai.list_runs(root)[0][0]
            self.assertEqual(run.usable_ticks(), 0)
            gbai.write_validation(root, run.run_id, {"completed": True, "frame_offset": 0, "died_x": None,
                                                     "percent": 100.0})
            run = gbai.list_runs(root)[0][0]
            self.assertEqual(run.usable_ticks(), 2000 - TPS)
            side = gbai.read_json(gbai.validation_path(root, run.run_id))
            self.assertEqual(side["format"], "gbai-validation")
            self.assertIn("when", side)
            gbai.write_validation(root, run.run_id, {"completed": False, "died_x": run.header["died_x"] + 2 * gbai.BLOCK,
                                                     "frame_offset": 0})
            self.assertEqual(gbai.list_runs(root)[0][0].usable_ticks(), 0)

    def test_pick_level(self):
        pick = self.o.pick_level
        self.assertEqual(pick("Found", [{"id": 5, "name": "FOUND"}, {"id": 6, "name": "Found 2"}])[0], 5)
        self.assertIsNone(pick("Found", [{"id": 6, "name": "Found 2"}])[0])
        self.assertIsNone(pick("x", [])[0])
        self.assertEqual(pick("A", [{"id": 1, "name": "a", "stars": 0}, {"id": 2, "name": "A", "stars": 5}])[0], 2)
        self.assertEqual(pick("A", [{"id": 1, "name": "a", "stars": 2, "downloads": 10},
                                    {"id": 2, "name": "A", "stars": 3, "downloads": 900}])[0], 2)
        self.assertIsNone(pick("A", [{"id": 1, "name": "a", "stars": 2}, {"id": 2, "name": "A", "stars": 3}])[0])
        self.assertIsNone(pick("A", [{"id": 1, "name": "a"}, {"id": 2, "name": "A"}])[0])
        self.assertEqual(self.o.sanitize("Thé End!"), "Th___End_")  # per UTF-8 byte (2 for the e), like the mod
        self.assertEqual(self.o.expected_run_id(14, "C:/x/Clubstep.gdr", "gdr"), "14__Clubstep__gdr")

    def test_record_validates_resumes_and_reports(self):
        with TempRoot() as root:
            m = os.path.join(root, "manifest.json")
            with open(m, "w", encoding="utf-8") as f:
                json.dump(self.entries(), f)
            game = self.game(root)
            # The real game's deaths, placed against where the simulation died (FakeGame seeds by level id).
            sim_x = {stem: float(toy_arrays(lid % 997, 4000, TPS, 1.0, 2000)[4][2000, gbai.META_X])
                     for stem, lid in (("SamePlace", 3003), ("Elsewhere", 3004))}
            game.real = {"RealDone": {"completed": True, "died_x": None, "percent": 100.0},
                         "SamePlace": {"completed": False, "died_x": sim_x["SamePlace"] + 20.0, "percent": 50.0},
                         "Elsewhere": {"completed": False, "died_x": sim_x["Elsewhere"] + 90.0, "percent": 51.0},
                         "Hangs": "hang"}
            out = self.run_record(root, m, game)["record"]
            by = {os.path.splitext(os.path.basename(r["macro_path"]))[0]: r for r in out}
            self.assertNotIn("Junk", by)
            self.assertNotIn("Dup", by)
            self.assertEqual(by["Done"]["verdict"], "whole")
            self.assertEqual(by["RealDone"]["verdict"], "before_death")
            self.assertEqual(by["SamePlace"]["verdict"], "before_death")
            self.assertEqual(by["Elsewhere"]["verdict"], "none")
            self.assertEqual(by["Hangs"]["verdict"], "none")
            self.assertTrue(by["Hangs"]["real"]["timed_out"])
            self.assertTrue(by["Hangs"]["real"]["real_passed_sim_death"])
            self.assertEqual((by["Found"]["state"], by["Found"]["level_id"]), ("done", 5555))
            self.assertEqual(by["Twice"]["state"], "unresolved")  # two rated exact matches, no downloads to pick by
            self.assertEqual(by["Nowhere"]["state"], "unresolved")
            self.assertEqual(by["Crashy"]["state"], "done")
            self.assertEqual(game.recoveries, 1)
            self.assertEqual(by["Bloodbath"]["state"], "done")
            # Sidecars only for runs that died in the simulation; the usable rule reads them.
            vdir = os.path.join(root, "validation")
            self.assertEqual(sorted(os.listdir(vdir)),
                             sorted(f"{by[s]['run_id']}.json" for s in ("RealDone", "SamePlace", "Elsewhere", "Hangs")))
            runs = {r.run_id: r for r in gbai.list_runs(root)[0]}
            self.assertEqual(runs[by["RealDone"]["run_id"]].usable_ticks(), 2000 - TPS)
            self.assertEqual(runs[by["SamePlace"]["run_id"]].usable_ticks(), 2000 - TPS)
            self.assertEqual(runs[by["Elsewhere"]["run_id"]].usable_ticks(), 0)
            self.assertEqual(runs[by["Done"]["run_id"]].usable_ticks(), 4000)
            # The exam's run is recorded but never trained on, in any fold.
            for k in range(3):
                split = gbai.make_split(list(runs.values()), gbai.Showcase(), (k, 3),
                                        usable=lambda r: r.usable_ticks() > 0)
                self.assertNotIn(10565740, {r.level_id for r in split["train"] + split["val"]})
            progress = gbai.read_json(os.path.join(root, "train", "record_progress.json"))
            self.assertEqual(progress["resolved"]["found"]["id"], 5555)
            with open(os.path.join(root, "DATA_REPORT.md"), encoding="utf-8") as f:
                report = f.read()
            for text in ("By format", "By frame offset", "Simulation vs the real game", "simulator bugs to fix",
                         "Held-out exam macros", "Bloodbath", "Not recorded, and why", "Twice", "ambiguous",
                         "sim died, real completed", "Elsewhere"):
                self.assertIn(text, report)
            self.assertTrue(os.path.exists(os.path.join(root, "train", "reports", "DATA_REPORT.md")))
            # A second run carries on: nothing finished is recorded again.
            game2 = self.game(root)
            again = self.run_record(root, m, game2)["record"]
            self.assertTrue(all(r["state"] == "skipped" for r in again), [r["state"] for r in again])
            self.assertFalse([c for c in game2.calls if c[0] in ("gucci_ai_record", "gucci_ai_validate_real")])

    def test_a_game_that_keeps_crashing_is_given_up_on(self):
        with TempRoot() as root:
            m = os.path.join(root, "manifest.json")
            with open(m, "w", encoding="utf-8") as f:
                json.dump([{"file": "C:/m/Bad.ybot", "level_id": 3100, "level_name": "Bad", "use_for_training": True},
                           {"file": "C:/m/Good.ybot", "level_id": 3101, "level_name": "Good",
                            "use_for_training": True}], f)
            game = FakeGame(root, crash={"Bad": 99})
            out = self.run_record(root, m, game)["record"]
            states = {os.path.basename(r["macro_path"]): r["state"] for r in out}
            self.assertEqual(states, {"Bad.ybot": "failed", "Good.ybot": "done"})
            self.assertEqual(game.recoveries, self.o.MAX_CRASHES_PER_ENTRY)

    def test_same_run_id_from_two_folders_gets_a_copy(self):
        with TempRoot() as root:
            src = os.path.join(root, "src")
            for sub, data in (("a", b"one"), ("b", b"two")):
                os.makedirs(os.path.join(src, sub))
                with open(os.path.join(src, sub, "Same.ybot"), "wb") as f:
                    f.write(data)
            m = os.path.join(root, "manifest.json")
            with open(m, "w", encoding="utf-8") as f:
                json.dump([{"file": os.path.join(src, s, "Same.ybot"), "format": "ybot2", "level_id": 3200,
                            "level_name": "Same", "use_for_training": True} for s in ("a", "b")], f)
            out = self.run_record(root, m, FakeGame(root))["record"]
            self.assertEqual([r["state"] for r in out], ["done", "done"])
            self.assertNotEqual(out[0]["run_id"], out[1]["run_id"])
            self.assertIn("macro_copies", out[1]["recorded_from_copy"])


if __name__ == "__main__":
    unittest.main()

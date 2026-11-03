"""Self-contained tests for the AI trainer package (tools/ai/).

Writes a SYNTHETIC dataset in the exact contract format into a temporary
AI_ROOT and runs the real trainers on it. Nothing here touches the mod's own
AI_ROOT, the game, or anything outside a temp folder.

The toy: a cube runs right at GD speed (240 tps, one level at 360). Gaps come
every 6-20 blocks; the "human" holds exactly while the next gap is 2.5 to 1.0
blocks ahead (about 35 ticks of hold, then 100-450 ticks released: sparse
presses like a real macro). Speed portals change the speed inside a level,
so a hold's length in ticks varies. The pilot sees that distance (coarse,
and a fine copy clipped at 3 blocks) among 38 other inputs (the button it
holds, ticks since the last press/release, 19 smooth distractors), so it has
to learn the window; the trivial baseline (repeat the last tick's button)
never presses at all.

    python tools/ai/test_ai.py            # all tests
    python tools/ai/test_ai.py -k pilot   # the ones with "pilot" in their name
"""
import hashlib
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
TOY_LAYOUT = [
    {"name": "player.mode", "offset": 0, "size": 8},
    {"name": "player.gravity_flipped", "offset": 8, "size": 1},
    {"name": "player.speed", "offset": 9, "size": 5},
    {"name": "player.yvel", "offset": 14, "size": 1},
    {"name": "player.held", "offset": 15, "size": 1},
    {"name": "player.log_since_press", "offset": 16, "size": 1},
    {"name": "player.log_since_release", "offset": 17, "size": 1},
    {"name": "player.inv_tps", "offset": 18, "size": 1},
    {"name": "scan.gap_ahead", "offset": 19, "size": 1},
    {"name": "scan.gap_ahead_near", "offset": 20, "size": 1},
    {"name": "scan.distractors", "offset": 21, "size": 19},
]
TOY_DIM = 40
TOY_HASH = hashlib.sha256(json.dumps(TOY_LAYOUT).encode()).hexdigest()[:16]
TAPE_LAYOUT = [{"name": "gap", "offset": 0, "size": 1}, {"name": "deadly_band", "offset": 1, "size": 2},
               {"name": "solid_band", "offset": 3, "size": 2}, {"name": "portal_ship", "offset": 5, "size": 1}]
TAPE_K = 6
# (level_id, name, speed, tps, ticks): twelve ordinary levels and two
# showcase ones (Bloodbath by its id, The Nightmare by its name).
TOY_LEVELS = [(1001, "Toy One", 1.0, 240, 7000), (1002, "Toy Two", 0.9, 240, 7000),
              (1003, "Toy Three", 1.1, 240, 7000), (1004, "Toy Four", 1.0, 360, 9000),
              (1005, "Toy Five", 1.25, 240, 6000), (1006, "Toy Six", 0.8, 240, 7000),
              (1007, "Toy Seven", 1.0, 240, 7000), (1008, "Toy Eight", 0.85, 240, 7000),
              (1009, "Toy Nine", 1.15, 240, 7000), (1010, "Toy Ten", 1.0, 360, 9000),
              (1011, "Toy Eleven", 0.95, 240, 7000), (1012, "Toy Twelve", 1.2, 240, 6000),
              (10565740, "BB copy", 1.0, 240, 6000), (2002, "the  NIGHTMARE", 1.0, 240, 6000)]
SHOWCASE_TOY_IDS = {10565740, 2002}


SPEED_STEPS = (0.8, 1.0, 1.25)  # speed portals: the multiplier changes every 900-2400 ticks


def toy_arrays(seed, ticks, tps=TPS, speed=1.0, died_tick=None, ship=True):
    """(obs float16, act, meta, gaps_x) for one synthetic run, per the contract's tick convention.

    The speed changes inside the run (like speed portals), so how long a hold
    lasts in ticks varies and only the gap distance tells when to let go. The
    20 distractor inputs are smooth along the level (as real scan inputs are),
    not fresh noise every tick, which a network could memorize presses by.
    """
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
    g = np.minimum(gaps[nxt] - x, 20.0).astype(np.float16)  # the next gap ahead, as the pilot sees it
    act = ((g >= HOLD_TO) & (g < HOLD_FROM)).astype(np.uint8)
    h = gbai.before(act).astype(np.uint8)
    t = np.arange(ticks)
    press = (act == 1) & (h == 0)
    release = (act == 0) & (h == 1)
    # Ticks since the last press/release BEFORE this tick (obs[t] is before step t).
    lp = np.maximum.accumulate(np.where(press, t, -100000))
    lr = np.maximum.accumulate(np.where(release, t, -100000))
    lp_before = np.concatenate([[-100000], lp[:-1]])
    lr_before = np.concatenate([[-100000], lr[:-1]])
    speed_index = np.searchsorted(np.array(SPEED_STEPS) * speed, mult - 1e-9).clip(0, 4)
    obs = np.zeros((ticks, TOY_DIM), np.float32)
    obs[:, 0] = 1.0
    obs[t, 9 + speed_index] = 1.0
    obs[:, 14] = 0.05 * np.sin(x * 1.7 + seed)
    obs[:, 15] = h
    obs[:, 16] = np.log1p(np.minimum(t - lp_before, 100000))
    obs[:, 17] = np.log1p(np.minimum(t - lr_before, 100000))
    obs[:, 18] = 1.0 / tps
    obs[:, 19] = g
    obs[:, 20] = np.minimum(g, np.float16(3.0))  # the same distance, fine scale (moves ~6x more per tick)
    freq = rng.uniform(0.05, 0.6, 19)
    phase = rng.uniform(0, 2 * np.pi, 19)
    obs[:, 21:] = np.sin(x[:, None] * freq[None, :] + phase[None, :]) * rng.uniform(0.5, 3.0, 19)
    meta = np.zeros((ticks, len(gbai.META_FIELDS)), np.float32)
    meta[:, gbai.META_X] = x * gbai.BLOCK
    meta[:, gbai.META_Y] = 105.0 + gbai.BLOCK * act
    meta[:, gbai.META_YVEL] = obs[:, 14]
    if ship:
        meta[int(ticks * 0.6):int(ticks * 0.7), gbai.META_MODE] = 1  # a ship part, for the reader's labels
    meta[:, gbai.META_SPEED] = speed_index
    if died_tick is not None:
        meta[died_tick + 1:, gbai.META_DEAD] = 1
    return obs.astype(np.float16), act, meta, gaps


def run_header(level_id, name, ticks, tps, meta, completed=True, died_tick=None, macro_file="", source="human",
               layout_hash=TOY_HASH, layout=TOY_LAYOUT, dim=TOY_DIM, **extra):
    h = {"format": "gbai-run", "version": 1, "source": source, "level_id": level_id, "level_name": name,
         "macro_file": macro_file, "macro_format": "slc3", "tps": tps, "ticks": ticks, "obs_dim": dim,
         "obs_layout_hash": layout_hash, "obs_layout": layout, "meta_fields": list(gbai.META_FIELDS),
         "completed": completed, "died_tick": died_tick,
         "died_x": None if died_tick is None else float(meta[died_tick, gbai.META_X]),
         "level_length_x": float(meta[-1, gbai.META_X]) + 300.0, "notes": "synthetic test data"}
    h.update(extra)
    return h


def write_toy_run(root, level_id, name, speed=1.0, tps=TPS, ticks=7000, seed=0, died_tick=None, stem=None,
                  macro_file=None):
    obs, act, meta, gaps = toy_arrays(seed, ticks, tps, speed, died_tick)
    stem = stem or name.strip().replace(" ", "_")
    run_id = f"{level_id}__{stem}"
    header = run_header(level_id, name, ticks, tps, meta, died_tick is None, died_tick,
                        macro_file or f"C:/macros/{stem}.slc")
    gbai.write_run(os.path.join(root, "data"), run_id, header, obs, act, meta)
    return run_id, gaps, float(meta[-1, gbai.META_X]) / gbai.BLOCK + 2.0


def write_toy_tape(root, level_id, gaps, length_blocks):
    """A tape for a toy level: 0.5-block columns, a 'gap' channel where a gap starts, a ship portal mark."""
    cols = int(np.ceil(length_blocks * 2)) + 4
    data = np.zeros((cols, TAPE_K), np.float32)
    for gx in gaps:
        c = int(np.floor(gx * gbai.BLOCK / 15.0))
        if 0 <= c < cols:
            data[c, 0] = 1.0
            data[c, 1] = 1.0
    data[:, 3] = 1.0  # the ground
    data[cols * 6 // 10, 5] = 1.0
    header = {"format": "gbai-tape", "version": 1, "level_id": level_id, "column_width": 15.0, "x0": 0.0,
              "columns": cols, "channels": TAPE_K, "channel_layout": TAPE_LAYOUT, "y0": 105.0, "band_height": 15.0}
    gbai.write_tape(os.path.join(root, "tapes"), header, data.astype(np.float16))


def make_toy_root(root, tapes=False):
    """Every toy level (one run each, two on Toy One, Toy Two's second run dies). Returns {run_id: level}."""
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
    return made


class TempRoot:
    """A temporary AI_ROOT, removed afterwards (memmaps released first: Windows can't delete mapped files)."""

    def __enter__(self):
        self.path = tempfile.mkdtemp(prefix="gbai_test_")
        return self.path

    def __exit__(self, *exc):
        import gc
        gc.collect()
        shutil.rmtree(self.path, ignore_errors=True)


def write_toy_practice(root, level_id, name, seed, ticks=3000, tps=TPS):
    """Practice attempts on one toy level (contract s4):
    a0 skips its 3rd press and dies 40 ticks later; b0 branches from a0 at that
    tick, presses, and completes (its log starts at tick 0, a copy of a0's
    before it); a1 completes; a2 stops at a limit without dying (its last H
    rows have no known outcome). Returns (H, the branch tick, row counts)."""
    obs, act, meta, _ = toy_arrays(seed, ticks, tps, ship=False)
    H = int(round(tps))
    presses = np.flatnonzero((act == 1) & (gbai.before(act) == 0))
    tf = int(presses[2])
    folder = os.path.join(root, "practice", str(level_id))
    t = np.arange(ticks)

    def put(attempt, n, a, alive, completed, died_tick, **extra):
        header = run_header(level_id, name, n, tps, meta[:n], completed, died_tick, "", "ai", **extra)
        gbai.write_run(folder, attempt, header, obs[:n], a[:n], meta[:n], alive.astype(np.int32))

    died = tf + 40
    a0 = act.copy()
    a0[tf:] = 0
    put("a0", died + 1, a0, np.minimum(H, died - t[:died + 1]), False, died)
    put("b0", ticks, act, np.full(ticks, H), True, None, branch_of={"attempt": "a0", "tick": tf}, first_tick=0)
    put("a1", ticks, act, np.full(ticks, H), True, None)
    stop = 2000
    put("a2", stop, act, np.minimum(H, stop - t[:stop]), False, None)
    rows = {"a0": died + 1, "b0": ticks - max(0, tf - H), "a1": ticks, "a2": stop - H}
    return H, tf, rows


class FakeGame:
    """Stands in for the game in orchestrate.py's tests: the contract's tools (s6), answered from here.

    gucci_ai_record writes a toy run (and tape) for the macro into the temp
    AI_ROOT; gucci_ai_play answers with a made-up result. Leave tools out to
    test what happens on a build without them.
    """

    ALL = {"gucci_ai_record", "gucci_ai_status", "gucci_ai_reload", "gucci_ai_play", "gucci_ai_layout",
           "gucci_open_level", "gucci_quit_level", "gucci_get_state"}
    TAPE_HASH = "00aa11bb22cc33dd"

    def __init__(self, root, tools=None):
        self.root = root
        self.tools = set(self.ALL if tools is None else tools)
        self.jobs = {}
        self.level = None
        self.calls = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def alive(self):
        return True

    def _job(self, result):
        job = f"job{len(self.jobs)}"
        self.jobs[job] = {"polls": 0, "result": result}
        return {"job": job}

    def call(self, tool, **args):
        self.calls.append((tool, args))
        if tool not in self.tools:
            raise RuntimeError(f"{tool}: no tool called {tool}")
        if tool == "gucci_ai_layout":
            return {"obs_layout": TOY_LAYOUT, "obs_layout_hash": TOY_HASH, "tape_layout": TAPE_LAYOUT,
                    "tape_layout_hash": self.TAPE_HASH}
        if tool == "gucci_ai_record":
            lid = int(args["level_id"])
            stem = os.path.splitext(os.path.basename(args["macro_path"]))[0]
            run_id, gaps, length = write_toy_run(self.root, lid, f"Level {stem}", ticks=4000, seed=lid % 997,
                                                 stem=stem, macro_file=args["macro_path"])
            write_toy_tape(self.root, lid, gaps, length)
            return self._job({"run_id": run_id})
        if tool == "gucci_ai_status":
            j = self.jobs[args["job"]]
            j["polls"] += 1
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
            return self._job({"completed": False, "best_percent": 42.5, "attempts": 3,
                              "deaths": [{"tick": 900, "x": 2700.0, "percent": 12.0, "cause": "spike from above",
                                          "mistake_tick": 893, "fix": "pressed 3 ticks earlier"}]})
        raise RuntimeError(f"FakeGame has no {tool}")


def quiet(*_):
    pass


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
        x = (self.rng.standard_normal((64, TOY_DIM)) * 5).astype(np.float32)
        self.check(models.Pilot(TOY_DIM, (256, 256)), x)

    def test_critic(self):
        import models
        x = (self.rng.standard_normal((64, TOY_DIM)) * 5).astype(np.float32)
        net = self.check(models.Critic(TOY_DIM, (256, 256)), x)
        self.assertEqual(net.layers[-1].activation, gbai.ACT_SIGMOID)
        self.assertEqual(net.out_names, ["alive_hold", "alive_release"])

    def test_reader(self):
        import models
        model = models.Reader(TAPE_K, 16, 5, (1, 2, 4, 8, 16, 32))
        seqs = [self.rng.standard_normal((c, TAPE_K)).astype(np.float32) for c in (1, 2, 7, 40, 300)]
        net = self.check(model, seqs)
        self.assertEqual(net.out_names, models.READER_OUTPUTS)

    def test_broken_values_read_as_zero_everywhere(self):
        """A value that isn't finite (a float16 overflow is inf) trains as 0. Validation and the report
        must read it the same way: they used to give P = 0.5 (a hold) and NaN (a release)."""
        import evaluate
        import export
        import models
        import train_pilot
        model = models.Pilot(TOY_DIM, (16,))
        model.set_norm(self.rng.standard_normal(TOY_DIM), self.rng.uniform(0.5, 2.0, TOY_DIM))
        net = gbai.parse_gbnn(gbai.gbnn_bytes(export.to_gbnn(model, TOY_HASH)))
        rows = (self.rng.standard_normal((6, TOY_DIM)) * 3).astype(np.float16)
        rows[0, 3], rows[1, 5], rows[2, 7] = np.inf, -np.inf, np.nan
        clean = rows.astype(np.float32)
        clean[~np.isfinite(clean)] = 0
        want = export.sigmoid(export.forward(net, clean)[:, 0])
        np.testing.assert_allclose(evaluate.pilot_probs(net, rows), want, atol=1e-5)
        np.testing.assert_allclose(export.sigmoid(train_pilot.predict_logits(model, rows)), want, atol=1e-5)
        self.assertTrue(np.isfinite(rows[3:]).all())  # the caller's rows are left as they were
        self.assertTrue(np.isinf(rows[0, 3]))

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
            runs, problems = gbai.list_runs(root)
            self.assertEqual((len(runs), problems), (2, []))
            for r in runs:
                r.close()
            base = os.path.join(root, "data", "1002__Toy_Two")
            with open(base + ".act.u8", "ab") as f:
                f.write(b"\0")  # one row too many
            runs, problems = gbai.list_runs(root)
            self.assertEqual(len(runs), 1)
            self.assertIn("act.u8 is 501 bytes, not 500", problems[0][1])
            obs, act, meta, _ = toy_arrays(3, 300)
            act[5] = 2
            gbai.write_run(os.path.join(root, "data"), "1003__bad", run_header(1003, "Bad", 300, TPS, meta), obs,
                           act, meta)
            h = run_header(1004, "Bad hash", 300, TPS, meta, layout_hash="xyz")
            gbai.write_run(os.path.join(root, "data"), "1004__badhash", h, obs, act.clip(0, 1), meta)
            runs, problems = gbai.list_runs(root)
            text = " | ".join(p[1] for p in problems)
            self.assertIn("only 0 and 1", text)
            self.assertIn("not 16 hex", text)
            for r in runs:
                r.close()

    def test_one_layout_per_training(self):
        with TempRoot() as root:
            write_toy_run(root, 1001, "Toy One", ticks=300, seed=1)
            obs, act, meta, _ = toy_arrays(2, 300)
            other = "aaaaaaaaaaaaaaaa"
            gbai.write_run(os.path.join(root, "data"), "1002__other",
                           run_header(1002, "Other", 300, TPS, meta, layout_hash=other), obs, act, meta)
            runs, _ = gbai.list_runs(root)
            with self.assertRaises(gbai.DataError) as cm:
                gbai.pick_layout(runs)
            self.assertIn("--layout-hash", str(cm.exception))
            picked, h = gbai.pick_layout(runs, other)
            self.assertEqual(([r.run_id for r in picked], h), (["1002__other"], other))
            for r in runs:
                r.close()

    def test_a_run_that_died_loses_its_last_second(self):
        with TempRoot() as root:
            write_toy_run(root, 1002, "Toy Two", ticks=5000, seed=8, died_tick=4200, stem="died")
            write_toy_run(root, 1001, "Toy One", ticks=5000, seed=7)
            runs = {r.run_id: r for r in gbai.list_runs(root)[0]}
            self.assertEqual(runs["1002__died"].usable_ticks(), 4200 - 240)
            self.assertEqual(runs["1001__Toy_One"].usable_ticks(), 5000)
            for r in runs.values():
                r.close()

    def test_showcase_matching(self):
        show = gbai.Showcase()
        for lid, name, macro in [(10565740, "anything", ""), (14, "", ""), (0, "Theory of Everything 2", ""),
                                 (55, "LIMBO", ""), (56, "Heliopolis      ", ""), (57, "", "C:/x/ORBIT.slc"),
                                 (58, "the  NIGHTMARE", ""), (59, "Silent clubstep", ""), (60, "", "toe2.gdr"),
                                 # every showcase level by id alone too (an empty or changed name can't let one in)
                                 (13519, "", ""), (18, "", ""), (20, "", ""), (86084399, "a renamed copy", ""),
                                 (133175713, "", "run.slc"), (136530685, "", "")]:
            self.assertIsNotNone(show.reason(lid, name, macro), (lid, name, macro))
        for lid, name, macro in [(1, "Stereo Madness", ""), (61, "Lucid Nightmares", ""), (62, "Bloodlust", ""),
                                 (63, "Evolutionary Theory", ""), (12, "Theory of Everything", "toe.slc")]:
            self.assertIsNone(show.reason(lid, name, macro), (lid, name, macro))
        with TempRoot() as d:
            path = os.path.join(d, "manifest.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"entries": [{"path": "x.slc", "level_id": 777, "level_name": "Some Bar Level",
                                        "is_bar_level": True}]}, f)
            show = gbai.Showcase(manifest=path)
            self.assertIsNotNone(show.reason(777, "renamed", ""))
            self.assertIsNotNone(show.reason(0, "some bar level", ""))

    def test_levels_with_nothing_usable_take_no_split_slot(self):
        """A level whose only run is empty or died in its first second is set aside before the split,
        so it can't be the level a fold validates on (nothing to validate) or all that trains."""
        with TempRoot() as root:
            for lid in (1001, 1002, 1003):
                write_toy_run(root, lid, f"Toy {lid}", ticks=600, seed=lid)
            write_toy_run(root, 1004, "Toy Early", ticks=600, seed=4, died_tick=100, stem="early")
            obs, act, meta, _ = toy_arrays(5, 10)
            gbai.write_run(os.path.join(root, "data"), "1005__empty", run_header(1005, "Empty", 0, TPS, meta),
                           obs[:0], act[:0], meta[:0])
            runs, problems = gbai.list_runs(root)
            self.assertEqual(problems, [])
            for k in range(3):
                split = gbai.make_split(runs, gbai.Showcase(), (k, 3), usable=lambda r: r.usable_ticks() > 0)
                self.assertEqual(sorted(r.run_id for r in split["unusable"]), ["1004__early", "1005__empty"])
                self.assertEqual((len(split["train"]), len(split["val"])), (2, 1))
                self.assertTrue(all(r.usable_ticks() > 0 for r in split["train"] + split["val"]))
                rec = gbai.split_record(split, "pilot", TOY_HASH, (k, 3))
                self.assertEqual(sorted(r["run_id"] for r in rec["unusable_runs"]), ["1004__early", "1005__empty"])
            for r in runs:
                r.close()

    def test_splits_are_by_level(self):
        keys = [f"id:{i}" for i in range(1, 11)]
        seen = []
        for k in range(5):
            train, val = gbai.split_levels(keys, fold=(k, 5))
            self.assertFalse(set(train) & set(val))
            self.assertEqual(sorted(train + val), sorted(keys))
            seen += val
        self.assertEqual(sorted(seen), sorted(keys))  # every level validates exactly once over the folds
        train, val = gbai.split_levels(keys, val_frac=0.15)
        self.assertEqual(len(val), 2)
        self.assertEqual(gbai.split_levels(keys, val_frac=0)[1], [])
        self.assertEqual(gbai.parse_fold("2/7"), (2, 7))
        for bad in ("7/7", "1/1", "x"):
            with self.assertRaises(ValueError):
                gbai.parse_fold(bad)


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
            self.assertFalse(set(split["train_levels"]) & set(split["val_levels"]))
            report = gbai.read_json(os.path.join(out, "eval_report.json"))
            self.assertEqual({r["group"] for r in report["runs"]}, {"validation", "showcase"})
            for g in ("validation", "showcase"):
                m, b = report["groups"][g]["model"], report["groups"][g]["baseline"]
                # Measured over 4 seeds and 3 folds: presses 79-100%, releases 69-89% within 2 ticks.
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
            self.assertEqual(net.layout_hash, TOY_HASH)
            model, ck = models.from_checkpoint(os.path.join(out, "best.pt"))
            run = [r for r in gbai.list_runs(root)[0] if r.level_id == 10565740][0]
            x = np.asarray(run.obs[:3000], np.float32)  # real rows of a level it never saw
            run.close()
            self.assertLessEqual(export.compare(net, model, x), 1e-5)
            prog = gbai.read_json(os.path.join(out, "progress.json"))
            self.assertEqual((prog["state"], prog["epoch"], prog["epochs"]), ("done", 30, 30))
            self.assertEqual(res["best"]["epoch"], prog["best_epoch"])
            with open(os.path.join(out, "eval_report.md"), encoding="utf-8") as f:
                self.assertIn("Showcase levels (never trained on)", f.read())
            del model

    def test_same_seed_and_threads_train_the_same_network(self):
        """--seed seeds torch's weight init too (it used to give a different network every run),
        and --threads reaches numpy's own BLAS as well as torch."""
        import torch
        import train_pilot
        with TempRoot() as root:
            for lid in (1001, 1002, 1003):
                write_toy_run(root, lid, f"Toy {lid}", ticks=1500, seed=lid)
            files = []
            for i in range(2):
                out = os.path.join(root, f"out{i}")
                train_pilot.main(["--root", root, "--out", out, "--epochs", "1", "--threads", "2", "--val-frac", "0",
                                  "--no-eval"], log=quiet)
                self.assertEqual(torch.get_num_threads(), 2)
                with open(os.path.join(out, "pilot.gbnn"), "rb") as f:
                    files.append(f.read())
            self.assertEqual(files[0], files[1])
            self.assertIsInstance(gbai.limit_blas_threads(2), bool)  # True with the pip numpy (OpenBLAS)

    def test_pilot_resumes_and_refuses_other_settings(self):
        import train_pilot
        with TempRoot() as root:
            make_toy_root(root)
            lines = []
            train_pilot.main(["--root", root, "--epochs", "2", "--threads", "2", "--no-eval"], log=lines.append)
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


class ReaderTests(unittest.TestCase):
    def test_labels_per_column(self):
        import train_reader
        with TempRoot() as root:
            header = {"format": "gbai-tape", "version": 1, "level_id": 5, "column_width": 15.0, "x0": 0.0,
                      "columns": 6, "channels": 2, "channel_layout": [{"name": "a", "offset": 0, "size": 2}],
                      "y0": 100.0, "band_height": 15.0}
            gbai.write_tape(os.path.join(root, "tapes"), header, np.zeros((6, 2), np.float16))
            meta = np.zeros((8, 8), np.float32)
            meta[:, gbai.META_X] = [0, 5, 14.9, 15, 29, 45, 46, 200]  # columns 0 0 0 1 1 3 3 (13: off the tape)
            meta[:, gbai.META_Y] = [100, 130, 160, 115, 145, 100, 100, 100]
            meta[:, gbai.META_MODE] = [0, 1, 1, 4, 4, 2, 2, 0]
            meta[:, gbai.META_GRAVITY] = [0, 0, 1, 1, 1, 0, 0, 0]
            act = np.array([1, 0, 0, 1, 1, 0, 0, 1], np.uint8)
            obs = np.zeros((8, TOY_DIM), np.float16)
            gbai.write_run(os.path.join(root, "data"), "5__hand", run_header(5, "Hand", 8, TPS, meta), obs, act, meta)
            tape = gbai.load_tape(root, 5)
            run = gbai.list_runs(root)[0][0]
            lab = train_reader.column_labels(tape, run)
            self.assertEqual(lab["mask"].tolist(), [True, True, False, True, False, False])
            np.testing.assert_allclose(lab["y_rel"][[0, 1, 3]], [1.0, 1.0, 0.0], atol=1e-6)
            self.assertEqual(lab["mode"][[0, 1, 3]].tolist(), [1, 4, 2])
            np.testing.assert_allclose(lab["gravity"][[0, 1, 3]], [1 / 3, 1.0, 0.0], atol=1e-6)
            np.testing.assert_allclose(lab["hold"][[0, 1, 3]], [1 / 3, 1.0, 0.0], atol=1e-6)
            run.close()
            tape.close()

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
            del model


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
            self.assertEqual(int((labels == 1).sum()), len(X) - 2)  # every other row: the head of its action
            died = int(((Y == 0) & (M == 1)).sum())
            self.assertEqual(died, H)  # a0's last H rows (each less than 1 s before its death); a2's last H aren't there
            for a in attempts:
                a.close()

    def test_critic_trains_and_exports_without_showcase(self):
        import export
        import train_critic
        with TempRoot() as root:
            for i, (lid, name) in enumerate([(1001, "Toy One"), (1002, "Toy Two"), (1003, "Toy Three"),
                                             (10565740, "Bloodbath")]):
                write_toy_practice(root, lid, name, seed=20 + i)
            res = train_critic.main(["--root", root, "--epochs", "3", "--threads", "4", "--val-frac", "0.34"],
                                    log=quiet)
            out = os.path.join(root, "train", "critic")
            split = gbai.read_json(os.path.join(out, "split.json"))
            self.assertTrue(split["showcase_excluded"])
            self.assertTrue(all(r["level_id"] == 10565740 for r in split["showcase_excluded"]))
            self.assertFalse([r for r in split["train_runs"] + split["val_runs"] if r["level_id"] == 10565740])
            gbnn = os.path.join(out, "critic.gbnn")
            self.assertTrue(export.check_golden(gbnn)[0])
            self.assertEqual(gbai.read_gbnn(gbnn).out_names, ["alive_hold", "alive_release"])
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
                                    "--reader-epochs", "2", "--threads", "2", "--levels", "10565740,14"],
                                   log=quiet, game_factory=factory)
            recorded = [r for r in out["record"] if r["state"] == "done"]
            self.assertEqual(len(recorded), 7)  # six to train on and the bar level; not the unused one
            self.assertEqual(gbai.read_json(os.path.join(root, "train", "layout.json"))["obs_layout_hash"], TOY_HASH)
            self.assertEqual(out["train"]["pilot"]["state"], "done")
            self.assertEqual(out["train"]["reader"]["state"], "done")
            self.assertNotIn("critic", out["train"])  # no practice attempts yet
            installed = {i["model"]: i for i in out["install"]}
            self.assertEqual(set(installed), {"pilot", "reader"})
            self.assertEqual(installed["reader"]["layout_hash"], FakeGame.TAPE_HASH)
            for name in ("pilot", "reader"):
                with open(os.path.join(root, "weights", f"{name}.gbnn"), "rb") as a, \
                        open(os.path.join(root, "train", name, f"{name}.gbnn"), "rb") as b:
                    self.assertEqual(a.read(), b.read())
            split = gbai.read_json(os.path.join(root, "train", "pilot", "split.json"))
            self.assertIn(10565740, {r["level_id"] for r in split["showcase_excluded"]})
            play = out["play"]
            self.assertEqual(sorted(r["level_id"] for r in play["results"]), [14, 10565740])
            self.assertTrue(all(r["state"] == "done" for r in play["results"]))
            opened = [args for tool, args in games[-1].calls if tool == "gucci_open_level"]
            self.assertEqual(sorted(opened, key=str), sorted([{"id": 10565740}, {"main": 14}], key=str))
            reports = os.listdir(os.path.join(root, "train", "reports"))
            self.assertTrue(any(n.startswith("play_") and n.endswith(".md") for n in reports))
            # Recording again skips what's there.
            again = orchestrate.main(["record", "--root", root, "--manifest", m], log=quiet,
                                     game_factory=lambda: FakeGame(root))
            self.assertTrue(all(r["state"] == "skipped" for r in again["record"]))

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


if __name__ == "__main__":
    unittest.main()

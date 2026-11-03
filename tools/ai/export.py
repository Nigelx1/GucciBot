"""Trained models -> GBNN files (contract v2 s7), the reference forward pass, and the golden set.

forward() is a pure-numpy forward pass over a GBNN, written the way the mod's
C++ loop runs it (normalize, then each layer, then its activation; conv1d with
zeros outside [0, C)). It must match PyTorch to 1e-5: export_model() checks
that on every export, and test_ai.py checks it for all three model kinds.
"1e-5" is relative once an output is bigger than 1 (|a - b| / max(1, |b|)):
PyTorch computes in float32, whose own rounding on a logit of 70 is ~8e-6,
so a flat 1e-5 would fail good exports of confident models.

Every export also writes golden vectors next to the file: <model>.golden.json
with a few raw inputs and the outputs forward() gives (in float64, the exact
answer for the float32 weights). The C++ side loads the .gbnn, runs the same
inputs and compares within the tolerance written in the file.

Golden layout: an MLP case is {"input": [n_in], "output": [n_out]}. A conv1d
case is {"columns": C, "input": [C][n_in], "output": [C][n_out]}, rows per
column like the tape file. Inputs are raw (before mean/std).

All three models output what the file computes with no final activation in
v2: the pilot's hold_logit (hold when > 0, ties release), the critic's two
logits, the reader's 11 columns.

`golden` writes tools/ai/golden/ (make_golden): small pilot, critic and
reader GBNN files with inputs, float64 outputs and decisions, plus a compact
record and its 472-float expansion. The mod's offline C++ test
(tools/run_ai_net_tests.cpp, stage C) loads them; golden.json describes every
file.

    python tools/ai/export.py show FILE.gbnn
    python tools/ai/export.py check FILE.gbnn            # forward() vs FILE.golden.json
    python tools/ai/export.py from-checkpoint CKPT.pt OUT.gbnn --layout-hash HASH
    python tools/ai/export.py golden [--out DIR]         # make the golden set (default tools/ai/golden)
    python tools/ai/export.py check-golden [--out DIR]   # check it as the C++ test will
"""
import argparse
import json
import os
import sys

import numpy as np

import gbai

GOLDEN_TOLERANCE = {"abs": 1e-4, "rel": 1e-4}


def _activate(v, code):
    if code == gbai.ACT_RELU:
        return np.maximum(v, 0)
    if code == gbai.ACT_SIGMOID:
        return 0.5 * (1.0 + np.tanh(0.5 * v))  # 1/(1+e^-v), without overflow warnings
    if code == gbai.ACT_TANH:
        return np.tanh(v)
    return v


def _conv_same(h, w, b, dilation):
    """h [I][C], w [O][I][K], b [O] -> [O][C], zeros outside [0, C) (the contract's loop)."""
    _, C = h.shape
    O, _, K = w.shape
    out = np.empty((O, C), h.dtype)
    out[:] = b[:, None]
    half = (K - 1) // 2
    for k in range(K):
        s = (k - half) * dilation  # out[t] reads h[t + s]
        if abs(s) >= C:
            continue
        shifted = np.zeros_like(h)
        if s >= 0:
            shifted[:, :C - s] = h[:, s:]
        else:
            shifted[:, -s:] = h[:, :C + s]
        out += w[:, :, k] @ shifted
    return out


def forward(net, x, dtype=np.float64):
    """The GBNN's output for raw input x, computed as the mod's C++ loop does.

    MLP: x [n_in] or [B][n_in] -> [n_out] or [B][n_out].
    conv1d stack: x [C][n_in] (the tape's layout) -> [C][n_out].
    dtype float64 (default) is the reference; float32 is what the mod computes.
    """
    x = np.asarray(x, dtype)
    mean = net.mean.astype(dtype)
    std = net.std.astype(dtype)
    if net.kind == gbai.KIND_MLP:
        single = x.ndim == 1
        h = (np.atleast_2d(x) - mean) / std
        for L in net.layers:
            h = _activate(h @ L.weight[:, :, 0].astype(dtype).T + L.bias.astype(dtype), L.activation)
        return h[0] if single else h
    h = ((x - mean) / std).T  # [n_in][C]
    for L in net.layers:
        h = _activate(_conv_same(h, L.weight.astype(dtype), L.bias.astype(dtype), L.dilation), L.activation)
    return h.T


def forward_batched(net, x, chunk=65536, dtype=np.float32):
    """forward() of an MLP over many rows (float16/float32), in chunks, as float32."""
    out = np.empty((len(x), net.n_out), np.float32)
    for a in range(0, len(x), chunk):
        out[a:a + chunk] = forward(net, np.asarray(x[a:a + chunk], dtype), dtype)
    return out


def sigmoid(v):
    return 0.5 * (1.0 + np.tanh(0.5 * np.asarray(v, np.float64)))


def to_gbnn(model, layout_hash):
    """A trained model (models.py) as a GBNN."""
    return gbai.GBNN(model.gbnn_kind, layout_hash, model.in_mean.detach().cpu().numpy(),
                     model.in_std.detach().cpu().numpy(), model.gbnn_layers(), model.out_names)


def torch_outputs(model, inputs):
    """model.export_forward() on golden-style inputs (MLP rows [B][n_in], or a list of [C][n_in])."""
    import torch
    model.eval()
    with torch.no_grad():
        if model.gbnn_kind == gbai.KIND_MLP:
            return model.export_forward(torch.as_tensor(np.asarray(inputs, np.float32))).numpy().astype(np.float64)
        return [model.export_forward(torch.as_tensor(np.asarray(seq, np.float32).T[None]))[0].numpy().T
                .astype(np.float64) for seq in inputs]


def _scaled_error(a, b):
    return float(np.max(np.abs(a - b) / np.maximum(1.0, np.abs(a)))) if np.size(a) else 0.0


def compare(net, model, inputs):
    """Largest |numpy forward - torch| / max(1, |numpy forward|) over the inputs (see the module docstring)."""
    ref = torch_outputs(model, inputs)
    if net.kind == gbai.KIND_MLP:
        return _scaled_error(forward(net, inputs), ref) if len(inputs) else 0.0
    return max((_scaled_error(forward(net, seq), r) for seq, r in zip(inputs, ref)), default=0.0)


def golden_cases(net, inputs):
    """[{"input", "output"}]: raw inputs and forward()'s float64 outputs."""
    cases = []
    if net.kind == gbai.KIND_MLP:
        x = np.asarray(inputs, np.float32)
        y = forward(net, x)
        for xi, yi in zip(x, y):
            cases.append({"input": [float(v) for v in xi], "output": [float(v) for v in yi]})
    else:
        for seq in inputs:
            x = np.asarray(seq, np.float32)
            y = forward(net, x)
            cases.append({"columns": int(len(x)), "input": x.astype(np.float64).tolist(), "output": y.tolist()})
    return cases


def golden_path(gbnn_path):
    """<model>.gbnn -> <model>.golden.json, next to it."""
    base = gbnn_path[:-5] if gbnn_path.lower().endswith(".gbnn") else gbnn_path
    return base + ".golden.json"


def write_golden(net, gbnn_path, inputs, model_name, note=""):
    cases = golden_cases(net, inputs)
    # How far float32 arithmetic (what the mod runs) lands from the float64 answer here.
    f32_err = 0.0
    for c in cases:
        got = forward(net, np.asarray(c["input"], np.float32), np.float32)
        f32_err = max(f32_err, float(np.max(np.abs(got - np.asarray(c["output"])))))
    doc = {"format": "gbnn-golden", "version": 1, "model": model_name, "gbnn_file": os.path.basename(gbnn_path),
           "gbnn_sha256": gbai.sha256_file(gbnn_path), "kind": net.kind, "obs_layout_hash": net.layout_hash,
           "n_in": net.n_in, "n_out": net.n_out, "out_names": net.out_names, "tolerance": GOLDEN_TOLERANCE,
           "float32_max_abs_error_seen": f32_err,
           "notes": ("Inputs are raw (the file's mean/std are applied first). MLP: input [n_in], output [n_out]. "
                     "conv1d: input [columns][n_in] like the tape file, output [columns][n_out]; zeros outside "
                     "[0, columns) at every layer. Outputs are float64 forward passes of the float32 weights; "
                     "a float32 C++ forward must land within tolerance (|got - want| <= abs + rel * |want|). "
                     + note),
           "cases": cases}
    path = golden_path(gbnn_path)
    gbai.write_json_atomic(path, doc)
    return path


def check_golden(gbnn_path, golden=None, dtype=np.float32):
    """Runs forward() (float32 by default, like the mod) on a golden file's inputs.
    Returns (ok, worst |error| beyond the tolerance check, cases)."""
    net = gbai.read_gbnn(gbnn_path)
    doc = gbai.read_json(golden or golden_path(gbnn_path))
    tol = doc.get("tolerance", GOLDEN_TOLERANCE)
    ok, worst = True, 0.0
    for c in doc["cases"]:
        got = forward(net, np.asarray(c["input"], np.float32), dtype)
        want = np.asarray(c["output"], np.float64)
        err = np.abs(got - want)
        worst = max(worst, float(err.max()) if err.size else 0.0)
        if (err > tol["abs"] + tol["rel"] * np.abs(want)).any():
            ok = False
    return ok, worst, len(doc["cases"])


def export_model(model, layout_hash, gbnn_path, golden_inputs, atol=1e-5, note=""):
    """Writes <model>.gbnn and <model>.golden.json; returns a summary dict.

    Checks before returning: the file reads back to the same bytes, and the
    numpy forward pass of the file matches PyTorch within atol on the golden
    inputs (raises otherwise: a file the mod would run differently is no use).
    """
    net = to_gbnn(model, layout_hash)
    data = gbai.write_gbnn(gbnn_path, net)
    back = gbai.read_gbnn(gbnn_path)
    if gbai.gbnn_bytes(back) != data:
        raise RuntimeError(f"{gbnn_path}: reading the file back gave different bytes")
    err = compare(back, model, golden_inputs)
    if not err <= atol:
        raise RuntimeError(f"{gbnn_path}: the numpy forward pass is {err:.3g} away from PyTorch (allowed {atol})")
    gpath = write_golden(back, gbnn_path, golden_inputs, model.model_kind, note)
    return {"gbnn": gbnn_path, "golden": gpath, "sha256": gbai.sha256_file(gbnn_path), "bytes": len(data),
            "max_err_vs_torch": err, "layout_hash": layout_hash}


# ---- the golden set for the mod's offline test (stage C) ---------------------------

GOLDEN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden")
GOLDEN_HASH = "a1b2c3d4e5f60718"  # what the golden models carry; the C++ test passes it as the expected hash
GOLDEN_MARGIN = 1e-3  # no golden decision sits closer than this to its boundary


def golden_record(G=16, seed=7):
    """A compact record (contract v2 s3) built to hit every corner of the expansion: scan bytes 0, 1,
    95, 96 and 255; every token type 1..31; empty slots; negative, zero and -0.0 dx/dy; -0.0 and
    large state floats. Returns (state float32 [G][40], scan uint8 [G][160], tok uint8 [G][48])."""
    rng = np.random.default_rng(seed)
    state = np.zeros((G, gbai.STATE_DIM), np.float32)
    for g in range(G):
        state[g, g % 8] = 1.0  # mode one-hot
        state[g, gbai.STATE_SPEED0 + g % 5] = 1.0
    state[:, 8] = rng.integers(0, 2, G)  # gravity flipped
    state[:, 9] = rng.integers(0, 2, G)  # mini
    state[:, 15] = rng.normal(0, 6, G)  # y velocity
    state[:, 16:19] = rng.integers(0, 2, (G, 3))  # on ground, on ceiling, held before
    state[:, 19:21] = np.log1p(rng.integers(0, 5000, (G, 2))) / 5
    state[:, 21:23] = rng.uniform(0, 15, (G, 2))
    state[:, 23] = np.float32(60.0) / rng.choice([60.0, 240.0, 360.0, 480.0], G).astype(np.float32)
    state[::3, 24] = 1.0  # dual on some ticks
    state[::3, 25:31] = rng.normal(0, 2, (len(state[::3]), 6))
    for g in range(G):
        if g % 4 == 1:
            state[g, 32 + g % 8] = 1.0  # touching an orb
    state[0, 15] = np.float32(-0.0)
    state[1, 21] = np.float32(3.0e6)
    scan = rng.integers(0, gbai.SCAN_MAX + 1, (G, gbai.SCAN_DIM)).astype(np.uint8)
    scan[rng.random((G, gbai.SCAN_DIM)) < 0.35] = gbai.SCAN_NONE
    scan[0, :4] = [0, 1, gbai.SCAN_MAX - 1, gbai.SCAN_MAX]
    scan[1, :] = gbai.SCAN_NONE
    tok = np.zeros((G, gbai.TOKENS, gbai.TOKEN_BYTES), np.uint8)
    types = list(range(1, gbai.TOKEN_TYPES))
    next_type = 0
    for g in range(G):
        used = int(rng.integers(0, gbai.TOKENS + 1)) if g > 1 else (gbai.TOKENS if g == 0 else 0)
        dx = np.sort(rng.uniform(-1, 20, used)).astype(np.float16)
        dy = rng.uniform(-10, 10, used).astype(np.float16)
        for s in range(used):
            pair = np.array([dx[s], dy[s]], "<f2").view(np.uint8)
            tok[g, s, :4] = pair
            tok[g, s, 4] = types[next_type % len(types)]
            next_type += 1
    # Exact and signed values the conversion must keep: -0.0, 0, -1, 19.75.
    tok[0, 0, :4] = np.array([np.float16(-0.0), np.float16(0.0)], "<f2").view(np.uint8)
    tok[0, 1, :4] = np.array([np.float16(-1.0), np.float16(19.75)], "<f2").view(np.uint8)
    return state, scan, tok.reshape(G, gbai.TOK_DIM)


def _away_from_boundary(values, margin=GOLDEN_MARGIN):
    return np.abs(np.asarray(values, np.float64)) >= margin


def make_golden(out_dir=None, seed=0, log=print):
    """Writes the golden set stage C's C++ test checks (tools/ai/golden/ by default):

    record.state.f32 [G][40], record.scan.u8 [G][160], record.tok.u8 [G][48]: a compact record;
    record.expanded.f32 [G][472]: its expansion (gbai.expand_np; the C++ expansion must match
    bit for bit). pilot/critic/reader.gbnn: small random-init models with layout hash
    GOLDEN_HASH. <model>.in.f32 and <model>.out.f64: raw inputs and the float64 forward pass of
    the float32 weights (the C++ float32 forward must land within 1e-5); pilot.dec.u8 (1 = hold:
    hold_logit > 0) and critic.dec.u8 (1 = alive_hold > alive_release): every one at least
    GOLDEN_MARGIN from its boundary, so the decisions must be identical. reader.case<i>.in.f32
    [C][K] / .out.f64 [C][11], one with fewer columns than the receptive field. golden.json says
    all of it. Also <model>.golden.json, the trainer's own check format.
    """
    import torch
    import models
    out_dir = os.path.abspath(out_dir or GOLDEN_DIR)
    os.makedirs(out_dir, exist_ok=True)
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    files = {}

    def put(name, arr, dtype):
        a = np.ascontiguousarray(arr, dtype)
        a.tofile(os.path.join(out_dir, name))
        files[name] = {"dtype": np.dtype(dtype).name, "shape": list(a.shape), "bytes": int(a.nbytes),
                       "sha256": gbai.sha256_file(os.path.join(out_dir, name))}

    state, scan, tok = golden_record()
    expanded = gbai.expand_np(state, scan, tok)
    put("record.state.f32", state, "<f4")
    put("record.scan.u8", scan, "<u1")
    put("record.tok.u8", tok, "<u1")
    put("record.expanded.f32", expanded, "<f4")

    # MLP inputs: the golden record's expanded rows, then random rows around them.
    extra = expanded[rng.integers(0, len(expanded), 16)] + rng.normal(0, 0.5, (16, gbai.OBS_DIM)).astype(np.float32)
    x_all = np.concatenate([expanded, extra]).astype(np.float32)
    mean = x_all.mean(0)
    std = np.where(x_all.std(0) > 1e-3, x_all.std(0), 1.0)
    summary = {}
    for kind, cls in (("pilot", models.Pilot), ("critic", models.Critic)):
        model = cls(gbai.OBS_DIM, (32, 32))
        model.set_norm(mean, std)
        model.eval()
        net = gbai.parse_gbnn(gbai.gbnn_bytes(to_gbnn(model, GOLDEN_HASH)))
        y = forward(net, x_all)
        # Center the decisions (a random init holds on every row, or never): about half each way.
        with torch.no_grad():
            if kind == "pilot":
                model.layers[-1].bias[0] -= float(np.median(y[:, 0]))
            else:
                model.layers[-1].bias[0] -= float(np.median(y[:, 0] - y[:, 1]))
        net = gbai.parse_gbnn(gbai.gbnn_bytes(to_gbnn(model, GOLDEN_HASH)))
        y = forward(net, x_all)
        dec_value = y[:, 0] if kind == "pilot" else y[:, 0] - y[:, 1]
        keep = _away_from_boundary(dec_value)
        x, y, dec_value = x_all[keep], y[keep], dec_value[keep]
        gb = os.path.join(out_dir, f"{kind}.gbnn")
        info = export_model(model, GOLDEN_HASH, gb, x)
        files[f"{kind}.gbnn"] = {"bytes": info["bytes"], "sha256": info["sha256"]}
        put(f"{kind}.in.f32", x, "<f4")
        put(f"{kind}.out.f64", y, "<f8")
        put(f"{kind}.dec.u8", (dec_value > 0).astype(np.uint8), "<u1")
        summary[kind] = {"cases": int(len(x)), "n_in": net.n_in, "n_out": net.n_out, "out_names": net.out_names,
                         "holds": int((dec_value > 0).sum()), "max_err_vs_torch": info["max_err_vs_torch"]}

    K = 24
    reader = models.Reader(K, 8, 5, (1, 2, 4))
    tapes = [rng.integers(0, 4, (c, K)).astype(np.float32) for c in (1, 5, 40)]
    tapes[2][:, :4] = rng.integers(0, 256, (40, 4))  # count channels with large values
    pool = np.concatenate(tapes)
    reader.set_norm(pool.mean(0), np.where(pool.std(0) > 1e-3, pool.std(0), 1.0))
    reader.eval()
    info = export_model(reader, GOLDEN_HASH, os.path.join(out_dir, "reader.gbnn"), tapes)
    files["reader.gbnn"] = {"bytes": info["bytes"], "sha256": info["sha256"]}
    net = gbai.read_gbnn(os.path.join(out_dir, "reader.gbnn"))
    for i, t in enumerate(tapes):
        put(f"reader.case{i}.in.f32", t, "<f4")
        put(f"reader.case{i}.out.f64", forward(net, t), "<f8")
    summary["reader"] = {"cases": len(tapes), "columns": [len(t) for t in tapes], "n_in": K, "n_out": net.n_out,
                         "out_names": net.out_names, "receptive_field": reader.receptive_field(),
                         "max_err_vs_torch": info["max_err_vs_torch"]}

    for kind in ("pilot", "critic", "reader"):  # the trainer's own JSON copies; golden.json + .f32/.f64 say it all
        try:
            os.remove(golden_path(os.path.join(out_dir, f"{kind}.gbnn")))
        except FileNotFoundError:
            pass
    doc = {"format": "gbai-golden", "version": 2, "contract": "AI pilot contract v2", "layout_hash": GOLDEN_HASH,
           "tolerance_abs": 1e-5, "decision_margin": GOLDEN_MARGIN, "obs_dim": gbai.OBS_DIM,
           "record": {"rows": int(len(state)), "expansion": "[state 40 as-is][scan 160: v == 255 ? 12.5 : v / 8.0]"
                      "[8 tokens: dx, dy (float16 -> float32), one-hot(type, 32), index 0 unused; type 0 = all zeros]",
                      "check": "expand(record) == record.expanded.f32 bit for bit"},
           "models": summary,
           "rules": {"pilot": "hold when hold_logit > 0 (ties release); pilot.dec.u8 1 = hold",
                     "critic": "two logits, no sigmoid; critic.dec.u8 1 = alive_hold > alive_release",
                     "reader": "conv1d same padding with dilation, zeros outside [0, C) in normalized space and at "
                               "every layer; outputs y_rel, 8 mode logits, gravity_logit, hold_frac (unclamped)",
                     "inputs": "raw (before the file's mean/std); MLP in [cases][n_in], out [cases][n_out]; reader "
                               "in [C][K], out [C][11]",
                     "compare": "|C++ float32 - out.f64| <= tolerance_abs for every value; decisions identical"},
           "files": files, "made_by": "python tools/ai/export.py golden", "seed": seed}
    gbai.write_json_atomic(os.path.join(out_dir, "golden.json"), doc)
    log(f"golden set written to {out_dir}: " + ", ".join(f"{k} {v['cases']} cases" for k, v in summary.items()))
    return doc


def check_golden_dir(out_dir=None):
    """Re-reads a golden folder and checks it the way the C++ test will (numpy float32 forward, the
    record's expansion bit for bit, identical decisions). Returns a list of problems (empty: fine)."""
    out_dir = os.path.abspath(out_dir or GOLDEN_DIR)
    doc = gbai.read_json(os.path.join(out_dir, "golden.json"))
    problems = []

    def load(name):
        f = doc["files"][name]
        return np.fromfile(os.path.join(out_dir, name), np.dtype(f["dtype"]).newbyteorder("<")).reshape(f["shape"])

    exp = gbai.expand_np(load("record.state.f32"), load("record.scan.u8"), load("record.tok.u8"))
    if not np.array_equal(exp.view(np.uint32), load("record.expanded.f32").view(np.uint32)):
        problems.append("record expansion differs from record.expanded.f32")
    tol = doc["tolerance_abs"]
    for kind in ("pilot", "critic"):
        net = gbai.read_gbnn(os.path.join(out_dir, f"{kind}.gbnn"))
        if net.layout_hash != doc["layout_hash"]:
            problems.append(f"{kind}.gbnn layout hash {net.layout_hash}")
        got = forward(net, load(f"{kind}.in.f32"), np.float32)
        want = load(f"{kind}.out.f64")
        err = float(np.max(np.abs(got - want)))
        if err > tol:
            problems.append(f"{kind}: float32 forward is {err:.3g} from out.f64")
        dec = (got[:, 0] > 0) if kind == "pilot" else (got[:, 0] > got[:, 1])
        if not np.array_equal(dec.astype(np.uint8), load(f"{kind}.dec.u8")):
            problems.append(f"{kind}: decisions differ")
    net = gbai.read_gbnn(os.path.join(out_dir, "reader.gbnn"))
    for i in range(doc["models"]["reader"]["cases"]):
        got = forward(net, load(f"reader.case{i}.in.f32"), np.float32)
        err = float(np.max(np.abs(got - load(f"reader.case{i}.out.f64"))))
        if err > tol:
            problems.append(f"reader case {i}: float32 forward is {err:.3g} from out.f64")
    for name, f in doc["files"].items():
        if gbai.sha256_file(os.path.join(out_dir, name)) != f["sha256"]:
            problems.append(f"{name}: sha256 differs from golden.json")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description="GBNN files: show, check against golden vectors, export.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("golden")
    p.add_argument("--out", help="default: tools/ai/golden")
    p.add_argument("--seed", type=int, default=0)
    p = sub.add_parser("check-golden")
    p.add_argument("--out", help="default: tools/ai/golden")
    p = sub.add_parser("show")
    p.add_argument("gbnn")
    p = sub.add_parser("check")
    p.add_argument("gbnn")
    p.add_argument("--golden")
    p = sub.add_parser("from-checkpoint")
    p.add_argument("checkpoint")
    p.add_argument("out")
    p.add_argument("--layout-hash", help="default: the one stored in the checkpoint")
    args = ap.parse_args(argv)
    if args.cmd == "golden":
        make_golden(args.out, args.seed)
        problems = check_golden_dir(args.out)
        for p in problems:
            print(f"FAIL {p}")
        return 1 if problems else 0
    if args.cmd == "check-golden":
        problems = check_golden_dir(args.out)
        print(json.dumps({"ok": not problems, "problems": problems}))
        return 1 if problems else 0
    if args.cmd == "show":
        print(gbai.read_gbnn(args.gbnn).describe())
        return 0
    if args.cmd == "check":
        ok, worst, n = check_golden(args.gbnn, args.golden)
        print(json.dumps({"ok": ok, "cases": n, "max_abs_error_float32": worst}))
        return 0 if ok else 1
    import models
    model, ck = models.from_checkpoint(args.checkpoint)
    layout_hash = args.layout_hash or ck.get("layout_hash")
    if not layout_hash:
        raise SystemExit("no layout hash in the checkpoint: pass --layout-hash")
    rng = np.random.default_rng(0)
    mean = model.in_mean.numpy()
    std = model.in_std.numpy()
    if model.gbnn_kind == gbai.KIND_MLP:
        inputs = (mean + std * rng.standard_normal((4, model.n_in))).astype(np.float32)
    else:
        inputs = [(mean + std * rng.standard_normal((c, model.n_in))).astype(np.float32) for c in (5, 64)]
    print(json.dumps(export_model(model, layout_hash, args.out, inputs), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

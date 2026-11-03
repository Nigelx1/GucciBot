"""Trained models -> GBNN files (contract s5), and the reference forward pass.

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

    python tools/ai/export.py show FILE.gbnn
    python tools/ai/export.py check FILE.gbnn            # forward() vs FILE.golden.json
    python tools/ai/export.py from-checkpoint CKPT.pt OUT.gbnn --layout-hash HASH
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


def main(argv=None):
    ap = argparse.ArgumentParser(description="GBNN files: show, check against golden vectors, export.")
    sub = ap.add_subparsers(dest="cmd", required=True)
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

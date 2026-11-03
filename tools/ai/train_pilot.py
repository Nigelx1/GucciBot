"""Behavior cloning: the pilot learns from human runs what a human holds each tick.

Data: the human runs in AI_ROOT\\data, all recorded with one observation
layout (--layout-hash, or the only one there is). The showcase levels
(gbai.SHOWCASE_LEVELS, plus any manifest is_bar_level entry) are never
trained on, nor used to pick the best epoch: they're for evaluation only.
The rest is split by level, never by tick: --fold k/K holds out one fold of
the levels (K = the number of levels is leave-one-out); without it about
--val-frac of the levels validate (0 trains on all of them).

A run that died loses its last second (gbai.Run.usable_ticks). Loss: binary
cross-entropy on act[t] (the human's button during tick t, from obs[t]),
class balanced, and ticks within +-8 of a human press or release weigh 5x:
when the button changes is what matters. The class balance goes by the
button going INTO each tick (act[t-1]), not by the label: the time spent
holding and the time spent released weigh the same in total, and the ticks
on both sides of one press (or release) get the same multiplier, so the
balance can't move where the pilot presses. (Balancing by the label made a
release tick weigh ~1/64 of the held ticks just before it, and every release
came late.) On top of that, the human's press and release ticks themselves
weigh --change-weight (8) times more: while the button is released the only
"press" examples are the press ticks, about one in 200, so without it the
pilot is rarely sure enough to change the button and presses late (the
usual copycat trap of cloning with the held button as an input). The input
mean/std come from the training split and go into the GBNN header.

Every epoch: a checkpoint (last.pt, and best.pt) and progress.json, which
another process can poll (state, epoch, losses, validation numbers, eta).
"Best" is by validation timing by default (--select timing: the share of
presses and releases within 2 ticks, minus 0.25 per false transition per
second): on the toy data the validation loss kept rising while the timing
kept improving, because a few confidently wrong ticks dominate the loss. Run it again with the same data and it carries on
from last.pt; --fresh starts over. At the end it exports pilot.gbnn (+
pilot.golden.json) from the best epoch and writes eval_report.json/.md
(evaluate.py) for every held-out run.

    python tools/ai/train_pilot.py [--root DIR] [--fold 0/5] [--epochs 8] [--threads 6]

Outputs go to AI_ROOT\\train\\pilot\\ (or --out): progress.json, split.json,
last.pt, best.pt, pilot.gbnn, pilot.golden.json, metrics.json, eval_report.*.
"""
import argparse
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

import evaluate
import export
import gbai
import models


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Train the pilot on human runs (behavior cloning).")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--out", help="output folder (default: AI_ROOT\\train\\pilot)")
    ap.add_argument("--layout-hash", help="train on runs with this obs layout only (gucci_ai_layout gives the mod's)")
    ap.add_argument("--fold", help="k/K: hold out fold k of K, split by level")
    ap.add_argument("--val-frac", type=float, default=0.15, help="without --fold: share of levels to validate on")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-4)
    ap.add_argument("--hidden", default="256,256", help="hidden layer sizes")
    ap.add_argument("--near-ticks", type=int, default=8, help="ticks either side of a press/release that weigh more")
    ap.add_argument("--near-weight", type=float, default=5.0, help="how much more they weigh")
    ap.add_argument("--change-weight", type=float, default=8.0,
                    help="how much more the human's press and release ticks themselves weigh")
    ap.add_argument("--far-keep", type=float, default=1.0,
                    help="share of the other ticks each epoch samples (weighted back up); 1 = all")
    ap.add_argument("--threshold", type=float, default=0.5, help="hold when P >= this (validation numbers)")
    ap.add_argument("--select", choices=["timing", "loss", "last"], default="timing",
                    help="which epoch to export: best validation timing, lowest validation loss, or the last")
    ap.add_argument("--threads", type=int, default=6, help="CPU threads (6 = one per core on Nigel's PC)")
    ap.add_argument("--manifest", help="a manifest whose is_bar_level entries are showcase levels too")
    ap.add_argument("--exclude", action="append", default=[],
                    help="another level to leave out of training, by id or name (repeatable)")
    ap.add_argument("--ema", type=int, default=-1,
                    help="export an average of the weights over this many steps (-1: half an epoch; 0: off)")
    ap.add_argument("--fresh", action="store_true", help="ignore last.pt and start over")
    ap.add_argument("--no-eval", action="store_true", help="skip the held-out report at the end")
    args = ap.parse_args(argv)
    args.fold_k = gbai.parse_fold(args.fold) if args.fold else None
    if not 0 < args.far_keep <= 1:
        ap.error("--far-keep is in (0, 1]")
    return args


def showcase_from_args(args):
    ids = [int(e) for e in args.exclude if str(e).strip().isdigit()]
    names = [e for e in args.exclude if not str(e).strip().isdigit()]
    return gbai.Showcase(extra_ids=ids, extra_names=names, manifest=args.manifest)


def assemble(runs, near_ticks):
    """Every training tick in RAM: obs float16 [N][obs_dim], act uint8 [N], the button going into
    each tick uint8 [N] (act[t-1]), near bool [N]; and how many obs values weren't finite.
    A press or release is where act differs from the button going in."""
    sizes = [r.usable_ticks() for r in runs]
    total = sum(sizes)
    dim = runs[0].obs_dim
    # Random batches need the rows in RAM (from a memmap they're ~50x slower: TOOLCHAIN.md 4.3).
    # That's dim * 2 bytes a tick: ~3.6 GB for the 4.2M human ticks at ~430 inputs.
    try:
        X = np.empty((total, dim), np.float16)
    except MemoryError:
        raise gbai.DataError(f"{total} training ticks x {dim} inputs need {total * dim * 2 / 2**30:.1f} GiB of free "
                             "RAM, which isn't there (close what's using it; OneDrive is often the biggest)")
    y = np.empty(total, np.uint8)
    hb = np.empty(total, np.uint8)
    near = np.empty(total, bool)
    pos = 0
    for r, n in zip(runs, sizes):
        if n:
            X[pos:pos + n] = r.obs[:n]
            a = np.asarray(r.act)
            y[pos:pos + n] = a[:n]
            hb[pos:pos + n] = gbai.before(a)[:n]
            near[pos:pos + n] = gbai.near_mask(a, near_ticks)[:n]
            pos += n
        r.close()
    bad = 0
    for a in range(0, total, 65536):
        bad += gbai.finite_rows(X[a:a + 65536])
    return X, y, hb, near, bad


def class_balance(y, hb):
    """Weights by the button going into each tick, so held and released time weigh the same in total."""
    n1 = int(y.sum())
    if n1 == 0 or n1 == len(y):
        raise gbai.DataError("the training runs never " + ("hold" if n1 == 0 else "release") + " the button")
    h1 = int(hb.sum())
    h1 = min(max(h1, 1), len(hb) - 1)
    return np.array([len(hb) / (2.0 * (len(hb) - h1)), len(hb) / (2.0 * h1)], np.float64)


def tick_weights(y, hb, near, cb, args, norm=1.0):
    """Per-tick loss weights: class balance by the button going in, x near_weight within
    +-near_ticks of a press/release, x change_weight on the press/release tick itself."""
    w = cb[hb.astype(np.int64)] * np.where(near, args.near_weight, 1.0)
    w = w * np.where(y != hb, args.change_weight, 1.0)
    return (w / norm).astype(np.float32)


def predict_logits(model, obs, chunk=65536):
    """Logits per row; values that aren't finite count as 0, as in training (gbai.clean_rows)."""
    out = np.empty(len(obs), np.float32)
    with torch.no_grad():
        for a in range(0, len(obs), chunk):
            out[a:a + chunk] = model(torch.from_numpy(gbai.clean_rows(obs[a:a + chunk]))).numpy()
    return out


def validate(model, runs, cb, w_norm, args):
    """(weighted validation loss, evaluate.rates of the pooled counts) over the validation runs."""
    model.eval()
    counts = evaluate.empty_counts()
    loss_sum, n_sum = 0.0, 0
    for r in runs:
        n = r.usable_ticks()
        if n == 0:
            continue
        a = np.asarray(r.act)
        yv = a[:n]
        logits = predict_logits(model, r.obs[:n])
        np.nan_to_num(logits, copy=False)
        w = tick_weights(yv, gbai.before(a)[:n], gbai.near_mask(a, args.near_ticks)[:n], cb, args, w_norm)
        lt = torch.from_numpy(logits)
        loss = F.binary_cross_entropy_with_logits(lt, torch.from_numpy(yv.astype(np.float32)), reduction="none")
        loss_sum += float((loss.numpy() * w).sum())
        n_sum += n
        prob = export.sigmoid(logits)
        counts = evaluate.add_counts(counts, evaluate.transition_metrics(yv, prob >= args.threshold, r.tps, prob))
        r.close()
    return (loss_sum / n_sum if n_sum else None), (evaluate.rates(counts) if n_sum else None)


def timing_score(val):
    """One number for picking an epoch: mean of press/release within 2 ticks, minus 0.25 per false
    transition per second (None without validation numbers)."""
    if not val:
        return None
    parts = [v for v in (val["press_within_2"], val["release_within_2"]) if v is not None]
    if not parts:
        return None
    return sum(parts) / len(parts) - 0.25 * (val["false_per_sec"] or 0.0)


def golden_rows(runs, near_ticks, n=6, seed=0):
    """A few raw obs rows for the golden file: some at human presses/releases, some anywhere."""
    rng = np.random.default_rng(seed)
    rows = []
    for r in runs:
        u = r.usable_ticks()
        if u == 0:
            continue
        tr = gbai.transitions(np.asarray(r.act[:u]))
        picks = list(tr[:n // 2]) + list(rng.integers(0, u, n - min(len(tr), n // 2)))
        rows += [np.asarray(r.obs[int(t)], np.float32) for t in picks[:n]]
        r.close()
        if len(rows) >= n:
            break
    return rows


def main(argv=None, log=print):
    args = parse_args(argv)
    models.set_threads(args.threads)
    models.set_seed(args.seed)
    root = gbai.ai_root(args.root)
    out = os.path.abspath(args.out) if args.out else gbai.train_dir(root, "pilot")
    os.makedirs(out, exist_ok=True)
    prog = gbai.Progress(os.path.join(out, "progress.json"), "pilot", out)
    try:
        result = train(args, root, out, prog, log)
    except BaseException as e:
        prog.finish("failed", f"{type(e).__name__}: {e}")
        raise
    prog.finish("done", "trained and exported", exported=result["export"]["gbnn"])
    return result


def train(args, root, out, prog, log):
    prog.set(state="loading", message="reading runs", force=True)
    runs, problems = gbai.list_runs(root, "human")
    for _, why in problems:
        log(f"skipped (doesn't follow the contract): {why}")
    if not runs:
        raise gbai.DataError(f"no human runs in {os.path.join(root, 'data')}")
    runs, layout_hash = gbai.pick_layout(runs, args.layout_hash)
    showcase = showcase_from_args(args)
    split = gbai.make_split(runs, showcase, args.fold_k, args.val_frac, args.seed,
                            usable=lambda r: r.usable_ticks() > 0)
    record = gbai.split_record(split, "pilot", layout_hash, args.fold_k, args.val_frac, args.seed)
    gbai.write_json_atomic(os.path.join(out, "split.json"), record)
    for r, why in split["excluded"]:
        log(f"left out of training: {r.run_id}: {why}")
    for r in split["unusable"]:
        log(f"not used: {r.run_id} has no usable ticks (empty, or it died in its first second)")
    train_runs, val_runs = split["train"], split["val"]
    if not train_runs:
        raise gbai.DataError("no runs left to train on once the showcase levels and the runs with no usable ticks "
                             "are left out")
    log(f"{len(train_runs)} training runs ({len(split['train_levels'])} levels), {len(val_runs)} validation runs "
        f"({len(split['val_levels'])} levels), {len(split['excluded'])} showcase runs left out; layout {layout_hash}")

    X, y, hb, near, bad = assemble(train_runs, args.near_ticks)
    if bad:
        log(f"NOTE: {bad} obs values weren't finite; trained on them as 0 (a recorder bug to look at)")
    if len(X) == 0:
        raise gbai.DataError("the training runs have no usable ticks")
    dim = X.shape[1]
    mean, std = gbai.moments((X[a:a + 16384] for a in range(0, len(X), 16384)), dim)
    cb = class_balance(y, hb)
    raw_w = tick_weights(y, hb, near, cb, args)
    w_norm = float(raw_w.mean())
    w = raw_w / w_norm
    log(f"{len(X)} training ticks, {int(y.sum())} held ({100.0 * y.mean():.1f}%), {int(near.sum())} near a "
        f"press/release; {X.nbytes / 2**30:.2f} GiB in RAM")

    model = models.Pilot(dim, models.parse_ints(args.hidden))
    model.set_norm(mean, std)
    signature = {"layout_hash": layout_hash, "obs_dim": dim, "model_config": model.config(),
                 "train_runs": sorted(r.run_id for r in train_runs), "val_runs": sorted(r.run_id for r in val_runs),
                 "near_ticks": args.near_ticks, "near_weight": args.near_weight,
                 "change_weight": args.change_weight, "far_keep": args.far_keep,
                 "batch": args.batch, "seed": args.seed, "select": args.select}

    def check(m):
        val_loss, val = validate(m, val_runs, cb, w_norm, args)
        text = "" if not val else (f"press within 2 {evaluate._pct(val['press_within_2'])}, release within 2 "
                                   f"{evaluate._pct(val['release_within_2'])}, false "
                                   f"{evaluate._num(val['false_per_sec'])}/s")
        return val_loss, val, timing_score(val), text

    best, history = models.fit_rows(model, X, y.astype(np.float32)[:, None], w[:, None], args,
                                    check if val_runs else None, out, signature, layout_hash, prog, log, near)
    prog.set(state="exporting", force=True, message="writing pilot.gbnn")
    if best["epoch"] is not None:
        how = {"timing": f"timing score {best['score']}", "loss": f"validation loss {best['val_loss']}",
               "last": "the last one"}[args.select if val_runs else "last"]
        log(f"exporting epoch {best['epoch']} ({how})")
    model.eval()
    golden = golden_rows(val_runs or train_runs, args.near_ticks)
    golden.append(np.zeros(dim, np.float32))
    info = export.export_model(model, layout_hash, os.path.join(out, "pilot.gbnn"), np.stack(golden))
    log(f"wrote {info['gbnn']} ({info['bytes']} bytes, numpy vs torch {info['max_err_vs_torch']:.2g})")
    result = {"model": "pilot", "export": info, "layout_hash": layout_hash, "best": best, "history": history,
              "split": os.path.join(out, "split.json")}
    if not args.no_eval:
        prog.set(state="evaluating", force=True, message="held-out report")
        net = gbai.read_gbnn(info["gbnn"])
        all_runs, _ = gbai.list_runs(root, "human")
        report = evaluate.evaluate(net, all_runs, record, showcase, args.threshold, log=log)
        report["model"] = info["gbnn"]
        report["split"] = result["split"]
        gbai.write_json_atomic(os.path.join(out, "eval_report.json"), report)
        gbai.write_bytes_atomic(os.path.join(out, "eval_report.md"),
                                evaluate.markdown(report, info["gbnn"]).encode("utf-8"))
        result["eval"] = report["groups"]
    gbai.write_json_atomic(os.path.join(out, "metrics.json"), result)
    return result


if __name__ == "__main__":
    try:
        main()
    except (gbai.DataError, ValueError) as e:
        sys.exit(f"train_pilot: {e}")

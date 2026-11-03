"""Behavior cloning: the pilot learns from human runs what a human holds each tick.

Data: the human runs in AI_ROOT\\data (contract v2), all recorded with one
layout (--layout-hash, or the only one there is). Each tick is read compact
(state, scan, tokens, geometry: 456 bytes) into RAM and expanded to the 494 floats the
pilot sees per batch, in torch (gbai.expand_torch; the recorder's golden rows
are checked bit for bit against the same expansion when a run loads). The
held-out exam levels (tools/ai/heldout.json by level id, plus any manifest
is_bar_level / held_out id) are never trained on, nor used to pick the best epoch: they're held out of
every fold, for evaluation only. The rest is split by level, never by tick,
into 8 folds: --fold k/8 (default 0/8) validates on fold k and trains on the
other seven; --fold all trains on every level (the model to ship, once the
folds have given honest numbers).

What a run gives (gbai.Run.usable_ticks): all of a run that completed in the
copies; from one that didn't, only the ticks before its death minus a
second, and only when the real game agreed it dies there (validated "real");
nothing from a "failed" run. Loss: binary
cross-entropy on act[t] (the human's button during tick t, from rec[t]),
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
another process can poll (state, epoch, losses, validation numbers, eta);
mid-epoch checkpoints every --ckpt-every-s. It runs at BelowNormal priority;
a STOP file in the output folder makes it checkpoint and exit, and the same
command resumes (background.py, which can also launch it detached).
"Best" is by validation timing by default (--select timing: the share of
presses and releases within 2 ticks, minus 0.25 per false transition per
second): on the toy data the validation loss kept rising while the timing
kept improving, because a few confidently wrong ticks dominate the loss. Run it again with the same data and it carries on
from last.pt; --fresh starts over. At the end it exports pilot.gbnn (+
pilot.golden.json) from the best epoch and writes eval_report.json/.md
(evaluate.py) for every held-out run.

    python tools/ai/train_pilot.py [--root DIR] [--fold 0/8 | all] [--epochs 8] [--threads auto]

Outputs go to AI_ROOT\\train\\pilot\\ (or --out): progress.json, split.json,
last.pt, best.pt, pilot.gbnn, pilot.golden.json, metrics.json, eval_report.*.
"""
import argparse
import hashlib
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

import background
import evaluate
import export
import gbai
import models


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Train the pilot on human runs (behavior cloning).")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--out", help="output folder (default: AI_ROOT\\train\\pilot)")
    ap.add_argument("--layout-hash", help="train on runs with this obs layout only (gucci_ai_layout gives the mod's)")
    ap.add_argument("--fold", default="0/8",
                    help="k/K (or k, of 8): validate on fold k of the levels, train on the rest; all: train on all")
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
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="validation numbers hold when P > this (0.5 = the mod's rule, logit > 0)")
    ap.add_argument("--load-far-keep", type=float, default=1.0,
                    help="share of the ticks away from presses/releases to load at all (weighted back up); "
                         "for when RAM is short")
    ap.add_argument("--select", choices=["timing", "loss", "last"], default="timing",
                    help="which epoch to export: best validation timing, lowest validation loss, or the last")
    ap.add_argument("--manifest", help="a manifest whose is_bar_level / held_out entries (by id) are held out too")
    ap.add_argument("--exclude", action="append", default=[],
                    help="another level id to leave out of training (repeatable); a name only warns")
    ap.add_argument("--ema", type=int, default=-1,
                    help="export an average of the weights over this many steps (-1: half an epoch; 0: off)")
    ap.add_argument("--fresh", action="store_true", help="ignore last.pt and start over")
    ap.add_argument("--no-eval", action="store_true", help="skip the held-out report at the end")
    background.add_args(ap)
    args = ap.parse_args(argv)
    args.fold_k = gbai.parse_fold(args.fold)
    if not 0 < args.far_keep <= 1:
        ap.error("--far-keep is in (0, 1]")
    if not 0 < args.load_far_keep <= 1:
        ap.error("--load-far-keep is in (0, 1]")
    return args


def showcase_from_args(args):
    ids = [int(e) for e in args.exclude if str(e).strip().isdigit()]
    names = [e for e in args.exclude if not str(e).strip().isdigit()]
    return gbai.Showcase(extra_ids=ids, extra_names=names, manifest=args.manifest)


def pick_rows(run, n, near, load_far_keep, seed):
    """Which of a run's first n ticks to load: all of them, or (load_far_keep < 1) every tick near a
    press/release and that share of the rest, the same ones every time for (seed, run)."""
    if load_far_keep >= 1:
        return np.arange(n)
    h = int(hashlib.sha256(f"{seed}:{run.run_id}".encode()).hexdigest()[:8], 16)
    keep = near[:n] | (np.random.default_rng([h]).random(n) < load_far_keep)
    return np.flatnonzero(keep)


def assemble(runs, near_ticks, load_far_keep=1.0, seed=0):
    """Every training tick in RAM, compact: X gbai.CompactRows [N] (456 bytes a tick), act uint8
    [N], the button going into each tick uint8 [N] (act[t-1]), near bool [N], a loss boost float32
    [N] (1/load_far_keep on the far ticks loaded, else 1); and how many values weren't finite.
    A press or release is where act differs from the button going in.

    Random batches need the rows in RAM (from a memmap they're ~50x slower: TOOLCHAIN.md 4.3):
    ~1.5 GiB for the 4.2M human ticks. Runs are read one at a time and dropped once copied."""
    plans, total = [], 0
    for r in runs:
        n = r.usable_ticks()
        if n:
            rows = pick_rows(r, n, gbai.near_mask(np.asarray(r.act), near_ticks), load_far_keep, seed)
            plans.append((r, rows))
            total += len(rows)
        r.close()
    X = gbai.CompactRows(total)
    y = np.empty(total, np.uint8)
    hb = np.empty(total, np.uint8)
    near = np.empty(total, bool)
    pos = 0
    for r, rows in plans:
        k = len(rows)
        a = np.asarray(r.act)
        X.put(slice(pos, pos + k), r, rows)
        y[pos:pos + k] = a[rows]
        hb[pos:pos + k] = gbai.before(a)[rows]
        near[pos:pos + k] = gbai.near_mask(a, near_ticks)[rows]
        pos += k
        r.close()
    boost = np.where(near, 1.0, 1.0 / load_far_keep).astype(np.float32)
    return X, y, hb, near, boost, X.clean()


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


def predict_logits(model, obs, n=None, chunk=16384):
    """Logits for the first n rows (all by default) of obs (a run's obs view, expanded a chunk at a
    time, or an array); values that aren't finite count as 0, as in training (gbai.clean_rows)."""
    n = len(obs) if n is None else int(n)
    out = np.empty(n, np.float32)
    with torch.no_grad():
        for a in range(0, n, chunk):
            out[a:min(a + chunk, n)] = model(torch.from_numpy(gbai.clean_rows(obs[a:min(a + chunk, n)]))).numpy()
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
        logits = predict_logits(model, r.obs, n)
        np.nan_to_num(logits, copy=False)
        w = tick_weights(yv, gbai.before(a)[:n], gbai.near_mask(a, args.near_ticks)[:n], cb, args, w_norm)
        lt = torch.from_numpy(logits)
        loss = F.binary_cross_entropy_with_logits(lt, torch.from_numpy(yv.astype(np.float32)), reduction="none")
        loss_sum += float((loss.numpy() * w).sum())
        n_sum += n
        prob = export.sigmoid(logits)
        counts = evaluate.add_counts(counts, evaluate.transition_metrics(yv, evaluate.decide(logits, args.threshold),
                                                                         r.tps, prob))
        r.close()
    return (loss_sum / n_sum if n_sum else None), (evaluate.rates(counts) if n_sum else None)


def fold_text(split, args):
    """'fold 3/8', 'all levels', or the fold it became when there were fewer levels than folds."""
    if not split["fold"]:
        return "all levels"
    k, n = split["fold"]
    text = f"fold {k}/{n}"
    if args.fold_k and n != args.fold_k[1]:
        text += f" (asked for {args.fold_k[0]}/{args.fold_k[1]}; there are only {n} levels)"
    return text


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
    background.setup(args, log)
    models.set_seed(args.seed)
    root = gbai.ai_root(args.root)
    out = os.path.abspath(args.out) if args.out else gbai.train_dir(root, "pilot")
    os.makedirs(out, exist_ok=True)
    prog = gbai.Progress(os.path.join(out, "progress.json"), "pilot", out)
    try:
        result = train(args, root, out, prog, log)
    except background.StopRequested:
        return {"model": "pilot", "stopped": True}
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
    split = gbai.make_split(runs, showcase, args.fold_k, args.seed, usable=lambda r: r.usable_ticks() > 0)
    record = gbai.split_record(split, "pilot", layout_hash, args.seed)
    gbai.write_json_atomic(os.path.join(out, "split.json"), record)
    for r, why in split["excluded"]:
        log(f"left out of training: {r.run_id}: {why}")
    for r, warn in split.get("name_warnings", []):
        log(f"NOTE (kept): {r.run_id}: {warn}")
    for r in split["unusable"]:
        log(f"not used: {r.run_id} has no usable ticks (validated {r.validated}, completed {r.completed}: empty, "
            "a failed run, or it died in its first second)")
    train_runs, val_runs = split["train"], split["val"]
    if not train_runs:
        raise gbai.DataError("no runs left to train on once the showcase levels and the runs with no usable ticks "
                             "are left out")
    log(f"{fold_text(split, args)}: {len(train_runs)} training runs ({len(split['train_levels'])} levels), "
        f"{len(val_runs)} validation runs ({len(split['val_levels'])} levels), {len(split['excluded'])} showcase "
        f"runs left out; layout {layout_hash}")

    prog.set(message="reading the training ticks", force=True)
    X, y, hb, near, boost, bad = assemble(train_runs, args.near_ticks, args.load_far_keep, args.seed)
    if bad:
        log(f"NOTE: {bad} values weren't finite; trained on them as 0 (a recorder bug to look at)")
    if len(X) == 0:
        raise gbai.DataError("the training runs have no usable ticks")
    dim = gbai.OBS_DIM
    mean, std = gbai.moments((X[a:a + 16384] for a in range(0, len(X), 16384)), dim)
    cb = class_balance(y, hb)
    plain_w = tick_weights(y, hb, near, cb, args)
    w_norm = float(plain_w.mean())  # validation weighs its ticks the same way (no load boost there)
    w = plain_w * boost
    w = w / float(w.mean())
    log(f"{len(X)} training ticks, {int(y.sum())} held ({100.0 * y.mean():.1f}%), {int(near.sum())} near a "
        f"press/release; {X.nbytes / 2**30:.2f} GiB in RAM")

    model = models.Pilot(dim, models.parse_ints(args.hidden))
    model.set_norm(mean, std)
    signature = {"layout_hash": layout_hash, "obs_dim": dim, "model_config": model.config(), "fold": record["fold"],
                 "load_far_keep": args.load_far_keep,
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

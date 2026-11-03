"""The critic: from the AI's own practice, how likely it is to still be alive 1 s later if it
holds now, and if it releases now.

Data: the practice attempts in AI_ROOT\\practice\\<level_id>\\ (contract v2 s6),
all with one layout; rows are kept compact in RAM and expanded to the 472
floats per batch, like the pilot's. Each tick t of an attempt labels ONE head, the
action it took: act[t] = 1 -> alive_hold, 0 -> alive_release; the label is
alive[t] >= H (H = round(1.0 * tps): it was still alive H ticks later, or
completed).

Branches: an attempt the practice loop made by flipping one decision
(branch_of = {attempt, tick t}) took the other action from the parent's
state at t. Its outcome at t labels the OTHER head of the parent's row at t,
so that state carries both answers (and weighs --pair-weight more: it's the
cleanest evidence of which action was the mistake). The branch's own rows
from t on are ordinary rows; its rows before t - H, if the log has them, are
copies of the parent's and are skipped. (Optional header field first_tick:
the tick of row 0, when a branch's log starts at the branch; see
CONTRACT_ISSUES.md.)

Rows used: every tick the player is alive. An attempt that ended without
completing or dying (stopped by a limit) loses its last H rows: their
outcome isn't known. Practice on the showcase levels is evaluation, never
training. Each head is class balanced (surviving and dying weigh the same in
total). The split is by level, like the pilot's (--fold k/8, default 0/8, or
all). Checkpoints, progress.json, the STOP file and resuming work as in
train_pilot.py; the epoch with the lowest validation loss is exported as
critic.gbnn (two logits, no sigmoid: alive_hold, alive_release; the mod
compares them) with critic.golden.json.

    python tools/ai/train_critic.py [--root DIR] [--epochs 6] [--threads auto]
"""
import argparse
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

import background
import export
import gbai
import models
import train_pilot


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Train the critic on the AI's practice attempts.")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--out", help="output folder (default: AI_ROOT\\train\\critic)")
    ap.add_argument("--layout-hash", help="train on attempts with this obs layout only")
    ap.add_argument("--fold", default="0/8",
                    help="k/K (or k, of 8): validate on fold k of the levels, train on the rest; all: train on all")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--epochs", type=int, default=6)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-4)
    ap.add_argument("--hidden", default="256,256", help="hidden layer sizes")
    ap.add_argument("--pair-weight", type=float, default=4.0,
                    help="how much more a state with both answers (a branch and its parent) weighs")
    ap.add_argument("--select", choices=["loss", "last"], default="loss",
                    help="which epoch to export: lowest validation loss, or the last")
    ap.add_argument("--manifest", help="a manifest whose is_bar_level / held_out entries (by id) are held out too")
    ap.add_argument("--exclude", action="append", default=[],
                    help="another level id to leave out of training (repeatable); a name only warns")
    ap.add_argument("--ema", type=int, default=-1,
                    help="export an average of the weights over this many steps (-1: half an epoch; 0: off)")
    ap.add_argument("--fresh", action="store_true", help="ignore last.pt and start over")
    background.add_args(ap)
    args = ap.parse_args(argv)
    args.fold_k = gbai.parse_fold(args.fold)
    args.far_keep = 1.0
    return args


def rows_to_use(a):
    """Which rows of an attempt are labels (bool [T])."""
    T = a.ticks
    if T == 0:
        return np.zeros(0, bool)
    keep = np.asarray(a.meta[:, gbai.META_DEAD]) < 0.5
    if not a.completed and a.died_tick is None:
        keep[max(0, T - a.horizon):] = False  # stopped by a limit: what came next isn't known
    b = a.branch_of
    if b:
        first = b["tick"] - a.horizon - a.first_tick  # before this the log copies the parent's rows
        if first > 0:
            keep[:first] = False
    return keep


def _attempt_name(text):
    text = os.path.basename(str(text))
    return text[:-5] if text.endswith(".json") else text


def assemble(attempts, pair_weight, log=print):
    """Rows for the critic: X gbai.CompactRows [N], Y float32 [N][2] (alive_hold, alive_release),
    M float32 [N][2] (1 where a head has a label), row weights [N], the rows that hold a pair,
    and how many values weren't finite (trained as 0)."""
    plans, total = [], 0
    by_name = {}
    for a in attempts:
        keep = rows_to_use(a)
        idx = np.full(a.ticks, -1, np.int64)
        n = int(keep.sum())
        idx[keep] = np.arange(total, total + n)
        plans.append((a, keep, idx))
        by_name[(a.level_id, a.run_id)] = (a, idx)
        total += n
        a.close()
    # In RAM like the pilot's rows (368 bytes a row). Practice logs grow with every play
    # session, so this is the trainer that will outgrow RAM first.
    X = gbai.CompactRows(total)
    Y = np.zeros((total, 2), np.float32)
    M = np.zeros((total, 2), np.float32)
    sw = np.ones(total, np.float32)
    for a, keep, idx in plans:
        rows = np.flatnonzero(keep)
        if not len(rows):
            continue
        g = idx[rows]
        X.put(g, a, rows)
        act = np.asarray(a.act)[rows]
        alive = np.asarray(a.alive)[rows]
        head = np.where(act == 1, models.Critic.HOLD, models.Critic.RELEASE)
        Y[g, head] = (alive >= a.horizon).astype(np.float32)
        M[g, head] = 1.0
        a.close()
    pairs = []
    for a, keep, idx in plans:
        b = a.branch_of
        if not b:
            continue
        found = by_name.get((a.level_id, _attempt_name(b["attempt"])))
        if not found:
            log(f"NOTE: {a.run_id} branches from {b['attempt']}, which isn't there; used as a plain attempt")
            continue
        p, pidx = found
        t = int(b["tick"])
        rp, rb = t - p.first_tick, t - a.first_tick
        if not (0 <= rp < p.ticks and 0 <= rb < a.ticks):
            log(f"NOTE: {a.run_id}: branch tick {t} is outside its parent's or its own log")
            continue
        gp, gb = int(pidx[rp]), int(idx[rb])
        if gp < 0 or gb < 0:
            continue
        if int(p.act[rp]) == int(a.act[rb]):
            log(f"NOTE: {a.run_id}: the branch took the same action as its parent at tick {t}; not a pair")
            continue
        sp, sb = p.obs[rp], a.obs[rb]
        if not np.allclose(sp, sb, atol=1e-2, rtol=1e-2, equal_nan=True):
            log(f"NOTE: {a.run_id}: at tick {t} the branch saw a different state than its parent (max difference "
                f"{float(np.nanmax(np.abs(sp - sb))):.3g}); the snapshot restore may not be exact. Not paired.")
            continue
        other = models.Critic.HOLD if int(a.act[rb]) == 1 else models.Critic.RELEASE
        Y[gp, other] = Y[gb, other]
        M[gp, other] = 1.0
        sw[gp] = pair_weight
        M[gb] = 0.0  # the branch's own row is now the pair's other half
        pairs.append(gp)
        a.close()
        p.close()
    for a, _, _ in plans:
        a.close()
    return X, Y, M, sw, np.array(pairs, np.int64), X.clean()


def head_balance(Y, M):
    """[2 heads][2 outcomes] weights so dying and surviving weigh the same in each head's total."""
    cb = np.ones((2, 2), np.float64)
    for j in range(2):
        lab = M[:, j] > 0
        n = int(lab.sum())
        pos = int((Y[lab, j] > 0.5).sum())
        if n and 0 < pos < n:
            cb[j] = [n / (2.0 * (n - pos)), n / (2.0 * pos)]
    return cb


def weights(Y, M, sw, cb, norm=None):
    W = np.zeros_like(Y)
    for j in range(2):
        W[:, j] = M[:, j] * cb[j][(Y[:, j] > 0.5).astype(np.int64)] * sw
    if norm is None:
        lab = M > 0
        norm = float(W[lab].mean()) if lab.any() else 1.0
    return (W / norm).astype(np.float32), norm


def predict(model, X, chunk=65536):
    out = np.empty((len(X), 2), np.float32)
    with torch.no_grad():
        for a in range(0, len(X), chunk):
            out[a:a + chunk] = model(models.as_batch(X, np.arange(a, min(a + chunk, len(X))))).numpy()
    return out


def metrics(logits, Y, M, pairs):
    """Per head: labels, base rate, accuracy and balanced accuracy (alive when logit > 0); and for
    each pair whose two answers differ, whether the critic ranks the surviving action higher."""
    p = np.asarray(logits)
    out = {}
    for j, name in enumerate(models.Critic.out_names):
        lab = M[:, j] > 0
        y = Y[lab, j] > 0.5
        q = p[lab, j] > 0
        n = int(lab.sum())
        pos, neg = int(y.sum()), int((~y).sum())
        tpr = float((q & y).sum()) / pos if pos else None
        tnr = float((~q & ~y).sum()) / neg if neg else None
        out[name] = {"labels": n, "alive_rate": float(y.mean()) if n else None,
                     "accuracy": float((q == y).mean()) if n else None,
                     "balanced_accuracy": (tpr + tnr) / 2 if tpr is not None and tnr is not None else None}
    differ = [i for i in pairs if (Y[i, 0] > 0.5) != (Y[i, 1] > 0.5)]
    right = sum(1 for i in differ if (p[i, 0] > p[i, 1]) == (Y[i, 0] > 0.5))
    out["pairs"] = len(pairs)
    out["pairs_that_differ"] = len(differ)
    out["pair_ranking_accuracy"] = right / len(differ) if differ else None
    return out


def main(argv=None, log=print):
    args = parse_args(argv)
    background.setup(args, log)
    models.set_seed(args.seed)
    root = gbai.ai_root(args.root)
    out = os.path.abspath(args.out) if args.out else gbai.train_dir(root, "critic")
    os.makedirs(out, exist_ok=True)
    prog = gbai.Progress(os.path.join(out, "progress.json"), "critic", out)
    try:
        result = train(args, root, out, prog, log)
    except background.StopRequested:
        return {"model": "critic", "stopped": True}
    except BaseException as e:
        prog.finish("failed", f"{type(e).__name__}: {e}")
        raise
    prog.finish("done", "trained and exported", exported=result["export"]["gbnn"])
    return result


def train(args, root, out, prog, log):
    prog.set(state="loading", message="reading practice attempts", force=True)
    attempts, problems = gbai.list_practice(root)
    for _, why in problems:
        log(f"skipped (doesn't follow the contract): {why}")
    if not attempts:
        raise gbai.DataError(f"no practice attempts in {os.path.join(root, 'practice')} (gucci_ai_play writes them)")
    attempts, layout_hash = gbai.pick_layout(attempts, args.layout_hash)
    showcase = train_pilot.showcase_from_args(args)
    split = gbai.make_split(attempts, showcase, args.fold_k, args.seed, usable=lambda a: bool(rows_to_use(a).any()))
    record = gbai.split_record(split, "critic", layout_hash, args.seed)
    gbai.write_json_atomic(os.path.join(out, "split.json"), record)
    for a, why in split["excluded"]:
        log(f"left out of training: {a.run_id}: {why}")
    for a, warn in split.get("name_warnings", []):
        log(f"NOTE (kept): {a.run_id}: {warn}")
    for a in split["unusable"]:
        log(f"not used: {a.run_id} has no rows with a known outcome")
    if not split["train"]:
        raise gbai.DataError("no practice attempts left to train on once the showcase levels and the attempts with "
                             "no usable rows are left out")
    X, Y, M, sw, pairs, bad = assemble(split["train"], args.pair_weight, log)
    if bad:
        log(f"NOTE: {bad} obs values weren't finite; trained on them as 0")
    if not (M > 0).any():
        raise gbai.DataError("the practice attempts have no usable ticks")
    cb = head_balance(Y, M)
    W, norm = weights(Y, M, sw, cb)
    lab = M > 0
    log(f"{train_pilot.fold_text(split, args)}: {len(split['train'])} attempts ({len(split['train_levels'])} levels), "
        f"{len(X)} rows ({X.nbytes / 2**30:.2f} GiB in RAM), {len(pairs)} pairs; "
        f"alive rate hold {float(Y[lab[:, 0], 0].mean()) if lab[:, 0].any() else float('nan'):.3f}, release "
        f"{float(Y[lab[:, 1], 1].mean()) if lab[:, 1].any() else float('nan'):.3f}")
    dim = gbai.OBS_DIM
    mean, std = gbai.moments((X[a:a + 16384] for a in range(0, len(X), 16384)), dim)
    val = None
    if split["val"]:
        vX, vY, vM, vsw, vpairs, _ = assemble(split["val"], args.pair_weight, log)
        vW, _ = weights(vY, vM, vsw, cb, norm)
        val = (vX, vY, vM, vW, vpairs)

    model = models.Critic(dim, models.parse_ints(args.hidden))
    model.set_norm(mean, std)
    signature = {"layout_hash": layout_hash, "obs_dim": dim, "model_config": model.config(), "fold": record["fold"],
                 "train_runs": sorted(a.run_id for a in split["train"]),
                 "val_runs": sorted(a.run_id for a in split["val"]), "pair_weight": args.pair_weight,
                 "batch": args.batch, "seed": args.seed, "select": args.select}

    def check(m):
        vX, vY, vM, vW, vpairs = val
        logits = predict(m, vX)
        loss = F.binary_cross_entropy_with_logits(torch.from_numpy(logits), torch.from_numpy(vY), reduction="none")
        val_loss = float((loss.numpy() * vW).sum(1).mean()) if len(vX) else None
        mt = metrics(logits, vY, vM, vpairs)
        text = ", ".join(f"{name} balanced acc {mt[name]['balanced_accuracy']:.3f}" for name in models.Critic.out_names
                         if mt[name]["balanced_accuracy"] is not None)
        return val_loss, mt, None, text

    best, history = models.fit_rows(model, X, Y, W, args, check if val else None, out, signature, layout_hash, prog,
                                    log)
    prog.set(state="exporting", force=True, message="writing critic.gbnn")
    rng = np.random.default_rng(args.seed)
    src = val[0] if val and len(val[0]) else X
    rows = [src[int(i)] for i in rng.integers(0, len(src), min(6, len(src)))]
    rows += [X[int(i)] for i in pairs[:2]]
    rows.append(np.zeros(dim, np.float32))
    info = export.export_model(model, layout_hash, os.path.join(out, "critic.gbnn"), np.stack(rows))
    log(f"wrote {info['gbnn']} ({info['bytes']} bytes, numpy vs torch {info['max_err_vs_torch']:.2g})")
    final = metrics(predict(model, val[0]), val[1], val[2], val[4]) if val else None
    result = {"model": "critic", "export": info, "layout_hash": layout_hash, "best": best, "history": history,
              "val": final, "split": os.path.join(out, "split.json")}
    gbai.write_json_atomic(os.path.join(out, "metrics.json"), result)
    return result


if __name__ == "__main__":
    try:
        main()
    except (gbai.DataError, ValueError) as e:
        sys.exit(f"train_critic: {e}")

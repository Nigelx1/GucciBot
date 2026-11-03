"""The level reader: from a level's tape, where a human goes along it.

Input: the tape (contract s3), float16 [C columns][K channels], 0.5 block per
column. Output per column: y_rel (blocks above the tape's y0), 8 mode logits,
the gravity-flipped logit and hold_frac. The pilot reads them as its route.

Labels come from the human runs of each level that has a tape: every usable
tick (the same cut as the pilot's: a run that died loses its last second)
falls in column floor((x - x0) / column_width), and per column
    y_rel     = mean(y of those ticks) - y0, in blocks (30 units)
    mode      = the mode most of them are in (meta's order: cube ship ball ufo
                wave robot spider swing)
    gravity   = the share with gravity flipped (a soft label for the logit)
    hold_frac = the share with the button held (a plain output trained toward
                0..1; the mod clamps it)
Columns no tick falls in are masked (no loss there). Each (tape, run) pair is
one sample, so a level with two macros teaches both routes. Showcase levels
are left out, as for the pilot, and the split is by level (--fold or
--val-frac).

The network is a conv1d stack (same padding; dilations 1..32; a 1x1 conv on
top), and every tape goes through whole, one at a time: padding tapes into a
batch would put non-zero activations where the mod's C++ loop has zeros
outside [0, C). Gradients add up over --tapes-per-step tapes.

reader.gbnn carries a layout hash like the others; tapes have none in
contract v1 (CONTRACT_ISSUES.md), so it is --layout-hash, else the tapes'
"layout_hash" field, else the human runs' obs_layout_hash.

    python tools/ai/train_reader.py [--root DIR] [--epochs 60] [--threads 6]
"""
import argparse
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

import export
import gbai
import models
import train_pilot


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Train the level reader on tapes and human runs.")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--out", help="output folder (default: AI_ROOT\\train\\reader)")
    ap.add_argument("--layout-hash", help="the hash reader.gbnn carries (the mod's tape layout)")
    ap.add_argument("--fold", help="k/K: hold out fold k of K, split by level")
    ap.add_argument("--val-frac", type=float, default=0.15, help="without --fold: share of levels to validate on")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--tapes-per-step", type=int, default=4, help="tapes whose gradients add up per step")
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-4)
    ap.add_argument("--channels", type=int, default=64)
    ap.add_argument("--kernel", type=int, default=5, help="odd")
    ap.add_argument("--dilations", default="1,2,4,8,16,32")
    ap.add_argument("--w-y", type=float, default=1.0, help="loss weight of y_rel (Huber, in blocks)")
    ap.add_argument("--w-mode", type=float, default=1.0, help="loss weight of the mode")
    ap.add_argument("--w-gravity", type=float, default=0.5, help="loss weight of gravity")
    ap.add_argument("--w-hold", type=float, default=1.0, help="loss weight of hold_frac")
    ap.add_argument("--select", choices=["loss", "last"], default="loss")
    ap.add_argument("--threads", type=int, default=6, help="CPU threads (6 = one per core on Nigel's PC)")
    ap.add_argument("--manifest", help="a manifest whose is_bar_level entries are showcase levels too")
    ap.add_argument("--exclude", action="append", default=[],
                    help="another level to leave out of training, by id or name (repeatable)")
    ap.add_argument("--fresh", action="store_true", help="ignore last.pt and start over")
    args = ap.parse_args(argv)
    args.fold_k = gbai.parse_fold(args.fold) if args.fold else None
    if args.kernel < 1 or args.kernel % 2 == 0:
        ap.error("--kernel must be odd")
    return args


def column_labels(tape, run):
    """Per-column labels from one human run: dict of y_rel, mode, gravity, hold (float32/int64 [C]) and
    mask (bool [C]); None when no usable tick falls on the tape."""
    n = run.usable_ticks()
    C = tape.columns
    if n == 0 or C == 0:
        return None
    meta = np.asarray(run.meta[:n], np.float64)
    act = np.asarray(run.act[:n], np.float64)
    col = tape.column_of(meta[:, gbai.META_X])
    ok = (col >= 0) & (col < C)
    if not ok.any():
        return None
    col, meta, act = col[ok], meta[ok], act[ok]
    count = np.bincount(col, minlength=C).astype(np.float64)
    mask = count > 0
    safe = np.where(mask, count, 1.0)
    y = np.bincount(col, weights=meta[:, gbai.META_Y], minlength=C) / safe
    hold = np.bincount(col, weights=act, minlength=C) / safe
    grav = np.bincount(col, weights=(meta[:, gbai.META_GRAVITY] >= 0.5).astype(np.float64), minlength=C) / safe
    mode = np.clip(np.round(meta[:, gbai.META_MODE]), 0, len(gbai.MODES) - 1).astype(np.int64)
    votes = np.bincount(col * len(gbai.MODES) + mode, minlength=C * len(gbai.MODES)).reshape(C, len(gbai.MODES))
    return {"y_rel": np.where(mask, (y - tape.y0) / gbai.BLOCK, 0.0).astype(np.float32),
            "mode": votes.argmax(1).astype(np.int64),
            "gravity": np.where(mask, grav, 0.0).astype(np.float32),
            "hold": np.where(mask, hold, 0.0).astype(np.float32),
            "mask": mask}


def samples_for(runs, tapes, log=print):
    """[(level_id, run_id, labels)] for every run whose level has a tape."""
    out = []
    for r in runs:
        tape = tapes.get(r.level_id) if r.level_id else None
        if tape is None:
            log(f"NOTE: {r.run_id}: no tape for level {r.level_id}; not used by the reader")
            continue
        lab = column_labels(tape, r)
        r.close()
        if lab is None:
            log(f"NOTE: {r.run_id}: no usable tick falls on its level's tape")
            continue
        out.append((r.level_id, r.run_id, lab))
    return out


def reader_hash(args, tapes, runs, log=print):
    if args.layout_hash:
        return args.layout_hash
    hashes = {t.layout_hash for t in tapes}
    if len(hashes) == 1 and None not in hashes:
        return hashes.pop()
    if len(hashes - {None}) > 1:
        raise gbai.DataError(f"the tapes were made with different tape layouts ({sorted(hashes - {None})}); "
                             "pass --layout-hash")
    run_hashes = {r.layout_hash for r in runs}
    if len(run_hashes) == 1:
        log("NOTE: the tapes carry no layout_hash (contract v1 has none, see CONTRACT_ISSUES.md); reader.gbnn gets "
            "the human runs' obs_layout_hash")
        return run_hashes.pop()
    raise gbai.DataError("can't tell which layout hash reader.gbnn should carry: pass --layout-hash "
                         "(gucci_ai_layout gives the mod's)")


def tensors(sample, inputs):
    lid, _, lab = sample
    x = inputs[lid]
    m = torch.from_numpy(lab["mask"])
    return (x, m, torch.from_numpy(lab["y_rel"]), torch.from_numpy(lab["mode"]), torch.from_numpy(lab["gravity"]),
            torch.from_numpy(lab["hold"]))


def sample_loss(model, sample, inputs, args):
    """The weighted loss of one (tape, run) sample, and its parts."""
    x, m, y, mode, grav, hold = tensors(sample, inputs)
    out = model(x)[0]  # [11][C]
    if not bool(m.any()):
        return None, {}
    o = out[:, m]
    parts = {"y": F.smooth_l1_loss(o[models.READER_Y], y[m], beta=1.0),
             "mode": F.cross_entropy(o[models.READER_MODE0:models.READER_GRAVITY].T, mode[m]),
             "gravity": F.binary_cross_entropy_with_logits(o[models.READER_GRAVITY], grav[m]),
             "hold": F.mse_loss(o[models.READER_HOLD], hold[m])}
    loss = (args.w_y * parts["y"] + args.w_mode * parts["mode"] + args.w_gravity * parts["gravity"]
            + args.w_hold * parts["hold"])
    return loss, parts


def validate(model, samples, inputs, args):
    """(mean validation loss, metrics): y MAE in blocks, mode accuracy, gravity accuracy, hold_frac MAE,
    over every labeled column."""
    model.eval()
    losses = []
    tot = {"cols": 0, "y_abs": 0.0, "mode_ok": 0, "grav_ok": 0, "hold_abs": 0.0}
    with torch.no_grad():
        for s in samples:
            loss, _ = sample_loss(model, s, inputs, args)
            if loss is None:
                continue
            losses.append(float(loss))
            x, m, y, mode, grav, hold = tensors(s, inputs)
            o = model(x)[0][:, m]
            tot["cols"] += int(m.sum())
            tot["y_abs"] += float((o[models.READER_Y] - y[m]).abs().sum())
            tot["mode_ok"] += int((o[models.READER_MODE0:models.READER_GRAVITY].argmax(0) == mode[m]).sum())
            tot["grav_ok"] += int(((o[models.READER_GRAVITY] >= 0) == (grav[m] >= 0.5)).sum())
            tot["hold_abs"] += float((o[models.READER_HOLD].clamp(0, 1) - hold[m]).abs().sum())
    n = tot["cols"]
    if not n:
        return None, None
    return float(np.mean(losses)), {"columns": n, "y_mae_blocks": tot["y_abs"] / n, "mode_accuracy": tot["mode_ok"] / n,
                                    "gravity_accuracy": tot["grav_ok"] / n, "hold_frac_mae": tot["hold_abs"] / n}


def main(argv=None, log=print):
    args = parse_args(argv)
    models.set_threads(args.threads)
    models.set_seed(args.seed)
    root = gbai.ai_root(args.root)
    out = os.path.abspath(args.out) if args.out else gbai.train_dir(root, "reader")
    os.makedirs(out, exist_ok=True)
    prog = gbai.Progress(os.path.join(out, "progress.json"), "reader", out)
    try:
        result = train(args, root, out, prog, log)
    except BaseException as e:
        prog.finish("failed", f"{type(e).__name__}: {e}")
        raise
    prog.finish("done", "trained and exported", exported=result["export"]["gbnn"])
    return result


def train(args, root, out, prog, log):
    prog.set(state="loading", message="reading tapes and runs", force=True)
    tapes, problems = gbai.list_tapes(root)
    runs, run_problems = gbai.list_runs(root, "human")
    for _, why in problems + run_problems:
        log(f"skipped (doesn't follow the contract): {why}")
    if not tapes:
        raise gbai.DataError(f"no tapes in {os.path.join(root, 'tapes')} (gucci_ai_record writes them)")
    runs = [r for r in runs if r.level_id in tapes]
    if not runs:
        raise gbai.DataError("no human run is on a level that has a tape")
    showcase = train_pilot.showcase_from_args(args)
    split = gbai.make_split(runs, showcase, args.fold_k, args.val_frac, args.seed,
                            usable=lambda r: r.usable_ticks() > 0)
    for r in split["unusable"]:
        log(f"not used: {r.run_id} has no usable ticks (empty, or it died in its first second)")
    used = {r.level_id for r in split["train"] + split["val"]}
    layouts = {(t.channels, str(t.channel_layout)) for lid, t in tapes.items() if lid in used}
    if len(layouts) > 1:
        raise gbai.DataError("the tapes were made with different channel layouts; record them again with one build")
    layout_hash = reader_hash(args, [t for lid, t in tapes.items() if lid in used], split["train"] + split["val"], log)
    record = gbai.split_record(split, "reader", layout_hash, args.fold_k, args.val_frac, args.seed)
    gbai.write_json_atomic(os.path.join(out, "split.json"), record)
    for r, why in split["excluded"]:
        log(f"left out of training: {r.run_id}: {why}")
    train_s = samples_for(split["train"], tapes, log)
    val_s = samples_for(split["val"], tapes, log)
    if not train_s:
        raise gbai.DataError("no (tape, run) pairs left to train on once the showcase levels and the runs with no "
                             "usable ticks on their tape are left out")
    inputs = {}
    for lid in {s[0] for s in train_s + val_s}:
        data = np.asarray(tapes[lid].data, np.float32)
        gbai.finite_rows(data)
        inputs[lid] = torch.from_numpy(np.ascontiguousarray(data.T[None]))  # [1][K][C]
        tapes[lid].close()
    K = next(iter(inputs.values())).shape[1]
    train_levels = sorted({s[0] for s in train_s})
    mean, std = gbai.moments((inputs[lid][0].numpy().T for lid in train_levels), K)
    log(f"{len(train_s)} training samples ({len(train_levels)} tapes, {K} channels), {len(val_s)} validation samples")

    model = models.Reader(K, args.channels, args.kernel, models.parse_ints(args.dilations))
    model.set_norm(mean, std)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    signature = models.plain({"layout_hash": layout_hash, "channels_in": K, "model_config": model.config(),
                              "train": sorted(s[1] for s in train_s), "val": sorted(s[1] for s in val_s),
                              "weights": [args.w_y, args.w_mode, args.w_gravity, args.w_hold],
                              "tapes_per_step": args.tapes_per_step, "seed": args.seed, "select": args.select})
    last_path, best_path = os.path.join(out, "last.pt"), os.path.join(out, "best.pt")
    start_epoch, step, history = 0, 0, []
    best = {"epoch": None, "val_loss": None}
    if os.path.exists(last_path) and not args.fresh:
        ck = models.load_checkpoint(last_path)
        have = ck.get("signature") or {}
        if have != signature:
            differ = sorted(k for k in set(signature) | set(have) if signature.get(k) != have.get(k))
            raise SystemExit(f"{last_path} was trained with other data or settings ({', '.join(differ)}); run with "
                             "--fresh to start over (or give --out another folder)")
        model.load_state_dict(ck["model_state"])
        opt.load_state_dict(ck["optimizer_state"])
        start_epoch, step, history, best = ck["epoch"], ck["step"], ck["history"], ck["best"]
        log(f"resuming after epoch {start_epoch}")

    steps_per_epoch = max(1, int(np.ceil(len(train_s) / args.tapes_per_step)))
    total_steps = steps_per_epoch * args.epochs
    prog.set(state="training", epochs=args.epochs, epoch=start_epoch, steps_per_epoch=steps_per_epoch, step=step,
             history=history, best_epoch=best["epoch"], force=True)
    t_start, epochs_run = time.time(), 0
    for epoch in range(start_epoch, args.epochs):
        rng = np.random.default_rng([args.seed, epoch])
        order = rng.permutation(len(train_s))
        model.train()
        t_epoch = time.time()
        sums, count = {}, 0
        for b in range(steps_per_epoch):
            chunk = order[b * args.tapes_per_step:(b + 1) * args.tapes_per_step]
            lr = models.lr_at(step, total_steps, args.lr)
            for g in opt.param_groups:
                g["lr"] = lr
            opt.zero_grad(set_to_none=True)
            for i in chunk:
                loss, parts = sample_loss(model, train_s[int(i)], inputs, args)
                if loss is None:
                    continue
                (loss / len(chunk)).backward()
                count += 1
                sums["loss"] = sums.get("loss", 0.0) + loss.item()
                for k, v in parts.items():
                    sums[k] = sums.get(k, 0.0) + v.item()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            opt.step()
            step += 1
        train_parts = {k: v / max(1, count) for k, v in sums.items()}
        val_loss, val = validate(model, val_s, inputs, args) if val_s else (None, None)
        epochs_run += 1
        entry = {"epoch": epoch + 1, "train_loss": train_parts.get("loss"), "train_parts": train_parts,
                 "val_loss": val_loss, "val": val, "seconds": round(time.time() - t_epoch, 1)}
        history.append(entry)
        improved = args.select == "last" or not val_s or (
            val_loss is not None and (best["val_loss"] is None or val_loss < best["val_loss"]))
        if improved:
            best = {"epoch": epoch + 1, "val_loss": val_loss}
        history, best = models.plain(history), models.plain(best)
        ck = {"format": "gbai-ckpt", "model": "reader", "model_config": model.config(),
              "model_state": model.state_dict(), "optimizer_state": opt.state_dict(), "epoch": epoch + 1,
              "step": step, "signature": signature, "layout_hash": layout_hash, "history": history, "best": best}
        models.save_checkpoint(last_path, ck)
        if improved:
            models.save_checkpoint(best_path, ck)
        eta = (args.epochs - epoch - 1) * (time.time() - t_start) / epochs_run
        prog.set(force=True, epoch=epoch + 1, step=step, train_loss=entry["train_loss"], val_loss=val_loss, val=val,
                 history=history, best_epoch=best["epoch"], eta_seconds=eta)
        if epoch == start_epoch or (epoch + 1) % 10 == 0 or epoch + 1 == args.epochs:
            log(f"epoch {epoch + 1}/{args.epochs}: train {entry['train_loss']:.4f}" +
                (f", val {val_loss:.4f}, y off by {val['y_mae_blocks']:.2f} blocks, mode right "
                 f"{100 * val['mode_accuracy']:.1f}%" if val else ""))

    prog.set(state="exporting", force=True, message="writing reader.gbnn")
    if best["epoch"] is not None and best["epoch"] != len(history) and os.path.exists(best_path):
        model.load_state_dict(models.load_checkpoint(best_path)["model_state"])
    model.eval()
    lid0 = (val_s or train_s)[0][0]
    real = inputs[lid0][0].numpy().T[:64]  # [C][K], the tape's own layout
    rng = np.random.default_rng(args.seed)
    golden = [np.ascontiguousarray(real, np.float32),
              (mean + std * rng.standard_normal((5, K))).astype(np.float32)]
    info = export.export_model(model, layout_hash, os.path.join(out, "reader.gbnn"), golden)
    log(f"wrote {info['gbnn']} ({info['bytes']} bytes, numpy vs torch {info['max_err_vs_torch']:.2g}, "
        f"sees {model.receptive_field()} columns)")
    final = validate(model, val_s, inputs, args)[1] if val_s else None
    result = {"model": "reader", "export": info, "layout_hash": layout_hash, "best": best, "history": history,
              "val": final, "split": os.path.join(out, "split.json")}
    gbai.write_json_atomic(os.path.join(out, "metrics.json"), result)
    return result


if __name__ == "__main__":
    try:
        main()
    except (gbai.DataError, ValueError) as e:
        sys.exit(f"train_reader: {e}")

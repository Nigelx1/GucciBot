"""Held-out numbers for the pilot: does it press and release when the human did?

Runs the exported pilot.gbnn (the same numpy forward pass export.py checks
against PyTorch, so this is what the mod runs) over human runs of levels it
never trained on, and reports, next to a trivial baseline that repeats the
previous tick's button:

- tick accuracy: the share of ticks where the pilot's button (hold when its
  logit > 0, ties release: the mod's rule, contract v2 s7)
  matches the human's. The baseline scores ~99% here, which is why this number
  alone says little.
- presses and releases within 1, 2 and 4 ticks: for each human press (or
  release) at tick t0, whether the pilot decided the same change somewhere in
  t0-k .. t0.
- first decision within 2 (strict): the same, but the pilot's FIRST decision
  since the human's previous change must be the one in the window; any
  earlier one means it would have acted too early.
- false transitions per second: pilot decisions with no human change of the
  same kind within the next 4 ticks.

How a "decision" is counted. Every tick the pilot sees the human's own state
(teacher forcing: obs[t] carries the human's button and position), so it can
only be asked "would you change the button now?". A decision is the first
tick of a stretch where its answer differs from the human's current button.
That shows a decision at or before the human's own tick; after it, the
human's change is already in what the pilot sees, so a LATE press can't be
told apart and counts as missed. The windows are therefore "up to k ticks
early, or on the tick". Closed-loop numbers come from the game
(gucci_ai_play), not from here.

    python tools/ai/evaluate.py [--model pilot.gbnn] [--split split.json] [--root DIR]

Writes eval_report.json and eval_report.md next to the model (or --out).
"""
import argparse
import os
import sys

import numpy as np

import export
import gbai

TOLERANCES = (1, 2, 4)
STRICT_K = 2


def _side():
    return {"events": 0, "within": {str(k): 0 for k in TOLERANCES}, "first_within": {str(k): 0 for k in TOLERANCES},
            "missed": 0, "early_sum": 0, "p_sum": 0.0}


def empty_counts():
    return {"runs": 0, "ticks": 0, "seconds": 0.0, "correct": 0, "decisions": 0, "false": 0,
            "press": _side(), "release": _side()}


def transition_metrics(act, pred, tps, prob=None, tolerances=TOLERANCES):
    """Counts for one run: act = the human's button per tick, pred = the model's (bools).

    prob (optional) is the model's P(hold) per tick, for the mean probability
    it gave the human's new state at each change.
    """
    act = np.asarray(act).astype(bool)
    pred = np.asarray(pred).astype(bool)
    T = len(act)
    out = empty_counts()
    out.update(runs=1, ticks=T, seconds=T / float(tps), correct=int((pred == act).sum()))
    if T == 0:
        return out
    h = gbai.before(act)  # the human's button going into each tick
    changes = np.flatnonzero(act != h)  # the human's presses and releases
    wants = pred != h  # the model would change the button on this tick
    prev = np.zeros(T, bool)
    prev[1:] = wants[:-1]
    same = np.ones(T, bool)
    same[1:] = h[1:] == h[:-1]
    starts = np.flatnonzero(wants & ~(prev & same))  # the model's decisions
    out["decisions"] = int(len(starts))
    maxk = max(tolerances)
    near = np.zeros(len(starts), bool)
    seg_start = np.zeros(len(changes), np.int64)  # first tick since the human's previous change
    seg_start[1:] = changes[:-1] + 1
    for name, d in (("press", True), ("release", False)):
        side = out[name]
        pick = np.flatnonzero(pred[starts] == d)  # the model's decisions of this kind
        st = starts[pick]
        for i in np.flatnonzero(act[changes] == d):
            t0 = int(changes[i])
            side["events"] += 1
            if prob is not None:
                side["p_sum"] += float(prob[t0] if d else 1.0 - prob[t0])
            lo = np.searchsorted(st, max(int(seg_start[i]), t0 - maxk), "left")
            hi = np.searchsorted(st, t0, "right")
            if lo < hi:
                early = t0 - int(st[lo])  # the first decision in the window
                side["early_sum"] += early
                for k in tolerances:
                    if early <= k:
                        side["within"][str(k)] += 1
                near[pick[lo:hi]] = True
            else:
                side["missed"] += 1
            first = np.searchsorted(st, int(seg_start[i]), "left")
            if first < hi:
                early = t0 - int(st[first])
                for k in tolerances:
                    if early <= k:
                        side["first_within"][str(k)] += 1
    out["false"] = int((~near).sum())
    return out


def add_counts(a, b):
    """a + b, for pooling runs."""
    out = empty_counts()
    for key in ("runs", "ticks", "seconds", "correct", "decisions", "false"):
        out[key] = a[key] + b[key]
    for name in ("press", "release"):
        s, x, y = out[name], a[name], b[name]
        for key in ("events", "missed", "early_sum", "p_sum"):
            s[key] = x[key] + y[key]
        for key in ("within", "first_within"):
            s[key] = {k: x[key][k] + y[key][k] for k in x[key]}
    return out


def rates(c, with_prob=True):
    """Counts -> the numbers a report shows."""
    def frac(n, d):
        return float(n) / d if d else None
    out = {"runs": c["runs"], "ticks": c["ticks"], "seconds": round(c["seconds"], 2),
           "tick_accuracy": frac(c["correct"], c["ticks"])}
    for name in ("press", "release"):
        s = c[name]
        n = s["events"]
        out[f"{name}es" if name == "press" else f"{name}s"] = n
        for k in TOLERANCES:
            out[f"{name}_within_{k}"] = frac(s["within"][str(k)], n)
        for k in TOLERANCES:
            out[f"{name}_first_within_{k}"] = frac(s["first_within"][str(k)], n)
        out[f"{name}_missed"] = frac(s["missed"], n)
        out[f"{name}_mean_early_ticks"] = frac(s["early_sum"], n - s["missed"])
        if with_prob:
            out[f"{name}_mean_p"] = frac(s["p_sum"], n)
    out["decisions_per_sec"] = frac(c["decisions"], c["seconds"])
    out["false_per_sec"] = frac(c["false"], c["seconds"])
    return out


def decide(logits, threshold=0.5):
    """The pilot's button from its logits: hold when logit > 0 (ties release), the mod's rule.
    threshold is a P(hold) cut for experiments: hold when logit > log(t / (1 - t))."""
    logits = np.asarray(logits)
    cut = 0.0 if threshold == 0.5 else float(np.log(threshold / (1.0 - threshold)))
    return logits > cut


def pilot_logits(net, obs, n=None, chunk=16384):
    """hold_logit per row from a pilot GBNN (float32 numpy forward, like the mod). A value that
    isn't finite counts as 0, as it did in training (gbai.clean_rows); `gbai.py check` names
    the runs that have them. obs: a run's obs view (expanded a chunk at a time) or an array."""
    n = len(obs) if n is None else int(n)
    out = np.empty(n, np.float32)
    for a in range(0, n, chunk):
        out[a:min(a + chunk, n)] = export.forward(net, gbai.clean_rows(obs[a:min(a + chunk, n)]), np.float32)[:, 0]
    return out


def pilot_probs(net, obs, chunk=65536):
    """P(hold) per row (sigmoid of pilot_logits), for reports."""
    return export.sigmoid(pilot_logits(net, obs, chunk=chunk)).astype(np.float32)


def evaluate_run(net, run, threshold=0.5):
    """{"model": counts, "baseline": counts} over the run's usable ticks."""
    n = run.usable_ticks()
    act = np.asarray(run.act[:n])
    logits = pilot_logits(net, run.obs, n) if n else np.zeros(0, np.float32)
    prob = export.sigmoid(logits)
    model = transition_metrics(act, decide(logits, threshold), run.tps, prob)
    base = transition_metrics(act, gbai.before(act), run.tps)
    return {"model": model, "baseline": base}


def load_split(path):
    if path and os.path.exists(path):
        return gbai.read_json(path)
    return None


def group_of(run, split, showcase):
    """Where a run stands against the training split: trained / validation / showcase / unseen."""
    key = gbai.level_key(run)
    if split and key in set(split.get("train_levels", [])):
        return "trained"
    if showcase.run_reason(run):
        return "showcase"
    if split and key in set(split.get("val_levels", [])):
        return "validation"
    return "unseen"


GROUP_TITLES = {"validation": "Validation levels (held out from training)",
                "showcase": "Held-out exam levels (never trained on)",
                "unseen": "Other levels the model never trained on",
                "trained": "Levels it trained on (NOT held out; for comparison only)"}


def evaluate(net, runs, split, showcase, threshold=0.5, include_trained=False, log=print):
    """The report dict for these runs (grouped as group_of() says)."""
    groups = {}
    per_run = []
    skipped = []
    for run in runs:
        g = group_of(run, split, showcase)
        if g == "trained" and not include_trained:
            continue
        if run.layout_hash != net.layout_hash:
            skipped.append({"run_id": run.run_id, "why": f"recorded with layout {run.layout_hash}, the model "
                                                          f"expects {net.layout_hash}"})
            continue
        if run.usable_ticks() == 0:
            skipped.append({"run_id": run.run_id, "why": f"no usable ticks (validated {run.validated}, completed "
                                                          f"{run.completed})"})
            continue
        res = evaluate_run(net, run, threshold)
        run.close()
        per_run.append({"run_id": run.run_id, "level_id": run.level_id, "level_name": run.level_name,
                        "group": g, "usable_ticks": res["model"]["ticks"],
                        "model": rates(res["model"]), "baseline": rates(res["baseline"], with_prob=False)})
        acc = groups.setdefault(g, {"model": empty_counts(), "baseline": empty_counts()})
        acc["model"] = add_counts(acc["model"], res["model"])
        acc["baseline"] = add_counts(acc["baseline"], res["baseline"])
        m = per_run[-1]["model"]
        log(f"  {run.run_id} [{g}]: press within 2 {_pct(m['press_within_2'])}, release within 2 "
            f"{_pct(m['release_within_2'])}, false {_num(m['false_per_sec'])}/s")
    report = {"format": "gbai-eval", "version": 2, "created": gbai.now_iso(), "threshold": threshold,
              "decision": "hold when hold_logit > 0, ties release" if threshold == 0.5 else
              f"hold when P(hold) > {threshold}",
              "layout_hash": net.layout_hash, "tolerances": list(TOLERANCES),
              "groups": {g: {"title": GROUP_TITLES[g], "model": rates(v["model"]),
                             "baseline": rates(v["baseline"], with_prob=False)} for g, v in groups.items()},
              "runs": per_run, "skipped": skipped,
              "method": "teacher-forced: a decision counts at or before the human's tick; a late one counts "
                        "as missed (see evaluate.py)"}
    return report


def _pct(v):
    return "-" if v is None else f"{100.0 * v:.1f}%"


def _num(v):
    return "-" if v is None else f"{v:.2f}"


def markdown(report, model_path=""):
    lines = ["# Pilot evaluation", "",
             f"Model: `{model_path}` (layout {report['layout_hash']}), threshold {report['threshold']}. "
             f"{report['created']}.", "",
             "Each tick the pilot sees the human's own state, so a press shows up only at or before the human's "
             "tick: \"within k\" means up to k ticks early or on the tick, and a late one counts as missed. "
             "\"First decision\" is the strict version (no earlier decision since the human's last change). "
             "The baseline repeats the previous tick's button: it never decides anything, which is the point "
             "of showing it next to the pilot.", ""]
    order = ["validation", "unseen", "showcase", "trained"]
    for g in [g for g in order if g in report["groups"]]:
        grp = report["groups"][g]
        m, b = grp["model"], grp["baseline"]
        lines += [f"## {grp['title']}", "",
                  f"{m['runs']} runs, {m['ticks']} ticks ({m['seconds']:.0f} s), {m['presses']} presses, "
                  f"{m['releases']} releases.", "",
                  "| | Pilot | Baseline |", "|---|---|---|",
                  f"| Tick accuracy | {_pct(m['tick_accuracy'])} | {_pct(b['tick_accuracy'])} |"]
        for k in TOLERANCES:
            lines.append(f"| Presses within {k} | {_pct(m[f'press_within_{k}'])} | {_pct(b[f'press_within_{k}'])} |")
        for k in TOLERANCES:
            lines.append(f"| Releases within {k} | {_pct(m[f'release_within_{k}'])} | "
                         f"{_pct(b[f'release_within_{k}'])} |")
        lines += [f"| Presses, first decision within {STRICT_K} | {_pct(m[f'press_first_within_{STRICT_K}'])} | "
                  f"{_pct(b[f'press_first_within_{STRICT_K}'])} |",
                  f"| Releases, first decision within {STRICT_K} | {_pct(m[f'release_first_within_{STRICT_K}'])} | "
                  f"{_pct(b[f'release_first_within_{STRICT_K}'])} |",
                  f"| False transitions per second | {_num(m['false_per_sec'])} | {_num(b['false_per_sec'])} |",
                  ""]
    if report["skipped"]:
        lines += ["Skipped:", ""] + [f"- {s['run_id']}: {s['why']}" for s in report["skipped"]] + [""]
    if not report["groups"]:
        lines += ["No held-out runs to evaluate.", ""]
    return "\n".join(lines)


def main(argv=None, log=print):
    ap = argparse.ArgumentParser(description="Held-out numbers for the pilot (presses and releases vs a human).")
    ap.add_argument("--root", help="AI_ROOT (default: $GUCCIBOT_AI_ROOT or the mod's save folder)")
    ap.add_argument("--model", help="pilot.gbnn (default: AI_ROOT\\train\\pilot\\pilot.gbnn)")
    ap.add_argument("--split", help="split.json from train_pilot.py (default: next to the model)")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="hold when P > this; 0.5 is the mod's rule (logit > 0, ties release)")
    ap.add_argument("--runs", help="only these run ids, comma separated")
    ap.add_argument("--include-trained", action="store_true",
                    help="also report the levels it trained on (as their own group, never as held out)")
    ap.add_argument("--manifest", help="a manifest whose is_bar_level / held_out entries (by id) are held out too")
    ap.add_argument("--out", help="report JSON (default: eval_report.json next to the model); .md goes beside it")
    ap.add_argument("--threads", default="6", help="CPU threads for numpy, or auto (3 while Geometry Dash runs)")
    args = ap.parse_args(argv)
    import background
    background.set_below_normal_priority()
    gbai.limit_blas_threads(background.resolve_threads(args.threads))
    root = gbai.ai_root(args.root)
    model_path = args.model or os.path.join(root, "train", "pilot", "pilot.gbnn")
    net = gbai.read_gbnn(model_path)
    split_path = args.split or os.path.join(os.path.dirname(os.path.abspath(model_path)), "split.json")
    split = load_split(split_path)
    if split is None:
        log(f"NOTE: no split file at {split_path}: can't tell which levels it trained on, so every "
            "non-showcase run counts as 'unseen'. Pass --split.")
    runs, problems = gbai.list_runs(root, source="human")
    for path, why in problems:
        log(f"skipped (doesn't follow the contract): {why}")
    if args.runs:
        want = {r.strip() for r in args.runs.split(",") if r.strip()}
        runs = [r for r in runs if r.run_id in want]
    showcase = gbai.Showcase(manifest=args.manifest)
    log(f"Evaluating {model_path}")
    report = evaluate(net, runs, split, showcase, args.threshold, args.include_trained, log)
    report["model"] = os.path.abspath(model_path)
    report["split"] = os.path.abspath(split_path) if split else None
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(model_path)), "eval_report.json")
    gbai.write_json_atomic(out, report)
    md = os.path.splitext(out)[0] + ".md"
    gbai.write_bytes_atomic(md, markdown(report, model_path).encode("utf-8"))
    log(f"Wrote {out} and {md}")
    return report


if __name__ == "__main__":
    main()

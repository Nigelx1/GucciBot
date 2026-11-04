"""PPO for the AI pilot: the network learns from its own attempts (2026-11-03).

Like the Rocket League bots (RLGym + RocketSim + PPO): the game's copy of GD's
physics plays the CURRENT pilot with sampled choices (gucci_ai_rollout writes
every tick to AI_ROOT\\rl\\rollouts\\), and this file makes the choices that
went well more likely. No hand-written rules decide anything.

    actor   the pilot (models.Pilot, 494 -> 256 -> 256 -> hold_logit), started
            from the imitation pilot (AI_ROOT\\train\\pilot\\best.pt)
    critic  a value network (494 -> 256 -> 256 -> value), new, same input:
            how much reward is still to come from here
    GAE     gamma 0.999 a tick (240 tps: a horizon of about 4 s), lambda 0.95;
            an episode cut short (truncated) is bootstrapped with the value
    loss    clipped PPO (0.2) + entropy bonus + value loss, plus a small
            imitation loss on human ticks (a fixed sample of the training
            levels' human runs, never the exam levels) whose weight decays
            each iteration, so it stays human-like early and lets go later

State: AI_ROOT\\rl\\ppo.pt (both networks, both optimizers, the iteration).
Exports each update: AI_ROOT\\rl\\weights\\pilot.gbnn and critic.gbnn (golden
checked); install() copies pilot.gbnn into AI_ROOT\\weights\\ (the mod's
pilot; the first install keeps the imitation pilot as pilot_before_rl.gbnn).
The critic stays in rl\\weights: the mod's critic slot expects the old
alive_hold/alive_release heads and has no use for a value network yet.

    python tools/ai/rl_ppo.py update --files A.json B.json [--iteration N]
    python tools/ai/rl_ppo.py human-sample          # builds the imitation sample
"""
import argparse
import glob
import json
import math
import os
import shutil
import time

import numpy as np
import torch
import torch.nn.functional as F

import background
import export
import gbai
import models

ROLLOUT_ARRAYS = [(".state.f32", "<f4", gbai.STATE_DIM), (".scan.u8", "<u1", gbai.SCAN_DIM),
                  (".tok.u8", "<u1", gbai.TOK_DIM), (".geo.f32", "<f4", gbai.GEO_DIM), (".act.u8", "<u1", 1),
                  (".logp.f32", "<f4", 1), (".reward.f32", "<f4", 1), (".done.u8", "<u1", 1),
                  (".trunc.u8", "<u1", 1), (".tick.i32", "<i4", 1), (".ep.i32", "<i4", 1)]


def defaults():
    return dict(gamma=0.999, lam=0.95, clip=0.2, epochs=3, minibatch=4096, lr_actor=3e-5, lr_critic=3e-4,
                entropy=0.003, value_coef=1.0, imitation=0.5, imitation_decay=0.97, imitation_min=0.02,
                target_kl=0.03, grad_clip=0.5, value_warmup_epochs=3, human_ticks=200000, seed=0)


# The loop's own folder under AI_ROOT and the folder its pilot is installed to: a second loop (another pilot,
# e.g. "Accurate") sets both (rl_loop.py --rl-name / --weights-name) so the two never share a file.
RL_NAME = "rl"
WEIGHTS_NAME = "weights"


def rl_dir(root, *parts):
    p = os.path.join(root, RL_NAME, *parts)
    os.makedirs(p if not os.path.splitext(p)[1] else os.path.dirname(p), exist_ok=True)
    return p


class Value(models._MLP):
    """How much reward is still to come: 494 -> 256 -> 256 -> 1 ("value")."""

    model_kind = "value"
    out_names = ["value"]

    def __init__(self, n_in, hidden=(256, 256)):
        super().__init__(n_in, hidden, 1, gbai.ACT_NONE)

    def forward(self, x):
        return self.logits(x)[..., 0]

    def config(self):
        return {"kind": "value", "n_in": self.n_in, "hidden": list(self.hidden)}


# ---- data ------------------------------------------------------------------------

def load_rollouts(files, layout_hash=None):
    """Concatenates rollout parts. Returns (dict of arrays, headers). Episode ids are made unique."""
    parts, headers, ep_base = {k: [] for k, _, _ in ROLLOUT_ARRAYS}, [], 0
    temps, lens, levels = [], [], []
    for path in files:
        h = gbai.read_json(path)
        if h.get("format") != "gbai-rollout":
            raise gbai.DataError(f"{path}: not a rollout")
        if layout_hash and h["layout_hash"] != layout_hash:
            raise gbai.DataError(f"{path}: layout {h['layout_hash']}, the pilot's is {layout_hash}")
        n = int(h["rows"])
        base = path[:-len(".json")]
        for ext, dt, w in ROLLOUT_ARRAYS:
            a = np.fromfile(base + ext, dtype=dt)
            a = a.reshape(n, w) if w > 1 else a.reshape(n)
            parts[ext].append(a)
        ep = parts[".ep.i32"][-1].astype(np.int64)
        parts[".ep.i32"][-1] = ep - ep.min() + ep_base if n else ep
        ep_base = int(parts[".ep.i32"][-1].max()) + 1 if n else ep_base
        temps.append(np.full(n, float(h.get("temperature") or 1.0), np.float32))
        lens.append(np.full(n, float(h.get("level_length_x") or 0.0), np.float32))
        levels.append(np.full(n, int(h.get("level_id") or 0), np.int64))
        headers.append(h)
    d = {k[1:].split(".")[0]: (np.concatenate(v) if v else np.zeros(0)) for k, v in parts.items()}
    d["temp"] = np.concatenate(temps) if temps else np.zeros(0, np.float32)
    d["level_length"] = np.concatenate(lens) if lens else np.zeros(0, np.float32)
    d["level"] = np.concatenate(levels) if levels else np.zeros(0, np.int64)
    gbai.clean_compact(d["state"], np.ascontiguousarray(d["tok"]), d["geo"])
    return d, headers


def expand_rows(d, idx):
    t = torch.from_numpy
    return gbai.expand_torch(t(d["state"][idx]), t(d["scan"][idx]), t(np.ascontiguousarray(d["tok"][idx])),
                             t(d["geo"][idx]))


def gae(reward, value, done, trunc, ep, gamma, lam):
    """Advantages and returns. An episode's last row: done -> no value after it; truncated (or the batch's
    end) -> bootstrapped with that row's own value (the state after it isn't stored)."""
    n = len(reward)
    adv = np.zeros(n, np.float32)
    nxt_adv, nxt_v = 0.0, 0.0
    for i in range(n - 1, -1, -1):
        last = i == n - 1 or ep[i + 1] != ep[i]
        if done[i]:
            nv, carry = 0.0, 0.0
        elif last:
            nv, carry = float(value[i]), 0.0
        else:
            nv, carry = nxt_v, nxt_adv
        delta = float(reward[i]) + gamma * nv - float(value[i])
        a = delta + gamma * lam * carry
        adv[i] = a
        nxt_adv, nxt_v = a, float(value[i])
    return adv, adv + value.astype(np.float32)


def episode_stats(d):
    """Per episode: return, progress (blocks of dx over the level's length), how it ended."""
    ep = d["ep"]
    if len(ep) == 0:
        return {"episodes": 0}
    cut = np.flatnonzero(np.diff(ep)) + 1
    starts = np.concatenate([[0], cut])
    rets = np.add.reduceat(d["reward"].astype(np.float64), starts)
    ends = np.concatenate([cut, [len(ep)]]) - 1
    died = d["done"][ends].astype(bool) & (d["reward"][ends] < -0.5)
    return {"episodes": int(len(starts)), "mean_return": float(rets.mean()),
            "death_rate": float(died.mean()), "mean_ticks": float(len(ep) / len(starts))}


def human_sample(root, n=200000, seed=0, log=print):
    """A fixed sample of human ticks from the training levels (never the exam levels), cached in
    rl\\human_sample.npz: compact rows + the human's act."""
    path = os.path.join(rl_dir(root), "human_sample.npz")
    runs, _ = gbai.list_runs(root)
    runs, layout_hash = gbai.pick_layout(runs)
    if os.path.exists(path):
        z = np.load(path)
        if str(z["layout_hash"]) == layout_hash:
            return {k: z[k] for k in ("state", "scan", "tok", "geo", "act")}
    show = gbai.Showcase()
    use = []
    for r in runs:
        if show.run_reason(r):
            r.close()
            continue
        u = r.usable_ticks()
        if u > 0:
            use.append((r, u))
        r.close()
    total = sum(u for _, u in use)
    rng = np.random.default_rng(seed)
    out = {"state": [], "scan": [], "tok": [], "geo": [], "act": []}
    for r, u in use:
        k = max(1, int(round(n * u / total)))
        idx = np.sort(rng.integers(0, u, k))
        out["state"].append(np.array(r.state[idx]))
        out["scan"].append(np.array(r.scan[idx]))
        out["tok"].append(np.array(r.tok[idx]))
        out["geo"].append(np.array(r.geo[idx]))
        out["act"].append(np.array(r.act[idx]).reshape(-1))
        r.close()
    out = {k: np.concatenate(v) for k, v in out.items()}
    gbai.clean_compact(out["state"], out["tok"], out["geo"])
    np.savez(path, layout_hash=layout_hash, **out)
    log(f"human sample: {len(out['act'])} ticks from {len(use)} training runs -> {path}")
    return out


def training_levels(root):
    """{level_id: name} of levels with a usable human run, the exam levels left out."""
    runs, _ = gbai.list_runs(root)
    show = gbai.Showcase()
    out = {}
    for r in runs:
        if r.layout_hash and not show.run_reason(r) and abs(r.tps - 240.0) < 0.5 and r.usable_ticks() > 0:
            out[r.level_id] = r.level_name
        r.close()
    return out


# ---- the networks ----------------------------------------------------------------

def imitation_pilot_path(root):
    return os.path.join(root, "train", "pilot", "best.pt")


def load_state(root, cfg, log=print):
    """(actor, critic, opt_a, opt_c, state dict). From rl\\ppo.pt, else the imitation pilot + a new critic."""
    path = os.path.join(rl_dir(root), "ppo.pt")
    if os.path.exists(path):
        ck = models.load_checkpoint(path)
        actor = models.build(ck["actor_config"])
        actor.load_state_dict(ck["actor_state"])
        critic = Value(ck["critic_config"]["n_in"], ck["critic_config"]["hidden"])
        critic.load_state_dict(ck["critic_state"])
        st = ck["state"]
    else:
        actor, _ = models.from_checkpoint(imitation_pilot_path(root))
        critic = Value(actor.n_in, (256, 256))
        with torch.no_grad():
            critic.in_mean.copy_(actor.in_mean)
            critic.in_std.copy_(actor.in_std)
        st = {"iteration": 0, "updates": 0, "critic_warm": False, "layout_hash": None,
              "started_from": imitation_pilot_path(root)}
        log(f"PPO starts from the imitation pilot {imitation_pilot_path(root)} and a new critic")
    opt_a = torch.optim.Adam(actor.parameters(), lr=cfg["lr_actor"])
    opt_c = torch.optim.Adam(critic.parameters(), lr=cfg["lr_critic"])
    if os.path.exists(path):
        try:
            opt_a.load_state_dict(ck["opt_actor"])
            opt_c.load_state_dict(ck["opt_critic"])
        except (KeyError, ValueError):
            pass
    for g in opt_a.param_groups:
        g["lr"] = cfg["lr_actor"]
    for g in opt_c.param_groups:
        g["lr"] = cfg["lr_critic"]
    return actor, critic, opt_a, opt_c, st


def save_state(root, actor, critic, opt_a, opt_c, st):
    models.save_checkpoint(os.path.join(rl_dir(root), "ppo.pt"), {
        "actor_config": actor.config(), "actor_state": actor.state_dict(),
        "critic_config": critic.config(), "critic_state": critic.state_dict(),
        "opt_actor": opt_a.state_dict(), "opt_critic": opt_c.state_dict(), "state": st})


def layout_hash_of(root):
    net = gbai.read_gbnn(os.path.join(root, "train", "pilot", "pilot.gbnn"))
    return net.layout_hash


def export_nets(root, actor, critic, layout_hash, golden_inputs):
    wdir = rl_dir(root, "weights")
    a = export.export_model(actor, layout_hash, os.path.join(wdir, "pilot.gbnn"), golden_inputs, note="PPO actor")
    c = export.export_model(critic, layout_hash, os.path.join(wdir, "critic.gbnn"), golden_inputs, note="PPO value")
    return a, c


def install(root, log=print):
    """rl\\weights\\pilot.gbnn -> weights\\pilot.gbnn (golden-checked; the imitation pilot kept once)."""
    src = os.path.join(rl_dir(root, "weights"), "pilot.gbnn")
    ok, worst, _ = export.check_golden(src)
    if not ok:
        raise RuntimeError(f"{src} doesn't reproduce its golden vectors (worst {worst:.3g}); not installed")
    wdir = os.path.join(root, WEIGHTS_NAME)
    os.makedirs(wdir, exist_ok=True)
    dst = os.path.join(wdir, "pilot.gbnn")
    keep = os.path.join(wdir, "pilot_before_rl.gbnn")
    if os.path.exists(dst) and not os.path.exists(keep):
        shutil.copyfile(dst, keep)
        log(f"kept the pilot that was installed as {keep}")
    with open(src, "rb") as f:
        gbai.write_bytes_atomic(dst, f.read())
    return dst


# ---- the update ------------------------------------------------------------------

def update(root, files, cfg=None, log=print):
    """One PPO update on the rollout files. Returns the stats dict (also the exported files)."""
    cfg = dict(defaults(), **(cfg or {}))
    t0 = time.time()
    models.set_seed(cfg["seed"])
    actor, critic, opt_a, opt_c, st = load_state(root, cfg, log)
    layout_hash = st.get("layout_hash") or layout_hash_of(root)
    st["layout_hash"] = layout_hash
    d, headers = load_rollouts(files, layout_hash)
    n = len(d["act"])
    if n == 0:
        raise RuntimeError("the rollouts hold no ticks")
    human = human_sample(root, cfg["human_ticks"], log=log)
    nh = len(human["act"])
    pos = float(human["act"].mean()) if nh else 0.5
    hw = torch.tensor([0.5 / max(1e-3, 1 - pos), 0.5 / max(1e-3, pos)])  # release, hold: class-balanced
    rng = np.random.default_rng(cfg["seed"] + st["updates"])

    def values():
        out = np.zeros(n, np.float32)
        with torch.no_grad():
            for a in range(0, n, 32768):
                idx = np.arange(a, min(n, a + 32768))
                out[idx] = critic(expand_rows(d, idx)).numpy()
        return out

    act = torch.from_numpy(d["act"].astype(np.float32))
    old_logp = torch.from_numpy(d["logp"].astype(np.float32))
    temp = torch.from_numpy(d["temp"])
    mb = int(cfg["minibatch"])

    def critic_epochs(k, ret):
        losses = []
        rt = torch.from_numpy(ret)
        for _ in range(k):
            order = rng.permutation(n)
            for a in range(0, n, mb):
                idx = order[a:a + mb]
                loss = F.mse_loss(critic(expand_rows(d, idx)), rt[idx])
                opt_c.zero_grad()
                loss.backward()
                torch.nn.utils.clip_grad_norm_(critic.parameters(), cfg["grad_clip"])
                opt_c.step()
                losses.append(loss.item())
        return float(np.mean(losses)) if losses else 0.0

    v = values()
    adv, ret = gae(d["reward"], v, d["done"], d["trunc"], d["ep"], cfg["gamma"], cfg["lam"])
    warm = None
    if not st.get("critic_warm"):
        # A new critic knows nothing: fit it to this batch's returns before the actor listens to it.
        warm = critic_epochs(int(cfg["value_warmup_epochs"]), ret)
        st["critic_warm"] = True
        v = values()
        adv, ret = gae(d["reward"], v, d["done"], d["trunc"], d["ep"], cfg["gamma"], cfg["lam"])
    advt = torch.from_numpy((adv - adv.mean()) / (adv.std() + 1e-8))
    rett = torch.from_numpy(ret)
    imit_w = max(cfg["imitation_min"], cfg["imitation"] * cfg["imitation_decay"] ** st["iteration"]) \
        if cfg["imitation"] > 0 else 0.0
    stats = {"pl": [], "vl": [], "ent": [], "kl": [], "clip": [], "il": []}
    stopped_early = False
    for epoch in range(int(cfg["epochs"])):
        order = rng.permutation(n)
        kls = []
        for a in range(0, n, mb):
            idx = order[a:a + mb]
            x = expand_rows(d, idx)
            z = actor(x) / temp[idx]
            y = act[idx]
            logp = y * F.logsigmoid(z) + (1 - y) * F.logsigmoid(-z)
            ratio = torch.exp(logp - old_logp[idx])
            A = advt[idx]
            pl = -torch.min(ratio * A, torch.clamp(ratio, 1 - cfg["clip"], 1 + cfg["clip"]) * A).mean()
            p = torch.sigmoid(z)
            ent = -(p * F.logsigmoid(z) + (1 - p) * F.logsigmoid(-z)).mean()
            loss = pl - cfg["entropy"] * ent
            il = torch.tensor(0.0)
            if imit_w > 0 and nh:
                hi = rng.integers(0, nh, max(256, len(idx) // 4))
                t = torch.from_numpy
                xh = gbai.expand_torch(t(human["state"][hi]), t(human["scan"][hi]), t(human["tok"][hi]),
                                       t(human["geo"][hi]))
                yh = t(human["act"][hi].astype(np.float32))
                il = F.binary_cross_entropy_with_logits(actor(xh), yh, weight=hw[yh.long()])
                loss = loss + imit_w * il
            opt_a.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(actor.parameters(), cfg["grad_clip"])
            opt_a.step()
            vl = F.mse_loss(critic(x), rett[idx])
            opt_c.zero_grad()
            (cfg["value_coef"] * vl).backward()
            torch.nn.utils.clip_grad_norm_(critic.parameters(), cfg["grad_clip"])
            opt_c.step()
            with torch.no_grad():
                lr_ = logp - old_logp[idx]
                kl = float(((torch.exp(lr_) - 1) - lr_).mean())  # k3 estimator, never negative
            kls.append(kl)
            stats["pl"].append(pl.item())
            stats["vl"].append(vl.item())
            stats["ent"].append(ent.item())
            stats["kl"].append(kl)
            stats["clip"].append(float(((ratio - 1).abs() > cfg["clip"]).float().mean()))
            stats["il"].append(il.item())
        if np.mean(kls) > cfg["target_kl"]:
            stopped_early = True
            break
    st["iteration"] += 1
    st["updates"] += 1
    golden = expand_rows(d, np.linspace(0, n - 1, 6).astype(np.int64)).numpy()
    actor.eval(), critic.eval()
    a_info, c_info = export_nets(root, actor, critic, layout_hash, golden)
    actor.train(), critic.train()
    save_state(root, actor, critic, opt_a, opt_c, st)
    es = episode_stats(d)
    out = {"iteration": st["iteration"], "ticks": n, "files": len(files),
           "levels": sorted({int(h.get("level_id") or 0) for h in headers}),
           "mean_episode_return": es.get("mean_return"), "episodes": es.get("episodes"),
           "death_rate": es.get("death_rate"),
           "entropy": float(np.mean(stats["ent"])), "value_loss": float(np.mean(stats["vl"])),
           "policy_loss": float(np.mean(stats["pl"])), "approx_kl": float(np.mean(stats["kl"])),
           "clip_frac": float(np.mean(stats["clip"])), "imitation_loss": float(np.mean(stats["il"])),
           "imitation_weight": imit_w, "value_warmup_loss": warm, "stopped_early": stopped_early,
           "mean_value": float(v.mean()), "mean_return_target": float(ret.mean()),
           "seconds": round(time.time() - t0, 1), "pilot_sha256": a_info["sha256"], "critic_sha256": c_info["sha256"]}
    log("PPO update: " + json.dumps(out))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="PPO for the GucciBot AI pilot.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("update")
    p.add_argument("--files", nargs="+", required=True)
    p.add_argument("--root", default=None)
    background.add_args(p)
    p = sub.add_parser("human-sample")
    p.add_argument("--root", default=None)
    args = ap.parse_args(argv)
    root = gbai.ai_root(args.root)
    if args.cmd == "human-sample":
        human_sample(root)
        print(json.dumps(training_levels(root), indent=1))
        return
    background.setup(args)
    files = []
    for f in args.files:
        files += glob.glob(f)
    update(root, files)


if __name__ == "__main__":
    main()

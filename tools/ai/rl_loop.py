"""The reinforcement learning loop (2026-11-03): collect, learn, install, repeat.

Each iteration:
  1. rollout: a random handful of TRAINING levels (never the exam levels), each
     opened in our own game (tools/assist.py: safe mode forced, muted, never
     Nigel's game) and played by the current pilot with sampled choices
     (gucci_ai_rollout) for its share of --minutes. With --games N, N games
     (each its own Assistant Access port, --port-base + k) do this side by
     side, each on its own --levels levels, and all their ticks go into the
     one update; a game that crashes is brought back alone;
  2. PPO update on those ticks (rl_ppo.update);
  3. install the new pilot (AI_ROOT\\weights\\pilot.gbnn) and gucci_ai_reload;
  4. every --exam-every iterations (and before the first): a COLD EXAM, the
     greedy pilot, no practice, one attempt from the start on each tier 4-5
     exam level and 3 fixed training levels (gucci_ai_play max_attempts 1),
     on the first game.

Writes AI_ROOT\\rl\\curve.csv (one row an iteration) and rl\\progress.json;
state in rl\\loop_state.json, so running the same command again resumes.
A STOP file (AI_ROOT\\rl\\STOP) stops it after the step it is on. It brings a
crashed game back, and while a Geometry Dash no assist script started runs
(Nigel's), it closes its own and waits, checking every 5 minutes.

A second loop training another pilot (e.g. "Accurate", with the human pull
kept) gets its own folder and weights: --rl-name rl_accurate --weights-name
weights_accurate --port-base 8800. Its rollout files start with its folder's
name, so the two loops never touch each other's files.

    python tools/ai/rl_loop.py [--end "2026-11-08 05:00"] [--iterations N] ...
    python tools/ai/rl_loop.py --launch ...      # detached (outlives this session)
"""
import argparse
import csv
import datetime
import json
import os
import random
import sys
import threading
import time

import background
import gbai
import orchestrate
import rl_ppo

EXAMS = [(1, "Stereo Madness"), (5, "Base After Base"), (11, "Clutterfunk"), (13519, "The Nightmare"),
         (11402965, "Forest Temple"), (4545425, "Stalemate")]
OTHER_POLL = 300.0


def safe_name(name):
    """A folder name as a file-name prefix: letters, digits, - and _ only."""
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in name)


def parse_args(argv):
    ap = argparse.ArgumentParser(description="GucciBot AI: reinforcement learning loop (PPO).")
    ap.add_argument("--root", default=None)
    ap.add_argument("--end", default="2026-11-08 05:00", help="wall-clock end (local time)")
    ap.add_argument("--iterations", type=int, default=0, help="stop after this many iterations (0: until --end)")
    ap.add_argument("--minutes", type=float, default=4.0, help="rollout minutes an iteration (each game, all levels)")
    ap.add_argument("--levels", type=int, default=3, help="training levels an iteration (each game)")
    ap.add_argument("--max-ticks", type=int, default=600000, help="rollout ticks an iteration (each game)")
    ap.add_argument("--games", type=int, default=1, help="games playing rollouts side by side")
    ap.add_argument("--port-base", type=int, default=8790, help="Assistant Access port of the first game")
    ap.add_argument("--max-game-mb", type=float, default=3000,
                    help="a game using more private memory than this is closed and launched again between iterations")
    ap.add_argument("--rl-name", default="rl", help="this loop's folder under AI_ROOT")
    ap.add_argument("--weights-name", default="weights", help="the folder under AI_ROOT its pilot is installed to")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--max-episode-seconds", type=float, default=20.0)
    ap.add_argument("--start-mix", default="0.3,0.5,0.2", help="start,human,frontier")
    ap.add_argument("--slice-ms", type=float, default=30.0, help="AI work per game frame during rollouts")
    ap.add_argument("--exam-every", type=int, default=5)
    ap.add_argument("--exam-seconds", type=float, default=120.0)
    ap.add_argument("--keep-rollouts", type=int, default=2, help="iterations of rollout files kept on disk")
    ap.add_argument("--lr-actor", type=float, default=None)
    ap.add_argument("--imitation", type=float, default=None)
    ap.add_argument("--entropy", type=float, default=None, help="PPO entropy bonus (how hard it keeps exploring)")
    ap.add_argument("--imitation-decay", type=float, default=None,
                    help="the human pull's shrink per update (1 keeps it at --imitation)")
    ap.add_argument("--launch", action="store_true", help="start detached and return")
    return ap.parse_args(argv)


class Loop:
    def __init__(self, args, log):
        self.args = args
        self.log = log
        self.root = gbai.ai_root(args.root)
        rl_ppo.RL_NAME, rl_ppo.WEIGHTS_NAME = args.rl_name, args.weights_name
        self.dir = rl_ppo.rl_dir(self.root)
        # Rollout files: the mod writes them all to AI_ROOT\rl\rollouts; a second loop's start with its name.
        self.rollout_dir = os.path.join(self.root, "rl", "rollouts")
        os.makedirs(self.rollout_dir, exist_ok=True)
        self.prefix = "" if args.rl_name == "rl" else safe_name(args.rl_name) + "_"
        self.state_path = os.path.join(self.dir, "loop_state.json")
        self.curve = os.path.join(self.dir, "curve.csv")
        self.stop_path = os.path.join(self.dir, "STOP")
        self.end = datetime.datetime.strptime(args.end, "%Y-%m-%d %H:%M")
        self.state = gbai.read_json(self.state_path) if os.path.exists(self.state_path) else {}
        self.games = [None] * max(1, args.games)
        self.ports = [args.port_base + k for k in range(len(self.games))]
        mix = [float(v) for v in args.start_mix.split(",")]
        self.mix = {"start": mix[0], "human": mix[1], "frontier": mix[2]}
        self.cfg = {}
        if args.lr_actor is not None:
            self.cfg["lr_actor"] = args.lr_actor
        if args.imitation is not None:
            self.cfg["imitation"] = args.imitation
        if args.entropy is not None:
            self.cfg["entropy"] = args.entropy
        if args.imitation_decay is not None:
            self.cfg["imitation_decay"] = args.imitation_decay

    # ---- bookkeeping
    def save_state(self):
        gbai.write_json_atomic(self.state_path, self.state)

    def progress(self, **kw):
        doc = dict(self.state.get("last") or {}, updated=gbai.now_iso(), pid=os.getpid(), **kw)
        gbai.write_json_atomic(os.path.join(self.dir, "progress.json"), doc)

    def stopping(self):
        if os.path.exists(self.stop_path):
            return "STOP file"
        if datetime.datetime.now() >= self.end:
            return f"reached {self.args.end}"
        n = self.args.iterations
        if n and self.state.get("iteration", 0) - self.state.get("first_iteration", 0) >= n:
            return f"{n} iterations done"
        return None

    def exam_columns(self):
        return [f"exam_{lid}" for lid, _ in self.exams()]

    def exams(self):
        return EXAMS + [tuple(x) for x in self.state["fixed_training"]]

    def append_curve(self, row):
        cols = ["iteration", "time", "ticks", "ticks_per_s", "mean_episode_return", "mean_progress", "entropy",
                "value_loss", "approx_kl", "imitation_weight", "levels"] + self.exam_columns()
        new = not os.path.exists(self.curve)
        with open(self.curve, "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, cols, extrasaction="ignore")
            if new:
                w.writeheader()
            w.writerow(row)

    # ---- the games
    @property
    def game(self):
        """The first game that runs (exams)."""
        return next((g for g in self.games if g), None)

    def others(self):
        """Geometry Dash games no assist script started (Nigel's)."""
        sys.path.insert(0, orchestrate.TOOLS_DIR)
        import assist
        return assist.others()

    def ensure_game(self):
        """Every game up with this loop's weights loaded; only a missing or crashed one is launched again.
        While someone else's Geometry Dash runs, closes ours and waits."""
        while True:
            # Looked at twice: a game another loop has just started (or one Geode restarted) is unrecorded for a
            # moment, and must not pass for Nigel's.
            if self.others() and (time.sleep(10) or self.others()):
                if any(self.games):
                    self.log(f"a Geometry Dash this loop didn't start is running ({self.others()}); closing ours "
                             "and waiting")
                    self.close_all()
                self.progress(state="waiting", why="another Geometry Dash is running")
                if self.stopping():
                    return False
                time.sleep(OTHER_POLL)
                continue
            for k, g in enumerate(self.games):
                if g and not g.alive():
                    self.log(f"game {k} (port {self.ports[k]}) isn't running any more; launching it again")
                    self.close_game(k)
                elif g:
                    # A game that has played for hours holds several GB (seen: 5.4 GB after 3 hours); with
                    # several at once that adds up, so a big one starts fresh.
                    mb = self.memory_mb(k)
                    if mb and mb > self.args.max_game_mb:
                        self.log(f"game {k} uses {mb:.0f} MB; closing it and launching it again")
                        self.close_game(k)
                if not self.games[k]:
                    g = orchestrate.AssistGame(self.log, check_saves=False, port=self.ports[k])
                    g.__enter__()
                    self.games[k] = g
            break
        self.reload_all()
        return True

    def memory_mb(self, k):
        sys.path.insert(0, orchestrate.TOOLS_DIR)
        import assist
        rec = assist._recorded(self.ports[k])
        return assist.memory_mb(rec[0]) if rec else None

    def reload_all(self):
        for g in self.games:
            if g:
                g.call("gucci_ai_reload", dir=self.args.weights_name)

    def close_game(self, k):
        g = self.games[k]
        self.games[k] = None
        if g:
            try:
                g.__exit__(None, None, None)
            except Exception as e:  # noqa: BLE001 - closing must not stop the loop
                self.log(f"closing game {k}: {e}")

    def close_all(self):
        for k in range(len(self.games)):
            self.close_game(k)

    def recover_one(self, k):
        """Game k after a failed step: dropped when dead (launched again next time), else back in the menu."""
        g = self.games[k]
        if not g:
            return
        if not g.alive():
            self.close_game(k)
            return
        try:
            orchestrate.leave_level(g)
        except Exception:  # noqa: BLE001
            try:
                g.recover()
            except Exception as e2:  # noqa: BLE001
                self.log(f"recover of game {k} failed: {e2!r}")
                self.close_game(k)

    # ---- steps
    def rollouts(self, it, plan):
        """plan[k]: game k's levels. All games at once, one thread each. Returns (files, results) of every game
        that finished; raises only when none did."""
        a = self.args
        tag0 = f"{self.prefix}it{it:05d}_"
        for name in os.listdir(self.rollout_dir):
            if name.startswith(tag0):
                os.remove(os.path.join(self.rollout_dir, name))
        out = [None] * len(self.games)
        errors = [None] * len(self.games)

        def work(k):
            g, levels = self.games[k], plan[k]
            files, results = [], []
            secs = a.minutes * 60.0 / len(levels)
            for i, (lid, name) in enumerate(levels):
                orchestrate.open_level(g, lid)
                job = g.call("gucci_ai_rollout", level_id=lid, seconds=secs,
                             max_ticks=int(a.max_ticks / len(levels)), temperature=a.temperature,
                             seed=it * 1000 + k * 100 + i + 1, max_episode_seconds=a.max_episode_seconds,
                             start_mix=self.mix, tag=f"{tag0}g{k}_{lid}", slice_ms=a.slice_ms)["job"]
                st = orchestrate.wait_job(g, job, secs + 600, f"rollout {name or lid} (game {k})", self.log)
                if st.get("state") != "done":
                    raise RuntimeError(f"rollout on {lid} {st.get('state')}: {st.get('error')}")
                res = st["result"]
                files += res.get("files") or []
                results.append(res)
                self.log(f"  rollout {name or lid} (game {k}): {res['ticks']} ticks, {res['episodes']} episodes, "
                         f"{res['ticks_per_second']:.0f} ticks/s working ({res['ticks_per_wall_second']:.0f}/s "
                         f"wall), mean progress {100 * res['mean_progress']:.2f}%, best from start "
                         f"{res['best_percent_from_start']:.1f}%")
                orchestrate.leave_level(g)
            out[k] = (files, results)

        def guarded(k):
            try:
                work(k)
            except Exception as e:  # noqa: BLE001 - one game's trouble is that game's alone
                errors[k] = e

        threads = [threading.Thread(target=guarded, args=(k,), daemon=True)
                   for k in range(len(self.games)) if self.games[k] and plan[k]]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        files, results = [], []
        for k, e in enumerate(errors):
            if e is not None:
                self.log(f"  game {k}: rollouts failed ({e!r}); its ticks are left out of this update")
                self.recover_one(k)
        for got in out:
            if got:
                files += got[0]
                results += got[1]
        if not results:
            raise RuntimeError(f"no game finished its rollouts: {[repr(e) for e in errors if e]}")
        return files, results

    def exam(self):
        out = {}
        for lid, name in self.exams():
            try:
                orchestrate.open_level(self.game, lid)
                job = self.game.call("gucci_ai_play", max_attempts=1, max_seconds=self.args.exam_seconds,
                                     practice=False, judge=False)["job"]
                st = orchestrate.wait_job(self.game, job, self.args.exam_seconds + 300, f"exam {name}", self.log)
                pct = (st.get("result") or {}).get("best_percent")
                out[lid] = None if pct is None else round(float(pct), 2)
                self.log(f"  cold exam {name}: {out[lid]}%")
            finally:
                if self.game.alive():
                    orchestrate.leave_level(self.game)
        return out

    def first_time(self):
        """Fixed exam training levels; the imitation pilot exported and installed, so the first rollouts are
        played by exactly the network PPO starts from."""
        if "fixed_training" not in self.state:
            lv = sorted(rl_ppo.training_levels(self.root).items())
            self.state["training_levels"] = lv
            rnd = random.Random(0)
            self.state["fixed_training"] = rnd.sample(lv, 3)
            self.state.setdefault("iteration", 0)
            self.save_state()
        if not os.path.exists(os.path.join(self.dir, "ppo.pt")):
            import numpy as np
            import torch
            cfg = dict(rl_ppo.defaults(), **self.cfg)
            actor, critic, oa, oc, st = rl_ppo.load_state(self.root, cfg, self.log)
            st["layout_hash"] = rl_ppo.layout_hash_of(self.root)
            human = rl_ppo.human_sample(self.root, cfg["human_ticks"], log=self.log)
            t = torch.from_numpy
            idx = np.linspace(0, len(human["act"]) - 1, 6).astype(np.int64)
            golden = gbai.expand_torch(t(human["state"][idx]), t(human["scan"][idx]), t(human["tok"][idx]),
                                       t(human["geo"][idx])).numpy()
            actor.eval(), critic.eval()
            rl_ppo.export_nets(self.root, actor, critic, st["layout_hash"], golden)
            rl_ppo.save_state(self.root, actor, critic, oa, oc, st)
        rl_ppo.install(self.root, self.log)

    def threads(self):
        return 3 if self.others() else 6

    def run(self):
        self.first_time()
        self.state["first_iteration"] = self.state.get("iteration", 0)
        self.state["started"] = gbai.now_iso()
        self.save_state()
        self.log(f"RL loop: iteration {self.state['iteration']}, until {self.args.end}; settings "
                 f"{json.dumps(vars(self.args))}")
        rng = random.Random(int(time.time()))
        failures = 0
        while True:
            why = self.stopping()
            if why:
                self.log(f"stopping: {why}")
                break
            it = self.state["iteration"]
            try:
                if not self.ensure_game():
                    continue
                row = {"iteration": it, "time": gbai.now_iso()}
                if it % self.args.exam_every == 0 and self.state.get("examined") != it:
                    self.progress(state="exam", iteration=it)
                    ex = self.exam()
                    for lid, v in ex.items():
                        row[f"exam_{lid}"] = v
                    self.state["examined"] = it
                    self.state["last_exam"] = {"iteration": it, "percent": {str(k): v for k, v in ex.items()}}
                    self.save_state()
                    if self.stopping():
                        self.append_curve(row)
                        continue
                live = [k for k, g in enumerate(self.games) if g]
                pool = self.state["training_levels"]
                levels = rng.sample(pool, min(self.args.levels * len(live), len(pool)))
                plan = [[] for _ in self.games]
                for i, lv in enumerate(levels):
                    plan[live[i % len(live)]].append(lv)
                self.progress(state="rollout", iteration=it, levels=levels, games=len(live))
                t0 = time.time()
                files, results = self.rollouts(it, plan)
                roll_s = time.time() - t0
                self.progress(state="training", iteration=it)
                import models
                models.set_threads(self.threads())
                st = rl_ppo.update(self.root, files, self.cfg, self.log)
                rl_ppo.install(self.root, self.log)
                self.reload_all()
                ticks = sum(r["ticks"] for r in results)
                eps = sum(r["episodes"] for r in results) or 1
                row.update(ticks=ticks, ticks_per_s=round(ticks / max(1e-9, sum(r["work_seconds"] for r in results))),
                           mean_episode_return=round(st["mean_episode_return"], 4),
                           mean_progress=round(sum(r["mean_progress"] * r["episodes"] for r in results) / eps, 5),
                           entropy=round(st["entropy"], 4), value_loss=round(st["value_loss"], 5),
                           approx_kl=round(st["approx_kl"], 5), imitation_weight=round(st["imitation_weight"], 4),
                           levels=" ".join(str(l[0]) for l in levels))
                self.append_curve(row)
                self.state["iteration"] = it + 1
                self.state["last"] = dict(row, rollout_seconds=round(roll_s), update_seconds=st["seconds"],
                                          stopped_early=st["stopped_early"], games=len(live),
                                          rollout_ticks_per_wall_s=round(ticks / max(1e-9, roll_s)),
                                          ticks_per_hour=round(ticks * 3600 / max(1e-9, time.time() - t0)))
                self.save_state()
                self.progress(state="running", iteration=it + 1)
                self.prune(it)
                failures = 0
            except Exception as e:  # noqa: BLE001 - a failed iteration is retried, never the end of the loop
                failures += 1
                self.log(f"iteration {it} failed ({failures} in a row): {e!r}")
                self.progress(state="recovering", error=str(e))
                for k in range(len(self.games)):
                    self.recover_one(k)
                if failures >= 5:
                    self.log("5 failures in a row: waiting 10 minutes")
                    time.sleep(600)
        self.close_all()
        self.progress(state="stopped", why=self.stopping())
        try:
            os.remove(self.stop_path)
        except FileNotFoundError:
            pass

    def prune(self, it):
        n = len(self.prefix) + 8  # "<prefix>it00042_"
        keep = {f"{self.prefix}it{i:05d}_" for i in range(it - self.args.keep_rollouts + 1, it + 1)}
        rdir = self.rollout_dir
        for name in os.listdir(rdir):
            if name.startswith(self.prefix + "it") and name[n - 1:n] == "_" and name[:n] not in keep:
                try:
                    os.remove(os.path.join(rdir, name))
                except OSError:
                    pass


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parse_args(argv)
    if args.launch:
        root = gbai.ai_root(args.root)
        rl_ppo.RL_NAME = args.rl_name
        log = os.path.join(rl_ppo.rl_dir(root), "loop.log")
        rest = [a for a in argv if a != "--launch"]
        info = background.launch([os.path.abspath(__file__)] + rest, log)
        print(json.dumps(dict(info, log=log)))
        return
    background.set_below_normal_priority()

    def log(msg):
        print(time.strftime("%Y-%m-%d %H:%M:%S ") + str(msg), flush=True)

    loop = Loop(args, log)
    try:
        loop.run()
    finally:
        loop.close_all()


if __name__ == "__main__":
    main()

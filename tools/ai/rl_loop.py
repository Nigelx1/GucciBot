"""The reinforcement learning loop (2026-11-03): collect, learn, install, repeat.

Each iteration:
  1. rollout: a random handful of TRAINING levels (never the exam levels), each
     opened in our own game (tools/assist.py: safe mode forced, muted, never
     Nigel's game) and played by the current pilot with sampled choices
     (gucci_ai_rollout) for its share of --minutes;
  2. PPO update on those ticks (rl_ppo.update);
  3. install the new pilot (AI_ROOT\\weights\\pilot.gbnn) and gucci_ai_reload;
  4. every --exam-every iterations (and before the first): a COLD EXAM, the
     greedy pilot, no practice, one attempt from the start on each tier 4-5
     exam level and 3 fixed training levels (gucci_ai_play max_attempts 1).

Writes AI_ROOT\\rl\\curve.csv (one row an iteration) and rl\\progress.json;
state in rl\\loop_state.json, so running the same command again resumes.
A STOP file (AI_ROOT\\rl\\STOP) stops it after the step it is on. It brings a
crashed game back, and while a Geometry Dash it didn't start runs (Nigel's),
it closes its own and waits, checking every 5 minutes.

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
import time

import background
import gbai
import orchestrate
import rl_ppo

EXAMS = [(1, "Stereo Madness"), (5, "Base After Base"), (11, "Clutterfunk"), (13519, "The Nightmare"),
         (11402965, "Forest Temple"), (4545425, "Stalemate")]
OTHER_POLL = 300.0


def parse_args(argv):
    ap = argparse.ArgumentParser(description="GucciBot AI: reinforcement learning loop (PPO).")
    ap.add_argument("--root", default=None)
    ap.add_argument("--end", default="2026-11-08 05:00", help="wall-clock end (local time)")
    ap.add_argument("--iterations", type=int, default=0, help="stop after this many iterations (0: until --end)")
    ap.add_argument("--minutes", type=float, default=4.0, help="rollout minutes an iteration (all levels)")
    ap.add_argument("--levels", type=int, default=3, help="training levels an iteration")
    ap.add_argument("--max-ticks", type=int, default=600000, help="rollout ticks an iteration (all levels)")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--max-episode-seconds", type=float, default=20.0)
    ap.add_argument("--start-mix", default="0.3,0.5,0.2", help="start,human,frontier")
    ap.add_argument("--slice-ms", type=float, default=30.0, help="AI work per game frame during rollouts")
    ap.add_argument("--exam-every", type=int, default=5)
    ap.add_argument("--exam-seconds", type=float, default=120.0)
    ap.add_argument("--keep-rollouts", type=int, default=2, help="iterations of rollout files kept on disk")
    ap.add_argument("--lr-actor", type=float, default=None)
    ap.add_argument("--imitation", type=float, default=None)
    ap.add_argument("--launch", action="store_true", help="start detached and return")
    return ap.parse_args(argv)


class Loop:
    def __init__(self, args, log):
        self.args = args
        self.log = log
        self.root = gbai.ai_root(args.root)
        self.dir = rl_ppo.rl_dir(self.root)
        self.state_path = os.path.join(self.dir, "loop_state.json")
        self.curve = os.path.join(self.dir, "curve.csv")
        self.stop_path = os.path.join(self.dir, "STOP")
        self.end = datetime.datetime.strptime(args.end, "%Y-%m-%d %H:%M")
        self.state = gbai.read_json(self.state_path) if os.path.exists(self.state_path) else {}
        self.game = None
        mix = [float(v) for v in args.start_mix.split(",")]
        self.mix = {"start": mix[0], "human": mix[1], "frontier": mix[2]}
        self.cfg = {}
        if args.lr_actor is not None:
            self.cfg["lr_actor"] = args.lr_actor
        if args.imitation is not None:
            self.cfg["imitation"] = args.imitation

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

    # ---- the game
    def others(self):
        a = self.game.assist if self.game else None
        if a is None:
            sys.path.insert(0, orchestrate.TOOLS_DIR)
            import assist as a
        mine = a._recorded()[0] if a.ours() else None
        return [p for p in a._running(os.path.basename(a.GD_EXE)) if p != mine]

    def ensure_game(self):
        """Our game, up and with the weights loaded. Waits while someone else's Geometry Dash runs."""
        while True:
            others = self.others() if self.game else []
            if self.game and others:
                self.log(f"a Geometry Dash this loop didn't start is running ({others}); closing ours and waiting")
                self.close_game()
            if not self.game:
                sys.path.insert(0, orchestrate.TOOLS_DIR)
                import assist
                if assist._running(os.path.basename(assist.GD_EXE)):
                    self.progress(state="waiting", why="another Geometry Dash is running")
                    if self.stopping():
                        return False
                    time.sleep(OTHER_POLL)
                    continue
                g = orchestrate.AssistGame(self.log, check_saves=False)
                g.__enter__()
                self.game = g
            if self.game.alive():
                break
            self.close_game()
        self.game.call("gucci_ai_reload")
        return True

    def close_game(self):
        if self.game:
            try:
                self.game.__exit__(None, None, None)
            except Exception as e:  # noqa: BLE001 - closing must not stop the loop
                self.log(f"closing the game: {e}")
            self.game = None

    # ---- steps
    def rollouts(self, it, levels):
        a = self.args
        tag0 = f"it{it:05d}_"
        rdir = rl_ppo.rl_dir(self.root, "rollouts")
        for name in os.listdir(rdir):
            if name.startswith(tag0):
                os.remove(os.path.join(rdir, name))
        files, results = [], []
        secs = a.minutes * 60.0 / len(levels)
        for i, (lid, name) in enumerate(levels):
            orchestrate.open_level(self.game, lid)
            job = self.game.call("gucci_ai_rollout", level_id=lid, seconds=secs,
                                 max_ticks=int(a.max_ticks / len(levels)), temperature=a.temperature,
                                 seed=it * 1000 + i + 1, max_episode_seconds=a.max_episode_seconds,
                                 start_mix=self.mix, tag=f"{tag0}{lid}", slice_ms=a.slice_ms)["job"]
            st = orchestrate.wait_job(self.game, job, secs + 600, f"rollout {name or lid}", self.log)
            if st.get("state") != "done":
                raise RuntimeError(f"rollout on {lid} {st.get('state')}: {st.get('error')}")
            res = st["result"]
            files += res.get("files") or []
            results.append(res)
            self.log(f"  rollout {name or lid}: {res['ticks']} ticks, {res['episodes']} episodes, "
                     f"{res['ticks_per_second']:.0f} ticks/s working ({res['ticks_per_wall_second']:.0f}/s wall), "
                     f"mean progress {100 * res['mean_progress']:.2f}%, best from start "
                     f"{res['best_percent_from_start']:.1f}%")
            orchestrate.leave_level(self.game)
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
        return 3 if self.game is None and background.game_running() or (self.game and self.others()) else 6

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
                levels = rng.sample(self.state["training_levels"],
                                    min(self.args.levels, len(self.state["training_levels"])))
                self.progress(state="rollout", iteration=it, levels=levels)
                t0 = time.time()
                files, results = self.rollouts(it, levels)
                roll_s = time.time() - t0
                self.progress(state="training", iteration=it)
                import models
                models.set_threads(self.threads())
                st = rl_ppo.update(self.root, files, self.cfg, self.log)
                rl_ppo.install(self.root, self.log)
                self.game.call("gucci_ai_reload")
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
                                          stopped_early=st["stopped_early"])
                self.save_state()
                self.progress(state="running", iteration=it + 1)
                self.prune(it)
                failures = 0
            except (RuntimeError, TimeoutError, OSError, KeyError, ValueError) as e:
                failures += 1
                self.log(f"iteration {it} failed ({failures} in a row): {e!r}")
                self.progress(state="recovering", error=str(e))
                if self.game and not self.game.alive():
                    self.close_game()
                elif self.game:
                    try:
                        self.game.recover()
                    except Exception as e2:  # noqa: BLE001
                        self.log(f"recover failed: {e2!r}")
                        self.close_game()
                if failures >= 5:
                    self.log("5 failures in a row: waiting 10 minutes")
                    time.sleep(600)
        self.close_game()
        self.progress(state="stopped", why=self.stopping())
        try:
            os.remove(self.stop_path)
        except FileNotFoundError:
            pass

    def prune(self, it):
        rdir = rl_ppo.rl_dir(self.root, "rollouts")
        keep = {f"it{i:05d}_" for i in range(it - self.args.keep_rollouts + 1, it + 1)}
        for name in os.listdir(rdir):
            if name.startswith("it") and name[:8] not in keep:
                try:
                    os.remove(os.path.join(rdir, name))
                except OSError:
                    pass


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parse_args(argv)
    if args.launch:
        root = gbai.ai_root(args.root)
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
        loop.close_game()


if __name__ == "__main__":
    main()

#pragma once

// Reinforcement learning, the game's half (2026-11-03): the pilot plays the
// open level in the AI's own session of GD's physics, again and again, and
// every tick it plays is written down for the trainer (tools/ai/rl_ppo.py):
// what it saw, what it chose, how likely that choice was, and the reward.
// The choice is SAMPLED from the pilot's own probability (at a temperature),
// so it tries things; the trainer then makes the choices that went well more
// likely. No hand-written rules decide anything here.
//
// Episodes start at the level's start, at a point along the human run of the
// level (its recorded act replayed in the copies, like RLGym's replay
// states), or at the furthest point this job's own episodes reached. An
// episode ends at a death, at the level's end, or after max_episode_seconds
// of game time (cut short, not failed).
//
// Files: <AI_ROOT>\rl\rollouts\<tag>_<n>.json + per-tick arrays
// (.state.f32 .scan.u8 .tok.u8 .geo.f32 as the recorder writes them, then
// .act.u8 the sampled choice, .logp.f32 its log-probability, .reward.f32,
// .done.u8, .trunc.u8, .tick.i32 the tick from the level's start, .ep.i32).

#include "ai/job.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gucci::ai::rollout {

    struct Options {
        double seconds = 60.0;          // working time (wall clock)
        int64_t maxTicks = 400000;      // stop after this many ticks are written
        double temperature = 1.0;       // P(hold) = sigmoid(logit / temperature)
        uint64_t seed = 1;
        double maxEpisodeSeconds = 30.0;  // game time; longer episodes are cut (truncated)
        double mixStart = 0.4;          // where episodes start: the level's start,
        double mixHuman = 0.4;          // a point on the human run,
        double mixFrontier = 0.2;       // the furthest point reached so far
        std::string tag = "rollout";    // file names: <tag>_<n>
        double sliceMs = 0.0;           // > 0: the AI's slice per frame while this runs
        int64_t levelId = 0;            // checked against the open level (0: any)
    };

    // The reward of one tick, in one place so it is easy to change: progress
    // (dx over the level's length, a whole level is about +10), -1 for a
    // death, +2 for finishing.
    float reward(float dx, float levelLength, bool died, bool completed);

    std::unique_ptr<job::Job> make(Options const& options, std::string& why);

} // namespace gucci::ai::rollout

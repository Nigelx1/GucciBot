#pragma once

// The AI pilot plays and practices (stage D of the AI pilot build,
// 2026-11-02). Trained networks (src/ai/net.*) decide every tick from what
// the pilot sees (src/ai/observe.*), in the AI's own session of GD's physics
// (src/ai/sim.hpp). Nothing here calls the old search.
//
// How it practices (DESIGN.md, "How it practices"): play from the level's
// start with the pilot; on a death, name what killed it, pick the decision
// that was most likely the mistake (the critic's advantage when a critic is
// loaded, else the pilot's least-sure decisions near its last presses and
// releases, most recent first), change it, go back to a saved state from
// just before it and let the pilot carry on. A change that gets further is
// kept; the same death again tries the next mistake; when no change near a
// death gets past it, the changes get longer and reach further back, and
// last of all it goes back to the death before. Every death gets a plain
// sentence. A completed run is saved as a GucciBot macro and played in the
// real game (absense::judge) to confirm it.

#include "ai/job.hpp"
#include "ai/net.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

#include <matjson.hpp>

namespace gucci::ai::pilot {

    // <AI_ROOT>\weights (the trainer's exports go there), or the folder in
    // GUCCIBOT_AI_WEIGHTS, or the one given to setWeightsDir (gucci_ai_reload
    // dir): several games, each with its own pilot (2026-11-03). A relative
    // folder is under AI_ROOT.
    std::filesystem::path weightsDir();
    // For the rest of this launch; empty goes back to the default above.
    void setWeightsDir(std::string const& dir);

    // Loads pilot.gbnn, critic.gbnn and reader.gbnn from weightsDir(), else
    // the copies shipped with the mod (resources/ai: only pilot.gbnn ships,
    // since 2026-11-19). A network trained for another layout is refused with
    // a message saying which. Returns {models: [...] (each with built_in),
    // pilot_loaded, critic_loaded, reader_loaded, obs_layout_hash,
    // tape_layout_hash, weights_dir}.
    matjson::Value reload();
    // The pilot is loaded (loads once on first ask).
    bool ready();
    // One plain line for the menu: which pilot is loaded (the built-in one or
    // the user's own), or why none is.
    std::string weightsLine();
    // The loaded pilot for the other AI jobs (the rollout), or null; its
    // hold_logit output and the file it came from.
    net::Net* pilotNet(int& holdIndex, std::string& path);

    struct PlayOptions {
        int maxAttempts = 50000;  // a few thousand a second on easy levels
        double maxSeconds = 300.0;
        std::string saveMacro;      // empty: "AI - <level name>"
        bool logPractice = false;   // write practice logs (AI contract v2 section 6)
        bool judge = true;          // confirm a completed run in the real game
        // The old hand-written practice rules (comparison only, 2026-11-03).
        // Off: the network alone plays. Its first attempt is its own best
        // guess (greedy); later attempts start from the level's start again
        // and sample from its own odds at `temperature`.
        bool practice = false;
        double temperature = 1.0;
        // Self-training (2026-11-19), with the network alone: each time a
        // sampled run gets further (a block past the last time it trained, up
        // to a quarter second before the death, while learning has taken at
        // most half the playing time), the pilot trains on what that run did
        // (up to 60 passes, in the job's slices); the next attempt plays the trained
        // network's best guess, and the trained network is kept, and saved for
        // the level (weightsDir()/levels/<level id>.gbnn), only if that gets
        // further than the kept one's did. A finished sampled run is learned
        // until every decision of it is the network's own. Off with Practice
        // (its line replays the network's decisions).
        bool selfTrain = true;
        // Play with the level's own trained network when there is one (from
        // the same pilot), before pilot.gbnn and the built-in one.
        bool levelNet = true;
    };
    // Where a level's trained network is kept.
    std::filesystem::path levelNetPath(int64_t levelId);
    // Plays the open level. Null with `why` when it can't start.
    std::unique_ptr<job::Job> makePlay(PlayOptions const& options, std::string& why);

    // For the menu: the play job running now (0: none), and the newest lines
    // it wrote (death sentences, the outcome).
    struct Live {
        int job = 0;
        float bestPercent = 0.0f;
        int attempts = 0;
        std::deque<std::string> lines;  // newest last, at most 8
    };
    Live const& live();
    void setLiveJob(int id);

    // The Pathfinder page's engine: the AI or the search (the default).
    bool engineAi();
    void setEngineAi(bool on);

} // namespace gucci::ai::pilot

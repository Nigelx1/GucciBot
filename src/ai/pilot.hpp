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

    // <AI_ROOT>\weights (the trainer's exports go there).
    std::filesystem::path weightsDir();

    // Loads pilot.gbnn, critic.gbnn and reader.gbnn from weightsDir(), else
    // the copies shipped with the mod (resources/ai; none yet). A network
    // trained for another layout is refused with a message saying which.
    // Returns {models: [...], pilot_loaded, critic_loaded, reader_loaded,
    // obs_layout_hash, tape_layout_hash, weights_dir}.
    matjson::Value reload();
    // The pilot is loaded (loads once on first ask).
    bool ready();
    // One plain line for the menu: what is loaded, or why nothing is.
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
    };
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

    // The Pathfinder page's engine: the AI (default) or the old search.
    bool engineAi();
    void setEngineAi(bool on);

} // namespace gucci::ai::pilot

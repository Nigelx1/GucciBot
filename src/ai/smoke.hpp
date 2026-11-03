#pragma once

// gucci_ai_sim_smoke (stage A, 2026-11-02): a human macro of any format
// (read through gucci::convert::read) played in the AI's session from the
// level's start. Answers whether the AI's access to GD's physics carries a
// known-good run to the end: {ticks, completed, died_tick, died_x, final_x}.

#include "ai/job.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gucci::ai {

    // Null with `why` when the macro can't be read or the level isn't the
    // one open. levelId 0 skips the level check; maxTicks 0: until the macro's
    // last input plus a minute. probe: every tick's state of both copies goes
    // into gucci_tick_probe's reference (to diff against the real game).
    std::unique_ptr<job::Job> makeSimSmoke(std::string const& macroPath, int64_t levelId, int offset,
                                           int64_t maxTicks, std::string& why, bool probe = false);

} // namespace gucci::ai

#pragma once

// The AI pilot's recorder and its helpers (stage B, 2026-11-02; AI contract
// v2 sections 4, 5 and 8). Every one is a job (src/ai/job.hpp): queued,
// worked in slices, polled with gucci_ai_status.
//
//  record        a human macro of any format played in the AI's session of
//                GD's physics from the level's start, at the macro's tick
//                rate, for each frame offset asked for until one completes;
//                the completing one (or the furthest) is played again with
//                the observer on and written to <AI_ROOT>\data as the
//                contract's per-tick record, with golden expanded rows, and
//                the level's tape to <AI_ROOT>\tapes if it has none yet.
//  validate_real the same macro, shifted by an offset, saved as a zz_gbtest_
//                GucciBot macro and played by GucciBot's own playback in the
//                real game (safe mode required): completed, or where it died.
//                The copy is deleted after.
//  find_level    a name searched on GD's servers: the levels it finds.

#include "ai/job.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <matjson.hpp>

namespace gucci::ai {

    // <the mod's save folder>\ai
    std::filesystem::path aiRoot();

    // Opens a level by id the way gucci_open_level does (Assistant Access
    // hands its own tools in): the answer, which may be {downloading: true}.
    using ToolCall = std::function<matjson::Value(char const* tool, matjson::Value const& args)>;

    std::unique_ptr<job::Job> makeRecord(std::string const& macroPath, int64_t levelId, std::vector<int> offsets,
                                         bool offsetsGiven, double tps, ToolCall tools, std::string& why);
    std::unique_ptr<job::Job> makeValidateReal(std::string const& macroPath, int64_t levelId, int frameOffset,
                                               ToolCall tools, std::string& why);
    std::unique_ptr<job::Job> makeFindLevel(std::string const& name, std::string& why);

} // namespace gucci::ai

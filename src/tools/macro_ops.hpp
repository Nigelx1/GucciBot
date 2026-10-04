#pragma once

// Macro Tools' work, on GBR6 files rather than on the loaded macro: Diff,
// Trim, Merge, the header (Metadata) and mid-macro TPS changes. Pure: no
// game, Geode or ImGui, so tools/run_macroops_tests.cpp runs it outside the
// game, like edit_core and Check Macro. Written fresh for 2.0.
//
// A file is read into a Macro the way GucciReplaySystem::load reads it into
// the action list, and written back the way GucciReplaySystem::save writes
// one (both in core/engine_core.cpp): inputs through ActionAtom::addAction,
// deaths and restarts as actions, sub-tick offsets matched onto their input,
// TPS changes as actions, then a stable sort by frame. If either side
// changes how a macro is stored, change the other with it.
//
// Every operation that builds a new macro runs Check Macro
// (tools/macro_check.hpp) on the result and refuses it if it has more
// problems than its source(s) had, so Trim and Merge can never leave a press
// without its release or a release without its press. A source that is
// already broken can still be trimmed or merged; the result says how many of
// its problems came along.
//
// Frames are the macro's own numbers (CLAUDE.md section 4): an action at
// frame F is handed over while the game is on F - 1, and the first frame
// playback ever asks for is 1, so nothing is put at frame 0.

#include "core/action_types.hpp"
#include "core/gbr6_format.hpp"
#include "tools/macro_check.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gucci::macroops {

    using Actions = std::vector<gb::Action>;

    inline constexpr uint32_t kFirstFrame = 1;

    struct Macro {
        // name, levelName, tps (the rate the macro STARTS at), rngSeed and
        // timestamp are what GucciBot reads back. The section flags are
        // worked out again on every write.
        GBR6Header header;
        Actions actions;  // stable-sorted by frame
    };

    // ---- files

    Macro fromFile(GBR6File const& f);
    GBR6File toFile(Macro const& m);

    // Reads a GBR6 macro. Anything else (or a damaged file) is a nullopt with
    // the reason in *error.
    std::optional<Macro> load(std::filesystem::path const& path, std::string* error);
    // Writes through a temporary file next to the target and renames it over,
    // so a failed write never leaves half a macro where a whole one was.
    bool save(Macro const& m, std::filesystem::path const& path, std::string* error);

    // ---- facts

    struct Stats {
        uint32_t inputs = 0;     // press and release events
        uint32_t clicks = 0;     // complete press..release pairs
        uint32_t resets = 0;     // deaths and restarts in the macro
        uint32_t subticks = 0;   // inputs with a sub-tick offset
        uint32_t tpsChanges = 0;
        uint32_t lastFrame = 0;  // frame of the last action
        uint32_t problems = 0;   // Check Macro findings
        bool player2 = false;    // any P2 input
    };
    Stats stats(Macro const& m);

    // The rate the macro runs at once every action before `frame` has been
    // handed over: the header's, then each TPS change in turn.
    double tpsBefore(Macro const& m, uint32_t frame);

    struct TpsChange {
        uint32_t frame = 0;
        double tps = 0.0;
    };
    std::vector<TpsChange> tpsChanges(Macro const& m);

    // Adds a TPS change at `frame`, or changes the rate of the one already
    // there. It goes last among the actions on its frame, where loading the
    // file puts it too; every action on a frame is handed over before that
    // tick's physics runs, so the tick runs at the new rate either way. False
    // (and why) for frame 0 -- the header's TPS is the rate at the start --
    // or a rate that is not a positive number.
    bool setTpsChange(Macro& m, uint32_t frame, double tps, std::string* error);
    bool removeTpsChange(Macro& m, uint32_t frame);

    // ---- building new macros

    struct Outcome {
        bool ok = false;
        std::string message;  // why it was refused, or notes on the result
        Macro macro;
        macrocheck::Report report;
        uint32_t carriedProblems = 0;  // problems the source(s) already had
    };

    // Keeps the actions on frames start..end (both included). A button held
    // into the range is pressed again on `start`; one held past `end` that the
    // source lets go of later is released on end + 1, so every press in the
    // result has its release. The header's TPS becomes the rate in effect at
    // `start`. With rebase, everything moves so `start` lands on frame 1, the
    // first frame playback hands over.
    Outcome trim(Macro const& src, uint32_t start, uint32_t end, bool rebase);

    // B after A: B's frame f becomes (A's last frame + gap) + f. A button A
    // still holds at its end is let go of before B starts, and if B starts at
    // a different TPS than A ends at, a TPS change is put on B's first frame.
    // The header (level, seed) is A's.
    Outcome merge(Macro const& a, Macro const& b, uint32_t gap);

    // ---- diff

    enum class DiffKind : uint8_t {
        Moved,          // the same input in both, on different frames
        OnlyA,          // an action only A has
        OnlyB,          // an action only B has
        SubtickDiffers, // the same input on the same frame, at a different point in it
        TpsDiffers,     // a TPS change on the same frame, to a different rate
    };

    struct DiffItem {
        DiffKind kind = DiffKind::OnlyA;
        gb::ActionType type = gb::ActionType::Jump;
        int lane = -1;         // Check Macro's lane for inputs, -1 for the rest
        bool press = false;    // inputs: press or release
        uint32_t frameA = 0;   // OnlyB: unused
        uint32_t frameB = 0;   // OnlyA: unused
        double valueA = 0.0;   // sub-tick offset or TPS
        double valueB = 0.0;
        uint32_t frame() const { return kind == DiffKind::OnlyB ? frameB : frameA; }
    };

    struct Diff {
        std::vector<DiffItem> items;  // by frame
        uint32_t matched = 0;         // inputs both have on the same frame
        uint32_t moved = 0, onlyA = 0, onlyB = 0, subtick = 0, tps = 0;
        // Differences in what the two files carry besides their actions.
        bool startTpsDiffers = false;
        bool levelDiffers = false;
        bool seedDiffers = false;
        bool identical() const {
            return items.empty() && !startTpsDiffers && !levelDiffers && !seedDiffers;
        }
    };

    // Matches each lane's presses and releases between the two macros: first
    // the ones on the same frame, then, in order, an unmatched one in A with
    // an unmatched one in B at most `moveWindow` frames away (Moved). What is
    // left is in only one of them. Deaths, restarts and TPS changes are
    // matched by frame.
    Diff diff(Macro const& a, Macro const& b, uint32_t moveWindow);

} // namespace gucci::macroops

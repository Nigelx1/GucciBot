#pragma once

// The Frame Editor's rules: pure functions over the loaded macro's actions
// (GucciReplaySystem::m_actionAtom.m_actions), so the page (ui/pages/
// editor.cpp) and tools/run_edit_tests.cpp run exactly the same code. Nothing
// here touches the game, Geode or ImGui.
//
// Ported from Absense's macro editor (replay/edit_core.*, by Absent; GPL via
// Silicate) and cut down to what a timeline needs: its model of a macro as
// clicks in lanes, its lane rule (where a click may sit), its attempt rule (a
// death or restart in the macro is a wall nothing moves across), its ranges
// with their reasons, and its rule for where an inserted action goes among
// the actions already on its frame. Rewritten over gb::Action. The planners
// build a whole new list instead of Absense's hunks: a GucciBot macro is a
// few hundred thousand actions at most, a copy of that is a few megabytes,
// and History keeps only the part that changed.
//
// Every planner ends by running Check Macro (tools/macro_check.hpp) on its
// result and refuses an edit that would leave a problem the macro did not
// have: a press while the button is already held, a release with nothing
// held, or an action out of frame order. So an edit can never break a macro,
// whatever the rules above get wrong; a macro that already has problems can
// still be edited, as long as the edit adds none.
//
// Frames are the macro's own numbers (CLAUDE.md section 4): an input made
// while the game is on frame N is stored at N + 1, and playback hands it over
// when the game reaches N again. The first frame playback ever asks for is 1
// (lookupFrame = frame + 1 in GJBaseGameLayer::processQueuedButtons), and an
// input at 0 would sit at the front of the list unmatched, holding back every
// input after it (see GucciReplaySystem::onReset), so nothing is put there.

#include "core/action_types.hpp"
#include "tools/macro_check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gucci::edit {

    using Actions = std::vector<gb::Action>;

    inline constexpr uint32_t kNone = 0xFFFFFFFFu;
    inline constexpr uint32_t kFirstFrame = 1;
    // The frame counter is a uint32 and playback looks one frame ahead, so
    // the last frame it can ever ask for is one below the counter's limit.
    inline constexpr uint32_t kLastFrame = 0xFFFFFFFEu;
    inline constexpr int kLanes = 6;  // P1 Jump, Left, Right, P2 Jump, Left, Right

    // Lane index as Check Macro numbers them: p2 * 3 + button - 1.
    const char* laneName(int lane);  // "P1 Jump"

    // ------------------------------------------------------------------ model

    // A Click is a press and the release that ends it. A lone Press was never
    // released: held to the end, cut by a death or restart, or pressed again
    // before a release. A lone Release has no press before it (the recorder
    // writes one after a respawn let go of a button; anything else is a
    // problem Check Macro reports).
    enum class Kind : uint8_t { Click, Press, Release };

    struct Item {
        uint32_t press = kNone;    // action index of the press (Click, Press)
        uint32_t release = kNone;  // action index of the release (Click, Release)
        Kind kind = Kind::Click;
        int8_t lane = -1;
        // The item's key: the index of its first action. Valid until the list
        // changes; every edit hands back the new keys of what it touched.
        uint32_t key() const { return press != kNone ? press : release; }
        bool startsWithPress() const { return kind != Kind::Release; }
        bool endsWithRelease() const { return kind != Kind::Press; }
    };

    struct Model {
        std::vector<Item> items;                       // ascending key
        std::array<std::vector<uint32_t>, kLanes> lanes;  // item indices per lane, list order
        std::vector<uint32_t> resetFrames;             // frames of deaths and restarts, ascending
        std::vector<uint32_t> events;                  // action indices of everything that is not an input
        uint32_t lastFrame = 0;                        // the highest frame in the list
        bool sorted = true;
        uint32_t clicks = 0, lonePresses = 0, loneReleases = 0;
    };

    Model build(Actions const& a);

    uint32_t firstFrame(Actions const& a, Item const& it);
    // The release of a click, else the item's own frame.
    uint32_t lastFrame(Actions const& a, Item const& it);
    // How long a lone press stays down for the lane rule: the frame before
    // the next death or restart, or kLastFrame when none comes. For a click
    // or a release, lastFrame.
    uint32_t holdEnd(Actions const& a, Model const& m, Item const& it);
    // The item whose key is `key`, as an index into m.items, or kNone.
    uint32_t itemOfKey(Model const& m, uint32_t key);

    // ------------------------------------------------------------------ ranges

    // Inclusive. For a move it is the delta, for an edge the frame. The
    // current position is always inside, so a range is never empty; the why
    // strings say what stops it at each end ("the next P1 Jump click starts at
    // 1200").
    struct Range {
        int64_t lo = 0, hi = 0;
        std::string loWhy, hiWhy;
        int64_t clamp(int64_t v) const { return v < lo ? lo : v > hi ? hi : v; }
    };

    Range moveRange(Actions const& a, Model const& m, std::span<uint32_t const> keys);
    enum class Edge : uint8_t { Press, Release };
    // Clicks only (a lone press or release has one edge, which a move covers).
    Range edgeRange(Actions const& a, Model const& m, uint32_t key, Edge e);

    // ------------------------------------------------------------------ edits

    struct Draft {
        bool ok = false;
        std::string error;                // why it was refused, one sentence
        std::string label;                // what it did: the undo step's name
        Actions result;                   // the whole list after the edit
        std::vector<uint32_t> selection;  // keys in `result` of what it made or moved
    };

    // Moves the items by `delta` frames. Refused outside moveRange.
    Draft move(Actions const& a, Model const& m, std::span<uint32_t const> keys, int64_t delta);
    // Puts a click's press and release on these frames (a stretch or a
    // shorten from either end). press == release is a tap inside one frame.
    Draft setEdges(Actions const& a, Model const& m, uint32_t key, uint32_t press, uint32_t release);
    Draft remove(Actions const& a, Model const& m, std::span<uint32_t const> keys);
    // A new click in `lane`, pressed at `press` and released `hold` frames later.
    Draft addClick(Actions const& a, Model const& m, int lane, uint32_t press, uint32_t hold);

    // ----------------------------------------------------------------- history

    // Undo and redo. Each step keeps only the window of the list that changed
    // (the part between what the two lists share at the front and at the
    // back), and the selection on each side of it.
    class History {
    public:
        void record(Actions const& before, Actions const& after, std::string label,
                    std::vector<uint32_t> selectionBefore, std::vector<uint32_t> selectionAfter);
        // False, and the history is cleared, when the list is not the one the
        // step left behind (something else changed it since); `a` is then
        // untouched. On success `selection` is what to select.
        bool undo(Actions& a, std::vector<uint32_t>& selection, std::string& label);
        bool redo(Actions& a, std::vector<uint32_t>& selection, std::string& label);
        bool canUndo() const { return !m_undo.empty(); }
        bool canRedo() const { return !m_redo.empty(); }
        std::string const& undoLabel() const;
        std::string const& redoLabel() const;
        size_t bytes() const { return m_bytes; }
        void clear();

        // Oldest steps are dropped past either limit.
        static constexpr size_t kMaxSteps = 300;
        static constexpr size_t kMaxBytes = size_t(96) << 20;

    private:
        struct Step {
            uint32_t lo = 0;
            Actions before, after;
            std::string label;
            std::vector<uint32_t> selBefore, selAfter;
            size_t bytes() const;
        };
        static bool swap(Actions& a, uint32_t lo, Actions const& expect, Actions const& with);
        void trim();
        std::vector<Step> m_undo, m_redo;
        size_t m_bytes = 0;
    };

    // ------------------------------------------------------------------ misc

    // Every field playback or the file reads, compared exactly.
    bool same(gb::Action const& x, gb::Action const& y);
    // A hash of every such field of every action, so the page can tell when
    // the macro changed outside it (a load, a recording, the pathfinder).
    uint64_t fingerprint(Actions const& a);

} // namespace gucci::edit

#ifndef _edit_core_test_hpp
#define _edit_core_test_hpp

// Tests for the frame editor's rules.
//
// There is no test runner in this project and adding one is not worth it: the
// mod is a DLL that only exists inside Geometry Dash. So these run at startup
// alongside the rest of the self-check, and land in the same Diagnostics panel
// Nigel already reads. A failure is visible before he has played a single
// frame, which is the whole point -- the alternative for this code is finding
// out from a desync in a real run.
//
// Every case here is a shape the editor can actually produce with the mouse.

#include "tools/edit_core.hpp"

#include <string>

// The cases below are pure -- edit_core.hpp has no Geode in it either -- so
// they can also be compiled and run outside the game. tools/run_edit_tests.cpp
// does exactly that, which is how they get checked without launching GD.
#ifndef GB_EDIT_CORE_TEST_STANDALONE
#include "tools/selfcheck.hpp"
#endif

namespace gucci::editcoretest {

    namespace {

        using editcore::Input;

        Input press(int32_t f, int act = 0, bool p2 = false) {
            Input i;
            i.frame = f;
            i.actionType = act;
            i.player2 = p2;
            i.pressed = true;
            return i;
        }

        Input release(int32_t f, int act = 0, bool p2 = false) {
            Input i;
            i.frame = f;
            i.actionType = act;
            i.player2 = p2;
            i.pressed = false;
            return i;
        }

    } // namespace

    // Every case, reported through a callback so the same list serves both
    // the in-game self-check and the standalone runner.
    template <typename Check>
    inline void runCases(Check&& check) {

        // --- pairing ------------------------------------------------------
        {
            std::vector<Input> in{press(10), release(20), press(40), release(45)};
            auto segs = editcore::buildSegments(in, false, 100);
            check("two holds pair up", segs.size() == 2);
            check("first hold spans 10..20",
                  segs.size() == 2 && segs[0].startFrame == 10 && segs[0].endFrame == 20 &&
                      segs[0].hasRelease);
            check("second hold spans 40..45",
                  segs.size() == 2 && segs[1].startFrame == 40 && segs[1].endFrame == 45);
        }
        {
            // A press with no release runs to the end of the macro.
            std::vector<Input> in{press(10)};
            auto segs = editcore::buildSegments(in, false, 300);
            check("unreleased press runs to the end",
                  segs.size() == 1 && segs[0].endFrame == 300 && !segs[0].hasRelease);
        }
        {
            // Different action types are different lanes and may overlap.
            std::vector<Input> in{press(10, 0), press(12, 1), release(20, 0), release(25, 1)};
            auto segs = editcore::buildSegments(in, false, 100);
            check("different lanes overlap freely", segs.size() == 2);
            check("both lanes keep their own release",
                  segs.size() == 2 && segs[0].hasRelease && segs[1].hasRelease);
        }
        {
            // Player 2 is a separate lane, but only when the macro has one.
            std::vector<Input> in{press(10, 0, false), press(10, 0, true), release(20, 0, false),
                                  release(30, 0, true)};
            auto twoP = editcore::buildSegments(in, true, 100);
            check("two-player macro keeps the players apart", twoP.size() == 2);
            auto oneP = editcore::buildSegments(in, false, 100);
            check("one-player macro folds p2 onto p1 and sees it as malformed",
                  !editcore::lanesAlternate(in, false) && oneP.size() == 2);
        }
        {
            // Out-of-order input still pairs by frame, not by position.
            std::vector<Input> in{release(20), press(10)};
            auto segs = editcore::buildSegments(in, false, 100);
            check("pairing goes by frame, not list order",
                  segs.size() == 1 && segs[0].startFrame == 10 && segs[0].endFrame == 20);
        }

        // --- the alternation invariant ------------------------------------
        {
            std::vector<Input> ok{press(10), release(20), press(40), release(45)};
            check("a clean macro alternates", editcore::lanesAlternate(ok, false));

            std::vector<Input> nested{press(10), press(15), release(20), release(25)};
            check("press-press-release-release is caught",
                  !editcore::lanesAlternate(nested, false));

            std::vector<Input> orphan{release(10)};
            check("a release with nothing held is caught",
                  !editcore::lanesAlternate(orphan, false));
        }

        // --- dragging a whole hold ----------------------------------------
        {
            // 10..20 and 40..45. Drag the first one right; it must stop before
            // the second, not slide through it.
            std::vector<Input> in{press(10), release(20), press(40), release(45)};
            int32_t d = editcore::clampSegmentMove(in, false, 0, 1, 10, 20, +100);
            check("a hold cannot be dragged through the next one", 20 + d < 40);
            check("it gets as close as it legally can", 20 + d == 39);

            // Applying that delta must leave a macro that still alternates.
            auto moved = in;
            moved[0].frame += d;
            moved[1].frame += d;
            check("dragging right leaves the macro well formed",
                  editcore::lanesAlternate(moved, false));
        }
        {
            // Drag the second hold left, into the first.
            std::vector<Input> in{press(10), release(20), press(40), release(45)};
            int32_t d = editcore::clampSegmentMove(in, false, 2, 3, 40, 45, -100);
            check("a hold cannot be dragged back through the previous one", 40 + d > 20);
            check("dragging left stops one frame clear", 40 + d == 21);

            auto moved = in;
            moved[2].frame += d;
            moved[3].frame += d;
            check("dragging left leaves the macro well formed",
                  editcore::lanesAlternate(moved, false));
        }
        {
            // Frame 0 is the start of the level and nothing may go before it.
            std::vector<Input> in{press(5), release(9)};
            int32_t d = editcore::clampSegmentMove(in, false, 0, 1, 5, 9, -100);
            check("a hold cannot be dragged before frame 0", 5 + d == 0);
        }
        {
            // A hold on its own lane ignores a neighbour on another lane.
            std::vector<Input> in{press(10, 0), release(20, 0), press(25, 1), release(30, 1)};
            int32_t d = editcore::clampSegmentMove(in, false, 0, 1, 10, 20, +50);
            check("another lane's hold is not an obstacle", 10 + d == 60);
        }

        // --- dragging one edge --------------------------------------------
        {
            std::vector<Input> in{press(10), release(20), press(40), release(45)};
            check("the left edge cannot pass its own release",
                  editcore::clampEdgeLeft(in, false, 0, 1, 99) == 19);
            check("the left edge cannot go before frame 0",
                  editcore::clampEdgeLeft(in, false, 0, 1, -5) == 0);
            check("the right edge cannot pass its own press",
                  editcore::clampEdgeRight(in, false, 0, 1, 2) == 11);
            check("the right edge cannot reach into the next hold",
                  editcore::clampEdgeRight(in, false, 0, 1, 100) == 39);
            check("the left edge of the second hold stops after the first",
                  editcore::clampEdgeLeft(in, false, 2, 3, 5) == 21);
        }
        {
            // Stretching an edge to its limit must still leave it well formed.
            std::vector<Input> in{press(10), release(20), press(40), release(45)};
            auto stretched = in;
            stretched[1].frame = editcore::clampEdgeRight(in, false, 0, 1, 1000);
            check("a stretched hold leaves the macro well formed",
                  editcore::lanesAlternate(stretched, false));
        }

    }

#ifndef GB_EDIT_CORE_TEST_STANDALONE
    // Adds ONE self-check line, because a panel with twenty green editor rows
    // in it buries the four checks that say whether the mod will run at all.
    // The detail string names the first failure.
    inline void run() {
        int passed = 0;
        std::string firstFailure;
        runCases([&](char const* name, bool ok) {
            if (ok)
                passed++;
            else if (firstFailure.empty())
                firstFailure = name;
        });

        bool const ok = firstFailure.empty();
        gbcheck::add("Frame editor rules",
                     ok,
                     ok ? (std::to_string(passed) + " cases")
                        : ("first failure: " + firstFailure));
        if (ok)
            geode::log::info("[GucciBot]  [OK]   Frame editor rules: {} cases", passed);
        else
            geode::log::error("[GucciBot]  [FAIL] Frame editor rules: \"{}\" -- the editor can "
                              "write a macro that plays differently from what it draws",
                              firstFailure);
    }
#endif

} // namespace gucci::editcoretest

#endif

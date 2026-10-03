// The Frame Editor's rules (src/tools/edit_core.hpp), outside the game. Run
// by run_tests.bat. Fixed cases for each edit, then a fuzz: random macros,
// random selections moved by random amounts inside moveRange, which must all
// go through (the range and the placement agree) and leave every lane paired.
#include "tools/edit_core.hpp"

#include <cstdio>
#include <random>

using namespace gucci;
using namespace gucci::edit;
using T = gb::ActionType;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

static gb::Action in(uint32_t f, bool press, bool p2 = false, T t = T::Jump) {
    gb::Action a;
    a.m_frame = f;
    a.m_type = t;
    a.m_holding = press;
    a.m_player2 = p2;
    return a;
}

static gb::Action ev(uint32_t f, T t, double tps = 0.0) {
    gb::Action a;
    a.m_frame = f;
    a.m_type = t;
    a.m_tps = tps;
    return a;
}

static bool sorted(Actions const& a) {
    for (size_t i = 1; i < a.size(); ++i)
        if (a[i].m_frame < a[i - 1].m_frame)
            return false;
    return true;
}

static std::vector<uint32_t> one(uint32_t k) {
    return {k};
}

int main() {
    int cases = 0;

    // the model: two clicks, a lone press cut by a death, a release after it
    {
        cases++;
        Actions a{in(10, true), in(20, false), in(30, true), ev(40, T::Death), in(41, false), in(50, true, true),
                  in(60, false, true)};
        Model m = build(a);
        CHECK(m.sorted && m.items.size() == 4 && m.clicks == 2 && m.lonePresses == 1 && m.loneReleases == 1);
        CHECK(m.resetFrames.size() == 1 && m.resetFrames[0] == 40);
        CHECK(m.lanes[0].size() == 3 && m.lanes[3].size() == 1);
        CHECK(holdEnd(a, m, m.items[1]) == 39);
        CHECK(itemOfKey(m, 2) == 1 && itemOfKey(m, 1) == kNone);
    }
    // a move, its new key, and a no-op refused
    {
        cases++;
        Actions a{in(10, true), in(20, false), in(30, true), in(40, false)};
        Model m = build(a);
        auto d = move(a, m, one(2), 5);
        CHECK(d.ok && d.result.size() == 4 && d.result[2].m_frame == 35 && d.result[3].m_frame == 45);
        CHECK(d.selection.size() == 1 && d.selection[0] == 2);
        CHECK(!move(a, m, one(2), 0).ok);
    }
    // the lane rule: a click may meet the one before it only as a re-click
    {
        cases++;
        Actions a{in(10, true), in(20, false), in(30, true), in(40, false)};
        Model m = build(a);
        auto r = moveRange(a, m, one(2));
        CHECK(r.lo == -10 && r.hi > 1000000);
        auto d = move(a, m, one(2), -10);
        CHECK(d.ok && sorted(d.result));
        // on frame 20: the first click's release, then the second's press
        CHECK(d.ok && !d.result[1].m_holding && d.result[2].m_holding && d.result[2].m_frame == 20);
        CHECK(macrocheck::check(d.result).ok());
        CHECK(!move(a, m, one(2), -11).ok);
        // the first click can't go before frame 1
        CHECK(moveRange(a, m, one(0)).lo == -9);
    }
    // the attempt rule: nothing moves onto or across a death
    {
        cases++;
        Actions a{in(30, true), in(40, false), ev(50, T::Death), in(60, true), in(70, false)};
        Model m = build(a);
        auto r = moveRange(a, m, one(0));
        CHECK(r.hi == 9);
        auto r2 = moveRange(a, m, one(3));
        CHECK(r2.lo == -9);
        CHECK(!move(a, m, one(0), 10).ok);
        CHECK(move(a, m, one(0), 9).ok);
    }
    // stretching and shortening from either end, and a tap
    {
        cases++;
        Actions a{in(30, true), in(40, false), in(50, true), in(55, false)};
        Model m = build(a);
        auto rr = edgeRange(a, m, 0, Edge::Release);
        CHECK(rr.lo == 30 && rr.hi == 50);
        auto pr = edgeRange(a, m, 2, Edge::Press);
        CHECK(pr.lo == 40 && pr.hi == 55);
        auto d = setEdges(a, m, 0, 30, 45);
        CHECK(d.ok && d.result[1].m_frame == 45);
        CHECK(!setEdges(a, m, 0, 30, 51).ok);
        auto tap = setEdges(a, m, 0, 33, 33);
        CHECK(tap.ok && tap.result[0].m_frame == 33 && tap.result[0].m_holding && tap.result[1].m_frame == 33 &&
              !tap.result[1].m_holding);
        CHECK(tap.ok && macrocheck::check(tap.result).ok());
        CHECK(!setEdges(a, m, 0, 41, 40).ok);
        // only clicks have edges
        Actions b{in(10, true)};
        CHECK(!setEdges(b, build(b), 0, 10, 12).ok);
    }
    // adding: overlaps refused, re-clicks and other lanes fine
    {
        cases++;
        Actions a{in(10, true), in(20, false), ev(100, T::Restart)};
        Model m = build(a);
        CHECK(!addClick(a, m, 0, 15, 2).ok);
        CHECK(!addClick(a, m, 0, 5, 6).ok);  // its release would land inside the click at 10
        auto touch = addClick(a, m, 0, 5, 5);  // released on the frame the next one is pressed
        CHECK(touch.ok && touch.result[1].m_frame == 10 && !touch.result[1].m_holding && touch.result[2].m_holding);
        auto re = addClick(a, m, 0, 20, 5);
        CHECK(re.ok && re.result.size() == 5 && !re.result[1].m_holding && re.result[2].m_holding &&
              re.result[2].m_frame == 20);
        CHECK(re.ok && re.selection.size() == 1 && re.selection[0] == 2);
        CHECK(addClick(a, m, 3, 15, 2).ok);
        CHECK(!addClick(a, m, 0, 95, 10).ok);  // across the restart
        CHECK(!addClick(a, m, 0, 0, 3).ok);    // frame 0 is never read
        // a tap on the frame a click starts goes in front of that press
        auto tap = addClick(a, m, 0, 10, 0);
        CHECK(tap.ok && tap.result[0].m_frame == 10 && tap.result[0].m_holding && !tap.result[1].m_holding &&
              tap.result[2].m_holding);
        CHECK(tap.ok && macrocheck::check(tap.result).ok());
    }
    // deleting a click and a lone press
    {
        cases++;
        Actions a{in(10, true), in(20, false), in(30, true)};
        Model m = build(a);
        std::vector<uint32_t> both{0, 2};
        auto d = remove(a, m, both);
        CHECK(d.ok && d.result.empty() && d.label == "Deleted 2 inputs");
    }
    // an edit that would add a problem is refused; one next to an old problem is not
    {
        cases++;
        // a lone release already in the macro (an orphan) stays; moving the click is fine
        Actions a{in(5, false), in(10, true), in(20, false)};
        Model m = build(a);
        auto d = move(a, m, one(1), 3);
        CHECK(d.ok);
        CHECK(!move(a, m, one(1), -6).ok);  // past the orphan: it would become this click's release
        CHECK(move(a, m, one(1), -5).ok);   // a press on the orphan's frame, after it, is fine
    }
    // undo and redo restore the list and the selection
    {
        cases++;
        Actions a{in(10, true), in(20, false), in(30, true), in(40, false), ev(90, T::TPS, 240.0)};
        Actions const orig = a;
        History h;
        Model m = build(a);
        auto d = move(a, m, one(2), 7);
        CHECK(d.ok);
        h.record(a, d.result, d.label, one(2), d.selection);
        a = d.result;
        std::vector<uint32_t> sel;
        std::string label;
        CHECK(h.undo(a, sel, label) && a.size() == orig.size() && sel == one(2));
        bool same1 = true;
        for (size_t i = 0; i < a.size(); ++i)
            same1 = same1 && same(a[i], orig[i]);
        CHECK(same1 && fingerprint(a) == fingerprint(orig));
        CHECK(h.redo(a, sel, label) && a[2].m_frame == 37 && !h.canRedo() && h.canUndo());
        // a list changed elsewhere: undo refuses and the history starts over
        a[3].m_frame = 99;
        CHECK(!h.undo(a, sel, label) && !h.canUndo() && a[3].m_frame == 99);
    }
    // an unsorted macro is not edited
    {
        cases++;
        Actions a{in(20, true), in(10, false)};
        CHECK(!move(a, build(a), one(0), 1).ok);
    }

    // fuzz: every move inside moveRange goes through and keeps the macro whole
    {
        std::mt19937 rng(12345);
        int moves = 0, edges = 0, adds = 0, refusedAdds = 0;
        for (int round = 0; round < 400; ++round) {
            cases++;
            Actions a;
            uint32_t f = 1 + rng() % 5;
            int const n = 5 + static_cast<int>(rng() % 40);
            std::array<bool, 6> held{};
            for (int k = 0; k < n; ++k) {
                f += rng() % 6;
                if (rng() % 23 == 0) {
                    a.push_back(ev(f, (rng() % 2) ? T::Death : T::Restart));
                    held.fill(false);
                    f += 1;
                    continue;
                }
                if (rng() % 31 == 0) {
                    a.push_back(ev(f, T::TPS, 240.0));
                    continue;
                }
                int const lane = static_cast<int>(rng() % 4) * (rng() % 3 == 0 ? 1 : 0);
                bool const p2 = lane >= 3;
                T const t = static_cast<T>(lane % 3 + 1);
                a.push_back(in(f, !held[lane], p2, t));
                held[lane] = !held[lane];
            }
            Model const m = build(a);
            auto const before = macrocheck::check(a);
            CHECK(before.ok());
            if (m.items.empty())
                continue;

            std::vector<uint32_t> keys;
            for (auto const& it : m.items)
                if (rng() % 3 == 0)
                    keys.push_back(it.key());
            if (keys.empty())
                keys.push_back(m.items[rng() % m.items.size()].key());
            Range const r = moveRange(a, m, keys);
            CHECK(r.lo <= 0 && r.hi >= 0);
            int64_t const span = std::min<int64_t>(r.hi, 50) - std::max<int64_t>(r.lo, -50);
            int64_t const delta = std::max<int64_t>(r.lo, -50) + (span > 0 ? static_cast<int64_t>(rng() % (span + 1)) : 0);
            if (delta != 0) {
                auto d = move(a, m, keys, delta);
                CHECK(d.ok);
                if (!d.ok)
                    std::printf("  round %d: move by %lld refused: %s\n", round, static_cast<long long>(delta),
                                d.error.c_str());
                if (d.ok) {
                    moves++;
                    CHECK(sorted(d.result) && d.result.size() == a.size());
                    auto const after = macrocheck::check(d.result);
                    CHECK(after.ok() && after.clicks == before.clicks);
                    Model const m2 = build(d.result);
                    CHECK(d.selection.size() == keys.size());
                    for (uint32_t k : d.selection)
                        CHECK(itemOfKey(m2, k) != kNone);
                }
            }

            // an edge of a random click, anywhere inside its range
            uint32_t const pick = m.items[rng() % m.items.size()].key();
            uint32_t const idx = itemOfKey(m, pick);
            if (m.items[idx].kind == Kind::Click) {
                Range const pr = edgeRange(a, m, pick, Edge::Press);
                Range const rr = edgeRange(a, m, pick, Edge::Release);
                uint32_t const rf = a[m.items[idx].release].m_frame;
                uint32_t const pf = a[m.items[idx].press].m_frame;
                uint32_t const np = static_cast<uint32_t>(pr.lo + static_cast<int64_t>(rng() % (pr.hi - pr.lo + 1)));
                int64_t const rhi = std::min<int64_t>(rr.hi, rf + 60);
                uint32_t const nr = static_cast<uint32_t>(rr.lo + static_cast<int64_t>(rng() % (rhi - rr.lo + 1)));
                if (np != pf) {
                    auto d = setEdges(a, m, pick, np, rf);
                    CHECK(d.ok);
                    if (d.ok) {
                        edges++;
                        CHECK(macrocheck::check(d.result).ok());
                    }
                }
                if (nr != rf) {
                    auto d = setEdges(a, m, pick, pf, nr);
                    CHECK(d.ok);
                    if (d.ok) {
                        edges++;
                        CHECK(macrocheck::check(d.result).ok());
                    }
                }
            }

            // an add anywhere: refused or clean, never broken
            auto d = addClick(a, m, static_cast<int>(rng() % 6), 1 + rng() % (f + 10), rng() % 8);
            if (d.ok) {
                adds++;
                CHECK(macrocheck::check(d.result).ok() && sorted(d.result));
            } else {
                refusedAdds++;
            }
        }
        std::printf("edit fuzz: %d moves, %d edge sets, %d adds (%d refused)\n", moves, edges, adds, refusedAdds);
    }

    std::printf(fails ? "frame editor: %d case(s), %d FAILED\n" : "frame editor: %d cases, all passed\n", cases,
                fails);
    return fails ? 1 : 0;
}

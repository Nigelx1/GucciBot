// Macro Tools' work (src/tools/macro_ops.hpp), outside the game. Run by
// run_tests.bat: the file round trip, Trim, Merge, TPS changes and Diff on
// small macros, then a fuzz that trims and merges random well-formed macros
// and checks every lane stays paired.
#include "tools/macro_ops.hpp"

#include <cstdio>
#include <random>

using namespace gucci;
using namespace gucci::macroops;
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

static Macro mk(Actions a, float tps = 240.f) {
    Macro m;
    m.header.tps = tps;
    m.header.name = "t";
    m.header.levelName = "Level";
    m.actions = std::move(a);
    return m;
}

static Macro roundTrip(Macro const& m) {
    auto bytes = toFile(m).serialize();
    auto f = GBR6File::deserialize(bytes.data(), bytes.size());
    CHECK(f.has_value());
    return f ? fromFile(*f) : Macro{};
}

static bool same(Actions const& x, Actions const& y) {
    if (x.size() != y.size())
        return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i].m_frame != y[i].m_frame || x[i].m_type != y[i].m_type || x[i].m_holding != y[i].m_holding ||
            x[i].m_player2 != y[i].m_player2 || x[i].m_tps != y[i].m_tps || x[i].m_subtick != y[i].m_subtick)
            return false;
    return true;
}

int main() {
    // file round trip: inputs of every lane, a sub-tick offset, a death, a
    // TPS change, the header -- and a platformer left tap stays a left tap
    {
        auto a = in(10, true);
        a.m_subtick = 0.5;
        auto m = mk({a, in(14, false), in(20, true, false, T::Left), in(22, false, false, T::Left),
                     in(30, true, true, T::Right), in(31, false, true, T::Right), ev(40, T::Death),
                     ev(50, T::TPS, 480.0), in(60, true), in(61, false)});
        auto r = roundTrip(m);
        CHECK(same(m.actions, r.actions));
        CHECK(r.header.levelName == "Level" && r.header.tps == 240.f);
        auto s = stats(r);
        CHECK(s.inputs == 8 && s.clicks == 4 && s.resets == 1 && s.tpsChanges == 1 && s.subticks == 1);
        CHECK(s.problems == 0 && s.player2 && s.lastFrame == 61);
    }
    // release then press of one button on one frame keeps its order
    {
        auto m = mk({in(10, true), in(20, false), in(20, true), in(30, false)});
        auto r = roundTrip(m);
        CHECK(same(m.actions, r.actions));
        CHECK(macrocheck::check(r.actions).ok());
    }

    // TPS changes
    {
        auto m = mk({in(10, true), in(20, false), in(30, true), in(40, false)});
        std::string err;
        CHECK(!setTpsChange(m, 0, 360.0, &err) && !err.empty());
        CHECK(!setTpsChange(m, 15, -1.0, &err));
        CHECK(setTpsChange(m, 30, 360.0, &err));
        CHECK(m.actions[2].m_frame == 30 && m.actions[2].m_type == T::Jump);  // after the press on 30
        CHECK(m.actions[3].m_type == T::TPS);
        CHECK(setTpsChange(m, 30, 480.0, &err) && tpsChanges(m).size() == 1 && tpsChanges(m)[0].tps == 480.0);
        CHECK(tpsBefore(m, 30) == 240.0 && tpsBefore(m, 31) == 480.0);
        auto r = roundTrip(m);
        CHECK(same(m.actions, r.actions));
        CHECK(removeTpsChange(m, 30) && tpsChanges(m).empty() && !removeTpsChange(m, 30));
    }

    // trim: a hold across the start is pressed again there, one across the
    // end is let go of on end + 1, the header takes the rate at the start
    {
        auto m = mk({in(10, true), ev(12, T::TPS, 480.0), in(20, false), in(30, true), in(40, false),
                     in(50, true), in(60, false)});
        auto o = trim(m, 15, 45, false);
        CHECK(o.ok && o.report.ok());
        CHECK(o.macro.header.tps == 480.f);
        CHECK(o.macro.actions.front().m_frame == 15 && o.macro.actions.front().m_holding);
        CHECK(o.macro.actions.size() == 4);  // press 15, release 20, press 30, release 40
        auto o2 = trim(m, 15, 35, false);
        CHECK(o2.ok && o2.report.ok() && o2.macro.actions.back().m_frame == 36 && !o2.macro.actions.back().m_holding);
        auto o3 = trim(m, 15, 35, true);
        CHECK(o3.ok && o3.macro.actions.front().m_frame == 1 && o3.macro.actions.back().m_frame == 22);
        CHECK(!trim(m, 41, 49, false).ok);  // nothing there
        CHECK(!trim(m, 30, 20, false).ok);
    }
    // trim: a hold to the very end of the source stays held
    {
        auto m = mk({in(10, true), in(20, false), in(30, true)});
        auto o = trim(m, 5, 35, false);
        CHECK(o.ok && o.report.findings.empty() && o.report.heldToEnd == 1);
    }
    // trim just after a death: the recorder's release after the respawn is
    // left out instead of becoming a release with nothing to end
    {
        auto m = mk({in(10, true), ev(15, T::Death), in(16, false), in(20, true), in(25, false)});
        CHECK(macrocheck::check(m.actions).ok());
        auto o = trim(m, 16, 30, false);
        CHECK(o.ok && o.report.ok() && o.macro.actions.size() == 2);
    }

    // merge: B shifted, A's open hold let go of, a TPS change where B begins
    {
        auto a = mk({in(10, true), in(20, false), in(30, true)}, 240.f);
        auto b = mk({in(5, true), in(8, false)}, 360.f);
        auto o = merge(a, b, 10);
        CHECK(o.ok && o.report.ok());
        // A: 10,20,30 ; release at 40 ; TPS at 41 ; B at 45, 48
        CHECK(o.macro.actions.size() == 7);
        CHECK(o.macro.actions[3].m_frame == 40 && !o.macro.actions[3].m_holding);
        CHECK(o.macro.actions[4].m_type == T::TPS && o.macro.actions[4].m_frame == 41 &&
              o.macro.actions[4].m_tps == 360.0);
        CHECK(o.macro.actions[5].m_frame == 45 && o.macro.actions[6].m_frame == 48);
        auto o0 = merge(a, b, 0);
        CHECK(o0.ok && o0.report.ok());
        CHECK(!merge(a, mk({}), 0).ok);
    }

    // diff
    {
        auto a = mk({in(10, true), in(20, false), in(30, true), in(40, false), ev(50, T::TPS, 480.0)});
        auto b = a;
        auto d0 = diff(a, b, 5);
        CHECK(d0.identical() && d0.matched == 4);
        b.actions[0].m_frame = 12;            // moved by 2
        b.actions[2].m_subtick = 0.25;        // sub-tick
        b.actions[4].m_tps = 360.0;           // TPS
        b.actions.push_back(in(100, true));   // only B
        b.actions.push_back(in(110, false));
        b.header.tps = 120.f;
        auto d = diff(a, b, 5);
        CHECK(!d.identical() && d.startTpsDiffers && !d.levelDiffers);
        CHECK(d.moved == 1 && d.subtick == 1 && d.tps == 1 && d.onlyB == 2 && d.onlyA == 0);
        CHECK(d.items.front().kind == DiffKind::Moved && d.items.front().frameA == 10 && d.items.front().frameB == 12);
        auto far = diff(a, b, 1);  // 2 frames is past the window: one only-A, one only-B
        CHECK(far.moved == 0 && far.onlyA == 1 && far.onlyB == 3);
    }

    // fuzz: random well-formed macros, random trims and merges, all paired
    {
        std::mt19937 rng(12345);
        auto randomMacro = [&]() {
            Actions a;
            uint32_t f = 1;
            std::array<bool, 6> held{};
            int n = 5 + (int)(rng() % 60);
            for (int k = 0; k < n; ++k) {
                f += 1 + rng() % 12;  // one action per frame: the file has its own order within a frame
                if (rng() % 25 == 0) {
                    a.push_back(ev(f, T::Death));
                    held.fill(false);
                    continue;
                }
                if (rng() % 30 == 0) {
                    a.push_back(ev(f, T::TPS, 120.0 + (rng() % 4) * 120.0));
                    continue;
                }
                int lane = (int)(rng() % 6);
                a.push_back(in(f, !held[lane], lane >= 3, static_cast<T>(lane % 3 + 1)));
                held[lane] = !held[lane];
            }
            return mk(a);
        };
        int ran = 0;
        for (int it = 0; it < 3000; ++it) {
            auto m = randomMacro();
            auto rt = roundTrip(m);
            CHECK(same(m.actions, rt.actions));
            uint32_t const last = m.actions.empty() ? 1 : m.actions.back().m_frame;
            uint32_t s = rng() % (last + 2), e = s + rng() % (last + 2);
            auto o = trim(m, s, e, rng() % 2);
            if (o.ok) {
                CHECK(o.report.ok());
                ran++;
            }
            auto o2 = merge(m, randomMacro(), rng() % 20);
            if (o2.ok)
                CHECK(o2.report.ok());
        }
        CHECK(ran > 1000);
    }

    std::printf(fails ? "macro ops: %d FAILED\n" : "macro ops: all passed\n", fails);
    return fails ? 1 : 0;
}

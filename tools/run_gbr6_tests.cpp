// GBR6 sub-tick section round trip (SCBF offsets, 2026-09-27). Run by
// run_tests.bat, outside the game. Built /O2 /MD like the mod: with cl's
// defaults (static CRT, no optimisation) this toolchain fast-fails inside the
// very first vector::insert in serialize(), which the mod runs on every save.
#include "core/gbr6_format.hpp"

#include <cstdio>
#include <cstring>

using namespace gucci;

static int fails = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);      \
            fails++;                                              \
        }                                                         \
    } while (0)

static GBR6File make(bool deaths, bool subticks) {
    GBR6Header hdr;
    hdr.tps = 240.f;
    hdr.name = "t";
    std::vector<GBR6Input> p1 = {{10, 1, true}, {20, 1, false}, {30, 1, true}, {31, 1, false}};
    std::vector<GBR6Input> p2 = {{15, 1, true, true}, {25, 1, false, true}};
    auto f = GBR6File::fromInputs(hdr, p1, p2);
    if (deaths) {
        f.deaths.push_back({40, 10});
        f.header.flags |= GBR6_HAS_DEATHS;
    }
    if (subticks) {
        f.subticks.push_back({10, 1, true, false, 0.25});
        f.subticks.push_back({25, 1, false, true, 0.8125});
        f.subticks.push_back({31, 1, false, false, 0.5});
        f.header.flags |= GBR6_HAS_SUBTICK;
    }
    return f;
}

int main() {
    // offsets + deaths survive a round trip, and the inputs are untouched
    {
        auto f = make(true, true);
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value());
        CHECK(g->p1Inputs.size() == 4 && g->p2Inputs.size() == 2);
        CHECK(g->deaths.size() == 1 && g->deaths[0].frame == 40);
        CHECK(g->subticks.size() == 3);
        CHECK(g->subticks[0].frame == 10 && g->subticks[0].pressed && !g->subticks[0].player2 &&
              g->subticks[0].offset == 0.25);
        CHECK(g->subticks[1].frame == 25 && !g->subticks[1].pressed && g->subticks[1].player2 &&
              g->subticks[1].offset == 0.8125);
        CHECK(g->subticks[2].offset == 0.5);
    }
    // offsets with NO deaths: the empty deaths block keeps the layout findable
    {
        auto f = make(false, true);
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value());
        CHECK(g->deaths.empty());
        CHECK(g->subticks.size() == 3);
    }
    // a file from before the section loads exactly as before
    {
        auto f = make(true, false);
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value());
        CHECK(g->deaths.size() == 1);
        CHECK(g->subticks.empty());
    }
    // an OLD reader: simulate one by clearing the flag bit and reading the
    // same bytes -- it must load the inputs and deaths and ignore the tail
    {
        auto f = make(true, true);
        auto bytes = f.serialize();
        bytes[5] &= (uint8_t)~GBR6_HAS_SUBTICK;  // flags byte: magic(4) + version(1)
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value());
        CHECK(g->p1Inputs.size() == 4 && g->deaths.size() == 1);
        CHECK(g->subticks.empty());
    }
    // out-of-range offsets are dropped on load, not trusted
    {
        auto f = make(false, true);
        f.subticks[0].offset = 1.0;
        f.subticks[1].offset = -0.1;
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value() && g->subticks.size() == 1);
    }
    std::printf(fails ? "gbr6 sub-tick: %d FAILED\n" : "gbr6 sub-tick: all passed\n", fails);
    return fails ? 1 : 0;
}

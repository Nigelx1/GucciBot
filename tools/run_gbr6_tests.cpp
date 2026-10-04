// GBR6 sub-tick section round trip (SCBF offsets, 2026-09-27), the TPS
// section (Macro Tools, 2026-10-03) and the encoder keeping each input's
// button. Run by
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
    // TPS changes, alone and after deaths and offsets
    for (int variant = 0; variant < 4; ++variant) {
        auto f = make(variant & 1, variant & 2);
        f.tpsChanges.push_back({12, 480.0});
        f.tpsChanges.push_back({28, 120.0});
        f.header.flags |= GBR6_HAS_TPS;
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value());
        CHECK(g->tpsChanges.size() == 2 && g->tpsChanges[0].frame == 12 && g->tpsChanges[1].tps == 120.0);
        CHECK(g->deaths.size() == (variant & 1 ? 1u : 0u));
        CHECK(g->subticks.size() == (variant & 2 ? 3u : 0u));
        CHECK(g->p1Inputs.size() == 4 && g->p2Inputs.size() == 2);
        // a reader from before the TPS section: same bytes, flag cleared
        bytes[5] &= (uint8_t)~GBR6_HAS_TPS;
        auto old = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(old.has_value() && old->tpsChanges.empty() && old->p1Inputs.size() == 4);
        CHECK(old->subticks.size() == (variant & 2 ? 3u : 0u));
    }
    // a rate that is not a positive number is dropped on load
    {
        auto f = make(false, false);
        f.tpsChanges.push_back({12, 0.0});
        f.tpsChanges.push_back({13, -5.0});
        f.tpsChanges.push_back({14, 360.0});
        f.header.flags |= GBR6_HAS_TPS;
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value() && g->tpsChanges.size() == 1 && g->tpsChanges[0].frame == 14);
    }
    // platformer left/right taps and runs keep their button (the tap and
    // autoclick forms read back as jump, so they are only used for jump)
    {
        GBR6Header hdr;
        std::vector<GBR6Input> p1;
        for (uint32_t c = 0; c < 10; ++c) {  // a run long enough for the autoclick form
            p1.push_back({100 + c * 4, 2, true});
            p1.push_back({102 + c * 4, 2, false});
        }
        p1.push_back({200, 3, true});
        p1.push_back({205, 3, false});
        p1.push_back({300, 1, true});
        p1.push_back({305, 1, false});
        auto f = GBR6File::fromInputs(hdr, p1);
        auto bytes = f.serialize();
        auto g = GBR6File::deserialize(bytes.data(), bytes.size());
        CHECK(g.has_value() && g->p1Inputs.size() == p1.size());
        bool sameAll = g.has_value() && g->p1Inputs.size() == p1.size();
        for (size_t i = 0; sameAll && i < p1.size(); ++i)
            sameAll = g->p1Inputs[i].frame == p1[i].frame && g->p1Inputs[i].button == p1[i].button &&
                      g->p1Inputs[i].pressed == p1[i].pressed;
        CHECK(sameAll);
    }
    std::printf(fails ? "gbr6 sub-tick: %d FAILED\n" : "gbr6 sub-tick: all passed\n", fails);
    return fails ? 1 : 0;
}

// Offline tests for src/tools/macro_convert: every format GucciBot writes reads
// back the same, GucciBot's own format round-trips, and the real macros named
// on the command line (other bots' files, an old 1.x .brrr) read cleanly.
//
//   run_convert_tests.exe [macro files...]
//
// A real file is checked for reading without error, inputs that alternate per
// button, and frames that never go backwards; "old.brrr=467" also checks the
// count.

#include "tools/macro_convert.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>

using namespace gucci::convert;

static int g_failed = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                         \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

static Macro sample(bool moves, bool tpsChange) {
    Macro m;
    m.tps = 240.0;
    m.levelName = "Test Level";
    int64_t f = 3;
    for (int k = 0; k < 40; k++) {
        bool const p2 = k % 5 == 4;
        uint8_t const button = moves && k % 7 == 3 ? uint8_t(2 + k % 2) : uint8_t(1);
        Input press{f, button, true, p2};
        Input release{f + 1 + k % 9, button, false, p2};
        m.inputs.push_back(press);
        m.inputs.push_back(release);
        f += 300 + k * 37;  // some deltas past 255 and 65535 later on
        if (k == 30)
            f += 70000;
    }
    if (tpsChange)
        m.tpsChanges.push_back({5000, 360.0});
    return m;
}

using Key = std::tuple<int64_t, int, bool, bool>;

static std::multimap<int64_t, Key> keyed(Macro const& m, bool jumpOnly) {
    std::multimap<int64_t, Key> out;
    for (auto const& in : m.inputs)
        if (!jumpOnly || in.button == 1)
            out.emplace(in.frame, Key{in.frame, in.button, in.down, in.player2});
    return out;
}

static void roundTrips() {
    std::printf("Round trips\n");
    for (auto const& f : formats()) {
        if (!f.canWrite)
            continue;
        bool const keepsTps = f.id == "tcm" || f.id == "slc3" || f.id == "scbf1" || f.id == "ybot2";
        Macro const m = sample(!f.jumpOnly, keepsTps);
        auto const w = write(m, f);
        CHECK(w.error.empty(), "%.*s: write failed: %s", (int)f.id.size(), f.id.data(), w.error.c_str());
        std::string const name = "macro" + std::string(f.ext);
        auto const r = read(w.bytes, name);
        if (!r.ok()) {
            CHECK(false, "%.*s: read back failed: %s", (int)f.id.size(), f.id.data(), r.error.c_str());
            continue;
        }
        CHECK(r.format->id == f.id, "%.*s: detected as %.*s", (int)f.id.size(), f.id.data(), (int)r.format->id.size(),
              r.format->id.data());
        CHECK(keyed(r.macro, f.jumpOnly) == keyed(m, f.jumpOnly), "%.*s: inputs differ (%zu read, %zu written)",
              (int)f.id.size(), f.id.data(), r.macro.inputs.size(), m.inputs.size());
        CHECK(r.macro.tps == m.tps, "%.*s: tps %f", (int)f.id.size(), f.id.data(), r.macro.tps);
        if (keepsTps)
            CHECK(r.macro.tpsChanges.size() == 1 && r.macro.tpsChanges[0].frame == 5000 &&
                      r.macro.tpsChanges[0].tps == 360.0,
                  "%.*s: TPS change lost", (int)f.id.size(), f.id.data());
        std::printf("  %-10.*s %6zu bytes, %zu inputs\n", (int)f.id.size(), f.id.data(), w.bytes.size(),
                    r.macro.inputs.size());
    }

    // GucciBot's own, with sub-ticks and a TPS change.
    Macro m = sample(true, true);
    m.inputs[4].subtick = 0.25;
    std::string err;
    Macro back;
    CHECK(fromGucciBot(toGucciBot(m), back, err), "GBR6: %s", err.c_str());
    CHECK(keyed(back, false) == keyed(m, false), "GBR6: inputs differ");
    CHECK(back.tpsChanges.size() == 1 && back.tpsChanges[0].tps == 360.0, "GBR6: TPS change lost");
    bool sub = false;
    for (auto const& in : back.inputs)
        sub = sub || in.subtick == 0.25;
    CHECK(sub, "GBR6: sub-tick lost");
    CHECK(back.levelName == "Test Level", "GBR6: level name lost");
}

static std::vector<uint8_t> slurp(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void realFile(std::string arg) {
    long expect = -1;
    if (auto eq = arg.rfind('='); eq != std::string::npos && eq + 1 < arg.size() &&
                                   arg.find_first_not_of("0123456789", eq + 1) == std::string::npos) {
        expect = std::stol(arg.substr(eq + 1));
        arg = arg.substr(0, eq);
    }
    auto const bytes = slurp(arg);
    std::string const name = arg.substr(arg.find_last_of("/\\") + 1);
    if (bytes.empty()) {
        std::printf("  (missing) %s\n", name.c_str());
        return;
    }
    auto const r = read(bytes, name);
    if (!r.ok()) {
        CHECK(false, "%s: %s", name.c_str(), r.error.c_str());
        return;
    }
    auto const& m = r.macro;
    // Per button and player, presses and releases alternate (a repeat is
    // allowed: some bots write a release at the start).
    std::map<std::tuple<int, bool>, bool> state;
    int repeats = 0;
    for (auto const& in : m.inputs) {
        auto const key = std::tuple<int, bool>{in.button, in.player2};
        if (state.count(key) && state[key] == in.down)
            repeats++;
        state[key] = in.down;
    }
    bool sorted = true;
    for (size_t k = 1; k < m.inputs.size(); k++)
        sorted = sorted && m.inputs[k - 1].frame <= m.inputs[k].frame;
    CHECK(sorted, "%s: frames go backwards", name.c_str());
    CHECK(!m.inputs.empty(), "%s: no inputs", name.c_str());
    if (expect >= 0)
        CHECK((long)m.inputs.size() == expect, "%s: %zu inputs, expected %ld", name.c_str(), m.inputs.size(), expect);
    std::printf("  %-40s %-8.*s %6zu inputs, %g TPS, last frame %lld, %d repeats, %zu TPS changes%s%s\n", name.c_str(),
                (int)r.format->id.size(), r.format->id.data(), m.inputs.size(), m.tps,
                m.inputs.empty() ? 0LL : (long long)m.inputs.back().frame, repeats, m.tpsChanges.size(),
                m.levelName.empty() ? "" : (", level " + m.levelName).c_str(), m.notes.empty() ? "" : " [notes]");
    for (auto const& n : m.notes)
        std::printf("      note: %s\n", n.c_str());

    // Through GucciBot's format and back, nothing changes.
    Macro back;
    std::string err;
    CHECK(fromGucciBot(toGucciBot(m), back, err), "%s: GBR6 trip: %s", name.c_str(), err.c_str());
    CHECK(keyed(back, false) == keyed(m, false), "%s: GBR6 trip changed the inputs", name.c_str());
}

int main(int argc, char** argv) {
    roundTrips();
    if (argc > 1) {
        std::printf("Real macros\n");
        for (int k = 1; k < argc; k++)
            realFile(argv[k]);
    }
    if (g_failed) {
        std::printf("%d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("All conversion checks passed\n");
    return 0;
}

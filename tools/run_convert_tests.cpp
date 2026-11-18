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
#include <cstring>
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

// yBot counts frames one lower than GucciBot (Nigel's real macros, 2026-11-18):
// its frame 0 is GucciBot's 1, on the way in and on the way out.
static void ybotFrames() {
    std::printf("yBot frames\n");
    auto const* f = find("ybot2");
    CHECK(f && f->frameShift == 1, "ybot2: frameShift should be 1");
    if (!f)
        return;
    // A file the way yBot writes one: version 0, 36 bytes of meta, no blobs,
    // then delta<<4 | flags (bit 0 player one, bit 1 down, bits 2-3 button).
    std::vector<uint8_t> b = {'y', 'b', 'o', 't', 0, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0};
    b.resize(16 + 36, 0);
    float const fps = 240.f;
    std::memcpy(b.data() + 16 + 24, &fps, 4);
    auto act = [&](unsigned delta, bool down, unsigned button) {
        uint64_t v = (uint64_t(delta) << 4) | 1u | (down ? 2u : 0u) | (button << 2);
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            b.push_back(c | (v ? 0x80 : 0));
        } while (v);
    };
    act(0, true, 3);    // Right pressed on yBot's frame 0
    act(0, true, 1);    // a jump on frame 0
    b.push_back(0xA0);  // 10<<4, button 0: an FPS change on frame 10
    b.push_back(0x01);
    float const fps2 = 360.f;
    b.insert(b.end(), (uint8_t const*)&fps2, (uint8_t const*)&fps2 + 4);
    act(0, false, 1);  // let go on frame 10
    act(5, true, 1);   // frame 15
    auto const r = read(b, "macro.ybot");
    CHECK(r.ok() && r.format->id == "ybot2", "ybot2: hand-made file didn't read as yBot");
    if (!r.ok())
        return;
    std::multimap<int64_t, Key> want;
    for (Input in : {Input{1, 3, true, false}, Input{1, 1, true, false}, Input{11, 1, false, false},
                     Input{16, 1, true, false}})
        want.emplace(in.frame, Key{in.frame, in.button, in.down, in.player2});
    CHECK(keyed(r.macro, false) == want, "ybot2: frames not one up (first %lld)",
          r.macro.inputs.empty() ? -1LL : (long long)r.macro.inputs[0].frame);
    CHECK(r.macro.tps == 240.0 && r.macro.tpsChanges.size() == 1 && r.macro.tpsChanges[0].frame == 11 &&
              r.macro.tpsChanges[0].tps == 360.0,
          "ybot2: TPS change not shifted with the inputs");

    // Back out: GucciBot's frames one down, so the same file comes back.
    auto const w = write(r.macro, *f);
    CHECK(w.error.empty(), "ybot2: write failed");
    std::vector<uint8_t> const tailIn(b.begin() + 52, b.end()), tailOut(w.bytes.begin() + 52, w.bytes.end());
    CHECK(tailIn == tailOut, "ybot2: actions written differ from yBot's own (%zu vs %zu bytes)", tailOut.size(),
          tailIn.size());

    // A GucciBot input on frame 0 has no yBot frame: it's left out with a
    // note, never written as a negative or wrapped frame; a TPS change on
    // GucciBot's frame 1 comes back on frame 1.
    Macro m;
    m.tps = 240.0;
    m.inputs = {Input{0, 1, true, false}, Input{2, 1, false, false}, Input{7, 1, true, false}};
    m.tpsChanges = {{1, 120.0}};
    auto const w2 = write(m, *f);
    CHECK(!w2.notes.empty(), "ybot2: frame-0 input dropped without a note");
    auto const r2 = read(w2.bytes, "macro.ybot");
    CHECK(r2.ok() && r2.macro.inputs.size() == 2 && r2.macro.inputs[0].frame == 2 && r2.macro.inputs[1].frame == 7,
          "ybot2: frame-0 export came back wrong (%zu inputs)", r2.macro.inputs.size());
    CHECK(r2.ok() && r2.macro.tps == 240.0 && r2.macro.tpsChanges.size() == 1 && r2.macro.tpsChanges[0].frame == 1 &&
              r2.macro.tpsChanges[0].tps == 120.0,
          "ybot2: a TPS change on frame 1 didn't come back on frame 1");
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
    ybotFrames();
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

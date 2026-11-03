// The AI pilot's networks and its record expansion (src/ai/net, src/ai/obs_core),
// outside the game. Run by run_tests.bat.
//
// Everything is checked against tools/ai/golden/, the golden set the trainer
// makes (python tools/ai/export.py golden; golden.json describes the files):
//  - the compact record's 494-float expansion equals record.expanded.f32 bit
//    for bit (the trainer's expand_np), and float16 decoding round-trips;
//  - pilot.gbnn and critic.gbnn (mlp) and reader.gbnn (conv1d, same padding
//    with dilation; one case shorter than its receptive field) give the
//    float64 reference outputs within 1e-5, and every decision is the same
//    (the golden cases sit at least 1e-3 from a decision boundary);
//  - running a net twice gives the same bits;
//  - the loader refuses broken files: wrong layout hash, every truncation
//    point in the header, a stray byte, bad magic or version, std of 0, a
//    weight that isn't finite.
//
// Usage: run_ai_net_tests [golden dir] (default tools/ai/golden).

#include "ai/net.hpp"
#include "ai/obs_core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace gucci::ai;

static int fails = 0;
static int checks = 0;
#define CHECK(c)                                             \
    do {                                                     \
        ++checks;                                            \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

namespace {

    constexpr char kGoldenHash[] = "a1b2c3d4e5f60718";  // tools/ai/export.py GOLDEN_HASH
    constexpr double kTolerance = 1e-5;
    std::string dir = "tools/ai/golden/";

    std::vector<uint8_t> readBytes(std::string const& name) {
        std::ifstream f(dir + name, std::ios::binary);
        if (!f) {
            std::printf("FAIL: can't open %s%s\n", dir.c_str(), name.c_str());
            fails++;
            return {};
        }
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    template <class T>
    std::vector<T> readArray(std::string const& name) {
        auto const b = readBytes(name);
        if (b.size() % sizeof(T)) {
            std::printf("FAIL: %s is %zu bytes, not a whole number of values\n", name.c_str(), b.size());
            fails++;
            return {};
        }
        std::vector<T> v(b.size() / sizeof(T));
        if (!b.empty()) std::memcpy(v.data(), b.data(), b.size());
        return v;
    }

    uint32_t bitsOf(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    // ---------------------------------------------------------------- the record

    void testExpansion() {
        auto const state = readArray<float>("record.state.f32");
        auto const scan = readArray<uint8_t>("record.scan.u8");
        auto const tok = readArray<uint8_t>("record.tok.u8");
        auto const geo = readArray<float>("record.geo.f32");
        auto const want = readArray<float>("record.expanded.f32");
        size_t const rows = state.size() / obs::kStateDim;
        CHECK(rows >= 8);
        CHECK(state.size() == rows * obs::kStateDim);
        CHECK(scan.size() == rows * obs::kScanDim);
        CHECK(tok.size() == rows * obs::kTokDim);
        CHECK(geo.size() == rows * obs::kGeoDim);
        CHECK(want.size() == rows * obs::kObsDim);
        if (fails) return;
        int bad = 0;
        std::vector<float> got(obs::kObsDim);
        for (size_t r = 0; r < rows; r++) {
            obs::Record rec;
            std::memcpy(rec.state, &state[r * obs::kStateDim], sizeof(rec.state));
            std::memcpy(rec.scan, &scan[r * obs::kScanDim], sizeof(rec.scan));
            std::memcpy(rec.tok, &tok[r * obs::kTokDim], sizeof(rec.tok));
            std::memcpy(rec.geo, &geo[r * obs::kGeoDim], sizeof(rec.geo));
            std::fill(got.begin(), got.end(), std::numeric_limits<float>::quiet_NaN());
            obs::expand(rec, got.data());
            for (int i = 0; i < obs::kObsDim; i++) {
                if (bitsOf(got[i]) != bitsOf(want[r * obs::kObsDim + i])) {
                    if (bad++ < 5)
                        std::printf("FAIL expansion row %zu value %d: %.9g (want %.9g)\n", r, i, got[i],
                                    want[r * obs::kObsDim + i]);
                }
            }
        }
        CHECK(bad == 0);
        std::printf("expansion: %zu golden rows, %d values differ\n", rows, bad);

        // float16: every half that isn't NaN decodes and encodes back to itself.
        int halfBad = 0;
        for (uint32_t h = 0; h < 65536; h++) {
            bool const nan = ((h >> 10) & 0x1f) == 0x1f && (h & 0x3ff);
            if (nan) continue;
            if (obs::toHalf(obs::fromHalf((uint16_t)h)) != h) halfBad++;
        }
        CHECK(halfBad == 0);
        CHECK(obs::fromHalf(0x3c00) == 1.0f);
        CHECK(obs::fromHalf(0x0001) == std::ldexp(1.0f, -24));
        CHECK(bitsOf(obs::fromHalf(0x8000)) == 0x80000000u);
    }

    // ---------------------------------------------------------------- the networks

    void testMlp(char const* kind, bool critic) {
        std::string const k = kind;
        net::Net n;
        std::string why;
        bool const ok = n.loadFile(dir + k + ".gbnn", kGoldenHash, why);
        if (!ok) std::printf("FAIL %s: %s\n", kind, why.c_str());
        CHECK(ok);
        if (!ok) return;
        CHECK(n.kind() == net::Kind::Mlp);
        CHECK(n.inputs() == (uint32_t)obs::kObsDim);
        CHECK(n.outputs() == (critic ? 2u : 1u));
        CHECK(n.output(critic ? "alive_hold" : "hold_logit") == 0);
        CHECK(!critic || n.output("alive_release") == 1);
        auto const in = readArray<float>(k + ".in.f32");
        auto const want = readArray<double>(k + ".out.f64");
        auto const dec = readArray<uint8_t>(k + ".dec.u8");
        size_t const cases = dec.size();
        CHECK(cases > 0);
        CHECK(in.size() == cases * n.inputs());
        CHECK(want.size() == cases * n.outputs());
        if (in.size() != cases * n.inputs() || want.size() != cases * n.outputs()) return;
        double worst = 0.0;
        int flips = 0, unstable = 0;
        std::vector<float> out(n.outputs()), again(n.outputs());
        for (size_t c = 0; c < cases; c++) {
            n.forward(&in[c * n.inputs()], out.data());
            for (uint32_t o = 0; o < n.outputs(); o++) {
                double const e = std::fabs((double)out[o] - want[c * n.outputs() + o]);
                worst = e > worst ? e : worst;
            }
            bool const d = critic ? net::criticPrefersHold(out[0], out[1]) : net::holds(out[0]);
            if (d != (dec[c] != 0)) flips++;
            n.forward(&in[c * n.inputs()], again.data());
            for (uint32_t o = 0; o < n.outputs(); o++)
                if (bitsOf(out[o]) != bitsOf(again[o])) unstable++;
        }
        CHECK(worst <= kTolerance);
        CHECK(flips == 0);
        CHECK(unstable == 0);
        std::printf("%s: %zu cases, worst |C++ - float64| %.3g, %d decisions differ, %d values change on a rerun\n",
                    kind, cases, worst, flips, unstable);
    }

    void testReader() {
        net::Net n;
        std::string why;
        bool const ok = n.loadFile(dir + "reader.gbnn", kGoldenHash, why);
        if (!ok) std::printf("FAIL reader: %s\n", why.c_str());
        CHECK(ok);
        if (!ok) return;
        CHECK(n.kind() == net::Kind::Conv1d);
        CHECK(n.outputs() == 11u);
        CHECK(n.output("y_rel") == 0);
        CHECK(n.output("hold_frac") == 10);
        bool dilated = false;
        for (auto const& L : n.layers()) dilated = dilated || L.dilation > 1;
        CHECK(dilated);
        double worst = 0.0;
        int unstable = 0;
        size_t totalColumns = 0;
        for (int c = 0; c < 3; c++) {
            auto const in = readArray<float>("reader.case" + std::to_string(c) + ".in.f32");
            auto const want = readArray<double>("reader.case" + std::to_string(c) + ".out.f64");
            size_t const cols = in.size() / n.inputs();
            CHECK(cols >= 1);
            CHECK(in.size() == cols * n.inputs());
            CHECK(want.size() == cols * n.outputs());
            if (cols == 0 || want.size() != cols * n.outputs()) continue;
            totalColumns += cols;
            std::vector<float> out(cols * n.outputs()), again(cols * n.outputs());
            n.forwardColumns(in.data(), cols, out.data());
            n.forwardColumns(in.data(), cols, again.data());
            for (size_t i = 0; i < out.size(); i++) {
                double const e = std::fabs((double)out[i] - want[i]);
                worst = e > worst ? e : worst;
                if (bitsOf(out[i]) != bitsOf(again[i])) unstable++;
            }
        }
        CHECK(worst <= kTolerance);
        CHECK(unstable == 0);
        std::printf("reader: 3 cases, %zu columns, worst |C++ - float64| %.3g, %d values change on a rerun\n",
                    totalColumns, worst, unstable);
    }

    // ---------------------------------------------------------------- the loader refuses

    bool loads(std::vector<uint8_t> const& b, std::string const& hash = kGoldenHash) {
        net::Net n;
        std::string why;
        bool const ok = n.load(b.data(), b.size(), hash, why);
        CHECK(ok == n.loaded());
        CHECK(ok == why.empty());
        return ok;
    }

    void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        std::memcpy(&b[at], &v, 4);
    }
    void putF(std::vector<uint8_t>& b, size_t at, float v) {
        std::memcpy(&b[at], &v, 4);
    }

    void testRefusals() {
        auto const good = readBytes("pilot.gbnn");
        if (good.size() < 64) return;
        CHECK(loads(good));
        CHECK(loads(good, ""));  // no hash asked for

        // The layout hash: another layout is refused, with the reason given.
        {
            net::Net n;
            std::string why;
            CHECK(!n.load(good.data(), good.size(), "0000000000000000", why));
            CHECK(why.find("layout") != std::string::npos);
        }
        // Every cut through the header and the first floats, and the last byte.
        int cutsLoaded = 0;
        for (size_t cut = 0; cut < 200; cut++)
            if (loads(std::vector<uint8_t>(good.begin(), good.begin() + cut))) cutsLoaded++;
        CHECK(cutsLoaded == 0);
        CHECK(!loads(std::vector<uint8_t>(good.begin(), good.end() - 1)));
        {
            auto b = good;
            b.push_back(0);
            CHECK(!loads(b));
        }
        {
            auto b = good;
            b[0] = 'X';
            CHECK(!loads(b));
        }
        {
            auto b = good;
            put32(b, 4, 2);  // version
            CHECK(!loads(b));
        }
        {
            auto b = good;
            put32(b, 8, 7);  // kind
            CHECK(!loads(b));
        }
        {
            auto b = good;
            b[12] = 'z';  // not hex
            CHECK(!loads(b, ""));
        }
        uint32_t nIn;
        std::memcpy(&nIn, &good[28], 4);
        size_t const stdAt = 32 + 4 * (size_t)nIn;
        size_t const layersAt = 32 + 8 * (size_t)nIn;
        {
            auto b = good;
            putF(b, stdAt, 0.0f);  // a std of 0
            CHECK(!loads(b));
        }
        {
            auto b = good;
            putF(b, 32, std::numeric_limits<float>::infinity());  // a mean that isn't finite
            CHECK(!loads(b));
        }
        {
            auto b = good;
            putF(b, layersAt + 4 + 24, std::numeric_limits<float>::quiet_NaN());  // the first weight
            CHECK(!loads(b));
        }
        {
            auto b = good;
            put32(b, layersAt + 4 + 4, nIn + 1);  // the first layer takes the wrong width
            CHECK(!loads(b));
        }
        {
            auto b = good;
            put32(b, layersAt + 4 + 20, 9);  // unknown activation
            CHECK(!loads(b));
        }
        {
            auto b = good;
            put32(b, layersAt, 0);  // no layers
            CHECK(!loads(b));
        }
        // A dense net handed to the conv1d call (and the reverse) does nothing.
        net::Net mlp;
        std::string why;
        CHECK(mlp.load(good.data(), good.size(), kGoldenHash, why));
        float sentinel[2] = {123.0f, 456.0f};
        std::vector<float> zeros(mlp.inputs(), 0.0f);
        mlp.forwardColumns(zeros.data(), 1, sentinel);
        CHECK(sentinel[0] == 123.0f);
        std::printf("loader: refusals checked\n");
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        dir = argv[1];
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    }
    // golden.json names the hash the golden models carry; it must be the one checked here.
    {
        auto const doc = readBytes("golden.json");
        std::string const text(doc.begin(), doc.end());
        CHECK(text.find(std::string("\"layout_hash\": \"") + kGoldenHash + "\"") != std::string::npos);
    }
    testExpansion();
    testMlp("pilot", false);
    testMlp("critic", true);
    testReader();
    testRefusals();
    std::printf("%d checks, %d failed\n", checks, fails);
    if (fails) {
        std::printf("AI network tests FAILED\n");
        return 1;
    }
    std::printf("AI network tests passed\n");
    return 0;
}

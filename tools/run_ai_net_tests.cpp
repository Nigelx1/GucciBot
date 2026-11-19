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
//    weight that isn't finite;
//  - self-training (src/ai/train, 2026-11-19): the GBNN writer gives back
//    every golden file byte for byte; the trainer's forward is Net::forward's
//    to the bit; its gradient matches a float64 finite difference on a tiny
//    net (tanh, ReLU, sigmoid, two outputs, one trained); the loss falls on a
//    toy set; the same steps twice give the same bits.
//
// Usage: run_ai_net_tests [golden dir] (default tools/ai/golden).

#include "ai/net.hpp"
#include "ai/obs_core.hpp"
#include "ai/train.hpp"

#include <chrono>
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

    // ---------------------------------------------------------------- self-training

    uint64_t s_rng = 0x5EEDF00Dull;
    double urand() {  // splitmix64 -> [0, 1)
        uint64_t z = (s_rng += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return (double)(z >> 11) * (1.0 / 9007199254740992.0);
    }
    float srand1() {  // [-1, 1)
        return (float)(urand() * 2.0 - 1.0);
    }

    // A GBNN laid out as the file is ([out][in] weights), random weights.
    std::vector<uint8_t> makeGbnn(std::vector<uint32_t> const& widths, std::vector<net::Act> const& acts, bool randomNorm) {
        std::vector<uint8_t> b;
        auto p32 = [&](uint32_t v) {
            uint8_t p[4];
            std::memcpy(p, &v, 4);
            b.insert(b.end(), p, p + 4);
        };
        auto pf = [&](float v) {
            uint8_t p[4];
            std::memcpy(p, &v, 4);
            b.insert(b.end(), p, p + 4);
        };
        for (char c : std::string("GBNN")) b.push_back((uint8_t)c);
        p32(1);
        p32(0);
        for (char c : std::string("0123456789abcdef")) b.push_back((uint8_t)c);
        p32(widths[0]);
        for (uint32_t i = 0; i < widths[0]; i++) pf(randomNorm ? srand1() : 0.0f);
        for (uint32_t i = 0; i < widths[0]; i++) pf(randomNorm ? (float)(0.5 + 1.5 * urand()) : 1.0f);
        p32((uint32_t)acts.size());
        for (size_t l = 0; l < acts.size(); l++) {
            uint32_t const in = widths[l], out = widths[l + 1];
            p32(0), p32(in), p32(out), p32(1), p32(1), p32((uint32_t)acts[l]);
            float const s = 1.7f / std::sqrt((float)in);
            for (uint32_t k = 0; k < in * out; k++) pf(s * srand1());
            for (uint32_t o = 0; o < out; o++) pf(0.1f * srand1());
        }
        p32(widths.back());
        for (uint32_t o = 0; o < widths.back(); o++) {
            std::string const name = "out" + std::to_string(o);
            p32((uint32_t)name.size());
            b.insert(b.end(), name.begin(), name.end());
        }
        return b;
    }

    // The mean loss in float64, written separately from train.cpp (1/(1+e^-v) for the sigmoid).
    double refLoss(net::Net const& n, std::vector<std::vector<double>> const& w, std::vector<std::vector<double>> const& bias,
                   float const* rows, float const* targets, size_t count, int out) {
        auto const& L = n.layers();
        double total = 0.0;
        for (size_t r = 0; r < count; r++) {
            std::vector<double> h(n.inputs());
            for (size_t i = 0; i < h.size(); i++)
                h[i] = ((double)rows[r * n.inputs() + i] - (double)n.mean()[i]) / (double)n.stdev()[i];
            for (size_t l = 0; l < L.size(); l++) {
                size_t const I = L[l].nIn, O = L[l].nOut;
                std::vector<double> y(O);
                for (size_t o = 0; o < O; o++) {
                    double s = bias[l][o];
                    for (size_t i = 0; i < I; i++) s += h[i] * w[l][i * O + o];
                    switch (L[l].act) {
                    case net::Act::Relu: s = s > 0.0 ? s : 0.0; break;
                    case net::Act::Sigmoid: s = 1.0 / (1.0 + std::exp(-s)); break;
                    case net::Act::Tanh: s = std::tanh(s); break;
                    case net::Act::None: break;
                    }
                    y[o] = s;
                }
                h = y;
            }
            double const z = h[(size_t)out], t = targets[r];
            total += std::max(z, 0.0) - z * t + std::log1p(std::exp(-std::fabs(z)));
        }
        return total / (double)count;
    }

    void testWriter() {
        for (char const* k : {"pilot", "critic", "reader"}) {
            auto const bytes = readBytes(std::string(k) + ".gbnn");
            net::Net n;
            std::string why;
            bool const ok = n.load(bytes.data(), bytes.size(), kGoldenHash, why);
            CHECK(ok);
            if (!ok) continue;
            auto const again = train::save(n);
            CHECK(again == bytes);
            if (again != bytes) std::printf("FAIL writer: %s.gbnn does not come back byte for byte\n", k);
        }
        CHECK(train::save(net::Net()).empty());
        CHECK(train::fingerprint({}) == "cbf29ce484222325");
        std::printf("writer: the golden networks come back byte for byte\n");
    }

    void testTrainerForward() {
        net::Net n;
        std::string why;
        if (!n.loadFile(dir + "pilot.gbnn", kGoldenHash, why)) return;  // testMlp reported it
        auto const in = readArray<float>("pilot.in.f32");
        size_t const cases = in.size() / n.inputs();
        train::Trainer tr;
        CHECK(tr.begin(&n, 0, train::Options{}, why));
        CHECK(!tr.begin(&n, 1, train::Options{}, why));  // no output 1
        CHECK(tr.begin(&n, 0, train::Options{}, why));
        int differ = 0;
        float out[4];
        for (size_t c = 0; c < cases; c++) {
            n.forward(&in[c * n.inputs()], out);
            for (float t : {0.0f, 1.0f})
                if (tr.loss(&in[c * n.inputs()], &t, 1) != train::bce(out[0], t)) differ++;
        }
        CHECK(cases > 0);
        CHECK(differ == 0);
        std::printf("trainer forward: %zu golden cases, %d losses differ from Net::forward's\n", cases, differ);
    }

    void testGradient() {
        s_rng = 0x5EEDF00Dull;
        auto const bytes = makeGbnn({6, 5, 4, 3, 2}, {net::Act::Tanh, net::Act::Relu, net::Act::Sigmoid, net::Act::None}, true);
        net::Net n;
        std::string why;
        bool const ok = n.load(bytes.data(), bytes.size(), "", why);
        if (!ok) std::printf("FAIL gradient net: %s\n", why.c_str());
        CHECK(ok);
        if (!ok) return;
        size_t const count = 16;
        std::vector<float> rows(count * 6), targets(count);
        for (auto& v : rows) v = 2.0f * srand1();
        for (auto& t : targets) t = urand() < 0.5 ? 1.0f : 0.0f;
        int const out = 1;  // the second output is the trained one
        train::Trainer tr;
        CHECK(tr.begin(&n, out, train::Options{}, why));
        double const lossF = tr.gradient(rows.data(), targets.data(), count);
        auto const& L = n.layers();
        std::vector<std::vector<double>> w(L.size()), bias(L.size());
        for (size_t l = 0; l < L.size(); l++) {
            w[l].assign(L[l].w.begin(), L[l].w.end());
            bias[l].assign(L[l].b.begin(), L[l].b.end());
        }
        double const lossD = refLoss(n, w, bias, rows.data(), targets.data(), count, out);
        CHECK(std::fabs(lossF - lossD) < 1e-5);
        double const h = 1e-5;
        double worst = 0.0;
        int bad = 0, params = 0;
        auto check = [&](double analytic, double& wv) {
            double const keep = wv;
            wv = keep + h;
            double const up = refLoss(n, w, bias, rows.data(), targets.data(), count, out);
            wv = keep - h;
            double const down = refLoss(n, w, bias, rows.data(), targets.data(), count, out);
            wv = keep;
            double const fd = (up - down) / (2.0 * h);
            double const e = std::fabs(analytic - fd);
            worst = e > worst ? e : worst;
            params++;
            if (e > 1e-5 + 1e-3 * std::fabs(fd)) {
                if (bad++ < 5) std::printf("FAIL gradient: analytic %.9g, finite difference %.9g\n", analytic, fd);
            }
        };
        for (size_t l = 0; l < L.size(); l++) {
            for (size_t k = 0; k < w[l].size(); k++) check(tr.gradW()[l][k], w[l][k]);
            for (size_t k = 0; k < bias[l].size(); k++) check(tr.gradB()[l][k], bias[l][k]);
        }
        CHECK(bad == 0);
        CHECK(params == 6 * 5 + 5 + 5 * 4 + 4 + 4 * 3 + 3 + 3 * 2 + 2);
        // The untrained output gets no gradient.
        auto const& last = L.back();
        bool zero = tr.gradB().back()[0] == 0.0f;
        for (size_t i = 0; i < last.nIn; i++) zero = zero && tr.gradW().back()[i * last.nOut + 0] == 0.0f;
        CHECK(zero);
        std::printf("gradient: %d weights against a float64 finite difference, worst gap %.3g, %d off\n", params, worst, bad);
    }

    void testLearns() {
        s_rng = 0xC0FFEEull;
        auto const bytes = makeGbnn({6, 16, 16, 1}, {net::Act::Relu, net::Act::Relu, net::Act::None}, true);
        size_t const count = 512, batch = 32;
        std::vector<float> rows(count * 6), targets(count);
        for (auto& v : rows) v = 2.0f * srand1();
        net::Net n;
        std::string why;
        CHECK(n.load(bytes.data(), bytes.size(), "", why));
        if (!n.loaded()) return;
        for (size_t r = 0; r < count; r++) {
            float const a = (rows[r * 6 + 0] - n.mean()[0]) / n.stdev()[0];
            float const b = (rows[r * 6 + 1] - n.mean()[1]) / n.stdev()[1];
            targets[r] = a + 0.5f * b > 0.0f ? 1.0f : 0.0f;
        }
        train::Options o;
        o.lr = 3e-3f;
        auto run = [&](net::Net& net, size_t& wrongBefore, size_t& wrongAfter, double& before, double& after) {
            train::Trainer tr;
            std::string w;
            CHECK(tr.begin(&net, 0, o, w));
            before = tr.loss(rows.data(), targets.data(), count, &wrongBefore);
            for (size_t s = 0; s < 400; s++) {
                size_t const at = (s * batch) % count;
                tr.step(&rows[at * 6], &targets[at], batch);
            }
            after = tr.loss(rows.data(), targets.data(), count, &wrongAfter);
            CHECK(tr.steps() == 400);
        };
        size_t wb = 0, wa = 0;
        double before = 0.0, after = 0.0;
        run(n, wb, wa, before, after);
        CHECK(after < 0.5 * before);
        CHECK(wa < wb);
        CHECK(wa <= count / 10);
        // The same steps from the same start give the same bits.
        net::Net n2;
        CHECK(n2.load(bytes.data(), bytes.size(), "", why));
        size_t wb2 = 0, wa2 = 0;
        double before2 = 0.0, after2 = 0.0;
        run(n2, wb2, wa2, before2, after2);
        auto const saved = train::save(n);
        CHECK(train::save(n2) == saved);
        CHECK(saved != bytes);
        // The trained network written and read back is the same network.
        net::Net n3;
        CHECK(n3.load(saved.data(), saved.size(), "0123456789abcdef", why));
        CHECK(train::save(n3) == saved);
        std::printf("learns: toy set of %zu, loss %.4f -> %.4f, wrong %zu -> %zu, a rerun gives the same bits\n", count,
                    before, after, wb, wa);

        // How fast, for the pilot's own shape (494 -> 256 -> 256 -> 1): printed, not checked.
        s_rng = 7;
        auto const big = makeGbnn({494, 256, 256, 1}, {net::Act::Relu, net::Act::Relu, net::Act::None}, true);
        net::Net p;
        if (!p.load(big.data(), big.size(), "", why)) return;
        size_t const prow = 1024;
        std::vector<float> prows(prow * 494), pt(prow);
        for (auto& v : prows) v = srand1();
        for (auto& t : pt) t = urand() < 0.3 ? 1.0f : 0.0f;
        train::Trainer tp;
        if (!tp.begin(&p, 0, train::Options{}, why)) return;
        auto const t0 = std::chrono::steady_clock::now();
        for (size_t s = 0; s < prow / 64; s++) tp.step(&prows[s * 64 * 494], &pt[s * 64], 64);
        auto const t1 = std::chrono::steady_clock::now();
        double const us = std::chrono::duration<double, std::micro>(t1 - t0).count() / (double)prow;
        std::printf("speed: a 494-256-256-1 pilot trains at %.1f us a row (minibatches of 64)\n", us);
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
    testWriter();
    testTrainerForward();
    testGradient();
    testLearns();
    std::printf("%d checks, %d failed\n", checks, fails);
    if (fails) {
        std::printf("AI network tests FAILED\n");
        return 1;
    }
    std::printf("AI network tests passed\n");
    return 0;
}

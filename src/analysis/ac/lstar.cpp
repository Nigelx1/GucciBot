// L* -- a whole macro's frame windows reduced to one difficulty number:
// the precision a player would need to clear the level inside a target
// time, given every miss costs a restart. Higher is harder.
//
// The formula is NaN GD's, published at nandl.pages.dev/#formula -- he
// calls it a PRECISION rate in sigma/s, not a difficulty score, and says
// plainly that frame windows alone do not determine difficulty. Worth
// keeping that framing: every term here maps onto his page one-to-one
// (s_i = 1/2 w_i L, the nerve/fatigue/cps multipliers, E[T_C] = E[T_A]/P(C),
// and L* defined as the L solving E[T_C] = 24h).
//
// The C++ came via C0nscious's implementation of that formula in Frame
// Window Counter (MIT, github.com/hyper-5/frame-window-counter,
// src/Math/Calculator.cpp) -- a mod named after NaN's video series. This
// is anticroom's port of it, which came across with the rest of his
// analyzer; the readout that drives it lives in the GUI's Frame Windows
// tab and is GucciBot's own, because he finished his after sending the
// source. Upstream solves eight penalty combinations on eight threads and
// picks between them; this solves the one combination the settings
// select, which is the same math with the dead variants not computed.

#include "lstar.hpp"

#include <Geode/Geode.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace lstar {

namespace {

constexpr int ERFC_STEPS = 100000;
constexpr double ERFC_MAX = 6.0;
double g_erfc[ERFC_STEPS + 1];
bool g_erfcReady = false;

void buildErfcTable() {
    if (g_erfcReady) return;
    for (int i = 0; i <= ERFC_STEPS; i++)
        g_erfc[i] = std::erfc(static_cast<double>(i) / ERFC_STEPS * ERFC_MAX);
    g_erfcReady = true;
}

double fastErfc(double x) {
    if (std::isnan(x)) return 1.0;
    if (x <= 0.0) return 1.0;
    if (x >= ERFC_MAX) return 0.0;

    double const scaled = x * (ERFC_STEPS / ERFC_MAX);
    int const idx = static_cast<int>(scaled);
    if (idx >= ERFC_STEPS) return 0.0;
    double const frac = scaled - idx;
    return g_erfc[idx] + frac * (g_erfc[idx + 1] - g_erfc[idx]);
}

constexpr double SIGMA_SCALE = 0.5 * 0.7071067811865475;
constexpr double L_MIN = 1e-6;
constexpr double L_MAX = 1e12;
constexpr int SECANT_STEPS = 64;
constexpr int BISECT_STEPS = 200;

struct Prepared {
    std::vector<double> m_weight;
    std::vector<double> m_time;
};

Prepared prepare(std::vector<Input> const& inputs, Settings const& s) {
    Prepared out;
    out.m_weight.reserve(inputs.size());
    out.m_time.reserve(inputs.size());

    double const tps = s.m_tps > 0.0 ? s.m_tps : 240.0;
    bool const useCps = s.m_useCps && s.m_cps > 0.0;

    double maxFrames = 0.0;
    for (auto const& in : inputs) {
        if (!in.m_ignored)
            maxFrames =
                std::max(maxFrames, in.m_frames <= 0.0 ? 1.0 : in.m_frames);
    }

    double prevTime = 0.0;
    uint32_t prevInput = 0;

    for (size_t i = 0; i < inputs.size(); i++) {
        auto const& in = inputs[i];
        uint32_t const number = in.m_input > 0 ? in.m_input : i + 1;
        double const t = s.m_respawnSeconds + in.m_frame / tps;

        double dt = 1.0;
        if (number != prevInput) {
            dt = (t - prevTime) / ((double)number - prevInput);
            if (dt == 0.0 || !std::isfinite(dt)) dt = 1.0;
        }
        prevTime = t;
        prevInput = number;

        if (in.m_ignored && !useCps) {
            out.m_weight.push_back(std::numeric_limits<double>::infinity());
            out.m_time.push_back(t);
            continue;
        }

        double frames = in.m_ignored ? maxFrames + 1.0 : in.m_frames;
        if (frames <= 0.0) frames = 1.0;

        double w = (frames / tps) * SIGMA_SCALE;
        if (s.m_useNerve) w *= std::exp(-s.m_nerve * t);
        if (s.m_useFatigue) w *= std::exp(-s.m_fatigue * number);
        if (useCps)
            w *= std::pow(4.0, s.m_cps) /
                 std::max(1.0, std::pow(2.0 / dt, s.m_cps));

        out.m_weight.push_back(w);
        out.m_time.push_back(t);
    }
    return out;
}

double expectedTime(Prepared const& p, size_t first, size_t last, double L) {
    if (std::isnan(L) || L <= 0.0) return 1e100;

    double survive = 1.0;
    double failCost = 0.0;

    for (size_t j = first; j <= last; j++) {
        double const x = p.m_weight[j] * L;
        if (x > ERFC_MAX) continue;

        double const fail = fastErfc(x);
        double pass = 1.0 - fail;
        if (pass < 1e-15) pass = 1e-15;

        failCost += p.m_time[j] * survive * fail;
        survive *= pass;
        if (survive < 1e-200) return 1e100;
    }

    if (survive <= 0.0 || std::isnan(survive) || std::isnan(failCost))
        return 1e100;

    double const total = (p.m_time[last] * survive + failCost) / survive;
    return (std::isnan(total) || std::isinf(total)) ? 1e100 : total;
}

double solveAt(Prepared const& p, size_t last, size_t& first, double target,
               double seed) {
    if (std::isnan(seed) || seed < L_MIN) seed = L_MIN;

    while (first < last && p.m_weight[first] * seed > ERFC_MAX + 0.5) first++;

    double l0 = seed;
    double f0 = expectedTime(p, first, last, l0) - target;
    if (f0 <= 0.0 && f0 > -1.0) return l0;

    double l1 = l0 * 1.001 + 0.001;
    double f1 = expectedTime(p, first, last, l1) - target;
    double next = l1;
    bool converged = false;

    for (int i = 0; i < SECANT_STEPS; i++) {
        double const denom = f1 - f0;
        if (std::abs(denom) < 1e-9 || !std::isfinite(denom)) break;

        double const step = f1 * (l1 - l0) / denom;
        if (!std::isfinite(step)) break;

        next = std::clamp(l1 - step, L_MIN, L_MAX);
        double const fn = expectedTime(p, first, last, next) - target;
        if (std::isnan(fn)) break;
        if (std::abs(fn) < 0.5) {
            converged = true;
            break;
        }
        l0 = l1;
        f0 = f1;
        l1 = next;
        f1 = fn;
    }

    if (!converged) {
        double left = std::max(seed, L_MIN);
        double right = left + 10.0;
        for (int i = 0; i < 64 && expectedTime(p, first, last, right) > target;
             i++) {
            right *= 2.0;
            if (right > L_MAX) {
                right = L_MAX;
                break;
            }
        }
        for (int i = 0; i < BISECT_STEPS; i++) {
            double const mid = (left + right) * 0.5;
            if (expectedTime(p, first, last, mid) > target)
                left = mid;
            else
                right = mid;
            if (right - left < 0.005) break;
            if ((right - left) / std::max(mid, 1.0) < 1e-4) break;
        }
        next = (left + right) * 0.5;
    }

    return std::isfinite(next) ? std::clamp(next, L_MIN, L_MAX) : L_MIN;
}

bool order(std::vector<Input>& inputs) {
    bool const anyWindow =
        std::any_of(inputs.begin(), inputs.end(),
                    [](Input const& in) { return !in.m_ignored; });
    if (!anyWindow) return false;

    std::stable_sort(inputs.begin(), inputs.end(),
                     [](Input const& a, Input const& b) {
                         if (a.m_frame != b.m_frame)
                             return a.m_frame < b.m_frame;
                         return a.m_input < b.m_input;
                     });
    return true;
}

}  // namespace

double solve(std::vector<Input> inputs, Settings const& settings) {
    if (!order(inputs)) return 0.0;

    buildErfcTable();

    Prepared const p = prepare(inputs, settings);
    double const target =
        settings.m_targetSeconds > 0.0 ? settings.m_targetSeconds : 86400.0;

    size_t first = 0;
    return solveAt(p, p.m_weight.size() - 1, first, target, L_MIN);
}

Solver* Solver::get() {
    static Solver instance;
    return &instance;
}

void Solver::cancel() {
    m_generation++;
    m_running = false;
    m_progress = 0.f;
}

void Solver::start(std::vector<Input> inputs, Settings settings) {
    this->cancel();

    m_result = Result{};
    m_dirty = false;

    if (!order(inputs)) return;

    buildErfcTable();

    int const generation = m_generation.load();
    auto pending = std::make_shared<Result>();
    m_pending = pending;
    m_running = true;
    m_progress = 0.f;

    std::thread([this, generation, pending, inputs = std::move(inputs),
                 settings]() mutable {
        Prepared const p = prepare(inputs, settings);
        double const target =
            settings.m_targetSeconds > 0.0 ? settings.m_targetSeconds : 86400.0;

        std::vector<double> per;
        per.reserve(p.m_weight.size());

        size_t first = 0;
        double seed = L_MIN;
        for (size_t i = 0; i < p.m_weight.size(); i++) {
            if (m_generation.load() != generation) return;

            seed = solveAt(p, i, first, target, seed);
            per.push_back(seed);

            if ((i & 0x3F) == 0)
                m_progress.store(static_cast<float>(i + 1) /
                                 static_cast<float>(p.m_weight.size()) * 100.f);
        }

        if (m_generation.load() != generation) return;

        pending->m_ok = true;
        pending->m_value = per.empty() ? 0.0 : per.back();
        pending->m_perInput = std::move(per);
        pending->m_frames.reserve(inputs.size());
        for (auto const& in : inputs) pending->m_frames.push_back(in.m_frame);

        geode::queueInMainThread([this, generation, pending]() {
            if (m_generation.load() != generation) return;
            m_result = *pending;
            m_progress.store(100.f);
            m_running = false;
        });
    }).detach();
}

}  // namespace lstar

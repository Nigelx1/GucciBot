#include "analysis/ac/steer.hpp"

#include <algorithm>
#include <cmath>

// GucciBot (2026-11-10): Ship Steering's controller (steer.hpp). Deterministic:
// plain arithmetic on the inputs, no clock, no randomness, ties to the
// earlier frame and to keeping the button.

namespace gucci::steer {

namespace {
constexpr float kGrid = 0.05f;
constexpr size_t kMaxGrid = 4000;

struct Sample {
    float v;      // normalized velocity before the tick
    float dv;     // normalized change over the tick
    size_t frame;
};

float median(std::vector<float>& xs) {
    if (xs.empty()) return 0.f;
    std::sort(xs.begin(), xs.end());
    size_t const n = xs.size();
    return n % 2 ? xs[n / 2] : 0.5f * (xs[n / 2 - 1] + xs[n / 2]);
}

// The median dv of the kNearest samples nearest v; `sorted` is by (v, frame).
float nearestMedian(std::vector<Sample> const& sorted, float v) {
    size_t const n = sorted.size();
    if (n == 0) return 0.f;
    size_t hi = static_cast<size_t>(
        std::lower_bound(sorted.begin(), sorted.end(), v,
                         [](Sample const& s, float x) { return s.v < x; }) -
        sorted.begin());
    size_t lo = hi;  // [lo, hi) taken so far
    std::vector<float> picked;
    picked.reserve(kNearest);
    while (picked.size() < static_cast<size_t>(kNearest) && (lo > 0 || hi < n)) {
        bool takeLow;
        if (lo == 0) takeLow = false;
        else if (hi >= n) takeLow = true;
        else {
            float const dl = v - sorted[lo - 1].v;
            float const dh = sorted[hi].v - v;
            if (dl != dh) takeLow = dl < dh;
            else takeLow = sorted[lo - 1].frame <= sorted[hi].frame;  // ties: the earlier frame
        }
        if (takeLow) picked.push_back(sorted[--lo].dv);
        else picked.push_back(sorted[hi++].dv);
    }
    return median(picked);
}
}  // namespace

int ticksAt(int ticks240, double tps) {
    return std::max(1, static_cast<int>(std::lround(ticks240 * tps / 240.0)));
}

int reactTicks(int ms, double tps) {
    return std::max(0, static_cast<int>(std::lround(ms * tps / 1000.0)));
}

float Model::accel(bool hold, float v) const {
    auto const& t = hold ? accelHold : accelFree;
    if (t.empty()) return 0.f;
    long idx = std::lround((v - gridLo) / kGrid);
    idx = std::clamp<long>(idx, 0, static_cast<long>(t.size()) - 1);
    return t[static_cast<size_t>(idx)];
}

float Model::predict(float y, float v, bool hold, int k) const {
    for (int i = 0; i < k; i++) {
        v = std::clamp(v + this->accel(hold, v), vMin, vMax);
        y += lift * v;
    }
    return y;
}

Model fit(LinePoint const* line, size_t size, size_t lo, size_t hi, bool upside) {
    Model m;
    if (!line || size < 2) return m;
    hi = std::min(hi, size - 1);
    if (lo >= hi) return m;

    float const g = upside ? -1.f : 1.f;
    auto const usable = [&](LinePoint const& p) {
        uint8_t const need = kValid | kShip | kAlive;
        return (p.flags & need) == need && !(p.flags & kGround) &&
               static_cast<bool>(p.flags & kUpside) == upside;
    };

    struct Raw {
        float vBefore, vAfter, dv, ratio;
        bool hold;
        size_t frame;
    };
    std::vector<Raw> raw;
    for (size_t f = lo; f < hi; f++) {
        auto const& a = line[f];
        auto const& b = line[f + 1];
        if (!usable(a) || !usable(b)) continue;
        if ((a.flags & kMini) != (b.flags & kMini)) continue;
        if ((a.flags & kHold) != (b.flags & kHold)) continue;
        if (b.vy == 0.f) continue;
        float const dv = b.vy - a.vy;
        if (std::fabs(dv) >= 1.f) continue;
        raw.push_back({a.vy, b.vy, dv, g * (b.y - a.y) / b.vy,
                       static_cast<bool>(b.flags & kHold), f});
    }
    if (raw.empty()) return m;

    std::vector<float> ratios;
    ratios.reserve(raw.size());
    for (auto const& r : raw) ratios.push_back(r.ratio);
    float const liftSigned = median(ratios);
    if (liftSigned == 0.f) return m;
    m.vSign = liftSigned < 0.f ? -1.f : 1.f;
    m.lift = std::fabs(liftSigned);

    std::vector<Sample> hold, free;
    float vLo = 1e9f, vHi = -1e9f;
    m.vMax = -1e9f;
    m.vMin = 1e9f;
    for (auto const& r : raw) {
        float const v = m.vSign * r.vBefore;
        float const va = m.vSign * r.vAfter;
        (r.hold ? hold : free).push_back({v, m.vSign * r.dv, r.frame});
        vLo = std::min({vLo, v, va});
        vHi = std::max({vHi, v, va});
        if (r.hold) m.vMax = std::max(m.vMax, va);
        else m.vMin = std::min(m.vMin, va);
    }
    m.samplesHold = static_cast<int>(hold.size());
    m.samplesFree = static_cast<int>(free.size());
    if (m.samplesHold < kMinSamples || m.samplesFree < kMinSamples) return m;
    if (m.vMin >= m.vMax) return m;

    auto const bySpeed = [](Sample const& a, Sample const& b) {
        return a.v != b.v ? a.v < b.v : a.frame < b.frame;
    };
    std::sort(hold.begin(), hold.end(), bySpeed);
    std::sort(free.begin(), free.end(), bySpeed);

    m.gridLo = std::floor(vLo / kGrid) * kGrid;
    size_t const n = std::min(kMaxGrid, static_cast<size_t>(std::lround((vHi - m.gridLo) / kGrid)) + 1);
    m.accelHold.resize(n);
    m.accelFree.resize(n);
    for (size_t i = 0; i < n; i++) {
        float const v = m.gridLo + static_cast<float>(i) * kGrid;
        m.accelHold[i] = nearestMedian(hold, v);
        m.accelFree[i] = nearestMedian(free, v);
    }
    m.ok = true;
    return m;
}

// Each option is "this hold for the dwell, then the recording's own holds"
// over the look-ahead, scored by the mean distance from the line. Following
// the recording after the correction is what makes y and y-velocity meet the
// line together, so the handback can happen. A change has to beat keeping by
// kSwitchMargin, and ties keep.
static float optionCost(Model const& m, float y, float v, bool first, int dwell, int look, float g,
                        LinePoint const* ahead) {
    float cost = 0.f;
    for (int k = 0; k < look; k++) {
        bool const hold = k < dwell ? first : static_cast<bool>(ahead[k].flags & kHold);
        v = std::clamp(v + m.accel(hold, v), m.vMin, m.vMax);
        y += m.lift * v;
        cost += std::fabs(y - g * ahead[k].y);
    }
    return cost / static_cast<float>(std::max(1, look));
}

Decision decide(Model const& m, State const& s, int since, int dwell, int look,
                LinePoint const* ahead, LinePoint const& now, float* eOut) {
    if (eOut) *eOut = 0.f;
    if (!m.ok || !ahead || look <= 0) return Decision::Keep;
    float const g = s.upside ? -1.f : 1.f;
    float const y = g * s.y;
    float const v = m.vSign * s.vy;
    float const keep = optionCost(m, y, v, s.hold, dwell, look, g, ahead);
    if (eOut) *eOut = keep;
    if (since < dwell) return Decision::Keep;
    // On the floor (or the ceiling upside down) where the line stays in
    // contact too, the prediction has no floor and would say "falling": keep.
    LinePoint const& end = ahead[look - 1];
    if (s.ground && (end.flags & kGround) && std::fabs(g * end.y - g * now.y) <= kSwitchMargin * 4.f)
        return Decision::Keep;
    float const flip = optionCost(m, y, v, !s.hold, dwell, look, g, ahead);
    if (flip + kSwitchMargin < keep) return s.hold ? Decision::Release : Decision::Press;
    return Decision::Keep;
}

bool onLine(Model const& m, State const& s, LinePoint const& at) {
    if (!(at.flags & kValid)) return false;
    if (static_cast<bool>(at.flags & kUpside) != s.upside) return false;
    if (static_cast<bool>(at.flags & kHold) != s.hold) return false;
    float const g = s.upside ? -1.f : 1.f;
    return std::fabs(g * (s.y - at.y)) <= kHandbackDy &&
           std::fabs(m.vSign * (s.vy - at.vy)) <= kHandbackDvy;
}

}  // namespace gucci::steer

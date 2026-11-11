// Ship Steering's controller (src/analysis/ac/steer.*), offline: a synthetic
// ship with KOCMOC's measured numbers (lift 0.225 at 240 TPS; holding about
// +0.127/+0.108/+0.101 a tick by speed, not holding -0.081/-0.069, or -0.122
// going up fast), a recorded zig-zag line, and a start off the line by -20,
// +20 and 0 units. Checks: the fit finds the numbers; it hands back within
// the limit and only inside the tolerances; no button change comes sooner than
// the dwell; the same inputs give the same taps twice; too few samples is no
// model. Run by run_tests.bat.

#include "analysis/ac/steer.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace gucci::steer;

static int g_fail = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                   \
            std::printf("\n");                          \
            g_fail++;                                   \
        }                                               \
    } while (0)

struct Ship {
    float y = 100.f, vy = 0.f;
    bool hold = false;
    void tick() {
        float a;
        if (hold) a = vy < 2.f ? 0.127f : vy < 5.f ? 0.108f : 0.101f;
        else a = vy > 4.f ? -0.122f : vy > -2.f ? -0.081f : -0.069f;
        vy = std::fmax(-6.4f, std::fmin(8.f, vy + a));
        y += 0.225f * vy;
    }
};

static std::vector<LinePoint> zigzag(size_t n, int on, int off) {
    std::vector<LinePoint> line(n);
    Ship s;
    for (size_t f = 0; f < n; f++) {
        if (f > 0) {
            s.hold = ((f + off / 2) % (on + off)) < static_cast<size_t>(on);
            s.tick();
        }
        line[f] = {static_cast<float>(f) * 2.6f, s.y, s.vy,
                   static_cast<uint8_t>(kValid | kShip | kAlive | (s.hold ? kHold : 0))};
    }
    return line;
}

struct Run {
    bool handedBack = false;
    int handbackTick = -1;
    std::vector<int> taps;  // tick each tap lands on, sign = press(+)/release(-)
    int minGap = 1 << 30;
    float maxLateErr = 0.f;  // the largest |y - line| after the first 120 ticks
};

static Run steerFrom(std::vector<LinePoint> const& line, size_t start, float err, int limit) {
    Model const m = fit(line.data(), line.size(), start - 240, start + 240, false);
    Run r;
    Ship s;
    s.y = line[start].y + err;
    s.vy = line[start].vy;
    s.hold = line[start].flags & kHold;
    int const dwell = ticksAt(kDwell240, 240.0), look = ticksAt(kLook240, 240.0);
    int lastChange = static_cast<int>(start) - dwell;  // the recording's own last change, long enough ago
    for (int t = static_cast<int>(start); t < static_cast<int>(start) + limit; t++) {
        State st{s.y, s.vy, s.hold, false, false};
        if (t >= static_cast<int>(start) + 120)
            r.maxLateErr = std::fmax(r.maxLateErr, std::fabs(s.y - line[t].y));
        if (t > static_cast<int>(start) && onLine(m, st, line[t])) {
            r.handedBack = true;
            r.handbackTick = t;
            // the tolerances, checked here on their own
            CHECK(std::fabs(s.y - line[t].y) <= kHandbackDy && std::fabs(s.vy - line[t].vy) <= kHandbackDvy &&
                      s.hold == static_cast<bool>(line[t].flags & kHold),
                  "handback at %d outside the tolerances", t);
            break;
        }
        size_t const ahead = std::min(line.size() - 1, static_cast<size_t>(t + look));
        (void)ahead;
        auto const d = decide(m, st, (t + 1) - lastChange, dwell, look, &line[t + 1], line[t]);
        if (d != Decision::Keep) {
            r.minGap = std::min(r.minGap, (t + 1) - lastChange);
            lastChange = t + 1;
            s.hold = d == Decision::Press;
            r.taps.push_back(d == Decision::Press ? t + 1 : -(t + 1));
        }
        s.tick();
    }
    return r;
}

int main() {
    auto const line = zigzag(2000, 32, 44);

    // The fit.
    Model const m = fit(line.data(), line.size(), 700, 1300, false);
    CHECK(m.ok, "no model on a clean line");
    CHECK(std::fabs(m.lift - 0.225f) < 0.002f, "lift %.4f, want 0.225", m.lift);
    CHECK(std::fabs(m.accel(true, 0.5f) - 0.127f) < 0.002f, "hold accel at 0.5: %.4f", m.accel(true, 0.5f));
    CHECK(std::fabs(m.accel(false, 0.5f) + 0.081f) < 0.002f, "free accel at 0.5: %.4f", m.accel(false, 0.5f));
    CHECK(m.samplesHold >= kMinSamples && m.samplesFree >= kMinSamples, "samples %d/%d", m.samplesHold,
          m.samplesFree);

    // Too few samples: no model.
    {
        Model const few = fit(line.data(), line.size(), 100, 105, false);
        CHECK(!few.ok, "a model from 5 ticks");
    }
    // Upside down: the same numbers with y mirrored.
    {
        auto flipped = line;
        for (auto& p : flipped) {
            p.y = -p.y;
            p.vy = -p.vy;
            p.flags |= kUpside;
        }
        Model const up = fit(flipped.data(), flipped.size(), 700, 1300, true);
        CHECK(up.ok && std::fabs(up.lift - 0.225f) < 0.002f, "upside-down lift %.4f", up.lift);
        CHECK(std::fabs(up.accel(true, 0.5f) - 0.127f) < 0.002f, "upside-down hold accel %.4f",
              up.accel(true, 0.5f));
    }

    int const limit = 480;
    int const dwell = ticksAt(kDwell240, 240.0);
    for (float err : {-20.f, 20.f, 0.f, -8.f, 8.f}) {
        for (size_t start : {1000u, 1015u, 1037u, 1062u}) {
            Run const a = steerFrom(line, start, err, limit);
            Run const b = steerFrom(line, start, err, limit);
            CHECK(a.handedBack, "error %+.0f from %zu: no handback in %d ticks (%zu taps)", err, start, limit,
                  a.taps.size());
            CHECK(a.taps.empty() || a.minGap >= dwell, "error %+.0f from %zu: a change after %d ticks", err, start,
                  a.minGap);
            CHECK(a.taps == b.taps && a.handbackTick == b.handbackTick, "error %+.0f from %zu: not deterministic",
                  err, start);
            std::printf("error %+5.0f from %4zu: handback after %3d ticks, %2zu taps\n", err, start,
                        a.handbackTick - static_cast<int>(start), a.taps.size());
        }
    }

    // Timing in ticks at other TPS.
    CHECK(reactTicks(100, 240.0) == 24, "react 100 ms at 240");
    CHECK(reactTicks(100, 480.0) == 48, "react 100 ms at 480");
    CHECK(ticksAt(kDwell240, 120.0) == 5, "dwell at 120");
    CHECK(ticksAt(kDwell240, 30.0) == 1, "dwell at 30");

    if (g_fail) {
        std::printf("steer tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("steer tests: all passed\n");
    return 0;
}

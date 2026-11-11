#pragma once

// GucciBot (2026-11-10): Ship Steering for Calculate, built from Antic's
// Counter 1.0.0-beta.16's behavior (anticroom's counter; its UI, results files
// and log lines only, no source). "A ship input also counts if you could fly
// back onto the recorded line after it with taps a player could make."
//
// This file is the controller's math only: a motion model fitted from the
// capture's recorded line, the tap decision and the handback test. No GD
// types, so tools/run_steer_tests.cpp can test it offline. The analyzer
// (framewindow.cpp) feeds it the player's state and the line. The model only
// picks the taps; GD judges every death for real, so a bad model can only
// lose a shift, never invent one.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gucci::steer {

// One frame of the capture pass, for one player.
struct LinePoint {
    float x = 0.f;
    float y = 0.f;
    float vy = 0.f;
    uint8_t flags = 0;
};

enum : uint8_t {
    kValid = 1,
    kHold = 2,
    kShip = 4,
    kUpside = 8,
    kMini = 16,
    kGround = 32,  // riding the floor or the ceiling (on ground with no y-velocity)
    kAlive = 64,
};

// The controller's constants, at 240 TPS (ticks are scaled to the macro's TPS).
// Tuned once on KOCMOC's ship ranges, then frozen; not settings.
// A press or release has to bring the next 100 ms this much closer to the line
// (mean distance, level units) than keeping the button as it is.
inline constexpr float kSwitchMargin = 0.25f;
inline constexpr float kHandbackDy = 1.5f;      // back on the line: y within this
inline constexpr float kHandbackDvy = 0.6f;     // and y-velocity within this
inline constexpr int kDwell240 = 10;            // the shortest hold or let-go, in ticks at 240
inline constexpr int kLook240 = 24;             // look-ahead, in ticks at 240
inline constexpr int kFitSpan240 = 240;         // the fit uses the line within this many ticks of the input
inline constexpr int kMinSamples = 8;           // fewer for either hold state: no model, no steering
inline constexpr int kNearest = 7;              // accel = median of this many nearest samples

int ticksAt(int ticks240, double tps);          // max(1, round(ticks240 * tps / 240))
int reactTicks(int ms, double tps);             // round(ms * tps / 1000)

// The ship's motion, in the player's own up: y' = g * y, v' = s * vy, where s
// makes lift positive (so it holds whichever way GD signs the velocity).
struct Model {
    bool ok = false;
    float lift = 0.f;      // y change per tick per unit of v (0.225 at 240 TPS)
    float vSign = 1.f;     // s above
    float vMin = 0.f;      // terminal speeds, normalized
    float vMax = 0.f;
    float gridLo = 0.f;    // accel tables on a 0.05 grid from gridLo
    std::vector<float> accelHold;
    std::vector<float> accelFree;
    int samplesHold = 0;
    int samplesFree = 0;

    float accel(bool hold, float v) const;
    // y after keeping `hold` for k ticks from (y, v), both normalized.
    float predict(float y, float v, bool hold, int k) const;
};

// Fits from line[lo..hi] (inclusive), using only ticks f -> f+1 where both are
// ship, alive, the same size and gravity as `upside`, the hold doesn't change,
// vy != 0 and |dvy| < 1. ok = false below kMinSamples for either hold.
Model fit(LinePoint const* line, size_t size, size_t lo, size_t hi, bool upside);

// The player now, in world terms.
struct State {
    float y = 0.f;
    float vy = 0.f;
    bool hold = false;
    bool upside = false;
    bool ground = false;
};

enum class Decision { Keep, Press, Release };

// Every tie goes to Keep. `since` is how many ticks the current hold will have
// lasted when this decision's input lands; `ahead` the line from the tick the
// input lands on, `look` points (the caller repeats the last one past the
// end). e (keeping's mean distance from the line) is written for the log.
// The design's first controller (press below the line by 3 units 100 ms
// ahead, release above it) tracked the line but seldom met it in y and
// y-velocity together, so it rarely handed back; this one looks at the same
// 100 ms with the recording's holds after the correction.
Decision decide(Model const& m, State const& s, int since, int dwell, int look,
                LinePoint const* ahead, LinePoint const& now, float* e = nullptr);

// Back on the line: y, vy and the hold all match it (and the same gravity).
bool onLine(Model const& m, State const& s, LinePoint const& at);

}  // namespace gucci::steer

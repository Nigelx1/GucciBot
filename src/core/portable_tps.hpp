#pragma once

// Tick rates other than 240 off Windows (multiplatform branch, 2026-10-05).
//
// On Windows the rate is set by two things working together:
//   1. GB7GJBaseGameLayer::getModifiedDelta (hooks/hook_gjbasegamelayer.cpp,
//      a normal Geode hook on every platform) hands GD exactly one tick,
//      1/tps seconds, per update; runSlowLockDelta (engine_updater.cpp) calls
//      the update once per tick.
//   2. The physDt midhook inside GJBaseGameLayer::update (Silicate's) swaps
//      the "* 4" in GD's step count for "* tps / 60", so that one tick is one
//      physics step.
//
// GD's step count, read from the Windows binary (GJBaseGameLayer::update,
// 0x237a5b-0x237a99): steps = max(1, round(delta * 60 / min(timeWarp, 1) * 4)),
// i.e. round(delta * 240) at normal speed. With the hook's delta of 1/tps that
// is round(240 / tps), which is already 1 for any rate above 160: GD then runs
// one step of exactly 1/tps, the same step the Windows midhook makes (the
// per-step deltas GD derives are the delta divided by that same step count of
// 1). So off Windows no new hook is needed for those rates: only the clamp to
// 240 had to go.
//
// At 160 and below GD would split each tick into round(240 / tps) steps of
// 1/240-ish, each with its own processCommands call: the physics would not be
// the Windows physics and the frame counter (one per processCommands) would
// count several frames per tick. Those rates can't run off Windows without a
// patch at a fixed address, so they fall back to 240.
//
// Assumption, not verified: the other platforms' GD is built from the same
// source, so its step count rounds the same way. Only the Windows binary was
// read. The check in portable_tps.cpp logs it if a device ever disagrees.
//
// Several ticks in one update (Performance, Lock delta off, the editor,
// Calculate's batches) need the Windows-only physStepCount/restorePhysDt
// midhooks; at 240 getModifiedDelta stands in for them. At these other rates
// Performance steps like Accuracy (useFastLockDelta), and the rest run one
// update per tick (gdUpdateSteps in hook_gjbasegamelayer.cpp, runUpdates'
// batch fallback in engine_updater.cpp).

namespace gucci::portable_tps {

    // GD's own rate.
    inline constexpr double kGdTps = 240.0;
    // The lowest rate GD runs as one physics step per tick off Windows:
    // round(240 / tps) must be 1, so 240 / tps must stay clearly under 1.5
    // (162 leaves room for float error at the 160 boundary).
    inline constexpr double kMinTps = 162.0;

    inline bool runnable(double tps) {
        return tps == kGdTps || tps >= kMinTps;
    }

    // Off Windows: `tps` if GD can run it, otherwise 240 (with a log line and,
    // once per refused rate, a notification). Defined in portable_tps.cpp,
    // which is compiled out on Windows; nothing calls it there.
    double resolve(double tps);

} // namespace gucci::portable_tps

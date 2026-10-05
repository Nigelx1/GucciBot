// Tick rates other than 240 off Windows: which rates GD can run there, and a
// check that it runs them as one physics step per tick. The reasoning is in
// core/portable_tps.hpp. Compiled out on Windows, where the physDt midhook
// (engine_updater.cpp) sets the step and nothing here is used.

#include "core/platform.hpp"

#if !GB_NATIVE_ENGINE

#include "core/GucciBot.hpp"
#include "core/portable_tps.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <string>

using namespace geode::prelude;

namespace gucci::portable_tps {

    double resolve(double tps) {
        if (runnable(tps))
            return tps;
        // Once per refused rate: a macro with this rate asks again on every
        // reset (applyTpsAtIndex), and the log and the screen needn't say so
        // each time.
        static double lastRefused = -1.0;
        if (tps != lastRefused) {
            lastRefused = tps;
            log::warn("[GucciBot] {:.0f} TPS can't run on this platform (only 240, or {:.0f} "
                      "and up); running at 240. A macro made at {:.0f} TPS will not replay here.",
                      tps, kMinTps, tps);
            std::string msg = fmt::format("{:.0f} TPS needs Windows: running at 240", tps);
            geode::queueInMainThread([msg] {
                geode::Notification::create(msg, geode::NotificationIcon::Warning)->show();
            });
        }
        return kGdTps;
    }

} // namespace gucci::portable_tps

// The check. When the bot hands GD one tick (a level, Lock delta on, Accuracy,
// no analyzer batch), GD must run exactly one full physics step for it: a
// second processCommands with isHalfTick false in the same update means GD
// split the tick, so the frame counter (one per processCommands,
// GBPortableTick in engine_updater.cpp) ran ahead of the ticks. Half-tick
// halves (isHalfTick true) happen on Windows too and are not counted. It only
// reads state and calls each original exactly once.
namespace {
    int g_fullSteps = 0;
    bool g_warned = false;
}

class $modify(GBPortableTpsCheck, GJBaseGameLayer) {
    void update(float dt) {
        // GD can call update from inside update; each call counts its own.
        int const outer = g_fullSteps;
        g_fullSteps = 0;
        GJBaseGameLayer::update(dt);
        int const steps = g_fullSteps;
        g_fullSteps = outer;

        if (steps <= 1 || g_warned)
            return;
        auto* gb = gucci::GucciEngine::get();
        auto& upd = gb->updater;
        bool const oneTickPerUpdate = gb->enabled && !upd.m_onlyRefresh && PlayLayer::get() &&
                                      upd.m_lockDelta && !upd.useFastLockDelta() &&
                                      upd.m_analysisBatch == 0;
        if (!oneTickPerUpdate)
            return;
        g_warned = true;
        log::error("[GucciBot] GD ran {} physics steps for one tick at {:.0f} TPS (frame {}): "
                   "frame numbers here will not match Windows. Please report this.",
                   steps, upd.m_tps, upd.getFrame());
        std::string msg = fmt::format("GucciBot: {} steps in one tick at {:.0f} TPS, please report",
                                      steps, upd.m_tps);
        geode::queueInMainThread([msg] {
            geode::Notification::create(msg, geode::NotificationIcon::Error)->show();
        });
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick)
            g_fullSteps++;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

#endif // !GB_NATIVE_ENGINE

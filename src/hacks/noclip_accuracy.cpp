// Noclip accuracy (see noclip_accuracy.hpp). Written fresh for GucciBot on
// 2026-10-03; neither Silicate nor Absense has one to port.

#include "hacks/noclip_accuracy.hpp"

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <set>

using namespace geode::prelude;

namespace gucci::noclipacc {

    namespace {

        // The frames of this attempt on which a death was blocked. A set, so a
        // tick that touched several hazards (or both players) counts once.
        std::set<uint32_t> g_hits;

        void clear() {
            g_hits.clear();
            GucciEngine::get()->noclipAccuracy = 1.f;
        }

        float share(size_t hits, uint32_t ticks) {
            if (ticks == 0)
                return 1.f;
            return std::clamp(1.f - static_cast<float>(hits) / static_cast<float>(ticks), 0.f, 1.f);
        }

    } // namespace

    bool onBlockedDeath() {
        auto* gb = GucciEngine::get();
        auto const& upd = gb->updater;
        // A death happens inside a tick; the frame counter moves after it, so
        // the tick running now is the one past the counter.
        uint32_t const tick = upd.getFrame() + 1;
        uint32_t const ticks = upd.m_frame + 1;
        bool const counted = g_hits.count(tick) != 0;
        size_t const withThis = g_hits.size() + (counted ? 0 : 1);
        float const after = share(withThis, ticks);
        if (gb->noclipThreshold > 0.f && after < gb->noclipThreshold)
            return false;
        g_hits.insert(tick);
        gb->noclipAccuracy = after;
        return true;
    }

    float accuracy() {
        auto* gb = GucciEngine::get();
        auto const& upd = gb->updater;
        // A step back can take the counter behind hits it had passed; those
        // ticks have not happened on this timeline.
        g_hits.erase(g_hits.upper_bound(upd.getFrame()), g_hits.end());
        gb->noclipAccuracy = share(g_hits.size(), upd.m_frame);
        return gb->noclipAccuracy;
    }

    int hits() {
        return static_cast<int>(g_hits.size());
    }

} // namespace gucci::noclipacc

// Every reset starts a new attempt (a restart, a practice respawn, Calculate's
// own resets), and a new level starts from nothing.
class $modify(GBNoclipAccuracyPL, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        gucci::noclipacc::clear();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        gucci::noclipacc::clear();
    }
};

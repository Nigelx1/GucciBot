#include "hooks/respawn.hpp"

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <cmath>

using namespace geode::prelude;

namespace gucci::respawn {

    namespace {
        // The game's respawn sequence carries this tag (PlayLayer::destroyPlayer).
        constexpr int kRespawnActionTag = 0x10;
    }

    void retime(PlayLayer* pl, float delaySeconds) {
        if (!pl || !GucciEngine::get()->enabled)
            return;

        auto* pending = pl->getActionByTag(kRespawnActionTag);
        if (!pending)
            return; // nothing scheduled: not a real death

        float delay = delaySeconds;
        if (!std::isfinite(delay) || delay < 0.f)
            delay = 0.f;
        delay = std::min(delay, 10.f);

        pl->stopAction(pending);
        auto* sequence = CCSequence::create(
            CCDelayTime::create(delay),
            CallFuncExt::create([pl] { pl->delayedResetLevel(); }),
            nullptr);
        sequence->setTag(kRespawnActionTag);
        pl->runAction(sequence);
        log::info("[GucciBot] respawn in {:.2f}s (custom)", delay);
    }

} // namespace gucci::respawn

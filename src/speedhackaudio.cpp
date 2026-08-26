#include "GucciBot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
using namespace geode::prelude;

class $modify(GB7FMODAudio, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);
        auto* gb = GucciEngine::get();
        if (!gb->enabled)
            return;
        if (!gb->audioPitchEnabled)
            return;
        FMOD::ChannelGroup* master;
        m_system->getMasterChannelGroup(&master);
        if (master)
            master->setPitch((float)gb->updater.m_speedhack);
    }
};

#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

class GameAudioMute {
public:
    static void acquire() {
        int& count = refCount();
        count++;
        if (count == 1)
            setMuted(true);
    }
    static void release() {
        int& count = refCount();
        if (count > 0)
            count--;
        if (count == 0)
            setMuted(false);
    }

private:
    static int& refCount() {
        static int count = 0;
        return count;
    }
    static void setMuted(bool muted) {
        auto* eng = FMODAudioEngine::sharedEngine();
        if (!eng)
            return;
        if (eng->m_backgroundMusicChannel)
            eng->m_backgroundMusicChannel->setMute(muted);
        if (eng->m_globalChannel)
            eng->m_globalChannel->setMute(muted);
    }
};

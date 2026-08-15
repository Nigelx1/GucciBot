#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

// Shared, reference-counted mute for GD's own audio -- used by both BIG
// BRRRR and the Jupiter synced music so neither one accidentally unmutes
// the game while the other is still playing. Mutes FMODAudioEngine's own
// background-music and SFX channel groups specifically (m_backgroundMusicChannel,
// m_globalChannel), NOT the FMOD master group -- anything this mod plays into
// its own independent channel (passing nullptr as the channel group, or a
// dedicated group like ClickSounds') is a sibling of those, not a child, so
// it's never affected by this.
class GameAudioMute {
public:
    static void acquire() {
        int& count = refCount();
        count++;
        if (count == 1) setMuted(true);
    }
    static void release() {
        int& count = refCount();
        if (count > 0) count--;
        if (count == 0) setMuted(false);
    }

private:
    static int& refCount() { static int count = 0; return count; }
    static void setMuted(bool muted) {
        auto* eng = FMODAudioEngine::sharedEngine();
        if (!eng) return;
        if (eng->m_backgroundMusicChannel) eng->m_backgroundMusicChannel->setMute(muted);
        if (eng->m_globalChannel) eng->m_globalChannel->setMute(muted);
    }
};

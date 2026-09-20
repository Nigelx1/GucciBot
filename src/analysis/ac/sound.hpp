#ifndef ANALYSIS_SOUND_HPP
#define ANALYSIS_SOUND_HPP

#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>

#include <string>
#include <unordered_map>

class FrameWindowSound {
   public:
    static void play(std::string const& path, float volume);
    static void stopAll();
    static void clearCache();

    // GucciBot's renderer records frame-window audio onto its own track and
    // needs the group to attach to. Creates it on demand, like play() does.
    static FMOD::ChannelGroup* channelGroup();

    // Set while GucciBot is rendering. A render sets the engine's effects
    // volume to the render's SFX setting, which is routinely 0 -- and that
    // silenced these entirely, so nothing reached the recording. These have
    // their own volume; the game's SFX slider should not mute them.
    static void setRenderMode(bool on) { s_renderMode = on; }

   private:
    static bool ensureChannelGroup(FMOD::System* sys);

    static constexpr size_t MAX_ACTIVE_VOICES = 6;

    static std::unordered_map<std::string, FMOD::Sound*> s_cache;
    static FMOD::ChannelGroup* s_group;
    static bool s_renderMode;
};

#endif  // ANALYSIS_SOUND_HPP

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

   private:
    static bool ensureChannelGroup(FMOD::System* sys);

    static constexpr size_t MAX_ACTIVE_VOICES = 6;

    static std::unordered_map<std::string, FMOD::Sound*> s_cache;
    static FMOD::ChannelGroup* s_group;
};

#endif  // ANALYSIS_SOUND_HPP

#include "sound.hpp"

#include <filesystem>
#include <vector>

using namespace geode::prelude;

std::unordered_map<std::string, FMOD::Sound*> FrameWindowSound::s_cache;
FMOD::ChannelGroup* FrameWindowSound::s_group = nullptr;

static std::unordered_map<std::string, std::vector<FMOD::Channel*>>
    s_activeChannels;

bool FrameWindowSound::ensureChannelGroup(FMOD::System* sys) {
    if (!sys) return false;
    if (s_group) return true;

    FMOD_RESULT res = sys->createChannelGroup("SilicateFrameWindow", &s_group);
    return res == FMOD_OK && s_group != nullptr;
}

void FrameWindowSound::play(std::string const& path, float volume) {
    if (path.empty()) return;

    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;
    if (engine->m_sfxVolume <= 0.f) return;

    FMOD::System* sys = engine->m_system;
    if (!ensureChannelGroup(sys)) return;

    s_group->setVolume(engine->m_sfxVolume * std::clamp(volume, 0.f, 1.f));

    std::filesystem::path audioPath(path);
    if (!audioPath.is_absolute()) {
        std::error_code rc;
        audioPath = Mod::get()->getConfigDir() / path;
        if (!std::filesystem::exists(audioPath, rc))
            audioPath = Mod::get()->getResourcesDir() / path;
    }

    std::string const key = audioPath.string();

    FMOD::Sound* sound = nullptr;
    if (auto it = s_cache.find(key); it != s_cache.end()) {
        sound = it->second;
    } else {
        std::error_code ec;
        if (!std::filesystem::exists(audioPath, ec)) return;

        FMOD_RESULT res = sys->createSound(
            key.c_str(), FMOD_DEFAULT | FMOD_LOOP_OFF | FMOD_CREATESAMPLE,
            nullptr, &sound);
        if (res != FMOD_OK || !sound) return;

        sound->setMode(FMOD_LOOP_OFF);
        s_cache[key] = sound;
    }

    auto& channels = s_activeChannels[key];
    for (auto it = channels.begin(); it != channels.end();) {
        bool playing = false;
        if (*it && (*it)->isPlaying(&playing) == FMOD_OK && playing) {
            ++it;
        } else {
            it = channels.erase(it);
        }
    }

    while (channels.size() >= MAX_ACTIVE_VOICES) {
        if (channels.front()) channels.front()->stop();
        channels.erase(channels.begin());
    }

    FMOD::Channel* channel = nullptr;
    if (sys->playSound(sound, s_group, false, &channel) == FMOD_OK && channel) {
        channel->setMode(FMOD_LOOP_OFF);
        channel->setLoopCount(0);
        channel->setPriority(0);
        channel->setVolumeRamp(false);
        channels.push_back(channel);
    }
}

void FrameWindowSound::stopAll() {
    if (s_group) s_group->stop();
    s_activeChannels.clear();
}

void FrameWindowSound::clearCache() {
    stopAll();
    for (auto& [path, sound] : s_cache) {
        if (sound) sound->release();
    }
    s_cache.clear();
    s_activeChannels.clear();

    if (s_group) {
        s_group->release();
        s_group = nullptr;
    }
}

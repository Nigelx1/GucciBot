#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <atomic>
#include <filesystem>

using namespace geode::prelude;

namespace gucci {

    class BigBrrrManager {
    public:
        static BigBrrrManager* get();

        static double kStartOffsetSec();
        static double kBpm();

        bool enabled = false;
        bool shakeEnabled = false;
        float flickerIntensity = 0.5f;

        void setEnabled(bool on);
        std::filesystem::path getBrrrDir() const;
        void openBrrrFolder();
        bool hasFile() const;

        float getBassLevel() const {
            return rawBassLevel.load();
        }

        // For Assistant Access's get_state: whether FMOD is playing the track,
        // which file it is, and whether it holds the game's music muted.
        bool playing() const;
        std::string const& trackFile() const {
            return track;
        }
        bool holdsGameAudio() const {
            return audioMuteHeld;
        }

    private:
        std::string track;  // the file start() opened, "" when none
        FMOD::Sound* sound = nullptr;
        FMOD::Channel* channel = nullptr;
        FMOD::DSP* bassDsp = nullptr;
        std::atomic<float> rawBassLevel{0.f};
        bool audioMuteHeld = false;
        void start();
        void stop();
        static FMOD_RESULT F_CALLBACK
        bassDspCallback(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
    };

} // namespace gucci

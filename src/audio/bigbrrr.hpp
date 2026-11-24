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
        // Start at the drop (Nigel, 2026-11-23), on by default: playback starts
        // at the active theme's drop (kStartOffsetSec), as it always did, a
        // track in the BIG BRRRR folder included; off, at the song's start.
        // Saved, like Shake and Flicker.
        bool startAtDrop = true;

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
        // Where in the track it's playing, in seconds, as FMOD says (-1 when
        // nothing plays).
        double songSeconds() const;
        // Where the playing track started: the drop, or 0 (Start at the drop
        // off, a drop at 0, or a track too short to reach the drop). The
        // menu's bob counts its beats from the drop with it.
        double startedAtSec() const {
            return startedAt;
        }
        // Counts every start, so the bob starts counting again when the song
        // starts over while BIG BRRRR stays on (a new theme, a switch here).
        unsigned starts() const {
            return startCount;
        }

    private:
        std::string track;  // the file start() opened, "" when none
        double startedAt = 0.0;
        unsigned startCount = 0;
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

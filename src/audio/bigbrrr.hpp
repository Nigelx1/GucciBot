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
        // Start at the drop (Nigel, 2026-11-23), on by default: a theme's
        // bundled song starts at its drop (kStartOffsetSec), as it always did,
        // and your own song at the start time you set for it (Song::dropSec);
        // off, every song starts at its start. Saved, like Shake and Flicker.
        bool startAtDrop = true;

        // Where a song can come from, in the order start() looks: the BIG
        // BRRRR folder's first file, the active custom theme's own track, the
        // theme's bundled song (a custom theme without a track of its own
        // plays GucciBot's).
        enum class Source { None, Folder, CustomTheme, Theme };
        struct Song {
            Source source = Source::None;
            std::filesystem::path path;
            // Where it starts with Start at the drop on: a bundled song's
            // coded drop; your own song's start time (2026-11-23, Nigel: your
            // own song starts from its beginning unless you set one): the one
            // saved for the folder file's name (songStart), or a custom
            // theme's own "Drop starts at".
            double dropSec = 0.0;
        };
        // What start() would play now. It lists the BIG BRRRR folder, so the
        // menu doesn't ask every frame.
        Song pick() const;
        // True for the songs whose start time you set (the folder's, a
        // custom theme's own); a bundled song keeps its coded drop.
        static bool ownSong(Source s) {
            return s == Source::Folder || s == Source::CustomTheme;
        }
        // The start time saved for a BIG BRRRR folder file, by its file name
        // (saved key brrr_song_starts, {file name: seconds}); 0 when none is,
        // so another file, or this one renamed, starts at its start.
        static constexpr const char* kSongStartsKey = "brrr_song_starts";
        static constexpr double kMaxSongStart = 3600.0;  // as a custom theme's Drop starts at
        static double songStart(std::string const& fileName);
        // Saves it (0 takes the file's entry out). A playing song isn't
        // started over here (ui::setBrrrSongStart does that).
        static void setSongStart(std::string const& fileName, double sec);

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
        // Where the playing track started: its drop (Song::dropSec), or 0
        // (Start at the drop off, a drop at 0, or a track too short to reach
        // the drop). The menu's bob counts its beats from the drop with it.
        double startedAtSec() const {
            return startedAt;
        }
        // The playing track's drop, and where it came from (Source::None when
        // nothing plays).
        double dropAtSec() const {
            return dropAt;
        }
        Source source() const {
            return playingFrom;
        }
        // Counts every start, so the bob starts counting again when the song
        // starts over while BIG BRRRR stays on (a new theme, a switch here).
        unsigned starts() const {
            return startCount;
        }

    private:
        std::string track;  // the file start() opened, "" when none
        double startedAt = 0.0;
        double dropAt = 0.0;
        Source playingFrom = Source::None;
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

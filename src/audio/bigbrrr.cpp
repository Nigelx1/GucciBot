#include <cstdio>
#include "core/geode_compat.hpp" // gucci::pathToString
#include "audio/bigbrrr.hpp"
#include "audio/gameaudiomute.hpp"
#include "ui/ui.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using namespace geode::prelude;

namespace gucci {

    FMOD_RESULT F_CALLBACK BigBrrrManager::bassDspCallback(FMOD_DSP_STATE*,
                                                           float* inbuffer,
                                                           float* outbuffer,
                                                           unsigned int length,
                                                           int inchannels,
                                                           int*) {
        unsigned int total = length * (unsigned int)std::max(inchannels, 1);
        std::memcpy(outbuffer, inbuffer, (size_t)total * sizeof(float));

        static float lpState = 0.f;
        const float alpha = 0.06f;
        double sumSq = 0.0;
        for (unsigned int i = 0; i < total; i++) {
            lpState += alpha * (inbuffer[i] - lpState);
            sumSq += (double)lpState * (double)lpState;
        }
        float rms = total > 0 ? (float)std::sqrt(sumSq / (double)total) : 0.f;
        BigBrrrManager::get()->rawBassLevel.store(std::clamp(rms * 4.0f, 0.f, 1.f));
        return FMOD_OK;
    }

    BigBrrrManager* BigBrrrManager::get() {
        static BigBrrrManager* instance = new BigBrrrManager();
        return instance;
    }



    double BigBrrrManager::kStartOffsetSec() {
        return ui::activeThemeAudio().dropOffsetSec;
    }
    double BigBrrrManager::kBpm() {
        return ui::activeThemeAudio().bpm;
    }

    std::filesystem::path BigBrrrManager::getBrrrDir() const {
        return gucci::stdPath(Mod::get()->getSaveDir()) / "brrr";
    }

    void BigBrrrManager::openBrrrFolder() {
        auto dir = getBrrrDir();
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directories(dir);
        geode::utils::file::openFolder(dir);
    }

    static bool isPlayableAudioFile(std::filesystem::path const& p) {
        // Not path.string(): on Windows it throws for a name the ANSI code
        // page can't spell, and this runs over every file in the folder.
        auto ext = gucci::pathToString(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".mp3" || ext == ".wav" || ext == ".ogg";
    }

    static std::filesystem::path findFirstAudioFile(std::filesystem::path const& dir) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            return {};
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && isPlayableAudioFile(entry.path()))
                return entry.path();
        }
        return {};
    }

    bool BigBrrrManager::hasFile() const {
        return !findFirstAudioFile(getBrrrDir()).empty();
    }

    BigBrrrManager::Song BigBrrrManager::pick() const {
        Song song;
        // A theme with no BIG BRRRR (SolarBot, JupiterBot, GBot) has no track
        // (ui.hpp) and plays nothing, not even a file in the brrr folder,
        // which otherwise wins over every theme's.
        auto const audio = ui::activeThemeAudio();
        if (audio.file.empty())
            return song;
        if (auto folder = findFirstAudioFile(getBrrrDir()); !folder.empty()) {
            song.source = Source::Folder;
            song.dropSec = songStart(gucci::pathToString(folder.filename()));
            song.path = std::move(folder);
            return song;
        }
        std::error_code ec;
        if (!ui::activeCustomThemeExtension().empty()) {
            auto customPath = ui::customThemesDir() / (ui::activeCustomThemeExtension().substr(1) + "_brrr.mp3");
            if (std::filesystem::exists(customPath, ec)) {
                // Its own "Drop starts at" (0 for a new theme), as it always
                // started.
                song.source = Source::CustomTheme;
                song.path = std::move(customPath);
                song.dropSec = kStartOffsetSec();
                return song;
            }
        }
        // The active theme's own track, as its Themes page shows it. Every
        // bundled song has explicit lyrics (Nigel, 2026-11-22), so none
        // plays while Explicit content is off; a track the user added (the
        // folder above, a custom theme's own) still does.
        if (ui::explicitContent()) {
            std::string themeTrack = audio.file;
            if (themeTrack.empty())
                themeTrack = "big_brrr.mp3";
            auto bundled = gucci::stdPath(Mod::get()->getResourcesDir()) / themeTrack;
            if (std::filesystem::exists(bundled, ec)) {
                song.source = Source::Theme;
                song.path = std::move(bundled);
                song.dropSec = kStartOffsetSec();
            }
        }
        return song;
    }

    double BigBrrrManager::songStart(std::string const& fileName) {
        // Read where it's used, never written here: getSavedValue would put
        // an empty list into everyone's save.
        auto const& saved = Mod::get()->getSaveContainerConst();
        if (!saved.contains(kSongStartsKey))
            return 0.0;
        auto const& starts = saved[kSongStartsKey];
        if (!starts.isObject() || !starts.contains(fileName))
            return 0.0;
        double const sec = starts[fileName].asDouble().unwrapOr(0.0);
        return std::isfinite(sec) ? std::clamp(sec, 0.0, kMaxSongStart) : 0.0;
    }

    void BigBrrrManager::setSongStart(std::string const& fileName, double sec) {
        sec = std::isfinite(sec) ? std::clamp(sec, 0.0, kMaxSongStart) : 0.0;
        auto& saved = Mod::get()->getSaveContainer();
        matjson::Value starts = matjson::Value::object();
        if (saved.contains(kSongStartsKey) && std::as_const(saved)[kSongStartsKey].isObject())
            starts = std::as_const(saved)[kSongStartsKey];
        if (sec > 0.0)
            starts[fileName] = sec;
        else if (starts.contains(fileName))
            starts.erase(fileName);
        else
            return;  // 0 already: nothing to save
        saved[kSongStartsKey] = starts;
    }

    void BigBrrrManager::setEnabled(bool on) {
        // The flag first: start() switches it back off when no track can be
        // found or opened, and setting it after used to undo that, leaving
        // the switch on over silence.
        enabled = on;
        if (!on) {
            stop();
            return;
        }
        start();
        if (!channel)
            enabled = false;  // opened, but FMOD wouldn't play it
    }

    void BigBrrrManager::start() {
        stop();
        Song const song = pick();
        if (song.source == Source::None) {
            enabled = false;
            return;
        }
        auto const& path = song.path;

        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) {
            enabled = false;
            return;
        }

        // FMOD takes UTF-8 file names (path.string() is the ANSI code page).
        std::string const name = gucci::pathToString(path);
        if (system->createSound(
                name.c_str(), FMOD_CREATESAMPLE | FMOD_LOOP_NORMAL, nullptr, &sound) !=
                FMOD_OK ||
            !sound) {
            sound = nullptr;
            enabled = false;
            return;
        }
        sound->setMode(FMOD_LOOP_NORMAL);
        system->playSound(sound, nullptr, false, &channel);
        if (channel) {
            track = gucci::pathToString(path.filename());
            channel->setVolume(1.f);
            // From its drop (a bundled song's coded one, your own song's start
            // time), or with Start at the drop off from the song's start.
            // FMOD refuses a place past the track's end, and it plays from the
            // start then.
            playingFrom = song.source;
            dropAt = song.dropSec;
            startedAt = 0.0;
            if (startAtDrop && dropAt > 0.0 &&
                channel->setPosition((unsigned int)(dropAt * 1000.0), FMOD_TIMEUNIT_MS) == FMOD_OK)
                startedAt = dropAt;
            ++startCount;
            GameAudioMute::acquire();
            audioMuteHeld = true;

            FMOD_DSP_DESCRIPTION desc = {};
            std::snprintf(desc.name, sizeof(desc.name), "%s", "guccibot bass tap");
            desc.version = 0x00020000;
            desc.numinputbuffers = 1;
            desc.numoutputbuffers = 1;
            desc.read = BigBrrrManager::bassDspCallback;
            if (system->createDSP(&desc, &bassDsp) == FMOD_OK && bassDsp) {
                channel->addDSP(0, bassDsp);
            } else {
                bassDsp = nullptr;
            }
        }
    }

    void BigBrrrManager::stop() {
        if (channel) {
            if (bassDsp)
                channel->removeDSP(bassDsp);
            channel->stop();
            channel = nullptr;
        }
        if (bassDsp) {
            bassDsp->release();
            bassDsp = nullptr;
        }
        rawBassLevel.store(0.f);
        if (sound) {
            sound->release();
            sound = nullptr;
        }
        if (audioMuteHeld) {
            GameAudioMute::release();
            audioMuteHeld = false;
        }
        track.clear();
        playingFrom = Source::None;
        dropAt = 0.0;
    }

    bool BigBrrrManager::playing() const {
        bool on = false;
        return channel && channel->isPlaying(&on) == FMOD_OK && on;
    }

    double BigBrrrManager::songSeconds() const {
        unsigned int ms = 0;
        if (!channel || channel->getPosition(&ms, FMOD_TIMEUNIT_MS) != FMOD_OK)
            return -1.0;
        return ms / 1000.0;
    }

} // namespace gucci

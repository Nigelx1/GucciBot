// Juice's song on the Credits page (audio/juice_song.hpp). Written fresh for
// GucciBot (2026-11-24).

#include "audio/juice_song.hpp"

#include "audio/bigbrrr.hpp"
#include "audio/gameaudiomute.hpp"
#include "core/geode_compat.hpp"  // gucci::pathToString, gucci::stdPath

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

using namespace geode::prelude;

namespace gucci::juicesong {

    namespace {

        constexpr char kFile[] = "juice_song.ogg";

        FMOD::System* g_system = nullptr;  // the sound system the song was opened with
        FMOD::Sound* g_sound = nullptr;
        FMOD::Channel* g_channel = nullptr;
        bool g_muteHeld = false;  // holds the game's own sound quiet (GameAudioMute)
        bool g_shown = false;     // the Credits page was drawn this frame
        int g_plays = 0;          // plays this launch
        std::string g_last = "not played yet";
        // What FMOD said about the file when it last opened it.
        FMOD_SOUND_TYPE g_type = FMOD_SOUND_TYPE_UNKNOWN;
        unsigned int g_lengthMs = 0;

        FMOD::System* audioSystem() {
            auto* engine = FMODAudioEngine::sharedEngine();
            return engine ? engine->m_system : nullptr;
        }

        // GD's own Music slider (0 to 1), read every frame, so the song follows it.
        float musicVolume() {
            auto* engine = FMODAudioEngine::sharedEngine();
            return engine ? std::clamp(engine->getBackgroundMusicVolume(), 0.f, 1.f) : 0.f;
        }

        const char* typeName(FMOD_SOUND_TYPE type) {
            switch (type) {
                case FMOD_SOUND_TYPE_OGGVORBIS: return "Ogg Vorbis";
                case FMOD_SOUND_TYPE_VORBIS: return "Vorbis";
                case FMOD_SOUND_TYPE_OPUS: return "Opus";
                case FMOD_SOUND_TYPE_MPEG: return "MP3";
                case FMOD_SOUND_TYPE_WAV: return "WAV";
                case FMOD_SOUND_TYPE_FLAC: return "FLAC";
                case FMOD_SOUND_TYPE_MEDIA_FOUNDATION: return "Media Foundation";
                case FMOD_SOUND_TYPE_AUDIOQUEUE: return "AudioQueue";
                case FMOD_SOUND_TYPE_MEDIACODEC: return "MediaCodec";
                case FMOD_SOUND_TYPE_UNKNOWN: return "unknown";
                default: return "other";
            }
        }

        bool channelPlaying() {
            bool on = false;
            return g_channel && g_channel->isPlaying(&on) == FMOD_OK && on;
        }

        // Lets go of everything: the channel stops, the sound is released and
        // the game's sound comes back. If GD has made a new sound system since
        // (its old one, and everything made with it, is gone), the old handles
        // are dropped without a call into them.
        void letGo() {
            bool const sameSystem = g_system && g_system == audioSystem();
            if (g_channel && sameSystem)
                g_channel->stop();
            g_channel = nullptr;
            if (g_sound && sameSystem)
                g_sound->release();
            g_sound = nullptr;
            g_system = nullptr;
            if (std::exchange(g_muteHeld, false))
                GameAudioMute::release();
        }

        // Stops it, if anything of it is held, and says why.
        void stopBecause(const char* why) {
            if (!g_channel && !g_sound && !g_muteHeld)
                return;
            bool const was = playing();
            letGo();
            g_last = why;
            if (was)
                log::info("[juice song] stopped: {}", why);
        }

        void play() {
            letGo();
            auto* sys = audioSystem();
            if (!sys) {
                g_last = "the game's sound system isn't up";
                log::warn("[juice song] not played: {}", g_last);
                return;
            }
            // FMOD takes UTF-8 file names (path.string() is the ANSI code page).
            std::string const name = gucci::pathToString(gucci::stdPath(Mod::get()->getResourcesDir()) / kFile);
            FMOD::Sound* sound = nullptr;
            // Streamed: it opens at once, with nothing decoded up front.
            FMOD_RESULT const opened =
                sys->createSound(name.c_str(), FMOD_DEFAULT | FMOD_CREATESTREAM | FMOD_LOOP_OFF, nullptr, &sound);
            if (opened != FMOD_OK || !sound) {
                g_last = fmt::format("FMOD couldn't open it (error {})", static_cast<int>(opened));
                log::warn("[juice song] FMOD couldn't open {} (error {})", name, static_cast<int>(opened));
                return;
            }
            FMOD_SOUND_FORMAT format = FMOD_SOUND_FORMAT_NONE;
            int channels = 0, bits = 0;
            g_type = FMOD_SOUND_TYPE_UNKNOWN;
            g_lengthMs = 0;
            sound->getFormat(&g_type, &format, &channels, &bits);
            sound->getLength(&g_lengthMs, FMOD_TIMEUNIT_MS);
            // Never over BIG BRRRR: it stops first (its switch, which lasts
            // only this launch, goes off). Only once the song has opened, so
            // a song that can't play leaves BIG BRRRR playing.
            auto* brrr = BigBrrrManager::get();
            if (brrr->enabled || brrr->playing()) {
                brrr->setEnabled(false);
                log::info("[juice song] BIG BRRRR stopped first");
            }
            // Paused until its volume is set, so it never starts loud.
            FMOD::Channel* channel = nullptr;
            FMOD_RESULT const started = sys->playSound(sound, nullptr, true, &channel);
            if (started != FMOD_OK || !channel) {
                sound->release();
                g_last = fmt::format("FMOD couldn't play it (error {})", static_cast<int>(started));
                log::warn("[juice song] {}", g_last);
                return;
            }
            g_system = sys;
            g_sound = sound;
            g_channel = channel;
            float const volume = musicVolume();
            channel->setVolume(volume);
            channel->setPaused(false);
            GameAudioMute::acquire();
            g_muteHeld = true;
            ++g_plays;
            g_last = "playing";
            log::info("[juice song] playing {} ({}, {} channel(s), {} ms) at GD's music volume {:.2f}", kFile,
                      typeName(g_type), channels, g_lengthMs, volume);
        }

    } // namespace

    void toggle() {
        if (playing()) {
            stopBecause("a second click");
            return;
        }
        play();
    }

    bool playing() {
        return g_system && g_system == audioSystem() && channelPlaying();
    }

    void shown() {
        g_shown = true;
    }

    void frameEnd() {
        bool const shownNow = std::exchange(g_shown, false);
        if (!g_channel && !g_sound && !g_muteHeld)
            return;
        if (!g_system || g_system != audioSystem()) {
            letGo();
            g_last = "the game's sound system changed";
            return;
        }
        if (!shownNow) {
            stopBecause("the Credits page closed (another page, or the menu closed)");
            return;
        }
        if (BigBrrrManager::get()->playing()) {
            stopBecause("BIG BRRRR started");
            return;
        }
        if (!channelPlaying()) {
            letGo();
            g_last = "played to the end";
            log::info("[juice song] played to the end");
            return;
        }
        g_channel->setVolume(musicVolume());
    }

    matjson::Value state() {
        auto s = matjson::Value::object();
        bool const on = playing();
        s["playing"] = on;
        s["plays"] = g_plays;
        s["last"] = g_last;
        s["holds_game_audio"] = g_muteHeld;
        if (g_plays > 0) {
            s["format"] = std::string(typeName(g_type));
            s["length_ms"] = static_cast<int64_t>(g_lengthMs);
        }
        if (on) {
            unsigned int position = 0;
            float volume = 0.f;
            g_channel->getPosition(&position, FMOD_TIMEUNIT_MS);
            g_channel->getVolume(&volume);
            s["position_ms"] = static_cast<int64_t>(position);
            s["volume"] = static_cast<double>(volume);
        }
        return s;
    }

} // namespace gucci::juicesong

#include "bigbrrr.hpp"
#include "gameaudiomute.hpp"
#include "gui.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace geode::prelude;

// Live bass-tap DSP, attached directly to Big Brrr's own playback channel
// (not the master channel group) so it only ever sees this track's audio,
// never gameplay music/SFX/click sounds. Unlike render/dsp.cpp's
// AudioRecorder (which zeroes its output buffer -- fine for render capture,
// where nothing needs to be heard live), this one passes audio through
// unchanged and only reads it, since the whole point is the user actually
// hearing the bass-boosted track while the menu reacts to it.
FMOD_RESULT F_CALLBACK BigBrrrManager::bassDspCallback(FMOD_DSP_STATE*,
                                                        float* inbuffer,
                                                        float* outbuffer,
                                                        unsigned int length,
                                                        int inchannels, int*) {
    unsigned int total = length * (unsigned int)std::max(inchannels, 1);
    std::memcpy(outbuffer, inbuffer, (size_t)total * sizeof(float));

    // Cheap single-pole low-pass (not a true FFT band-split -- far lower
    // risk to get right than spectral analysis, and good enough given the
    // source tracks are already bass-boosted) then RMS of the filtered
    // signal for this buffer as "how much bass right now." alpha/gain
    // tuned by ear, not derived from the sample rate.
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

static BotTheme currentTheme() {
    auto* ui = MenuInterface::get();
    return ui ? ui->activeTheme : THEME_GUCCI;
}

// Nigel's per-theme Big Brrr tracks: each gets its own bundled file, BPM,
// and beat-drop offset -- extended from the original Maybach-only special
// case (2026-08-24, Romo/Grizzley themes, then again for user-created
// custom themes) to a real per-theme dispatch instead of stacking more
// booleans next to isMaybachTheme().
double BigBrrrManager::kStartOffsetSec() {
    auto* ui = MenuInterface::get();
    if (auto* c = ui ? ui->getActiveCustomTheme() : nullptr) return c->dropOffsetSec;
    switch (currentTheme()) {
        case THEME_MAYBACH:  return 0.0;
        case THEME_ROMO:     return 16.0 + 11.0 / 30.0;
        case THEME_GRIZZLEY: return 90.0 + 4.0 / 30.0;
        default:             return 20.0 + 11.0 / 15.0;
    }
}
double BigBrrrManager::kBpm() {
    auto* ui = MenuInterface::get();
    if (auto* c = ui ? ui->getActiveCustomTheme() : nullptr) return c->bpm;
    switch (currentTheme()) {
        case THEME_MAYBACH:  return 75.0;
        case THEME_ROMO:     return 130.0;
        case THEME_GRIZZLEY: return 98.0;
        default:             return 140.0;
    }
}

std::filesystem::path BigBrrrManager::getBrrrDir() const {
    return Mod::get()->getSaveDir() / "brrr";
}

void BigBrrrManager::openBrrrFolder() {
    auto dir = getBrrrDir();
    if (!std::filesystem::exists(dir)) std::filesystem::create_directories(dir);
    geode::utils::file::openFolder(dir);
}

static bool isPlayableAudioFile(std::filesystem::path const& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg";
}

static std::filesystem::path findFirstAudioFile(std::filesystem::path const& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && isPlayableAudioFile(entry.path())) return entry.path();
    }
    return {};
}

bool BigBrrrManager::hasFile() const {
    return !findFirstAudioFile(getBrrrDir()).empty();
}

void BigBrrrManager::setEnabled(bool on) {
    if (on) start(); else stop();
    enabled = on;
}

void BigBrrrManager::start() {
    stop();
    auto path = findFirstAudioFile(getBrrrDir());
    if (path.empty()) {
        auto* ui = MenuInterface::get();
        auto* custom = ui ? ui->getActiveCustomTheme() : nullptr;
        std::error_code ec;
        if (custom && custom->hasAudio) {
            // Custom themes' tracks aren't bundled mod resources -- they
            // were imported by the user and copied into customthemes/,
            // named by extension the same way the theme's own JSON is.
            auto customPath = ui->getCustomThemesDir() / (custom->extension + "_brrr.mp3");
            if (std::filesystem::exists(customPath, ec)) path = customPath;
        }
        if (path.empty()) {
            // No custom track imported yet (or a built-in theme active) --
            // fall back to the theme's bundled default rather than leaving
            // BIG BRRRR looking broken for a custom theme that just hasn't
            // had a track picked for it.
            const char* bundledName = "big_brrr.mp3";
            switch (currentTheme()) {
                case THEME_MAYBACH:  bundledName = "big_brrr_maybach.mp3"; break;
                case THEME_ROMO:     bundledName = "big_brrr_romo.mp3"; break;
                case THEME_GRIZZLEY: bundledName = "big_brrr_grizzley.mp3"; break;
                default: break;
            }
            auto bundled = Mod::get()->getResourcesDir() / bundledName;
            if (std::filesystem::exists(bundled, ec)) path = bundled;
        }
    }
    if (path.empty()) { enabled = false; return; }

    auto* system = FMODAudioEngine::sharedEngine()->m_system;
    if (!system) { enabled = false; return; }

    if (system->createSound(path.string().c_str(), FMOD_CREATESAMPLE | FMOD_LOOP_NORMAL, nullptr, &sound) != FMOD_OK || !sound) {
        sound = nullptr;
        enabled = false;
        return;
    }
    sound->setMode(FMOD_LOOP_NORMAL);
    system->playSound(sound, nullptr, false, &channel);
    if (channel) {
        channel->setVolume(1.f);
        channel->setPosition((unsigned int)(kStartOffsetSec() * 1000.0), FMOD_TIMEUNIT_MS);
        GameAudioMute::acquire();
        audioMuteHeld = true;

        FMOD_DSP_DESCRIPTION desc = {};
        strcpy_s(desc.name, "guccibot bass tap");
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
        if (bassDsp) channel->removeDSP(bassDsp);
        channel->stop();
        channel = nullptr;
    }
    if (bassDsp) { bassDsp->release(); bassDsp = nullptr; }
    rawBassLevel.store(0.f);
    if (sound) { sound->release(); sound = nullptr; }
    if (audioMuteHeld) { GameAudioMute::release(); audioMuteHeld = false; }
}

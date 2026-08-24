#include "bigbrrr.hpp"
#include "gameaudiomute.hpp"
#include "gui.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>

using namespace geode::prelude;

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
// case (2026-08-24, Romo/Grizzley themes) to a real per-theme dispatch
// instead of stacking more booleans next to isMaybachTheme().
double BigBrrrManager::kStartOffsetSec() {
    switch (currentTheme()) {
        case THEME_MAYBACH:  return 0.0;
        case THEME_ROMO:     return 16.0 + 11.0 / 30.0;
        case THEME_GRIZZLEY: return 90.0 + 4.0 / 30.0;
        default:             return 20.0 + 11.0 / 15.0;
    }
}
double BigBrrrManager::kBpm() {
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
        const char* bundledName = "big_brrr.mp3";
        switch (currentTheme()) {
            case THEME_MAYBACH:  bundledName = "big_brrr_maybach.mp3"; break;
            case THEME_ROMO:     bundledName = "big_brrr_romo.mp3"; break;
            case THEME_GRIZZLEY: bundledName = "big_brrr_grizzley.mp3"; break;
            default: break;
        }
        auto bundled = Mod::get()->getResourcesDir() / bundledName;
        std::error_code ec;
        if (std::filesystem::exists(bundled, ec)) path = bundled;
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
    }
}

void BigBrrrManager::stop() {
    if (channel) { channel->stop(); channel = nullptr; }
    if (sound) { sound->release(); sound = nullptr; }
    if (audioMuteHeld) { GameAudioMute::release(); audioMuteHeld = false; }
}

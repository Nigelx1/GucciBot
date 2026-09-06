#include "audio/bigbrrr.hpp"
#include "audio/gameaudiomute.hpp"
#include "gui/gui.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

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

    static BotTheme currentTheme() {
        auto* ui = MenuInterface::get();
        return ui ? ui->activeTheme : THEME_GUCCI;
    }

    double BigBrrrManager::kStartOffsetSec() {
        auto* ui = MenuInterface::get();
        if (auto* c = ui ? ui->getActiveCustomTheme() : nullptr)
            return c->dropOffsetSec;
        switch (currentTheme()) {
        case THEME_MAYBACH:
            return 0.0;
        case THEME_ROMO:
            return 16.0 + 11.0 / 30.0;
        case THEME_GRIZZLEY:
            return 90.0 + 4.0 / 30.0;
        case THEME_REDKINGDOM:
            return 14.0 + 17.0 / 30.0;
        case THEME_LEMONADE:
            return 0.0;
        case THEME_BRRR:
            return 38.0 + 19.0 / 30.0;
        case THEME_YOUNGSTA:
            return 0.0;
        case THEME_KNOCKERZ:
            return 21.0 + 17.0 / 30.0;
        case THEME_TOOSII:
        case THEME_TOOSII_SYRACUSE:
        case THEME_TOOSII_SACSTATE:
            return 33.0 + 25.0 / 30.0;
        case THEME_SEXYY:
            return 11.0 + 2.0 / 30.0;
        case THEME_SAWEETIE:
            return 11.0 + 1.0 / 30.0;
        default:
            return 20.0 + 11.0 / 15.0;
        }
    }
    double BigBrrrManager::kBpm() {
        auto* ui = MenuInterface::get();
        if (auto* c = ui ? ui->getActiveCustomTheme() : nullptr)
            return c->bpm;
        switch (currentTheme()) {
        case THEME_MAYBACH:
            return 75.0;
        case THEME_ROMO:
            return 130.0;
        case THEME_GRIZZLEY:
            return 98.0;
        case THEME_REDKINGDOM:
            return 100.0;
        case THEME_LEMONADE:
            return 142.0;
        case THEME_BRRR:
            return 150.0;
        case THEME_YOUNGSTA:
            return 144.0;
        case THEME_KNOCKERZ:
            return 90.0;
        case THEME_TOOSII:
        case THEME_TOOSII_SYRACUSE:
        case THEME_TOOSII_SACSTATE:
            return 116.0;
        case THEME_SEXYY:
            return 178.0;
        case THEME_SAWEETIE:
            return 105.0;
        default:
            return 140.0;
        }
    }

    std::filesystem::path BigBrrrManager::getBrrrDir() const {
        return Mod::get()->getSaveDir() / "brrr";
    }

    void BigBrrrManager::openBrrrFolder() {
        auto dir = getBrrrDir();
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directories(dir);
        geode::utils::file::openFolder(dir);
    }

    static bool isPlayableAudioFile(std::filesystem::path const& p) {
        auto ext = p.extension().string();
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

    void BigBrrrManager::setEnabled(bool on) {
        if (on)
            start();
        else
            stop();
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
                auto customPath = ui->getCustomThemesDir() / (custom->extension + "_brrr.mp3");
                if (std::filesystem::exists(customPath, ec))
                    path = customPath;
            }
            if (path.empty()) {
                const char* bundledName = "big_brrr.mp3";
                switch (currentTheme()) {
                case THEME_MAYBACH:
                    bundledName = "big_brrr_maybach.mp3";
                    break;
                case THEME_ROMO:
                    bundledName = "big_brrr_romo.mp3";
                    break;
                case THEME_GRIZZLEY:
                    bundledName = "big_brrr_grizzley.mp3";
                    break;
                case THEME_REDKINGDOM:
                    bundledName = "big_brrr_redkingdom.mp3";
                    break;
                case THEME_LEMONADE:
                    bundledName = "big_brrr_lemonade.mp3";
                    break;
                case THEME_BRRR:
                    bundledName = "big_brrr_brrrbot.mp3";
                    break;
                case THEME_YOUNGSTA:
                    bundledName = "big_brrr_youngsta.mp3";
                    break;
                case THEME_KNOCKERZ:
                    bundledName = "big_brrr_knockerz.mp3";
                    break;
                case THEME_TOOSII:
                case THEME_TOOSII_SYRACUSE:
                case THEME_TOOSII_SACSTATE:
                    bundledName = "big_brrr_toosii.mp3";
                    break;
                case THEME_SEXYY:
                    bundledName = "big_brrr_sexyy.mp3";
                    break;
                case THEME_SAWEETIE:
                    bundledName = "big_brrr_saweetie.mp3";
                    break;
                default:
                    break;
                }
                auto bundled = Mod::get()->getResourcesDir() / bundledName;
                if (std::filesystem::exists(bundled, ec))
                    path = bundled;
            }
        }
        if (path.empty()) {
            enabled = false;
            return;
        }

        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) {
            enabled = false;
            return;
        }

        if (system->createSound(
                path.string().c_str(), FMOD_CREATESAMPLE | FMOD_LOOP_NORMAL, nullptr, &sound) !=
                FMOD_OK ||
            !sound) {
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
    }

} // namespace gucci

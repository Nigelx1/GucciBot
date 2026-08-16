#include "trainerghost.hpp"
#include "GucciBot.hpp"
#include "gameaudiomute.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

namespace gbtr {
    bool isTrainerLevel(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!pl || !pl->m_level || !gb->trainerMacro.loaded) return false;
        if (gb->trainerMacro.levelName.empty()) return true; // see header comment
        std::string a = pl->m_level->m_levelName;
        std::string b = gb->trainerMacro.levelName;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        return a == b;
    }
}

// Parallel to JupiterMusicSync (jupiterghost.cpp) -- see trainerghost.hpp for
// why this is a separate class rather than a shared/parameterized one.
class TrainerMusicSync {
public:
    static TrainerMusicSync* get() { static TrainerMusicSync inst; return &inst; }

    void sync(PlayLayer* pl, uint32_t frame, double tps) {
        auto* gb = GucciEngine::get();
        bool shouldPlay = gb->trainerMusicEnabled && pl && pl->m_started && gbtr::isTrainerLevel(pl);
        if (!shouldPlay) return;
        if (!m_channel) start();
        if (!m_channel) return;
        m_channel->setPaused(false);

        double targetD = tps > 0.0 ? (frame / tps) * 1000.0 : 0.0;
        seekIfDrifted(targetD + gb->trainerMusicOffsetSec * 1000.0);
    }

    void syncPreview(bool active, bool paused, double posSec) {
        auto* pl = PlayLayer::get();
        auto* gb = GucciEngine::get();
        bool liveOwns = gb->trainerMusicEnabled && pl && pl->m_started && gbtr::isTrainerLevel(pl);
        if (liveOwns) return;

        bool shouldPlay = active && gb->trainerMusicEnabled;
        if (!shouldPlay) { stop(); return; }
        if (!m_channel) start();
        if (!m_channel) return;

        m_channel->setPaused(paused);
        if (paused) return;
        seekIfDrifted(posSec * 1000.0 + gb->trainerMusicOffsetSec * 1000.0);
    }

    void stop() {
        if (m_channel) { m_channel->stop(); m_channel = nullptr; }
        if (m_sound) { m_sound->release(); m_sound = nullptr; }
        if (m_audioMuteHeld) { GameAudioMute::release(); m_audioMuteHeld = false; }
    }

private:
    void seekIfDrifted(double targetMsD) {
        unsigned int targetMs = (unsigned int)std::clamp(targetMsD, 0.0, 1e9);
        unsigned int posMs = 0;
        m_channel->getPosition(&posMs, FMOD_TIMEUNIT_MS);
        long long diff = (long long)posMs - (long long)targetMs;
        if (diff < 0) diff = -diff;
        if (diff > 60) m_channel->setPosition(targetMs, FMOD_TIMEUNIT_MS);
    }

    void start() {
        // Fixed on-disk copy, not a live reference to wherever the user's
        // originally-picked file lives -- see trainerMusicImportTask in
        // gui.cpp for why (source file could move/get deleted later).
        auto path = Mod::get()->getSaveDir() / "trainer_music.mp3";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) return;
        if (system->createSound(path.string().c_str(), FMOD_CREATESAMPLE, nullptr, &m_sound) != FMOD_OK || !m_sound) {
            m_sound = nullptr;
            return;
        }
        system->playSound(m_sound, nullptr, false, &m_channel);
        if (m_channel) {
            m_channel->setVolume(1.f);
            GameAudioMute::acquire();
            m_audioMuteHeld = true;
        }
    }

    FMOD::Sound* m_sound = nullptr;
    FMOD::Channel* m_channel = nullptr;
    bool m_audioMuteHeld = false;
};

// Parallel to JupiterGhostOverlay (jupiterghost.cpp) -- see trainerghost.hpp
// for why this is a separate class. Attached at CCDrawNode z-order 1404
// (Jupiter's is 1403) so both overlays can coexist on the same object layer
// without contending for the same draw-order slot, in case a user happens
// to be on Jupiter My Favourite with a Jupiter-named macro loaded into the
// Trainer tab too -- harmless, just keeps draw order well-defined.
class TrainerGhostOverlay {
public:
    static TrainerGhostOverlay* get() { static TrainerGhostOverlay inst; return &inst; }

    void attach(PlayLayer* pl) {
        if (m_node || !pl) return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor) return;
        auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1404);
        m_node = node;
    }

    void detach() {
        if (m_node) { m_node->removeFromParent(); m_node = nullptr; }
        m_liveAttemptPath.clear();
    }

    void render(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl) return;
        if (!m_node) { attach(pl); if (!m_node) return; }
        m_node->clear();

        double tps = gb->updater.m_tps > 0.0 ? gb->updater.m_tps : 240.0;
        TrainerMusicSync::get()->sync(pl, gb->updater.getFrame(), tps);

        if (!pl->m_started) return;

        if (gb->trainerBestGhostEnabled && !gb->isPlaying() && pl->m_player1) {
            m_liveAttemptPath.push_back({ pl->m_player1->m_position.x, pl->m_player1->m_position.y });
        }

        if (gb->trainerGhostEnabled && !gb->trainerMacro.pathSamples.empty()) {
            uint32_t frame = resolveFrame(gb, gb->trainerMacro.pathSamples.size());
            if (frame < gb->trainerMacro.pathSamples.size()) {
                auto const& s = gb->trainerMacro.pathSamples[frame];
                drawGhost(s.p1x, s.p1y, ccc4f(0.30f, 0.85f, 1.f, 0.65f));
            }
        }

        if (gb->trainerBestGhostEnabled && !m_bestAttemptPath.empty()) {
            uint32_t frame = resolveFrame(gb, m_bestAttemptPath.size());
            if (frame < m_bestAttemptPath.size()) {
                auto const& p = m_bestAttemptPath[frame];
                drawGhost(p.first, p.second, ccc4f(1.f, 0.75f, 0.20f, 0.65f));
            }
        }
    }

    void onAttemptEnded() {
        float reachX = m_liveAttemptPath.empty() ? 0.f : m_liveAttemptPath.back().first;
        if (reachX > m_bestReachX) {
            m_bestReachX = reachX;
            m_bestAttemptPath = m_liveAttemptPath;
        }
        m_liveAttemptPath.clear();
    }

private:
    static uint32_t resolveFrame(GucciEngine* gb, size_t pathLen) {
        if (gb->trainerScrubActive)
            return (uint32_t)std::clamp(gb->trainerScrubPercent / 100.f * (float)pathLen, 0.f, (float)(pathLen > 0 ? pathLen - 1 : 0));
        return gb->updater.getFrame();
    }

    void drawGhost(float x, float y, ccColor4F col) {
        m_node->drawDot(CCPoint(x, y), 9.f, col);
    }

    CCDrawNode* m_node = nullptr;
    std::vector<std::pair<float,float>> m_liveAttemptPath;
    std::vector<std::pair<float,float>> m_bestAttemptPath;
    float m_bestReachX = 0.f;
};

class $modify(TrainerGhostPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        TrainerGhostOverlay::get()->attach(this);
        return true;
    }
    void onQuit() {
        TrainerGhostOverlay::get()->detach();
        TrainerMusicSync::get()->stop();
        PlayLayer::onQuit();
    }
};

namespace gbtr {
    void renderTrainerGhost(PlayLayer* pl) {
        TrainerGhostOverlay::get()->render(pl);
    }
    void notifyTrainerAttemptEnded() {
        TrainerGhostOverlay::get()->onAttemptEnded();
    }
    void syncTrainerClickBarMusic(bool active, bool paused, double posSec) {
        TrainerMusicSync::get()->syncPreview(active, paused, posSec);
    }
    void stopTrainerClickBarMusic() {
        TrainerMusicSync::get()->stop();
    }
}

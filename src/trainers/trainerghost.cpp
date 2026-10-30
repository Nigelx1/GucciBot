#include "trainers/trainerghost.hpp"
#include "core/GucciBot.hpp"
#include "audio/gameaudiomute.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

using namespace gucci;

namespace gucci::gbtr {
    bool isTrainerLevel(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!pl || !pl->m_level || !gb->trainerMacro.loaded)
            return false;
        if (gb->trainerMacro.levelName.empty())
            return true;
        std::string a = pl->m_level->m_levelName;
        std::string b = gb->trainerMacro.levelName;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        return a == b;
    }
} // namespace gbtr

class TrainerMusicSync {
public:
    static TrainerMusicSync* get() {
        static TrainerMusicSync inst;
        return &inst;
    }

    void sync(PlayLayer* pl, uint32_t frame, double tps) {
        auto* gb = GucciEngine::get();
        bool shouldPlay =
            gb->trainerMusicEnabled && pl && pl->m_started && gbtr::isTrainerLevel(pl);
        if (!shouldPlay)
            return;
        if (!m_channel)
            start();
        if (!m_channel)
            return;
        // Dead, paused or stepping: the song waits with the run instead of being
        // dragged back to the frozen frame every look (the loud repeat on a
        // death, as in the JMF Trainer's player).
        bool const frozen = pl->m_playerDied || (pl->m_player1 && pl->m_player1->m_isDead) || pl->m_isPaused ||
                            GucciEngine::get()->updater.m_paused;
        m_channel->setPaused(frozen);
        if (frozen)
            return;

        double targetD = tps > 0.0 ? (frame / tps) * 1000.0 : 0.0;
        seekIfDrifted(targetD + gb->trainerMusicOffsetSec * 1000.0);
    }

    void syncPreview(bool active, bool paused, double posSec) {
        auto* pl = PlayLayer::get();
        auto* gb = GucciEngine::get();
        bool liveOwns = gb->trainerMusicEnabled && pl && pl->m_started && gbtr::isTrainerLevel(pl);
        if (liveOwns)
            return;

        bool shouldPlay = active && gb->trainerMusicEnabled;
        if (!shouldPlay) {
            stop();
            return;
        }
        if (!m_channel)
            start();
        if (!m_channel)
            return;

        m_channel->setPaused(paused);
        if (paused)
            return;
        seekIfDrifted(posSec * 1000.0 + gb->trainerMusicOffsetSec * 1000.0);
    }

    void stop() {
        if (m_channel) {
            m_channel->stop();
            m_channel = nullptr;
        }
        if (m_sound) {
            m_sound->release();
            m_sound = nullptr;
        }
        if (m_audioMuteHeld) {
            GameAudioMute::release();
            m_audioMuteHeld = false;
        }
    }

private:
    void seekIfDrifted(double targetMsD) {
        unsigned int targetMs = (unsigned int)std::clamp(targetMsD, 0.0, 1e9);
        unsigned int posMs = 0;
        m_channel->getPosition(&posMs, FMOD_TIMEUNIT_MS);
        long long diff = (long long)posMs - (long long)targetMs;
        if (diff < 0)
            diff = -diff;
        if (diff > 60)
            m_channel->setPosition(targetMs, FMOD_TIMEUNIT_MS);
    }

    void start() {
        auto path = Mod::get()->getSaveDir() / "trainer_music.mp3";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system)
            return;
        if (system->createSound(path.string().c_str(), FMOD_CREATESAMPLE, nullptr, &m_sound) !=
                FMOD_OK ||
            !m_sound) {
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

class TrainerGhostOverlay {
public:
    static TrainerGhostOverlay* get() {
        static TrainerGhostOverlay inst;
        return &inst;
    }

    void attach(PlayLayer* pl) {
        if (m_node || !pl)
            return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor)
            return;
        auto* node = CCDrawNode::create();
        node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        node->m_bUseArea = false;
        anchor->addChild(node, 1404);
        m_node = node;
    }

    void detach() {
        if (m_node) {
            m_node->removeFromParent();
            m_node = nullptr;
        }
        m_liveAttemptPath.clear();
    }

    void render(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl)
            return;
        if (!m_node) {
            attach(pl);
            if (!m_node)
                return;
        }
        m_node->clear();

        double tps = gb->updater.m_tps > 0.0 ? gb->updater.m_tps : 240.0;
        TrainerMusicSync::get()->sync(pl, gb->updater.getFrame(), tps);

        if (!pl->m_started)
            return;
        // The ghosts belong to the trainer's own level, as its music does.
        if (!gbtr::isTrainerLevel(pl)) {
            m_liveAttemptPath.clear();
            return;
        }

        // Your attempt, for the best attempt's ghost: alive only (the wait
        // after a death used to run on into the next attempt and put its
        // ghost behind), and by frame, so it lines up with the macro's.
        auto* p1 = pl->m_player1;
        if (gb->trainerBestGhostEnabled && !gb->isPlaying() && p1 && !pl->m_playerDied && !p1->m_isDead)
            recordLive(gb->updater.getFrame(), {p1->m_position.x, p1->m_position.y});

        // One frame for both ghosts. Scrub reads it off the macro's path (your
        // best's, when the macro has none), so the two show the same moment.
        auto const& macroPath = gb->trainerMacro.pathSamples;
        uint32_t const frame = resolveFrame(gb, !macroPath.empty() ? macroPath.size() : m_bestAttemptPath.size());

        if (gb->trainerGhostEnabled && !macroPath.empty()) {
            if (frame < macroPath.size()) {
                auto const& s = macroPath[frame];
                drawGhost(s.p1x, s.p1y, ccc4f(0.30f, 0.85f, 1.f, 0.65f));
            }
        }

        if (gb->trainerBestGhostEnabled && !m_bestAttemptPath.empty()) {
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

    // Reset stats, or another macro: the best attempt so far no longer counts.
    void resetBest() {
        m_bestAttemptPath.clear();
        m_bestReachX = 0.f;
    }

private:
    // About 72 minutes at 240 TPS; past it the last place is kept.
    static constexpr size_t kMaxPathFrames = size_t(1) << 20;

    // The player's place at this frame of the live attempt. Frames are
    // absolute: an attempt from a checkpoint starts part way in, and the
    // frames before are filled with where it began, as is a gap of several
    // ticks in one update (Lock delta off). A restart or a step back cuts
    // the path back to the frame.
    void recordLive(uint32_t frame, std::pair<float, float> pos) {
        size_t const f = std::min<size_t>(frame, kMaxPathFrames);
        if (m_liveAttemptPath.size() > f)
            m_liveAttemptPath.resize(f);
        while (m_liveAttemptPath.size() < f)
            m_liveAttemptPath.push_back(m_liveAttemptPath.empty() ? pos : m_liveAttemptPath.back());
        m_liveAttemptPath.push_back(pos);
    }

    static uint32_t resolveFrame(GucciEngine* gb, size_t pathLen) {
        if (gb->trainerScrubActive)
            return (uint32_t)std::clamp(gb->trainerScrubPercent / 100.f * (float)pathLen,
                                        0.f,
                                        (float)(pathLen > 0 ? pathLen - 1 : 0));
        return gb->updater.getFrame();
    }

    void drawGhost(float x, float y, ccColor4F col) {
        m_node->drawDot(CCPoint(x, y), 9.f, col);
    }

    CCDrawNode* m_node = nullptr;
    std::vector<std::pair<float, float>> m_liveAttemptPath;
    std::vector<std::pair<float, float>> m_bestAttemptPath;
    float m_bestReachX = 0.f;
};

class $modify(TrainerGhostPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        TrainerGhostOverlay::get()->attach(this);
        return true;
    }
    void onQuit() {
        TrainerGhostOverlay::get()->detach();
        TrainerMusicSync::get()->stop();
        PlayLayer::onQuit();
    }
};

namespace gucci::gbtr {
    void renderTrainerGhost(PlayLayer* pl) {
        TrainerGhostOverlay::get()->render(pl);
    }
    void notifyTrainerAttemptEnded() {
        TrainerGhostOverlay::get()->onAttemptEnded();
    }
    void resetBestGhost() {
        TrainerGhostOverlay::get()->resetBest();
    }
    void syncTrainerClickBarMusic(bool active, bool paused, double posSec) {
        TrainerMusicSync::get()->syncPreview(active, paused, posSec);
    }
    void stopTrainerClickBarMusic() {
        TrainerMusicSync::get()->stop();
    }
} // namespace gbtr

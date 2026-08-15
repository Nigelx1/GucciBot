#include "jupiterghost.hpp"
#include "GucciBot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

// Synced level music: plays resources/jupiter_music.mp3 while actually on
// Jupiter My Favourite, seeked to match the current frame ((frame/tps)*1000ms
// = song position, no offset) rather than just played once from the start.
// Only corrects drift past a small tolerance instead of seeking every single
// frame, so normal playback stays smooth and only actually jumps right after
// a respawn/restart where the frame legitimately jumped too.
class JupiterMusicSync {
public:
    static JupiterMusicSync* get() { static JupiterMusicSync inst; return &inst; }

    void sync(PlayLayer* pl, uint32_t frame, double tps) {
        auto* gb = GucciEngine::get();
        bool shouldPlay = gb->jupiterMusicEnabled && pl && pl->m_started && isJupiterLevel(pl);
        if (!shouldPlay) { stop(); return; }
        if (!m_channel) start();
        if (!m_channel) return;

        double targetD = tps > 0.0 ? (frame / tps) * 1000.0 : 0.0;
        unsigned int targetMs = (unsigned int)std::clamp(targetD, 0.0, 1e9);
        unsigned int posMs = 0;
        m_channel->getPosition(&posMs, FMOD_TIMEUNIT_MS);
        long long diff = (long long)posMs - (long long)targetMs;
        if (diff < 0) diff = -diff;
        if (diff > 60) m_channel->setPosition(targetMs, FMOD_TIMEUNIT_MS);
    }

    void stop() {
        if (m_channel) { m_channel->stop(); m_channel = nullptr; }
        if (m_sound) { m_sound->release(); m_sound = nullptr; }
    }

private:
    static bool isJupiterLevel(PlayLayer* pl) {
        if (!pl->m_level) return false;
        std::string lower = pl->m_level->m_levelName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower.find("jupiter my favourite") != std::string::npos;
    }

    void start() {
        auto path = Mod::get()->getResourcesDir() / "jupiter_music.mp3";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) return;
        if (system->createSound(path.string().c_str(), FMOD_CREATESAMPLE, nullptr, &m_sound) != FMOD_OK || !m_sound) {
            m_sound = nullptr;
            return;
        }
        system->playSound(m_sound, nullptr, false, &m_channel);
        if (m_channel) m_channel->setVolume(1.f);
    }

    FMOD::Sound* m_sound = nullptr;
    FMOD::Channel* m_channel = nullptr;
};

// Mirrors PracticeRangeOverlay (practicerange.cpp) exactly on purpose: a
// CCDrawNode attached to m_objectLayer, cleared and redrawn every frame,
// attached on PlayLayer::init and detached on PlayLayer::onQuit. That
// pattern is already proven safe in this codebase for per-frame gameplay
// overlays -- reusing it here means this feature can't touch anything it
// isn't explicitly reading (m_pathSamples, a locally-tracked best-attempt
// path), unlike the checkpoint/reset system flagged elsewhere as fragile.
class JupiterGhostOverlay {
public:
    static JupiterGhostOverlay* get() { static JupiterGhostOverlay inst; return &inst; }

    void attach(PlayLayer* pl) {
        if (m_node || !pl) return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor) return;
        auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1403);
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
        JupiterMusicSync::get()->sync(pl, gb->updater.getFrame(), tps);

        if (!pl->m_started) return;

        // Capture this attempt's live path for the "own best attempt" ghost.
        // Only during real manual play -- bot playback is just replaying the
        // same macro we're already comparing against, so capturing it would
        // be redundant at best and would corrupt "your own best" at worst.
        if (gb->jupiterBestGhostEnabled && !gb->isPlaying() && pl->m_player1) {
            m_liveAttemptPath.push_back({ pl->m_player1->m_position.x, pl->m_player1->m_position.y });
        }

        if (gb->jupiterGhostEnabled && !gb->jupiterMacro.pathSamples.empty()) {
            uint32_t frame = resolveFrame(gb, gb->jupiterMacro.pathSamples.size());
            if (frame < gb->jupiterMacro.pathSamples.size()) {
                auto const& s = gb->jupiterMacro.pathSamples[frame];
                drawGhost(s.p1x, s.p1y, ccc4f(0.30f, 0.85f, 1.f, 0.65f));
            }
        }

        if (gb->jupiterBestGhostEnabled && !m_bestAttemptPath.empty()) {
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
        if (gb->jupiterScrubActive)
            return (uint32_t)std::clamp(gb->jupiterScrubPercent / 100.f * (float)pathLen, 0.f, (float)(pathLen > 0 ? pathLen - 1 : 0));
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

class $modify(JupiterGhostPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        JupiterGhostOverlay::get()->attach(this);
        return true;
    }
    void onQuit() {
        JupiterGhostOverlay::get()->detach();
        JupiterMusicSync::get()->stop();
        PlayLayer::onQuit();
    }
};

namespace gbju {
    void renderJupiterGhost(PlayLayer* pl) {
        JupiterGhostOverlay::get()->render(pl);
    }
    void notifyJupiterAttemptEnded() {
        JupiterGhostOverlay::get()->onAttemptEnded();
    }
}

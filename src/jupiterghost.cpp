#include "jupiterghost.hpp"
#include "GucciBot.hpp"
#include "gui.hpp"
#include "gameaudiomute.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

// Synced music, two independent trigger paths sharing one channel:
//  - sync(pl,frame,tps): live gameplay, called from the per-gameplay-frame
//    hook (renderJupiterGhost) -- seeked to the ACTUAL level frame, works
//    even with the menu closed.
//  - syncPreview(...): click-bar-preview, called directly from
//    MenuInterface::drawJupiterClickTrainerPage every frame that page is
//    open -- seeked to the click bar's own transport position, works from
//    the main menu with no level loaded at all.
// Live gameplay always takes priority if both are momentarily true (e.g.
// the menu's open to the Click Trainer page WHILE actually playing Jupiter
// My Favourite) -- syncPreview defers to sync() in that case rather than
// fighting over the channel's seek position.
//
// Neither path unconditionally stop()s the channel just because ITS OWN
// condition isn't met, since the OTHER path might legitimately still want
// it running -- only syncPreview's "nothing wants this at all" branch calls
// stop(), since it's reachable independent of whether a PlayLayer exists
// (sync() only runs inside an active level to begin with).
class JupiterMusicSync {
public:
    static JupiterMusicSync* get() { static JupiterMusicSync inst; return &inst; }

    void sync(PlayLayer* pl, uint32_t frame, double tps) {
        auto* gb = GucciEngine::get();
        bool shouldPlay = gb->jupiterMusicEnabled && pl && pl->m_started && isJupiterLevel(pl);
        if (!shouldPlay) return;
        if (!m_channel) start();
        if (!m_channel) return;
        m_channel->setPaused(false);

        double targetD = tps > 0.0 ? (frame / tps) * 1000.0 : 0.0;
        seekIfDrifted(targetD);
    }

    void syncPreview(bool active, bool paused, double posSec) {
        auto* pl = PlayLayer::get();
        auto* gb = GucciEngine::get();
        bool liveOwns = gb->jupiterMusicEnabled && pl && pl->m_started && isJupiterLevel(pl);
        if (liveOwns) return; // sync() has it this frame instead

        bool shouldPlay = active && gb->jupiterMusicEnabled;
        if (!shouldPlay) { stop(); return; }
        if (!m_channel) start();
        if (!m_channel) return;

        m_channel->setPaused(paused);
        if (paused) return;
        seekIfDrifted(posSec * 1000.0);
    }

    void stop() {
        if (m_channel) { m_channel->stop(); m_channel = nullptr; }
        if (m_sound) { m_sound->release(); m_sound = nullptr; }
        if (m_audioMuteHeld) { GameAudioMute::release(); m_audioMuteHeld = false; }
    }

private:
    static bool isJupiterLevel(PlayLayer* pl) {
        if (!pl->m_level) return false;
        std::string lower = pl->m_level->m_levelName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower.find("jupiter my favourite") != std::string::npos;
    }

    void seekIfDrifted(double targetMsD) {
        unsigned int targetMs = (unsigned int)std::clamp(targetMsD, 0.0, 1e9);
        unsigned int posMs = 0;
        m_channel->getPosition(&posMs, FMOD_TIMEUNIT_MS);
        long long diff = (long long)posMs - (long long)targetMs;
        if (diff < 0) diff = -diff;
        if (diff > 60) m_channel->setPosition(targetMs, FMOD_TIMEUNIT_MS);
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

// Click Trainer's own click/release marks (spacebar, up arrow, W): a real
// CCKeyboardDispatcher hook, not ImGui::IsKeyPressed -- game keys aren't
// reliably delivered to ImGui's IO in this GD+ImGui integration, which is
// exactly why keybinds.cpp already has its own separate dispatcher hook
// instead of relying on ImGui for these. Deliberately NOT gated on
// PlayLayer::get() (unlike keybinds.cpp's Autoclicker tracking) since the
// click bar is meant to work without a level loaded; gated on the Click
// Trainer page actually being open instead, so ordinary jumping during real
// gameplay doesn't silently pile up into the click bar's history.
class $modify(JupiterClickBarKeyHandler, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        if (!repeat && (key == enumKeyCodes::KEY_Space || key == enumKeyCodes::KEY_Up || key == enumKeyCodes::KEY_W)) {
            auto* ui = MenuInterface::get();
            if (ui && ui->jupiterClickBarPageOpen) {
                auto* gb = GucciEngine::get();
                if (down) gb->jupiterClickBarMyClicks.push_back(gb->jupiterClickBarPosSec);
                else gb->jupiterClickBarMyReleases.push_back(gb->jupiterClickBarPosSec);
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

namespace gbju {
    void renderJupiterGhost(PlayLayer* pl) {
        JupiterGhostOverlay::get()->render(pl);
    }
    void notifyJupiterAttemptEnded() {
        JupiterGhostOverlay::get()->onAttemptEnded();
    }
    void syncClickBarMusic(bool active, bool paused, double posSec) {
        JupiterMusicSync::get()->syncPreview(active, paused, posSec);
    }
    void stopClickBarMusic() {
        JupiterMusicSync::get()->stop();
    }
}

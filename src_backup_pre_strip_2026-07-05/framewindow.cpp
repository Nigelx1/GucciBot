// framewindow.cpp — GucciBot 10.0
// Frame-window tracker: DISPLAY layer.
//
// This file owns the *visualization* of frame-window data — circle markers
// pinned to world positions that scroll with the level, like the reference.
// The heavy ±frame analysis that fills in the real window numbers lives
// separately (see analyzeFrameWindows, added in a later pass); this layer just
// renders whatever marks are currently in GucciEngine::fwMarks.
//
// World-pinning works exactly like the hitbox overlay: a CCDrawNode added as a
// child of the object layer's parent inherits the scroll transform, so anything
// drawn at a world coordinate sticks to that spot in the level automatically.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "GucciBot.hpp"

using namespace geode::prelude;

// Mirror portals flip the object layer horizontally — a negative scaleX somewhere
// in the object-layer chain. Our markers are children of that layer, so their
// content (number labels, tier sprites) renders BACKWARDS while the level is
// mirrored. Walk the parent chain and report whether the cumulative X scale is
// negative, so each marker can counter-flip its own content to stay upright.
// (Positions stay correct: a marker rides the flip along with the level, so it
// stays pinned to the right spot — only the glyph orientation needs fixing.)
static bool parentChainFlipped(cocos2d::CCNode* n) {
    float sx = 1.f;
    for (auto* p = n; p; p = p->getParent()) sx *= p->getScaleX();
    return sx < 0.f;
}

class FrameWindowOverlay {
public:
    static FrameWindowOverlay* get() {
        static FrameWindowOverlay inst;
        return &inst;
    }

    void attach(PlayLayer* pl) {
        if (m_node || !pl) return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor) return;

        // Marker draw node — scrolls with the level (child of object layer).
        auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1403);
        m_node = node;

        // Number labels live on a sibling batch so they scroll too.
        auto* labels = CCNode::create();
        anchor->addChild(labels, 1404);
        m_labelLayer = labels;
    }

    void detach() {
        if (m_node) { m_node->removeFromParent(); m_node = nullptr; }
        if (m_labelLayer) { m_labelLayer->removeFromParent(); m_labelLayer = nullptr; }
        m_builtForCount = -1;
    }

    // Called every frame from the PlayLayer update hook.
    void render(PlayLayer* pl, bool isRendering) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl) return;
        // v10.1: lazily attach if init-time attach didn't take (m_objectLayer
        // can be null during init depending on dontCreateObjects). Without this
        // the overlay never appeared even when analysis produced valid marks.
        if (!m_node) {
            attach(pl);
            if (!m_node) return;  // object layer still not ready; try next frame
        }

        bool show = isRendering ? gb->fwEnabledRender : gb->fwEnabledLive;
        if (!show || !gb->fwHasData) {
            if (m_builtForCount != 0) { clear(); m_builtForCount = 0; }
            return;
        }

        // v8.13: markers appear progressively — a click's marker is only drawn
        // once playback has reached its frame, then it stays world-pinned and
        // scrolls off naturally. So we rebuild as the current frame advances
        // past each click (plus when the data set or threshold changes).
        uint32_t curFrame = gb->updater.getFrame();
        // How many marks are visible at this frame?
        int visibleCount = 0;
        for (auto const& mk : gb->fwMarks)
            if (mk.frame <= curFrame && mk.window <= gb->fwMaxWindow) ++visibleCount;

        // Mirror portals flip the object layer; our markers inherit that flip, so
        // each marker counter-flips its content to keep number text/sprites upright.
        // Include the flip state in the rebuild signature so crossing a portal
        // (which flips the whole layer at once) triggers a rebuild w/ the right flip.
        bool mirrored = m_labelLayer && parentChainFlipped(m_labelLayer);

        int sig = visibleCount * 100000
                + static_cast<int>(gb->fwMarks.size()) * 100
                + gb->fwMaxWindow
                + (mirrored ? 1 : 0);
        if (sig == m_builtForCount) return;
        m_builtForCount = sig;

        // v10.1 DIAGNOSTIC: log when the visible set changes so it's clear
        // whether the overlay attached and how many marks pass the threshold.
        // If totalMarks > 0 but visible == 0, the Max Window slider is hiding
        // them (all clicks looser than the threshold) — not a render failure.
        {
            int loosest = 0;
            for (auto const& mk : gb->fwMarks) if (mk.window > loosest) loosest = mk.window;
            log::info("[GucciBot] Frame-window overlay: node={}, {} marks total, "
                      "{} visible at threshold <= {} (loosest measured = {})",
                      m_node ? "attached" : "NULL",
                      gb->fwMarks.size(), visibleCount, gb->fwMaxWindow, loosest);
        }

        clear();

        for (auto const& mk : gb->fwMarks) {
            if (mk.window > gb->fwMaxWindow) continue;  // tight clicks only
            if (mk.frame > curFrame) continue;          // v8.13: not reached yet

            CCPoint at{ mk.x, mk.y };

            // v8.11: a matching tier overrides color and can supply a PNG marker.
            auto* tier = gb->fwTierFor(mk.window);
            ccColor4F col = tier
                ? ccColor4F{ tier->r, tier->g, tier->b, 1.f }
                : gradeColor(mk.window, gb->fwMaxWindow);

            bool drewSprite = false;
            if (tier && tier->imageFile[0]) {
                auto path = Mod::get()->getSaveDir() / "fw_assets" / tier->imageFile;
                std::error_code ec;
                if (std::filesystem::exists(path, ec)) {
                    if (auto* spr = CCSprite::create(path.string().c_str())) {
                        spr->setPosition(at);
                        float maxDim = std::max(spr->getContentSize().width,
                                                spr->getContentSize().height);
                        if (maxDim > 0.f) spr->setScale((kRadius * 2.2f) / maxDim);
                        spr->setColor({ (GLubyte)(col.r*255),(GLubyte)(col.g*255),(GLubyte)(col.b*255) });
                        if (mirrored) spr->setScaleX(-spr->getScaleX());  // counter-flip tier sprite
                        m_labelLayer->addChild(spr);
                        drewSprite = true;
                    }
                }
            }
            if (!drewSprite) drawRing(at, kRadius, col);  // fallback: drawn ring

            // Number beside/above the marker (not inside).
            auto* lbl = CCLabelBMFont::create(
                std::to_string(mk.window).c_str(), "bigFont.fnt");
            lbl->setScale(0.45f);
            if (mirrored) lbl->setScaleX(-0.45f);  // counter-flip number text so it reads correctly
            lbl->setPosition({ at.x, at.y + kRadius + 11.f });
            lbl->setColor({ (GLubyte)(col.r * 255), (GLubyte)(col.g * 255), (GLubyte)(col.b * 255) });
            lbl->setOpacity(255);
            m_labelLayer->addChild(lbl);
        }
    }

private:
    static constexpr float kRadius = 11.f;  // Juice: rings were a bit too big (was 14)

    // window == maxWindow  -> green (loose)
    // window == 1          -> red   (tight)
    // proportional to the slider, as requested.
    ccColor4F gradeColor(int window, int maxWindow) const {
        float span = std::max(1, maxWindow - 1);
        float t = std::clamp(static_cast<float>(window - 1) / span, 0.f, 1.f);
        // t=0 (tight) red -> t=1 (loose) green
        return ccColor4F{ 1.f - t, t, 0.15f, 1.f };
    }

    void drawRing(CCPoint center, float radius, ccColor4F color) {
        const int segs = 28;
        const float thickness = 2.2f;
        ccColor4F clear4{ 0, 0, 0, 0 };
        // Approximate a ring by drawing a thin-bordered circle.
        m_node->drawCircle(center, radius, clear4, thickness, color, segs);
    }

    void clear() {
        if (m_node) m_node->clear();
        if (m_labelLayer) m_labelLayer->removeAllChildren();
    }

    CCDrawNode* m_node = nullptr;
    CCNode*     m_labelLayer = nullptr;
    int         m_builtForCount = -1;
};

// Hook lifecycle into PlayLayer so the overlay attaches/detaches cleanly.
class $modify(FrameWindowPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        FrameWindowOverlay::get()->attach(this);
        return true;
    }

    void onQuit() {
        FrameWindowOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};

// Public entry the update hook calls each frame (defined here, declared in
// GucciBot.hpp via the renderer-adjacent overlay calls).
namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRendering) {
        FrameWindowOverlay::get()->render(pl, isRendering);
    }

    // v8.11/8.13: play a tier's sound for a click of the given window size.
    // Resolution order: explicit "none" -> silent; a custom file -> play it;
    // otherwise -> the bundled default (fw_default.mp3). Called per click.
    //
    // v10: stop-and-restart, never overlap. A single persistent channel is
    // reused: if it's still playing from the previous click (high-CPS section),
    // it's stopped first so the new hit restarts the sound cleanly instead of
    // stacking dozens of overlapping copies. Sounds are cached so the file is
    // only decoded once.
    void playTierSound(int window) {
        static std::unordered_map<std::string, FMOD::Sound*> s_soundCache;
        static FMOD::Channel* s_channel = nullptr;
        static FMOD::Sound*   s_currentSound = nullptr;  // sound currently loaded on s_channel

        auto* gb = GucciEngine::get();
        auto* tier = gb->fwTierFor(window);

        std::filesystem::path path;
        if (tier && std::string(tier->soundFile) == "none") {
            return;  // explicitly silent
        } else if (tier && tier->soundFile[0]) {
            path = Mod::get()->getSaveDir() / "fw_assets" / tier->soundFile;
        } else {
            path = Mod::get()->getResourcesDir() / "fw_default.mp3";
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) return;

        // Cached sound (decode once).
        std::string key = path.string();
        FMOD::Sound* sound = nullptr;
        auto it = s_soundCache.find(key);
        if (it != s_soundCache.end()) {
            sound = it->second;
        } else {
            if (system->createSound(key.c_str(), FMOD_CREATESAMPLE, nullptr, &sound) != FMOD_OK || !sound)
                return;
            s_soundCache[key] = sound;
        }

        // RESTART on each click ("play on click, restart on next click"). The
        // sound file is a hit + several seconds of silence, so stop+play on every
        // click churns FMOD voices and — in dense sections — leaves transient
        // stopping-but-not-freed voices that stack up and starve out later clicks
        // (sounds right on click 1, then "random"/missed after). Retrigger the
        // SAME sound by seeking the live channel back to 0 (setPosition: atomic,
        // no new voice, nothing to stack); only stop+play when the tier sound
        // actually changes.
        bool playing = false;
        if (s_channel) s_channel->isPlaying(&playing);
        if (s_channel && playing && s_currentSound == sound) {
            s_channel->setPosition(0, FMOD_TIMEUNIT_MS);  // restart same instance in place
            return;
        }
        if (s_channel) s_channel->stop();
        system->playSound(sound, nullptr, false, &s_channel);
        s_currentSound = sound;
    }
}

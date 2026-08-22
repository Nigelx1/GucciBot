#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "GucciBot.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>

using namespace geode::prelude;

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

                auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1403);
        m_node = node;

                auto* labels = CCNode::create();
        anchor->addChild(labels, 1404);
        m_labelLayer = labels;
    }

    void detach() {
        if (m_node) { m_node->removeFromParent(); m_node = nullptr; }
        if (m_labelLayer) { m_labelLayer->removeFromParent(); m_labelLayer = nullptr; }
        if (m_debugNode) { m_debugNode->removeFromParent(); m_debugNode = nullptr; }
        m_builtForCount = -1;
    }

    // Juice's debug/slow mode: draws a mark at wherever the player ended up for
    // every individual test Calculate has run so far on the CURRENT click (green
    // ring = survived, red X = died) -- separate draw node from m_node/m_labelLayer
    // above since these change every single test and shouldn't be gated by that
    // node's own "only rebuild when something changed" signature check. Live-only,
    // never drawn into an actual render.
    void renderDebugMarks(PlayLayer* pl, GucciEngine* gb, bool isRendering) {
        if (!m_debugNode) {
            auto* anchor = pl->m_objectLayer;
            if (!anchor) return;
            auto* node = CCDrawNode::create();
            node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
            node->m_bUseArea = false;
            anchor->addChild(node, 1405);
            m_debugNode = node;
        }
        m_debugNode->clear();
        if (isRendering || !gb->fwAnalyzing || !gb->fwDebugMode) return;

        for (auto const& mk : gb->fwDebugMarks) {
            CCPoint at{ mk.x, mk.y };
            if (mk.survived) {
                ccColor4F col{ 0.25f, 1.f, 0.35f, 1.f };
                ccColor4F clear4{ 0.f, 0.f, 0.f, 0.f };
                m_debugNode->drawCircle(at, 6.f, clear4, 2.5f, col, 16);
            } else {
                ccColor4F col{ 1.f, 0.2f, 0.2f, 1.f };
                float s = 6.f;
                m_debugNode->drawSegment({ at.x - s, at.y - s }, { at.x + s, at.y + s }, 2.5f, col);
                m_debugNode->drawSegment({ at.x - s, at.y + s }, { at.x + s, at.y - s }, 2.5f, col);
            }
        }
    }

        void render(PlayLayer* pl, bool isRendering) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl) return;
                                if (!m_node) {
            attach(pl);
            if (!m_node) return;
        }

        renderDebugMarks(pl, gb, isRendering);

        bool show = isRendering ? gb->fwEnabledRender : gb->fwEnabledLive;
        if (!show || !gb->fwHasData) {
            if (m_builtForCount != 0) { clear(); m_builtForCount = 0; }
            return;
        }

                                        uint32_t curFrame = gb->updater.getFrame();
                int visibleCount = 0;
        for (auto const& mk : gb->fwMarks)
            if (mk.frame <= curFrame && mk.window <= gb->fwMaxWindow) ++visibleCount;

                                        bool mirrored = m_labelLayer && parentChainFlipped(m_labelLayer);

                        // Pulse effects need to redraw every frame to animate -- the
        // signature cache below would otherwise freeze them on their first frame.
        bool anyPulseActive = false;
        for (auto const& mk : gb->fwMarks) {
            if (mk.frame > curFrame || mk.window > gb->fwMaxWindow) continue;
            auto* t = gb->fwTierFor(mk.window);
            if (t && (t->markerPulseEnabled || t->textPulseEnabled)) { anyPulseActive = true; break; }
        }

        int sig = visibleCount * 100000
                + static_cast<int>(gb->fwMarks.size()) * 100
                + gb->fwMaxWindow
                + (mirrored ? 1 : 0);
        if (!anyPulseActive && sig == m_builtForCount) return;
        m_builtForCount = anyPulseActive ? -1 : sig;

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
            if (mk.window > gb->fwMaxWindow) continue;
            if (mk.frame > curFrame) continue;

                        auto* tier = gb->fwTierFor(mk.window);
            // Juice: if tiers are actually configured, a window that doesn't fall
            // into ANY of them shouldn't get a marker at all -- e.g. tiers for 1-4
            // and 5-6 only, a 25-frame window should be invisible, not shown with
            // fallback coloring just because it's under fwMaxWindow. An empty tier
            // list (nothing configured yet) keeps the old show-everything behavior.
            if (!gb->fwTiers.empty() && !tier) continue;

            CCPoint at{ mk.x, mk.y };

            ccColor4F col = tier
                ? ccColor4F{ tier->r, tier->g, tier->b, 1.f }
                : gradeColor(mk.window, gb->fwMaxWindow);

            // Pulse effects (Juice's spec): marker and text pulse independently,
            // each fading from the tier's normal color to its own pulse color,
            // holding, fading back, and repeating on a real-time (not game-tick)
            // clock -- see applyPulse().
            ccColor4F markerCol = col;
            ccColor4F textCol   = col;
            if (tier && tier->markerPulseEnabled)
                markerCol = applyPulse(col, tier->markerPulseColor,
                    tier->markerPulseFadeIn, tier->markerPulseHold, tier->markerPulseFadeOut);
            if (tier && tier->textPulseEnabled)
                textCol = applyPulse(col, tier->textPulseColor,
                    tier->textPulseFadeIn, tier->textPulseHold, tier->textPulseFadeOut);

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
                        spr->setColor({ (GLubyte)(markerCol.r*255),(GLubyte)(markerCol.g*255),(GLubyte)(markerCol.b*255) });
                        if (mirrored) spr->setScaleX(-spr->getScaleX());
                        m_labelLayer->addChild(spr);
                        drewSprite = true;
                    }
                }
            }
            if (!drewSprite) drawMarkerShape(at, kRadius, markerCol, tier);

                        auto* lbl = CCLabelBMFont::create(
                std::to_string(mk.window).c_str(), "bigFont.fnt");
            lbl->setScale(0.45f);
            if (mirrored) lbl->setScaleX(-0.45f);
            lbl->setPosition({ at.x, at.y + kRadius + 11.f });
            lbl->setColor({ (GLubyte)(textCol.r * 255), (GLubyte)(textCol.g * 255), (GLubyte)(textCol.b * 255) });
            lbl->setOpacity(255);
            m_labelLayer->addChild(lbl);
        }
    }

private:
    static constexpr float kRadius = 11.f;

                static float nowSeconds() {
        using namespace std::chrono;
        static const auto start = steady_clock::now();
        return duration<float>(steady_clock::now() - start).count();
    }

    // Fades from normalColor to pulseRGB, holds, fades back, repeats forever
    // on a real-time (wall-clock) cycle -- durations are in real seconds per
    // Juice's spec, not scaled by TPS/speedhack.
    static ccColor4F applyPulse(ccColor4F normalColor, const float pulseRGB[3],
                                 float fadeIn, float hold, float fadeOut) {
        float cycle = fadeIn + hold + fadeOut;
        if (cycle <= 0.0001f) return normalColor;
        float t = std::fmod(nowSeconds(), cycle);
        float mix;
        if (t < fadeIn) {
            mix = fadeIn > 0.0001f ? t / fadeIn : 1.f;
        } else if (t < fadeIn + hold) {
            mix = 1.f;
        } else {
            float ft = t - fadeIn - hold;
            mix = fadeOut > 0.0001f ? 1.f - (ft / fadeOut) : 0.f;
        }
        mix = std::clamp(mix, 0.f, 1.f);
        return ccColor4F{
            normalColor.r + (pulseRGB[0] - normalColor.r) * mix,
            normalColor.g + (pulseRGB[1] - normalColor.g) * mix,
            normalColor.b + (pulseRGB[2] - normalColor.b) * mix,
            normalColor.a
        };
    }

                ccColor4F gradeColor(int window, int maxWindow) const {
        float span = std::max(1, maxWindow - 1);
        float t = std::clamp(static_cast<float>(window - 1) / span, 0.f, 1.f);
                return ccColor4F{ 1.f - t, t, 0.15f, 1.f };
    }

    // Juice's marker customization spec (2026-08-21). Dispatches on the
    // marking tier's shape/fill settings (tier == nullptr means no tier
    // matched -- keeps the original double-ring circle, Inverted style).
    // Rounded corners (Polygon shape) are only applied in Inverted (outline)
    // style -- Normal/donut fill uses sharp corners for both boundaries.
    // Known limitation: the donut fill's inner/outer seam can show a minor
    // triangulation artifact on some shapes -- flagged as a follow-up, not
    // blocking.
    void drawMarkerShape(CCPoint center, float radius, ccColor4F color,
                          const GucciEngine::FrameWindowTier* tier) {
        auto shape     = tier ? tier->shape     : GucciEngine::FwMarkerShape::Circle;
        auto fillStyle = tier ? tier->fillStyle : GucciEngine::FwFillStyle::Inverted;
        bool noBorder  = tier && tier->noBorder;
        float stroke   = tier ? tier->strokeSize : GucciEngine::get()->fwRingBoldness;
        int sides      = tier ? std::clamp(tier->polygonSides, 3, 12) : 5;
        float cornerR  = tier ? std::clamp(tier->polygonCornerRadius, 0.f, 1.f) : 0.f;

        switch (shape) {
            case GucciEngine::FwMarkerShape::Star:
                drawStarShape(center, radius, color, fillStyle, noBorder, stroke);
                break;
            case GucciEngine::FwMarkerShape::Spiral:
                drawSpiralShape(center, radius, color, stroke);
                break;
            case GucciEngine::FwMarkerShape::Polygon:
                drawPolygonShape(center, radius, sides, cornerR, color, fillStyle, noBorder, stroke);
                break;
            case GucciEngine::FwMarkerShape::Circle:
            default:
                drawCircleShape(center, radius, color, fillStyle, noBorder, stroke);
                break;
        }
    }

    static ccColor4F borderColorFor(bool noBorder) {
        return noBorder ? ccColor4F{ 0, 0, 0, 0 } : ccColor4F{ 0, 0, 0, 1.f };
    }

    void drawCircleShape(CCPoint center, float radius, ccColor4F color,
                          GucciEngine::FwFillStyle fillStyle, bool noBorder, float stroke) {
        const int segs = 28;
        ccColor4F clear4{ 0, 0, 0, 0 };
        float innerR = radius * 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            ccColor4F bc = borderColorFor(noBorder);
            std::vector<CCPoint> ring = buildAnnulusVerts(center, radius, innerR, segs,
                [](CCPoint c, float ang, float r){ return CCPoint{ c.x + r * std::cos(ang), c.y + r * std::sin(ang) }; });
            m_node->drawPolygon(ring.data(), (int)ring.size(), color, noBorder ? 0.f : stroke, bc);
        } else {
            m_node->drawCircle(center, radius, clear4, stroke, color, segs);
            m_node->drawCircle(center, innerR, clear4, stroke, color, segs);
        }
    }

    void drawPolygonShape(CCPoint center, float radius, int sides, float cornerRadius,
                           ccColor4F color, GucciEngine::FwFillStyle fillStyle,
                           bool noBorder, float stroke) {
        ccColor4F clear4{ 0, 0, 0, 0 };
        float innerR = radius * 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            ccColor4F bc = borderColorFor(noBorder);
            auto outer = regularPolygonVerts(center, radius, sides, 0.f);
            auto inner = regularPolygonVerts(center, innerR, sides, 0.f);
            auto ring = buildAnnulusFromVerts(outer, inner);
            m_node->drawPolygon(ring.data(), (int)ring.size(), color, noBorder ? 0.f : stroke, bc);
        } else {
            auto outer = roundedPolygonVerts(center, radius, sides, cornerRadius);
            auto inner = roundedPolygonVerts(center, innerR, sides, cornerRadius);
            m_node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
            m_node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
        }
    }

    void drawStarShape(CCPoint center, float radius, ccColor4F color,
                        GucciEngine::FwFillStyle fillStyle, bool noBorder, float stroke) {
        const int points = 5;
        ccColor4F clear4{ 0, 0, 0, 0 };
        auto outer = starVerts(center, radius, radius * 0.42f, points);
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            ccColor4F bc = borderColorFor(noBorder);
            m_node->drawPolygon(outer.data(), (int)outer.size(), color, noBorder ? 0.f : stroke, bc);
        } else {
            m_node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
        }
    }

    void drawSpiralShape(CCPoint center, float radius, ccColor4F color, float stroke) {
        // Spirals don't have an inside/outside to fill -- always stroke-only,
        // regardless of fill style.
        const int turns = 2;
        const int segsPerTurn = 16;
        const int total = turns * segsPerTurn;
        CCPoint prev = center;
        for (int i = 1; i <= total; ++i) {
            float t = (float)i / (float)total;
            float ang = t * turns * 2.f * (float)M_PI;
            float r = t * radius;
            CCPoint cur{ center.x + r * std::cos(ang), center.y + r * std::sin(ang) };
            m_node->drawSegment(prev, cur, stroke, color);
            prev = cur;
        }
    }

    static std::vector<CCPoint> regularPolygonVerts(CCPoint center, float radius, int sides, float rotOffset) {
        std::vector<CCPoint> v;
        v.reserve(sides);
        for (int i = 0; i < sides; ++i) {
            float ang = rotOffset + (float)i / (float)sides * 2.f * (float)M_PI - (float)M_PI / 2.f;
            v.push_back({ center.x + radius * std::cos(ang), center.y + radius * std::sin(ang) });
        }
        return v;
    }

    // Chamfers each corner toward its neighbors by cornerRadius (0..1 fraction
    // of the shorter adjacent edge) instead of a true rounded arc -- a
    // reasonable approximation, not geometrically exact.
    static std::vector<CCPoint> roundedPolygonVerts(CCPoint center, float radius, int sides, float cornerRadius) {
        auto sharp = regularPolygonVerts(center, radius, sides, 0.f);
        if (cornerRadius <= 0.001f) return sharp;
        std::vector<CCPoint> out;
        out.reserve(sides * 2);
        int n = (int)sharp.size();
        for (int i = 0; i < n; ++i) {
            CCPoint prev = sharp[(i - 1 + n) % n];
            CCPoint cur  = sharp[i];
            CCPoint next = sharp[(i + 1) % n];
            float lenIn  = ccpDistance(prev, cur);
            float lenOut = ccpDistance(cur, next);
            float cut = cornerRadius * 0.5f * std::min(lenIn, lenOut);
            CCPoint a{ cur.x + (prev.x - cur.x) / lenIn  * cut, cur.y + (prev.y - cur.y) / lenIn  * cut };
            CCPoint b{ cur.x + (next.x - cur.x) / lenOut * cut, cur.y + (next.y - cur.y) / lenOut * cut };
            out.push_back(a);
            out.push_back(b);
        }
        return out;
    }

    static std::vector<CCPoint> starVerts(CCPoint center, float outerR, float innerR, int points) {
        std::vector<CCPoint> v;
        v.reserve(points * 2);
        int total = points * 2;
        for (int i = 0; i < total; ++i) {
            float r = (i % 2 == 0) ? outerR : innerR;
            float ang = (float)i / (float)total * 2.f * (float)M_PI - (float)M_PI / 2.f;
            v.push_back({ center.x + r * std::cos(ang), center.y + r * std::sin(ang) });
        }
        return v;
    }

    // Converts an outer/inner boundary pair into one simple polygon fillable
    // as a ring: around the outside, a zero-width bridge back to the start,
    // then around the inside in reverse. May leave a faint seam artifact at
    // the bridge on some shapes.
    template <typename PointAt>
    static std::vector<CCPoint> buildAnnulusVerts(CCPoint center, float outerR, float innerR, int segs, PointAt pointAt) {
        std::vector<CCPoint> outer, inner;
        for (int i = 0; i <= segs; ++i) {
            float ang = (float)i / (float)segs * 2.f * (float)M_PI;
            outer.push_back(pointAt(center, ang, outerR));
        }
        for (int i = 0; i <= segs; ++i) {
            float ang = (float)i / (float)segs * 2.f * (float)M_PI;
            inner.push_back(pointAt(center, ang, innerR));
        }
        return buildAnnulusFromVerts(outer, inner);
    }

    static std::vector<CCPoint> buildAnnulusFromVerts(std::vector<CCPoint> outer, std::vector<CCPoint> inner) {
        std::vector<CCPoint> v = outer;
        v.push_back(outer[0]);
        std::reverse(inner.begin(), inner.end());
        for (auto& p : inner) v.push_back(p);
        return v;
    }

    void clear() {
        if (m_node) m_node->clear();
        if (m_labelLayer) m_labelLayer->removeAllChildren();
    }

    CCDrawNode* m_node = nullptr;
    CCNode*     m_labelLayer = nullptr;
    CCDrawNode* m_debugNode = nullptr;
    int         m_builtForCount = -1;
};

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

namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRendering) {
        FrameWindowOverlay::get()->render(pl, isRendering);
    }

                                        void playTierSound(int window) {
        static std::unordered_map<std::string, FMOD::Sound*> s_soundCache;

        auto* gb = GucciEngine::get();
        auto* tier = gb->fwTierFor(window);

        std::filesystem::path path;
        if (tier && std::string(tier->soundFile) == "none") {
            return;
        } else if (tier && tier->soundFile[0]) {
            path = Mod::get()->getSaveDir() / "fw_assets" / tier->soundFile;
        } else {
            path = Mod::get()->getResourcesDir() / "fw_default.mp3";
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) return;

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

        // Each call gets its own fresh FMOD channel instead of sharing/stealing one --
        // these are supposed to be able to overlap (e.g. two clicks close together each
        // get their own cue), and FMOD already handles concurrent channels natively.
        // Previously this stopped whatever was already playing before starting the new
        // one, which is exactly why overlapping cues were cutting each other off.
        //
        // Starts paused so the per-tier volume (Juice's spec) is applied before
        // any audio actually comes out, instead of a frame at full volume first.
        FMOD::Channel* channel = nullptr;
        system->playSound(sound, nullptr, true, &channel);
        if (channel) {
            channel->setVolume(tier ? std::clamp(tier->volume, 0.f, 1.f) : 1.f);
            channel->setPaused(false);
        }
    }
}

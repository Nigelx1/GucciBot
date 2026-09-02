#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "core/GucciBot.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>

using namespace geode::prelude;

using namespace gucci;

static bool parentChainFlipped(cocos2d::CCNode* n) {
    float sx = 1.f;
    for (auto* p = n; p; p = p->getParent())
        sx *= p->getScaleX();
    return sx < 0.f;
}

class FrameWindowOverlay {
public:
    static FrameWindowOverlay* get() {
        static FrameWindowOverlay inst;
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
        anchor->addChild(node, 1403);
        m_node = node;

        auto* labels = CCNode::create();
        anchor->addChild(labels, 1404);
        m_labelLayer = labels;
    }

    void detach() {
        if (m_node) {
            m_node->removeFromParent();
            m_node = nullptr;
        }
        if (m_labelLayer) {
            m_labelLayer->removeFromParent();
            m_labelLayer = nullptr;
        }
        if (m_debugNode) {
            m_debugNode->removeFromParent();
            m_debugNode = nullptr;
        }
        m_builtForCount = -1;
    }

    CCRect computeVisibleRect() {
        auto* director = CCDirector::sharedDirector();
        CCSize visSize = director->getVisibleSize();
        CCPoint visOrigin = director->getVisibleOrigin();
        CCPoint bl = m_node->convertToNodeSpace(visOrigin);
        CCPoint tr =
            m_node->convertToNodeSpace({visOrigin.x + visSize.width, visOrigin.y + visSize.height});
        const float margin = 60.f;
        float minX = std::min(bl.x, tr.x) - margin;
        float maxX = std::max(bl.x, tr.x) + margin;
        float minY = std::min(bl.y, tr.y) - margin;
        float maxY = std::max(bl.y, tr.y) + margin;
        return CCRect(minX, minY, maxX - minX, maxY - minY);
    }

    void renderDebugMarks(PlayLayer* pl, GucciEngine* gb, bool isRendering) {
        if (!m_debugNode) {
            auto* anchor = pl->m_objectLayer;
            if (!anchor)
                return;
            auto* node = CCDrawNode::create();
            node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            node->m_bUseArea = false;
            anchor->addChild(node, 1405);
            m_debugNode = node;
        }
        m_debugNode->clear();
        if (isRendering || !gb->fwAnalyzing || !gb->fwDebugMode)
            return;

        CCRect visRect = computeVisibleRect();

        for (auto const& mk : gb->fwDebugMarks) {
            CCPoint at{mk.x, mk.y};
            if (!visRect.containsPoint(at))
                continue;
            if (mk.survived) {
                ccColor4F col{0.25f, 1.f, 0.35f, 1.f};
                ccColor4F clear4{0.f, 0.f, 0.f, 0.f};
                m_debugNode->drawCircle(at, 6.f, clear4, 2.5f, col, 16);
            } else {
                ccColor4F col{1.f, 0.2f, 0.2f, 1.f};
                float s = 6.f;
                m_debugNode->drawSegment({at.x - s, at.y - s}, {at.x + s, at.y + s}, 2.5f, col);
                m_debugNode->drawSegment({at.x - s, at.y + s}, {at.x + s, at.y - s}, 2.5f, col);
            }
        }

        if (gb->fwPositionCheckEnabled && gb->fwProbeHasNext) {
            CCPoint c{gb->fwProbeNextX, gb->fwProbeNextY};
            float s = gb->fwPositionSlack;
            ccColor4F clear4{0.f, 0.f, 0.f, 0.f};
            ccColor4F boxCol{0.2f, 0.7f, 1.f, 1.f};
            CCPoint box[4] = {
                {c.x - s, c.y - s},
                {c.x + s, c.y - s},
                {c.x + s, c.y + s},
                {c.x - s, c.y + s},
            };
            m_debugNode->drawPolygon(box, 4, clear4, 2.f, boxCol);
        }
    }

    void render(PlayLayer* pl, bool isRendering) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl)
            return;
        if (!m_node) {
            attach(pl);
            if (!m_node)
                return;
        }

        renderDebugMarks(pl, gb, isRendering);

        bool show = isRendering ? gb->fwEnabledRender : gb->fwEnabledLive;
        if (!show || !gb->fwHasData) {
            if (m_builtForCount != 0) {
                clear();
                m_builtForCount = 0;
            }
            return;
        }

        uint32_t curFrame = gb->updater.getFrame();

        if (curFrame < m_lastCurFrame)
            m_pulseStart.clear();
        m_lastCurFrame = curFrame;

        CCRect visRect = computeVisibleRect();

        int visibleCount = 0;
        bool anyPulseActive = false;
        for (auto const& mk : gb->fwMarks) {
            if (mk.frame > curFrame || mk.window > gb->fwMaxWindow)
                continue;
            ++visibleCount;
            auto* t = gb->fwTierFor(mk.window);
            if (!t || (!t->markerPulseEnabled && !t->textPulseEnabled))
                continue;
            uint64_t key = pulseKey(mk.frame, mk.player2);
            auto it = m_pulseStart.find(key);
            if (it == m_pulseStart.end()) {
                m_pulseStart[key] = nowSeconds();
                anyPulseActive = true;
            } else {
                float maxCycle =
                    std::max(t->markerPulseEnabled
                                 ? t->markerPulseFadeIn + t->markerPulseHold + t->markerPulseFadeOut
                                 : 0.f,
                             t->textPulseEnabled
                                 ? t->textPulseFadeIn + t->textPulseHold + t->textPulseFadeOut
                                 : 0.f);
                if (nowSeconds() - it->second < maxCycle)
                    anyPulseActive = true;
            }
        }

        bool mirrored = m_labelLayer && parentChainFlipped(m_labelLayer);

        int camBucketX = (int)std::floor(visRect.getMidX() / 20.f);
        int camBucketY = (int)std::floor(visRect.getMidY() / 20.f);

        int sig = visibleCount * 100000 + static_cast<int>(gb->fwMarks.size()) * 100 +
                  gb->fwMaxWindow + (mirrored ? 1 : 0) + camBucketX * 7919 + camBucketY * 104729;
        if (!anyPulseActive && sig == m_builtForCount)
            return;
        m_builtForCount = anyPulseActive ? -1 : sig;

        {
            int loosest = 0;
            for (auto const& mk : gb->fwMarks)
                if (mk.window > loosest)
                    loosest = mk.window;
            log::info("[GucciBot] Frame-window overlay: node={}, {} marks total, "
                      "{} visible at threshold <= {} (loosest measured = {})",
                      m_node ? "attached" : "NULL",
                      gb->fwMarks.size(),
                      visibleCount,
                      gb->fwMaxWindow,
                      loosest);
        }

        clear();

        for (auto const& mk : gb->fwMarks) {
            if (mk.window > gb->fwMaxWindow)
                continue;
            if (mk.frame > curFrame)
                continue;

            auto* tier = gb->fwTierFor(mk.window);
            if (!gb->fwTiers.empty() && !tier)
                continue;

            CCPoint at{mk.x, mk.y};
            if (!visRect.containsPoint(at))
                continue;

            ccColor4F col = tier ? ccColor4F{tier->r, tier->g, tier->b, 1.f}
                                 : gradeColor(mk.window, gb->fwMaxWindow);

            ccColor4F markerCol = col;
            ccColor4F textCol = col;
            if (tier && (tier->markerPulseEnabled || tier->textPulseEnabled)) {
                auto it = m_pulseStart.find(pulseKey(mk.frame, mk.player2));
                float elapsed = it != m_pulseStart.end() ? (nowSeconds() - it->second) : 0.f;
                if (tier->markerPulseEnabled)
                    markerCol = applyPulse(col,
                                           tier->markerPulseColor,
                                           elapsed,
                                           tier->markerPulseFadeIn,
                                           tier->markerPulseHold,
                                           tier->markerPulseFadeOut);
                if (tier->textPulseEnabled)
                    textCol = applyPulse(col,
                                         tier->textPulseColor,
                                         elapsed,
                                         tier->textPulseFadeIn,
                                         tier->textPulseHold,
                                         tier->textPulseFadeOut);
            }

            if (gb->fwCircleSkinEnabled) {
                drawCircleSkinMarker(at, mk.window, markerCol);
            } else {
                bool drewSprite = false;
                if (tier && tier->imageFile[0]) {
                    auto path = Mod::get()->getSaveDir() / "fw_assets" / tier->imageFile;
                    std::error_code ec;
                    if (std::filesystem::exists(path, ec)) {
                        if (auto* spr = CCSprite::create(path.string().c_str())) {
                            spr->setPosition(at);
                            float maxDim = std::max(spr->getContentSize().width,
                                                    spr->getContentSize().height);
                            float sizeMul = std::max(0.1f, tier->sizeScale);
                            if (maxDim > 0.f)
                                spr->setScale((kRadius * 2.2f * sizeMul) / maxDim);
                            spr->setColor({(GLubyte)(markerCol.r * 255),
                                           (GLubyte)(markerCol.g * 255),
                                           (GLubyte)(markerCol.b * 255)});
                            if (mirrored)
                                spr->setScaleX(-spr->getScaleX());
                            m_labelLayer->addChild(spr);
                            drewSprite = true;
                        }
                    }
                }
                if (!drewSprite)
                    drawMarkerShape(at, kRadius, markerCol, tier);
            }

            auto* lbl = CCLabelBMFont::create(std::to_string(mk.window).c_str(), "bigFont.fnt");
            lbl->setScale(0.45f);
            if (mirrored)
                lbl->setScaleX(-0.45f);
            // Circle-skin's ring can grow well past kRadius (that's the
            // whole point), so the label needs to clear the ring itself,
            // not the normal marker's fixed radius.
            float labelClearance = kRadius;
            if (gb->fwCircleSkinEnabled)
                labelClearance = std::min(gb->fwCircleSkinMaxRadius,
                                          gb->fwCircleSkinDotRadius +
                                              std::max(0, mk.window) *
                                                  gb->fwCircleSkinRadiusPerFrame);
            lbl->setPosition({at.x, at.y + labelClearance + 11.f});
            lbl->setColor({(GLubyte)(textCol.r * 255),
                           (GLubyte)(textCol.g * 255),
                           (GLubyte)(textCol.b * 255)});
            lbl->setOpacity(255);
            m_labelLayer->addChild(lbl);
        }
    }

private:
    static constexpr float kRadius = 11.f;

    std::unordered_map<uint64_t, float> m_pulseStart;
    uint32_t m_lastCurFrame = 0xFFFFFFFFu;

    static uint64_t pulseKey(uint32_t frame, bool player2) {
        return (static_cast<uint64_t>(frame) << 1) | (player2 ? 1u : 0u);
    }

    static float nowSeconds() {
        using namespace std::chrono;
        static const auto start = steady_clock::now();
        return duration<float>(steady_clock::now() - start).count();
    }

    static ccColor4F applyPulse(ccColor4F normalColor,
                                const float pulseRGB[3],
                                float elapsed,
                                float fadeIn,
                                float hold,
                                float fadeOut) {
        float cycle = fadeIn + hold + fadeOut;
        if (cycle <= 0.0001f || elapsed >= cycle)
            return normalColor;
        float mix;
        if (elapsed < fadeIn) {
            mix = fadeIn > 0.0001f ? elapsed / fadeIn : 1.f;
        } else if (elapsed < fadeIn + hold) {
            mix = 1.f;
        } else {
            float ft = elapsed - fadeIn - hold;
            mix = fadeOut > 0.0001f ? 1.f - (ft / fadeOut) : 0.f;
        }
        mix = std::clamp(mix, 0.f, 1.f);
        return ccColor4F{normalColor.r + (pulseRGB[0] - normalColor.r) * mix,
                         normalColor.g + (pulseRGB[1] - normalColor.g) * mix,
                         normalColor.b + (pulseRGB[2] - normalColor.b) * mix,
                         normalColor.a};
    }

    ccColor4F gradeColor(int window, int maxWindow) const {
        float span = std::max(1, maxWindow - 1);
        float t = std::clamp(static_cast<float>(window - 1) / span, 0.f, 1.f);
        return ccColor4F{1.f - t, t, 0.15f, 1.f};
    }

    // Juice's "circle skin" -- a fixed-size filled dot at the macro's exact
    // click timing, plus a thin unfilled ring around it whose radius grows
    // with the window (leniency) size, capped so a very forgiving click
    // doesn't blow up into an enormous circle covering the level. Ignores
    // Tier shape/image settings entirely -- this is a whole alternate skin,
    // not another marker shape option within the existing tier system.
    void drawCircleSkinMarker(CCPoint center, int window, ccColor4F color) {
        auto* gb = GucciEngine::get();
        float dotR = std::max(1.f, gb->fwCircleSkinDotRadius);
        float ringR = std::min(gb->fwCircleSkinMaxRadius,
                               dotR + std::max(0, window) * gb->fwCircleSkinRadiusPerFrame);
        ccColor4F clear4{0.f, 0.f, 0.f, 0.f};
        m_node->drawDot(center, dotR, color);
        if (ringR > dotR + 1.f) {
            float stroke = std::clamp(gb->fwRingBoldness, 1.f, ringR - dotR);
            ccColor4F ringCol = color;
            ringCol.a = 0.85f;
            m_node->drawCircle(center, ringR, clear4, stroke, ringCol, 28);
        }
    }

    void drawMarkerShape(CCPoint center,
                         float radius,
                         ccColor4F color,
                         const GucciEngine::FrameWindowTier* tier) {
        auto shape = tier ? tier->shape : GucciEngine::FwMarkerShape::Circle;
        auto fillStyle = tier ? tier->fillStyle : GucciEngine::FwFillStyle::Inverted;
        bool noBorder = tier && tier->noBorder;
        float stroke = tier ? tier->strokeSize : GucciEngine::get()->fwRingBoldness;
        int sides = tier ? std::clamp(tier->polygonSides, 3, 12) : 5;
        float cornerR = tier ? std::clamp(tier->polygonCornerRadius, 0.f, 1.f) : 0.f;
        float sizeMul = tier ? std::max(0.1f, tier->sizeScale) : 1.f;
        radius *= sizeMul;

        switch (shape) {
        case GucciEngine::FwMarkerShape::Star:
            drawStarShape(center, radius, color, fillStyle, noBorder, stroke);
            break;
        case GucciEngine::FwMarkerShape::Spiral:
            drawSpiralShape(center, radius, color, fillStyle, stroke);
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

    void drawCircleShape(CCPoint center,
                         float radius,
                         ccColor4F color,
                         GucciEngine::FwFillStyle fillStyle,
                         bool noBorder,
                         float stroke) {
        const int segs = 28;
        ccColor4F clear4{0, 0, 0, 0};
        float innerR = radius * 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            drawDonutRing(radius, innerR, color, noBorder, stroke, [&](float r) {
                return regularPolygonVerts(center, r, segs, 0.f);
            });
        } else {
            m_node->drawCircle(center, radius, clear4, stroke, color, segs);
            m_node->drawCircle(center, innerR, clear4, stroke, color, segs);
        }
    }

    void drawPolygonShape(CCPoint center,
                          float radius,
                          int sides,
                          float cornerRadius,
                          ccColor4F color,
                          GucciEngine::FwFillStyle fillStyle,
                          bool noBorder,
                          float stroke) {
        ccColor4F clear4{0, 0, 0, 0};
        float innerR = radius * 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            drawDonutRing(radius, innerR, color, noBorder, stroke, [&](float r) {
                return regularPolygonVerts(center, r, sides, 0.f);
            });
        } else {
            auto outer = roundedPolygonVerts(center, radius, sides, cornerRadius);
            auto inner = roundedPolygonVerts(center, innerR, sides, cornerRadius);
            m_node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
            m_node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
        }
    }

    void drawStarShape(CCPoint center,
                       float radius,
                       ccColor4F color,
                       GucciEngine::FwFillStyle fillStyle,
                       bool noBorder,
                       float stroke) {
        const int points = 5;
        ccColor4F clear4{0, 0, 0, 0};
        float innerScale = 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            drawDonutRing(radius, radius * innerScale, color, noBorder, stroke, [&](float r) {
                return starVerts(center, r, r * 0.42f, points);
            });
        } else {
            auto outer = starVerts(center, radius, radius * 0.42f, points);
            auto inner =
                starVerts(center, radius * innerScale, radius * innerScale * 0.42f, points);
            m_node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
            m_node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
        }
    }

    template <typename BoundaryAt>
    void drawDonutRing(float outerR,
                       float innerR,
                       ccColor4F color,
                       bool noBorder,
                       float stroke,
                       BoundaryAt boundaryAt) {
        if (outerR <= innerR)
            return;
        ccColor4F black{0, 0, 0, 1.f};
        float maxBorder = std::max(0.f, (outerR - innerR) * 0.4f);
        float bt = noBorder ? 0.f : std::clamp(stroke, 0.f, maxBorder);
        if (!noBorder && bt > 0.001f) {
            fillRingBetween(boundaryAt(outerR), boundaryAt(outerR - bt), black);
            fillRingBetween(boundaryAt(innerR + bt), boundaryAt(innerR), black);
        }
        fillRingBetween(boundaryAt(outerR - bt), boundaryAt(innerR + bt), color);
    }

    void fillRingBetween(const std::vector<CCPoint>& outer,
                         const std::vector<CCPoint>& inner,
                         ccColor4F color) {
        if (outer.size() != inner.size() || outer.size() < 2)
            return;
        ccColor4F clear4{0, 0, 0, 0};
        int n = (int)outer.size();
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            CCPoint quad[4] = {outer[i], outer[j], inner[j], inner[i]};
            m_node->drawPolygon(quad, 4, color, 0.f, clear4);
        }
    }

    void drawSpiralShape(
        CCPoint center, float radius, ccColor4F color, GucciEngine::FwFillStyle, float stroke) {
        const int turns = 2;
        const int segsPerTurn = 16;
        const int total = turns * segsPerTurn;

        auto pointAt = [&](float t) -> CCPoint {
            float ang = t * turns * 2.f * (float)M_PI;
            float r = t * radius;
            return {center.x + r * std::cos(ang), center.y + r * std::sin(ang)};
        };

        CCPoint prev = pointAt(0.f);
        for (int i = 1; i <= total; ++i) {
            CCPoint cur = pointAt((float)i / (float)total);
            m_node->drawSegment(prev, cur, stroke, color);
            prev = cur;
        }
    }

    static std::vector<CCPoint>
    regularPolygonVerts(CCPoint center, float radius, int sides, float rotOffset) {
        std::vector<CCPoint> v;
        v.reserve(sides);
        for (int i = 0; i < sides; ++i) {
            float ang = rotOffset + (float)i / (float)sides * 2.f * (float)M_PI - (float)M_PI / 2.f;
            v.push_back({center.x + radius * std::cos(ang), center.y + radius * std::sin(ang)});
        }
        return v;
    }

    static std::vector<CCPoint>
    roundedPolygonVerts(CCPoint center, float radius, int sides, float cornerRadius) {
        auto sharp = regularPolygonVerts(center, radius, sides, 0.f);
        if (cornerRadius <= 0.001f)
            return sharp;
        std::vector<CCPoint> out;
        out.reserve(sides * 2);
        int n = (int)sharp.size();
        for (int i = 0; i < n; ++i) {
            CCPoint prev = sharp[(i - 1 + n) % n];
            CCPoint cur = sharp[i];
            CCPoint next = sharp[(i + 1) % n];
            float lenIn = ccpDistance(prev, cur);
            float lenOut = ccpDistance(cur, next);
            float cut = cornerRadius * 0.5f * std::min(lenIn, lenOut);
            CCPoint a{cur.x + (prev.x - cur.x) / lenIn * cut,
                      cur.y + (prev.y - cur.y) / lenIn * cut};
            CCPoint b{cur.x + (next.x - cur.x) / lenOut * cut,
                      cur.y + (next.y - cur.y) / lenOut * cut};
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
            v.push_back({center.x + r * std::cos(ang), center.y + r * std::sin(ang)});
        }
        return v;
    }

    void clear() {
        if (m_node)
            m_node->clear();
        if (m_labelLayer)
            m_labelLayer->removeAllChildren();
    }

    CCDrawNode* m_node = nullptr;
    CCNode* m_labelLayer = nullptr;
    CCDrawNode* m_debugNode = nullptr;
    int m_builtForCount = -1;
};

class $modify(FrameWindowPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        FrameWindowOverlay::get()->attach(this);
        return true;
    }

    void onQuit() {
        FrameWindowOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};

namespace gucci::gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRendering) {
        FrameWindowOverlay::get()->render(pl, isRendering);
    }

    FMOD::ChannelGroup* frameWindowChannelGroup() {
        static FMOD::ChannelGroup* group = nullptr;
        if (group)
            return group;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system)
            return nullptr;
        if (system->createChannelGroup("gucciFrameWindow", &group) != FMOD_OK || !group)
            return nullptr;
        FMOD::ChannelGroup* master = nullptr;
        system->getMasterChannelGroup(&master);
        if (master)
            master->addGroup(group);
        return group;
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
        if (!std::filesystem::exists(path, ec))
            return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system)
            return;

        std::string key = path.string();
        FMOD::Sound* sound = nullptr;
        auto it = s_soundCache.find(key);
        if (it != s_soundCache.end()) {
            sound = it->second;
        } else {
            if (system->createSound(key.c_str(), FMOD_CREATESAMPLE, nullptr, &sound) != FMOD_OK ||
                !sound)
                return;
            s_soundCache[key] = sound;
        }

        FMOD::Channel* channel = nullptr;
        system->playSound(sound, frameWindowChannelGroup(), true, &channel);
        if (channel) {
            channel->setVolume(tier ? std::clamp(tier->volume, 0.f, 1.f) : 1.f);
            channel->setPaused(false);
        }
    }
} // namespace gbfw

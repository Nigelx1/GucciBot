#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "GucciBot.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>

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

    // The camera's actual current view, converted into m_node's local space
    // (the same space mk.x/mk.y are already in) via its real transform --
    // not a fixed distance from the player, so this stays correct through
    // zoom triggers. A margin keeps marks from popping in/out right at the
    // screen edge as the camera scrolls.
    CCRect computeVisibleRect() {
        auto* director = CCDirector::sharedDirector();
        CCSize visSize = director->getVisibleSize();
        CCPoint visOrigin = director->getVisibleOrigin();
        CCPoint bl = m_node->convertToNodeSpace(visOrigin);
        CCPoint tr = m_node->convertToNodeSpace(
            { visOrigin.x + visSize.width, visOrigin.y + visSize.height });
        const float margin = 60.f;
        float minX = std::min(bl.x, tr.x) - margin;
        float maxX = std::max(bl.x, tr.x) + margin;
        float minY = std::min(bl.y, tr.y) - margin;
        float maxY = std::max(bl.y, tr.y) + margin;
        return CCRect(minX, minY, maxX - minX, maxY - minY);
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

        // Juice's request (2026-08-21): cull debug marks outside the camera's
        // actual view too -- same reasoning/technique as the main marker
        // overlay's culling. m_debugNode is an untransformed sibling of
        // m_node under the same anchor, so m_node's conversion is valid here.
        CCRect visRect = computeVisibleRect();

        for (auto const& mk : gb->fwDebugMarks) {
            CCPoint at{ mk.x, mk.y };
            if (!visRect.containsPoint(at)) continue;
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

        // Juice's request: a visual box showing the allowed Position
        // Tolerance range around the next input's true position, so it's
        // possible to actually SEE whether a survived shift landed inside or
        // outside it instead of only trusting the reported pass/fail.
        if (gb->fwPositionCheckEnabled && gb->fwProbeHasNext) {
            CCPoint c{ gb->fwProbeNextX, gb->fwProbeNextY };
            float s = gb->fwPositionSlack;
            ccColor4F clear4{ 0.f, 0.f, 0.f, 0.f };
            ccColor4F boxCol{ 0.2f, 0.7f, 1.f, 1.f };
            CCPoint box[4] = {
                { c.x - s, c.y - s }, { c.x + s, c.y - s },
                { c.x + s, c.y + s }, { c.x - s, c.y + s },
            };
            m_debugNode->drawPolygon(box, 4, clear4, 2.f, boxCol);
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

        // A frame count lower than last time means a new attempt/playthrough
        // just started (checkpoint respawn, restart, or a fresh watch) --
        // clear every mark's pulse-start time so pulses can trigger again.
        if (curFrame < m_lastCurFrame) m_pulseStart.clear();
        m_lastCurFrame = curFrame;

                        // Juice's lag report (2026-08-21): nothing was culling marks
        // outside the camera's actual view -- every mark ever revealed
        // (frame <= curFrame) got drawn, even ones far behind/ahead of where
        // the camera currently is, worst on Spiral since it's the most
        // expensive shape to draw per mark. Computed in the CCDrawNode's own
        // local space (same space mk.x/mk.y are already in) via the real
        // camera transform, not a fixed distance guess, so it stays correct
        // through zoom triggers too.
        CCRect visRect = computeVisibleRect();

                int visibleCount = 0;
        // Pulse effects need to redraw every frame while any are actually
        // mid-animation -- the signature cache below would otherwise freeze
        // them on their first frame. Registers each newly-visible mark's
        // pulse start time here too, so it's set before drawing needs it.
        bool anyPulseActive = false;
        for (auto const& mk : gb->fwMarks) {
            if (mk.frame > curFrame || mk.window > gb->fwMaxWindow) continue;
            ++visibleCount;
            auto* t = gb->fwTierFor(mk.window);
            if (!t || (!t->markerPulseEnabled && !t->textPulseEnabled)) continue;
            uint64_t key = pulseKey(mk.frame, mk.player2);
            auto it = m_pulseStart.find(key);
            if (it == m_pulseStart.end()) {
                m_pulseStart[key] = nowSeconds();
                anyPulseActive = true;
            } else {
                float maxCycle = std::max(
                    t->markerPulseEnabled ? t->markerPulseFadeIn + t->markerPulseHold + t->markerPulseFadeOut : 0.f,
                    t->textPulseEnabled   ? t->textPulseFadeIn   + t->textPulseHold   + t->textPulseFadeOut   : 0.f);
                if (nowSeconds() - it->second < maxCycle) anyPulseActive = true;
            }
        }

                                        bool mirrored = m_labelLayer && parentChainFlipped(m_labelLayer);

                        // Which marks are actually inside visRect changes as the camera
        // scrolls, even when nothing else here does -- fold a coarsely
        // quantized camera position into the signature so the overlay
        // rebuilds as marks cross into/out of view, without rebuilding
        // every single tick for sub-pixel camera motion.
        int camBucketX = (int)std::floor(visRect.getMidX() / 20.f);
        int camBucketY = (int)std::floor(visRect.getMidY() / 20.f);

        int sig = visibleCount * 100000
                + static_cast<int>(gb->fwMarks.size()) * 100
                + gb->fwMaxWindow
                + (mirrored ? 1 : 0)
                + camBucketX * 7919
                + camBucketY * 104729;
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
            if (!visRect.containsPoint(at)) continue;

            ccColor4F col = tier
                ? ccColor4F{ tier->r, tier->g, tier->b, 1.f }
                : gradeColor(mk.window, gb->fwMaxWindow);

            // Pulse effects (Juice's spec): marker and text pulse independently,
            // each fading from the tier's normal color to its own pulse color,
            // holding, and fading back -- ONCE, per mark, from the moment that
            // specific mark first became visible this playthrough. Real seconds,
            // not scaled by TPS/speedhack. See applyPulse()/m_pulseStart.
            ccColor4F markerCol = col;
            ccColor4F textCol   = col;
            if (tier && (tier->markerPulseEnabled || tier->textPulseEnabled)) {
                auto it = m_pulseStart.find(pulseKey(mk.frame, mk.player2));
                float elapsed = it != m_pulseStart.end() ? (nowSeconds() - it->second) : 0.f;
                if (tier->markerPulseEnabled)
                    markerCol = applyPulse(col, tier->markerPulseColor, elapsed,
                        tier->markerPulseFadeIn, tier->markerPulseHold, tier->markerPulseFadeOut);
                if (tier->textPulseEnabled)
                    textCol = applyPulse(col, tier->textPulseColor, elapsed,
                        tier->textPulseFadeIn, tier->textPulseHold, tier->textPulseFadeOut);
            }

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
                        if (maxDim > 0.f) spr->setScale((kRadius * 2.2f * sizeMul) / maxDim);
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

    // Per-mark pulse state (Juice's fix, 2026-08-21): pulses used to share one
    // global, endlessly-looping clock across every marker of a tier, so a
    // 4-frame mark at frame 30 would pulse in lockstep with a 4-frame mark at
    // frame 10, forever. Each mark now gets its OWN one-shot pulse, keyed by
    // (frame, player2), starting the instant it first becomes visible this
    // playthrough and never repeating until the next playthrough (detected by
    // the frame counter going backwards -- a fresh attempt/replay).
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

    // One-shot fade from normalColor to pulseRGB, hold, fade back -- does NOT
    // repeat (elapsed >= the full cycle just stays at normalColor). Durations
    // are real seconds per Juice's spec, not scaled by TPS/speedhack.
    static ccColor4F applyPulse(ccColor4F normalColor, const float pulseRGB[3],
                                 float elapsed, float fadeIn, float hold, float fadeOut) {
        float cycle = fadeIn + hold + fadeOut;
        if (cycle <= 0.0001f || elapsed >= cycle) return normalColor;
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
    //
    // Normal/donut fill redone AGAIN 2026-08-21 per Juice's second report
    // with screenshots: the "three concentric strokes via drawPolygon's own
    // border" attempt still came out wrong on a triangle (yellow outside,
    // black middle, filled yellow center) -- drawPolygon's border/stroke
    // parameter isn't reliable for small, pointed polygons, only smooth
    // shapes like circles. Rebuilt AGAIN, this time not depending on that
    // parameter at all: each ring (outer black border, colored band, inner
    // black border) is built from scratch as a set of simple convex quads
    // between two boundaries at adjacent radii (fillRingBetween) -- a quad
    // can't misrender regardless of how pointed the shape is, so this should
    // be reliable for any shape/side count. See drawDonutRing/fillRingBetween.
    void drawMarkerShape(CCPoint center, float radius, ccColor4F color,
                          const GucciEngine::FrameWindowTier* tier) {
        auto shape     = tier ? tier->shape     : GucciEngine::FwMarkerShape::Circle;
        auto fillStyle = tier ? tier->fillStyle : GucciEngine::FwFillStyle::Inverted;
        bool noBorder  = tier && tier->noBorder;
        float stroke   = tier ? tier->strokeSize : GucciEngine::get()->fwRingBoldness;
        int sides      = tier ? std::clamp(tier->polygonSides, 3, 12) : 5;
        float cornerR  = tier ? std::clamp(tier->polygonCornerRadius, 0.f, 1.f) : 0.f;
        float sizeMul  = tier ? std::max(0.1f, tier->sizeScale) : 1.f;
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

    void drawCircleShape(CCPoint center, float radius, ccColor4F color,
                          GucciEngine::FwFillStyle fillStyle, bool noBorder, float stroke) {
        const int segs = 28;
        ccColor4F clear4{ 0, 0, 0, 0 };
        float innerR = radius * 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            drawDonutRing(radius, innerR, color, noBorder, stroke,
                [&](float r){ return regularPolygonVerts(center, r, segs, 0.f); });
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
            // Sharp corners for the donut fill -- rounded corners only apply
            // to Inverted's outline below.
            drawDonutRing(radius, innerR, color, noBorder, stroke,
                [&](float r){ return regularPolygonVerts(center, r, sides, 0.f); });
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
        float innerScale = 0.55f;
        if (fillStyle == GucciEngine::FwFillStyle::Normal) {
            drawDonutRing(radius, radius * innerScale, color, noBorder, stroke,
                [&](float r){ return starVerts(center, r, r * 0.42f, points); });
        } else {
            auto outer = starVerts(center, radius, radius * 0.42f, points);
            auto inner = starVerts(center, radius * innerScale, radius * innerScale * 0.42f, points);
            m_node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
            m_node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
        }
    }

    // Draws a filled ring as three concentric bands -- outer black border,
    // colored middle, inner black border, clear beyond that -- exactly as
    // Juice specified. boundaryAt(r) must return a closed-loop vertex list
    // (same length/winding for any r) at radius r; each ring is built from
    // simple convex quads between two such boundaries (fillRingBetween),
    // never relying on CCDrawNode's own border/stroke algorithm, which
    // proved unreliable for small pointed shapes like a triangle.
    template <typename BoundaryAt>
    void drawDonutRing(float outerR, float innerR, ccColor4F color, bool noBorder,
                        float stroke, BoundaryAt boundaryAt) {
        if (outerR <= innerR) return;
        ccColor4F black{ 0, 0, 0, 1.f };
        float maxBorder = std::max(0.f, (outerR - innerR) * 0.4f);
        float bt = noBorder ? 0.f : std::clamp(stroke, 0.f, maxBorder);
        if (!noBorder && bt > 0.001f) {
            fillRingBetween(boundaryAt(outerR), boundaryAt(outerR - bt), black);
            fillRingBetween(boundaryAt(innerR + bt), boundaryAt(innerR), black);
        }
        fillRingBetween(boundaryAt(outerR - bt), boundaryAt(innerR + bt), color);
    }

    void fillRingBetween(const std::vector<CCPoint>& outer, const std::vector<CCPoint>& inner, ccColor4F color) {
        if (outer.size() != inner.size() || outer.size() < 2) return;
        ccColor4F clear4{ 0, 0, 0, 0 };
        int n = (int)outer.size();
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            CCPoint quad[4] = { outer[i], outer[j], inner[j], inner[i] };
            m_node->drawPolygon(quad, 4, color, 0.f, clear4);
        }
    }

    // Spiral donut mode, take 2 (2026-08-21) -- Juice: the tube-cross-section
    // idea wasn't it, "the inside shouldn't be filled, just the outside."
    // Matches the same mental model as Circle/Polygon/Star's donut: a clear
    // hole in the middle. For a spiral that means simply not drawing the
    // innermost portion of the coil (r < innerR) at all, same tier color as
    // Inverted, rather than any black-bordered tube treatment.
    void drawSpiralShape(CCPoint center, float radius, ccColor4F color,
                          GucciEngine::FwFillStyle fillStyle, float stroke) {
        const int turns = 2;
        const int segsPerTurn = 16;
        const int total = turns * segsPerTurn;
        bool donut = fillStyle == GucciEngine::FwFillStyle::Normal;
        float innerR = donut ? radius * 0.55f : 0.f;

        auto pointAt = [&](float t) -> CCPoint {
            float ang = t * turns * 2.f * (float)M_PI;
            float r = t * radius;
            return { center.x + r * std::cos(ang), center.y + r * std::sin(ang) };
        };

        int startI = 0;
        if (donut && radius > 0.001f) {
            float tStart = std::clamp(innerR / radius, 0.f, 1.f);
            startI = (int)std::ceil(tStart * total);
        }
        CCPoint prev = pointAt((float)startI / (float)total);
        for (int i = startI + 1; i <= total; ++i) {
            CCPoint cur = pointAt((float)i / (float)total);
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

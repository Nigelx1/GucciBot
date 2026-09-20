#include "shapes.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;
using namespace cocos2d;

namespace gbshape {
namespace {

using FwFillStyle = Fill;
using FwMarkerShape = Shape;

// Declared up front: the originals were members of a class, so they could call
// each other in any order. As free functions they cannot.
std::vector<CCPoint> regularPolygonVerts(CCPoint center, float radius, int sides,
                                         float rotOffset);
std::vector<CCPoint> roundedPolygonVerts(CCPoint center, float radius, int sides,
                                         float cornerRadius);
std::vector<CCPoint> starVerts(CCPoint center, float outerR, float innerR, int points);
void fillRingBetween(CCDrawNode* node, std::vector<CCPoint> const& outer,
                     std::vector<CCPoint> const& inner, ccColor4F color);
template <typename BoundaryAt>
void drawDonutRing(CCDrawNode* node, float outerR, float innerR, ccColor4F color,
                   bool noBorder, float stroke, BoundaryAt boundaryAt);
void drawCircleShape(CCDrawNode* node, CCPoint center, float radius, ccColor4F color,
                     FwFillStyle fillStyle, bool noBorder, float stroke);
void drawPolygonShape(CCDrawNode* node, CCPoint center, float radius, int sides,
                      float cornerRadius, ccColor4F color, FwFillStyle fillStyle,
                      bool noBorder, float stroke);
void drawStarShape(CCDrawNode* node, CCPoint center, float radius, ccColor4F color,
                   FwFillStyle fillStyle, bool noBorder, float stroke);
void drawSpiralShape(CCDrawNode* node, CCPoint center, float radius, ccColor4F color,
                     FwFillStyle fillStyle, float stroke);

void drawCircleShape(CCDrawNode* node,
                     CCPoint center,
                     float radius,
                     ccColor4F color,
                     FwFillStyle fillStyle,
                     bool noBorder,
                     float stroke) {
    const int segs = 28;
    ccColor4F clear4{0, 0, 0, 0};
    float innerR = radius * 0.55f;
    if (fillStyle == FwFillStyle::Normal) {
        drawDonutRing(node, radius, innerR, color, noBorder, stroke, [&](float r) {
            return regularPolygonVerts(center, r, segs, 0.f);
        });
    } else {
        node->drawCircle(center, radius, clear4, stroke, color, segs);
        node->drawCircle(center, innerR, clear4, stroke, color, segs);
    }
}

void drawPolygonShape(CCDrawNode* node,
                      CCPoint center,
                      float radius,
                      int sides,
                      float cornerRadius,
                      ccColor4F color,
                      FwFillStyle fillStyle,
                      bool noBorder,
                      float stroke) {
    ccColor4F clear4{0, 0, 0, 0};
    float innerR = radius * 0.55f;
    if (fillStyle == FwFillStyle::Normal) {
        drawDonutRing(node, radius, innerR, color, noBorder, stroke, [&](float r) {
            return regularPolygonVerts(center, r, sides, 0.f);
        });
    } else {
        auto outer = roundedPolygonVerts(center, radius, sides, cornerRadius);
        auto inner = roundedPolygonVerts(center, innerR, sides, cornerRadius);
        node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
        node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
    }
}

void drawStarShape(CCDrawNode* node,
                   CCPoint center,
                   float radius,
                   ccColor4F color,
                   FwFillStyle fillStyle,
                   bool noBorder,
                   float stroke) {
    const int points = 5;
    ccColor4F clear4{0, 0, 0, 0};
    float innerScale = 0.55f;
    if (fillStyle == FwFillStyle::Normal) {
        drawDonutRing(node, radius, radius * innerScale, color, noBorder, stroke, [&](float r) {
            return starVerts(center, r, r * 0.42f, points);
        });
    } else {
        auto outer = starVerts(center, radius, radius * 0.42f, points);
        auto inner =
            starVerts(center, radius * innerScale, radius * innerScale * 0.42f, points);
        node->drawPolygon(outer.data(), (int)outer.size(), clear4, stroke, color);
        node->drawPolygon(inner.data(), (int)inner.size(), clear4, stroke, color);
    }
}

template <typename BoundaryAt>
void drawDonutRing(CCDrawNode* node,
                   float outerR,
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
        fillRingBetween(node, boundaryAt(outerR), boundaryAt(outerR - bt), black);
        fillRingBetween(node, boundaryAt(innerR + bt), boundaryAt(innerR), black);
    }
    fillRingBetween(node, boundaryAt(outerR - bt), boundaryAt(innerR + bt), color);
}

void fillRingBetween(CCDrawNode* node,
                     const std::vector<CCPoint>& outer,
                     const std::vector<CCPoint>& inner,
                     ccColor4F color) {
    if (outer.size() != inner.size() || outer.size() < 2)
        return;
    ccColor4F clear4{0, 0, 0, 0};
    int n = (int)outer.size();
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        CCPoint quad[4] = {outer[i], outer[j], inner[j], inner[i]};
        node->drawPolygon(quad, 4, color, 0.f, clear4);
    }
}

void drawSpiralShape(
    CCDrawNode* node, CCPoint center, float radius, ccColor4F color, FwFillStyle, float stroke) {
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
        node->drawSegment(prev, cur, stroke, color);
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


}  // namespace

void draw(CCDrawNode* node,
          CCPoint center,
          float radius,
          ccColor4F color,
          Style const& style) {
    if (!node)
        return;

    float const r = std::max(1.f, radius * std::max(0.1f, style.sizeScale));
    int const sides = std::clamp(style.polygonSides, 3, 12);
    float const corner = std::clamp(style.polygonCornerRadius, 0.f, 1.f);

    switch (style.shape) {
        case Shape::Star:
            drawStarShape(node, center, r, color, style.fill, style.noBorder,
                          style.strokeSize);
            break;
        case Shape::Spiral:
            drawSpiralShape(node, center, r, color, style.fill, style.strokeSize);
            break;
        case Shape::Polygon:
            drawPolygonShape(node, center, r, sides, corner, color, style.fill,
                             style.noBorder, style.strokeSize);
            break;
        case Shape::Circle:
        default:
            drawCircleShape(node, center, r, color, style.fill, style.noBorder,
                            style.strokeSize);
            break;
    }
}

}  // namespace gbshape

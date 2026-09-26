// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#ifndef TRAILBUF_GEOMETRY_HPP
#define TRAILBUF_GEOMETRY_HPP

#include <algorithm>
#include <cmath>

namespace tbuf {

struct RectF {
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;

    static RectF fromCenter(float cx, float cy, float w, float h) {
        return RectF{cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                     cy + h * 0.5f};
    }

    float width() const { return maxX - minX; }
    float height() const { return maxY - minY; }
    float centerX() const { return (minX + maxX) * 0.5f; }
    float centerY() const { return (minY + maxY) * 0.5f; }

    RectF expanded(float m) const {
        return RectF{minX - m, minY - m, maxX + m, maxY + m};
    }

    RectF scaledAboutCenter(float s) const {
        return fromCenter(centerX(), centerY(), width() * s, height() * s);
    }

    RectF unionWith(RectF const& o) const {
        return RectF{std::min(minX, o.minX), std::min(minY, o.minY),
                     std::max(maxX, o.maxX), std::max(maxY, o.maxY)};
    }

    bool overlaps(RectF const& o) const {
        return minX < o.maxX && o.minX < maxX && minY < o.maxY &&
               o.minY < maxY;
    }

    bool operator==(RectF const& o) const {
        return minX == o.minX && minY == o.minY && maxX == o.maxX &&
               maxY == o.maxY;
    }
};

}  // namespace tbuf

#endif  // TRAILBUF_GEOMETRY_HPP

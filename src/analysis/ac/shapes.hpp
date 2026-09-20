#pragma once

// Juice's marker shapes, brought forward from GucciBot 1.7.2.
//
// anticroom's analyzer draws one plain ring per mark. This is the richer set
// GucciBot had before it: circles, stars, spirals and polygons, each either
// Inverted (two concentric outlines -- the "inner circle" look) or Normal (a
// filled donut with an optional border).
//
// Lifted essentially unchanged; the only real edit is that every function
// takes the CCDrawNode to draw into, where the originals were members of the
// overlay and used its own node.

#include <Geode/Geode.hpp>
#include <vector>

namespace gbshape {

    enum class Shape : int { Circle = 0, Star = 1, Spiral = 2, Polygon = 3 };
    enum class Fill : int { Inverted = 0, Normal = 1 };

    struct Style {
        Shape shape = Shape::Circle;
        Fill fill = Fill::Inverted;
        int polygonSides = 5;
        float polygonCornerRadius = 0.f;
        bool noBorder = false;
        float strokeSize = 2.2f;
        float sizeScale = 1.f;
    };

    // Draws one marker in the given style. radius is the base size before
    // the style's own sizeScale is applied.
    void draw(cocos2d::CCDrawNode* node,
              cocos2d::CCPoint center,
              float radius,
              cocos2d::ccColor4F color,
              Style const& style);

}  // namespace gbshape

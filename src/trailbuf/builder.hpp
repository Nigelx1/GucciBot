// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#ifndef TRAILBUF_BUILDER_HPP
#define TRAILBUF_BUILDER_HPP

#include <string>
#include <vector>

#include "generator.hpp"

class LevelEditorLayer;

namespace tbuf {

struct BlockShape {
    float width = 30.f;
    float height = 30.f;
    float offsetX = 0.f;
    float offsetY = 0.f;
};

BlockShape measureObject(LevelEditorLayer* editor, int objectId);

std::vector<RectF> collectSolids(LevelEditorLayer* editor, RectF const& area);

struct BuildReport {
    bool ok = false;
    int placed = 0;
    std::string message;
    std::vector<RectF> placedRects;
};

BuildReport placeBlocks(LevelEditorLayer* editor, int objectId,
                        BlockShape const& shape,
                        std::vector<GenBlock> const& blocks);

}  // namespace tbuf

#endif  // TRAILBUF_BUILDER_HPP

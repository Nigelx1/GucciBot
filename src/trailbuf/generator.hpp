// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#ifndef TRAILBUF_GENERATOR_HPP
#define TRAILBUF_GENERATOR_HPP

#include <cstdint>
#include <vector>

#include "geometry.hpp"

namespace tbuf {

struct Sample {
    RectF rect;
    uint32_t frame = 0;
    bool startsRun = false;
};

struct Segment {
    RectF a;
    RectF b;
    uint32_t frame = 0;
    bool bandSource = true;

    float halfWidth() const { return std::max(a.width(), b.width()) * 0.5f; }
    float halfHeight() const { return std::max(a.height(), b.height()) * 0.5f; }

    float minX() const {
        return std::min(a.centerX(), b.centerX()) - this->halfWidth();
    }
    float maxX() const {
        return std::max(a.centerX(), b.centerX()) + this->halfWidth();
    }
};

std::vector<Segment> buildSegments(std::vector<Sample> const& samples,
                                   bool sweep);

bool sweptSpan(Segment const& seg, float x0, float x1, float& yMin,
               float& yMax);

struct GenSettings {
    float gap = 0.01f;
    float fillRadius = 1.f;
    float columnWidth = 0.25f;
    float minBlockSize = 0.0025f;
    float maxScale = 100.f;
    int maxObjects = 800000;
    int frameInterval = 1;
    float gateWidth = 30.f;
    float mergeTolerance = 0.5f;
    float startTrim = 0.f;
    float endTrim = 90.f;
};

struct GenBlock {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

struct GenResult {
    std::vector<GenBlock> blocks;
    int columns = 0;
    int gapsFound = 0;
    int splits = 0;
    bool hitObjectLimit = false;
    bool areaTooLarge = false;
};

GenResult generate(std::vector<Segment> const& segments,
                   std::vector<RectF> const& solids, float blockW,
                   float blockH, GenSettings const& settings);

struct VerifyResult {
    size_t blocksChecked = 0;
    size_t violations = 0;
    float worstOverlap = 0.f;
    float tightestClearance = 0.f;
    uint32_t firstViolationFrame = 0;
};

VerifyResult verify(std::vector<Segment> const& segments,
                    std::vector<RectF> const& placed);

constexpr float SCALE_STEP = 0.001f;

int fitToScaleSteps(std::vector<GenBlock>& blocks,
                    std::vector<Segment> const& segments, float blockW,
                    float blockH);

}  // namespace tbuf

#endif  // TRAILBUF_GENERATOR_HPP

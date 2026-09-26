// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#include "generator.hpp"

#include <algorithm>
#include <limits>

namespace tbuf {

namespace {

constexpr int64_t MAX_COLUMNS = 2'000'000;
constexpr size_t MAX_BUCKET_ENTRIES = 32'000'000;
constexpr float MATCH_EPSILON = 0.000001f;
constexpr float VERIFY_BUCKET_WIDTH = 60.f;

struct Interval {
    float min = 0.f;
    float max = 0.f;
    bool minTrail = false;
    bool maxTrail = false;
};

struct Run {
    float yMin = 0.f;
    float yMax = 0.f;
    float xStart = 0.f;
};

float edgeGuard(float a, float b) {
    return std::max(std::abs(a), std::abs(b)) * 2.f *
           std::numeric_limits<float>::epsilon();
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void mergeIntervals(std::vector<Interval>& intervals) {
    if (intervals.empty()) return;

    std::sort(
        intervals.begin(), intervals.end(),
        [](Interval const& a, Interval const& b) { return a.min < b.min; });

    size_t merged = 0;
    for (size_t i = 1; i < intervals.size(); i++) {
        if (intervals[i].min <= intervals[merged].max) {
            if (intervals[i].max > intervals[merged].max) {
                intervals[merged].max = intervals[i].max;
                intervals[merged].maxTrail = intervals[i].maxTrail;
            } else if (intervals[i].max == intervals[merged].max) {
                intervals[merged].maxTrail =
                    intervals[merged].maxTrail || intervals[i].maxTrail;
            }
        } else {
            intervals[++merged] = intervals[i];
        }
    }
    intervals.resize(merged + 1);
}

bool clipNonNegative(float f0, float f1, float& t0, float& t1) {
    float const d = f1 - f0;
    if (std::abs(d) <= std::numeric_limits<float>::min()) return f0 >= 0.f;

    float const cross = -f0 / d;
    if (d > 0.f) {
        t0 = std::max(t0, cross);
    } else {
        t1 = std::min(t1, cross);
    }
    return t0 <= t1;
}

std::vector<RectF> mergeSolidRuns(std::vector<RectF> const& solids) {
    std::vector<RectF> runs = solids;
    std::sort(runs.begin(), runs.end(), [](RectF const& a, RectF const& b) {
        if (a.minY != b.minY) return a.minY < b.minY;
        if (a.maxY != b.maxY) return a.maxY < b.maxY;
        return a.minX < b.minX;
    });

    size_t merged = 0;
    for (size_t i = 1; i < runs.size(); i++) {
        auto& into = runs[merged];
        if (runs[i].minY == into.minY && runs[i].maxY == into.maxY &&
            runs[i].minX <= into.maxX) {
            into.maxX = std::max(into.maxX, runs[i].maxX);
        } else {
            runs[++merged] = runs[i];
        }
    }
    if (!runs.empty()) runs.resize(merged + 1);

    return runs;
}

}  // namespace

std::vector<Segment> buildSegments(std::vector<Sample> const& samples,
                                   bool sweep) {
    std::vector<Segment> segments;
    segments.reserve(samples.size());

    for (size_t i = 0; i < samples.size(); i++) {
        Segment seg{samples[i].rect, samples[i].rect, samples[i].frame};
        if (sweep && i + 1 < samples.size() && !samples[i + 1].startsRun) {
            seg.b = samples[i + 1].rect;
        }
        segments.push_back(seg);
    }

    return segments;
}

bool sweptSpan(Segment const& seg, float x0, float x1, float& yMin,
               float& yMax) {
    float const halfW = seg.halfWidth();
    float const halfH = seg.halfHeight();

    float const cxA = seg.a.centerX(), cxB = seg.b.centerX();
    float const cyA = seg.a.centerY(), cyB = seg.b.centerY();

    float t0 = 0.f, t1 = 1.f;

    if (!clipNonNegative(cxA + halfW - x0, cxB + halfW - x0, t0, t1)) {
        return false;
    }
    if (!clipNonNegative(x1 - (cxA - halfW), x1 - (cxB - halfW), t0, t1)) {
        return false;
    }

    float const cy0 = lerp(cyA, cyB, t0), cy1 = lerp(cyA, cyB, t1);
    yMin = std::min(cy0, cy1) - halfH;
    yMax = std::max(cy0, cy1) + halfH;
    return true;
}

GenResult generate(std::vector<Segment> const& segments,
                   std::vector<RectF> const& solids, float blockW,
                   float blockH, GenSettings const& s) {
    GenResult result;
    if (segments.empty() || blockW <= 0.f || blockH <= 0.f) return result;

    float const columnWidth = std::max(0.0001f, s.columnWidth);
    float const minSize = std::max(0.000001f, s.minBlockSize);
    float const tol = std::max(MATCH_EPSILON, s.mergeTolerance);

    std::vector<Segment> keepOut;
    keepOut.reserve(segments.size());

    float boundsMinX = std::numeric_limits<float>::max();
    float boundsMaxX = std::numeric_limits<float>::lowest();

    for (auto const& seg : segments) {
        keepOut.push_back(Segment{seg.a.expanded(s.gap), seg.b.expanded(s.gap),
                                  seg.frame, seg.bandSource});
        boundsMinX = std::min(boundsMinX, keepOut.back().minX());
        boundsMaxX = std::max(boundsMaxX, keepOut.back().maxX());
    }

    auto const columnCount64 = static_cast<int64_t>(std::ceil(
                                   (boundsMaxX - boundsMinX) / columnWidth)) +
                               1;
    if (columnCount64 <= 0 || columnCount64 > MAX_COLUMNS) {
        result.areaTooLarge = true;
        return result;
    }
    auto const columnCount = static_cast<size_t>(columnCount64);
    result.columns = static_cast<int>(columnCount);

    size_t entries = 0;
    for (auto const& seg : keepOut) {
        entries +=
            static_cast<size_t>((seg.maxX() - seg.minX()) / columnWidth) + 1;
        if (entries > MAX_BUCKET_ENTRIES) {
            result.areaTooLarge = true;
            return result;
        }
    }

    std::vector<std::vector<uint32_t>> buckets(columnCount);
    for (size_t i = 0; i < keepOut.size(); i++) {
        auto const& seg = keepOut[i];
        auto lo = static_cast<int64_t>(
            std::floor((seg.minX() - boundsMinX) / columnWidth));
        auto hi = static_cast<int64_t>(
            std::floor((seg.maxX() - boundsMinX) / columnWidth));
        lo = std::max<int64_t>(lo, 0);
        hi = std::min<int64_t>(hi, columnCount64 - 1);
        for (int64_t c = lo; c <= hi; c++) {
            buckets[static_cast<size_t>(c)].push_back(static_cast<uint32_t>(i));
        }
    }

    std::vector<Interval> gates;
    if (s.frameInterval > 1 && s.gateWidth > 0.f) {
        auto const interval = static_cast<uint32_t>(s.frameInterval);
        for (auto const& seg : segments) {
            if (!seg.bandSource) continue;
            if (seg.frame % interval != 0) continue;
            float const cx = seg.a.centerX();
            gates.push_back(Interval{cx - s.gateWidth * 0.5f,
                                     cx + s.gateWidth * 0.5f, false, false});
        }
        mergeIntervals(gates);

        if (gates.empty()) return result;
    }
    size_t gateIndex = 0;

    auto const solidRuns = mergeSolidRuns(solids);

    std::vector<std::vector<uint32_t>> solidBuckets(columnCount);
    for (size_t i = 0; i < solidRuns.size(); i++) {
        auto const& r = solidRuns[i];
        auto lo = static_cast<int64_t>(
            std::ceil((r.minX - boundsMinX) / columnWidth));
        auto hi = static_cast<int64_t>(
                      std::floor((r.maxX - boundsMinX) / columnWidth)) -
                  1;
        lo = std::max<int64_t>(lo, 0);
        hi = std::min<int64_t>(hi, columnCount64 - 1);
        for (int64_t c = lo; c <= hi; c++) {
            solidBuckets[static_cast<size_t>(c)].push_back(
                static_cast<uint32_t>(i));
        }
    }

    float const maxSpanX = blockW * s.maxScale;
    float const maxSpanY = blockH * s.maxScale;

    std::vector<Run> active;
    std::vector<Run> nextActive;
    std::vector<Interval> blocked;
    std::vector<Interval> free;
    bool limitHit = false;

    auto const emit = [&](Run const& run, float xEnd) {
        if (limitHit) return;

        float const guardX = edgeGuard(run.xStart, xEnd);
        float const guardY = edgeGuard(run.yMin, run.yMax);

        float const x0 = run.xStart + guardX, x1 = xEnd - guardX;
        float const y0 = run.yMin + guardY, y1 = run.yMax - guardY;

        float const w = x1 - x0;
        float const h = y1 - y0;
        if (w < minSize || h < minSize) return;

        int const cols = std::max(1, static_cast<int>(std::ceil(w / maxSpanX)));
        int const rows = std::max(1, static_cast<int>(std::ceil(h / maxSpanY)));
        if (cols > 1 || rows > 1) result.splits++;

        float const pieceW = w / static_cast<float>(cols);
        float const pieceH = h / static_cast<float>(rows);

        for (int cx = 0; cx < cols; cx++) {
            for (int cy = 0; cy < rows; cy++) {
                if (static_cast<int>(result.blocks.size()) >= s.maxObjects) {
                    limitHit = true;
                    result.hitObjectLimit = true;
                    return;
                }
                result.blocks.push_back(
                    GenBlock{x0 + pieceW * (static_cast<float>(cx) + 0.5f),
                             y0 + pieceH * (static_cast<float>(cy) + 0.5f),
                             pieceW, pieceH});
            }
        }
    };

    auto const closeAll = [&](float xEnd) {
        for (auto const& run : active) emit(run, xEnd);
        active.clear();
    };

    float const wallFrom = boundsMinX + std::max(0.f, s.startTrim);
    float const wallTo = boundsMaxX - std::max(0.f, s.endTrim);

    for (size_t c = 0; c < columnCount && !limitHit; c++) {
        float const colStart = boundsMinX + static_cast<float>(c) * columnWidth;

        auto const& bucket = buckets[c];
        if (bucket.empty() || colStart < wallFrom || colStart > wallTo) {
            closeAll(colStart);
            continue;
        }

        if (!gates.empty()) {
            while (gateIndex < gates.size() && gates[gateIndex].max < colStart) {
                gateIndex++;
            }
            if (gateIndex >= gates.size() || colStart < gates[gateIndex].min) {
                closeAll(colStart);
                continue;
            }
        }

        blocked.clear();
        float bandLo = std::numeric_limits<float>::max();
        float bandHi = std::numeric_limits<float>::lowest();
        for (auto const index : bucket) {
            float yMin = 0.f, yMax = 0.f;
            if (sweptSpan(keepOut[index], colStart, colStart + columnWidth,
                          yMin, yMax)) {
                blocked.push_back(Interval{yMin, yMax, true, true});
                if (keepOut[index].bandSource) {
                    bandLo = std::min(bandLo, yMin);
                    bandHi = std::max(bandHi, yMax);
                }
            }
        }
        if (blocked.empty() || bandLo > bandHi) {
            closeAll(colStart);
            continue;
        }

        mergeIntervals(blocked);

        float const bandMin = bandLo - s.fillRadius;
        float const bandMax = bandHi + s.fillRadius;

        if (!solidBuckets[c].empty()) {
            for (auto const index : solidBuckets[c]) {
                float const lo = std::max(solidRuns[index].minY, bandMin);
                float const hi = std::min(solidRuns[index].maxY, bandMax);
                if (hi > lo) blocked.push_back(Interval{lo, hi, false, false});
            }
            mergeIntervals(blocked);
        }

        free.clear();
        float cursor = bandMin;
        bool cursorIsTrail = false;

        auto const addFree = [&](float lo, float hi) {
            lo = std::max(lo, bandMin);
            hi = std::min(hi, bandMax);
            if (hi > lo) free.push_back(Interval{lo, hi});
        };

        auto const addFreeBetween = [&](float lo, float hi, bool loIsTrail,
                                        bool hiIsTrail) {
            if (loIsTrail && hiIsTrail && hi - lo > s.fillRadius * 2.f) {
                addFree(lo, lo + s.fillRadius);
                addFree(hi - s.fillRadius, hi);
                return;
            }
            addFree(lo, hi);
        };

        for (auto const& b : blocked) {
            if (b.min > cursor && (cursorIsTrail || b.minTrail)) {
                addFreeBetween(cursor, b.min, cursorIsTrail, b.minTrail);
            }
            if (b.max > cursor) {
                cursor = b.max;
                cursorIsTrail = b.maxTrail;
            }
        }
        if (bandMax > cursor && cursorIsTrail) addFree(cursor, bandMax);
        result.gapsFound += static_cast<int>(free.size());

        nextActive.clear();
        size_t ai = 0;
        for (auto const& interval : free) {
            while (ai < active.size() && active[ai].yMin < interval.min - tol) {
                emit(active[ai], colStart);
                ai++;
            }

            bool const matches =
                ai < active.size() &&
                std::abs(active[ai].yMin - interval.min) <= tol &&
                std::abs(active[ai].yMax - interval.max) <= tol;

            if (matches) {
                Run carried = active[ai];
                carried.yMin = std::max(carried.yMin, interval.min);
                carried.yMax = std::min(carried.yMax, interval.max);

                if (carried.yMax - carried.yMin >= minSize) {
                    nextActive.push_back(carried);
                    ai++;
                    continue;
                }

                emit(active[ai], colStart);
                ai++;
            }

            nextActive.push_back(Run{interval.min, interval.max, colStart});
        }
        for (; ai < active.size(); ai++) emit(active[ai], colStart);

        active = nextActive;
    }

    closeAll(boundsMinX + static_cast<float>(columnCount) * columnWidth);
    return result;
}

int fitToScaleSteps(std::vector<GenBlock>& blocks,
                    std::vector<Segment> const& segments, float blockW,
                    float blockH) {
    if (blocks.empty() || blockW <= 0.f || blockH <= 0.f) return 0;

    float boundsMinX = std::numeric_limits<float>::max();
    float boundsMaxX = std::numeric_limits<float>::lowest();
    for (auto const& seg : segments) {
        boundsMinX = std::min(boundsMinX, seg.minX());
        boundsMaxX = std::max(boundsMaxX, seg.maxX());
    }

    size_t const bucketCount =
        segments.empty()
            ? 1
            : static_cast<size_t>(std::max(
                  1.0f, std::ceil((boundsMaxX - boundsMinX) /
                                  VERIFY_BUCKET_WIDTH) +
                            1.0f));
    auto const bucketOf = [&](float x) {
        auto i = static_cast<int64_t>((x - boundsMinX) / VERIFY_BUCKET_WIDTH);
        return static_cast<size_t>(
            std::clamp<int64_t>(i, 0, static_cast<int64_t>(bucketCount) - 1));
    };

    std::vector<std::vector<uint32_t>> buckets(bucketCount);
    for (size_t i = 0; i < segments.size(); i++) {
        for (size_t b = bucketOf(segments[i].minX());
             b <= bucketOf(segments[i].maxX()); b++)
            buckets[b].push_back(static_cast<uint32_t>(i));
    }

    auto const clearance = [&](RectF const& r) {
        float nearest = std::numeric_limits<float>::max();
        if (segments.empty()) return nearest;
        for (size_t b = bucketOf(r.minX); b <= bucketOf(r.maxX); b++) {
            for (auto const index : buckets[b]) {
                auto const& seg = segments[index];
                float yMin = 0.f, yMax = 0.f;
                float gap = 0.f;
                if (sweptSpan(seg, r.minX, r.maxX, yMin, yMax))
                    gap = std::max(yMin - r.maxY, r.minY - yMax);
                else
                    gap = std::max(seg.minX() - r.maxX, r.minX - seg.maxX());
                nearest = std::min(nearest, gap);
            }
        }
        return nearest;
    };

    auto const stepDown = [](float span, float unit) {
        return std::floor(span / unit / SCALE_STEP) * SCALE_STEP * unit;
    };

    int dropped = 0;
    std::vector<GenBlock> fitted;
    fitted.reserve(blocks.size());

    for (auto const& block : blocks) {
        float const w = stepDown(block.w, blockW);
        float const h = stepDown(block.h, blockH);
        if (w <= 0.f || h <= 0.f) {
            dropped++;
            continue;
        }

        float const minX = block.x - block.w * 0.5f;
        float const minY = block.y - block.h * 0.5f;
        float const xs[2] = {minX + w * 0.5f, minX + block.w - w * 0.5f};
        float const ys[2] = {minY + h * 0.5f, minY + block.h - h * 0.5f};

        GenBlock best{xs[0], ys[0], w, h};
        float bestGap = std::numeric_limits<float>::max();
        for (float x : xs) {
            for (float y : ys) {
                float const gap =
                    clearance(RectF::fromCenter(x, y, w, h));
                if (gap < bestGap) {
                    bestGap = gap;
                    best = GenBlock{x, y, w, h};
                }
            }
        }
        fitted.push_back(best);
    }

    blocks = std::move(fitted);
    return dropped;
}

VerifyResult verify(std::vector<Segment> const& segments,
                    std::vector<RectF> const& placed) {
    VerifyResult result;
    result.blocksChecked = placed.size();
    if (segments.empty() || placed.empty()) return result;

    float boundsMinX = std::numeric_limits<float>::max();
    float boundsMaxX = std::numeric_limits<float>::lowest();
    for (auto const& seg : segments) {
        boundsMinX = std::min(boundsMinX, seg.minX());
        boundsMaxX = std::max(boundsMaxX, seg.maxX());
    }

    auto const bucketCount = static_cast<size_t>(
        std::max(1.0f, std::ceil((boundsMaxX - boundsMinX) /
                                 VERIFY_BUCKET_WIDTH) +
                           1.0f));

    auto const bucketOf = [&](float x) {
        auto i = static_cast<int64_t>((x - boundsMinX) / VERIFY_BUCKET_WIDTH);
        return static_cast<size_t>(
            std::clamp<int64_t>(i, 0, static_cast<int64_t>(bucketCount) - 1));
    };

    std::vector<std::vector<uint32_t>> buckets(bucketCount);
    for (size_t i = 0; i < segments.size(); i++) {
        size_t const lo = bucketOf(segments[i].minX());
        size_t const hi = bucketOf(segments[i].maxX());
        for (size_t b = lo; b <= hi; b++) {
            buckets[b].push_back(static_cast<uint32_t>(i));
        }
    }

    float tightest = std::numeric_limits<float>::max();

    for (auto const& block : placed) {
        size_t const lo = bucketOf(block.minX);
        size_t const hi = bucketOf(block.maxX);

        bool violated = false;
        float blockWorst = 0.f;
        uint32_t blockFrame = 0;
        float blockClearance = std::numeric_limits<float>::max();

        for (size_t b = lo; b <= hi; b++) {
            for (auto const index : buckets[b]) {
                auto const& seg = segments[index];

                float yMin = 0.f, yMax = 0.f;
                if (!sweptSpan(seg, block.minX, block.maxX, yMin, yMax)) {
                    if (!violated) {
                        float const gapX = std::max(seg.minX() - block.maxX,
                                                    block.minX - seg.maxX());
                        blockClearance =
                            std::min(blockClearance, std::max(0.f, gapX));
                    }
                    continue;
                }

                float const oy =
                    std::min(block.maxY, yMax) - std::max(block.minY, yMin);

                if (oy > 0.f) {
                    float const depth =
                        std::min(oy, block.maxX - block.minX);
                    if (!violated || depth > blockWorst) blockWorst = depth;
                    if (!violated || seg.frame < blockFrame) {
                        blockFrame = seg.frame;
                    }
                    violated = true;
                } else if (!violated) {
                    blockClearance = std::min(blockClearance, -oy);
                }
            }
        }

        if (violated) {
            result.violations++;
            result.worstOverlap = std::max(result.worstOverlap, blockWorst);
            if (result.violations == 1 ||
                blockFrame < result.firstViolationFrame) {
                result.firstViolationFrame = blockFrame;
            }
        } else {
            tightest = std::min(tightest, blockClearance);
        }
    }

    if (tightest != std::numeric_limits<float>::max()) {
        result.tightestClearance = tightest;
    }

    return result;
}

}  // namespace tbuf

// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#include "spikes.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/UndoObject.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

#include "builder.hpp"

using namespace geode::prelude;

namespace tbuf {

namespace {

constexpr double CELL = 64.0;
constexpr double SLACK = 1e-6;
constexpr size_t MAX_PLACED = 20000;

enum Side { Below, Above, Left, Right };
constexpr float ROTATION[4] = {0.f, 180.f, 90.f, 270.f};

struct Box {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;

    static Box of(RectF const& r) { return {r.minX, r.minY, r.maxX, r.maxY}; }
    static Box of(CCRect const& r) {
        return {r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()};
    }

    double w() const { return maxX - minX; }
    double h() const { return maxY - minY; }
    double midX() const { return (minX + maxX) * 0.5; }
    double midY() const { return (minY + maxY) * 0.5; }
    bool usable() const {
        return std::isfinite(minX) && std::isfinite(minY) &&
               std::isfinite(maxX) && std::isfinite(maxY) && w() > 0.0 &&
               h() > 0.0;
    }
    bool operator==(Box const&) const = default;
};

double separation(Box const& a, Box const& b) {
    return std::max({b.minX - a.maxX, a.minX - b.maxX, b.minY - a.maxY,
                     a.minY - b.maxY});
}

bool insideOf(Box const& a, Box const& solid) {
    return a.minX < solid.maxX && solid.minX < a.maxX && a.minY < solid.maxY &&
           solid.minY < a.maxY;
}

double floatStep(double v) {
    float const f = static_cast<float>(v);
    return static_cast<double>(
               std::nextafter(f, std::numeric_limits<float>::infinity())) -
           static_cast<double>(f);
}

struct Grid {
    std::vector<Box> boxes;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    std::vector<uint32_t> wide;

    static int64_t cell(double v) {
        return static_cast<int64_t>(std::floor(v / CELL));
    }
    static uint64_t key(int64_t x, int64_t y) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(y);
    }

    void add(Box const& b) {
        auto const index = static_cast<uint32_t>(boxes.size());
        boxes.push_back(b);

        int64_t const x0 = cell(b.minX), x1 = cell(b.maxX);
        int64_t const y0 = cell(b.minY), y1 = cell(b.maxY);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) > 256) {
            wide.push_back(index);
            return;
        }
        for (int64_t y = y0; y <= y1; y++)
            for (int64_t x = x0; x <= x1; x++)
                cells[key(x, y)].push_back(index);
    }

    template <class F>
    void visit(double x0, double y0, double x1, double y1, F&& each) const {
        for (uint32_t i : wide) each(boxes[i]);
        for (int64_t y = cell(y0); y <= cell(y1); y++) {
            for (int64_t x = cell(x0); x <= cell(x1); x++) {
                auto const it = cells.find(key(x, y));
                if (it == cells.end()) continue;
                for (uint32_t i : it->second) each(boxes[i]);
            }
        }
    }
};

struct Shape {
    double w = 0.0, h = 0.0, offX = 0.0, offY = 0.0;
};

std::optional<Shape> measure(LevelEditorLayer* editor, int id, float rotation) {
    auto* probe = editor->createObject(id, {15.f, 15.f}, true);
    if (!probe) return std::nullopt;

    probe->setRotation(rotation);
    probe->updateStartValues();
    probe->m_isObjectRectDirty = true;

    CCPoint const at = probe->getPosition();
    Box box = Box::of(probe->getObjectRect());
    double const radius = static_cast<double>(probe->m_objectRadius) *
                          std::max(probe->m_scaleX, probe->m_scaleY);
    editor->removeObject(probe, true);

    box = {box.minX - at.x, box.minY - at.y, box.maxX - at.x, box.maxY - at.y};
    if (radius > 0.0)
        box = {std::min(box.minX, -radius), std::min(box.minY, -radius),
               std::max(box.maxX, radius), std::max(box.maxY, radius)};
    if (!box.usable()) return std::nullopt;

    return Shape{box.w(), box.h(), box.midX(), box.midY()};
}

std::optional<double> nearestFree(double target, double lo, double hi,
                                  std::vector<std::pair<double, double>>& blocked) {
    if (lo > hi) return std::nullopt;
    std::sort(blocked.begin(), blocked.end());

    std::optional<double> best;
    auto consider = [&](double from, double to) {
        double const c = std::clamp(target, from, to);
        if (!best || std::abs(c - target) < std::abs(*best - target))
            best = c;
    };

    double cursor = lo;
    for (auto const& [from, to] : blocked) {
        if (cursor > hi) break;
        if (to < cursor) continue;
        if (from > cursor) consider(cursor, std::min(from, hi));
        cursor = std::max(cursor, to);
    }
    if (cursor <= hi) consider(cursor, hi);
    return best;
}

std::optional<Box> fitOnSide(Box const& frame, int side, Shape const& shape,
                             double gap, Grid const& trail,
                             Grid const& solids) {
    bool const vertical = side == Below || side == Above;

    double fixedLo = 0.0, fixedHi = 0.0;
    switch (side) {
        case Below: fixedHi = frame.minY - gap; fixedLo = fixedHi - shape.h; break;
        case Above: fixedLo = frame.maxY + gap; fixedHi = fixedLo + shape.h; break;
        case Left: fixedHi = frame.minX - gap; fixedLo = fixedHi - shape.w; break;
        case Right: fixedLo = frame.maxX + gap; fixedHi = fixedLo + shape.w; break;
    }
    if (fixedLo < 0.0) return std::nullopt;

    double const target = vertical ? frame.midX() : frame.midY();
    double const half = (vertical ? shape.w : shape.h) * 0.5;
    double const frameHalf = (vertical ? frame.w() : frame.h()) * 0.5;
    double const nudge = 1e-4 + std::abs(target) * std::ldexp(1.0, -21);
    double const reach = frameHalf + half + gap + 2.0 * nudge;
    double const lo = std::max(target - reach, half);
    double const hi = target + reach;

    auto fixedMin = [&](Box const& b) { return vertical ? b.minY : b.minX; };
    auto fixedMax = [&](Box const& b) { return vertical ? b.maxY : b.maxX; };
    auto freeMin = [&](Box const& b) { return vertical ? b.minX : b.minY; };
    auto freeMax = [&](Box const& b) { return vertical ? b.maxX : b.maxY; };
    auto area = [&](Grid const& grid, double pad, auto&& each) {
        if (vertical)
            grid.visit(lo - half - pad, fixedLo - pad, hi + half + pad,
                       fixedHi + pad, each);
        else
            grid.visit(fixedLo - pad, lo - half - pad, fixedHi + pad,
                       hi + half + pad, each);
    };

    std::vector<std::pair<double, double>> blocked;
    area(trail, gap, [&](Box const& b) {
        if (fixedMin(b) - fixedHi >= gap - SLACK) return;
        if (fixedLo - fixedMax(b) >= gap - SLACK) return;
        blocked.emplace_back(freeMin(b) - gap - half - nudge,
                             freeMax(b) + gap + half + nudge);
    });
    area(solids, 0.0, [&](Box const& b) {
        if (!(fixedMin(b) < fixedHi && fixedLo < fixedMax(b))) return;
        blocked.emplace_back(freeMin(b) - half - nudge,
                             freeMax(b) + half + nudge);
    });

    auto const centre = nearestFree(target, lo, hi, blocked);
    if (!centre) return std::nullopt;

    Box const spot = vertical
                         ? Box{*centre - half, fixedLo, *centre + half, fixedHi}
                         : Box{fixedLo, *centre - half, fixedHi, *centre + half};

    bool clear = true;
    trail.visit(spot.minX - gap, spot.minY - gap, spot.maxX + gap,
               spot.maxY + gap, [&](Box const& b) {
                   if (separation(spot, b) < gap - SLACK) clear = false;
               });
    solids.visit(spot.minX, spot.minY, spot.maxX, spot.maxY, [&](Box const& b) {
        if (insideOf(spot, b)) clear = false;
    });
    if (!clear) return std::nullopt;
    return spot;
}

std::vector<size_t> pickFrames(std::vector<Box> const& frames,
                               std::vector<uint32_t> const& frameAt,
                               std::vector<uint32_t> const& clickFrames,
                               SpikeSettings const& s) {
    std::vector<char> wanted(frames.size(), 0);

    if (s.aroundClicks) {
        size_t const radius = static_cast<size_t>(s.clickRadius);
        for (uint32_t click : clickFrames) {
            auto const it = std::lower_bound(frameAt.begin(), frameAt.end(),
                                             click + 1);
            if (it == frameAt.end()) continue;
            size_t const at = static_cast<size_t>(it - frameAt.begin());
            size_t const from = at >= radius ? at - radius : 0;
            size_t const to = std::min(at + radius, frames.size() - 1);
            std::fill(wanted.begin() + from, wanted.begin() + to + 1, 1);
        }
    } else {
        size_t const nth = static_cast<size_t>(s.everyNth);
        for (size_t i = 0; i < frames.size(); i += nth) wanted[i] = 1;
        wanted.back() = 1;
    }

    std::vector<size_t> picked;
    Box const* previous = nullptr;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!wanted[i]) continue;
        if (previous && *previous == frames[i]) continue;
        previous = &frames[i];
        picked.push_back(i);
    }
    return picked;
}

}  // namespace

SpikeReport placeSpikes(LevelEditorLayer* editor,
                        std::vector<Sample> const& chosen,
                        std::vector<Sample> const& other,
                        std::vector<uint32_t> const& clickFrames,
                        SpikeSettings const& input) {
    SpikeReport report;
    auto refuse = [&](std::string message) {
        report.message = std::move(message);
        return report;
    };

    if (!editor || !editor->m_editorUI) return refuse("Open the editor first.");
    if (input.objectId <= 0) return refuse("Object ID must be positive.");
    if (std::none_of(input.sides.begin(), input.sides.end(),
                     [](bool on) { return on; }))
        return refuse("Allow at least one side.");
    if (chosen.empty()) return refuse("Nothing recorded for that player.");
    if (input.aroundClicks && clickFrames.empty())
        return refuse("The loaded macro has no clicks for that player.");

    SpikeSettings s = input;
    s.gap = std::clamp(s.gap, MIN_SPIKE_GAP, MAX_SPIKE_GAP);
    s.everyNth = std::clamp(s.everyNth, 1, MAX_EVERY_NTH);
    s.clickRadius = std::clamp(s.clickRadius, 0, MAX_CLICK_RADIUS);

    std::array<Shape, 4> shapes;
    for (int side = 0; side < 4; side++) {
        auto const shape = measure(editor, s.objectId, ROTATION[side]);
        if (!shape)
            return refuse(fmt::format(
                "Object {} can't be created or has no hitbox.", s.objectId));
        shapes[side] = *shape;
    }

    std::vector<Box> frames;
    std::vector<uint32_t> frameAt;
    Grid trail;
    double farthest = 0.0;
    RectF extent{std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};

    for (auto const* stream : {&chosen, &other}) {
        for (auto const& sample : *stream) {
            Box const b = Box::of(sample.rect);
            if (!b.usable()) continue;
            trail.add(b);
            extent = extent.unionWith(sample.rect);
            farthest = std::max({farthest, std::abs(b.minX), std::abs(b.maxX),
                                 std::abs(b.minY), std::abs(b.maxY)});
            if (stream == &chosen) {
                frames.push_back(b);
                frameAt.push_back(sample.frame);
            }
        }
    }
    if (frames.empty()) return refuse("The recording has no usable hitboxes.");

    double const gap = std::max<double>(s.gap, 8.0 * floatStep(farthest));

    Grid solids;
    for (auto const& r : collectSolids(editor, extent.expanded(90.f)))
        solids.add(Box::of(r));

    auto const picked = pickFrames(frames, frameAt, clickFrames, s);
    report.tried = static_cast<int>(picked.size());
    if (picked.size() > MAX_PLACED)
        return refuse(fmt::format(
            "{} frames would get a spike, over the {} limit. {}",
            picked.size(), MAX_PLACED,
            s.aroundClicks ? "Lower Frames Around Each Click."
                           : "Raise Every Nth Frame."));

    auto* placed = CCArray::create();
    std::array<int, 4> perSide{};

    for (size_t i : picked) {
        std::optional<Box> spot;
        int side = 0;
        for (; side < 4 && !spot; side++) {
            if (!s.sides[side]) continue;
            spot = fitOnSide(frames[i], side, shapes[side], gap, trail,
                             solids);
        }
        if (!spot) {
            report.noRoom++;
            continue;
        }
        side--;

        Shape const& shape = shapes[side];
        CCPoint const pos{static_cast<float>(spot->midX() - shape.offX),
                          static_cast<float>(spot->midY() - shape.offY)};

        auto* object = editor->createObject(s.objectId, pos, true);
        if (!object) break;

        object->setRotation(ROTATION[side]);
        if (editor->m_currentLayer >= 0)
            object->m_editorLayer = editor->m_currentLayer;
        object->updateStartValues();
        object->m_isObjectRectDirty = true;

        CCRect const actual = object->getObjectRect();
        Box const got = Box::of(actual);

        bool touches = false;
        trail.visit(got.minX, got.minY, got.maxX, got.maxY, [&](Box const& b) {
            if (separation(got, b) <= 0.0) touches = true;
        });
        if (touches) {
            editor->removeObject(object, true);
            report.removed++;
            continue;
        }

        placed->addObject(object);
        perSide[side]++;
    }

    report.placed = static_cast<int>(placed->count());
    if (report.placed == 0)
        return refuse(fmt::format(
            "No room next to any of the {} frames. Allow more sides or lower "
            "the gap.",
            report.tried));

    if (editor->m_undoObjects)
        editor->m_undoObjects->addObject(
            UndoObject::createWithArray(placed, UndoCommand::Paste));
    if (editor->m_redoObjects) editor->m_redoObjects->removeAllObjects();

    auto* ui = editor->m_editorUI;
    ui->deselectAll();
    ui->selectObjects(placed, true);
    ui->updateButtons();
    ui->updateObjectInfoLabel();

    report.ok = true;
    report.message = fmt::format(
        "Placed {} of {} frames ({} with no room{}), {:.4g} clear of the "
        "hitbox. {}/{}/{}/{} below/above/left/right. Ctrl+Z undoes it.",
        report.placed, report.tried, report.noRoom,
        report.removed ? fmt::format(", {} removed for touching", report.removed)
                       : "",
        gap, perSide[Below], perSide[Above], perSide[Left], perSide[Right]);

    log::info("[trailbuf] spikes: {}", report.message);
    return report;
}

}  // namespace tbuf

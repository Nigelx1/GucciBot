// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#include "trailbuf.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include "analysis/ac/shim.hpp"  // GucciBot: was Silicate's bot/settings/value headers
#include "spikes.hpp"

using namespace geode::prelude;

static constexpr size_t MAX_TICKS_PER_STREAM = 1'000'000;

static constexpr float INNER_HITBOX_SCALE = 0.3f;

static tbuf::RectF toRectF(cocos2d::CCRect const& r) {
    return tbuf::RectF{r.origin.x, r.origin.y, r.origin.x + r.size.width,
                       r.origin.y + r.size.height};
}

void TrailBuffer::clear() {
    for (auto& stream : m_streams) stream.clear();
    m_hasLast = {false, false};
    m_firstFrame = 0;
    m_lastFrame = 0;
    m_startedRecording = false;
    m_dashSamples = 0;
    m_sourceLevel.clear();
}

tbuf::RectF TrailBuffer::playerRect(PlayerObject* player) const {
    return toRectF(m_useInnerHitbox->inner()
                       ? player->getObjectRect(INNER_HITBOX_SCALE,
                                               INNER_HITBOX_SCALE)
                       : player->getObjectRect());
}

void TrailBuffer::saveSpiderDash(PlayerObject* player, cocos2d::CCPoint from,
                                 cocos2d::CCPoint to) {
    if (!m_enabled->inner() || !player) return;

    auto* pl = GJBaseGameLayer::get();
    if (!pl || pl->m_levelEndAnimationStarted) return;
    if (!PlayLayer::get()) return;

    int const slot = player == pl->m_player2 ? 1 : 0;
    if (slot == 1 && !pl->m_gameState.m_isDualMode) return;
    if (slot == 0 && player != pl->m_player1) return;
    if (m_streams[slot].size() + 2 >= MAX_TICKS_PER_STREAM) return;

    tbuf::RectF const rect = this->playerRect(player);
    cocos2d::CCPoint const at = player->getPosition();
    uint32_t const frame = Bot::get()->updater().getFrame();

    auto const moved = [&](cocos2d::CCPoint p) {
        return tbuf::RectF{rect.minX + (p.x - at.x), rect.minY + (p.y - at.y),
                           rect.maxX + (p.x - at.x), rect.maxY + (p.y - at.y)};
    };

    if (!m_startedRecording) {
        m_startedRecording = true;
        m_firstFrame = frame;
        if (pl->m_level) m_sourceLevel = pl->m_level->m_levelName;
    }

    this->append(slot, moved(from), frame);
    this->append(slot, moved(to), frame);
    m_dashSamples += 2;
    m_lastFrame = frame;
}

void TrailBuffer::saveTick(GJBaseGameLayer* pl) {
    if (!m_enabled->inner()) return;
    if (!pl || pl->m_levelEndAnimationStarted) return;

    if (!PlayLayer::get()) return;

    if (m_pendingReset) {
        this->clear();
        m_pendingReset = false;
    }

    uint32_t const frame = Bot::get()->updater().getFrame();
    this->record(pl, pl->m_player1, 0, frame);
    this->record(pl, pl->m_player2, 1, frame);
}

void TrailBuffer::saveCollision(GJBaseGameLayer* pl, PlayerObject* player) {
    if (!m_enabled->inner()) return;
    if (!pl || pl->m_levelEndAnimationStarted || !PlayLayer::get()) return;
    if (m_pendingReset) return;

    int const slot = player == pl->m_player2 ? 1 : 0;
    if (slot == 0 && player != pl->m_player1) return;
    this->record(pl, player, slot, Bot::get()->updater().getFrame());
}

void TrailBuffer::reportKill(PlayerObject* player, GameObject* killer) {
    int const id = killer->m_objectID;
    if (id != m_objectId->inner() && id != m_objectIdP2->inner() &&
        id != m_spikeObjectId->inner())
        return;

    if (auto* pl = PlayLayer::get(); pl && killer == pl->m_anticheatSpike)
        return;

    auto const box = [](cocos2d::CCRect const& r) {
        return fmt::format("[{:.5f}, {:.5f}]..[{:.5f}, {:.5f}]", r.getMinX(),
                           r.getMinY(), r.getMaxX(), r.getMaxY());
    };

    cocos2d::CCRect const them = killer->getObjectRect();
    cocos2d::CCRect const me = player->getObjectRect();
    float const overlapX = std::min(me.getMaxX(), them.getMaxX()) -
                           std::max(me.getMinX(), them.getMinX());
    float const overlapY = std::min(me.getMaxY(), them.getMaxY()) -
                           std::max(me.getMinY(), them.getMinY());

    log::warn(
        "[trailbuf] killed at frame {} by object {} at ({:.5f}, {:.5f}) scale "
        "{:.6f} x {:.6f}: object {} player {} overlap x {:.5f} y {:.5f}",
        Bot::get()->updater().getFrame(), id, killer->getPositionX(),
        killer->getPositionY(), killer->m_scaleX, killer->m_scaleY, box(them),
        box(me), overlapX, overlapY);
}

void TrailBuffer::record(GJBaseGameLayer* pl, PlayerObject* player, int slot,
                         uint32_t frame) {
    if (!player) return;
    if (slot == 1 && !pl->m_gameState.m_isDualMode) return;

    auto const rect = this->playerRect(player);
    if (m_hasLast[slot] && m_lastRect[slot] == rect) return;
    if (m_streams[slot].size() >= MAX_TICKS_PER_STREAM) return;

    if (!m_startedRecording) {
        m_startedRecording = true;
        m_firstFrame = frame;
        if (pl->m_level) m_sourceLevel = pl->m_level->m_levelName;
    }

    this->append(slot, rect, frame);
    m_lastFrame = frame;
}

void TrailBuffer::append(int slot, tbuf::RectF const& rect, uint32_t frame) {
    bool startsRun = !m_hasLast[slot];
    if (!startsRun) {
        float const dx = rect.centerX() - m_lastRect[slot].centerX();
        if (std::abs(dx) > m_breakDistance->inner()) startsRun = true;
    }

    m_streams[slot].push_back(tbuf::Sample{rect, frame, startsRun});
    m_hasLast[slot] = true;
    m_lastRect[slot] = rect;
}

std::vector<tbuf::Sample> TrailBuffer::flatten() const {
    std::vector<tbuf::Sample> samples;
    samples.reserve(this->tickCount());
    for (auto const& stream : m_streams) {
        samples.insert(samples.end(), stream.begin(), stream.end());
    }
    return samples;
}

void TrailBuffer::loadSamples(std::vector<tbuf::Sample> const& p1,
                              std::vector<tbuf::Sample> const& p2) {
    this->clear();
    m_streams[0] = p1;
    m_streams[1] = p2;
    m_pendingReset = false;
    m_startedRecording = this->tickCount() != 0;

    if (!m_streams[0].empty()) {
        m_firstFrame = m_streams[0].front().frame;
        m_lastFrame = m_streams[0].back().frame;
    }
}

TrailBuffer::Report TrailBuffer::placeSpikes(LevelEditorLayer* editor) {
    Report report;

    if (m_useInnerHitbox->inner()) {
        report.message =
            "Spikes kill on the full hitbox. Turn off Wall Against Inner "
            "Hitbox and record again.";
        return report;
    }

    bool const player2 = m_spikePlayer2->inner();
    int const slot = player2 ? 1 : 0;

    std::vector<uint32_t> clicks;
    if (m_spikeAroundClicks->inner()) {
        auto& rs = Bot::get()->replaySystem();
        bool const twoPlayer = editor && editor->m_levelSettings &&
                               editor->m_levelSettings->m_twoPlayerMode;
        for (auto const& a : rs.m_actionAtom.m_actions) {
            if (a.m_type != slc::ActionType::Jump) continue;
            if (!a.m_holding && !m_spikeReleases->inner()) continue;
            if (twoPlayer && rs.playerFlipped(a.m_player2) != player2) continue;
            clicks.push_back(static_cast<uint32_t>(a.m_frame));
        }
        std::sort(clicks.begin(), clicks.end());
    }

    tbuf::SpikeSettings settings;
    settings.objectId = m_spikeObjectId->inner();
    settings.gap = m_spikeGap->inner();
    settings.sides = {m_spikeBelow->inner(), m_spikeAbove->inner(),
                      m_spikeLeft->inner(), m_spikeRight->inner()};
    settings.everyNth = m_spikeEveryNth->inner();
    settings.aroundClicks = m_spikeAroundClicks->inner();
    settings.clickRadius = m_spikeClickRadius->inner();

    auto const result = tbuf::placeSpikes(editor, m_streams[slot],
                                          m_streams[1 - slot], clicks, settings);
    report.ok = result.ok;
    report.verified = result.ok;
    report.placed = result.placed;
    report.message = result.message;
    return report;
}

TrailBuffer::Report TrailBuffer::generate(LevelEditorLayer* editor) {
    Report report;

    if (!editor) {
        report.message = "Open the editor to generate.";
        return report;
    }
    if (this->tickCount() == 0) {
        report.message = "Nothing recorded. Play the level for real first.";
        return report;
    }

    bool const split = m_separatePlayers->inner() && !m_streams[1].empty();

    tbuf::GenSettings settings;
    settings.gap = m_gap->inner();
    settings.fillRadius = std::max(1.f, m_fillRadius->inner());
    settings.columnWidth = m_columnWidth->inner();
    settings.minBlockSize = m_minBlockSize->inner();
    settings.maxScale = m_maxScale->inner();
    settings.maxObjects = m_maxObjects->inner();
    settings.frameInterval = m_frameInterval->inner();
    settings.gateWidth = m_gateWidth->inner();
    settings.mergeTolerance = m_mergeTolerance->inner();
    settings.startTrim = m_startTrim->inner();
    settings.endTrim = m_endTrim->inner();

    bool const sweep = m_sweepBetweenTicks->inner();

    std::vector<tbuf::Segment> segments =
        tbuf::buildSegments(m_streams[0], sweep);
    size_t const p1Count = segments.size();
    {
        auto const p2 = tbuf::buildSegments(m_streams[1], sweep);
        segments.insert(segments.end(), p2.begin(), p2.end());
    }

    size_t const samples = this->tickCount();

    std::vector<tbuf::RectF> solids;
    if (m_skipSolids->inner()) {
        tbuf::RectF area{std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};
        for (auto const& seg : segments) {
            area.minX = std::min(area.minX, seg.minX());
            area.maxX = std::max(area.maxX, seg.maxX());
            area.minY = std::min({area.minY, seg.a.minY, seg.b.minY});
            area.maxY = std::max({area.maxY, seg.a.maxY, seg.b.maxY});
        }
        solids = tbuf::collectSolids(
            editor, area.expanded(std::max(0.f, settings.fillRadius)));
    }

    int const passes = split ? 2 : 1;

    std::vector<tbuf::RectF> placedRects;
    int columns = 0, gapsFound = 0, splits = 0, tooThin = 0;
    bool hitObjectLimit = false;

    for (int pass = 0; pass < passes; pass++) {
        int const objectId =
            pass == 1 ? m_objectIdP2->inner() : m_objectId->inner();

        settings.maxObjects =
            std::max(0, m_maxObjects->inner() - report.placed);
        if (settings.maxObjects == 0) {
            hitObjectLimit = true;
            break;
        }

        auto const shape = tbuf::measureObject(editor, objectId);

        for (size_t i = 0; i < segments.size(); i++) {
            segments[i].bandSource =
                !split || ((i < p1Count) == (pass == 0));
        }

        auto result = tbuf::generate(segments, solids, shape.width,
                                     shape.height, settings);
        tooThin += tbuf::fitToScaleSteps(result.blocks, segments, shape.width,
                                         shape.height);

        if (result.areaTooLarge) {
            report.message =
                "Recording covers too much area. Raise column width.";
            return report;
        }
        if (result.blocks.empty()) continue;

        auto const placement =
            tbuf::placeBlocks(editor, objectId, shape, result.blocks);
        if (!placement.ok) {
            report.message = placement.message;
            return report;
        }

        placedRects.insert(placedRects.end(), placement.placedRects.begin(),
                           placement.placedRects.end());
        report.placed += placement.placed;
        columns = std::max(columns, result.columns);
        gapsFound += result.gapsFound;
        splits += result.splits;
        hitObjectLimit = hitObjectLimit || result.hitObjectLimit;
    }

    if (placedRects.empty()) {
        report.message = "No gaps to fill. Raise fill radius.";
        return report;
    }

    for (auto& seg : segments) seg.bandSource = true;
    auto const check = tbuf::verify(segments, placedRects);

    report.verified = check.violations == 0;
    report.ok = true;
    report.message = fmt::format(
        "Placed {} blocks from {} ticks ({} columns, {} gaps, {} split, {} "
        "under GD's 0.001 scale step).\n{}{}",
        report.placed, samples, columns, gapsFound, splits, tooThin,
        report.verified
            ? fmt::format("Verified beatable: 0 of {} blocks touch the trail, "
                          "closest approach {:.4f}.",
                          check.blocksChecked, check.tightestClearance)
            : fmt::format("IMPOSSIBLE: {} of {} blocks overlap the trail by up "
                          "to {:.4f} (first at frame {}).",
                          check.violations, check.blocksChecked,
                          check.worstOverlap, check.firstViolationFrame),
        hitObjectLimit ? "\nHit the object limit." : "");

    if (report.verified) {
        log::info("[trailbuf] {}", report.message);
    } else {
        log::error("[trailbuf] {}", report.message);
    }
    return report;
}

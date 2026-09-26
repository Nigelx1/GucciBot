// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#ifndef TRAILBUF_TRAILBUF_HPP
#define TRAILBUF_TRAILBUF_HPP

#include <cocos2d.h>

#include <array>
#include <string>
#include <vector>

#include "builder.hpp"
#include "generator.hpp"
#include "analysis/ac/shim.hpp"  // GucciBot: was Silicate's bot/settings/value headers

class GJBaseGameLayer;
class GameObject;
class LevelEditorLayer;
class PlayerObject;

class TrailBuffer {
   public:
    struct Report {
        bool ok = false;
        bool verified = false;
        int placed = 0;
        std::string message;
    };

    void saveTick(GJBaseGameLayer* pl);
    void saveCollision(GJBaseGameLayer* pl, PlayerObject* player);
    void reportKill(PlayerObject* player, GameObject* killer);

    void saveSpiderDash(PlayerObject* player, cocos2d::CCPoint from,
                        cocos2d::CCPoint to);

    void clear();

    void beginAttempt() { m_pendingReset = true; }

    Report generate(LevelEditorLayer* editor);
    Report placeSpikes(LevelEditorLayer* editor);

    std::vector<tbuf::Sample> flatten() const;
    void loadSamples(std::vector<tbuf::Sample> const& p1,
                     std::vector<tbuf::Sample> const& p2);
    std::vector<tbuf::Sample> const& stream(int slot) const {
        return m_streams[slot];
    }

    size_t tickCount() const {
        return m_streams[0].size() + m_streams[1].size();
    }
    uint32_t firstFrame() const { return m_firstFrame; }
    uint32_t dashSamples() const { return m_dashSamples; }
    uint32_t lastFrame() const { return m_lastFrame; }
    std::string const& sourceLevel() const { return m_sourceLevel; }

    SLValuePtr<bool> m_enabled = SLValue<bool>::create(
        "trailbuf.enabled", &SLSettings::get()->trailBuffer.enabled);
    SLValuePtr<int> m_objectId = SLValue<int>::create(
        "trailbuf.object_id", &SLSettings::get()->trailBuffer.objectId);
    SLValuePtr<float> m_gap = SLValue<float>::create(
        "trailbuf.gap", &SLSettings::get()->trailBuffer.gap);
    SLValuePtr<bool> m_useInnerHitbox = SLValue<bool>::create(
        "trailbuf.inner_hitbox",
        &SLSettings::get()->trailBuffer.useInnerHitbox);
    SLValuePtr<float> m_fillRadius = SLValue<float>::create(
        "trailbuf.fill_radius", &SLSettings::get()->trailBuffer.fillRadius);
    SLValuePtr<float> m_columnWidth = SLValue<float>::create(
        "trailbuf.column_width", &SLSettings::get()->trailBuffer.columnWidth);
    SLValuePtr<float> m_minBlockSize = SLValue<float>::create(
        "trailbuf.min_block_size",
        &SLSettings::get()->trailBuffer.minBlockSize);
    SLValuePtr<float> m_maxScale = SLValue<float>::create(
        "trailbuf.max_scale", &SLSettings::get()->trailBuffer.maxScale);
    SLValuePtr<bool> m_sweepBetweenTicks = SLValue<bool>::create(
        "trailbuf.sweep", &SLSettings::get()->trailBuffer.sweepBetweenTicks);
    SLValuePtr<int> m_maxObjects = SLValue<int>::create(
        "trailbuf.max_objects", &SLSettings::get()->trailBuffer.maxObjects);
    SLValuePtr<bool> m_skipSolids = SLValue<bool>::create(
        "trailbuf.skip_solids", &SLSettings::get()->trailBuffer.skipSolids);
    SLValuePtr<int> m_frameInterval = SLValue<int>::create(
        "trailbuf.frame_interval",
        &SLSettings::get()->trailBuffer.frameInterval);
    SLValuePtr<float> m_gateWidth = SLValue<float>::create(
        "trailbuf.gate_width", &SLSettings::get()->trailBuffer.gateWidth);
    SLValuePtr<float> m_mergeTolerance = SLValue<float>::create(
        "trailbuf.merge_tolerance",
        &SLSettings::get()->trailBuffer.mergeTolerance);
    SLValuePtr<bool> m_separatePlayers = SLValue<bool>::create(
        "trailbuf.separate_players",
        &SLSettings::get()->trailBuffer.separatePlayers);
    SLValuePtr<int> m_objectIdP2 = SLValue<int>::create(
        "trailbuf.object_id_p2", &SLSettings::get()->trailBuffer.objectIdP2);
    SLValuePtr<float> m_breakDistance = SLValue<float>::create(
        "trailbuf.break_distance",
        &SLSettings::get()->trailBuffer.breakDistance);
    SLValuePtr<float> m_startTrim = SLValue<float>::create(
        "trailbuf.start_trim", &SLSettings::get()->trailBuffer.startTrim);
    SLValuePtr<float> m_endTrim = SLValue<float>::create(
        "trailbuf.end_trim", &SLSettings::get()->trailBuffer.endTrim);

    SLValuePtr<bool> m_spikePlayer2 = SLValue<bool>::create(
        "trailbuf.spike_player2", &SLSettings::get()->trailBuffer.spikePlayer2);
    SLValuePtr<int> m_spikeObjectId = SLValue<int>::create(
        "trailbuf.spike_object_id",
        &SLSettings::get()->trailBuffer.spikeObjectId);
    SLValuePtr<float> m_spikeGap = SLValue<float>::create(
        "trailbuf.spike_gap", &SLSettings::get()->trailBuffer.spikeGap);
    SLValuePtr<bool> m_spikeBelow = SLValue<bool>::create(
        "trailbuf.spike_below", &SLSettings::get()->trailBuffer.spikeBelow);
    SLValuePtr<bool> m_spikeAbove = SLValue<bool>::create(
        "trailbuf.spike_above", &SLSettings::get()->trailBuffer.spikeAbove);
    SLValuePtr<bool> m_spikeLeft = SLValue<bool>::create(
        "trailbuf.spike_left", &SLSettings::get()->trailBuffer.spikeLeft);
    SLValuePtr<bool> m_spikeRight = SLValue<bool>::create(
        "trailbuf.spike_right", &SLSettings::get()->trailBuffer.spikeRight);
    SLValuePtr<int> m_spikeEveryNth = SLValue<int>::create(
        "trailbuf.spike_every_nth",
        &SLSettings::get()->trailBuffer.spikeEveryNth);
    SLValuePtr<bool> m_spikeAroundClicks = SLValue<bool>::create(
        "trailbuf.spike_around_clicks",
        &SLSettings::get()->trailBuffer.spikeAroundClicks);
    SLValuePtr<int> m_spikeClickRadius = SLValue<int>::create(
        "trailbuf.spike_click_radius",
        &SLSettings::get()->trailBuffer.spikeClickRadius);
    SLValuePtr<bool> m_spikeReleases = SLValue<bool>::create(
        "trailbuf.spike_releases",
        &SLSettings::get()->trailBuffer.spikeReleases);

   private:
    void append(int slot, tbuf::RectF const& rect, uint32_t frame);
    void record(GJBaseGameLayer* pl, PlayerObject* player, int slot,
                uint32_t frame);
    tbuf::RectF playerRect(PlayerObject* player) const;

    std::array<std::vector<tbuf::Sample>, 2> m_streams;
    std::array<tbuf::RectF, 2> m_lastRect{};
    std::array<bool, 2> m_hasLast{false, false};

    uint32_t m_firstFrame = 0;
    uint32_t m_lastFrame = 0;
    bool m_startedRecording = false;
    uint32_t m_dashSamples = 0;
    bool m_pendingReset = false;

    std::string m_sourceLevel;
};

#endif  // TRAILBUF_TRAILBUF_HPP

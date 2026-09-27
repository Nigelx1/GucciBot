#pragma once

// Live sub-tick recording -- anticroom's SCBF (replay/scbf_input.*, "slc
// count" drop, 2026-09-26), ported 2026-09-27.
//
// GD stamps every button press with the OS time it happened
// (PlayerButtonCommand::m_timestamp, from the input event). The recorder keeps
// a running map from wall-clock time to tick time -- re-anchored each frame,
// and whenever the game stalls or pauses -- so a press can be placed at the
// tick and the fraction of a tick it really arrived in. processQueuedButtons
// then holds each press until its tick comes round and lets the CBF engine
// fire it that far into the physics step. The offset is saved with the input
// (gb::Action::m_subtick) and playback splits the step at the same point.
//
// Off unless CBF Recording is on. With Tick Splitting off, inputs record on
// the tick edge as usual; the recorder only counts them.

#include <Geode/binding/GJBaseGameLayer.hpp>

#include <cstdint>
#include <vector>

namespace scbf {

    // QueryPerformanceCounter seconds -- the clock GD's input timestamps use.
    double clockSeconds();
    // A real, recent press with a usable timestamp (not one GucciBot or the
    // replay queued with a zero stamp).
    bool isLivePress(PlayerButtonCommand const& cmd);

    struct Due {
        PlayerButtonCommand cmd;
        uint32_t frame = 0;
        double offset = 0.0;
    };

    class LiveRecorder {
    public:
        static LiveRecorder& get();

        bool recording() const;
        bool splitting() const;

        void beginFrame(uint32_t frame, bool paused);
        void endFrame(uint32_t frame);
        void desync() { m_synced = false; }

        void defer(gd::vector<PlayerButtonCommand>& queue, uint32_t frame);
        std::vector<Due> takeDue(uint32_t frame);

        void passThrough(gd::vector<PlayerButtonCommand>& queue);

        void notePlaced() { m_placed++; }
        void noteAligned(size_t count = 1) { m_aligned += static_cast<uint32_t>(count); }

        void reset();

        uint32_t placed() const { return m_placed; }
        uint32_t aligned() const { return m_aligned; }

    private:
        void place(Due& due, uint32_t frame) const;

        std::vector<Due> m_waiting;

        bool m_synced = false;
        bool m_paused = false;
        double m_anchorTime = 0.0;
        double m_anchorTicks = 0.0;
        double m_frameTime = 0.0;
        double m_rate = 240.0;

        uint32_t m_placed = 0;
        uint32_t m_aligned = 0;
    };

} // namespace scbf

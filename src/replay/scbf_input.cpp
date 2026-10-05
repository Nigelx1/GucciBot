// anticroom's SCBF live recorder, ported 2026-09-27. Logic is his; the changes
// are GucciBot's settings and accessors in place of Silicate's. See the header.

#include "replay/scbf_input.hpp"

#include <Geode/Geode.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif
#include <chrono>

#include <algorithm>
#include <cmath>

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/shim.hpp"
#include "core/GucciBot.hpp"
#include "replay/subtick_preview.hpp"

using namespace geode::prelude;

namespace scbf {

    namespace {

        constexpr double MIN_OFFSET = 1e-7;
        // A frame longer than this re-anchors the clock instead of stretching
        // the last anchor across a hitch.
        constexpr double STALL_SECONDS = 0.25;
        constexpr double MAX_STAMP_AGE = 1.0;
        constexpr double MAX_STAMP_LEAD = 0.05;

    } // namespace

    double clockSeconds() {
#ifdef _WIN32
        static double const freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();

        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / freq;
#else
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }

    bool isLivePress(PlayerButtonCommand const& cmd) {
        double const now = clockSeconds();
        return cmd.m_timestamp > 0.0 && cmd.m_timestamp >= now - MAX_STAMP_AGE &&
               cmd.m_timestamp <= now + MAX_STAMP_LEAD;
    }

    LiveRecorder& LiveRecorder::get() {
        static LiveRecorder instance;
        return instance;
    }

    bool LiveRecorder::recording() const {
        auto* gb = gucci::GucciEngine::get();
        // Input FPS moves presses to whole input ticks, which is the opposite
        // of what this does -- it wins when both are on, as in his.
        return gb->replay.m_scbfRecording && gb->enabled && gb->isRecording() &&
               PlayLayer::get() && !::Bot::get()->frameWindow().running() &&
               !gb->updater.inputFpsActive();
    }

    bool LiveRecorder::splitting() const {
        return this->recording() && gucci::GucciEngine::get()->replay.m_scbfTickSplit;
    }

    void LiveRecorder::beginFrame(uint32_t frame, bool paused) {
        auto& updater = gucci::GucciEngine::get()->updater;

        m_paused = paused;
        m_rate = updater.m_tps * updater.m_speedhack * std::max(1.f, updater.getTimeWarp());

        double const now = clockSeconds();
        m_frameTime = now;

        bool const stalled = m_synced && now - m_anchorTime > STALL_SECONDS;
        if (!m_synced || paused || stalled) {
            m_anchorTime = now;
            m_anchorTicks = frame;
            m_synced = !paused;
        }
    }

    void LiveRecorder::endFrame(uint32_t frame) {
        if (!m_synced || m_paused)
            return;

        double ticks = m_anchorTicks + (m_frameTime - m_anchorTime) * m_rate;
        ticks = std::clamp(
            ticks, static_cast<double>(frame), static_cast<double>(frame) + 1.0 - MIN_OFFSET);

        m_anchorTicks = ticks;
        m_anchorTime = m_frameTime;
    }

    void LiveRecorder::place(Due& due, uint32_t frame) const {
        due.frame = frame;
        due.offset = 0.0;
        if (!isLivePress(due.cmd))
            return;

        if (m_paused) {
            // Frame-advance: the press lands where the sub-tick preview is
            // stepped to (0, the tick edge, when it is off or not stepped in).
            due.offset = SubtickPreview::get().fraction();
            return;
        }
        if (!m_synced)
            return;

        double ticks = m_anchorTicks + (due.cmd.m_timestamp - m_anchorTime) * m_rate;
        ticks = std::clamp(ticks, static_cast<double>(frame), static_cast<double>(frame) + 2.0);

        double const whole = std::floor(ticks);
        due.frame = static_cast<uint32_t>(whole);
        due.offset = ticks - whole;
        if (due.offset < MIN_OFFSET)
            due.offset = 0.0;
    }

    void LiveRecorder::defer(gd::vector<PlayerButtonCommand>& queue, uint32_t frame) {
        if (queue.empty())
            return;

        for (auto const& cmd : queue) {
            Due due{cmd};
            this->place(due, frame);
            m_waiting.push_back(due);

            log::debug("[scbf] {} b{}{} at f{} -> f{} + {:.4f}",
                       cmd.m_isPush ? "press" : "release",
                       static_cast<int>(cmd.m_button),
                       cmd.m_isPlayer2 ? " p2" : "",
                       frame,
                       due.frame,
                       due.offset);
        }
        queue.clear();

        std::stable_sort(m_waiting.begin(), m_waiting.end(), [](Due const& a, Due const& b) {
            if (a.frame != b.frame)
                return a.frame < b.frame;
            return a.offset < b.offset;
        });
    }

    std::vector<Due> LiveRecorder::takeDue(uint32_t frame) {
        auto const end = std::find_if(
            m_waiting.begin(), m_waiting.end(), [frame](Due const& d) { return d.frame > frame; });
        std::vector<Due> due(m_waiting.begin(), end);
        m_waiting.erase(m_waiting.begin(), end);

        // Anything overdue (the frame went past it -- a stall, a reset) fires
        // now, on the edge, rather than never.
        for (auto& d : due) {
            if (d.frame == frame)
                continue;
            d.frame = frame;
            d.offset = 0.0;
        }
        return due;
    }

    void LiveRecorder::passThrough(gd::vector<PlayerButtonCommand>& queue) {
        this->noteAligned(queue.size() + m_waiting.size());
        if (m_waiting.empty())
            return;

        gd::vector<PlayerButtonCommand> merged;
        merged.reserve(m_waiting.size() + queue.size());
        for (auto const& d : m_waiting)
            merged.push_back(d.cmd);
        // Element by element: Geode's gnustl gd::vector (Android) has no range insert.
        for (auto const& cmd : queue)
            merged.push_back(cmd);
        queue = std::move(merged);
        m_waiting.clear();
    }

    void LiveRecorder::reset() {
        m_waiting.clear();
        m_synced = false;
        m_placed = 0;
        m_aligned = 0;
    }

} // namespace scbf

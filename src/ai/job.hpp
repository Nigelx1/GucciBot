#pragma once

// The AI pilot's long work, run on the main thread in slices (stage A,
// 2026-11-02). A job does a slice of work once a drawn frame, from the
// drawScene hook next to the search's slice (src/core/engine_updater.cpp),
// for at most sliceMs (10 ms by default), so the game stays responsive.
// Assistant Access queues a job and polls it (gucci_ai_status): every call
// returns at once. One job runs at a time (there is one pair of copies); the
// rest wait in order.

#include <chrono>
#include <memory>
#include <string>

#include <matjson.hpp>

namespace gucci::ai::job {

    using Clock = std::chrono::steady_clock;

    struct Job {
        virtual ~Job() = default;
        virtual char const* kind() const = 0;
        // One slice, to stop by `until`. True when the job is over: `result`
        // is set, or `error` when it failed. Exceptions fail the job.
        virtual bool slice(Clock::time_point until) = 0;
        // Stopped before it finished (cancelled, the level left, an error):
        // put back everything it changed. Called at most once, never after a
        // slice that returned true.
        virtual void cancel() = 0;
        // Whether it holds or drives the real game right now (the realtime
        // cap, the World's ledger after every real tick).
        virtual bool holdsGame() const { return false; }

        std::string progress;
        matjson::Value result;
        std::string error;
    };

    // Queues a job; its id for status().
    int submit(std::unique_ptr<Job> job);
    // {state: queued|running|done|failed, kind, progress, result | error};
    // throws std::out_of_range for an unknown id.
    matjson::Value status(int id);
    // Stops a queued or running job (it fails with "cancelled"). False when
    // the id is unknown or already over.
    bool cancel(int id);

    // A job queued or running (Assistant Access's runOwner() says "ai").
    bool busy();
    // The running job holds or drives the real game.
    bool holdsGame();

    // Once a drawn frame, before the frame is drawn: a slice of the running
    // job. Returns the milliseconds it took (0 when nothing ran).
    double tick();

    // The slice budget, ms a frame (default 10, clamped to 1-100).
    double sliceMs();
    void setSliceMs(double ms);

} // namespace gucci::ai::job

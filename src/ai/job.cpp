#include "ai/job.hpp"

#include <Geode/loader/Log.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <map>
#include <stdexcept>

namespace gucci::ai::job {

    namespace {
        enum class State { Queued, Running, Done, Failed };

        struct Entry {
            int id = 0;
            State state = State::Queued;
            std::unique_ptr<Job> job;
            std::string kind;
            Clock::time_point started{};
            double ms = 0.0;  // working, all slices together
            int slices = 0;
        };

        std::map<int, Entry> s_jobs;
        std::deque<int> s_queue;
        int s_running = 0;
        int s_nextId = 1;
        double s_sliceMs = 10.0;
        constexpr size_t kKeepFinished = 32;

        char const* stateName(State s) {
            switch (s) {
                case State::Queued: return "queued";
                case State::Running: return "running";
                case State::Done: return "done";
                case State::Failed: return "failed";
            }
            return "?";
        }

        void forgetOld() {
            size_t finished = 0;
            for (auto const& [id, e] : s_jobs)
                if (e.state == State::Done || e.state == State::Failed) finished++;
            for (auto it = s_jobs.begin(); it != s_jobs.end() && finished > kKeepFinished;) {
                if (it->second.state == State::Done || it->second.state == State::Failed) {
                    it = s_jobs.erase(it);
                    finished--;
                } else {
                    ++it;
                }
            }
        }

        void fail(Entry& e, std::string const& why, bool cancelIt) {
            if (cancelIt && e.job) {
                try {
                    e.job->cancel();
                } catch (...) {
                }
            }
            if (e.job && e.job->error.empty()) e.job->error = why;
            e.state = State::Failed;
            geode::log::info("[AI] job {} ({}) failed: {}", e.id, e.kind, e.job ? e.job->error : why);
        }
    }  // namespace

    int submit(std::unique_ptr<Job> job) {
        forgetOld();
        Entry e;
        e.id = s_nextId++;
        e.kind = job->kind();
        e.job = std::move(job);
        int const id = e.id;
        s_jobs.emplace(id, std::move(e));
        s_queue.push_back(id);
        geode::log::info("[AI] job {} ({}) queued", id, s_jobs[id].kind);
        return id;
    }

    matjson::Value status(int id) {
        auto it = s_jobs.find(id);
        if (it == s_jobs.end()) throw std::out_of_range("no such job");
        Entry const& e = it->second;
        auto out = matjson::Value::object();
        out["job"] = id;
        out["kind"] = e.kind;
        out["state"] = stateName(e.state);
        if (e.state == State::Queued) {
            int ahead = 0;
            for (int q : s_queue) {
                if (q == id) break;
                ahead++;
            }
            out["queued_behind"] = ahead + (s_running ? 1 : 0);
        }
        out["progress"] = e.job ? e.job->progress : "";
        out["work_ms"] = e.ms;
        out["slices"] = e.slices;
        if (e.state == State::Done) out["result"] = e.job->result;
        if (e.state == State::Failed) {
            out["error"] = e.job->error;
            if (!e.job->result.isNull()) out["result"] = e.job->result;  // what it had got to
        }
        return out;
    }

    bool cancel(int id) {
        auto it = s_jobs.find(id);
        if (it == s_jobs.end()) return false;
        Entry& e = it->second;
        if (e.state == State::Queued) {
            std::erase(s_queue, id);
            fail(e, "cancelled", false);
            return true;
        }
        if (e.state == State::Running) {
            fail(e, "cancelled", true);
            s_running = 0;
            return true;
        }
        return false;
    }

    bool busy() {
        return s_running != 0 || !s_queue.empty();
    }

    bool holdsGame() {
        if (!s_running) return false;
        auto it = s_jobs.find(s_running);
        return it != s_jobs.end() && it->second.job && it->second.job->holdsGame();
    }

    double tick() {
        if (!s_running) {
            if (s_queue.empty()) return 0.0;
            s_running = s_queue.front();
            s_queue.pop_front();
            auto& e = s_jobs[s_running];
            e.state = State::Running;
            e.started = Clock::now();
            geode::log::info("[AI] job {} ({}) running", e.id, e.kind);
        }
        auto it = s_jobs.find(s_running);
        if (it == s_jobs.end()) {
            s_running = 0;
            return 0.0;
        }
        Entry& e = it->second;
        auto const t0 = Clock::now();
        auto const until = t0 + std::chrono::microseconds((long long)(s_sliceMs * 1000.0));
        bool over = false;
        try {
            over = e.job->slice(until);
        } catch (std::exception const& ex) {
            fail(e, std::string("error: ") + ex.what(), true);
            s_running = 0;
        } catch (...) {
            fail(e, "error", true);
            s_running = 0;
        }
        double const ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        e.ms += ms;
        e.slices++;
        if (s_running && over) {
            e.state = e.job->error.empty() ? State::Done : State::Failed;
            geode::log::info("[AI] job {} ({}) {} after {:.1f} ms of work in {} slices", e.id, e.kind,
                             stateName(e.state), e.ms, e.slices);
            s_running = 0;
        }
        return ms;
    }

    double sliceMs() {
        return s_sliceMs;
    }

    void setSliceMs(double ms) {
        s_sliceMs = std::clamp(ms, 1.0, 100.0);
    }

} // namespace gucci::ai::job

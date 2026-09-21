#ifndef ANALYSIS_LSTAR_HPP
#define ANALYSIS_LSTAR_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lstar {

struct Input {
    uint32_t m_frame = 0;
    double m_frames = 1.0;
};

struct Settings {
    double m_tps = 240.0;
    double m_respawnSeconds = 0.0;
    double m_targetSeconds = 86400.0;
    double m_nerve = 0.0;
    double m_fatigue = 0.0;
    double m_cps = 0.0;
    bool m_useNerve = false;
    bool m_useFatigue = false;
    bool m_useCps = false;
};

struct Result {
    bool m_ok = false;
    double m_value = 0.0;
    std::vector<double> m_perInput;
};

class Solver {
   public:
    static Solver* get();

    void start(std::vector<Input> inputs, Settings settings);
    void cancel();

    bool running() const { return m_running.load(); }
    float progress() const { return m_progress.load(); }
    Result const& result() const { return m_result; }
    bool dirty() const { return m_dirty; }
    void markDirty() { m_dirty = true; }

   private:
    Solver() = default;

    std::atomic<bool> m_running{false};
    std::atomic<float> m_progress{0.f};
    std::atomic<int> m_generation{0};
    std::shared_ptr<Result> m_pending;
    Result m_result;
    bool m_dirty = true;
};

}  // namespace lstar

#endif  // ANALYSIS_LSTAR_HPP

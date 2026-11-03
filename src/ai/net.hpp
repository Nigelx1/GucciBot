#pragma once

// The AI pilot's networks in the mod (stage C, 2026-11-02): reads a GBNN v1
// file (AI contract v2 section 7, written by tools/ai/gbai.py) and runs it.
//
// Two kinds of network:
//   mlp      dense layers; one input row of n_in floats -> n_out outputs
//            (the pilot: hold_logit; the critic: alive_hold, alive_release)
//   conv1d   a stack of 1D convolutions over the level tape's columns,
//            same padding with dilation (zeros outside the tape) -> n_out
//            outputs per column (the level reader)
// Inputs are raw: the file's mean and std normalize them first.
//
// Every platform must make the same decisions, so the arithmetic is plain
// float32 in a fixed order on one thread: no fused multiply-add (net.cpp
// turns contraction off), no fast-math, no BLAS. Dense weights are turned
// to [in][out] at load so each output's sum still runs bias, input 0, 1, 2...
// while the inner loop runs across outputs (TOOLCHAIN.md section 2: the
// same bits as the textbook loop, about 5x faster).
//
// No game code in here: the offline test (tools/run_ai_net_tests.cpp)
// builds net.cpp on its own.

#include <cstdint>
#include <string>
#include <vector>

namespace gucci::ai::net {

    enum class Kind : uint32_t { Mlp = 0, Conv1d = 1 };
    enum class Act : uint32_t { None = 0, Relu = 1, Sigmoid = 2, Tanh = 3 };

    struct Layer {
        uint32_t nIn = 0, nOut = 0, kernel = 1, dilation = 1;
        Act act = Act::None;
        // Dense: [in][out]. Conv1d: [kernel][in][out]. (The file stores
        // [out][in][kernel]; turned at load.)
        std::vector<float> w;
        std::vector<float> b;  // [out]
    };

    class Net {
    public:
        // Reads a GBNN from its bytes. expectedHash: the layout hash the
        // caller computes inputs for (16 hex); empty = don't check. On
        // failure returns false and says why in plain words.
        bool load(uint8_t const* data, size_t size, std::string const& expectedHash, std::string& why);
        bool loadFile(std::string const& path, std::string const& expectedHash, std::string& why);

        bool loaded() const { return !m_layers.empty(); }
        Kind kind() const { return m_kind; }
        std::string const& layoutHash() const { return m_hash; }
        uint32_t inputs() const { return (uint32_t)m_mean.size(); }
        uint32_t outputs() const { return m_layers.empty() ? 0 : m_layers.back().nOut; }
        std::vector<std::string> const& outputNames() const { return m_names; }
        std::vector<Layer> const& layers() const { return m_layers; }
        // The output's index by name, or -1.
        int output(std::string const& name) const;

        // MLP: in [inputs()] -> out [outputs()]. Uses this net's scratch
        // buffers, so one call at a time per Net.
        void forward(float const* in, float* out);
        // conv1d: in [columns][inputs()] -> out [columns][outputs()].
        void forwardColumns(float const* in, size_t columns, float* out);

    private:
        Kind m_kind = Kind::Mlp;
        std::string m_hash;
        std::vector<float> m_mean, m_std;
        std::vector<Layer> m_layers;
        std::vector<std::string> m_names;
        std::vector<float> m_a, m_b;  // scratch
    };

    // The decisions (AI contract v2 section 7): hold when the logit is above
    // 0, ties release; the critic prefers holding when alive_hold is above
    // alive_release, ties release.
    inline bool holds(float holdLogit) { return holdLogit > 0.0f; }
    inline bool criticPrefersHold(float aliveHold, float aliveRelease) { return aliveHold > aliveRelease; }

} // namespace gucci::ai::net

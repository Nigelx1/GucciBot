#pragma once

// Self-training for the AI pilot's dense network (2026-11-19, GucciBot 2.0):
// the pilot learns from its own best run on a level. It is plain supervised
// learning on what that run did: binary cross-entropy with logits on one
// output (the pilot's hold_logit) against the action the run took (held 1,
// released 0), in minibatches, with Adam, the minibatch gradient's length
// clipped. An early, weak kind of learning: one level at a time.
//
// Same arithmetic rules as net.cpp: plain float32 in a fixed order on one
// thread, no fused multiply-add (train.cpp turns contraction off), no
// fast-math. Inputs are normalized with the network's own mean and std, the
// forward pass is Net::forward's sum for sum, so the logit trained on is the
// logit the pilot plays with. Sigmoid and tanh use std::tanh as net.cpp does.
//
// Also the GBNN v1 writer, the exact inverse of Net::load: writing a loaded
// file gives back the same bytes.
//
// No game code in here: the offline test (tools/run_ai_net_tests.cpp) builds
// it with net.cpp alone.

#include "ai/net.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gucci::ai::train {

    struct Options {
        float lr = 1e-4f;  // Adam's step size
        float beta1 = 0.9f;
        float beta2 = 0.999f;
        float eps = 1e-8f;
        float clip = 1.0f;  // the minibatch gradient's length (L2, all weights together), at most
    };

    class Trainer {
    public:
        // Trains `n` (not owned: it must outlive the trainer and keep its
        // shapes) on its output `output`. False with `why` for a network it
        // can't train: not a dense one, or no such output.
        bool begin(net::Net* n, int output, Options const& o, std::string& why);
        bool ready() const { return m_net != nullptr; }
        net::Net* net() const { return m_net; }

        // One minibatch step: rows [count][inputs()] raw (unnormalized),
        // targets [count] 0 or 1. Returns the minibatch's mean loss before
        // the step; `wrong` (if given) counts the rows whose decision (hold
        // when the logit is above 0) differed from the target before the step.
        double step(float const* rows, float const* targets, size_t count, size_t* wrong = nullptr);
        // The mean loss over rows, the network unchanged; `wrong` as above.
        double loss(float const* rows, float const* targets, size_t count, size_t* wrong = nullptr);
        // The gradient of the mean loss into gradW()/gradB() (per layer, laid
        // out as Layer::w and Layer::b), the network unchanged. The mean loss.
        double gradient(float const* rows, float const* targets, size_t count);
        std::vector<std::vector<float>> const& gradW() const { return m_gw; }
        std::vector<std::vector<float>> const& gradB() const { return m_gb; }

        uint64_t steps() const { return m_t; }
        // The last step's gradient length before clipping.
        double lastNorm() const { return m_lastNorm; }

    private:
        double accumulate(float const* rows, float const* targets, size_t count, size_t* wrong, bool backward);
        float forwardRow(float const* in);

        net::Net* m_net = nullptr;
        int m_out = 0;
        Options m_opt;
        uint64_t m_t = 0;
        double m_b1t = 1.0, m_b2t = 1.0;  // beta1^t, beta2^t
        double m_lastNorm = 0.0;
        std::vector<std::vector<float>> m_h;  // [layer + 1][width]: the normalized input, then each layer's output
        std::vector<float> m_delta, m_prev;   // the gradient at a layer's sums, and at its inputs
        std::vector<std::vector<float>> m_gw, m_gb, m_mw, m_vw, m_mb, m_vb;
    };

    // Binary cross-entropy with logits, in double (reporting only).
    double bce(float logit, float target);

    // The GBNN v1 bytes of a loaded network (empty when none is loaded).
    std::vector<uint8_t> save(net::Net const& n);
    // FNV-1a 64 of the bytes, as 16 hex digits: which network a file came from.
    std::string fingerprint(std::vector<uint8_t> const& bytes);

} // namespace gucci::ai::train

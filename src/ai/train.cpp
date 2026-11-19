// No fused multiply-add anywhere in this file, on any platform (see net.cpp):
// every platform must train the same weights from the same rows. Never build
// this file with fast-math.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#include "ai/train.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gucci::ai::train {

    namespace {

        float sigmoid(float v) {  // as net.cpp's Sigmoid
            return 0.5f * (1.0f + std::tanh(0.5f * v));
        }

        // net.cpp's activate, the same formulas.
        void activate(float* v, size_t n, net::Act a) {
            switch (a) {
            case net::Act::Relu:
                for (size_t i = 0; i < n; i++) v[i] = v[i] > 0.0f ? v[i] : 0.0f;
                break;
            case net::Act::Sigmoid:
                for (size_t i = 0; i < n; i++) v[i] = 0.5f * (1.0f + std::tanh(0.5f * v[i]));
                break;
            case net::Act::Tanh:
                for (size_t i = 0; i < n; i++) v[i] = std::tanh(v[i]);
                break;
            case net::Act::None: break;
            }
        }

        // The activation's slope, from its output y.
        float slope(net::Act a, float y) {
            switch (a) {
            case net::Act::Relu: return y > 0.0f ? 1.0f : 0.0f;
            case net::Act::Sigmoid: return y * (1.0f - y);
            case net::Act::Tanh: return 1.0f - y * y;
            case net::Act::None: return 1.0f;
            }
            return 1.0f;
        }

        // sum a[o] * b[o] in a fixed order: eight running sums (o mod 8),
        // then added pairwise. Written out, so every compiler adds the same way.
        float dot(float const* a, float const* b, size_t n) {
            float s[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            size_t o = 0;
            for (; o + 8 <= n; o += 8)
                for (size_t k = 0; k < 8; k++) s[k] += a[o + k] * b[o + k];
            for (size_t k = 0; o < n; o++, k++) s[k] += a[o] * b[o];
            return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
        }

        void put32(std::vector<uint8_t>& b, uint32_t v) {  // little-endian, as net.cpp reads
            uint8_t p[4];
            std::memcpy(p, &v, 4);
            b.insert(b.end(), p, p + 4);
        }

        void putFloats(std::vector<uint8_t>& b, float const* v, size_t n) {
            size_t const at = b.size();
            b.resize(at + n * 4);
            if (n) std::memcpy(b.data() + at, v, n * 4);
        }

    }  // namespace

    double bce(float logit, float target) {
        double const z = logit, y = target;
        return std::max(z, 0.0) - z * y + std::log1p(std::exp(-std::fabs(z)));
    }

    bool Trainer::begin(net::Net* n, int output, Options const& o, std::string& why) {
        m_net = nullptr;
        if (!n || !n->loaded()) {
            why = "no network is loaded";
            return false;
        }
        if (n->kind() != net::Kind::Mlp) {
            why = "only a dense network (mlp) can be trained";
            return false;
        }
        if (output < 0 || (uint32_t)output >= n->outputs()) {
            why = "the network has no output " + std::to_string(output);
            return false;
        }
        auto const& L = n->layers();
        m_h.assign(L.size() + 1, {});
        m_h[0].assign(n->inputs(), 0.0f);
        m_gw.assign(L.size(), {});
        m_gb.assign(L.size(), {});
        m_mw.assign(L.size(), {});
        m_vw.assign(L.size(), {});
        m_mb.assign(L.size(), {});
        m_vb.assign(L.size(), {});
        size_t widest = n->inputs();
        for (size_t l = 0; l < L.size(); l++) {
            m_h[l + 1].assign(L[l].nOut, 0.0f);
            m_gw[l].assign(L[l].w.size(), 0.0f);
            m_mw[l].assign(L[l].w.size(), 0.0f);
            m_vw[l].assign(L[l].w.size(), 0.0f);
            m_gb[l].assign(L[l].b.size(), 0.0f);
            m_mb[l].assign(L[l].b.size(), 0.0f);
            m_vb[l].assign(L[l].b.size(), 0.0f);
            widest = std::max<size_t>(widest, L[l].nOut);
        }
        m_delta.reserve(widest);
        m_prev.reserve(widest);
        m_net = n;
        m_out = output;
        m_opt = o;
        m_t = 0;
        m_b1t = m_b2t = 1.0;
        m_lastNorm = 0.0;
        why.clear();
        return true;
    }

    // Net::forward, sum for sum, keeping every layer's output.
    float Trainer::forwardRow(float const* in) {
        auto const& mean = m_net->mean();
        auto const& sd = m_net->stdev();
        auto const& L = m_net->layers();
        float* h = m_h[0].data();
        for (size_t i = 0; i < mean.size(); i++) h[i] = (in[i] - mean[i]) / sd[i];
        for (size_t l = 0; l < L.size(); l++) {
            auto const& Ly = L[l];
            float const* x = m_h[l].data();
            float* y = m_h[l + 1].data();
            size_t const O = Ly.nOut;
            for (size_t o = 0; o < O; o++) y[o] = Ly.b[o];
            float const* w = Ly.w.data();
            for (size_t i = 0; i < Ly.nIn; i++) {
                float const xi = x[i];
                float const* row = w + i * O;
                for (size_t o = 0; o < O; o++) y[o] += row[o] * xi;
            }
            activate(y, O, Ly.act);
        }
        return m_h[L.size()][(size_t)m_out];
    }

    double Trainer::accumulate(float const* rows, float const* targets, size_t count, size_t* wrong, bool backward) {
        auto const& L = m_net->layers();
        size_t const nIn = m_net->inputs();
        size_t const nL = L.size();
        if (backward)
            for (size_t l = 0; l < nL; l++) {
                std::fill(m_gw[l].begin(), m_gw[l].end(), 0.0f);
                std::fill(m_gb[l].begin(), m_gb[l].end(), 0.0f);
            }
        double total = 0.0;
        size_t bad = 0;
        float const inv = count ? 1.0f / (float)count : 0.0f;
        for (size_t r = 0; r < count; r++) {
            float const z = forwardRow(rows + r * nIn);
            float const t = targets[r];
            total += bce(z, t);
            if (net::holds(z) != (t > 0.5f)) bad++;
            if (!backward) continue;
            // The mean loss's gradient at the last layer's sums: (sigmoid(z) - t) / count
            // through its activation on the trained output, 0 on the others.
            auto const& last = L[nL - 1];
            m_delta.assign(last.nOut, 0.0f);
            m_delta[(size_t)m_out] = ((sigmoid(z) - t) * inv) * slope(last.act, z);
            for (size_t l = nL; l-- > 0;) {
                auto const& Ly = L[l];
                size_t const I = Ly.nIn, O = Ly.nOut;
                float const* x = m_h[l].data();
                float const* d = m_delta.data();
                float* gb = m_gb[l].data();
                for (size_t o = 0; o < O; o++) gb[o] += d[o];
                float* gw = m_gw[l].data();
                for (size_t i = 0; i < I; i++) {
                    float const xi = x[i];
                    if (xi == 0.0f) continue;  // adds nothing (ReLU outputs are often 0)
                    float* grow = gw + i * O;
                    for (size_t o = 0; o < O; o++) grow[o] += xi * d[o];
                }
                if (l == 0) break;
                // The gradient at this layer's inputs, through the layer before's activation.
                auto const& before = L[l - 1];
                m_prev.resize(I);
                float const* w = Ly.w.data();
                for (size_t i = 0; i < I; i++) {
                    float const s = slope(before.act, x[i]);
                    m_prev[i] = s == 0.0f ? 0.0f : dot(w + i * O, d, O) * s;
                }
                std::swap(m_delta, m_prev);
            }
        }
        if (wrong) *wrong = bad;
        return count ? total / (double)count : 0.0;
    }

    double Trainer::loss(float const* rows, float const* targets, size_t count, size_t* wrong) {
        if (!m_net) return 0.0;
        return accumulate(rows, targets, count, wrong, false);
    }

    double Trainer::gradient(float const* rows, float const* targets, size_t count) {
        if (!m_net) return 0.0;
        return accumulate(rows, targets, count, nullptr, true);
    }

    double Trainer::step(float const* rows, float const* targets, size_t count, size_t* wrong) {
        if (!m_net || count == 0) return 0.0;
        double const meanLoss = accumulate(rows, targets, count, wrong, true);
        auto& L = m_net->layersForTraining();
        // The gradient's length, all weights together, in a fixed order.
        double sq = 0.0;
        for (size_t l = 0; l < L.size(); l++) {
            for (float g : m_gw[l]) sq += (double)g * (double)g;
            for (float g : m_gb[l]) sq += (double)g * (double)g;
        }
        double const norm = std::sqrt(sq);
        m_lastNorm = norm;
        float const scale = norm > (double)m_opt.clip && norm > 0.0 ? (float)((double)m_opt.clip / norm) : 1.0f;
        m_t++;
        m_b1t *= (double)m_opt.beta1;
        m_b2t *= (double)m_opt.beta2;
        float const lrT = (float)((double)m_opt.lr * std::sqrt(1.0 - m_b2t) / (1.0 - m_b1t));
        float const b1 = m_opt.beta1, b2 = m_opt.beta2, c1 = 1.0f - b1, c2 = 1.0f - b2, eps = m_opt.eps;
        auto adam = [&](std::vector<float>& wv, std::vector<float> const& gv, std::vector<float>& mv, std::vector<float>& vv) {
            float* w = wv.data();
            float const* g = gv.data();
            float* m = mv.data();
            float* v = vv.data();
            for (size_t k = 0; k < wv.size(); k++) {
                float const gk = g[k] * scale;
                m[k] = b1 * m[k] + c1 * gk;
                v[k] = b2 * v[k] + c2 * (gk * gk);
                w[k] -= lrT * (m[k] / (std::sqrt(v[k]) + eps));
            }
        };
        for (size_t l = 0; l < L.size(); l++) {
            adam(L[l].w, m_gw[l], m_mw[l], m_vw[l]);
            adam(L[l].b, m_gb[l], m_mb[l], m_vb[l]);
        }
        return meanLoss;
    }

    std::vector<uint8_t> save(net::Net const& n) {
        std::vector<uint8_t> b;
        if (!n.loaded()) return b;
        auto const& L = n.layers();
        size_t floats = 2 * (size_t)n.inputs();
        for (auto const& Ly : L) floats += Ly.w.size() + Ly.b.size();
        b.reserve(64 + 4 * floats + 24 * L.size());
        char const magic[4] = {'G', 'B', 'N', 'N'};
        b.insert(b.end(), magic, magic + 4);
        put32(b, 1);  // version
        put32(b, (uint32_t)n.kind());
        std::string hash = n.layoutHash();
        hash.resize(16, '0');
        b.insert(b.end(), hash.begin(), hash.end());
        put32(b, n.inputs());
        putFloats(b, n.mean().data(), n.mean().size());
        putFloats(b, n.stdev().data(), n.stdev().size());
        put32(b, (uint32_t)L.size());
        std::vector<float> w;
        for (auto const& Ly : L) {
            put32(b, (uint32_t)n.kind());
            put32(b, Ly.nIn);
            put32(b, Ly.nOut);
            put32(b, Ly.kernel);
            put32(b, Ly.dilation);
            put32(b, (uint32_t)Ly.act);
            // [kernel][in][out] back to the file's [out][in][kernel].
            size_t const I = Ly.nIn, O = Ly.nOut, K = Ly.kernel;
            w.resize(O * I * K);
            for (size_t o = 0; o < O; o++)
                for (size_t i = 0; i < I; i++)
                    for (size_t kk = 0; kk < K; kk++) w[(o * I + i) * K + kk] = Ly.w[(kk * I + i) * O + o];
            putFloats(b, w.data(), w.size());
            putFloats(b, Ly.b.data(), Ly.b.size());
        }
        auto const& names = n.outputNames();
        put32(b, (uint32_t)names.size());
        for (auto const& s : names) {
            put32(b, (uint32_t)s.size());
            b.insert(b.end(), s.begin(), s.end());
        }
        return b;
    }

    std::string fingerprint(std::vector<uint8_t> const& bytes) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint8_t c : bytes) {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        static char const hex[] = "0123456789abcdef";
        std::string s(16, '0');
        for (int i = 15; i >= 0; i--, h >>= 4) s[(size_t)i] = hex[h & 15];
        return s;
    }

} // namespace gucci::ai::train

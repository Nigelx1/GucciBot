// No fused multiply-add anywhere in this file, on any platform: a fused
// a*b+c rounds once instead of twice, so a build that fuses (clang does by
// default on ARM: Mac, iOS, Android) would make different decisions than
// the Windows build. Never build this file with fast-math.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#include "ai/net.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gucci::ai::net {

    namespace {
        constexpr uint32_t kVersion = 1;
        constexpr uint32_t kMaxWidth = 65536;
        constexpr uint32_t kMaxLayers = 4096;
        constexpr uint32_t kMaxKernel = 4096;
        constexpr size_t kMaxFile = 256u << 20;

        struct Reader {
            uint8_t const* p;
            size_t size;
            size_t pos = 0;
            bool take(void* out, size_t n) {
                if (n > size - pos) return false;
                std::memcpy(out, p + pos, n);
                pos += n;
                return true;
            }
            bool u32(uint32_t& v) { return take(&v, 4); }  // little-endian on every platform GD runs on
            bool floats(std::vector<float>& v, size_t n) {
                if (n > (size - pos) / 4) return false;
                v.resize(n);
                return take(v.data(), n * 4);
            }
            size_t left() const { return size - pos; }
        };

        bool allFinite(std::vector<float> const& v) {
            for (float f : v)
                if (!std::isfinite(f)) return false;
            return true;
        }

        bool isHex16(std::string const& s) {
            if (s.size() != 16) return false;
            for (char c : s)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            return true;
        }

        void activate(float* v, size_t n, Act a) {
            switch (a) {
            case Act::Relu:
                for (size_t i = 0; i < n; i++) v[i] = v[i] > 0.0f ? v[i] : 0.0f;
                break;
            case Act::Sigmoid:  // 1/(1+e^-v), written as the trainer's numpy reference does
                for (size_t i = 0; i < n; i++) v[i] = 0.5f * (1.0f + std::tanh(0.5f * v[i]));
                break;
            case Act::Tanh:
                for (size_t i = 0; i < n; i++) v[i] = std::tanh(v[i]);
                break;
            case Act::None: break;
            }
        }
    }  // namespace

    bool Net::load(uint8_t const* data, size_t size, std::string const& expectedHash, std::string& why) {
        *this = Net();
        auto fail = [&](std::string const& w) {
            *this = Net();
            why = w;
            return false;
        };
        if (!data) return fail("no data");
        Reader r{data, size};
        char magic[4];
        uint32_t version = 0, kind = 0, nIn = 0;
        char hash[16];
        if (!r.take(magic, 4) || !r.u32(version) || !r.u32(kind) || !r.take(hash, 16) || !r.u32(nIn))
            return fail("the file is too short to be a network");
        if (std::memcmp(magic, "GBNN", 4) != 0) return fail("not a GBNN network file");
        if (version != kVersion)
            return fail("the network file is version " + std::to_string(version) + ", this build reads version 1");
        if (kind != 0 && kind != 1) return fail("unknown network kind " + std::to_string(kind));
        std::string const fileHash(hash, 16);
        if (!isHex16(fileHash)) return fail("the network's layout hash is not 16 hex characters");
        if (!expectedHash.empty() && fileHash != expectedHash)
            return fail("the network was trained for layout " + fileHash + ", but this build sees layout " +
                        expectedHash + " - it needs weights trained for this build");
        if (nIn < 1 || nIn > kMaxWidth) return fail("the network takes " + std::to_string(nIn) + " inputs");
        std::vector<float> mean, stdv;
        if (!r.floats(mean, nIn) || !r.floats(stdv, nIn)) return fail("the file ends inside the input mean and std");
        if (!allFinite(mean) || !allFinite(stdv)) return fail("the input mean or std holds values that aren't finite");
        for (float s : stdv)
            if (!(s > 0.0f)) return fail("the input std holds a value that isn't above 0");

        uint32_t nLayers = 0;
        if (!r.u32(nLayers)) return fail("the file ends before the layers");
        if (nLayers < 1 || nLayers > kMaxLayers) return fail("the network has " + std::to_string(nLayers) + " layers");
        std::vector<Layer> layers(nLayers);
        uint32_t width = nIn;
        for (uint32_t li = 0; li < nLayers; li++) {
            auto const at = "layer " + std::to_string(li) + ": ";
            uint32_t h[6];
            for (auto& v : h)
                if (!r.u32(v)) return fail(at + "the file ends inside its header");
            uint32_t const type = h[0], in = h[1], out = h[2], k = h[3], d = h[4], act = h[5];
            if (type != kind) return fail(at + (kind == 0 ? "not a dense layer in an mlp" : "not a conv1d layer in a conv1d stack"));
            if (in != width) return fail(at + "takes " + std::to_string(in) + " inputs, the one before gives " + std::to_string(width));
            if (out < 1 || out > kMaxWidth) return fail(at + std::to_string(out) + " outputs");
            if (act > 3) return fail(at + "unknown activation " + std::to_string(act));
            if (type == 0 && (k != 1 || d != 1)) return fail(at + "a dense layer needs kernel 1 and dilation 1");
            if (type == 1 && (k < 1 || k % 2 == 0 || k > kMaxKernel || d < 1 || d > kMaxWidth))
                return fail(at + "a conv1d layer needs an odd kernel and dilation of at least 1");
            uint64_t const count = (uint64_t)out * in * k;
            if (count > r.left() / 4) return fail(at + "its weights run past the end of the file");
            std::vector<float> w, b;
            if (!r.floats(w, (size_t)count) || !r.floats(b, out)) return fail(at + "the file ends inside its weights");
            if (!allFinite(w) || !allFinite(b)) return fail(at + "weights that aren't finite");
            Layer& L = layers[li];
            L.nIn = in;
            L.nOut = out;
            L.kernel = k;
            L.dilation = d;
            L.act = (Act)act;
            // [out][in][kernel] -> [kernel][in][out]: the inner loop runs across outputs.
            L.w.resize((size_t)count);
            for (uint32_t o = 0; o < out; o++)
                for (uint32_t i = 0; i < in; i++)
                    for (uint32_t kk = 0; kk < k; kk++)
                        L.w[((size_t)kk * in + i) * out + o] = w[((size_t)o * in + i) * k + kk];
            L.b = std::move(b);
            width = out;
        }

        uint32_t nNames = 0;
        if (!r.u32(nNames)) return fail("the file ends before the output names");
        if (nNames != width)
            return fail(std::to_string(nNames) + " output names for " + std::to_string(width) + " outputs");
        std::vector<std::string> names(nNames);
        for (auto& n : names) {
            uint32_t len = 0;
            if (!r.u32(len) || len > r.left()) return fail("the file ends inside the output names");
            n.assign((char const*)data + r.pos, len);
            r.pos += len;
            if (n.empty()) return fail("an output name is empty");
            for (unsigned char c : n)
                if (c >= 128) return fail("an output name is not plain ASCII");
        }
        if (r.left() != 0) return fail(std::to_string(r.left()) + " stray bytes after the output names");

        m_kind = (Kind)kind;
        m_hash = fileHash;
        m_mean = std::move(mean);
        m_std = std::move(stdv);
        m_layers = std::move(layers);
        m_names = std::move(names);
        why.clear();
        return true;
    }

    bool Net::loadFile(std::string const& path, std::string const& expectedHash, std::string& why) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            *this = Net();
            why = "can't open " + path;
            return false;
        }
        f.seekg(0, std::ios::end);
        auto const size = (std::streamoff)f.tellg();
        if (size < 0 || (uint64_t)size > kMaxFile) {
            *this = Net();
            why = path + " is too big to be a network";
            return false;
        }
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> data((size_t)size);
        if (size > 0 && !f.read((char*)data.data(), size)) {
            *this = Net();
            why = "can't read " + path;
            return false;
        }
        return load(data.data(), data.size(), expectedHash, why);
    }

    int Net::output(std::string const& name) const {
        for (size_t i = 0; i < m_names.size(); i++)
            if (m_names[i] == name) return (int)i;
        return -1;
    }

    void Net::forward(float const* in, float* out) {
        if (m_kind != Kind::Mlp || m_layers.empty()) return;
        size_t const nIn = m_mean.size();
        size_t widest = nIn;
        for (auto const& L : m_layers) widest = widest > L.nOut ? widest : L.nOut;
        m_a.resize(widest);
        m_b.resize(widest);
        float* h = m_a.data();
        float* y = m_b.data();
        for (size_t i = 0; i < nIn; i++) h[i] = (in[i] - m_mean[i]) / m_std[i];
        for (auto const& L : m_layers) {
            size_t const O = L.nOut;
            for (size_t o = 0; o < O; o++) y[o] = L.b[o];
            float const* w = L.w.data();
            for (size_t i = 0; i < L.nIn; i++) {
                float const xi = h[i];
                float const* row = w + i * O;
                for (size_t o = 0; o < O; o++) y[o] += row[o] * xi;
            }
            activate(y, O, L.act);
            std::swap(h, y);
        }
        std::memcpy(out, h, sizeof(float) * m_layers.back().nOut);
    }

    void Net::forwardColumns(float const* in, size_t columns, float* out) {
        if (m_kind != Kind::Conv1d || m_layers.empty() || columns == 0) return;
        size_t const nIn = m_mean.size();
        size_t widest = nIn;
        for (auto const& L : m_layers) widest = widest > L.nOut ? widest : L.nOut;
        m_a.resize(widest * columns);
        m_b.resize(widest * columns);
        float* h = m_a.data();  // [columns][width]
        float* y = m_b.data();
        for (size_t t = 0; t < columns; t++)
            for (size_t i = 0; i < nIn; i++) h[t * nIn + i] = (in[t * nIn + i] - m_mean[i]) / m_std[i];
        for (auto const& L : m_layers) {
            size_t const I = L.nIn, O = L.nOut;
            long long const half = ((long long)L.kernel - 1) / 2;
            for (size_t t = 0; t < columns; t++)
                for (size_t o = 0; o < O; o++) y[t * O + o] = L.b[o];
            // Per output: bias, then tap 0, 1, ... and within a tap input 0, 1, ...
            // Column t reads column t + (k - half) * dilation; outside the tape it reads 0.
            for (size_t t = 0; t < columns; t++) {
                float* yt = y + t * O;
                for (size_t k = 0; k < L.kernel; k++) {
                    long long const src = (long long)t + ((long long)k - half) * (long long)L.dilation;
                    if (src < 0 || src >= (long long)columns) continue;
                    float const* ht = h + (size_t)src * I;
                    float const* wk = L.w.data() + k * I * O;
                    for (size_t i = 0; i < I; i++) {
                        float const xi = ht[i];
                        float const* row = wk + i * O;
                        for (size_t o = 0; o < O; o++) yt[o] += row[o] * xi;
                    }
                }
            }
            activate(y, O * columns, L.act);
            std::swap(h, y);
        }
        std::memcpy(out, h, sizeof(float) * m_layers.back().nOut * columns);
    }

} // namespace gucci::ai::net

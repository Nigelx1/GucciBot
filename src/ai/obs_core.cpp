#include "ai/obs_core.hpp"

// Kept free of game code: the offline test (tools/run_ai_net_tests.cpp)
// builds this file on its own.

#include <cstring>

namespace gucci::ai::obs {

    // ------------------------------------------------------------------ halves

    uint16_t toHalf(float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        uint32_t const sign = (x >> 16) & 0x8000u;
        uint32_t const exp = (x >> 23) & 0xffu;
        uint32_t mant = x & 0x7fffffu;
        if (exp == 0xff) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0u));  // inf / nan
        int32_t e = (int32_t)exp - 127 + 15;
        if (e >= 0x1f) return (uint16_t)(sign | 0x7c00u);  // overflow: inf
        if (e <= 0) {
            // A subnormal half (or zero): shift the full mantissa in, round to nearest even.
            if (e < -10) return (uint16_t)sign;
            mant |= 0x800000u;
            uint32_t const shift = (uint32_t)(14 - e);
            uint32_t half = mant >> shift;
            uint32_t const rem = mant & ((1u << shift) - 1u);
            uint32_t const mid = 1u << (shift - 1u);
            if (rem > mid || (rem == mid && (half & 1u))) half++;
            return (uint16_t)(sign | half);
        }
        uint32_t half = ((uint32_t)e << 10) | (mant >> 13);
        uint32_t const rem = mant & 0x1fffu;
        if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;  // may carry into the exponent: right
        return (uint16_t)(sign | half);
    }

    float fromHalf(uint16_t h) {
        uint32_t const sign = (uint32_t)(h & 0x8000u) << 16;
        uint32_t const exp = (h >> 10) & 0x1fu;
        uint32_t mant = h & 0x3ffu;
        uint32_t x;
        if (exp == 0) {
            if (mant == 0) {
                x = sign;
            } else {
                int e = -1;
                do {
                    e++;
                    mant <<= 1;
                } while ((mant & 0x400u) == 0);
                x = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mant & 0x3ffu) << 13);
            }
        } else if (exp == 0x1f) {
            x = sign | 0x7f800000u | (mant << 13);
        } else {
            x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
        }
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }

    // ------------------------------------------------------------------ expansion

    void expand(Record const& r, float* out) {
        std::memcpy(out, r.state, sizeof(r.state));
        float* s = out + kStateDim;
        for (int i = 0; i < kScanDim; i++) {
            uint8_t const v = r.scan[i];
            s[i] = v == kScanNone ? 12.5f : (float)v / 8.0f;
        }
        float* t = out + kStateDim + kScanDim;
        std::memset(t, 0, sizeof(float) * kTokens * (2 + kTokenTypes));
        for (int k = 0; k < kTokens; k++) {
            uint8_t const* b = r.tok + k * kTokenBytes;
            float* slot = t + k * (2 + kTokenTypes);
            int const type = b[4];
            if (type < 1 || type >= kTokenTypes) continue;
            uint16_t dx, dy;
            std::memcpy(&dx, b, 2);
            std::memcpy(&dy, b + 2, 2);
            slot[0] = fromHalf(dx);
            slot[1] = fromHalf(dy);
            slot[2 + type] = 1.0f;
        }
        std::memcpy(out + kGeoOffset, r.geo, sizeof(r.geo));
    }

} // namespace gucci::ai::obs

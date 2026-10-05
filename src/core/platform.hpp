#pragma once

// What GucciBot can do on the platform it was built for.
//
// GucciBot's engine is a port of Silicate, which is Windows-only: its tick
// loop, TPS bypass and per-tick bookkeeping run on SafetyHook midhooks and byte
// patches placed at fixed addresses in the Windows build of GD, and a few
// subsystems call Windows APIs directly (Assistant Access's sockets, the
// renderer loading FFmpeg, sub-tick CBF recording's raw input). None of that
// exists on macOS, iOS or Android, so it is compiled out there, and the engine
// stands down on those platforms until a portable tick path replaces it
// (multiplatform branch, started 2026-10-04).
//
// GB_NATIVE_ENGINE: the Windows engine (midhooks, patches, raw addresses).
// GB_DESKTOP_GL: desktop OpenGL (pixel buffers for the renderer and Video Mode),
//   not OpenGL ES.

#include <Geode/platform/cplatform.h>

#include <Geode/Geode.hpp>

#include <cstdint>

#include <utility>

#if defined(GEODE_IS_WINDOWS)
#define GB_NATIVE_ENGINE 1
#else
#define GB_NATIVE_ENGINE 0
#endif

#if defined(GEODE_IS_DESKTOP)
#define GB_DESKTOP_GL 1
#else
#define GB_DESKTOP_GL 0
#endif

// Geode's gnustl (GD's standard library on Android) has no move assignment for
// gd::map, so a struct holding one, or a GJGameState, must not get an implicit
// move assignment there: declaring the copy assignment makes moves copy. Other
// platforms keep their cheap moves.
#if defined(GEODE_IS_ANDROID)
#define GB_GD_MAP_COPY_ONLY(T) T& operator=(T const&) = default;
#else
#define GB_GD_MAP_COPY_ONLY(T)
#endif

namespace gucci {

    // `dst = std::move(src)` for GD types holding gd::maps (a GJGameState):
    // a copy on Android, where Geode's gnustl has no map move assignment.
    // Copies a gd container into a std::vector element by element: on Android
    // (Geode's gnustl) gd iterators aren't standard iterators, so a std::vector
    // can't be built or assigned from a pair of them.
    template <class Vec, class Container>
    void fillFrom(Vec& out, Container const& in) {
        out.clear();
        out.reserve(in.size());
        for (auto const& e : in) {
            if constexpr (requires { e.first; e.second; })
                out.emplace_back(e.first, e.second);
            else
                out.push_back(e);
        }
    }

    template <class T>
    void gdAssign(T& dst, T& src) {
#if defined(GEODE_IS_ANDROID)
        dst = src;
#else
        dst = std::move(src);
#endif
    }

} // namespace gucci

namespace gucci {

    // GD's fast-rand state (GameToolbox::fast_srand / getfast_srand), which
    // updateRandomSeedOnReset rewinds on every reset and RNG lock pins; the
    // per-checkpoint RNG is captured and restored through these too.
    //
    // Windows: the global itself, at its known address (what GD's inline
    // fast_srand/getfast_srand touch there). Elsewhere GD's own functions,
    // bound on every other platform: on macOS and Android both are real
    // functions, on iOS getfast_srand is Geode's inline read of the global and
    // fast_srand a real function. GD's fast_rand is getfast_srand() * 214013 +
    // 2531011 fed back through fast_srand, so the two are one 64-bit state.
    inline uint64_t readGdRandom() {
#if GB_NATIVE_ENGINE
        return *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90);
#else
        return GameToolbox::getfast_srand();
#endif
    }

    inline void writeGdRandom(uint64_t state) {
#if GB_NATIVE_ENGINE
        *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90) = state;
#else
        GameToolbox::fast_srand(state);
#endif
    }

} // namespace gucci

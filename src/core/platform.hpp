#pragma once

// What GucciBot can do on the platform it was built for.
//
// GucciBot's engine is a port of Silicate, which is Windows-only: its tick
// loop, TPS bypass and per-tick bookkeeping run on SafetyHook midhooks and byte
// patches placed at fixed addresses in the Windows build of GD, and a few
// subsystems call Windows APIs directly (the renderer loading FFmpeg, sub-tick
// CBF recording's raw input). Those parts are compiled out on macOS, iOS and
// Android or have a fallback there (Assistant Access uses BSD sockets), and off
// Windows the engine ticks through GJBaseGameLayer::processCommands instead of
// midhooks (GBPortableTick, engine_updater.cpp).
//
// GB_NATIVE_ENGINE: the Windows engine (midhooks, patches, raw addresses).
// GB_DESKTOP_GL: desktop OpenGL (pixel buffers for the renderer and Video Mode),
//   not OpenGL ES.

#include <Geode/platform/cplatform.h>

#include <Geode/Geode.hpp>

#include <cstdint>

#include <utility>

#include "geode_compat.hpp"

// GB_FORCE_PORTABLE (a TEST-ONLY CMake option, default off; never set by CI,
// build_win.bat or a release) builds Windows with the off-Windows engine, so a
// hidden Windows test game can run the code macOS and Android run.
//
// The Windows engine's midhooks, patches and raw addresses are GD 2.2081's.
// The GD 2.2091 build (Geode 6) runs the off-Windows engine on Windows until
// they are found again in 2.2091 (docs/PORT_2209_ASSESSMENT.md, section 2).
#if defined(GEODE_IS_WINDOWS) && !defined(GB_FORCE_PORTABLE) && !GB_GEODE6
#define GB_NATIVE_ENGINE 1
#else
#define GB_NATIVE_ENGINE 0
#endif

// GB_GD2091_SITES: the GD 2.2091 build (Geode 6, Windows) places the raw
// sites it has 2.2091 addresses for (core/gd_addresses.hpp: the byte patches,
// the shake and teleport midhooks), each checked byte for byte before it goes
// in. The tick loop stays the off-Windows one there (GB_NATIVE_ENGINE 0).
#if defined(GEODE_IS_WINDOWS) && !defined(GB_FORCE_PORTABLE) && GB_GEODE6
#define GB_GD2091_SITES 1
#else
#define GB_GD2091_SITES 0
#endif

// GB_RAW_SITES: this build places midhooks or byte patches at all (the
// machinery in hooks/util_midhook.hpp, the master switch's half of it).
#if GB_NATIVE_ENGINE || GB_GD2091_SITES
#define GB_RAW_SITES 1
#else
#define GB_RAW_SITES 0
#endif

// GB_WORLD_LAYOUTS: GD 2.2081's Windows memory layout and code addresses,
// which the World (absense/world: the trigger model) and the pathfinder's
// physics copies read raw. Windows on GD 2.2081 only: on GD 2.2091 they are
// still to be found again (docs/PORT_2209_ASSESSMENT.md, section 2B), so the
// World stays off there as it does on macOS and Android.
#if defined(GEODE_IS_WINDOWS) && !GB_GEODE6
#define GB_WORLD_LAYOUTS 1
#else
#define GB_WORLD_LAYOUTS 0
#endif

// GB_HERE: how the menu names where it is running when a feature works
// differently from Windows on GD 2.2081: "GD 2.2091" in the 2.2091 build
// (Windows, Geode 6), "this platform" on macOS, iOS and Android. It joins
// string literals ("Untested on " GB_HERE "."), so the 2.2081 build's texts
// stay byte for byte what they were.
#if GB_GEODE6
#define GB_HERE "GD 2.2091"
#else
#define GB_HERE "this platform"
#endif

#if defined(GEODE_IS_DESKTOP)
#define GB_DESKTOP_GL 1
#else
#define GB_DESKTOP_GL 0
#endif

// GB_FFMPEG: the renderer and Video Mode have FFmpeg to load (render/ffmpeg.hpp):
// the Windows DLLs and, since 2.0, the macOS dylibs. Not on phones yet.
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
#define GB_FFMPEG 1
#else
#define GB_FFMPEG 0
#endif

// GB_IMGUI_FROM_DRAWSCENE: gd-imgui-cocos draws its frame (GucciBot's menu, the
// in-level boxes and Calculate's tick, all in gucci::ui::frame) from a
// CCDirector::drawScene hook, after the original, on macOS and Android; from
// CCEGLView::swapBuffers on Windows and iOS (gd-imgui-cocos src/hooks.cpp). A
// drawScene hook that returns without calling through has to draw it itself.
#if !defined(GEODE_IS_WINDOWS) && !defined(GEODE_IS_IOS)
#define GB_IMGUI_FROM_DRAWSCENE 1
#else
#define GB_IMGUI_FROM_DRAWSCENE 0
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

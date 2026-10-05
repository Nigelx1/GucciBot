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

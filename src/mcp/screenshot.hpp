#pragma once

// gucci_screenshot (Assistant Access, 2026-10-31): what the game window shows,
// saved as a PNG, so a session can look at the menu and the level itself
// instead of asking someone to describe them. Windows only for now.
//
// The capture sits in GucciBot's own CCEGLView::swapBuffers hook
// (hooks/hook_ccegview.cpp), hooked very late, after imgui-cocos's: by then
// the back buffer holds the scene, the menu, and every other mod's overlay
// drawn before the swap.
//
// The tool runs in the scheduler's update (mcp_server.cpp's pump), before the
// frame is drawn, so it only asks: request() arms a capture `frames` swaps
// from now, the hook takes it, and a thread of its own makes the PNG and
// writes it. The ask and the capture are both on the main thread; only the
// outcome crosses back from that thread, under a lock. A game that isn't
// drawing (minimized) never reaches its frame, and the shot stays pending.
//
// GucciBot (2026-11-01): a part of the window (crop), and bursts: `count`
// shots `every` frames apart, started now or by the game itself (player 1
// dying or respawning, the level's end, a frame), checked in the swap hook so
// a death flash a frame after the death is caught exactly, which no round
// trip from outside can do. One writer thread with a queue makes the PNGs;
// shots waiting for it are capped at 256 MB.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gucci::mcp::shot {

    // A part of the window. Rect: x, y, w, h in ImGui's pixels (its display,
    // top left), cut to the window. Player: a square around player 1, `half`
    // the window's pixels each side of it, worked out at each shot.
    struct Crop {
        enum class Kind { Window, Rect, Player };
        Kind kind = Kind::Window;
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        float half = 160.f;
    };

    enum class Trigger { Now, Death, Respawn, Complete, Frame };

    struct Burst {
        std::filesystem::path dir;  // must exist
        std::string name;           // <name>.png, or <name>_000.png... for more than one
        int frames = 3;             // swaps after the ask (Now) or the trigger before the first: 1 = that swap
        int count = 1;
        int every = 1;              // swaps between shots
        Trigger trigger = Trigger::Now;
        int64_t frame = 0;          // Trigger::Frame: the engine's frame to start at
        Crop crop;
        int scale = 1;              // 1, 2 or 4: each side divided by it
        bool manifest = false;      // write <name>.json: each shot's frame, player and rect
    };

    // A new ask replaces one still waiting for its frame (shots already taken
    // are still written).
    void request(Burst burst);
    // One shot at this path (the 2026-10-31 form).
    void request(std::filesystem::path path, int frames, Crop crop = {});

    // The last ask and how it went.
    struct Status {
        bool asked = false;    // anything asked for this launch
        bool pending = false;  // not all written yet (waiting for its trigger or frame, or being written)
        bool armed = false;    // waiting for its trigger
        int framesLeft = 0;    // swaps until the next shot; 0 once all are taken
        bool done = false;
        bool ok = false;
        std::string error;
        std::filesystem::path path;  // the first file
        std::vector<std::filesystem::path> files;
        std::filesystem::path manifest;
        int count = 1;
        int taken = 0;
        int written = 0;
        int width = 0;
        int height = 0;
        double ms = 0.0;  // from the ask to the last file on disk
    };
    Status status();

    // The window's size in pixels, as a shot reads it (false: minimized, or
    // no window).
    bool windowPixels(int& width, int& height);

    // Every swap, from the swapBuffers hook.
    void onSwap();

} // namespace gucci::mcp::shot

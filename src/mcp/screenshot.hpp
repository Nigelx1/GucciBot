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

#include <filesystem>
#include <string>

namespace gucci::mcp::shot {

    // A new ask replaces one still waiting for its frame. The folder must
    // exist already.
    void request(std::filesystem::path path, int frames);

    // The last ask and how it went.
    struct Status {
        bool asked = false;    // anything asked for this launch
        bool pending = false;  // not written yet (waiting for its frame, or being written)
        int framesLeft = 0;    // swaps until it's taken; 0 once it has been
        bool done = false;
        bool ok = false;
        std::string error;
        std::filesystem::path path;
        int width = 0;
        int height = 0;
        double ms = 0.0;  // from the ask to the file on disk
    };
    Status status();

    // Every swap, from the swapBuffers hook.
    void onSwap();

} // namespace gucci::mcp::shot

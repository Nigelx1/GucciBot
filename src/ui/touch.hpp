#pragma once

// The menu on a touch screen (Android and iOS). ImGui has no drag-to-scroll,
// and on a phone the pages and the page list are taller than the screen, so a
// vertical drag is turned into scrolling here. Also where the menu fits on the
// screen. Built only on Android and iOS: callers fence their calls with
// GEODE_IS_MOBILE (ui/shell.cpp). The scale the menu is drawn at is the
// look's (Look::deviceScale).

#include <imgui.h>

namespace gucci::ui::touch {

    // Once a frame, before any window is drawn: a finger that goes down on a
    // window and moves up or down by more than a few units scrolls the window
    // under it, and keeps it coasting a little after it lifts.
    void frame();

    // The screen inside the notch, rounded corners and home bar (Geode's safe
    // area), in ImGui units and less a small margin: where the menu goes.
    void screenArea(ImVec2* min, ImVec2* max);

} // namespace gucci::ui::touch

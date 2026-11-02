#pragma once

// GucciBot's mark, the iced cone Nigel approved on 2026-11-02, where the mod
// shows it itself: the cone's silhouette at the top of the menu's sidebar.
// (Geode's mod list shows logo.png, next to mod.json.) The pictures are in
// resources/brand, scaled down from docs/brand/logo_1080.png.

#include <imgui.h>

namespace gucci::ui::brand {

    // The cone's silhouette, all of it `tint`, `height` units tall with its
    // top-left corner at `at`. That's the cone itself: the picture's clear
    // margin is left out, so it lines up with what's beside and below it.
    // Returns how wide it was drawn, or 0 when its picture couldn't load (and
    // nothing was). Main thread, in the menu's draw.
    float drawMark(ImDrawList* dl, ImVec2 at, float height, ImU32 tint);

} // namespace gucci::ui::brand

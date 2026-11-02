#pragma once

// GucciBot's mark, the iced cone Nigel approved on 2026-11-02, where the mod
// shows it itself: the cone's silhouette at the top of the menu's sidebar,
// and the whole logo on the pause menu's button (phones and Macs). (Geode's
// mod list shows logo.png, next to mod.json; under GBot, the disguise,
// brand.cpp puts GBot's own logo over it as Geode makes it.) The pictures
// are in resources/brand, scaled down from the masters in docs/brand.

#include <imgui.h>

namespace cocos2d {
    class CCSprite;
}

namespace gucci::ui::brand {

    // GucciBot's logo for the pause menu's button (ui/pause_button.cpp),
    // `points` across, from a texture made for the pixels that many points
    // cover. nullptr when its picture couldn't load. Main thread.
    cocos2d::CCSprite* pauseSprite(float points);

    // The cone's silhouette, all of it `tint`, `height` units tall with its
    // top-left corner at `at`. That's the cone itself: the picture's clear
    // margin is left out, so it lines up with what's beside and below it.
    // Returns how wide it was drawn, or 0 when its picture couldn't load (and
    // nothing was). Main thread, in the menu's draw.
    float drawMark(ImDrawList* dl, ImVec2 at, float height, ImU32 tint);

} // namespace gucci::ui::brand

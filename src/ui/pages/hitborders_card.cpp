// The Hitborders card on the Macro page, under Hitboxes (hacks/hitborders.*):
// every hitbox grown by the player's own, so the player is a point (Nigel,
// 2026-11-02; the idea is NaN GD's "accurate gaps"). Everything here is saved
// the moment it changes, the way the Hitboxes card's switches are.

#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "hacks/hitborders.hpp"

#include <imgui.h>

namespace gucci::ui::pages {

    namespace {
        // Whether the color rows are open. Not a setting.
        bool g_showColors = false;
    } // namespace

    void hitbordersCard() {
        auto* overlay = HitborderOverlay::get();
        auto& set = overlay->settings;
        bool changed = false;

        kit::BeginCard("Hitborders",
                       "Every hitbox grown by the size of yours, so you can think of yourself as a point. The gaps "
                       "you see are the room your center really has. Hitborders were NaN GD's idea; he isn't "
                       "involved.");

        changed |= kit::SwitchRow("Show hitborders",
                                  "Red: your center dies there. Blue: your box touches something solid. Green: orbs, "
                                  "pads and portals",
                                  &set.show);
        changed |= kit::SwitchRow("Show on death", "Draw them once you die, even with the switch above off", &set.onDeath);

        kit::Section("What to draw");
        changed |= kit::SwitchRow("Hazards", "Spikes, saws and hazard slopes", &set.hazards);
        changed |= kit::SwitchRow("Blocks", nullptr, &set.blocks);
        if (set.blocks) {
            int mode = set.exactBlocks ? 0 : 1;
            const char* modes[] = {"Exact", "Simple"};
            if (kit::ChoiceRow("Block borders",
                               "Exact adds the red core: in a block, only the small box inside your icon kills you. "
                               "Simple shows just where your box touches",
                               &mode, modes, 2)) {
                set.exactBlocks = mode == 0;
                changed = true;
            }
        }
        changed |= kit::SwitchRow("Slopes", "New, not checked in the game yet", &set.slopes);
        changed |= kit::SwitchRow("Orbs, pads and portals", "And touch triggers. Ones you've used are hidden",
                                  &set.specials);
        changed |= kit::SwitchRow("Ground and ceiling", nullptr, &set.ground);
        changed |= kit::SwitchRow("Rotated objects use your tilt",
                                  "Turned objects are tested against your icon's own tilt. Off: drawn for an upright "
                                  "icon",
                                  &set.useTilt);
        changed |= kit::SwitchRow("Player 2",
                                  "In dual, player 2's own borders as outlines, where they differ from player 1's",
                                  &set.player2);

        kit::Section("You");
        changed |= kit::SwitchRow("Show yourself as a dot", "Where the game last tested you. Your icon stays",
                                  &set.point);
        changed |= kit::SwitchRow("Your path", "A line through your center, one point a tick", &set.path);
        if (set.path)
            changed |= kit::SliderRow("Path length", "Ticks kept; at 240 TPS, 240 is one second", &set.pathTicks,
                                      HitborderOverlay::kMinPathTicks, HitborderOverlay::kMaxPathTicks, "%d ticks");
        changed |= kit::SwitchRow("Clicks", "A dot where you pressed, a ring where you let go", &set.clicks);

        kit::Section("Look");
        changed |= kit::SliderRow("Line width", nullptr, &set.lineWidth, 0.25f, 6.f, "%.2f");
        changed |= kit::SliderRow("Fill", "How solid the inside of each border is", &set.fill, 0.f, 1.f, "%.2f");
        changed |= kit::SwitchRow("One outline",
                                  "Borders that overlap share one outline. Off: each one has its own", &set.merged);
        if (kit::FeatureBegin("Colors", "Pick the color of each kind", &g_showColors)) {
            for (int i = 0; i < HitborderOverlay::PartCount; ++i) {
                auto const part = static_cast<HitborderOverlay::Part>(i);
                auto& rgba = set.rgba[i];
                ImVec4 colour(rgba[0], rgba[1], rgba[2], rgba[3]);
                if (kit::ColorRow(HitborderOverlay::partLabel(part), nullptr, &colour)) {
                    rgba[0] = colour.x;
                    rgba[1] = colour.y;
                    rgba[2] = colour.z;
                    rgba[3] = colour.w;
                    changed = true;
                }
            }
            kit::Gap();
            if (kit::Button("Reset the look")) {
                auto const def = HitborderOverlay::defaults();
                set.lineWidth = def.lineWidth;
                set.fill = def.fill;
                set.merged = def.merged;
                for (int i = 0; i < HitborderOverlay::PartCount; ++i) {
                    for (int k = 0; k < 4; ++k)
                        set.rgba[i][k] = def.rgba[i][k];
                }
                changed = true;
            }
            kit::FeatureEnd();
        }
        if (changed)
            overlay->save();

        kit::EndCard();
    }

} // namespace gucci::ui::pages

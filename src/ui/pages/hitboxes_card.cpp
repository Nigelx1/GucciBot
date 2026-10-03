// The Hitboxes card on the Hacks page: Show Hitboxes (hacks/hitboxes.*, a
// port of Silicate's hitbox drawing, by peony). The switches are the
// engine's and are saved the moment they change; the look is the overlay's.

#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "core/GucciBot.hpp"
#include "hacks/hitboxes.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <cstdint>

namespace gucci::ui::pages {

    namespace {
        // Whether the parts-and-colours rows are open. Not a setting.
        bool g_showParts = false;
    } // namespace

    void hitboxCard() {
        auto* gb = GucciEngine::get();
        auto* overlay = HitboxOverlay::get();
        auto* mod = geode::Mod::get();

        kit::BeginCard("Hitboxes", "Collision boxes over the level, in play, practice, recording and playback.");

        if (kit::SwitchRow("Show hitboxes", "Players, blocks, hazards, orbs and portals", &gb->showHitboxes))
            mod->setSavedValue<bool>("hack_hitboxes", gb->showHitboxes);
        if (kit::SwitchRow("Show on death", "Draw them once the player dies, even with the switch above off",
                           &gb->hitboxOnDeath))
            mod->setSavedValue<bool>("hack_hitbox_death", gb->hitboxOnDeath);
        if (kit::SwitchRow("Trail", "Leave the player's boxes behind it, one per tick", &gb->hitboxTrail)) {
            mod->setSavedValue<bool>("hack_hitbox_trail", gb->hitboxTrail);
            overlay->clearTrail();
        }
        if (gb->hitboxTrail &&
            kit::SliderRow("Trail length", "Ticks kept; at 240 TPS, 240 is one second", &gb->hitboxTrailLength,
                           10, HitboxOverlay::kMaxTrailTicks, "%d ticks"))
            mod->setSavedValue<int64_t>("hack_hitbox_trail_len", gb->hitboxTrailLength);

        kit::Section("Look");
        auto& style = overlay->style;
        bool changed = false;
        changed |= kit::SliderRow("Line width", nullptr, &style.lineWidth, 0.1f, 3.f, "%.2f");
        changed |= kit::SliderRow("Fill", "How solid the inside of each box is", &style.fill, 0.f, 1.f, "%.2f");
        if (kit::FeatureBegin("Parts and colours", "Pick what is drawn, and in which colour", &g_showParts)) {
            for (int i = 0; i < HitboxOverlay::PartCount; ++i) {
                auto const part = static_cast<HitboxOverlay::Part>(i);
                auto& ps = style.parts[i];
                kit::RowBegin(HitboxOverlay::partLabel(part), HitboxOverlay::partHint(part));
                changed |= kit::Switch("##on", &ps.on);
                ImGui::SameLine(0.f, 12.f);
                ImVec4 colour(ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3]);
                if (kit::Color("##colour", &colour)) {
                    ps.rgba[0] = colour.x;
                    ps.rgba[1] = colour.y;
                    ps.rgba[2] = colour.z;
                    ps.rgba[3] = colour.w;
                    changed = true;
                }
                kit::RowEnd();
            }
            kit::Gap();
            if (kit::Button("Reset the look")) {
                style = HitboxOverlay::defaults();
                changed = true;
            }
            kit::FeatureEnd();
        }
        if (changed)
            overlay->saveStyle();

        kit::EndCard();
    }

} // namespace gucci::ui::pages

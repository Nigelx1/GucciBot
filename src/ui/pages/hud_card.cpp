// The HUD card on the Hacks page: the HUD's on/off, its readouts in three
// groups, and where and how it is drawn (hacks/hud.*). Every switch is the
// engine's (GucciEngine::hud) and is saved under its hud_<field> key the
// moment it changes; the HUD reads them every drawn frame.

#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "core/GucciBot.hpp"
#include "hacks/hud.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cstdint>

namespace gucci::ui::pages {

    namespace {
        constexpr const char* kCorners[hud::AnchorCount] = {"Top left", "Top right", "Bottom left",
                                                            "Bottom right"};
        constexpr const char* kFonts[] = {"Chat", "Big"};
    } // namespace

    void hudCard() {
        auto& cfg = GucciEngine::get()->hud;
        auto* mod = geode::Mod::get();

        kit::BeginCard("HUD", "Live readouts in a corner of the level. Hidden while a video renders.");

        if (kit::SwitchRow("Show the HUD", nullptr, &cfg.enabled))
            mod->setSavedValue<bool>(hud::kKeyEnabled, cfg.enabled);

        for (int g = 0; g < hud::GroupCount; ++g) {
            auto const group = static_cast<hud::Group>(g);
            kit::Section(hud::groupName(group));
            for (auto const& r : hud::kReadouts) {
                if (r.group != group)
                    continue;
                if (kit::SwitchRow(r.label, r.hint, &(cfg.*r.field)))
                    mod->setSavedValue<bool>(r.key, cfg.*r.field);
            }
        }

        kit::Section("Look");
        cfg.anchor = std::clamp(cfg.anchor, 0, hud::AnchorCount - 1);
        if (kit::DropdownRow("Corner", nullptr, &cfg.anchor, kCorners, hud::AnchorCount))
            mod->setSavedValue<int64_t>(hud::kKeyAnchor, cfg.anchor);
        int font = cfg.bigFont ? 1 : 0;
        if (kit::ChoiceRow("Font", "Chat is GD's small text; Big is its title font", &font, kFonts, 2)) {
            cfg.bigFont = font == 1;
            mod->setSavedValue<bool>(hud::kKeyBigFont, cfg.bigFont);
        }
        if (kit::SliderRow("Size", nullptr, &cfg.scale, hud::kMinScale, hud::kMaxScale, "%.2fx"))
            mod->setSavedValue<float>(hud::kKeyScale, cfg.scale);
        if (kit::SliderRow("Opacity", nullptr, &cfg.opacity, hud::kMinOpacity, hud::kMaxOpacity, "%.2f"))
            mod->setSavedValue<float>(hud::kKeyOpacity, cfg.opacity);

        kit::EndCard();
    }

} // namespace gucci::ui::pages

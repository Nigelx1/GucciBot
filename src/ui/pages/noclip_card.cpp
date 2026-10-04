// The Noclip accuracy card on the Hacks page (hacks/noclip_accuracy.hpp).
// Written fresh on 2026-10-03. Both settings write the key the engine loads
// them from (GucciEngine::loadEngineSettings, core/engine_core.cpp) the moment
// they change.

#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "core/GucciBot.hpp"
#include "hacks/noclip_accuracy.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace gucci::ui::pages {

    void noclipAccuracyCard() {
        auto* gb = GucciEngine::get();
        auto* mod = Mod::get();

        kit::BeginCard("Noclip accuracy",
                       "The share of this attempt's ticks on which noclip did not have to save you. A reset or a "
                       "respawn starts it over.");

        if (kit::SwitchRow("Show it in the level", "At the top of the screen, while noclip is on",
                           &gb->noclipAccuracyVisible))
            mod->setSavedValue<bool>("hack_noclip_accuracy", gb->noclipAccuracyVisible);

        // Shown in percent, kept as 0..1.
        float percent = gb->noclipThreshold * 100.f;
        if (kit::SliderRow("Die below",
                           "A hit that would take the attempt under this is not blocked: you die as without "
                           "noclip. Counted from the attempt's start, so a hit in its first moments costs the most. "
                           "0 = never",
                           &percent, 0.f, 100.f, "%.1f%%")) {
            gb->noclipThreshold = std::clamp(percent / 100.f, 0.f, 1.f);
            mod->setSavedValue<float>("hack_noclip_die_below", gb->noclipThreshold);
        }

        if (PlayLayer::get() && gb->noclipEnabled) {
            kit::RowBegin("This attempt", nullptr);
            kit::Label(fmt::format("{:.2f}% ({} {})", noclipacc::accuracy() * 100.f, noclipacc::hits(),
                                   noclipacc::hits() == 1 ? "death" : "deaths")
                           .c_str());
            kit::RowEnd();
        } else if (!gb->noclipEnabled) {
            kit::Hint("Noclip is off.");
        }

        kit::EndCard();
    }

} // namespace gucci::ui::pages

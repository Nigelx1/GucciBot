#pragma once

// Pages and cards the shell (ui/shell.cpp) draws. Each lives in its own file
// under src/ui/pages/ and is built only from the kit (ui/kit.hpp), the look
// and the engine.

namespace gucci::ui::pages {

    // Sidebar pages
    void themes();       // pages/themes.cpp: theme picker + custom theme editor
    void clickSounds();  // pages/clicksounds.cpp

    // Cards drawn inside other pages
    void hitboxCard();       // pages/hitboxes_card.cpp (Hacks page)
    void autoclickerCard();  // pages/autoclicker_card.cpp (Hacks page)
    void predictionCard();   // pages/prediction_card.cpp (Hacks page)
    void hudCard();          // pages/hud_card.cpp (Hacks page)

} // namespace gucci::ui::pages

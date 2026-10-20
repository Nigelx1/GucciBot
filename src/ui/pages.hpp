#pragma once

// Pages and cards the shell (ui/shell.cpp) draws. Each lives in its own file
// under src/ui/pages/ and is built only from the kit (ui/kit.hpp), the look
// and the engine.

namespace gucci::ui::pages {

    // Sidebar pages
    void themes();       // pages/themes.cpp: theme picker + custom theme editor
    void clickSounds();  // pages/clicksounds.cpp
    void editor();       // pages/editor.cpp: Frame Editor (timeline of the loaded macro's inputs)
    void calculate();    // pages/calculate.cpp: Calculate (frame-window analyzer) settings + run
    void macroTools();   // pages/macro_tools.cpp: Macro Tools (diff, trim, merge, metadata, TPS changes)
    void editorTools();  // pages/editor_tools.cpp: Replace All and Macro Buffing (level editor)
    void jupiterTrainer();  // pages/trainer_pages.cpp: Nigel's Jupiter My Favourite Trainer
    void jupiterBackdrop(); // ... its sky, drawn under the whole menu window
    void jupiterLookPush(); // ... its colours over the whole menu (the shell, around the window)
    void jupiterLookPop();
    void jupiterVideoOverlay();  // Video Mode: the JMF click bar over the video, in place of the menu
    void trainerVideoOverlay();  // ... the Trainer's, over the video you picked
    void anyTrainer();      // pages/trainer_pages.cpp: Trainer (any saved macro)
    void clickIndicators();  // pages/click_indicators_card.cpp: Click Indicators (both trainer pages)
    void indicators();      // pages/indicators.cpp: Survival Indicator, click cue + calibration, accuracy/streak (both trainer pages)

    // Cards drawn inside other pages
    void hitboxCard();       // pages/hitboxes_card.cpp (Macro page)
    void autoclickerCard();  // pages/autoclicker_card.cpp (Hacks page)
    void predictionCard();   // pages/prediction_card.cpp (Hacks page)
    void hudCard();          // pages/hud_card.cpp (Hacks page)
    void noclipAccuracyCard();  // pages/noclip_card.cpp (Hacks page)
    void videoModeCard();       // pages/videomode_card.cpp (JMF Trainer page)
    void trainerVideoModeCard();  // pages/videomode_card.cpp (Trainer page)

} // namespace gucci::ui::pages

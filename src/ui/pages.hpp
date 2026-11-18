#pragma once

#include <string>

// Pages and cards the shell (ui/shell.cpp) draws. Each lives in its own file
// under src/ui/pages/ and is built only from the kit (ui/kit.hpp), the look
// and the engine.

namespace gucci::ui::pages {

    // The menu's sidebar width (shell.cpp drawMain); RondoBot's gold line runs
    // down its inner edge.
    inline constexpr float kSidebarWidth = 150.f;

    // Sidebar pages
    void themes();       // pages/themes.cpp: theme picker + custom theme editor
    void clickSounds();  // pages/clicksounds.cpp
    void editor();       // pages/editor.cpp: Frame Editor (timeline of the loaded macro's inputs)
    bool editorKeysLive();  // ... its shortcuts were listening at its last draw (Assistant Access: gucci_layout)
    // ... a key from the game's keyboard (hacks/keybinds.cpp), with Ctrl held
    // as ui::ctrlHeld() reads it: Z (Shift+Z redoes), Y or A while the editor
    // has the keyboard (its page or a card focused at its last draw, the menu
    // open, no text box active) is its undo, redo or select all, done at its
    // next draw. True when it's the editor's: the key goes no further (not to
    // GD, not to a hotkey). A key going up is never taken.
    bool editorCtrlKey(int keyCode, bool shift, bool down, bool repeat);
    void calculate();    // pages/calculate.cpp: Calculate (frame-window analyzer) settings + run
    void macroTools();   // pages/macro_tools.cpp: Macro Tools (diff, trim, merge, metadata, TPS changes)
    void editorTools();  // pages/editor_tools.cpp: Replace All and Macro Buffing (level editor)
    void jupiterTrainer();  // pages/trainer_pages.cpp: Nigel's Jupiter My Favourite Trainer
    void jupiterBackdrop(); // pages/jupiter_backdrop.cpp: its sky (and JupiterBot's), drawn under the whole menu window
    // ... once a frame: its pictures' textures go a few seconds after it was
    // last drawn, unless `keep` (JupiterBot, which shows it on every page).
    void jupiterArtIdle(bool keep);
    void jupiterLookPush(); // pages/trainer_pages.cpp: its colours over the whole menu (the shell, around the window)
    void jupiterLookPop();
    void jupiterLookApply(); // ... the same colours written over the style, for JupiterBot (after look().refresh())
    void solarBackdrop();    // pages/solar_backdrop.cpp: SolarBot's scene, Janet in the corner of an orrery behind the menu
    // ... once a frame: its pictures' textures go a few seconds after it was
    // last drawn, unless `keep` (SolarBot is the theme).
    void solarArtIdle(bool keep);
    void solarLookApply();   // ... her gems' blue, her card edges, the framed page and scroll grab (after look().refresh())
    void gardenBackdrop();   // pages/garden_backdrop.cpp: RondoBot's scene, the Garden's parquet behind the menu
    void jupiterVideoOverlay();  // Video Mode: the JMF click bar over the video, in place of the menu
    void trainerVideoOverlay();  // ... the Trainer's, over the video you picked
    void anyTrainer();      // pages/trainer_pages.cpp: Trainer (any saved macro)
    void clickIndicators();  // pages/click_indicators_card.cpp: Click Indicators (both trainer pages)
    void indicators();      // pages/indicators.cpp: Survival Indicator, click cue + calibration, accuracy/streak (both trainer pages)

    // Cards drawn inside other pages
    void pathfinderQueueCard();   // pages/queue_cards.cpp: the Pathfinder queue (Pathfinder page)
    void frameWindowQueueCard();  // pages/queue_cards.cpp: the frame-window queue (Calculate page)
    void hitboxCard();       // pages/hitboxes_card.cpp (Macro page)
    void trajectoryRows();   // pages/prediction_card.cpp: Show Trajectory and its options (in the Hitboxes card)
    void hitbordersCard();   // pages/hitborders_card.cpp (Macro page, under Hitboxes)
    void autoclickerCard();  // pages/autoclicker_card.cpp (Hacks page)
    void predictionCard();   // pages/prediction_card.cpp (Hacks page)
    void hudCard();          // pages/hud_card.cpp (Hacks page)
    void noclipAccuracyCard();  // pages/noclip_card.cpp (Hacks page)
    void videoModeCard();       // pages/videomode_card.cpp (JMF Trainer page)
    void trainerVideoModeCard();  // pages/videomode_card.cpp (Trainer page)
    void videoModeSettings(bool jmf);  // pages/videomode_card.cpp: the card's settings, over the video while it plays

    // pages/editor.cpp: why a saved macro can't be loaded over the loaded one
    // right now (Calculate or a pathfinder works on the loaded macro's list),
    // or nullptr. The Saved macros list, its row menu, Compact Mode and Macro
    // Tools' Load it again ask it.
    const char* macroLoadLock();
    // shell.cpp: a saved macro file as a picker lists it, "name (Theme)" when
    // it was saved under a theme's own extension (the Saved macros list's tag).
    std::string macroFileLabel(std::string const& name, std::string const& ext);

} // namespace gucci::ui::pages

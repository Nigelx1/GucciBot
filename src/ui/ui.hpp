#pragma once

// GucciBot's interface, rewritten from scratch on 2026-10-01. This header is
// all the rest of the mod sees of it: whether the menu is open, the macro file
// extension of the active theme, the keybinds, and a way to save settings.
// Pages, widgets, the look and the settings registry live in src/ui/ and are
// not included from outside it.

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace gucci::ui {

    // ---- the menu window

    bool isOpen();
    void setOpen(bool open);
    void toggleOpen();

    // Assistant Access while GucciBot is switched off (core/bot_switch.hpp):
    // stopped with it, and started again with it if it was running.
    void suspendAssistantAccess();
    void resumeAssistantAccess();

    // The small quick-controls window instead of the full menu.
    bool compactMode();
    void setCompactMode(bool on);

    // ---- keybinds (virtual key codes as cocos reports them; 0 = unbound)

    struct Keys {
        int onMenu = 0xA4;          // left Alt
        int onFrameAdvance = 0x56;  // V
        int onFrameStep = 0x43;     // C
        int onReplayToggle = 0;
        int onNoclip = 0;
        int onSafeMode = 0;
        int onTrajectory = 0;
        int onAudioPitch = 0;
        int onRngLock = 0;
        int onHitboxes = 0;
        int onHitborders = 0;
        int onLayoutMode = 0;
        int onNoMirror = 0;
        int onAutoclicker = 0;
        int onIntentionalDeath = 0;
        int onBackStep = 0;
        int onAutoFlip = 0;
        int onPreventDeath = 0;
        int onMirrorInputs = 0;
        int onCompactMode = 0;
    };
    Keys& keys();

    // A key-capture control is waiting for the next key: the keyboard hook
    // hands it here first. True when the key was taken (Escape unbinds).
    bool feedKeyCapture(int keyCode);
    bool capturingKey();

    // Human-readable name of a key code ("Left Alt", "F5", "Unbound").
    std::string keyName(int keyCode);

    // ---- themes, as far as other features need them

    // The persisted theme number (built-ins keep their old numbers; a custom
    // theme is kCustomThemeId).
    constexpr int kCustomThemeId = 1000;
    int activeThemeId();

    // Dot-prefixed macro extension of the active theme (".brrr", ".icebrrr",
    // or a custom theme's own), and every extension any theme uses.
    std::string macroExtension();
    std::vector<std::string> knownMacroExtensions();

    // The active custom theme's name and extension (empty when a built-in is
    // active), and the folder custom themes (and their audio) live in.
    std::string activeCustomThemeName();
    std::string activeCustomThemeExtension();
    std::filesystem::path customThemesDir();

    // Built-in theme data other features read (the BIG BRRRR drop).
    struct ThemeAudio {
        double bpm = 140.0;
        double dropOffsetSec = 0.0;
        std::string file;  // resource file name, or a full path for a custom theme; empty = none
    };
    ThemeAudio activeThemeAudio();

    // ---- the GBot disguise
    //
    // GBot is a plain look with a plain name (Nigel, 2026-10-31): while it is
    // the theme in use, nothing on screen says GucciBot. Text that names the
    // mod goes through these where it is shown. Saved values, file contents
    // and log file names keep the real name; the Credits page, which names
    // it too, leaves the sidebar instead (as Themes and the JMF Trainer do).

    // GBot is the theme in use. Until the menu has loaded its settings (a
    // popup at launch can come first) the saved choice decides.
    bool disguised();
    // "GBot" while disguised, otherwise "GucciBot".
    const char* brandName();
    // `text` with every "GucciBot" in it said as brandName(). A temporary:
    // kit::Hint(branded(x).c_str()) is fine, a c_str() kept past the
    // statement is not.
    std::string branded(std::string text);

    // ---- settings

    // Write every setting the interface registers to the mod's saved values.
    void saveSettings();

    // ---- the menu driven by Assistant Access (gucci_menu, for screenshots)

    // The sidebar's pages, in its order, and the one showing.
    std::vector<std::string> pageTitles();
    std::string pageTitle();
    // Shows the sidebar's page with this title (exactly, else ignoring case).
    // False when the sidebar has none.
    bool setPageByTitle(const std::string& title);
    // gucci_menu checks what it's asked before it changes anything: whether a
    // theme has this name (as setTestTheme looks it up), and the sidebar's
    // titles once a test's `theme` is on ("" the user's own, nullptr the one
    // on now; GBot's sidebar has no Themes, JMF Trainer or Credits), and
    // whether `title` is one.
    bool hasTheme(const std::string& name);
    std::vector<std::string> pageTitlesWith(const std::string* theme);
    bool listsPage(const std::string& title, const std::string* theme);

    // A test's look: a theme by name (built-in or custom; exactly, else
    // ignoring case) and the Animations switch. Never saved: while one is on,
    // saveSettings keeps the user's own choice. setTestTheme("") ends both and
    // puts the user's own back, and so does a restart; the user picking a
    // theme, or flipping Animations, ends that one and saves theirs. False,
    // and nothing changes, when no theme has that name.
    bool setTestTheme(const std::string& name);
    void setTestEffects(bool on);
    // The user picked a theme (the Themes picker): a test's theme ends there,
    // even when the pick is the one the test put on, so theirs is saved. True
    // when a test's theme was on.
    bool endTestTheme();

    // ---- what the menu shows, as words (Assistant Access reads these)

    // The status bar under every page's title: what the bot is doing (IDLE,
    // RENDERING, CALCULATING, PATHFINDING, RECORDING or PLAYING), its chip's
    // tone (muted, bad, accent or good), the SAFE chip (safe mode forced for
    // this launch) and the numbers beside them. The bar is drawn from this,
    // so the two can't drift.
    struct StatusLine {
        std::string state;
        std::string tone;
        bool safe = false;
        std::string text;  // "frame 120  |  240 TPS  |  1.00x  |  56 inputs" (the frame only in a level)
    };
    StatusLine statusLine();

    // The Saved macros list as its rows show it: the name, the file's
    // extension, its theme tag ("" for none, and always while disguised) and
    // whether it's the loaded one.
    struct ListedMacro {
        std::string name;
        std::string ext;
        std::string tag;
        bool selected = false;
    };
    std::vector<ListedMacro> listedMacros();
    // Other bots' macros in the replays folder, each with the format the
    // card names for it (known false: not a format GucciBot can read).
    struct ForeignMacro {
        std::string file;
        std::string format;
        bool known = false;
    };
    std::vector<ForeignMacro> foreignMacroList();

    struct MenuState {
        bool open = false;
        bool compact = false;              // Compact mode: its small window shows instead of the pages
        std::string page;                  // the page showing
        std::vector<std::string> pages;    // pageTitles()
        int themeId = 0;                   // as activeThemeId()
        std::string themeName;             // as the sidebar shows it
        bool testTheme = false;            // the theme is setTestTheme's
        std::vector<std::string> themes;   // every theme's name: the built-ins by id, then the user's
        bool effects = true;               // Animations
        bool testEffects = false;          // set by setTestEffects
    };
    MenuState menuState();

} // namespace gucci::ui

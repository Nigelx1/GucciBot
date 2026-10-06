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

    // ---- settings

    // Write every setting the interface registers to the mod's saved values.
    void saveSettings();

} // namespace gucci::ui

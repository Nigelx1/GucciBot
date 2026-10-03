#pragma once

// Themes: GucciBot's twenty built-in rapper themes and the user's own.
//
// A theme is a look (five colours, corner radius, window opacity, an accent
// that can pulse), words (the name, the heading the menu shows, a subtitle, a
// brand tag, a credits badge and a quote for each of three pages), the file
// extension macros save with, and the BIG BRRRR drop it plays.
//
// Built-in ids are the numbers persisted as "active_theme": 0..19 in the old
// menu's order, so a saved choice keeps meaning the same theme. Every custom
// theme is kCustomThemeId and is told apart by its name, persisted as
// "active_custom_theme". Custom themes are JSON files in customThemesDir()
// named <extension>.json (no dot), with their own track beside them as
// <extension>_brrr.mp3; the JSON keys are the ones the old editor wrote, so
// themes made before the 2026-10-01 rewrite keep loading.

#include "ui/ui.hpp"

#include <imgui.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gucci::ui::themes {

    // The three places a theme puts a quote (the old menu's Replay, Tools and
    // Credits tabs).
    enum class Slot { Replay, Tools, Credits };
    constexpr int kSlotCount = 3;

    struct Quote {
        std::string text;  // with its quotation marks
        std::string by;    // "-- Gucci Mane, probably"; may be empty
    };

    struct Theme {
        int id = 0;
        std::string name;       // unique; what the picker lists
        std::string title;      // the menu's heading (the three ToosiiBot themes share one)
        std::string extension;  // dot-prefixed
        std::string subtitle;
        std::string brand;      // the short tag on the status line ("Brrr.")
        std::string badge;      // the credits page badge ("Concept | Vision | Brrr")
        std::array<Quote, kSlotCount> quotes;

        ImVec4 accent{0.788f, 0.659f, 0.298f, 1.f};
        ImVec4 background{0.051f, 0.051f, 0.051f, 0.96f};
        ImVec4 card{0.078f, 0.078f, 0.078f, 1.f};
        ImVec4 text{0.941f, 0.910f, 0.816f, 1.f};
        ImVec4 textMuted{0.478f, 0.447f, 0.376f, 1.f};
        float radius = 5.f;
        float opacity = 0.96f;
        bool pulse = false;  // the accent breathes (Red Kingdom)
        bool snow = false;   // the theme's look includes falling snow (BrrrBot); nothing draws it yet

        // BIG BRRRR
        double bpm = 140.0;
        double dropOffsetSec = 0.0;  // where in the track playback starts, in seconds
        std::string track;           // built-in: the bundled resource file it plays
        bool hasTrack = false;       // custom: its own <extension>_brrr.mp3 is in the folder

        bool custom = false;
        std::filesystem::path file;  // custom: the JSON it was loaded from
    };

    const Quote& quote(const Theme& theme, Slot slot);

    const std::vector<Theme>& builtins();
    const Theme* builtin(int id);

    // Custom themes found in customThemesDir(), sorted by name. loadCustoms()
    // re-reads the folder; pointers and references into the list do not
    // survive it (nor saveCustom/deleteCustom, which call it).
    std::vector<Theme>& customs();
    void loadCustoms();
    std::filesystem::path customThemesDir();
    const Theme* findCustom(std::string_view name);

    // A new custom theme: GucciBot's look and the old editor's starter words.
    Theme starterCustom();

    // The rules a custom theme is saved under. sanitizeExtension keeps ASCII
    // letters (lowercased) and digits, at most 16 of them, and puts the dot
    // back: "My.Ext!" -> ".myext", "" when nothing is left. The *Problem
    // functions say why a value can't be saved, or return "" when it can;
    // `self` is the name the theme being edited was saved under, so it may
    // keep its own name and extension.
    std::string sanitizeExtension(std::string_view raw);
    std::string extensionProblem(std::string_view extension, std::string_view self = {});
    std::string nameProblem(std::string_view name, std::string_view self = {});

    // Where a custom theme with this extension keeps its own track.
    std::filesystem::path customTrackPath(std::string_view extension);
    // The track a theme actually plays: its own file, or the bundled one
    // (GucciBot's, for a custom theme without a track of its own).
    std::filesystem::path trackPath(const Theme& theme);

    // What saving does to the theme's own track.
    struct TrackChange {
        std::filesystem::path copyFrom;  // not empty: this file becomes the track
        bool remove = false;             // delete the track
    };

    // Write a custom theme, new or replacing the one saved as `previousName`,
    // then reload the list. A change of extension takes the track along and
    // removes the old files; renaming the active theme keeps it active.
    // Returns "" when saved, otherwise what went wrong (nothing is lost then).
    std::string saveCustom(const Theme& theme, std::string_view previousName = {}, const TrackChange& track = {});
    // Delete a custom theme and its track. If it was active, GucciBot becomes
    // active (the caller saves settings). Returns "" or what went wrong.
    std::string deleteCustom(std::string_view name);

    // The BIG BRRRR drop a theme plays. `file` is a resource file name for a
    // built-in (and for a custom theme without a track: GucciBot's own, at its
    // BPM and offset), or the custom track's full path as UTF-8.
    ThemeAudio audio(const Theme& theme);

    const Theme& active();
    int activeId();
    const std::string& activeCustomName();
    void setActive(int id);
    void setActiveCustom(const std::string& name);

    // Live preview. While a page calls preview() every frame, active() shows
    // `draft`'s look and words; the chosen theme's id, extension and BIG BRRRR
    // drop stay in force, so nothing but the look changes. It lapses on its
    // own a frame after the calls stop (the page closed, another page shown).
    void preview(const Theme& draft);
    void endPreview();
    bool previewing();

} // namespace gucci::ui::themes

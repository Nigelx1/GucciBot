#pragma once

// Click sounds, written from scratch on 2026-10-02 (the earlier ones went out
// with the ToastyReplay-derived code). A click plays when jump is pressed and
// a release sound when it is let go, picked at random from a click pack.
//
// A click pack is a folder holding up to four folders of sounds (WAV, MP3,
// OGG, FLAC or AIFF):
//
//     clicks/   releases/   softClicks/   softReleases/
//
// Any of them can be missing. The soft ones are for quick re-clicks: a press
// that comes soon after the last release, or a release that comes soon after
// its press. A pack with no clicks/ folder can keep its click sounds loose in
// the pack folder itself, and a pack laid out per player (player1/ and
// player2/, each holding the four folders) works too. Packs made for other
// bots name some folders differently: hardClicks/ and hardReleases/ are read
// when clicks/ or releases/ is missing, microClicks/ and microReleases/ when
// the soft ones are. Folder names are not case sensitive (Windows).
//
// The player's own packs live in <save dir>/clickpacks/<pack>/. The built-in
// pack, Clickbot, ships in the mod's resources. Geode packages resource files
// flat, so its sounds are named clickpack_<pack>_<folder>_<number>.flac
// (resources/clicks/Clickbot/...), and any pack named that way is built in.
//
// The soft-click window is timed on the game's own clock in a level (frames
// over TPS), so a macro clicks the same at any speed or frame by frame, and on
// the wall clock elsewhere (the editor). A new attempt starts the timing over.
//
// The game reaches this through gucci::triggerClickAudio (audio/playsound.hpp).
// It stays quiet while the frame-window analyzer or the pathfinder drives the
// game: those inputs are theirs, not the player's. Main thread only.

#include <filesystem>
#include <string>
#include <vector>

namespace gucci::clicks {

    // Saved as the mod's "clicks_<name>" values (enabled, pack, volume,
    // soft_ms, playback, render, p2_separate, p2_pack). The removed click
    // sounds used "click_<name>"; those are not read.
    struct Settings {
        bool enabled = false;
        std::string pack = "Clickbot";
        float volume = 1.f;            // 0 to 2; 1 plays the files as they are
        int softMs = 100;              // quick re-click window; 0 = never soft
        bool duringPlayback = true;    // also click for a macro's inputs
        // Also click while a render is recording. The renderer takes its sound
        // from the game's mix, so the clicks land in the video. Off, as the
        // removed click sounds had it.
        bool inRenders = false;
        bool separateP2 = false;       // player 2 uses p2Pack instead of pack
        std::string p2Pack = "Clickbot";
    };

    // Read from the mod's saved values the first time it is asked for.
    Settings& settings();
    // Write every setting back. Call after changing any of them; a change of
    // pack is picked up on its own (the sounds reload the next time they are
    // needed).
    void saveSettings();

    // <save dir>/clickpacks, created when missing.
    std::filesystem::path packsDir();
    // Every pack there is: the built-in ones first, then the folders in
    // packsDir(). Cached; refresh() looks again.
    std::vector<std::string> const& packNames();
    // Look for packs again and reload the sounds of the ones in use.
    void refresh();

    // How a player's pack loaded, for the settings page. Asking loads the pack
    // when it is not loaded yet.
    struct PackStatus {
        std::string name;        // the pack whose sounds play
        bool missing = false;    // the chosen pack is gone; the built-in one plays instead
        bool builtIn = false;
        bool audioReady = false; // the game's sound system is up
        int clicks = 0;
        int releases = 0;
        int softClicks = 0;
        int softReleases = 0;
        int unreadable = 0;      // sound files the game could not open
    };
    PackStatus status(bool p2);

    // One click from player 1's (or player 2's) pack at the click volume,
    // whatever the switches say: for callers with a switch of their own, like
    // the trainers' metronome, and the settings page's test button.
    void playClick(bool p2);

} // namespace gucci::clicks

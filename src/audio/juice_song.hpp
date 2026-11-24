#pragma once

// Juice's song (Nigel, 2026-11-23): Juice made a short GucciBot parody song,
// and a click on his name on the in-game Credits page plays it (ui/shell.cpp,
// pageCredits). A second click stops it, and so do leaving the page and
// closing the menu. It's clean, so it plays whatever the Explicit content
// switch says. One at a time, and never over BIG BRRRR: a click stops BIG
// BRRRR first, and BIG BRRRR starting stops the song. It plays through GD's
// FMOD at GD's music volume, with the game's own sound held quiet while it
// plays, as BIG BRRRR holds it. It is not a level-complete sound.
//
// The file is resources/juice_song.ogg (mod.json lists it): Ogg Vorbis, mono,
// 48 kHz, 13.08 s, made from the Discord voice message Juice sent (Ogg Opus),
// which GD 2.2081's FMOD won't open (FMOD_ERR_FORMAT, tested in game
// 2026-11-24). GD's own .ogg sound effects are Ogg Vorbis too.

#include <matjson.hpp>

namespace gucci::juicesong {

    // A click on Juice's name: plays the song from the start, or stops it if
    // it's playing.
    void toggle();
    bool playing();

    // The Credits page is being drawn this frame (not the search's hidden
    // index pass).
    void shown();
    // Once a frame, as the menu's frame ends (ui/shell.cpp's frame(), on every
    // way out of it): stops the song when the Credits page wasn't drawn this
    // frame (another page, the menu closed, the bot switched off) or BIG BRRRR
    // is playing, keeps it at GD's music volume, and lets go of it once it has
    // played to the end.
    void frameEnd();

    // For Assistant Access (gucci_get_state's audio.juice_song): playing,
    // plays (this launch), last (what the last play or stop was), and while it
    // plays position_ms, length_ms, volume and FMOD's name for the format.
    matjson::Value state();

} // namespace gucci::juicesong

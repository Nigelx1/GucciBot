#pragma once

// GucciBot (2026-11-15): Share my windows. Nigel's ask (2026-11-11): "one
// button that saves a picture of a macro's frame windows, with the level
// name, the tightest windows and its precision score, ready for Discord or
// YouTube."
//
// The button sits on the Calculate card. It saves a 1600 x 900 PNG in the
// menu's look and the theme in use (ui/share_draw.hpp draws it) to shares/
// in the mod's save folder: the level, the macro, the legend's bands and
// their counts, the tightest windows, and L* (NaN GD's precision number, the
// one the Precision card shows). What's on it is copied on the game's thread
// (quick); reading the fonts, solving L* when the card's is stale, drawing,
// packing and writing the file happen on a thread of their own, so the game
// never waits for it.
//
// Next to the picture, under the same name, a .zip with the macro and its
// sidecars as saved (the .fwac with the windows, the macro path, ...;
// tools/share_bundle.hpp). Nigel (2026-11-15): "an image plus like... a full
// macro+fwc thing u can load up". Saved macros' Import a macro unpacks one
// under a name nothing has, sidecars with it, and selects it. A macro that
// isn't saved gets the picture alone, and the note says why.

#include <string>
#include <vector>

namespace gucci::ui::share {

    // The button, for the Calculate card's row of buttons (the caller puts it
    // on the line). Greyed out with the reason below it when there's nothing
    // to share, and while a picture is being made.
    void button();
    // What the button has to say, under the row: why it can't share, or
    // "Saved to ..." with Open folder (Windows and Mac; on phones the note
    // says where it went), or why the save failed.
    void notes();

    // Why there's nothing to share now, or "" when there is.
    std::string whyNot();
    // Starts a picture, as the button does. "" when it started, else why not.
    std::string start();

    // The last picture: for the menu's note and Assistant Access (gucci_share).
    struct Status {
        bool busy = false;   // one is being made
        bool done = false;   // one has finished (ok or not) this launch
        bool ok = false;
        std::string path;    // where it went, when ok
        std::string folder, pictureName;  // the same, in two parts
        std::string error;   // why not, when not ok
        // The macro file (.zip) next to it.
        bool bundleOk = false;
        std::string bundlePath, bundleName;
        std::string bundleError;  // why there's none (not saved, or it failed)
        int saved = 0;       // pictures saved this launch
        double seconds = 0;  // how long the last one took, start to file
        double gameMs = 0;   // of that, the game's own thread (the copying before the thread starts)
        // What the last picture shows, for a test to check against the game.
        std::string level, details, score;
        std::string warning;  // the red mark, when the windows aren't legit (Card::warning)
        std::vector<std::string> bands;  // "name: count"
        std::vector<std::string> tight;  // "#n kind, p%, w frames, t ms"
    };
    Status const& status();

} // namespace gucci::ui::share

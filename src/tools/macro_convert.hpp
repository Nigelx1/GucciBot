#pragma once

// Other bots' macro formats, read and written, for the Saved macros card's
// Convert, Import and Export (Nigel, 2026-10-19: "full conversion" in the
// game, the same formats as the site's converter). Pure C++, no Geode, so
// tools/run_convert_tests.cpp checks it offline against real macros.
//
// Where the layouts come from: most of the 2.1-era formats are ported from the
// site's converter, itself ported from peony's nat-converter
// (https://github.com/peonii/nat-converter, with her permission, credited);
// GDR from maxnut's GDReplayFormat, which GucciBot links; TcBot's .tcm from
// tcm-rs (MIT, tcbot.pro); Silicate's .slc v2 and v3 from peony's libslc
// (git.puppy.lgbt/silicate/slc); .scbf1 from anticroom's Silicate fork; and
// GucciBot 1.x's old .brrr worked out from a real 1.x macro, never from the
// code that wrote it (that code was ToastyReplay's, and is gone). Every
// reader here is written fresh from the layout. ToastyReplay's own formats
// (.ttr3, .ttrl) are left out on purpose.
//
// Frames are GucciBot's numbering throughout. A format whose bot counts one
// frame behind GucciBot (Silicate's family: GucciBot records at getFrame()+1
// where Silicate records at getFrame()) carries a frameShift, added on read
// and taken off on write.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gucci::convert {

    // One input, in GD's own button numbering.
    struct Input {
        int64_t frame = 0;
        uint8_t button = 1;  // 1 jump, 2 left, 3 right
        bool down = true;
        bool player2 = false;
        double subtick = 0.0;  // 0 <= x < 1: where in its tick (.scbf1 and GucciBot's own)
    };

    // From this frame on, the macro runs at this rate. Macro::tps is the rate it starts at.
    struct TpsChange {
        int64_t frame = 0;
        double tps = 240.0;
    };

    // What every format reads into and writes from.
    struct Macro {
        double tps = 240.0;
        std::vector<Input> inputs;  // in frame order
        std::vector<TpsChange> tpsChanges;
        // The RNG seed, only when GucciBot uses it the way the format's bot
        // did (Silicate's family, and GucciBot's own).
        uint64_t seed = 0;
        bool hasSeed = false;
        std::string name;
        std::string levelName;
        int64_t levelId = 0;
        std::string author;
        std::string bot;  // the bot that wrote it, when the file says
        bool platformer = false;
        // What didn't carry over, or needs knowing, in words for the menu.
        std::vector<std::string> notes;
    };

    struct Format {
        std::string_view id;    // "gdr", "slc3", ...
        std::string_view name;  // for the menu
        std::string_view ext;   // what an export is saved as
        bool canWrite;
        // Keeps the jump button only, so a platformer macro's Left and Right
        // don't survive the trip.
        bool jumpOnly;
        // Added to every frame read, taken off every frame written.
        int frameShift;
        // True when reading was checked against real macros from the bot.
        bool checked;
    };

    // Every format, in the order the menu lists them.
    std::vector<Format> const& formats();
    Format const* find(std::string_view id);

    // Whether a file name ends in an extension some format here uses. .brrr
    // counts: GucciBot 1.x's old macros share it with GucciBot's own.
    bool looksImportable(std::string_view fileName);
    // The file's format from its first bytes, then its name. Null when nothing fits.
    Format const* detect(std::vector<uint8_t> const& bytes, std::string_view fileName);
    // GucciBot's own format (GBR6), by its magic.
    bool isGucciBot(std::vector<uint8_t> const& bytes);

    struct ReadResult {
        Format const* format = nullptr;
        Macro macro;
        std::string error;  // empty when it read
        bool ok() const {
            return format && error.empty();
        }
    };
    // Reads a file of any format here; the frames come back shifted to
    // GucciBot's numbering, plus extraShift (the menu's own nudge).
    ReadResult read(std::vector<uint8_t> const& bytes, std::string_view fileName, int extraShift = 0);

    struct WriteResult {
        std::vector<uint8_t> bytes;
        std::vector<std::string> notes;  // what the format couldn't keep
        std::string error;               // empty when it wrote
    };
    WriteResult write(Macro const& m, Format const& f);

    // GucciBot's own format, both ways. The name goes in the header.
    std::vector<uint8_t> toGucciBot(Macro const& m);
    bool fromGucciBot(std::vector<uint8_t> const& bytes, Macro& out, std::string& error);

} // namespace gucci::convert

#pragma once

// GucciBot (2026-11-17): a light read of a macro's windows file (the .fwac
// sidecar Calculate writes) for code that only wants each click's window, not
// the analyzer: the Trainer's lane draws a note's size from it. Defined at the
// end of framewindow.cpp, beside the reader Calculate itself uses.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gucci::fwfile {

    struct Window {
        uint32_t frame = 0;   // the input's macro frame
        bool player2 = false;
        bool release = false;
        int window = 0;       // whole ticks
        float subframe = 0.f; // a CBF count, in ticks (0: none)
        bool cbf = false;
        int64_t low = 0;      // shifts that still lived (ticks; CBF slots when cbf)
        int64_t high = 0;
        bool unbounded = false;
        bool hidden = false;
        bool desynced = false;
        bool manual = false;
    };

    // False when the file is missing or doesn't read. `tps`: the file's.
    bool readWindows(std::filesystem::path const& path, std::vector<Window>& out, double& tps);

} // namespace gucci::fwfile

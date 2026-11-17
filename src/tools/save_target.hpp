#pragma once
// GucciBot (2026-11-16): whether a Save may write its file at once or has to
// ask first, and the name "Save as" offers. Pure, so run_tests.bat checks it
// (tools/run_savetarget_tests.cpp).
//
// Nigel's rule ("we gotta be able to append stuff, so the most intuitive
// option that keeps both"): the macro in memory remembers the file it came
// from (loaded, imported or last saved to). Saving to that file, after
// carrying on recording from it, editing it or not changing it at all, writes
// over it as always. Saving to a file that's there but isn't that one (a fresh
// recording with an old name still in the Name box, another macro's name typed
// in) asks first: Replace it, save as "<name> 2", or Cancel. A file that isn't
// there yet is written at once.

#include <string>

namespace gucci::savetarget {

    enum class Decision {
        Write, // write the file now (a new file, or the macro's own)
        Ask,   // a different macro has the file: ask before replacing it
    };

    // targetExists: the file this Save writes is there already.
    // targetIsSource: it is the file the macro in memory came from (false
    // when the macro came from none: a fresh recording).
    inline Decision decide(bool targetExists, bool targetIsSource) {
        if (!targetExists || targetIsSource)
            return Decision::Write;
        return Decision::Ask;
    }

    // "<name> 2", "<name> 3"...: the first one `taken` says no to. `taken`
    // checks a name against every theme's extension in the replays folder
    // (GucciEngine::findMacroFile), since the list shows names, not files.
    template <class Taken>
    std::string nextFreeName(std::string const& name, Taken&& taken) {
        for (int n = 2; n < 100000; ++n) {
            std::string candidate = name + " " + std::to_string(n);
            if (!taken(candidate))
                return candidate;
        }
        return name + " " + std::to_string(100000);
    }

} // namespace gucci::savetarget

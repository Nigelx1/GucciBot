#pragma once

// Replace All -- swaps every object of one id for another in the level editor.
// Ported from Absense (editor/tools.cpp, by Absent). Its Macro Buffing is the
// same feature as Silicate's trail buffer, which GucciBot took instead; this is
// the one editor tool it has that Silicate does not.

#include <string>

namespace gucci::editortools {

    struct ReplaceResult {
        int replaced = 0;
        bool ok = false;
        std::string message;
    };

    // Every object of `fromId` becomes `toId`, keeping everything else about it
    // (position, rotation, scale, flips, groups, colours, z-order, layer). The
    // replacements are left selected. Undoable in two presses: the first removes
    // the new objects, the second brings the originals back.
    ReplaceResult replaceAll(int fromId, int toId);

} // namespace gucci::editortools

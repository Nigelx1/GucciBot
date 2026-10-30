#pragma once

// The legacy BRR macro format. GucciBot saves GBR6. Old 1.x .brrr files are
// read by the macro converter (tools/macro_convert.cpp, readBrr, rebuilt from
// a real 1.x macro in build -eu), and the macro list converts them to GBR6
// instead of listing them; a trainer macro picked in 1.x is read through the
// same reader (engine_core.cpp, loadTrainerMacroData). Only the version number
// is left here, for the launch self-check.

namespace gucci {

    inline constexpr int BRR_FORMAT_VERSION = 4;

} // namespace gucci

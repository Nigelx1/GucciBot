#pragma once

// Stands in for Absense's settings/settings.hpp and shared/value/value.hpp.
// SLSettings, SLValue and SLValuePtr live in analysis/ac/shim.hpp, which
// anticroom's analyzer already uses for the same Silicate names; Absense's
// settings blocks are hung on SLSettings there (absense/compat/settings_types.hpp).
#include "analysis/ac/shim.hpp"

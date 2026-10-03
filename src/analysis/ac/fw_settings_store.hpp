#pragma once

// Saving and loading the frame-window analyzer's settings
// (SLSettings::get()->frameWindow, anticroom's FrameWindowSettings in shim.hpp).
//
// shim.hpp's SLValue deliberately leaves persistence to GucciBot. Since build
// -bz nothing did it, so every Calculate setting went back to its default on
// each launch. This is that missing half: every field the analyzer reads is in
// one of the tables in fw_settings_store.cpp, as (saved key, member pointer,
// lowest, highest), and load() and save() walk the tables, so a field that is
// in a table cannot be loaded and then forgotten on save, or the other way
// round. The same limits bound the menu's controls (see limits()), so what the
// menu allows and what a load accepts are one set of numbers.
//
// Keys are fwac_<field>. That is the prefix the pre-bz build saved these same
// fields under, so a player's settings from that build come back as they were.
// The plain fw_ prefix is not free: the old GucciBot analyzer (removed
// 2026-09-20) left fw_recovery_range, fw_tiers, fw_circle_skin and others in
// saved.json with different meanings and formats.

#include "analysis/ac/shim.hpp"

#include <cstdint>

namespace gucci::fwstore {

    // Lowest and highest value a numeric setting may hold.
    template <typename T>
    struct Limits {
        T lo;
        T hi;
    };

    Limits<int> limits(int FrameWindowSettings::* member);
    Limits<int64_t> limits(int64_t FrameWindowSettings::* member);
    Limits<float> limits(float FrameWindowSettings::* member);
    Limits<double> limits(double FrameWindowSettings::* member);

    // Reads every stored field (and the bands) into SLSettings, clamped.
    // Missing keys keep the struct's defaults. Run once at mod load.
    void load();

    // Clamps every field into its limits and writes all of them, bands too.
    // Cheap (Geode keeps saved values in memory until the game closes), so
    // the menu calls it after any change rather than tracking which field.
    void save();

    // Everything back to anticroom's defaults (bands included), then saved.
    void resetAll();

    // Only the bands back to their defaults, then saved.
    void resetBands();

    // Smallest and largest window a band edge may be set to.
    inline constexpr int kBandWindowMax = 999;

} // namespace gucci::fwstore

#pragma once

// gucci_test_set (mcp_tools.cpp): a test run's settings, in effect for this
// launch only and never saved.
//
// A test (tools/assist.py) needs settings Nigel hasn't got on: Force
// platformer, a render folder in the scratchpad, a muted game at night. The
// tool puts one whitelisted setting into effect through the setter its menu
// row uses (the field, the saved value, any side effect such as a TPS change
// recorded into the macro), and refuses unless safe mode is forced for the
// launch (hacks/safemode.hpp).
//
// What keeps it off the disk: menu rows write the save container the moment
// they change, several features save whole structs (Click Indicators writes
// every ci_* key) and some read the container live (render_*).
// So the override is shielded where the file is written, not where the value
// is set: every saved.json write puts the user's own value (or no key, when
// there wasn't one) under each overridden key, writes, and puts the test's
// back. That's shell.cpp's writeSavedValues (around its saveData) and a
// DataSaved handler, which Geode fires inside every Mod::saveData before it
// writes saved.json (loader/src/loader/ModImpl.cpp, saveData) - Geode's own
// save when the game closes among them. reset() puts every original back in
// memory and in the container; a restart ends it all anyway.

#include <matjson.hpp>
#include <string>

namespace gucci::testset {

    // A setting is overridden right now.
    bool active();

    // Around a write of saved.json: the user's values in for every overridden
    // key, then the test's back. shieldForWrite returns false when it did
    // nothing (no override, or already shielded), and then the matching
    // unshield must not be called by that caller.
    bool shieldForWrite();
    void unshieldAfterWrite();

    // The tool's three forms. They throw mcp::ToolError on a refusal.
    matjson::Value set(std::string const& key, matjson::Value const& value);
    matjson::Value reset();
    matjson::Value list();

    // The keys overridden right now, for get_state.
    matjson::Value heldKeys();

    // test_mute: the game's FMOD master group at volume 0 for this launch.
    bool muted();

    // fwac_conflict_test_id: one more mod id for Calculate's conflict check
    // (GucciEngine::analyzerConflicts), looked up with the real isModLoaded.
    // Empty unless a test set it.
    std::string const& conflictTestId();

} // namespace gucci::testset

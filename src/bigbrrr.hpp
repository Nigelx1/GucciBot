#pragma once

#include <Geode/Geode.hpp>
#include <filesystem>

using namespace geode::prelude;

namespace FMOD { class Sound; class Channel; }

// "BIG BRRRR" -- a pure joke toggle, unrelated to any bot functionality.
// Loops resources/big_brrr.mp3, bundled with the mod (Mod::get()->getResourcesDir(),
// same pattern as the frame-window tracker's default sound in framewindow.cpp) so
// it works out of the box with no manual setup. Dropping a file in the save-dir
// brrr/ folder (same disk-folder convention as ClickSoundManager's click packs)
// overrides it without needing a rebuild. While enabled, makes the menu bounce.
// Never touches the Jupiter tab -- see the jupiterActive guard at the bounce
// call sites in gui.cpp.
class BigBrrrManager {
public:
    static BigBrrrManager* get();

    // Skips the track's slow intro -- playback starts here instead of 0:00.
    static constexpr double kStartOffsetSec = 20.0;
    // Track's actual BPM, used to line up the menu bounce (see
    // applyBigBrrrBounce in gui.cpp) to the beat.
    static constexpr double kBpm = 140.0;

    bool enabled = false;

    void setEnabled(bool on);
    std::filesystem::path getBrrrDir() const;
    void openBrrrFolder();
    bool hasFile() const; // true if a save-dir override is present (bundled default always works)

private:
    FMOD::Sound*   sound   = nullptr;
    FMOD::Channel* channel = nullptr;
    bool audioMuteHeld = false; // whether this manager currently holds a GameAudioMute lock
    void start();
    void stop();
};

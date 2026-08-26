#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <atomic>
#include <filesystem>

using namespace geode::prelude;

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
    // The drop is at 20 and 11/15 seconds. Nigel, 2026-08-23: the Maybach
    // theme gets its own track (big_brrr_maybach.mp3, 75bpm, no intro to
    // skip), so these are no longer fixed constants -- both check the
    // active theme at call time. Implemented in bigbrrr.cpp (not here) so
    // this header doesn't need to depend on gui.hpp's MenuInterface.
    static double kStartOffsetSec();
    // Track's actual BPM, used to line up the menu bounce (see
    // applyBigBrrrBounce in gui.cpp) to the beat.
    static double kBpm();

    bool enabled = false;
    // Nigel, 2026-08-24: GrizzleyBot's track is heavily bass-boosted --
    // "vibrate the menu like a speaker on the bassy parts," and explicitly
    // fine with it being intense ("annoying on purpose if its accurate").
    // Separate opt-out from `enabled` itself since the shake is a lot more
    // aggressive than the existing beat-synced bounce.
    bool shakeEnabled = false;
    // Max alpha dip at full bass intensity (0 = no flicker, 1 = fully
    // transparent on the hardest hits). Nigel, 2026-08-24: wanted this
    // adjustable rather than the flat 50% dip it shipped with -- see
    // bigBrrrFlickerAlpha in gui.cpp, which reads this instead of a
    // hardcoded constant now. Not persisted across restarts, matching
    // shakeEnabled's own existing (also unpersisted) behavior.
    float flickerIntensity = 0.5f;

    void setEnabled(bool on);
    std::filesystem::path getBrrrDir() const;
    void openBrrrFolder();
    bool hasFile() const; // true if a save-dir override is present (bundled default always works)

    // Smoothed [0,1] live bass-energy reading, safe to poll every ImGui
    // frame from the main thread. Fed by a real FMOD DSP tap on the actual
    // playback channel (bassDspCallback, bigbrrr.cpp) -- not a BPM guess
    // like applyBigBrrrBounce's sine wave, genuine per-buffer signal.
    float getBassLevel() const { return rawBassLevel.load(); }

private:
    FMOD::Sound*   sound   = nullptr;
    FMOD::Channel* channel = nullptr;
    FMOD::DSP*     bassDsp = nullptr;
    std::atomic<float> rawBassLevel{0.f}; // written on FMOD's mixer thread, read on the main thread
    bool audioMuteHeld = false; // whether this manager currently holds a GameAudioMute lock
    void start();
    void stop();
    static FMOD_RESULT F_CALLBACK bassDspCallback(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
};

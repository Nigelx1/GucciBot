#pragma once

#include <Geode/Geode.hpp>
#include <filesystem>

using namespace geode::prelude;

namespace FMOD { class Sound; class Channel; }

// "BIG BRRRR" -- a pure joke toggle, unrelated to any bot functionality.
// Loops whatever audio file Nigel drops in the brrr/ folder (same
// disk-folder convention as ClickSoundManager's click packs) and, while
// enabled, makes the menu bounce. Never touches the Jupiter tab -- see the
// jupiterActive guard at the bounce call sites in gui.cpp.
class BigBrrrManager {
public:
    static BigBrrrManager* get();

    bool enabled = false;

    void setEnabled(bool on);
    std::filesystem::path getBrrrDir() const;
    void openBrrrFolder();
    bool hasFile() const;

private:
    FMOD::Sound*   sound   = nullptr;
    FMOD::Channel* channel = nullptr;
    void start();
    void stop();
};

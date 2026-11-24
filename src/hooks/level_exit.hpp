#pragma once

// GucciBot (2026-11-23, 2.0.1): what GucciBot does as a level closes, in two
// halves around GD's own exit. The PlayLayer::onQuit hook (hook_playlayer.cpp)
// runs them around GD's quit. GD's Edit exits never reach PlayLayer::onQuit,
// so their hooks (hook_edit_exit.cpp) run them around GD's goEdit. Each step
// is safe to run again.

class PlayLayer;

namespace gucci::levelexit {

    // While the level is still the current one, before GD's exit.
    void beforeQuit(PlayLayer* pl);

    // After GD's exit has begun (the layer itself goes at the end of the fade).
    void afterQuit();

} // namespace gucci::levelexit

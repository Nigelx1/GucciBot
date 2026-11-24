#pragma once

class PlayLayer;

namespace gucci {

    namespace gbju {
        bool isJupiterLevel(PlayLayer* pl);

        void renderJupiterGhost(PlayLayer* pl);
        void notifyJupiterAttemptEnded();
        // Forgets the best attempt's ghost (trainers::resetStats).
        void resetBestGhost();

        void syncClickBarMusic(bool active, bool paused, double posSec);
        void stopClickBarMusic();

        // The level is closing: the ghosts' node goes and the music stops
        // (it unmutes GD's own sound). The PlayLayer::onQuit hook's work, and
        // an Edit exit's (hooks/hook_edit_exit.cpp).
        void leaveLevel();
    } // namespace gbju

} // namespace gucci

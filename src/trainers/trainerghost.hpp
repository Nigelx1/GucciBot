#pragma once

class PlayLayer;

namespace gucci {

    namespace gbtr {
        bool isTrainerLevel(PlayLayer* pl);

        void renderTrainerGhost(PlayLayer* pl);
        void notifyTrainerAttemptEnded();
        // Forgets the best attempt's ghost (trainers::resetStats).
        void resetBestGhost();

        void syncTrainerClickBarMusic(bool active, bool paused, double posSec);
        void stopTrainerClickBarMusic();

        // The level is closing: the ghosts' node goes and the music stops
        // (it unmutes GD's own sound). The PlayLayer::onQuit hook's work, and
        // an Edit exit's (hooks/hook_edit_exit.cpp).
        void leaveLevel();
    } // namespace gbtr

} // namespace gucci

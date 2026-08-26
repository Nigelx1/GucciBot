#pragma once

class PlayLayer;

namespace gucci {

    namespace gbtr {
        bool isTrainerLevel(PlayLayer* pl);

        void renderTrainerGhost(PlayLayer* pl);
        void notifyTrainerAttemptEnded();

        void syncTrainerClickBarMusic(bool active, bool paused, double posSec);
        void stopTrainerClickBarMusic();
    } // namespace gbtr

} // namespace gucci

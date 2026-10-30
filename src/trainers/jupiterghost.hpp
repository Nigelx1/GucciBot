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
    } // namespace gbju

} // namespace gucci

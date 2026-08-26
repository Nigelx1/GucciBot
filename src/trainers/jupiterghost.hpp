#pragma once

class PlayLayer;

namespace gucci {

    namespace gbju {
        bool isJupiterLevel(PlayLayer* pl);

        void renderJupiterGhost(PlayLayer* pl);
        void notifyJupiterAttemptEnded();

        void syncClickBarMusic(bool active, bool paused, double posSec);
        void stopClickBarMusic();
    } // namespace gbju

} // namespace gucci

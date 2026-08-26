#pragma once

#include <Geode/Geode.hpp>
#include <vector>

using namespace geode::prelude;

struct GamemodeCalibration {
    float leadMs = 0.f;
    float jitterMs = 0.f;
    int sampleCount = 0;
    bool guideEnabled = true;
};

enum GamemodeIndex {
    GM_Cube = 0,
    GM_Ship,
    GM_Ball,
    GM_Ufo,
    GM_Wave,
    GM_Robot,
    GM_Spider,
    GM_Count
};

class CalibrationService {
public:
    static CalibrationService& get();

    GamemodeCalibration modes[GM_Count];

    bool active = false;
    int calibratingMode = GM_Cube;
    int repsTarget = 16;
    int repsDone = 0;
    float intervalSec = 0.65f;
    float timeToNextCue = 0.f;
    bool waitingForClick = false;
    float timeSinceCue = 0.f;
    std::vector<float> samples;

    void start(int gamemodeIndex);
    void cancel();
    void tick(float dt);
    void onRealClick();
    void resetMode(int gamemodeIndex);

    void load();
    void save() const;

    static int gamemodeIndexFor(PlayerObject* player);
    static const char* gamemodeName(int index);

private:
    void finish();
};

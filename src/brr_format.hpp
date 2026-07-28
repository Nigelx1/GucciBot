#ifndef _brr_format_hpp
#define _brr_format_hpp

#include <Geode/Geode.hpp>

#include <array>

enum class AccuracyMode { Vanilla = 0, CBF = 1, CBS = 2 };

struct MacroAction {
    int   frame      = 0;
    int   button     = 0;
    bool  pressed    = false;
    bool  down       = false;
    bool  player2    = false;
    float stepOffset = 0.f;

    MacroAction() = default;
        MacroAction(int f, int b, bool p2, bool press, float off)
        : frame(f), button(b), pressed(press), down(press), player2(p2), stepOffset(off) {}
};

struct PlayerStateBundle {
    struct Motion {
        cocos2d::CCPoint position;
        float  rotation = 0.f;
        double verticalVelocity = 0.0;
        double preSlopeVerticalVelocity = 0.0;
        double horizontalVelocity = 0.0;
    } motion;
    struct Flags {
        bool upsideDown = false;
        bool holdingLeft = false;
        bool holdingRight = false;
        bool platformer = false;
        bool dead = false;
        std::array<bool, 4> buttonHolds = {};
    } flags;
    struct Environment {
        double gravity = 0.0;
        bool dualContext = false;
        bool twoPlayerContext = false;
    } environment;
};

struct AnchorRngState {
    bool      locked        = false;
    uint32_t  seed          = 0;
    uintptr_t fastRandState = 0;
};

struct PlaybackAnchor {
    int  tick       = 0;
    bool hasPlayer2 = false;
    PlayerStateBundle player1;
    PlayerStateBundle player2;
    uint8_t player1LatchMask = 0;
    uint8_t player2LatchMask = 0;
    AnchorRngState rng;
};

#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace geode::prelude;

#define BRR_FORMAT_VERSION 4
#define BRR_MAGIC "BRR"

enum BRRFlags : uint32_t {
    BRR_FLAG_ACCURACY_CBS = 1 << 0,
    BRR_FLAG_FROM_START_POS = 1 << 1,
    BRR_FLAG_PLATFORMER = 1 << 2,
    BRR_FLAG_TWO_PLAYER = 1 << 3,
    BRR_FLAG_RNG_LOCKED = 1 << 4,
    BRR_FLAG_ACCURACY_CBF = 1 << 5,
    BRR_FLAG_HAS_DEATH_FRAMES = 1 << 6,
};

struct BRRInput {
    int32_t tick = 0;
    uint8_t actionType = 0;
    uint8_t flags = 0;
    float stepOffset = 0.0f;

    bool isPlayer2() const { return (flags & 0x01) != 0; }
    bool isPressed() const { return (flags & 0x02) != 0; }

    void setPlayer2(bool value) { flags = (flags & ~0x01) | (value ? 0x01 : 0x00); }
    void setPressed(bool value) { flags = (flags & ~0x02) | (value ? 0x02 : 0x00); }
};

struct BRRCheckpoint {
    int32_t tick = 0;
    uint64_t rngState = 0;
    int32_t priorTick = 0;
};

class BRRMacro {
public:
    std::string author;
    std::string name;
    std::string persistedName;
    std::string levelName;
    int32_t levelId = 0;
    double framerate = 240.0;
    double duration = 0.0;
    uint32_t gameVersion = 0;
    float startPosX = 0.f;
    float startPosY = 0.f;
    bool recordedFromStartPos = false;
    AccuracyMode accuracyMode = AccuracyMode::Vanilla;
    bool platformerMode = false;
    bool twoPlayerMode = false;
    bool rngLocked = false;
    uint32_t rngSeed = 0;
    int64_t recordTimestamp = 0;
    int32_t savedAnchorInterval = 240;
    std::vector<BRRInput> inputs;
    std::vector<PlaybackAnchor> anchors;
    std::vector<BRRCheckpoint> checkpoints;
    std::vector<int32_t> deathFrames;
    std::vector<int32_t> attemptStartTicks;

    void recordAction(int tick, int button, bool player2, bool pressed, float offset);
    void recordAnchor(int tick, PlayerObject* p1, PlayerObject* p2, bool isPlatformer, bool isDual = true);
    void truncateAfter(int tick);
    std::vector<uint8_t> serialize() const;
    static BRRMacro* deserialize(std::vector<uint8_t> const& data);
    void persist();
    void persist(AccuracyMode mode, int anchorInterval) {
        accuracyMode = mode;
        savedAnchorInterval = anchorInterval;
        persist();
    }
    static BRRMacro* loadFromDisk(std::string const& filename);
    std::vector<MacroAction> toMacroActions() const;
        bool isDeathFrame(int32_t tick, size_t index) const {
        if (index >= deathFrames.size()) return false;
        return std::abs(deathFrames[index] - tick) <= 5;
    }
        bool isAnyDeathFrame(int32_t tick) const {
        for (auto df : deathFrames)
            if (std::abs(df - tick) <= 5) return true;
        return false;
    }
};

#endif

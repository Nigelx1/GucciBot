#ifndef GPU_PROTOCOL_HPP
#define GPU_PROTOCOL_HPP

// Absense - the protocol between the mod and absense-gpu.exe.
//
// The graphics card scores candidate input scripts; it never approves one.
// The mod sends the level around the player and the player's own physics
// numbers, the card runs tens of thousands of scripts in parallel with a
// simplified model of the game, and sends back how long each one lasted.
// The mod then runs the best few through the exact simulation (the game's
// own physics on a copy of the player), which is the only thing allowed to
// say yes. An approximate model can therefore only ever cost time, never
// correctness.
//
// The card's own ideas are not sent tick by tick: every one is a small
// descriptor the two sides expand the same way (see scriptInput), so a batch
// of 65536 ideas is a megabyte instead of sixty. The mod's own ideas are
// already lists of ticks and cannot be squeezed into a descriptor, so those
// go as a RawBatch: four bits a tick (see packRawTick).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace absense::gpu {

constexpr uint32_t kMagic = 0x41475055;  // "AGPU"
// 2: objects carry how fast they are moving, scripts hold 32 events, and a
// batch can spell its scripts out tick by tick. The two sides are built and
// shipped together (CMake add_dependencies, mod.json resources), so this is
// a label for the size checks below rather than something to negotiate.
constexpr uint32_t kVersion = 2;

enum class Message : uint32_t {
    Hello = 1,     // mod -> app, answered with HelloReply (device name, limits)
    Level = 2,     // mod -> app: the objects around the player
    Batch = 3,     // mod -> app: a player state and N script descriptors
    Results = 4,   // app -> mod: one Result per descriptor
    HelloReply = 5,
    Error = 6,
    Bye = 7,
    RawBatch = 8,  // mod -> app: a player state and N scripts spelled out tick by tick (the mod's own ideas); answered with Results
};

struct FrameHeader {
    uint32_t magic = kMagic;
    Message type = Message::Hello;
    uint32_t bytes = 0;  // payload after this header
};

// ------------------------------------------------------------------ level

// What the shader knows about an object. Everything is an axis-aligned box
// (a circle when radius > 0); rotation is baked into the box by the mod.
enum ObjectKind : uint32_t {
    kSolid = 0,      // stops the player; landing on top is fine
    kHazard = 1,     // kills on contact with the player's whole hitbox (the inner one is for solids)
    kJumpOrb = 2,    // yellow-ish: a press while touching gives an impulse
    kGravityOrb = 3, // a press while touching flips gravity
    kJumpPad = 4,    // touching gives an impulse, no press needed
    kGravityPad = 5,
    kGravityPortalDown = 6,
    kGravityPortalUp = 7,
    kSlope = 8,      // treated as solid, but the player may ride up it
    kSpeedPortal = 9,     // power: the player speed it sets (0.7, 0.9, 1.1, 1.3, 1.6), from the next tick
    kModePortal = 10,     // power: the mode it sets (PlayerState::mode numbering)
    kSizePortal = 11,     // power: the vehicle size it sets (0.6 or 1.0)
    kGravityToggle = 12,  // flips gravity
    kDropOrb = 13,        // black orb: a press while touching drives the player down
    kSpiderOrb = 14,      // a press while touching turns gravity the way it faces and carries the player across
    kSpiderPad = 15,      // the same without a press
    kDashOrb = 16,        // power: how far the dash rises per unit of x; it lasts while the button is held
    kGravityDashOrb = 17, // the same after flipping gravity
};

// Object::pad carries these. The shader works out which orb or pad of a kind
// it is from Object::power (1.0 yellow, 0.72 pink, 1.38 red jump orbs; 0.8
// blue, 1.0 green gravity orbs; 1.0 yellow, 0.65 pink, 1.25 red pads), since
// the game's impulse depends on that and the mode together.
constexpr uint32_t kFlagMultiActivate = 1;  // a ring the game fires again on every new press while it is touched
constexpr uint32_t kFlagFacingDown = 2;     // a gravity pad, spider orb or spider pad that faces down

struct Object {
    float minX, minY, maxX, maxY;
    float radius;   // > 0: a circle centred in the box (saw blades)
    float power;    // orb / pad strength as a multiple of the jump impulse, or what the kind says
    uint32_t kind;
    uint32_t flags;  // kFlag... bits
    // How far it moves in one tick: a move trigger was running it when the
    // slice was taken, and the shader carries it on at that step exactly as
    // the exact simulation does (trajectory.cpp, MovingObjects::place).
    float vx = 0.0f, vy = 0.0f;
};

struct LevelHeader {
    uint32_t objectCount = 0;
    float minX = 0.0f;  // the slice of level these objects cover
    float maxX = 0.0f;
    // The furthest ahead a batch can ask about these objects, in ticks: a
    // moving one is put in every column it can sweep over that many ticks, so
    // the shader still finds it there (absense-gpu.exe, bucketLevel).
    uint32_t sweepTicks = 0;
};

// ------------------------------------------------------------------ batch

// The player's own numbers, taken from the live game so the shader does not
// have to guess at what a mode or a speed portal does.
struct PlayerState {
    float x = 0.0f, y = 0.0f;
    float yVelocity = 0.0f;
    float gravity = 0.0f;        // the player's m_gravity
    float gravityMod = 1.0f;
    float yStart = 0.0f;         // the jump impulse (m_yStart)
    float playerSpeed = 1.0f;    // units of x per tick come from this (see unitsPerSecond)
    float speedMultiplier = 1.0f;
    float vehicleSize = 1.0f;    // 1 normal, 0.6 mini
    float dt = 1.0f / 240.0f;
    uint32_t mode = 0;           // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
    uint32_t upsideDown = 0;
    uint32_t onGround = 0;
    uint32_t held = 0;           // the button as it is now
    // Ticks from when the slice's objects were placed to this state: a kept
    // start or a decision made ahead stands at another tick than the slice
    // was taken at, and the moving objects are carried that far (negative:
    // back). The exact simulation does the same (MovingObjects::origin).
    int32_t moveOrigin = 0;
    uint32_t pad1 = 0;
};

// Units of x a second at each portal speed: the game's own rate (the table
// bot/updater.cpp getSSB uses; 311.58 / 240 = 1.298 a tick is what the
// PHYSICS lines of the developer logs show at 0.9). It is playerSpeed times
// the game's m_speedMultiplier times 60, so playerSpeed * speedMultiplier *
// 5.978 counts the x velocity constant twice.
inline float unitsPerSecond(float playerSpeed) {
    if (playerSpeed < 0.8f) return 251.1601f;
    if (playerSpeed < 1.0f) return 311.5801f;
    if (playerSpeed < 1.2f) return 387.4201f;
    if (playerSpeed < 1.45f) return 468.0002f;
    return 576.0002f;
}

// One idea. The shader and the mod expand these the same way.
enum ScriptKind : uint32_t {
    kKeep = 0,       // whatever the button is now, all the way
    kTapAt = 1,      // a: the tick of a press let go at once
    kHoldFrom = 2,   // a: the tick it starts holding (or lets go, when held)
    kHoldFor = 3,    // a: the tick, b: how many ticks it holds
    kDoubleAt = 4,   // a: the tick of two presses inside one tick
    kOrbSpam = 5,    // a: the first tick, b: how many press/release pairs
    kRandom = 6,     // a: a seed, b: how many events
    kEvents = 7,      // a list of events: what a sequence of jumps looks like
};

// An event of a kEvents script: which tick, and what happens on it.
enum EventAction : uint32_t {
    kEventTap = 0,      // press and let go inside the tick
    kEventHold = 1,     // press and keep holding
    kEventRelease = 2,  // let go
    kEventDouble = 3,   // two presses inside the tick
    kEventOrb = 4,      // press, let go, press - let go on the next tick
};
// Events are kept in tick order (each goes at least two ticks after the last);
// the shader relies on it. Eight of them made three-input ideas at most, and
// an orb corridor or a ship stretch over a whole horizon needs a dozen.
constexpr uint32_t kMaxEvents = 32;

inline uint32_t makeEvent(uint32_t tick, uint32_t action) { return (tick << 3) | (action & 7u); }
inline uint32_t eventTick(uint32_t e) { return e >> 3; }
inline uint32_t eventAction(uint32_t e) { return e & 7u; }

struct Script {
    uint32_t kind = kKeep;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t count = 0;                  // events in use (kEvents)
    uint32_t events[kMaxEvents] = {};
};

struct BatchHeader {
    PlayerState player;
    uint32_t scriptCount = 0;
    uint32_t ticks = 0;  // how many ticks each script is run for
    uint32_t pad0 = 0, pad1 = 0;
};

struct Result {
    uint32_t survived = 0;  // whole ticks before it hit something
    float x = 0.0f;
    float y = 0.0f;
    uint32_t flags = 0;  // 1: reached the end of the slice
};

// A RawBatch: this header, then scriptCount * wordsPerScript words. Tick t of
// a script is the four bits at (t % 8) * 4 of its word t / 8: the presses on
// the tick (0-2) in the low two bits, the button after the tick in the third
// - the mod's own TickInput, so its ideas are scored exactly as written
// rather than as the nearest descriptor.
struct RawBatchHeader {
    PlayerState player;
    uint32_t scriptCount = 0;
    uint32_t ticks = 0;
    uint32_t wordsPerScript = 0;  // (ticks + 7) / 8
    uint32_t pad0 = 0;
};

inline void packRawTick(uint32_t* words, uint32_t tick, uint32_t presses, bool held) {
    const uint32_t nibble = std::min<uint32_t>(presses, 3u) | (held ? 4u : 0u);
    words[tick >> 3] |= nibble << ((tick & 7u) * 4u);
}

// The expansion both sides share: what the button does on `tick` under
// `script`, given the button state before it. Returns the presses on this
// tick and sets `held` to the state after it.
inline uint32_t scriptInput(const Script& s, uint32_t tick, uint32_t startHeld, uint32_t& held) {
    uint32_t presses = 0;
    switch (s.kind) {
        case kTapAt:
            if (tick == s.a) {
                presses = 1;
                held = 0;
            }
            break;
        case kHoldFrom:
            if (tick == s.a) {
                if (startHeld) {
                    held = 0;
                } else {
                    presses = 1;
                    held = 1;
                }
            }
            break;
        case kHoldFor:
            if (tick == s.a) {
                presses = startHeld ? 0u : 1u;
                held = 1;
            } else if (tick == s.a + s.b) {
                held = 0;
            }
            break;
        case kDoubleAt:
            if (tick == s.a) {
                presses = 2;
                held = 0;
            }
            break;
        case kOrbSpam:
            if (tick >= s.a && tick < s.a + s.b * 2u) {
                const uint32_t phase = (tick - s.a) & 1u;
                if (phase == 0u) {
                    presses = 2;
                    held = 1;
                } else {
                    held = 0;
                }
            }
            break;
        case kRandom: {
            // The same cheap hash on both sides: an event every few ticks.
            uint32_t h = tick * 374761393u + s.a * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            if ((h % 64u) < s.b) {
                if (h & 1u) {
                    presses = 1;
                    held = 0;
                } else {
                    presses = held ? 0u : 1u;
                    held = held ? 0u : 1u;
                }
            }
            break;
        }
        case kEvents: {
            for (uint32_t i = 0; i < s.count && i < kMaxEvents; i++) {
                if (eventTick(s.events[i]) != tick) continue;
                switch (eventAction(s.events[i])) {
                    case kEventTap:
                        presses += 1;
                        held = 0;
                        break;
                    case kEventHold:
                        if (!held) presses += 1;
                        held = 1;
                        break;
                    case kEventRelease:
                        held = 0;
                        break;
                    case kEventDouble:
                        presses += 2;
                        held = 0;
                        break;
                    case kEventOrb:
                        presses += 2;
                        held = 1;
                        break;
                    default:
                        break;
                }
            }
            // An orb unit lets go on the tick after it.
            for (uint32_t i = 0; i < s.count && i < kMaxEvents; i++) {
                if (eventAction(s.events[i]) == kEventOrb && eventTick(s.events[i]) + 1 == tick) held = 0;
            }
            break;
        }
        case kKeep:
        default:
            break;
    }
    return presses;
}

}  // namespace absense::gpu

#endif  // GPU_PROTOCOL_HPP

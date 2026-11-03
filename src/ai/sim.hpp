#pragma once

// The AI pilot's own access to GD's physics (stage A of the AI pilot build,
// 2026-11-02). One session of the simulated copies of the player, kept open
// tick by tick: begin it from a captured start, step one tick with player 1's
// button as the AI wants it, save and restore the whole state (a snapshot is
// a few microseconds), read both players, see a death or the level's end,
// end it. The copies are GD's own PlayerObjects stepping GD's own physics
// (Absense's copies, src/absense/trajectory); nothing here calls the old
// search (no planner, no route failure tables, no phantoms, no distrusted
// killers - none of them is read while a session is open).
//
// Main thread only, one session at a time (there is one pair of copies). A
// session spans frames: suspend() before the real game ticks or draws,
// resume() at the start of the next slice (src/ai/job.cpp does both).
// Implemented in src/absense/trajectory/trajectory.cpp, where the
// simulation's private types are.
//
// Frames: tick t of a session that began at real frame F (startFrame) is the
// input GucciBot records at frame F + 1 + t (its one-frame-ahead rule).

#include <cstdint>
#include <memory>
#include <string>

class GJBaseGameLayer;
class GameObject;
class PlayerObject;

namespace gucci::ai::sim {

    // Trajectory::FakeMode numbering.
    enum Mode : int { Cube = 0, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

    struct Player {
        float x = 0.0f, y = 0.0f;  // node position, GD units
        float yVel = 0.0f;         // m_yVelocity (GD units per 1/60 s)
        float xVel = 0.0f;         // m_platformerXVelocity
        float rotation = 0.0f;
        int mode = Cube;
        bool upsideDown = false;
        bool mini = false;
        bool onGround = false;
        bool onSlope = false;
        bool dashing = false;
        bool goingLeft = false;
        bool held = false;         // the jump button after this tick's input
        bool button = false;       // the copy's own m_holdingButtons[jump] (what GD applied)
        bool touchingOrb = false;  // a press on the next tick fires an orb
        float speed = 0.0f;        // m_playerSpeed (0.7 0.9 1.1 1.3 1.6)
        float vehicleSize = 1.0f;
        float gravityMod = 1.0f;
    };

    struct State {
        uint32_t tick = 0;     // ticks stepped since begin (restore puts it back)
        int startFrame = 0;    // the real frame the session began at
        int count = 1;         // copies simulated: 2 in dual mode
        Player p[2];
        bool dead = false;     // the last step ended in a death (either copy)
        bool deadP2 = false;   // ... and it was player 2's
        bool complete = false; // player 1 reached the level's end (or an end trigger fired)
        int fired = 0;         // the orb, pad, portal or trigger player 1 fired on the last tick (m_uniqueID; 0 none)
        bool ring = false;     // an orb fired on the last tick
        float levelLength = 0.0f;
        // What killed it, when it died: the object's id and m_uniqueID (0 for
        // the floor, the ceiling or a block side), where it stood, and whether
        // it fell out of the top of the level.
        int killerId = 0;
        int killerUid = 0;
        float killerX = 0.0f, killerY = 0.0f;
        bool voidDeath = false;
    };

    // A captured start (opaque): the real game's state at the tick it was
    // taken. Exact start flags follow the Pathfinder page's switch.
    struct Start;
    std::shared_ptr<Start> capture(GJBaseGameLayer* pl, std::string& why);
    int startFrame(Start const& s);
    float startX(Start const& s);

    // A saved state of the open session (opaque, reusable: save into the same
    // one again and again without allocating).
    struct Snapshot;
    struct SnapshotDelete {
        void operator()(Snapshot* s) const;
    };
    using SnapshotPtr = std::unique_ptr<Snapshot, SnapshotDelete>;
    SnapshotPtr newSnapshot();
    uint32_t snapshotTick(Snapshot const& s);

    // Begins the session from `start` (null: from the live player, as the game
    // stands). False with `why` when it can't: another run owns the copies, no
    // level, the player dead. The session is open and running on return.
    bool begin(GJBaseGameLayer* pl, std::shared_ptr<Start> start, std::string& why);
    bool open();
    bool suspended();
    // One tick: `presses` pushes inside the tick (each a release first when the
    // button is down), then the button as `held`. {1, false} is a tap. True
    // when the session is over (a death; see State::dead). Reaching the end
    // is State::complete, which does not stop stepping by itself.
    bool step(uint8_t presses, bool held);
    // The same with player 2's own button (presses2, held2 as above). Only a
    // two-player level's dual part reads it; in a one-player level the game
    // hands player 1's button to both players, and step2 is step.
    bool step2(uint8_t presses, bool held, uint8_t presses2, bool held2);
    void read(State& out);
    void save(Snapshot& out);
    void restore(Snapshot& in);  // the death and the killer go with it
    // Between frames: everything the session wrote is put back for the real
    // game, and taken up again on resume.
    void suspend();
    void resume();
    void end();

    // Stage B (what the pilot sees). Valid while the session is open and not
    // suspended. The copy of player i (0, or 1 in dual), or null.
    PlayerObject* copy(int i);
    // Where an object the level's triggers can move or toggle stands in the
    // session right now (the World's log, the slot of world::WorldDef), and
    // whether it is toggled off. False when the session doesn't run the
    // World's objects (the object's own fields are then as good as it gets).
    bool objectPose(int slot, double& x, double& y, bool& disabled);
    // Player 1's copy has used this orb, pad or portal in this session.
    bool copyActivated(GameObject* object);

} // namespace gucci::ai::sim

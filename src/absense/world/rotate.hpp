#pragma once

// Rotate triggers (1346) in the World, behind the test switch pf_rotate
// (GucciBot 2026-11-15, rotate lane; ROTATE_PLAN.md section 5, the minimum
// port). The fact table is at the top of world/rotate.cpp. Off (the default),
// nothing here is ever reached and the World runs exactly as before: rotate
// commands leave their groups uncertain.
//
// What the port covers: the default mode (no aim, follow or dynamic mode), a
// centre or none, times360, any easing, lock object rotation, commands merged
// on one (target, centre), and a move and a rotate on one group in one tick.
// Aim, follow and dynamic rotates keep marking their groups uncertain.
//
// The rotated position comes out of live cocos nodes in the game (the layer's
// m_areaTransformNode, whose nodeToWorldTransform holds the angle and the
// centre). A run must never write those for every tick, so it works the
// transform out on private nodes set up like the live chain, with libcocos2d's
// own functions. That is only trusted while it gives the live chain's answer
// bit for bit: the level check compares the two for 50 angles and centres, and
// every run compares one at its start, and turns rotate off for itself when
// they differ.

#include <Geode/Geode.hpp>

namespace world {

// The test switch (gucci_test_set pf_rotate, this launch only, off by
// default). A run decides once, when it begins, whether it models rotates
// (WorldState::rotate), and keeps that for its whole life.
extern bool g_rotate;
// Set only around the World::captureLive of a run that models rotates
// (Sim::begin): its running rotate commands are then not marked uncertain.
extern bool g_captureRotates;

// The offline checks of the rotate step (world/selftest.cpp): null when they
// pass, else what failed. Run once, at the first level check.
const char* rotateSelfTest();

namespace rotate {

// The level check (World::checkDef): the private chain set up from the live
// one and compared with it for 50 angles and centres. Logs "rotate T: n/50
// bit-identical" and keeps the answer for the level.
void checkLevel(GJBaseGameLayer* pl);
bool levelOk();

// Whether a run beginning now models rotates: the switch, the level check, and
// one more comparison of the two chains right now (ROTATE_PLAN.md risk 1: a
// parent of the chain can change during a level). Refreshes the private chain
// from the live one, which every transform of the run is then worked out on.
bool wantedForRun(GJBaseGameLayer* pl);

// The transform processRotationActions reads out of m_areaTransformNode for
// `angle` about `c`, worked out on the private chain the last run start set
// up (or, before any, on a lone node: the self-test's).
cocos2d::CCAffineTransform transform(float angle, cocos2d::CCPoint c);

// What the private chain was set up with (a hash of every field of it), as
// the last run start set it up; 0 for the lone node. A run keeps the key of
// its own start (WorldState::rotateChain): a paused run whose chain another
// run's start has set up differently since marks what it rotates uncertain.
uint64_t chainKey();

// The self-test (world/selftest.cpp) works on a lone private node, with no
// live chain behind it, while this is set.
void setLoneChain(bool on);

// The level is torn down: the live chain it named is let go.
void forget();

// Restores of written rotation fields that did not come out as they were
// (world/materialize.cpp): a gameplay bug if it is ever above 0.
uint64_t restoreMisses();
void noteRestoreMiss();
// What runs that model rotates did, for the log ("world: rotate so far"):
// rotate ops emitted, objects whose rotate fields were written, and restores
// of those that were checked.
void noteOp();
void noteWrite();
void noteRestoreChecked();

// GameObject::setRotation's flag (0x197f00-0x197f2c): +0x390 is set exactly
// for these four angles.
inline bool alignedAngle(float v) { return v == 90.0f || v == -90.0f || v == 270.0f || v == -270.0f; }

}  // namespace rotate
}  // namespace world

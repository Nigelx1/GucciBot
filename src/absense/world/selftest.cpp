// World::init: the level-load check that the offsets the World relies on hold
// in the game that is running. The compile-time asserts in world/offsets.hpp
// only prove the bindings and the constants agree; this proves the game's own
// code still reads and writes those places, so a different build of the game
// turns the World off instead of letting it write into the wrong fields.
//
// The same file holds the World's other load-time checks: ObjectCache against
// a small level of its own (World::init), and the first WorldDef read from a
// level together with the state imported from it (World::checkDef).
//
// Nothing here can stop a level: every check is a compare, a failure only sets
// World::disabled (every run then behaves as it did before the World) and
// logs why.

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "absense/compat/devlog.hpp"
#include "absense/world/def.hpp"
#include "absense/world/ledger.hpp"
#include "absense/world/materialize.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/state.hpp"
#include "absense/world/world.hpp"

namespace world {

namespace {

// A few instructions of the game, byte for byte, that encode the offsets. The
// sites are well inside their functions (a hook on a function's first bytes
// does not touch them) and away from every midhook the mod places
// (0x20FEDC, 0x237A7C, 0x237DCE, 0x237E42, 0x238BAA, 0x238F6E, 0x23E173...).
struct CodeSite {
    uintptr_t rva;
    const uint8_t* bytes;
    size_t size;
    const char* what;
};

// canBeActivatedByPlayer 0x2178fb: mov r9d, [rdi+0x39c] ; mov r8d, [rbp+0x39c]
const uint8_t kContactKey[] = {0x44, 0x8b, 0x8f, 0x9c, 0x03, 0x00, 0x00, 0x44, 0x8b, 0x85, 0x9c, 0x03, 0x00, 0x00};
// canBeActivatedByPlayer 0x217962: mov ecx, [rsi+0x3e0] ; mov [rax], ecx
const uint8_t kContactStamp[] = {0x8b, 0x8e, 0xe0, 0x03, 0x00, 0x00, 0x89, 0x08};
// processCommands 0x239cf0: add eax, 2 ; mov [rbx+0x3e0], eax
const uint8_t kCommandStep[] = {0x83, 0xc0, 0x02, 0x89, 0x83, 0xe0, 0x03, 0x00, 0x00};
// processCommands 0x239d23: add eax, 2 ; mov [rbx+0x3e8], eax
const uint8_t kProgressStep[] = {0x83, 0xc0, 0x02, 0x89, 0x83, 0xe8, 0x03, 0x00, 0x00};
// moveObjects 0x22ddf5: mov eax, [rbx+0x3e0] ; cmp [rdx+0x4dc], eax ; je +0x3d ; movsd xmm0, [rdx+0x3b8]
const uint8_t kMoveMarker[] = {0x8b, 0x83, 0xe0, 0x03, 0x00, 0x00, 0x39, 0x82, 0xdc, 0x04, 0x00,
                               0x00, 0x74, 0x3d, 0xf2, 0x0f, 0x10, 0x82, 0xb8, 0x03, 0x00, 0x00};
// collisionCheckObjects 0x214c01: the channel filter (+0x33c, +0x4e8 == 1, +0x700)
const uint8_t kChannelFilter[] = {0x8b, 0x8e, 0x3c, 0x03, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x1b, 0x83, 0xbf, 0xe8,
                                  0x04, 0x00, 0x00, 0x01, 0x75, 0x12, 0x8b, 0x87, 0x00, 0x07, 0x00, 0x00};
// activatedByPlayer 0x1a4ac4 / 0x1a4ad6: mov byte [rbx+0x5b4], 1 ; mov byte [rbx+0x5b5], 1
const uint8_t kFlagP1[] = {0xc6, 0x83, 0xb4, 0x05, 0x00, 0x00, 0x01};
const uint8_t kFlagP2[] = {0xc6, 0x83, 0xb5, 0x05, 0x00, 0x00, 0x01};
// teleportPlayer 0x20fe8a: mov edx, [rsi+0x5c8]
const uint8_t kTeleportGroup[] = {0x8b, 0x96, 0xc8, 0x05, 0x00, 0x00};
// teleportPlayer 0x20febb: the inlined rand() the teleportRandomOverride midhook
// replaces the result of (the midhook itself sits right after, at 0x20FEDC)
const uint8_t kTeleportRand[] = {0x48, 0x69, 0x05, 0x32, 0x30, 0x4b, 0x00, 0xfd, 0x43, 0x03, 0x00,
                                 0x48, 0x05, 0xc3, 0x9e, 0x26, 0x00, 0x48, 0x89, 0x05, 0x25, 0x30,
                                 0x4b, 0x00, 0x48, 0xc1, 0xe8, 0x10, 0x25, 0xff, 0x7f, 0x00, 0x00};
// addToSection 0x226549: movss xmm2, [rcx+0x36a0]
const uint8_t kSectionFactor[] = {0xf3, 0x0f, 0x10, 0x91, 0xa0, 0x36, 0x00, 0x00};
// addToSection 0x22673b: slot +0x270, count++, +0x278 / +0x27c
const uint8_t kSectionSlots[] = {0x89, 0x86, 0x70, 0x02, 0x00, 0x00, 0xff, 0x04, 0x99, 0x89, 0xae,
                                 0x78, 0x02, 0x00, 0x00, 0x44, 0x89, 0xb6, 0x7c, 0x02, 0x00, 0x00};
// processStateTriggers 0x205528: mov eax, [rsi+0x238] ; movzx r8d, byte [rcx+0x19] ; cmp [rbx+0x24], eax
const uint8_t kContactAgeing[] = {0x8b, 0x86, 0x38, 0x02, 0x00, 0x00, 0x44, 0x0f, 0xb6, 0x41, 0x19, 0x39, 0x43, 0x24};
// update 0x238016: movss xmm6, [rsi+0x4e8] (the parked speed)
const uint8_t kParkedSpeed[] = {0xf3, 0x0f, 0x10, 0xb6, 0xe8, 0x04, 0x00, 0x00};
// update 0x238081: movzx r8d, byte [rsi+0x3798] (fact (a))
const uint8_t kHalfStep[] = {0x44, 0x0f, 0xb6, 0x86, 0x98, 0x37, 0x00, 0x00};
// checkSpawnObjects 0x21a960 / 0x21a9e4 / 0x21aac8: +0x33c, +0x5d0, +0x5b3
const uint8_t kSpawnChannel[] = {0x48, 0x63, 0x97, 0x3c, 0x03, 0x00, 0x00};
const uint8_t kSpawnTouch[] = {0x44, 0x38, 0xb0, 0xd0, 0x05, 0x00, 0x00};
const uint8_t kSpawnActivated[] = {0x44, 0x38, 0xb0, 0xb3, 0x05, 0x00, 0x00};
// The step port (world/step.cpp): GroupCommandObject2::reset 0x25784e writes
// the uid counter back to 0x6ba170 (mov [rip + 0x46291c], eax). Both sites of
// the command functions sit past the first bytes a hook of theirs would take.
const uint8_t kCommandUid[] = {0x89, 0x05, 0x1c, 0x29, 0x46, 0x00};
// GroupCommandObject2::step 0x257950: mov edx, [rcx + 0x190] (the first action)
const uint8_t kCommandAction[] = {0x8b, 0x91, 0x90, 0x01, 0x00, 0x00};
// prepareMoveActions 0x25f732: the node's static and optimized sums +0x38/+0x40/+0x90/+0x98
const uint8_t kNodeSums[] = {0xf2, 0x0f, 0x58, 0x53, 0x38, 0xf2, 0x0f, 0x11, 0x53, 0x38, 0xf2, 0x0f, 0x58, 0x5b, 0x40,
                             0xf2, 0x0f, 0x11, 0x5b, 0x40, 0xf2, 0x0f, 0x58, 0xa3, 0x90, 0x00, 0x00, 0x00, 0xf2, 0x0f,
                             0x11, 0xa3, 0x90, 0x00, 0x00, 0x00, 0xf2, 0x0f, 0x58, 0xab, 0x98, 0x00, 0x00, 0x00};
// prepareMoveActions 0x260586: the loop tail (old deltas from +0x40 and +0x160)
const uint8_t kCommandTail[] = {0x0f, 0x10, 0x46, 0x40, 0x0f, 0x11, 0x46, 0x50, 0x0f, 0x10, 0x86,
                                0x60, 0x01, 0x00, 0x00, 0x0f, 0x11, 0x86, 0x70, 0x01, 0x00, 0x00};
// postMoveActions 0x260840: cmp byte [rbx + 0x1b1], 0
const uint8_t kCommandErase[] = {0x80, 0xbb, 0xb1, 0x01, 0x00, 0x00, 0x00};
// processMoveActions 0x22d858: m_staticGroups +0xf30
const uint8_t kStaticGroupsRead[] = {0x48, 0x8b, 0x87, 0x30, 0x0f, 0x00, 0x00};
// update 0x238222: the lock input y from the player's m_lastPosition.y +0x4d4
const uint8_t kLockInputY[] = {0xf3, 0x0f, 0x10, 0x70, 0x04, 0xf3, 0x0f, 0x5c, 0xb3, 0xd4, 0x04, 0x00, 0x00};
// updateSpawnTriggers 0x261e29: m_deltaTime += (double)dt at +0x10 of a 0x48 entry
const uint8_t kSpawnDelta[] = {0x0f, 0x57, 0xc0, 0xf3, 0x0f, 0x5a, 0xc7, 0xf2, 0x0f, 0x58, 0x44, 0x3b, 0x10};
// spawnGroup 0x21ac15: cmp [r14 + 0x6ef], r9b (m_enable22Changes, through the delegate at +0x198)
const uint8_t kSpawnUidKey[] = {0x45, 0x38, 0x8e, 0xef, 0x06, 0x00, 0x00};
// The materializer (world/materialize.cpp (s1)-(s3)). checkCollisions 0x214268:
// mov r11, [r14 + 0x35b0] ; mov rdx, [r14 + 0x35b8] (the columns it walks)
const uint8_t kCollColumns[] = {0x4d, 0x8b, 0x9e, 0xb0, 0x35, 0x00, 0x00, 0x49, 0x8b, 0x96, 0xb8, 0x35, 0x00, 0x00};
// checkCollisions 0x21436d: mov rax, [r14 + 0x3688] ; mov rax, [rax + r13*8] ; mov r8, [rax] (a column's flag words)
const uint8_t kCollFlags[] = {0x49, 0x8b, 0x86, 0x88, 0x36, 0x00, 0x00, 0x4a, 0x8b, 0x04, 0xe8, 0x4c, 0x8b, 0x00};
// checkCollisions 0x2143c7: mov [rax + 0x274], ecx (the sorted entry's slot)
const uint8_t kCollSortSlot[] = {0x89, 0x88, 0x74, 0x02, 0x00, 0x00};
// checkCollisions 0x2141f3: mulss xmm0, [r14 + 0x36a0] (the player's section)
const uint8_t kCollSection[] = {0xf3, 0x41, 0x0f, 0x59, 0x86, 0xa0, 0x36, 0x00};
// removeObjectFromSection 0x226f3e: movsxd rcx, [r10 + 0x274] ; mov rax, [r8 + rdx*8 - 8] ; mov [r8 + rcx*8], rax
const uint8_t kRemoveSwap[] = {0x49, 0x63, 0x8a, 0x74, 0x02, 0x00, 0x00, 0x49, 0x8b, 0x44, 0xd0, 0xf8, 0x49, 0x89, 0x04, 0xc8};
// addToSection 0x226d23: bts eax, ecx ; mov [r8 + rdx*4], eax (the flag of the bucket added to)
const uint8_t kAddFlag[] = {0x0f, 0xab, 0xc8, 0x41, 0x89, 0x04, 0x90};
// addToSection 0x2265f7: mov ecx, 0x18 ; call operator new 0x4d0770 (rel32 to the image offset)
const uint8_t kAddNew[] = {0xb9, 0x18, 0x00, 0x00, 0x00, 0xe8, 0x6f, 0xa1, 0x2a, 0x00};
// The Tier C kinds (world/fire.cpp (aa)-(ai)).
// The random trigger 0x4a7225: the game's own generator, which pins both the
// global at 0x6c2e90 (a rip-relative displacement) and its constants.
const uint8_t kTriggerRandom[] = {0x48, 0x69, 0x05, 0x60, 0xbc, 0x21, 0x00, 0xfd, 0x43, 0x03, 0x00, 0x48,
                                  0x05, 0xc3, 0x9e, 0x26, 0x00, 0x48, 0x89, 0x05, 0x53, 0xbc, 0x21, 0x00};
// getItemValue 0x2341e5: the attempt count +0x3084 and the level time +0x3560
const uint8_t kItemValues[] = {0x66, 0x0f, 0x6e, 0x81, 0x84, 0x30, 0x00, 0x00, 0xf3, 0x0f,
                               0xe6, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0xf2, 0x0f, 0x10,
                               0x81, 0x60, 0x35, 0x00, 0x00};
// updateTimers 0x2634e9: m_time (+0x20 of the map node) += dt * m_timeMod (+0x2c)
const uint8_t kTimerStep[] = {0xf2, 0x0f, 0x10, 0x56, 0x20, 0x0f, 0x28, 0xc7, 0xf3, 0x0f, 0x59, 0x46, 0x2c};
// activateTimerTrigger 0x234f79: the time control trigger sets m_active
const uint8_t kTimerControl[] = {0x48, 0x8b, 0x08, 0xc6, 0x41, 0x28, 0x01};
// gameEventTriggered 0x232035: the stamp against the command index +0x3e0
const uint8_t kEventStamp[] = {0x8b, 0x96, 0xe0, 0x03, 0x00, 0x00, 0x39, 0x10};
// processOptionsTrigger 0x223dab: the unlink-dual-gravity option -> +0x860
const uint8_t kOptionUnlink[] = {0x8b, 0x87, 0x44, 0x07, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x0c, 0x83,
                                 0xf8, 0x01, 0x0f, 0x94, 0xc0, 0x88, 0x83, 0x60, 0x08, 0x00, 0x00};
// updateTimeWarp 0x23617d: m_queuedTimeWarp = 0 ; m_timeWarp = the new one
const uint8_t kTimeWarpApply[] = {0xc7, 0x81, 0x34, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0xf3, 0x0f, 0x11, 0x89, 0x30, 0x03, 0x00, 0x00};

// operator new 0x4d0770: push rbx ; sub rsp, 0x20 ; mov rbx, rcx
const uint8_t kGameNewEntry[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9};
// sized operator delete 0x4d07ac: jmp free 0x4d0ce0
const uint8_t kGameDeleteEntry[] = {0xe9, 0x2f, 0x05, 0x00, 0x00};

#define SITE(rva, bytes, what) CodeSite{rva, bytes, sizeof(bytes), what}
const CodeSite kSites[] = {
    SITE(0x2178fb, kContactKey, "canBeActivatedByPlayer uid +0x39c"),
    SITE(0x217962, kContactStamp, "canBeActivatedByPlayer command index +0x3e0"),
    SITE(0x239cf0, kCommandStep, "processCommands command index += 2"),
    SITE(0x239d23, kProgressStep, "processCommands progress += 2"),
    SITE(0x22ddf5, kMoveMarker, "moveObjects marker +0x4dc"),
    SITE(0x214c01, kChannelFilter, "collisionCheckObjects channel filter"),
    SITE(0x1a4ac4, kFlagP1, "activatedByPlayer flag +0x5b4"),
    SITE(0x1a4ad6, kFlagP2, "activatedByPlayer flag +0x5b5"),
    SITE(0x20fe8a, kTeleportGroup, "teleportPlayer target group +0x5c8"),
    SITE(0x20febb, kTeleportRand, "teleportPlayer group random"),
    SITE(0x226549, kSectionFactor, "addToSection factor +0x36a0"),
    SITE(0x22673b, kSectionSlots, "addToSection slots +0x270/+0x278/+0x27c"),
    SITE(0x205528, kContactAgeing, "processStateTriggers command index +0x238"),
    SITE(0x238016, kParkedSpeed, "update parked speed +0x4e8"),
    SITE(0x238081, kHalfStep, "update half step +0x3798"),
    SITE(0x21a960, kSpawnChannel, "checkSpawnObjects channel +0x33c"),
    SITE(0x21a9e4, kSpawnTouch, "checkSpawnObjects touch trigger +0x5d0"),
    SITE(0x21aac8, kSpawnActivated, "checkSpawnObjects activated +0x5b3"),
    SITE(0x25784e, kCommandUid, "GroupCommandObject2::reset uid counter 0x6ba170"),
    SITE(0x257950, kCommandAction, "GroupCommandObject2::step action +0x190"),
    SITE(0x25f732, kNodeSums, "prepareMoveActions node sums"),
    SITE(0x260586, kCommandTail, "prepareMoveActions command tail"),
    SITE(0x260840, kCommandErase, "postMoveActions m_doUpdate +0x1b1"),
    SITE(0x22d858, kStaticGroupsRead, "processMoveActions m_staticGroups +0xf30"),
    SITE(0x238222, kLockInputY, "update lock input y +0x4d4"),
    SITE(0x261e29, kSpawnDelta, "updateSpawnTriggers delta +0x10"),
    SITE(0x21ac15, kSpawnUidKey, "spawnGroup m_enable22Changes +0x887"),
    SITE(0x214268, kCollColumns, "checkCollisions buckets +0x35b0"),
    SITE(0x21436d, kCollFlags, "checkCollisions bucket flags +0x3688"),
    SITE(0x2143c7, kCollSortSlot, "checkCollisions sorted slot +0x274"),
    SITE(0x2141f3, kCollSection, "checkCollisions section factor +0x36a0"),
    SITE(0x226f3e, kRemoveSwap, "removeObjectFromSection swap +0x274"),
    SITE(0x226d23, kAddFlag, "addToSection bucket flag"),
    SITE(0x2265f7, kAddNew, "addToSection operator new"),
    SITE(0x4a7225, kTriggerRandom, "random trigger generator 0x6c2e90"),
    SITE(0x2634e9, kTimerStep, "updateTimers time step"),
    SITE(0x2341e5, kItemValues, "getItemValue +0x3084 / +0x3560"),
    SITE(0x234f79, kTimerControl, "time control trigger m_active"),
    SITE(0x232035, kEventStamp, "gameEventTriggered stamp +0x3e0"),
    SITE(0x223dab, kOptionUnlink, "options trigger unlink dual +0x860"),
    SITE(0x23617d, kTimeWarpApply, "updateTimeWarp +0x330 / +0x334"),
    SITE((uintptr_t)off::kGameNew, kGameNewEntry, "operator new 0x4d0770"),
    SITE((uintptr_t)off::kGameDelete, kGameDeleteEntry, "operator delete 0x4d07ac"),
};
#undef SITE

// The code does not change while the game runs: checked once per process.
// Null when every site matches, the first one that does not otherwise.
const char* checkCode() {
    static bool done = false;
    static const char* failed = nullptr;
    if (done) return failed;
    done = true;
    const uintptr_t base = geode::base::get();
    for (const CodeSite& s : kSites) {
        if (std::memcmp(reinterpret_cast<const void*>(base + s.rva), s.bytes, s.size) != 0) {
            failed = s.what;
            break;
        }
    }
    return failed;
}

void disable(const char* why) {
    World::disabled = true;
    World::disabledReason = why;
    geode::log::warn("World: off, every run uses the fallback paths ({})", why);
    devlog::logf(devlog::Cat::System, "world: off, every run uses the fallback paths (%s)", why);
}

// ------------------------------------------------------------ ObjectCache

// A small fixed generator, so the check runs the same ops every time.
struct TestRandom {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 16);
    }
};

// The ops one tick of the check's level emits: moves of odd float sizes
// (zero and minus zero among them), optimized-group moves, toggles both ways,
// and now and then a rotate the cache does not port.
void testOps(uint64_t seed, uint32_t tick, std::vector<Op>& out) {
    TestRandom r{seed * 0x9E3779B97F4A7C15ull + tick * 0xD1B54A32D192ED03ull + 1};
    for (int i = 0; i < 4; i++) r.next();
    out.clear();
    const uint32_t count = r.next() % 4;
    for (uint32_t i = 0; i < count; i++) {
        Op op;
        op.tick = tick;
        op.commandIndex = 1000 + 2 * tick;
        op.group = 1 + (int)(r.next() % 5);
        const uint32_t pick = r.next() % 100;
        if (pick < 60) {
            op.kind = OpKind::Translate;
        } else if (pick < 75) {
            op.kind = OpKind::TranslateOptimized;
        } else if (pick < 97) {
            op.kind = OpKind::Toggle;
            op.sign = (r.next() & 1) ? 1 : -1;
        } else {
            op.kind = OpKind::RotateAbout;
            op.angle = 1.5;
        }
        const uint32_t size = r.next() % 10;
        op.dx = size == 0 ? 0.0f : (size == 1 ? -0.0f : ((float)(r.next() % 2001) - 1000.0f) / 7.0f);
        op.dy = size == 2 ? 0.0f : ((float)(r.next() % 2001) - 1000.0f) / 13.0f;
        out.push_back(op);
    }
}

// The check's level: eight objects in five groups, one listed twice in an
// array, some in optimized arrays, one that never moves on x, three whose
// last position the moves never take.
std::shared_ptr<WorldDef> testDef() {
    auto def = std::make_shared<WorldDef>();
    def->slots.assign(8, nullptr);
    def->flags.assign(8, kCollidable);
    def->defOfSlot.assign(8, -1);
    def->objectCount = 8;
    std::vector<std::vector<int>> groups(6), statics(6), optimized(6);
    groups[1] = {0, 1, 2};
    statics[1] = {0, 1, 2};
    groups[2] = {1, 3, 3};
    statics[2] = {1, 3, 3};
    groups[3] = {4, 5};
    optimized[3] = {4, 5};
    groups[4] = {0, 6};
    statics[4] = {0};
    optimized[4] = {6};
    groups[5] = {7, 2};
    statics[5] = {7, 2};
    def->setGroups(groups, statics, optimized, 8);
    return def;
}

Pose testBase(int slot) {
    Pose p;
    p.x = 100.1 + slot * 37.37;
    p.y = -5.5 + slot * 1.25;
    p.lastX = (float)p.x - 1.0f;
    p.lastY = (float)p.y;
    p.marker = slot == 0 ? 1000u : 7u;  // slot 0: already moved on the first index
    p.counter = slot == 5 ? -1 : 0;
    p.disabled = p.counter < 0;
    p.skipLast = slot == 4 || slot == 5 || slot == 6;
    p.noMoveX = slot == 7;
    return p;
}

// The arithmetic again, written out from moveObjects 0x22dd50 and toggleGroup
// 0x223bc0 over the arrays themselves - not through ObjectCache.
Pose testNaive(const WorldDef& def, int slot, uint64_t seed, uint32_t ticks, uint32_t uptoTick) {
    Pose p = testBase(slot);
    std::vector<Op> ops;
    for (uint32_t t = 0; t < ticks && t <= uptoTick; t++) {
        testOps(seed, t, ops);
        for (const Op& op : ops) {
            if (op.kind == OpKind::RotateAbout) {
                bool in = false;
                for (auto list : {def.members(op.group), def.staticMembers(op.group), def.optimizedMembers(op.group)}) {
                    in = in || std::find(list.begin(), list.end(), slot) != list.end();
                }
                if (in) p.uncertain = true;
                continue;
            }
            std::span<const int> array = op.kind == OpKind::Translate            ? def.staticMembers(op.group)
                                         : op.kind == OpKind::TranslateOptimized ? def.optimizedMembers(op.group)
                                                                                 : def.members(op.group);
            for (int member : array) {
                if (member != slot) continue;
                if (op.kind == OpKind::Toggle) {
                    if (op.sign > 0) p.counter++;
                    else p.counter--;
                    p.disabled = ((uint32_t)p.counter >> 31) != 0;
                    continue;
                }
                if (!p.skipLast && p.marker != op.commandIndex) {
                    p.lastX = (float)p.x;
                    p.lastY = (float)p.y;
                    p.marker = op.commandIndex;
                }
                const double dx = op.dx;
                const double dy = op.dy;
                if (dx != 0.0 && !p.noMoveX) p.x = dx + p.x;
                if (dy != 0.0) p.y = dy + p.y;
            }
        }
    }
    return p;
}

// Null when ObjectCache gives the arithmetic's answer every way it is asked:
// straight, after branches are taken and abandoned (their chunks sealed and
// dropped), from boundaries, on another log of the same lineage, and for a
// tick in the past.
const char* checkObjectCache() {
    const std::shared_ptr<WorldDef> def = testDef();
    constexpr uint32_t kTicks = 200;
    constexpr uint64_t kMain = 11;
    constexpr uint64_t kBranch = 29;
    constexpr int kSlots = 8;
    std::vector<Op> ops;

    // Straight.
    OpLog straight;
    ObjectCache a;
    a.reset(def);
    for (int s = 0; s < kSlots; s++) a.setBase(s, testBase(s));
    for (uint32_t t = 0; t < kTicks; t++) {
        straight.beginTick(t);
        testOps(kMain, t, ops);
        for (const Op& op : ops) straight.append(op);
        if (t % 3 == 0) a.pose(straight, (int)(t % kSlots));
    }

    // Branching: a snapshot, a few ticks of other ops (every pose asked for on
    // each of them, so the cache holds the abandoned branch), then back to the
    // snapshot and on with the main ops. Short branches every 7 ticks leave
    // the cache pointing into an open chunk the main ops then grow past before
    // anyone asks again; long ones every 11 ticks seal chunks that are dropped.
    OpLog branched;
    ObjectCache b;
    b.reset(def);
    for (int s = 0; s < kSlots; s++) b.setBase(s, testBase(s));
    auto branch = [&](uint32_t t, uint32_t length) {
        const OpLog snapshot = branched;
        for (uint32_t k = 1; k <= length; k++) {
            branched.beginTick(t + k);
            testOps(kBranch + t, t + k, ops);
            for (const Op& op : ops) branched.append(op);
            for (int s = 0; s < kSlots; s++) b.pose(branched, s);
        }
        branched = snapshot;
    };
    for (uint32_t t = 0; t < kTicks; t++) {
        branched.beginTick(t);
        testOps(kMain, t, ops);
        for (const Op& op : ops) branched.append(op);
        // Some slots are asked about often, some seldom: a seldom one keeps
        // an entry from an abandoned branch while the main ops seal chunks
        // over the index it names.
        for (int s = 0; s < kSlots; s++) {
            if (t % (s < 5 ? 5u : 37u) == 0) b.pose(branched, s);
        }
        if (t % 7 == 3) branch(t, 1 + t % 3);
        if (t % 11 == 5) branch(t, 1 + t % 23);
    }
    if (!branched.sameOps(straight)) return "ObjectCache check: a restored log holds other ops";

    for (int s = 0; s < kSlots; s++) {
        const Pose want = testNaive(*def, s, kMain, kTicks, ~0u);
        if (!bitEqual(a.pose(straight, s), want)) return "ObjectCache check: a straight run's pose is off";
        if (!bitEqual(a.replayFromBase(straight, s), want)) return "ObjectCache check: the pose from the base is off";
        if (!bitEqual(b.pose(branched, s), want)) return "ObjectCache check: a pose after branches is off";
        if (!bitEqual(b.pose(straight, s), want)) return "ObjectCache check: a pose asked of another log is off";
        for (uint32_t tick : {0u, 15u, 16u, 47u, 100u, 131u}) {
            if (!bitEqual(b.poseAt(branched, s, tick), testNaive(*def, s, kMain, kTicks, tick))) {
                return "ObjectCache check: a pose at a past tick is off";
            }
        }
    }
    return nullptr;
}

// ------------------------------------------------------------ the command step (Tier B)

// The transcribed GroupCommandObject2::step (world/step.cpp (i)) against the
// game's own on a scratch command, for every easing type, 5 rates, 6
// durations and 3 step lengths: 10,000 steps an easing type, shared out over
// its 90 commands, each run to its end or its share. A step of both takes about
// a microsecond and this runs on the first level load, where running every
// command to its end (1.7 million steps) would hold the level up for seconds;
// a wider sweep, every command to its end, was run once offline against a copy
// of the game's code and came out the same (world/step.cpp (i)). Null when every
// field of every step comes out the same bit for bit; otherwise a description
// of the first difference, and the World steps commands through the game's
// function from then on.
const char* checkCommandStep(long long& stepsRun) {
    static const float kRates[] = {0.35f, 1.0f, 2.0f, 2.37f, 5.5f};
    static const float kDurations[] = {0.0f, 0.1f, 0.5f, 1.0f, 2.37f, 10.0f};
    static const float kDts[] = {1.0f / 240.0f, 1.0f / 60.0f, 1.0f / 1000.0f};
    constexpr int kEasings = 19;  // EasingType 0 .. 18
    constexpr int kMaxSteps = 10000 / (5 * 6 * 3);
    static char why[160];
    stepsRun = 0;
    for (int easing = 0; easing < kEasings; easing++) {
        for (int r = 0; r < 5; r++) {
            for (int du = 0; du < 6; du++) {
                for (int t = 0; t < 3; t++) {
                    WCmd a = freshCommand(7);
                    a.easingType = easing;
                    a.easingRate = (double)kRates[r];
                    a.duration = (double)kDurations[du];
                    // Every action type and lock a step reads, spread over the rates.
                    switch (r) {
                        case 0:
                            a.actionType1 = 1;
                            a.actionValue1 = 123.456;
                            a.actionType2 = 2;
                            a.actionValue2 = -77.7;
                            break;
                        case 1:
                            a.actionType1 = 2;
                            a.actionValue1 = 30.0;
                            break;
                        case 2:
                            a.actionType1 = 3;
                            a.actionValue1 = 360.0;
                            a.actionType2 = 4;
                            a.actionValue2 = -45.5;
                            break;
                        case 3:
                            a.actionType1 = 1;
                            a.actionValue1 = -0.001;
                            a.actionType2 = 2;
                            a.actionValue2 = 9999.9;
                            a.lockedInY = true;
                            break;
                        default:
                            break;  // no action: finishes on m_deltaTime alone
                    }
                    WCmd b = a;
                    int after = -1;
                    for (int s = 0; s < kMaxSteps; s++) {
                        stepCommandTranscribed(a, kDts[t]);
                        stepCommandThroughGame(b, kDts[t]);
                        stepsRun++;
                        if (!(a == b)) {
                            std::snprintf(why, sizeof(why),
                                          "easing %d rate %.2f duration %.2f dt %.5f step %d: x %.17g / %.17g, t %.9g / %.9g",
                                          easing, kRates[r], kDurations[du], kDts[t], s, a.currentX, b.currentX,
                                          a.deltaTimeFloat, b.deltaTimeFloat);
                            return why;
                        }
                        // The move loop's tail clears the step's deltas.
                        a.deltaX = b.deltaX = 0.0;
                        a.deltaY = b.deltaY = 0.0;
                        a.rotateDelta = b.rotateDelta = 0.0;
                        if (a.finished && after < 0) after = 2;
                        if (after >= 0 && after-- == 0) break;
                    }
                }
            }
        }
    }
    // A paused command and one already finished.
    WCmd a = freshCommand(8);
    a.disabled = true;
    a.actionType1 = 1;
    a.actionValue1 = 5.0;
    a.duration = 1.0;
    WCmd b = a;
    stepCommandTranscribed(a, 0.25f);
    stepCommandThroughGame(b, 0.25f);
    if (!(a == b)) return "a paused command steps differently";
    return nullptr;
}

// ------------------------------------------------------------ the World step

// The state the World step check starts from: moves of every shape on the
// check level's groups (one paused, one finished, one with a -1 duration),
// and queued spawns, one of which a stop in the branches erases.
WorldState stepTestState() {
    WorldState ws;
    ws.commandIndex = 5000;
    ws.nextCommandUid = 900;
    std::vector<WCmd>& cmds = ws.cmds.edit();
    for (int i = 0; i < 9; i++) {
        WCmd c = freshCommand(100 + i);
        c.commandType = 0;
        c.targetGroup = 1 + i % 5;
        c.easingType = (i * 7) % 19;
        c.easingRate = 1.0 + 0.5 * i;
        c.duration = i == 3 ? -1.0 : 0.05 * (double)(i * i + 1);
        c.actionType1 = 1;
        c.actionValue1 = 13.7 * (i + 1);
        if (i % 2 == 0) {
            c.actionType2 = 2;
            c.actionValue2 = -3.3 * i;
        }
        c.disabled = i == 5;
        if (i == 7) {
            c.finished = true;
            c.finishRelated = true;
        }
        c.triggerUid = 700 + i;
        cmds.push_back(c);
    }
    std::vector<WSpawn>& spawns = ws.spawns.edit();
    for (int i = 0; i < 4; i++) {
        WSpawn s;
        s.duration = 0.02 * (i + 1);
        s.targetGroup = 1 + i;
        s.triggerUid = 800 + i;
        spawns.push_back(s);
    }
    // Timers of every shape the step has to carry (world/fire.cpp (ad)): one
    // running up to a target it stops at, one running down, one paused and one
    // disabled, with triggers waiting on three of them - one that fires again
    // and again and two that are erased the first time they do.
    std::vector<WTimer>& timers = ws.timers.edit();
    for (int i = 0; i < 4; i++) {
        WTimer t;
        t.key = 10 + i;
        t.itemId = 10 + i;
        t.time = i == 1 ? 2.0 : 0.0;
        t.active = i != 2;
        t.timeMod = i == 1 ? -1.5f : 1.0f + 0.25f * (float)i;
        t.targetTime = i == 1 ? 0.0 : 0.25 * (double)(i + 1);
        t.stopTimeEnabled = i != 3;
        t.targetGroup = 1 + i % 5;
        t.triggerUid = 850 + i;
        t.disabled = i == 3;
        timers.push_back(t);
    }
    std::vector<WTimerListener>& listeners = ws.timerListeners.edit();
    for (int i = 0; i < 3; i++) {
        WTimerListener l;
        l.item = 10 + i;
        l.time = 0.0f;
        l.targetTime = i == 1 ? 1.0f : 0.1f * (float)(i + 1);
        l.targetGroup = 2 + i % 4;
        l.triggerUid = 880 + i;
        l.multiActivate = i == 0;
        listeners.push_back(l);
    }
    // A time warp queued by a trigger, applied at the end of the first tick.
    ws.queuedTimeWarp = 1.5f;
    return ws;
}

// A tick of the check: the order Sim::step runs the World in, with no layer
// and no copies.
void stepTestTick(Run& run, float dt) {
    stepTickBegin(run, dt, nullptr, 0);
    run.ws->commandIndex += 2;
    stepTick(run, dt);
    endTick(run);
}

// Null when a WorldState stepped straight and one stepped through snapshots
// and abandoned branches (each branch pausing, stopping or adding commands and
// toggling groups before it is thrown away) end the same: the same state, the
// same ops, and the same poses out of the object cache.
const char* checkWorldStep() {
    const std::shared_ptr<WorldDef> def = testDef();
    constexpr int kTicks = 300;
    constexpr float kDt = 1.0f / 240.0f;
    constexpr int kSlots = 8;

    WorldState straightWs = stepTestState();
    OpLog straightLog;
    ObjectCache straightCache;
    straightCache.reset(def);
    for (int s = 0; s < kSlots; s++) straightCache.setBase(s, testBase(s));
    Run straight;
    straight.def = def.get();
    straight.ws = &straightWs;
    straight.log = &straightLog;
    straight.cache = &straightCache;
    for (int t = 0; t < kTicks; t++) stepTestTick(straight, kDt);

    WorldState ws = stepTestState();
    OpLog log;
    ObjectCache cache;
    cache.reset(def);
    for (int s = 0; s < kSlots; s++) cache.setBase(s, testBase(s));
    Run run;
    run.def = def.get();
    run.ws = &ws;
    run.log = &log;
    run.cache = &cache;
    for (int t = 0; t < kTicks; t++) {
        if (t % 7 == 3) {
            const WorldState savedWs = ws;
            const OpLog savedLog = log;
            const int length = 1 + t % 13;
            for (int k = 0; k < length; k++) {
                if (k == 0) {
                    // What a branch's triggers could do: pause a command,
                    // start one, stop a spawn, toggle a group, count an item.
                    if (!ws.cmds->empty()) ws.cmds.edit()[0].disabled = !(*ws.cmds)[0].disabled;
                    WCmd c = freshCommand(ws.nextCommandUid++);
                    c.targetGroup = 1 + t % 5;
                    c.duration = 0.3;
                    c.actionType1 = 2;
                    c.actionValue1 = 50.0;
                    ws.cmds.edit().push_back(c);
                    if (!ws.spawns->empty()) {
                        std::vector<WSpawn>& spawns = ws.spawns.edit();
                        spawns.erase(spawns.begin());
                    }
                    toggleGroup(run, 1 + t % 5, t % 2 == 0);
                    updateCountForItem(run, 3, t);
                    // What a Tier C trigger's branch could do: queue a warp,
                    // start a timer, pause one and add a listener to it.
                    ws.queuedTimeWarp = 0.5f + 0.1f * (float)(t % 5);
                    world::updateTimers(run, kDt);
                }
                stepTestTick(run, kDt);
                for (int s = 0; s < kSlots; s++) cache.pose(log, s);
            }
            ws = savedWs;
            log = savedLog;
        }
        stepTestTick(run, kDt);
        if (t % 5 == 0) cache.pose(log, t % kSlots);
    }
    if (!(ws == straightWs)) return "World step check: a state stepped through branches comes out different";
    if (!log.sameOps(straightLog)) return "World step check: a log stepped through branches holds other ops";
    if (log.empty()) return "World step check: the moves emitted no ops";
    for (int s = 0; s < kSlots; s++) {
        if (!bitEqual(cache.pose(log, s), straightCache.pose(straightLog, s))) {
            return "World step check: a pose after branches is off";
        }
    }
    return nullptr;
}

}  // namespace

void World::turnOff(const char* why) {
    if (World::disabled) return;  // the first reason is the one worth keeping
    disable(why ? why : "a check outside world/selftest.cpp failed");
}

void World::init(GJBaseGameLayer* pl) {
    // The code is checked whether or not a layer is at hand: returning early
    // without a layer left the World on unchecked.
    if (const char* bad = checkCode()) {
        disable(bad);
        return;
    }
    // The design's live reads: a known object's uid and the layer's command
    // index, raw and through the bindings. They can only differ if the layout
    // the mod was built against is not the one in memory.
    if (pl) {
        if (PlayerObject* p = pl->m_player1) {
            if (off::at<int>(p, off::kUid) != p->m_uniqueID) {
                disable("player uid +0x39c does not match m_uniqueID");
                return;
            }
        }
        if (pl->m_objects && pl->m_objects->count() > 0) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(0));
            if (o && (off::at<int>(o, off::kUid) != o->m_uniqueID || off::at<int>(o, off::kObjectId) != o->m_objectID)) {
                disable("object uid +0x39c / id +0x40c do not match the bindings");
                return;
            }
        }
        if (off::layerCommandIndex(pl) != pl->m_gameState.m_commandIndex) {
            disable("layer +0x3e0 does not match m_gameState.m_commandIndex");
            return;
        }
    }
    // The cache check runs the same ops on the same test level every time,
    // so its answer is the process's: worked out on the first level only.
    static bool cacheChecked = false;
    static const char* cacheBad = nullptr;
    if (!cacheChecked) {
        cacheChecked = true;
        cacheBad = checkObjectCache();
    }
    if (cacheBad) {
        disable(cacheBad);
        return;
    }
    // The command step against the game's own (Tier B), once per process: a
    // difference does not turn the World off, it makes the World step
    // commands through the game's function on a scratch command instead.
    static bool stepChecked = false;
    if (!stepChecked) {
        stepChecked = true;
        const auto started = std::chrono::steady_clock::now();
        long long steps = 0;
        const char* diff = checkCommandStep(steps);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        g_commandStepThroughGame = diff != nullptr;
        if (diff) {
            geode::log::warn("World: the transcribed command step differs from the game's ({}); commands step through "
                             "GroupCommandObject2::step",
                             diff);
            devlog::logf(devlog::Cat::System,
                         "world: the transcribed command step differs from the game's (%s); commands step through "
                         "the game's own function",
                         diff);
        } else {
            devlog::logf(devlog::Cat::System,
                         "world: the transcribed command step matches GroupCommandObject2::step bit for bit (%lld "
                         "steps, %.0f ms)",
                         steps, ms);
        }
    }
    // SectionShadow on a grid of its own, once per process: the materializer
    // writes the real level's collision buckets through it, so a restore that
    // does not give back every header, buffer and slot turns the World off.
    static bool shadowChecked = false;
    static const char* shadowBad = nullptr;
    if (!shadowChecked) {
        shadowChecked = true;
        shadowBad = checkSectionShadow();
    }
    if (shadowBad) {
        disable(shadowBad);
        return;
    }
    // The ledger's ring and its undo on a sequence of its own, once per
    // process: a start kept at a tick the game has left is given its objects
    // back through it (design step 10).
    static bool ledgerChecked = false;
    static const char* ledgerBad = nullptr;
    if (!ledgerChecked) {
        ledgerChecked = true;
        ledgerBad = checkLedger();
    }
    if (ledgerBad) {
        disable(ledgerBad);
        return;
    }
    // The World step through branches against the same step straight, once
    // per process (it runs the same fixed state every time).
    static bool worldStepChecked = false;
    static const char* worldStepBad = nullptr;
    if (!worldStepChecked) {
        worldStepChecked = true;
        worldStepBad = checkWorldStep();
    }
    if (worldStepBad) {
        disable(worldStepBad);
        return;
    }
    // What the kinds the World does not run yet can reach (world/tierd.cpp),
    // checked at the same places: a failure only makes an area effect mark
    // every object of a run uncertain, so it never turns the World off.
    checkTierDE();
    World::disabled = false;
    World::disabledReason = "";
    devlog::logf(devlog::Cat::System,
                 "world: offsets checked (%zu code sites and the live layer), the object cache agrees with the "
                 "game's arithmetic, the bucket shadow restores exactly, the ledger undoes its ring exactly and "
                 "the step comes out the same through branches, the World is on",
                 sizeof(kSites) / sizeof(kSites[0]));
}

void World::checkDef(const WorldDef& def, GJBaseGameLayer* pl) {
    if (World::disabled || !pl) return;
    // Every object leads back to its own slot.
    for (std::size_t s = 0; s < def.slots.size(); s++) {
        if (def.slots[s] && def.slotOf(def.slots[s]) != (int)s) {
            devlog::logf(devlog::Cat::System, "world: level check: slot %zu (uid %d) does not lead back to itself", s,
                         def.slots[s]->m_uniqueID);
            disable("the level check found an object whose uid does not lead back to it");
            return;
        }
    }
    // The group arrays read back the same.
    std::size_t entries = 0;
    std::vector<int> now;
    auto sameArray = [&](const gd::vector<cocos2d::CCArray*>& arrays, int g, std::span<const int> read) {
        now.clear();
        const cocos2d::CCArray* arr = (std::size_t)g < arrays.size() ? arrays[(std::size_t)g] : nullptr;
        if (arr && arr->data) {
            for (unsigned j = 0; j < arr->data->num; j++) {
                const int s = def.slotOf(static_cast<GameObject*>(arr->data->arr[j]));
                if (s >= 0) now.push_back(s);
            }
        }
        std::sort(now.begin(), now.end());
        entries += now.size();
        return std::equal(now.begin(), now.end(), read.begin(), read.end());
    };
    for (int g = 0; g < kGroupLimit; g++) {
        if (!sameArray(pl->m_groups, g, def.members(g)) || !sameArray(pl->m_staticGroups, g, def.staticMembers(g)) ||
            !sameArray(pl->m_optimizedGroups, g, def.optimizedMembers(g))) {
            devlog::logf(devlog::Cat::System, "world: level check: group %d reads back differently", g);
            disable("the level check read a group array back differently");
            return;
        }
    }
    // Every trigger is the kind its id says.
    for (const TriggerDef& d : def.triggers) {
        const GameObject* o = d.slot >= 0 ? def.slots[(std::size_t)d.slot] : nullptr;
        if (!o || kindOf(o->m_objectID) != d.kind || !(def.flags[(std::size_t)d.slot] & kTrigger)) {
            disable("the level check found a trigger read as another kind");
            return;
        }
    }
    // Importing the live state is a pure read: twice gives the same state.
    const WorldState first = World::captureLive(pl);
    const WorldState second = World::captureLive(pl);
    if (!(first == second)) {
        disable("the level check imported the live state twice and got two states");
        return;
    }
    // Each running command copies over field for field.
    int commands = 0;
    if (GJEffectManager* em = pl->m_effectManager) {
        std::vector<int> remaps;
        for (const GroupCommandObject2& c : em->m_unkVector560) {
            const WCmd w = importCommand(c, &def, remaps);
            if (!commandMatches(c, w, remaps)) {
                disable("the level check copied a running command and it came out different");
                return;
            }
            commands++;
        }
    }
    int missing = 0;
    for (const TriggerDef& d : def.triggers) missing += d.paramsMissing ? 1 : 0;
    devlog::logf(devlog::Cat::System,
                 "world: level check passed (%zu slots, %zu group entries, %zu triggers, %d without their settings, "
                 "%d running commands, %zu uncertain groups)",
                 def.slots.size(), entries, def.triggers.size(), missing, commands, first.uncertainGroups.size());
}

}  // namespace world

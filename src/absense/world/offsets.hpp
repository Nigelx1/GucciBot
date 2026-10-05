#pragma once

// The raw offsets the World reads and writes in GD 2.2081 (Windows), each one
// checked twice: against the Geode bindings here at compile time, and against
// the game's own code bytes by World::init at level load (world/selftest.cpp).
// A field whose binding name says nothing about what the game uses it for
// (m_unk4C4 is the move marker, m_isDecoration2 the optimized-object flag) is
// read through the raw accessors below, so the code says what it means; the
// assert still pins the binding member to the same place.
//
// Every constant below was compared with the bindings: none disagrees, so
// every assert stays. Two of the design's descriptions were wrong against the
// binary and are corrected where they are declared (kSectionW/H are factors,
// not widths; +0x351 and +0x368 are the other way round).

#include <Geode/Geode.hpp>

#include <cstddef>
#include <cstdint>

namespace world::off {

// ------------------------------------------------------------ GJBaseGameLayer
inline constexpr std::ptrdiff_t kGameState = 0x1a8;
inline constexpr std::ptrdiff_t kEffectManager = 0x950;
inline constexpr std::ptrdiff_t kPlayer1 = 0xda0;
inline constexpr std::ptrdiff_t kPlayer2 = 0xda8;
inline constexpr std::ptrdiff_t kSpawnObjects = 0xdd0;
// m_gameState.m_commandIndex: processCommands 0x239c60 adds 2 per whole step.
inline constexpr std::ptrdiff_t kCommandIndex = 0x3e0;
// m_gameState.m_currentProgress: processCommands adds 2 per whole step as well.
inline constexpr std::ptrdiff_t kCurrentProgress = 0x3e8;
inline constexpr std::ptrdiff_t kCurrentChannel = 0x33c;       // m_gameState.m_currentChannel
inline constexpr std::ptrdiff_t kSpawnCursor = 0x348;          // m_gameState.m_spawnChannelRelated0
inline constexpr std::ptrdiff_t kSpawnGoingBack = 0x388;       // m_gameState.m_spawnChannelRelated1
inline constexpr std::ptrdiff_t kParkedSpeed = 0x4e8;          // m_gameState.m_timeModRelated
inline constexpr std::ptrdiff_t kActivatedObjectIDs = 0x4f0;   // m_gameState.m_activatedObjectIDs
// m_gameState.m_isDualMode: toggleDualMode 0x2168e2 leaves at once when the
// level is already in the part it is asked for, and 0x2168f4 writes it.
inline constexpr std::ptrdiff_t kIsDualMode = 0x422;
// The camera settings a portal hands the level as it is crossed, all of them
// in the game state: playerWillSwitchMode 0x212f23-0x212f8a on the way in and
// toggleDualMode 0x216d16-0x216d8d on the way out of the dual part write the
// same four from the portal's own (m_cameraIsFreeMode, m_cameraDisableGridSnap,
// m_cameraEasingValue clamped to 1..40, m_cameraPaddingValue clamped to 0..1).
// The bindings name none of them for what they hold here.
inline constexpr std::ptrdiff_t kCameraFreeMode = 0x311;
inline constexpr std::ptrdiff_t kCameraGridSnap = 0x312;
inline constexpr std::ptrdiff_t kCameraEasing = 0x2d0;
inline constexpr std::ptrdiff_t kCameraPadding = 0x2cc;
// The portal kind updateDualGround 0x2131d0 last animated the ground for. Also
// in the game state, but past the bytes a run's snapshot keeps of it, so the
// trajectory's PortalState carries it on its own.
inline constexpr std::ptrdiff_t kDualGroundMode = 0x540;
inline constexpr std::ptrdiff_t kGroups = 0xf18;               // m_groups (vector<CCArray*>, null = empty)
inline constexpr std::ptrdiff_t kStaticGroups = 0xf30;         // m_staticGroups
inline constexpr std::ptrdiff_t kOptimizedGroups = 0xf48;      // m_optimizedGroups (decoration only, see step.cpp (e))
inline constexpr std::ptrdiff_t kObjectsToDeactivate = 0x1030;
inline constexpr std::ptrdiff_t kSections = 0x3598;            // every object, slot +0x270
inline constexpr std::ptrdiff_t kCollBuckets = 0x35b0;         // m_nonEffectObjects, slot +0x274
inline constexpr std::ptrdiff_t kCBBuckets = 0x35c8;           // m_collisionBlockSections (collision blocks 1816)
inline constexpr std::ptrdiff_t kExtList = 0x35e0;             // m_calcNonEffectObjects (extended collision)
inline constexpr std::ptrdiff_t kExtCount = 0x35f8;
inline constexpr std::ptrdiff_t kCBExtList = 0x3600;           // m_calcCollisionBlockObjects
inline constexpr std::ptrdiff_t kCBExtCount = 0x3618;
inline constexpr std::ptrdiff_t kSectionCounts = 0x3640;       // m_sectionSizes
inline constexpr std::ptrdiff_t kCollCounts = 0x3658;          // m_nonEffectObjectsSizes
inline constexpr std::ptrdiff_t kCBCounts = 0x3670;            // m_collisionBlockSectionSizes
inline constexpr std::ptrdiff_t kCollDirty = 0x3688;           // m_nonEffectObjectsFlags (a bit per bucket)
// Not a width: addToSection 0x226549 multiplies m_positionX by it, and
// GJBaseGameLayer::init 0x206fc8 sets both to 0.01f - one section per 100 units.
inline constexpr std::ptrdiff_t kSectionW = 0x36a0;            // m_sectionXFactor
inline constexpr std::ptrdiff_t kSectionH = 0x36a4;            // m_sectionYFactor
inline constexpr std::ptrdiff_t kMovedCount = 0x3724;          // m_movedCount (moveObjects adds the array count)
// The game's operator new (malloc retried through the new handler) and sized
// operator delete (a jump to free): addToSection 0x2265f7 allocates the
// section vectors with the first. Image offsets, not fields.
inline constexpr std::ptrdiff_t kGameNew = 0x4d0770;
inline constexpr std::ptrdiff_t kGameDelete = 0x4d07ac;
inline constexpr std::ptrdiff_t kHalfStep = 0x3798;            // the step is one half of a split step (fact (a))
inline constexpr std::ptrdiff_t kClickBetweenSteps = 0x3799;   // m_clickBetweenSteps (game variable 0177)

// ------------------------------------------------------------ GJGameState
inline constexpr std::ptrdiff_t kStateCommandIndex = 0x238;

// ------------------------------------------------------------ GameObject
inline constexpr std::ptrdiff_t kSectionSlot = 0x270;          // m_someOtherIndex: index in the +0x3598 bucket
inline constexpr std::ptrdiff_t kSlot = 0x274;                 // m_innerSectionIndex: index in the collision bucket
inline constexpr std::ptrdiff_t kSecX = 0x278;                 // m_outerSectionIndex
inline constexpr std::ptrdiff_t kSecY = 0x27c;                 // m_middleSectionIndex
inline constexpr std::ptrdiff_t kExtFlag = 0x280;              // m_hasExtendedCollision
inline constexpr std::ptrdiff_t kGroupDisabled = 0x28e;        // m_isGroupDisabled
inline constexpr std::ptrdiff_t kNoMoveX = 0x2c8;              // m_tempOffsetXRelated: moveObjects skips the x move
// The design had these two the other way round. moveObjects 0x22de37 writes
// the word at +0x368 (m_isObjectRectDirty, m_isOrientedBoxDirty) only on the
// first move of a command index, and 0x22de79 the word at +0x351
// (m_isObjectPosDirty, m_isUnmodifiedPosDirty) on every move.
inline constexpr std::ptrdiff_t kRectDirty = 0x351;            // position dirty pair (every move)
inline constexpr std::ptrdiff_t kDirty368 = 0x368;             // rect / oriented box dirty pair (first move of an index)
inline constexpr std::ptrdiff_t kRadius = 0x38c;               // m_objectRadius
inline constexpr std::ptrdiff_t kUid = 0x39c;                  // m_uniqueID
inline constexpr std::ptrdiff_t kType = 0x3a0;                 // m_objectType
inline constexpr std::ptrdiff_t kPosX = 0x3b8;                 // m_positionX (double)
inline constexpr std::ptrdiff_t kPosY = 0x3c0;                 // m_positionY (double)
inline constexpr std::ptrdiff_t kDisabled = 0x3d2;             // m_isDisabled
inline constexpr std::ptrdiff_t kObjectId = 0x40c;             // m_objectID
inline constexpr std::ptrdiff_t kGroupIds = 0x490;             // m_groups (std::array<short, 10>*)
inline constexpr std::ptrdiff_t kGroupCount = 0x498;           // m_groupCount
inline constexpr std::ptrdiff_t kToggleCounter = 0x4c0;        // m_enabledGroupsCounter
inline constexpr std::ptrdiff_t kLastPos = 0x4d0;              // m_lastPosition (float x, y)
inline constexpr std::ptrdiff_t kMoveMarker = 0x4dc;           // m_unk4C4: the command index of the last move
inline constexpr std::ptrdiff_t kClassType = 0x4e8;            // m_classType (1: effect object)
inline constexpr std::ptrdiff_t kDecoration = 0x511;           // m_isDecoration: m_objectType == 7 at setup (0x1a0a4b)
// m_isDecoration2: set with +0x511 at setup, cleared by moveObjectToStaticGroup
// 0x231410. moveObjects skips its m_lastPosition update while it is set.
inline constexpr std::ptrdiff_t kSkipLast = 0x512;
// The caches a copy's collision pass fills in on an object it reads (the
// materializer puts them back on an object it wrote, world/materialize.cpp).
// getOrientedBox 0x1a1516: the box at +0x2e0, made on first use (OBB2D::create
// 0x1a1634, retained), recomputed while +0x369 is set; it sets +0x2e8.
inline constexpr std::ptrdiff_t kOrientedBox = 0x2e0;          // m_orientedBox
inline constexpr std::ptrdiff_t kUseOuterOb = 0x2e8;           // m_shouldUseOuterOb
// getUnmodifiedPosition 0x1978f0 caches +0x340 (cleared +0x350 / +0x351) and
// getObjectRect 0x197850 caches +0x358 (clears +0x368): +0x340 .. +0x36a is
// both caches with their flags, +0x354 m_fadeMargin between them.
inline constexpr std::ptrdiff_t kRectCaches = 0x340;           // m_textureRect
inline constexpr std::ptrdiff_t kRectCachesEnd = 0x36a;        // m_colorSpriteLocked (not included)
inline constexpr std::ptrdiff_t kObjectRect = 0x358;           // m_objectRect

// ------------------------------------------------------------ EnhancedGameObject
inline constexpr std::ptrdiff_t kNoMultiActivate = 0x5b1;      // m_isNoMultiActivate
inline constexpr std::ptrdiff_t kMultiActivate = 0x5b2;        // m_isMultiActivate
inline constexpr std::ptrdiff_t kActivated = 0x5b3;            // m_activated (activatedByPlayer 0x1a4aa0)
inline constexpr std::ptrdiff_t kActivatedP1 = 0x5b4;          // m_activatedByPlayer1 (player uid 1)
inline constexpr std::ptrdiff_t kActivatedP2 = 0x5b5;          // m_activatedByPlayer2 (any other player)

// ------------------------------------------------------------ EffectGameObject
inline constexpr std::ptrdiff_t kTargetGroup = 0x5c8;          // m_targetGroupID
inline constexpr std::ptrdiff_t kTouchTriggered = 0x5d0;       // m_isTouchTriggered
inline constexpr std::ptrdiff_t kSpawnTriggered = 0x5d1;       // m_isSpawnTriggered
inline constexpr std::ptrdiff_t kActivateGroup = 0x675;        // m_activateGroup (the ring's toggle, 0x398f30)
inline constexpr std::ptrdiff_t kMultiTriggered = 0x690;       // m_isMultiTriggered
inline constexpr std::ptrdiff_t kControlId = 0x698;            // m_controlID (0x398e71, 0x398f23)
inline constexpr std::ptrdiff_t kSinglePTouch = 0x6dc;         // m_isSinglePTouch (playerTouchedTrigger 0x217f99)
inline constexpr std::ptrdiff_t kChannel = 0x700;              // m_channelValue
inline constexpr std::ptrdiff_t kSpeedStart = 0x708;           // m_speedStart (the spawn walk's position)
inline constexpr std::ptrdiff_t kOrangePortal = 0x748;         // TeleportPortalObject::m_orangePortal

// ------------------------------------------------------------ RingObject
inline constexpr std::ptrdiff_t kSpawnOnly = 0x741;            // m_isSpawnOnly (the ring's spawn branch, 0x398e84)

// ------------------------------------------------------------ PlayerObject
inline constexpr std::ptrdiff_t kPlayerDead = 0x9c0;           // m_isDead (handleButton's gate, 0x233aa3)
inline constexpr std::ptrdiff_t kPlayerSideways = 0x9c3;       // m_isSideways
inline constexpr std::ptrdiff_t kPlayerPlatformer = 0xb70;     // m_isPlatformer

// ------------------------------------------------------------ GJEffectManager
inline constexpr std::ptrdiff_t kCommands = 0x608;             // m_unkVector560: vector<GroupCommandObject2>
inline constexpr std::ptrdiff_t kToggleBits = 0x4f8;           // m_unkVector438: vector<bool>
inline constexpr std::ptrdiff_t kToggleSet = 0x518;            // m_unkMap460: unordered_set<int>
inline constexpr std::ptrdiff_t kSpawnActions = 0x5a8;         // m_spawnTriggerActions
inline constexpr std::ptrdiff_t kGroupNodes = 0x790;           // m_unkVector6c0: vector<CCMoveCNode*>
inline constexpr std::ptrdiff_t kLocalOffsets = 0x7d8;         // m_unkVector708
inline constexpr std::ptrdiff_t kLockInputs = 0x800;           // m_unk780 .. m_unk794: six floats up to +0x814
inline constexpr std::ptrdiff_t kLockInputsEnd = 0x818;
// What World::captureLive imports (world/capture.cpp). Several binding names
// carry an older offset in their name (m_unkVector1e0 is at +0x1f0); the
// asserts below pin each name to the place the game's code reads.
// playerButton 0x2621bd walks +0x1f0 in 0x38 strides (dual mode +0x1c,
// control +0x18, uid +0x14, remaps +0x20).
inline constexpr std::ptrdiff_t kTouchListeners = 0x1f0;       // m_unkVector1e0: vector<TouchToggleAction>
// updateCountForItem 0x26262c looks the item up in the map at +0x208 and walks
// its vector in 0x40 strides (previous +4, target count +8, group +0xc).
inline constexpr std::ptrdiff_t kCountListeners = 0x208;       // m_countTriggerActions
// registerCollisionTrigger 0x25c44d pushes 0x38-byte entries onto +0x248.
inline constexpr std::ptrdiff_t kCollisionListeners = 0x248;   // m_unkVector230: vector<CollisionTriggerAction>
// countForItem 0x262452: the hash map at +0x330 (list +0x338, buckets +0x348, mask +0x360).
inline constexpr std::ptrdiff_t kItems = 0x330;                // m_itemCountMap
// updateCountForItem 0x2625bb: the persistent copy at +0x370.
inline constexpr std::ptrdiff_t kPersistentItems = 0x370;      // m_persistentItemCountMap
// timeForItem 0x263949 / updateTimers 0x263384: TimerItem nodes, m_time at node +0x20.
inline constexpr std::ptrdiff_t kTimers = 0x470;               // m_timerItemMap
// updateTimers 0x2635cc: the timer's listeners by item id, 0x38-byte entries.
inline constexpr std::ptrdiff_t kTimerListeners = 0x4b0;       // m_unkMap3f8
// storeTriggeredID 0x262003: set<pair<int, int>>.
inline constexpr std::ptrdiff_t kTriggeredIds = 0x558;         // m_unkMap498

// ------------------------------------------------------------ GroupCommandObject2
inline constexpr std::size_t kCommandSize = 0x208;
inline constexpr std::ptrdiff_t kCmdDuration = 0x18;
inline constexpr std::ptrdiff_t kCmdDeltaTime = 0x20;
inline constexpr std::ptrdiff_t kCmdTargetGroup = 0x28;
inline constexpr std::ptrdiff_t kCmdFinished = 0x70;
inline constexpr std::ptrdiff_t kCmdCommandType = 0xd0;
// GroupCommandObject2::step 0x257900 and updateAction 0x2579d0 read these
// (the rest of WCmd's fields sit where the bindings put them, which
// createMoveCommand 0x25c7fa-0x25c984 agrees with).
inline constexpr std::ptrdiff_t kCmdEasingType = 0x0c;
inline constexpr std::ptrdiff_t kCmdEasingRate = 0x10;
inline constexpr std::ptrdiff_t kCmdCurrentX = 0x30;
inline constexpr std::ptrdiff_t kCmdDeltaX = 0x40;
inline constexpr std::ptrdiff_t kCmdDisabled = 0x71;
inline constexpr std::ptrdiff_t kCmdLockedInX = 0x77;
inline constexpr std::ptrdiff_t kCmdModX = 0x80;
inline constexpr std::ptrdiff_t kCmdRotateValue = 0x90;
inline constexpr std::ptrdiff_t kCmdTriggerUid = 0x158;
inline constexpr std::ptrdiff_t kCmdDeltaX3 = 0x160;
inline constexpr std::ptrdiff_t kCmdActionType1 = 0x190;
inline constexpr std::ptrdiff_t kCmdActionValue1 = 0x198;
inline constexpr std::ptrdiff_t kCmdDeltaTimeFloat = 0x1ac;
inline constexpr std::ptrdiff_t kCmdAlreadyUpdated = 0x1b0;
// The command kinds m_commandType holds: createMoveCommand 0x25c827 writes 0,
// createRotateCommand 0x25ca6b 1, createFollowCommand 0x25cb91 2,
// createPlayerFollowCommand 0x25cc8a 3, triggerTransformCommand 0x21f726 4,
// createKeyframeCommand 0x25ce05 5.
// GroupCommandObject2::reset 0x25770c: mov eax, [rip + 0x462a5e] -> the
// game's global counter of command uids (an image offset, not a field).
inline constexpr std::ptrdiff_t kCommandUidCounter = 0x6ba170;
// The game's own random generator (an image offset as well), inlined wherever
// it is used: the random triggers 0x4a7225 / 0x4b423c and a spawn trigger's
// delay spread 0x4b9231 take seed = seed * 0x343fd + 0x269ec3 in 64 bits and
// keep the 15 bits above the low word.
inline constexpr std::ptrdiff_t kRandomState = 0x6c2e90;
inline constexpr int kCmdMove = 0;
inline constexpr int kCmdRotate = 1;
inline constexpr int kCmdFollow = 2;
inline constexpr int kCmdFollowPlayerY = 3;
inline constexpr int kCmdTransform = 4;
inline constexpr int kCmdKeyframe = 5;

// ------------------------------------------------------------ element sizes
// The strides the game's own loops take over these vectors.
inline constexpr std::size_t kSpawnActionSize = 0x48;      // updateSpawnTriggers 0x261ed2
inline constexpr std::size_t kCountActionSize = 0x40;      // updateCountForItem 0x2626a9
inline constexpr std::size_t kCollisionActionSize = 0x38;  // registerCollisionTrigger 0x25c4b9
inline constexpr std::size_t kTouchActionSize = 0x38;      // playerButton 0x2621ce
inline constexpr std::size_t kTimerActionSize = 0x38;      // updateTimers 0x26363b
inline constexpr std::size_t kTimerItemSize = 0x58;        // updateTimers: node +0x18 .. +0x70
inline constexpr std::size_t kEventInstanceSize = 0x28;    // gameEventTriggered 0x2320ce
inline constexpr std::size_t kDynamicActionSize = 0x60;    // processDynamicObjectActions 0x22e3b0
inline constexpr std::size_t kAdvancedFollowSize = 0x20;   // processAdvancedFollowActions 0x22f17b
inline constexpr std::size_t kAreaInstanceSize = 0xe8;     // processAreaEffects 0x2284af

// ------------------------------------------------------------ layer (continued)
// updateTimeWarp 0x236187 writes the clamped warp to +0x330 and clears +0x334;
// applyTimeWarp 0x2361c1 keeps the one the scheduler runs at in +0x338.
inline constexpr std::ptrdiff_t kTimeWarp = 0x330;             // m_gameState.m_timeWarp
inline constexpr std::ptrdiff_t kQueuedTimeWarp = 0x334;       // m_gameState.m_queuedTimeWarp
inline constexpr std::ptrdiff_t kAppliedTimeWarp = 0x338;      // m_gameState.m_timeWarpRelated
inline constexpr std::ptrdiff_t kParkedSpeedFlag = 0x4ec;      // m_gameState.m_timeModRelated2 (update 0x238026)
// gameEventTriggered 0x231ff0: the stamps (event key -> command index) at
// +0x598 and the listeners at +0x588, keyed by (event, a * 10000 + b).
inline constexpr std::ptrdiff_t kEventListeners = 0x588;
inline constexpr std::ptrdiff_t kEventStamps = 0x598;
inline constexpr std::ptrdiff_t kAreaMoves = 0x658;            // processAreaActions 0x229049 (kind 0)
inline constexpr std::ptrdiff_t kAreaRotates = 0x670;          // (kind 1)
inline constexpr std::ptrdiff_t kAreaScales = 0x688;           // (kind 2)
inline constexpr std::ptrdiff_t kAdvancedFollows = 0x718;      // processAdvancedFollowActions 0x22f101
inline constexpr std::ptrdiff_t kDynamicMoves = 0x730;         // processDynamicObjectActions 0x22e2b5
inline constexpr std::ptrdiff_t kDynamicRotates = 0x748;       // (0x22e2ae)
// processCreateObjectsFromSetup 0x3ad7a1 sets it to 1.0 right before
// setupHasCompleted, which runs optimizeMoveGroups (0x3a672f): the group
// arrays only hold their final static / optimized split from then on.
inline constexpr std::ptrdiff_t kLoadingProgress = 0x31f4;     // m_loadingProgress

// ------------------------------------------------------------ EffectGameObject (continued)
// triggerMoveCommand 0x21ea40 reads the move's settings from these.
inline constexpr std::ptrdiff_t kEffDuration = 0x5bc;          // m_duration
inline constexpr std::ptrdiff_t kEffCenterGroup = 0x5cc;       // m_centerGroupID
inline constexpr std::ptrdiff_t kEffMoveOffset = 0x5e0;        // m_moveOffset
inline constexpr std::ptrdiff_t kEffLockPlayerX = 0x5f0;       // m_lockToPlayerX .. m_lockToCameraY (+0x5f3)
inline constexpr std::ptrdiff_t kEffUseTarget = 0x5f4;         // m_useMoveTarget
inline constexpr std::ptrdiff_t kEffTargetMode = 0x5f8;        // m_moveTargetMode
inline constexpr std::ptrdiff_t kEffModX = 0x5fc;              // m_moveModX
inline constexpr std::ptrdiff_t kEffModY = 0x600;              // m_moveModY
// Named m_isDirectionFollowSnap360; triggerMoveCommand 0x21ec5a takes it as
// the direction mode (targets like use-target, then moves +0x60c units along).
inline constexpr std::ptrdiff_t kEffDirectionMode = 0x605;
inline constexpr std::ptrdiff_t kEffTargetModCenter = 0x608;   // m_targetModCenterID
inline constexpr std::ptrdiff_t kEffDirectionDistance = 0x60c; // m_directionModeDistance
inline constexpr std::ptrdiff_t kEffDynamic = 0x610;           // m_isDynamicMode
inline constexpr std::ptrdiff_t kEffSilent = 0x611;            // m_isSilent: moves at once, no command
inline constexpr std::ptrdiff_t kEffControlId = 0x698;         // m_controlID
inline constexpr std::ptrdiff_t kEffTargetP1 = 0x6a4;          // m_targetPlayer1
inline constexpr std::ptrdiff_t kEffTargetP2 = 0x6a5;          // m_targetPlayer2
// spawnObjectsInOrder 0x21aea1: an object 2065 counts as spawnable there when
// this byte (m_animateOnTrigger) is set.
inline constexpr std::ptrdiff_t kAnimateOnTrigger = 0x599;
// The item ids of a count, item, timer or pickup trigger (activateItemEdit
// 0x234265 / 0x2342b8 reads both).
inline constexpr std::ptrdiff_t kEffItemId = 0x6a0;            // m_itemID
inline constexpr std::ptrdiff_t kEffItemId2 = 0x694;           // m_itemID2

// ------------------------------------------------------------ the Tier C triggers
// TimerTriggerGameObject (activateTimerTrigger 0x234e60 / startTimer 0x262f50).
inline constexpr std::ptrdiff_t kTimerStart = 0x740;           // m_startTime
inline constexpr std::ptrdiff_t kTimerTarget = 0x748;          // m_targetTime
inline constexpr std::ptrdiff_t kTimerStopEnabled = 0x750;     // m_stopTimeEnabled
inline constexpr std::ptrdiff_t kTimerControlType = 0x75c;     // m_controlType
// ItemTriggerGameObject (activateItemEditTrigger 0x234250).
inline constexpr std::ptrdiff_t kItemMode1 = 0x740;            // m_item1Mode
inline constexpr std::ptrdiff_t kItemSign2 = 0x770;            // m_signType2
inline constexpr std::ptrdiff_t kItemPersistent = 0x774;       // m_persistent .. m_timer (+0x777)
// SequenceTriggerGameObject (0x4b4870): its own two maps and its settings.
inline constexpr std::ptrdiff_t kSeqState = 0x758;             // m_sequenceState.m_sequenceTimes
inline constexpr std::ptrdiff_t kSeqIndices = 0x798;           // m_sequenceState.m_sequenceIndices
inline constexpr std::ptrdiff_t kSeqMinInterval = 0x7d8;       // m_minInt
inline constexpr std::ptrdiff_t kSeqMode = 0x7dc;              // m_sequenceMode
inline constexpr std::ptrdiff_t kSeqResetMode = 0x7e0;         // m_resetMode
inline constexpr std::ptrdiff_t kSeqReset = 0x7e4;             // m_reset
inline constexpr std::ptrdiff_t kSeqTotalCount = 0x7e8;        // m_sequenceTotalCount
inline constexpr std::ptrdiff_t kSeqUniqueRemap = 0x7ec;       // m_uniqueRemap
// EventLinkTrigger (activateEventTrigger 0x232110).
inline constexpr std::ptrdiff_t kEventIds = 0x740;             // m_eventIDs
inline constexpr std::ptrdiff_t kEventExtraId = 0x754;         // m_extraID (* 10000 + m_extraID2)
// GameOptionsTrigger (processOptionsTrigger 0x223d50).
inline constexpr std::ptrdiff_t kOptStreakAdditive = 0x740;    // m_streakAdditive
inline constexpr std::ptrdiff_t kOptBoostSlide = 0x774;        // m_boostSlide
// PlayerControlGameObject (activatePlayerControlTrigger 0x2174e0).
inline constexpr std::ptrdiff_t kCtrlStopJump = 0x740;         // m_stopJump .. m_stopSlide (+0x743)

// ------------------------------------------------------------ the layer and the players
// The unlink-dual-gravity option (processOptionsTrigger 0x223dbb), which the
// gravity of a dual copy reads (flipGravity's caller 0x212b3f).
inline constexpr std::ptrdiff_t kUnlinkedDual = 0x860;
// What getItemValue 0x2341e5 / 0x2341f6 reads for item modes 5 and 4: the
// level's attempt count, and the time it has been running - another sum of
// dt / timeWarp, which update 0x237edd stops once the level has ended or the
// player has died. Neither has a binding name that says so.
inline constexpr std::ptrdiff_t kAttemptCount = 0x3084;
inline constexpr std::ptrdiff_t kLevelTime = 0x3560;
// activatePlayerControlTrigger 0x2176a0-0x2176d8: what the stop-rotation and
// stop-slide options clear on a player. The word at +0x728 is two flags.
inline constexpr std::ptrdiff_t kPlayerRotateSpeed = 0x720;       // m_rotationSpeed
inline constexpr std::ptrdiff_t kPlayerRotating = 0x728;          // m_isRotating, m_isBallRotating2 (+0x729)
inline constexpr std::ptrdiff_t kPlayerBallRotating = 0x668;      // m_isBallRotating
inline constexpr std::ptrdiff_t kPlayerAccelerating = 0x952;      // m_isAccelerating
inline constexpr std::ptrdiff_t kPlayerAffectedByForces = 0xb94;  // m_affectedByForces
inline constexpr std::ptrdiff_t kPlayerControlsDisabled = 0xa2b;  // m_controlsDisabled (pushButton 0x397f7a)
inline constexpr std::ptrdiff_t kPlayerInputsLocked = 0xbc8;      // m_inputsLocked

// ------------------------------------------------------------ raw access
// A field at a raw offset of an object of the game. The offsets are the
// constants above; the type is the one the binary reads there.
template <class T>
inline T& at(void* base, std::ptrdiff_t offset) {
    return *static_cast<T*>(static_cast<void*>(static_cast<char*>(base) + offset));
}
template <class T>
inline const T& at(const void* base, std::ptrdiff_t offset) {
    return *static_cast<const T*>(static_cast<const void*>(static_cast<const char*>(base) + offset));
}

inline uint32_t& moveMarker(GameObject* o) { return at<uint32_t>(o, kMoveMarker); }
inline bool& skipLastPosition(GameObject* o) { return at<bool>(o, kSkipLast); }
inline uint16_t& positionDirtyWord(GameObject* o) { return at<uint16_t>(o, kRectDirty); }
inline uint16_t& rectDirtyWord(GameObject* o) { return at<uint16_t>(o, kDirty368); }
inline uint32_t& layerCommandIndex(GJBaseGameLayer* pl) { return at<uint32_t>(pl, kCommandIndex); }

}  // namespace world::off

// ------------------------------------------------------------ compile-time checks
// offsetof on these classes is conditionally supported (they are not standard
// layout); clang computes it from the same layout the game code is compiled
// against, which is exactly what is being checked.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

namespace world::off::detail {
inline constexpr std::ptrdiff_t kGs = offsetof(GJBaseGameLayer, m_gameState);
}

#ifdef GEODE_IS_WINDOWS // Windows layouts only (core/platform.hpp)
static_assert(offsetof(GJBaseGameLayer, m_gameState) == world::off::kGameState);
static_assert(offsetof(GJBaseGameLayer, m_effectManager) == world::off::kEffectManager);
static_assert(offsetof(GJBaseGameLayer, m_player1) == world::off::kPlayer1);
static_assert(offsetof(GJBaseGameLayer, m_player2) == world::off::kPlayer2);
static_assert(offsetof(GJBaseGameLayer, m_spawnObjects) == world::off::kSpawnObjects);
static_assert(offsetof(GJGameState, m_commandIndex) == world::off::kStateCommandIndex);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_commandIndex) == world::off::kCommandIndex);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_currentProgress) == world::off::kCurrentProgress);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_currentChannel) == world::off::kCurrentChannel);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_spawnChannelRelated0) == world::off::kSpawnCursor);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_spawnChannelRelated1) == world::off::kSpawnGoingBack);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeModRelated) == world::off::kParkedSpeed);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_activatedObjectIDs) == world::off::kActivatedObjectIDs);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_isDualMode) == world::off::kIsDualMode);
// The four camera fields and the ground kind have no binding name that says
// what the portal code puts there, so what is pinned here is where they sit
// rather than which member they are: a run only writes them because a snapshot
// and Sim::end put them back, and both do that by the byte. The four camera
// fields have to be in the first run of bytes a run's PortalState keeps
// (trajectory.cpp PortalState::head, which reaches m_spawnChannelRelated0 and
// stops at 0x1a0), and the ground kind past it but still inside the game
// state, which end() assigns whole. A layout that moved any of them out would
// leave a run writing into the live game with nothing to undo it.
static_assert(world::off::detail::kGs + (std::ptrdiff_t)offsetof(GJGameState, m_spawnChannelRelated0) >=
              world::off::kGameState + 0x1a0);
static_assert(world::off::kCameraPadding >= world::off::kGameState &&
              world::off::kCameraPadding + 4 <= world::off::kGameState + 0x1a0);
static_assert(world::off::kCameraEasing >= world::off::kGameState &&
              world::off::kCameraEasing + 4 <= world::off::kGameState + 0x1a0);
static_assert(world::off::kCameraFreeMode >= world::off::kGameState &&
              world::off::kCameraFreeMode + 1 <= world::off::kGameState + 0x1a0);
static_assert(world::off::kCameraGridSnap >= world::off::kGameState &&
              world::off::kCameraGridSnap + 1 <= world::off::kGameState + 0x1a0);
static_assert(world::off::kDualGroundMode >= world::off::kGameState + 0x1a0 &&
              world::off::kDualGroundMode + 4 <= world::off::kGameState + (std::ptrdiff_t)sizeof(GJGameState));
static_assert(offsetof(GJBaseGameLayer, m_groups) == world::off::kGroups);
static_assert(offsetof(GJBaseGameLayer, m_staticGroups) == world::off::kStaticGroups);
static_assert(offsetof(GJBaseGameLayer, m_optimizedGroups) == world::off::kOptimizedGroups);
static_assert(offsetof(GJBaseGameLayer, m_objectsToDeactivate) == world::off::kObjectsToDeactivate);
static_assert(offsetof(GJBaseGameLayer, m_sections) == world::off::kSections);
static_assert(offsetof(GJBaseGameLayer, m_nonEffectObjects) == world::off::kCollBuckets);
static_assert(offsetof(GJBaseGameLayer, m_collisionBlockSections) == world::off::kCBBuckets);
static_assert(offsetof(GJBaseGameLayer, m_calcNonEffectObjects) == world::off::kExtList);
static_assert(offsetof(GJBaseGameLayer, m_calcNonEffectObjectsSize) == world::off::kExtCount);
static_assert(offsetof(GJBaseGameLayer, m_calcCollisionBlockObjects) == world::off::kCBExtList);
static_assert(offsetof(GJBaseGameLayer, m_calcCollisionBlockObjectsSize) == world::off::kCBExtCount);
static_assert(offsetof(GJBaseGameLayer, m_sectionSizes) == world::off::kSectionCounts);
static_assert(offsetof(GJBaseGameLayer, m_nonEffectObjectsSizes) == world::off::kCollCounts);
static_assert(offsetof(GJBaseGameLayer, m_collisionBlockSectionSizes) == world::off::kCBCounts);
static_assert(offsetof(GJBaseGameLayer, m_nonEffectObjectsFlags) == world::off::kCollDirty);
static_assert(offsetof(GJBaseGameLayer, m_sectionXFactor) == world::off::kSectionW);
static_assert(offsetof(GJBaseGameLayer, m_sectionYFactor) == world::off::kSectionH);
static_assert(offsetof(GJBaseGameLayer, m_movedCount) == world::off::kMovedCount);
static_assert(offsetof(GJBaseGameLayer, m_clickBetweenSteps) == world::off::kClickBetweenSteps);

static_assert(offsetof(GameObject, m_someOtherIndex) == world::off::kSectionSlot);
static_assert(offsetof(GameObject, m_innerSectionIndex) == world::off::kSlot);
static_assert(offsetof(GameObject, m_outerSectionIndex) == world::off::kSecX);
static_assert(offsetof(GameObject, m_middleSectionIndex) == world::off::kSecY);
static_assert(offsetof(GameObject, m_hasExtendedCollision) == world::off::kExtFlag);
static_assert(offsetof(GameObject, m_isGroupDisabled) == world::off::kGroupDisabled);
static_assert(offsetof(GameObject, m_tempOffsetXRelated) == world::off::kNoMoveX);
static_assert(offsetof(GameObject, m_isDirty) + 1 == world::off::kRectDirty);
static_assert(offsetof(GameObject, m_isObjectRectDirty) == world::off::kDirty368);
static_assert(offsetof(GameObject, m_objectRadius) == world::off::kRadius);
static_assert(offsetof(GameObject, m_uniqueID) == world::off::kUid);
static_assert(offsetof(GameObject, m_objectType) == world::off::kType);
static_assert(offsetof(GameObject, m_positionX) == world::off::kPosX);
static_assert(offsetof(GameObject, m_positionY) == world::off::kPosY);
static_assert(offsetof(GameObject, m_isDisabled) == world::off::kDisabled);
static_assert(offsetof(GameObject, m_objectID) == world::off::kObjectId);
static_assert(offsetof(GameObject, m_groups) == world::off::kGroupIds);
static_assert(offsetof(GameObject, m_groupCount) == world::off::kGroupCount);
static_assert(offsetof(GameObject, m_enabledGroupsCounter) == world::off::kToggleCounter);
static_assert(offsetof(GameObject, m_lastPosition) == world::off::kLastPos);
static_assert(offsetof(GameObject, m_unk4C4) == world::off::kMoveMarker);
static_assert(offsetof(GameObject, m_classType) == world::off::kClassType);
static_assert(offsetof(GameObject, m_isDecoration) == world::off::kDecoration);
static_assert(offsetof(GameObject, m_isDecoration2) == world::off::kSkipLast);
static_assert(offsetof(GameObject, m_orientedBox) == world::off::kOrientedBox);
static_assert(offsetof(GameObject, m_shouldUseOuterOb) == world::off::kUseOuterOb);
static_assert(offsetof(GameObject, m_textureRect) == world::off::kRectCaches);
static_assert(offsetof(GameObject, m_colorSpriteLocked) == world::off::kRectCachesEnd);
static_assert(offsetof(GameObject, m_objectRect) == world::off::kObjectRect);
static_assert(offsetof(GameObject, m_isOrientedBoxDirty) == world::off::kDirty368 + 1);

static_assert(offsetof(EnhancedGameObject, m_isNoMultiActivate) == world::off::kNoMultiActivate);
static_assert(offsetof(EnhancedGameObject, m_isMultiActivate) == world::off::kMultiActivate);
static_assert(offsetof(EnhancedGameObject, m_activated) == world::off::kActivated);
static_assert(offsetof(EnhancedGameObject, m_activatedByPlayer1) == world::off::kActivatedP1);
static_assert(offsetof(EnhancedGameObject, m_activatedByPlayer2) == world::off::kActivatedP2);

static_assert(offsetof(EffectGameObject, m_targetGroupID) == world::off::kTargetGroup);
static_assert(offsetof(EffectGameObject, m_isTouchTriggered) == world::off::kTouchTriggered);
static_assert(offsetof(EffectGameObject, m_isSpawnTriggered) == world::off::kSpawnTriggered);
static_assert(offsetof(EffectGameObject, m_activateGroup) == world::off::kActivateGroup);
static_assert(offsetof(EffectGameObject, m_isMultiTriggered) == world::off::kMultiTriggered);
static_assert(offsetof(EffectGameObject, m_controlID) == world::off::kControlId);
static_assert(offsetof(EffectGameObject, m_isSinglePTouch) == world::off::kSinglePTouch);
static_assert(offsetof(EffectGameObject, m_channelValue) == world::off::kChannel);
static_assert(offsetof(EffectGameObject, m_speedStart) == world::off::kSpeedStart);
static_assert(offsetof(TeleportPortalObject, m_orangePortal) == world::off::kOrangePortal);
static_assert(offsetof(RingObject, m_isSpawnOnly) == world::off::kSpawnOnly);

static_assert(offsetof(PlayerObject, m_isDead) == world::off::kPlayerDead);
static_assert(offsetof(PlayerObject, m_isSideways) == world::off::kPlayerSideways);
static_assert(offsetof(PlayerObject, m_isPlatformer) == world::off::kPlayerPlatformer);

static_assert(offsetof(GJEffectManager, m_unkVector560) == world::off::kCommands);
static_assert(offsetof(GJEffectManager, m_unkVector438) == world::off::kToggleBits);
static_assert(offsetof(GJEffectManager, m_unkMap460) == world::off::kToggleSet);
static_assert(offsetof(GJEffectManager, m_spawnTriggerActions) == world::off::kSpawnActions);
static_assert(offsetof(GJEffectManager, m_unkVector6c0) == world::off::kGroupNodes);
static_assert(offsetof(GJEffectManager, m_unkVector708) == world::off::kLocalOffsets);
static_assert(offsetof(GJEffectManager, m_unk780) == world::off::kLockInputs);
static_assert(offsetof(GJEffectManager, m_unk798) == world::off::kLockInputsEnd);

static_assert(sizeof(GroupCommandObject2) == world::off::kCommandSize);
static_assert(offsetof(GroupCommandObject2, m_duration) == world::off::kCmdDuration);
static_assert(offsetof(GroupCommandObject2, m_deltaTime) == world::off::kCmdDeltaTime);
static_assert(offsetof(GroupCommandObject2, m_targetGroupID) == world::off::kCmdTargetGroup);
static_assert(offsetof(GroupCommandObject2, m_finished) == world::off::kCmdFinished);
static_assert(offsetof(GroupCommandObject2, m_commandType) == world::off::kCmdCommandType);
static_assert(offsetof(GroupCommandObject2, m_easingType) == world::off::kCmdEasingType);
static_assert(offsetof(GroupCommandObject2, m_easingRate) == world::off::kCmdEasingRate);
static_assert(offsetof(GroupCommandObject2, m_currentXOffset) == world::off::kCmdCurrentX);
static_assert(offsetof(GroupCommandObject2, m_deltaX) == world::off::kCmdDeltaX);
static_assert(offsetof(GroupCommandObject2, m_disabled) == world::off::kCmdDisabled);
static_assert(offsetof(GroupCommandObject2, m_lockedInX) == world::off::kCmdLockedInX);
static_assert(offsetof(GroupCommandObject2, m_moveModX) == world::off::kCmdModX);
static_assert(offsetof(GroupCommandObject2, m_currentRotateOrTransformValue) == world::off::kCmdRotateValue);
static_assert(offsetof(GroupCommandObject2, m_triggerUniqueID) == world::off::kCmdTriggerUid);
static_assert(offsetof(GroupCommandObject2, m_deltaX_3) == world::off::kCmdDeltaX3);
static_assert(offsetof(GroupCommandObject2, m_actionType1) == world::off::kCmdActionType1);
static_assert(offsetof(GroupCommandObject2, m_actionValue1) == world::off::kCmdActionValue1);
static_assert(offsetof(GroupCommandObject2, m_deltaTimeInFloat) == world::off::kCmdDeltaTimeFloat);
static_assert(offsetof(GroupCommandObject2, m_alreadyUpdated) == world::off::kCmdAlreadyUpdated);

static_assert(offsetof(GJEffectManager, m_unkVector1e0) == world::off::kTouchListeners);
static_assert(offsetof(GJEffectManager, m_countTriggerActions) == world::off::kCountListeners);
static_assert(offsetof(GJEffectManager, m_unkVector230) == world::off::kCollisionListeners);
static_assert(offsetof(GJEffectManager, m_itemCountMap) == world::off::kItems);
static_assert(offsetof(GJEffectManager, m_persistentItemCountMap) == world::off::kPersistentItems);
static_assert(offsetof(GJEffectManager, m_timerItemMap) == world::off::kTimers);
static_assert(offsetof(GJEffectManager, m_unkMap3f8) == world::off::kTimerListeners);
static_assert(offsetof(GJEffectManager, m_unkMap498) == world::off::kTriggeredIds);

static_assert(sizeof(SpawnTriggerAction) == world::off::kSpawnActionSize);
static_assert(offsetof(SpawnTriggerAction, m_duration) == 0x08 && offsetof(SpawnTriggerAction, m_deltaTime) == 0x10 &&
              offsetof(SpawnTriggerAction, m_targetGroupID) == 0x18 && offsetof(SpawnTriggerAction, m_triggerUniqueID) == 0x1c &&
              offsetof(SpawnTriggerAction, m_controlID) == 0x20 && offsetof(SpawnTriggerAction, m_spawnOrdered) == 0x24 &&
              offsetof(SpawnTriggerAction, m_gameObject) == 0x28 && offsetof(SpawnTriggerAction, m_remapKeys) == 0x30);
static_assert(sizeof(CountTriggerAction) == world::off::kCountActionSize);
static_assert(offsetof(CountTriggerAction, m_previousCount) == 0x04 && offsetof(CountTriggerAction, m_targetCount) == 0x08 &&
              offsetof(CountTriggerAction, m_targetGroupID) == 0x0c && offsetof(CountTriggerAction, m_activateGroup) == 0x10 &&
              offsetof(CountTriggerAction, m_triggerUniqueID) == 0x14 && offsetof(CountTriggerAction, m_controlID) == 0x18 &&
              offsetof(CountTriggerAction, m_itemID) == 0x1c && offsetof(CountTriggerAction, m_multiActivate) == 0x20 &&
              offsetof(CountTriggerAction, m_remapKeys) == 0x28);
static_assert(sizeof(CollisionTriggerAction) == world::off::kCollisionActionSize);
static_assert(offsetof(CollisionTriggerAction, m_blockAID) == 0x04 && offsetof(CollisionTriggerAction, m_blockBID) == 0x08 &&
              offsetof(CollisionTriggerAction, m_targetGroupID) == 0x0c && offsetof(CollisionTriggerAction, m_triggerOnExit) == 0x10 &&
              offsetof(CollisionTriggerAction, m_activateGroup) == 0x14 && offsetof(CollisionTriggerAction, m_triggerUniqueID) == 0x18 &&
              offsetof(CollisionTriggerAction, m_controlID) == 0x1c && offsetof(CollisionTriggerAction, m_remapKeys) == 0x20);
static_assert(sizeof(TouchToggleAction) == world::off::kTouchActionSize);
static_assert(offsetof(TouchToggleAction, m_touchTriggerControl) == 0x10 && offsetof(TouchToggleAction, m_triggerUniqueID) == 0x14 &&
              offsetof(TouchToggleAction, m_controlID) == 0x18 && offsetof(TouchToggleAction, m_dualMode) == 0x1c &&
              offsetof(TouchToggleAction, m_remapKeys) == 0x20);
static_assert(sizeof(TimerTriggerAction) == world::off::kTimerActionSize);
static_assert(offsetof(TimerTriggerAction, m_time) == 0x04 && offsetof(TimerTriggerAction, m_targetTime) == 0x08 &&
              offsetof(TimerTriggerAction, m_targetGroupID) == 0x0c && offsetof(TimerTriggerAction, m_multiActivate) == 0x1c &&
              offsetof(TimerTriggerAction, m_remapKeys) == 0x20);
static_assert(sizeof(TimerItem) == world::off::kTimerItemSize);
static_assert(offsetof(TimerItem, m_time) == 0x08 && offsetof(TimerItem, m_active) == 0x10 && offsetof(TimerItem, m_timeMod) == 0x14 &&
              offsetof(TimerItem, m_targetTime) == 0x20 && offsetof(TimerItem, m_stopTimeEnabled) == 0x28 &&
              offsetof(TimerItem, m_targetGroupID) == 0x2c && offsetof(TimerItem, m_triggerUniqueID) == 0x30 &&
              offsetof(TimerItem, m_controlID) == 0x34 && offsetof(TimerItem, m_remapKeys) == 0x38 &&
              offsetof(TimerItem, m_disabled) == 0x50);
static_assert(sizeof(EventTriggerInstance) == world::off::kEventInstanceSize);
static_assert(offsetof(EventTriggerInstance, m_uniqueID) == 0x04 && offsetof(EventTriggerInstance, m_controlID) == 0x08 &&
              offsetof(EventTriggerInstance, m_inactive) == 0x0c && offsetof(EventTriggerInstance, m_remapKeys) == 0x10);
static_assert(sizeof(DynamicObjectAction) == world::off::kDynamicActionSize);
static_assert(offsetof(DynamicObjectAction, m_targetGroupID) == 0x54 && offsetof(DynamicObjectAction, m_centerGroupID) == 0x58);
static_assert(sizeof(AdvancedFollowInstance) == world::off::kAdvancedFollowSize);
static_assert(offsetof(AdvancedFollowInstance, m_group) == 0x08);
static_assert(sizeof(EnterEffectInstance) == world::off::kAreaInstanceSize);
static_assert(offsetof(EnterEffectInstance, m_gameObject) == 0xa0 && offsetof(EnterEffectInstance, m_targetID) == 0xac &&
              offsetof(EnterEffectInstance, m_centerID) == 0xb0 && offsetof(EnterEffectInstance, m_paused) == 0xbc);

static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeWarp) == world::off::kTimeWarp);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_queuedTimeWarp) == world::off::kQueuedTimeWarp);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeWarpRelated) == world::off::kAppliedTimeWarp);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeModRelated2) == world::off::kParkedSpeedFlag);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_unkMapPairGJGameEventIntVectorEventTriggerInstance) ==
              world::off::kEventListeners);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_unkMapPairGJGameEventIntInt) == world::off::kEventStamps);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_moveEffectInstances) == world::off::kAreaMoves);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_rotateEffectInstances) == world::off::kAreaRotates);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_scaleEffectInstances) == world::off::kAreaScales);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_advanceFollowInstances) == world::off::kAdvancedFollows);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_dynamicMoveActions) == world::off::kDynamicMoves);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_dynamicRotateActions) == world::off::kDynamicRotates);
static_assert(offsetof(GJBaseGameLayer, m_loadingProgress) == world::off::kLoadingProgress);

static_assert(offsetof(EffectGameObject, m_duration) == world::off::kEffDuration);
static_assert(offsetof(EffectGameObject, m_centerGroupID) == world::off::kEffCenterGroup);
static_assert(offsetof(EffectGameObject, m_moveOffset) == world::off::kEffMoveOffset);
static_assert(offsetof(EffectGameObject, m_lockToPlayerX) == world::off::kEffLockPlayerX);
static_assert(offsetof(EffectGameObject, m_useMoveTarget) == world::off::kEffUseTarget);
static_assert(offsetof(EffectGameObject, m_moveTargetMode) == world::off::kEffTargetMode);
static_assert(offsetof(EffectGameObject, m_moveModX) == world::off::kEffModX);
static_assert(offsetof(EffectGameObject, m_moveModY) == world::off::kEffModY);
static_assert(offsetof(EffectGameObject, m_isDirectionFollowSnap360) == world::off::kEffDirectionMode);
static_assert(offsetof(EffectGameObject, m_targetModCenterID) == world::off::kEffTargetModCenter);
static_assert(offsetof(EffectGameObject, m_directionModeDistance) == world::off::kEffDirectionDistance);
static_assert(offsetof(EffectGameObject, m_isDynamicMode) == world::off::kEffDynamic);
static_assert(offsetof(EffectGameObject, m_isSilent) == world::off::kEffSilent);
static_assert(offsetof(EffectGameObject, m_controlID) == world::off::kEffControlId);
static_assert(offsetof(EffectGameObject, m_targetPlayer1) == world::off::kEffTargetP1);
static_assert(offsetof(EffectGameObject, m_targetPlayer2) == world::off::kEffTargetP2);
static_assert(offsetof(EnhancedGameObject, m_animateOnTrigger) == world::off::kAnimateOnTrigger);
static_assert(offsetof(EffectGameObject, m_itemID) == world::off::kEffItemId);
static_assert(offsetof(EffectGameObject, m_itemID2) == world::off::kEffItemId2);

// The Tier C triggers (world/fire.cpp (aa)-(ah)), each name pinned to the
// place its own function reads.
static_assert(offsetof(TimerTriggerGameObject, m_startTime) == world::off::kTimerStart);
static_assert(offsetof(TimerTriggerGameObject, m_targetTime) == world::off::kTimerTarget);
static_assert(offsetof(TimerTriggerGameObject, m_stopTimeEnabled) == world::off::kTimerStopEnabled);
static_assert(offsetof(TimerTriggerGameObject, m_dontOverride) == world::off::kTimerStopEnabled + 1);
static_assert(offsetof(TimerTriggerGameObject, m_ignoreTimeWarp) == world::off::kTimerStopEnabled + 2);
static_assert(offsetof(TimerTriggerGameObject, m_timeMod) == world::off::kTimerStopEnabled + 4);
static_assert(offsetof(TimerTriggerGameObject, m_startPaused) == world::off::kTimerStopEnabled + 8);
static_assert(offsetof(TimerTriggerGameObject, m_multiActivate) == world::off::kTimerStopEnabled + 9);
static_assert(offsetof(TimerTriggerGameObject, m_controlType) == world::off::kTimerControlType);
// TimerItem, as updateTimers 0x2634d5 walks it. The binary's offsets are 0x18
// higher than these because a node of the map holds its next and prev pointers
// first (the pair is 0x10 past the node's start) and the pair holds the item
// id it is keyed by before the TimerItem itself (another 8). The key is what
// updateTimers copies at +0x10; m_itemID, the field a timer's listeners are
// looked up by, is the TimerItem's own at +0x18.
static_assert(offsetof(TimerItem, m_time) == 0x08 && offsetof(TimerItem, m_active) == 0x10 &&
              offsetof(TimerItem, m_timeMod) == 0x14 && offsetof(TimerItem, m_ignoreTimeWarp) == 0x18 &&
              offsetof(TimerItem, m_targetTime) == 0x20 && offsetof(TimerItem, m_stopTimeEnabled) == 0x28 &&
              offsetof(TimerItem, m_targetGroupID) == 0x2c && offsetof(TimerItem, m_triggerUniqueID) == 0x30 &&
              offsetof(TimerItem, m_controlID) == 0x34 && offsetof(TimerItem, m_remapKeys) == 0x38 &&
              offsetof(TimerItem, m_disabled) == 0x50);
static_assert(offsetof(ItemTriggerGameObject, m_item1Mode) == world::off::kItemMode1);
static_assert(offsetof(ItemTriggerGameObject, m_item2Mode) == world::off::kItemMode1 + 4);
static_assert(offsetof(ItemTriggerGameObject, m_targetItemMode) == world::off::kItemMode1 + 8);
static_assert(offsetof(ItemTriggerGameObject, m_mod1) == world::off::kItemMode1 + 0xc);
static_assert(offsetof(ItemTriggerGameObject, m_mod2) == world::off::kItemMode1 + 0x10);
static_assert(offsetof(ItemTriggerGameObject, m_resultType1) == world::off::kItemMode1 + 0x14);
static_assert(offsetof(ItemTriggerGameObject, m_resultType2) == world::off::kItemMode1 + 0x18);
static_assert(offsetof(ItemTriggerGameObject, m_resultType3) == world::off::kItemMode1 + 0x1c);
static_assert(offsetof(ItemTriggerGameObject, m_tolerance) == world::off::kItemMode1 + 0x20);
static_assert(offsetof(ItemTriggerGameObject, m_roundType1) == world::off::kItemMode1 + 0x24);
static_assert(offsetof(ItemTriggerGameObject, m_roundType2) == world::off::kItemMode1 + 0x28);
static_assert(offsetof(ItemTriggerGameObject, m_signType1) == world::off::kItemMode1 + 0x2c);
static_assert(offsetof(ItemTriggerGameObject, m_signType2) == world::off::kItemSign2);
static_assert(offsetof(ItemTriggerGameObject, m_persistent) == world::off::kItemPersistent);
static_assert(offsetof(ItemTriggerGameObject, m_targetAll) == world::off::kItemPersistent + 1);
static_assert(offsetof(ItemTriggerGameObject, m_reset) == world::off::kItemPersistent + 2);
static_assert(offsetof(ItemTriggerGameObject, m_timer) == world::off::kItemPersistent + 3);
static_assert(offsetof(SequenceTriggerGameObject, m_sequenceState) == world::off::kSeqState);
static_assert(offsetof(SequenceTriggerGameObject, m_minInt) == world::off::kSeqMinInterval);
static_assert(offsetof(SequenceTriggerGameObject, m_sequenceMode) == world::off::kSeqMode);
static_assert(offsetof(SequenceTriggerGameObject, m_resetMode) == world::off::kSeqResetMode);
static_assert(offsetof(SequenceTriggerGameObject, m_reset) == world::off::kSeqReset);
static_assert(offsetof(SequenceTriggerGameObject, m_sequenceTotalCount) == world::off::kSeqTotalCount);
static_assert(offsetof(SequenceTriggerGameObject, m_uniqueRemap) == world::off::kSeqUniqueRemap);
static_assert(offsetof(EventLinkTrigger, m_eventIDs) == world::off::kEventIds);
static_assert(offsetof(EventLinkTrigger, m_extraID) == world::off::kEventExtraId);
static_assert(offsetof(EventLinkTrigger, m_extraID2) == world::off::kEventExtraId + 4);
static_assert(offsetof(GameOptionsTrigger, m_streakAdditive) == world::off::kOptStreakAdditive);
static_assert(offsetof(GameOptionsTrigger, m_unlinkDualGravity) == world::off::kOptStreakAdditive + 4);
static_assert(offsetof(GameOptionsTrigger, m_disableP1Controls) == world::off::kOptStreakAdditive + 0x14);
static_assert(offsetof(GameOptionsTrigger, m_disableP2Controls) == world::off::kOptStreakAdditive + 0x18);
static_assert(offsetof(GameOptionsTrigger, m_boostSlide) == world::off::kOptBoostSlide);
static_assert(offsetof(PlayerControlGameObject, m_stopJump) == world::off::kCtrlStopJump);
static_assert(offsetof(PlayerControlGameObject, m_stopSlide) == world::off::kCtrlStopJump + 3);
static_assert(offsetof(PlayerObject, m_controlsDisabled) == world::off::kPlayerControlsDisabled);
static_assert(offsetof(PlayerObject, m_inputsLocked) == world::off::kPlayerInputsLocked);
static_assert(offsetof(PlayerObject, m_rotationSpeed) == world::off::kPlayerRotateSpeed);
static_assert(offsetof(PlayerObject, m_isRotating) == world::off::kPlayerRotating);
static_assert(offsetof(PlayerObject, m_isBallRotating2) == world::off::kPlayerRotating + 1);
static_assert(offsetof(PlayerObject, m_isBallRotating) == world::off::kPlayerBallRotating);
static_assert(offsetof(PlayerObject, m_isAccelerating) == world::off::kPlayerAccelerating);
static_assert(offsetof(PlayerObject, m_affectedByForces) == world::off::kPlayerAffectedByForces);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_unkBool31) == world::off::kUnlinkedDual);
// activatePlayerControlTrigger 0x2175a4 asks the layer whether the level is a
// platformer before it releases the movement buttons.
static_assert(offsetof(GJBaseGameLayer, m_isPlatformer) == 0x309e);
// getItemValue 0x234203: item mode 3 is the level's points, which the item
// edit trigger's target mode 3 writes (+0x864, with the flag at +0x868).
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_points) == 0x864);
// The time warp: 1935 parks m_timeWarpTimeMod (+0x6f4 at 0x4a736e) in
// m_queuedTimeWarp (+0x334), and updateTimeWarp 0x23617d moves it to
// m_timeWarp (+0x330); applyTimeWarp 0x2361c1 writes m_timeWarpRelated (+0x338).
static_assert(offsetof(EffectGameObject, m_timeWarpTimeMod) == 0x6f4);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeWarp) == 0x330);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_queuedTimeWarp) == 0x334);
static_assert(world::off::detail::kGs + offsetof(GJGameState, m_timeWarpRelated) == 0x338);
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

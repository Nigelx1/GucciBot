#include "analysis/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "trailbuf/trailbuf.hpp"
#include "analysis/ac/shim.hpp"
#include "audio/clicksounds.hpp"
#include "trainers/calibration.hpp"

#include <fstream>
#include <fmt/format.h>

#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

using namespace gucci;

namespace {
    constexpr int kMaxTraceFrames = 480;
    constexpr float kIndicatorFlashDuration = 0.15f;

    const std::unordered_set<int> kInteractivePortalIds = {
        101, 99, 11, 10, 200, 201, 202, 203, 1334};

    struct ActivationSnapshot {
        bool activated;
        bool activatedByPlayer1;
        bool activatedByPlayer2;
        bool isActivated;
        bool isDisabled;
        bool isDisabled2;
    };

    class TrajectoryDrawNode final : public cocos2d::CCDrawNode {
    public:
        static TrajectoryDrawNode* create();
    };

    ActivationSnapshot captureActivation(EffectGameObject* object);
    void restoreActivation(EffectGameObject* object, ActivationSnapshot const& snapshot);
} // namespace

TrajectoryPredictionService& TrajectoryPredictionService::get() {
    static TrajectoryPredictionService service;
    return service;
}

bool TrajectoryPredictionService::isSimulatedPad(GameObjectType type) {
    switch (type) {
    case GameObjectType::YellowJumpPad:
    case GameObjectType::PinkJumpPad:
    case GameObjectType::RedJumpPad:
    case GameObjectType::GravityPad:
    case GameObjectType::SpiderPad:
        return true;
    default:
        return false;
    }
}

bool TrajectoryPredictionService::isSimulatedOrb(GameObjectType type) {
    switch (type) {
    case GameObjectType::YellowJumpRing:
    case GameObjectType::PinkJumpRing:
    case GameObjectType::GravityRing:
    case GameObjectType::GreenRing:
    case GameObjectType::RedJumpRing:
    case GameObjectType::DropRing:
    case GameObjectType::DashRing:
    case GameObjectType::GravityDashRing:
    case GameObjectType::SpiderOrb:
    case GameObjectType::CustomRing:
    case GameObjectType::TeleportOrb:
        return true;
    default:
        return false;
    }
}

namespace {
    TrajectoryDrawNode* TrajectoryDrawNode::create() {
        auto* node = new TrajectoryDrawNode();
        if (!node->init()) {
            delete node;
            return nullptr;
        }

        node->autorelease();
        node->m_bUseArea = false;
        return node;
    }

    ActivationSnapshot captureActivation(EffectGameObject* object) {
        return {object->m_activated,
                object->m_activatedByPlayer1,
                object->m_activatedByPlayer2,
                object->m_isActivated,
                object->m_isDisabled,
                object->m_isDisabled2};
    }

    void restoreActivation(EffectGameObject* object, ActivationSnapshot const& snapshot) {
        object->m_activated = snapshot.activated;
        object->m_activatedByPlayer1 = snapshot.activatedByPlayer1;
        object->m_activatedByPlayer2 = snapshot.activatedByPlayer2;
        object->m_isActivated = snapshot.isActivated;
        object->m_isDisabled = snapshot.isDisabled;
        object->m_isDisabled2 = snapshot.isDisabled2;
    }
} // namespace

bool TrajectoryPredictionService::watchChanged(PredictionWatchKey const& lhs,
                                               PredictionWatchKey const& rhs) {
    return lhs.position.x != rhs.position.x || lhs.position.y != rhs.position.y ||
           lhs.verticalVelocity != rhs.verticalVelocity || lhs.rotation != rhs.rotation ||
           lhs.gravityInverted != rhs.gravityInverted || lhs.movementSpeed != rhs.movementSpeed ||
           lhs.grounded != rhs.grounded || lhs.scale != rhs.scale || lhs.dashing != rhs.dashing ||
           lhs.inShipMode != rhs.inShipMode || lhs.inUfoMode != rhs.inUfoMode ||
           lhs.inBallMode != rhs.inBallMode || lhs.inWaveMode != rhs.inWaveMode ||
           lhs.inRobotMode != rhs.inRobotMode || lhs.inSpiderMode != rhs.inSpiderMode ||
           lhs.inSwingMode != rhs.inSwingMode || lhs.isGoingLeft != rhs.isGoingLeft ||
           lhs.isSideways != rhs.isSideways || lhs.reverseRelated != rhs.reverseRelated;
}

PredictionWatchKey TrajectoryPredictionService::buildWatchKey(PlayerObject* player) {
    return {player->getPosition(),
            player->m_yVelocity,
            player->getRotation(),
            player->m_isUpsideDown,
            player->m_playerSpeed,
            player->m_isOnGround,
            player->m_vehicleSize,
            player->m_isDashing,
            player->m_isShip,
            player->m_isBird,
            player->m_isBall,
            player->m_isDart,
            player->m_isRobot,
            player->m_isSpider,
            player->m_isSwing,
            player->m_isGoingLeft,
            player->m_isSideways,
            player->m_reverseRelated};
}

PlayerStateCapsule TrajectoryPredictionService::capturePlayerState(PlayerObject* player) {
    PlayerStateCapsule state;

    state.motion = {player->getPosition(),
                    player->m_lastPosition,
                    player->m_yVelocity,
                    player->m_yVelocityBeforeSlope,
                    player->getRotation(),
                    player->m_vehicleSize,
                    player->m_playerSpeed,
                    player->m_gravityMod,
                    player->m_totalTime,
                    player->m_objectType};

    state.form = {player->m_isUpsideDown,
                  player->m_isOnSlope,
                  player->m_wasOnSlope,
                  player->m_isShip,
                  player->m_isBird,
                  player->m_isBall,
                  player->m_isDart,
                  player->m_isRobot,
                  player->m_isSpider,
                  player->m_isSwing,
                  player->m_isOnGround,
                  player->m_isDashing,
                  player->m_isGoingLeft,
                  player->m_isSideways,
                  player->m_reverseRelated,
                  player->m_maybeReverseSpeed,
                  player->m_maybeReverseAcceleration};

    state.interaction = {player->m_padRingRelated,
                         player->m_ringJumpRelated,
                         player->m_ringRelatedSet,
                         player->m_touchedRing,
                         player->m_touchedCustomRing,
                         player->m_touchedPad,
                         player->m_lastActivatedPortal,
                         player->m_lastPortalPos,
                         player->m_playEffects};

    state.slope = {player->m_currentSlope,
                   player->m_currentSlope2,
                   player->m_currentPotentialSlope,
                   player->m_slopeAngle,
                   player->m_slopeAngleRadians,
                   player->m_isCollidingWithSlope,
                   player->m_collidingWithSlopeId,
                   player->m_slopeFlipGravityRelated,
                   player->m_slopeVelocity,
                   player->m_currentSlopeYVelocity,
                   player->m_isCurrentSlopeTop,
                   player->m_slopeSlidingMaybeRotated,
                   player->m_slopeRotation,
                   player->m_maybeSlopeForce,
                   player->m_maybeUpsideDownSlope,
                   player->m_maybeGoingCorrectSlopeDirection,
                   player->m_isSliding,
                   player->m_isSlidingRight,
                   player->m_slopeStartTime,
                   player->m_slopeEndTime};

    state.collision = {player->m_lastGroundObject,
                       player->m_preLastGroundObject,
                       player->m_collidedObject,
                       player->m_collidingWithLeft,
                       player->m_collidingWithRight,
                       player->m_groundYVelocity,
                       player->m_lastCollisionBottom,
                       player->m_lastCollisionTop,
                       player->m_lastCollisionLeft,
                       player->m_lastCollisionRight,
                       player->m_isOnGround2,
                       player->m_isOnGround3,
                       player->m_isOnGround4,
                       player->m_fallSpeed,
                       player->m_maybeIsColliding};

    return state;
}

void TrajectoryPredictionService::applyPlayerState(PlayerObject* player,
                                                   PlayerStateCapsule const& state) {
    player->setPosition(state.motion.position);
    player->m_lastPosition = state.motion.previousPosition;
    player->m_yVelocity = state.motion.verticalVelocity;
    player->m_yVelocityBeforeSlope = state.motion.preSlopeVelocity;
    player->setRotation(state.motion.rotation);
    player->m_vehicleSize = state.motion.scale;
    player->m_playerSpeed = state.motion.movementSpeed;
    player->m_gravityMod = state.motion.gravityFactor;
    player->m_totalTime = state.motion.totalTime;
    player->m_objectType = state.motion.objectType;

    player->m_isUpsideDown = state.form.gravityInverted;
    player->m_isOnSlope = state.form.onSlope;
    player->m_wasOnSlope = state.form.wasOnSlope;
    player->m_isShip = state.form.inShipMode;
    player->m_isBird = state.form.inUfoMode;
    player->m_isBall = state.form.inBallMode;
    player->m_isDart = state.form.inWaveMode;
    player->m_isRobot = state.form.inRobotMode;
    player->m_isSpider = state.form.inSpiderMode;
    player->m_isSwing = state.form.inSwingMode;
    player->m_isOnGround = state.form.grounded;
    player->m_isDashing = state.form.dashing;
    player->m_isGoingLeft = state.form.isGoingLeft;
    player->m_isSideways = state.form.isSideways;
    player->m_reverseRelated = state.form.reverseRelated;
    player->m_maybeReverseSpeed = state.form.reverseSpeed;
    player->m_maybeReverseAcceleration = state.form.reverseAcceleration;

    player->m_padRingRelated = state.interaction.padRingRelated;
    player->m_ringJumpRelated = state.interaction.ringJumpRelated;
    player->m_ringRelatedSet = state.interaction.ringRelatedSet;
    player->m_touchedRing = state.interaction.touchedRing;
    player->m_touchedCustomRing = state.interaction.touchedCustomRing;
    player->m_touchedPad = state.interaction.touchedPad;
    player->m_lastActivatedPortal = state.interaction.lastActivatedPortal;
    player->m_lastPortalPos = state.interaction.lastPortalPos;
    player->m_playEffects = state.interaction.playEffects;

    player->m_currentSlope = state.slope.currentSlope;
    player->m_currentSlope2 = state.slope.currentSlopeSecondary;
    player->m_currentPotentialSlope = state.slope.currentPotentialSlope;
    player->m_slopeAngle = state.slope.slopeAngle;
    player->m_slopeAngleRadians = state.slope.slopeAngleRadians;
    player->m_isCollidingWithSlope = state.slope.collidingWithSlope;
    player->m_collidingWithSlopeId = state.slope.collidingWithSlopeId;
    player->m_slopeFlipGravityRelated = state.slope.slopeFlipGravityRelated;
    player->m_slopeVelocity = state.slope.slopeVelocity;
    player->m_currentSlopeYVelocity = state.slope.currentSlopeVelocity;
    player->m_isCurrentSlopeTop = state.slope.currentSlopeTop;
    player->m_slopeSlidingMaybeRotated = state.slope.slopeSlideRotated;
    player->m_slopeRotation = state.slope.slopeRotation;
    player->m_maybeSlopeForce = state.slope.slopeForce;
    player->m_maybeUpsideDownSlope = state.slope.upsideDownSlope;
    player->m_maybeGoingCorrectSlopeDirection = state.slope.movingWithSlopeDirection;
    player->m_isSliding = state.slope.sliding;
    player->m_isSlidingRight = state.slope.slidingRight;
    player->m_slopeStartTime = state.slope.slopeStartTime;
    player->m_slopeEndTime = state.slope.slopeEndTime;

    player->m_lastGroundObject = state.collision.lastGroundObject;
    player->m_preLastGroundObject = state.collision.preLastGroundObject;
    player->m_collidedObject = state.collision.collidedObject;
    player->m_collidingWithLeft = state.collision.collidingWithLeft;
    player->m_collidingWithRight = state.collision.collidingWithRight;
    player->m_groundYVelocity = state.collision.groundYVelocity;
    player->m_lastCollisionBottom = state.collision.lastCollisionBottom;
    player->m_lastCollisionTop = state.collision.lastCollisionTop;
    player->m_lastCollisionLeft = state.collision.lastCollisionLeft;
    player->m_lastCollisionRight = state.collision.lastCollisionRight;
    player->m_isOnGround2 = state.collision.isOnGround2;
    player->m_isOnGround3 = state.collision.isOnGround3;
    player->m_isOnGround4 = state.collision.isOnGround4;
    player->m_fallSpeed = state.collision.fallSpeed;
    player->m_maybeIsColliding = state.collision.maybeColliding;
}
bool TrajectoryPredictionService::isActiveSimulation() const {
    return m_context.activeSimulation;
}

bool TrajectoryPredictionService::isProcessingOrbTouch() const {
    return m_context.processingOrbTouch;
}

void TrajectoryPredictionService::markDirty() {
    m_context.dirty = true;
}

void TrajectoryPredictionService::clearOverlay() {
    auto* drawNode = ensureDrawNode();
    if (!drawNode) {
        return;
    }

    drawNode->clear();
    drawNode->setVisible(false);
}

cocos2d::CCDrawNode* TrajectoryPredictionService::ensureDrawNode() {
    if (!m_drawNode) {
        auto* drawNode = TrajectoryDrawNode::create();
        if (!drawNode) {
            return nullptr;
        }

        drawNode->retain();
        drawNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        m_drawNode = drawNode;
    }

    return m_drawNode;
}

void TrajectoryPredictionService::attach(PlayLayer* playLayer) {
    detach();
    if (!playLayer || !playLayer->m_objectLayer) {
        return;
    }

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex) {
        auto* previewPlayer = PlayerObject::create(1, 1, playLayer, playLayer, true);
        if (!previewPlayer) {
            continue;
        }

        previewPlayer->setVisible(false);
        previewPlayer->setPosition({0.0f, 105.0f});
        playLayer->m_objectLayer->addChild(previewPlayer);
        m_context.previewPlayers[playerIndex] = previewPlayer;
    }

    if (auto* drawNode = ensureDrawNode()) {
        if (drawNode->getParent() != playLayer->m_objectLayer) {
            if (drawNode->getParent()) {
                drawNode->removeFromParent();
            }
            playLayer->m_objectLayer->addChild(drawNode, 9999);
        }
        drawNode->clear();
        drawNode->setVisible(false);
    }

    m_context.dirty = true;
    recalculateOverlapColors();
}

void TrajectoryPredictionService::detach() {
    m_context.previewPlayers[0] = nullptr;
    m_context.previewPlayers[1] = nullptr;
    m_context.activeSimulation = false;
    m_context.traceCancelled = false;
    m_context.holdingTrace = false;
    m_context.processedOrbs.clear();
    m_context.touchingPads.clear();
    m_context.frameTouchingPads.clear();
    m_context.dirty = true;

    if (m_drawNode) {
        m_drawNode->clear();
        m_drawNode->setVisible(false);
    }
}

void TrajectoryPredictionService::captureFrameDelta(float dt) {
    if (!m_context.activeSimulation) {
        m_context.stepDelta = dt;
    }
}

bool TrajectoryPredictionService::ownsPreviewPlayer(PlayerObject* player) const {
    return player &&
           (player == m_context.previewPlayers[0] || player == m_context.previewPlayers[1]);
}

void TrajectoryPredictionService::noteSimulatedDeath(PlayerObject* player, GameObject* killer) {
    if (!player) {
        return;
    }

    m_context.collisionRotation = player->getRotation();
    m_context.traceCancelled = true;

    m_forkDeaths++;
    m_lastKillerId = killer ? killer->m_objectID : -1;
    m_lastKillerType = killer ? static_cast<int>(killer->m_objectType) : -1;
}

void TrajectoryPredictionService::recalculateOverlapColors() {
    m_overlapColor = {std::min(1.0f, (m_holdColor.r + m_releaseColor.r) * 0.5f + 0.45f),
                      std::min(1.0f, (m_holdColor.g + m_releaseColor.g) * 0.5f + 0.45f),
                      std::min(1.0f, (m_holdColor.b + m_releaseColor.b) * 0.5f + 0.45f),
                      1.0f};

    m_overlapColorP2 = {std::min(1.0f, (m_holdColorP2.r + m_releaseColor.r) * 0.5f + 0.45f),
                        std::min(1.0f, (m_holdColorP2.g + m_releaseColor.g) * 0.5f + 0.45f),
                        std::min(1.0f, (m_holdColorP2.b + m_releaseColor.b) * 0.5f + 0.45f),
                        1.0f};
}

std::vector<CCPoint>
TrajectoryPredictionService::buildPlayerBounds(PlayerObject* player, CCRect bounds, float angle) {
    std::vector<CCPoint> vertices = {ccp(bounds.getMinX(), bounds.getMaxY()),
                                     ccp(bounds.getMaxX(), bounds.getMaxY()),
                                     ccp(bounds.getMaxX(), bounds.getMinY()),
                                     ccp(bounds.getMinX(), bounds.getMinY())};

    CCPoint center = ccp((bounds.getMinX() + bounds.getMaxX()) * 0.5f,
                         (bounds.getMinY() + bounds.getMaxY()) * 0.5f);

    float dimension = static_cast<float>(static_cast<int>(bounds.getMaxX() - bounds.getMinX()));
    if ((dimension == 18.0f || dimension == 5.0f) && player->getScale() == 1.0f) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) / 0.6f;
            vertex.y = center.y + (vertex.y - center.y) / 0.6f;
        }
    }

    if ((dimension == 7.0f || dimension == 30.0f || dimension == 29.0f || dimension == 9.0f) &&
        player->getScale() != 1.0f) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) * 0.6f;
            vertex.y = center.y + (vertex.y - center.y) * 0.6f;
        }
    }

    if (player->m_isDart) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) * 0.3f;
            vertex.y = center.y + (vertex.y - center.y) * 0.3f;
        }
    }

    float radians = CC_DEGREES_TO_RADIANS(angle * -1.0f);
    for (auto& vertex : vertices) {
        float dx = vertex.x - center.x;
        float dy = vertex.y - center.y;
        vertex.x = center.x + (dx * cos(radians)) - (dy * sin(radians));
        vertex.y = center.y + (dx * sin(radians)) + (dy * cos(radians));
    }

    return vertices;
}

void TrajectoryPredictionService::drawPredictionBounds(PlayerObject* player) {
    auto* drawNode = ensureDrawNode();
    if (!drawNode || !player) {
        return;
    }

    CCRect outerBounds = player->GameObject::getObjectRect();
    CCRect innerBounds = player->GameObject::getObjectRect(0.3f, 0.3f);

    auto outerVertices = buildPlayerBounds(player, outerBounds, m_context.collisionRotation);
    drawNode->drawPolygon(outerVertices.data(),
                          outerVertices.size(),
                          ccc4f(m_releaseColor.r, m_releaseColor.g, m_releaseColor.b, 0.2f),
                          0.5f,
                          m_releaseColor);

    auto innerVertices = buildPlayerBounds(player, innerBounds, m_context.collisionRotation);
    drawNode->drawPolygon(innerVertices.data(),
                          innerVertices.size(),
                          ccc4f(m_overlapColor.r, m_overlapColor.g, m_overlapColor.b, 0.2f),
                          0.35f,
                          ccc4f(m_overlapColor.r, m_overlapColor.g, m_overlapColor.b, 0.55f));
}

std::vector<CCPoint>
TrajectoryPredictionService::buildRingVertices(CCPoint center, float radius, int segments) {
    std::vector<CCPoint> vertices;
    vertices.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        float angle = (2.0f * 3.14159265f * static_cast<float>(i)) / static_cast<float>(segments);
        vertices.push_back(ccp(center.x + radius * cosf(angle), center.y + radius * sinf(angle)));
    }
    return vertices;
}

void TrajectoryPredictionService::drawSurvivalIndicator(PlayerObject* player, bool isSecondPlayer) {
    auto* drawNode = ensureDrawNode();
    if (!drawNode || !player) {
        return;
    }

    int gamemodeIndex = CalibrationService::gamemodeIndexFor(player);
    if (!CalibrationService::get().modes[gamemodeIndex].guideEnabled) {
        return;
    }

    auto* gb = GucciEngine::get();
    int playerIndex = isSecondPlayer ? 1 : 0;
    int survived = m_context.holdSurvivedFrames[playerIndex];
    bool survivable = survived >= gb->indicatorLookahead;

    float margin = static_cast<float>(survived - gb->indicatorLookahead);
    float tightness = std::clamp(
        1.0f - (margin / static_cast<float>(std::max(1, gb->indicatorLookahead))), 0.0f, 1.0f);

    float flash = gb->indicatorFlashEnabled
                      ? (m_context.indicatorFlashTimer[playerIndex] / kIndicatorFlashDuration)
                      : 0.0f;

    ccColor4F baseColor = survivable ? ccc4f(gb->indicatorSafeColorR,
                                             gb->indicatorSafeColorG,
                                             gb->indicatorSafeColorB,
                                             gb->indicatorOpacity)
                                     : ccc4f(gb->indicatorDangerColorR,
                                             gb->indicatorDangerColorG,
                                             gb->indicatorDangerColorB,
                                             gb->indicatorOpacity);
    float flashAlpha = std::min(1.0f, baseColor.a + flash * 0.3f);

    CCPoint center = player->getPosition();

    switch (gb->indicatorStyle) {
    case 1: {
        float half = 7.f + flash * 2.f;
        CCPoint c = center + ccp(0.f, 26.f);
        CCPoint verts[4] = {ccp(c.x - half, c.y - half),
                            ccp(c.x + half, c.y - half),
                            ccp(c.x + half, c.y + half),
                            ccp(c.x - half, c.y + half)};
        drawNode->drawPolygon(verts,
                              4,
                              ccc4f(baseColor.r, baseColor.g, baseColor.b, baseColor.a * 0.85f),
                              2.f,
                              ccc4f(baseColor.r, baseColor.g, baseColor.b, flashAlpha));
        break;
    }
    case 2: {
        float gap = std::max(6.f, 34.f - tightness * 18.f - flash * 10.f);
        float barHalfW = 10.f;
        ccColor4F c = ccc4f(baseColor.r, baseColor.g, baseColor.b, flashAlpha);
        drawNode->drawSegment(ccp(center.x - barHalfW, center.y + gap),
                              ccp(center.x + barHalfW, center.y + gap),
                              2.5f,
                              c);
        drawNode->drawSegment(ccp(center.x - barHalfW, center.y - gap),
                              ccp(center.x + barHalfW, center.y - gap),
                              2.5f,
                              c);
        break;
    }
    case 3: {
        float pulseSpeed = 2.0f + tightness * 6.0f;
        float pulse = 0.5f + 0.5f * sinf(m_context.indicatorPulsePhase * pulseSpeed);
        float radius = 14.f + pulse * 8.f + flash * 8.f;
        ccColor4F ring = ccc4f(baseColor.r,
                               baseColor.g,
                               baseColor.b,
                               std::min(1.0f, baseColor.a * (0.6f + pulse * 0.4f) + flash * 0.3f));
        auto verts = buildRingVertices(center, radius, 20);
        drawNode->drawPolygon(verts.data(),
                              verts.size(),
                              ccc4f(baseColor.r, baseColor.g, baseColor.b, ring.a * 0.25f),
                              2.f,
                              ring);
        break;
    }
    default: {
        float radius = 20.f + flash * 6.f;
        ccColor4F ring = ccc4f(baseColor.r, baseColor.g, baseColor.b, flashAlpha);
        auto verts = buildRingVertices(center, radius, 24);
        drawNode->drawPolygon(
            verts.data(), verts.size(), ccc4f(0.f, 0.f, 0.f, 0.f), 2.5f + flash * 1.5f, ring);
        break;
    }
    }
}

void TrajectoryPredictionService::onRealClick(bool player2, bool pressed) {
    auto* gb = GucciEngine::get();
    if (!gb->survivalIndicator) {
        return;
    }

    int playerIndex = player2 ? 1 : 0;
    if (gb->indicatorFlashEnabled) {
        m_context.indicatorFlashTimer[playerIndex] = kIndicatorFlashDuration;
    }

    if (pressed) {
        bool wasSurvivable = m_context.holdSurvivedFrames[playerIndex] >= gb->indicatorLookahead;
        gb->accuracyTotalClicks++;
        if (wasSurvivable) {
            gb->accuracyGoodClicks++;
            gb->currentStreak++;
            gb->bestStreak = std::max(gb->bestStreak, gb->currentStreak);
        } else {
            gb->currentStreak = 0;
        }
    }

    if (!gb->indicatorSoundEnabled || !ClickSoundManager::get()->enabled) {
        return;
    }

    int survived = pressed ? m_context.holdSurvivedFrames[playerIndex]
                           : m_context.releaseSurvivedFrames[playerIndex];
    float margin = static_cast<float>(survived - gb->indicatorLookahead);
    float tightness = std::clamp(
        1.0f - (margin / static_cast<float>(std::max(1, gb->indicatorLookahead))), 0.0f, 1.0f);
    float pitch = 1.0f + tightness * 0.35f;

    ClickSoundManager::get()->playClickPitched(pressed, player2, pitch);
}

void TrajectoryPredictionService::applyPortalHint(PlayerObject* player, int portalId) {
    if (!player || !kInteractivePortalIds.contains(portalId)) {
        return;
    }

    switch (portalId) {
    case 101:
        player->togglePlayerScale(true, true);
        player->updatePlayerScale();
        break;
    case 99:
        player->togglePlayerScale(false, true);
        player->updatePlayerScale();
        break;
    case 200:
        player->m_playerSpeed = 0.7f;
        break;
    case 201:
        player->m_playerSpeed = 0.9f;
        break;
    case 202:
        player->m_playerSpeed = 1.1f;
        break;
    case 203:
        player->m_playerSpeed = 1.3f;
        break;
    case 1334:
        player->m_playerSpeed = 1.6f;
        break;
    case 10:
        player->m_isUpsideDown = false;
        break;
    case 11:
        player->m_isUpsideDown = true;
        break;
    default:
        break;
    }
}
namespace {
    // The objects a move, rotate, scale or follow trigger can act on: anything
    // in a group, anything following, anything followed. Everything else
    // stays put, so leaving it out keeps the snapshot small.
    bool canBeMovedByTrigger(GameObject* object) {
        return object && (object->m_groupCount > 0 || object->m_followingSprite != nullptr ||
                          object->m_hasFollower);
    }
}

// Ported from Silicate's Trajectory (snapshotMovedObjects / stepMoveActions /
// restoreMovedObjects). GucciBot's port never took these, so every prediction
// ran against a frozen level. Two additions over Silicate, because this runs
// on every drawn frame and must leave the real run exactly as it found it:
// the variance table is saved and put back too, and so is the whole game
// state (the trace's callers only save four scalars of it).
void TrajectoryPredictionService::snapshotMovedObjects(PlayLayer* playLayer) {
    m_movedObjects.clear();
    m_movedSnapshotTaken = false;
    if (!playLayer || !playLayer->m_objects || !playLayer->m_effectManager) {
        return;
    }

    playLayer->m_effectManager->saveToState(m_savedEffectState);
    m_savedVariance = playLayer->m_varianceValues;

    for (unsigned int i = 0; i < playLayer->m_objects->count(); i++) {
        auto* object = static_cast<GameObject*>(playLayer->m_objects->objectAtIndex(i));
        if (!canBeMovedByTrigger(object)) {
            continue;
        }
        MovedObjectSnapshot snap;
        snap.object = object;
        snap.position = object->getPosition();
        snap.lastPosition = object->m_lastPosition;
        snap.positionX = object->m_positionX;
        snap.positionY = object->m_positionY;
        snap.positionXOffset = object->m_positionXOffset;
        snap.positionYOffset = object->m_positionYOffset;
        snap.rotationX = object->getRotationX();
        snap.rotationY = object->getRotationY();
        snap.rotationXOffset = object->m_rotationXOffset;
        snap.rotationYOffset = object->m_rotationYOffset;
        snap.scaleX = object->m_scaleX;
        snap.scaleY = object->m_scaleY;
        snap.scaleXOffset = object->m_scaleXOffset;
        snap.scaleYOffset = object->m_scaleYOffset;
        snap.isDirty = object->m_isDirty;
        m_movedObjects.push_back(snap);
    }
    m_movedSnapshotTaken = true;
}

void TrajectoryPredictionService::restoreMovedObjects(PlayLayer* playLayer) {
    if (!m_movedSnapshotTaken || !playLayer) {
        return;
    }
    for (auto const& snap : m_movedObjects) {
        auto* object = snap.object;
        if (!object) {
            continue;
        }
        object->m_positionX = snap.positionX;
        object->m_positionY = snap.positionY;
        object->m_positionXOffset = snap.positionXOffset;
        object->m_positionYOffset = snap.positionYOffset;
        object->m_rotationXOffset = snap.rotationXOffset;
        object->m_rotationYOffset = snap.rotationYOffset;
        object->m_scaleX = snap.scaleX;
        object->m_scaleY = snap.scaleY;
        object->m_scaleXOffset = snap.scaleXOffset;
        object->m_scaleYOffset = snap.scaleYOffset;

        object->setPosition(snap.position);
        object->setRotationX(snap.rotationX);
        object->setRotationY(snap.rotationY);
        object->m_lastPosition = snap.lastPosition;

        object->m_isDirty = snap.isDirty;
        object->setObjectRectDirty(true);
        object->setOrientedRectDirty(true);
        playLayer->updateObjectSection(object);
    }
    m_movedObjects.clear();

    if (playLayer->m_effectManager) {
        playLayer->m_effectManager->loadFromState(m_savedEffectState);
    }
    playLayer->m_varianceValues = m_savedVariance;
    m_movedSnapshotTaken = false;
}

// GD's own move step, the same three calls the game makes. `delta` is in the
// player-step unit; Silicate hands the move step one sixtieth of it, and so
// does this -- keeping its ratio rather than guessing at GD's units.
void TrajectoryPredictionService::stepMoveActions(PlayLayer* playLayer, float delta) {
    if (!playLayer || !playLayer->m_effectManager) {
        return;
    }
    playLayer->m_effectManager->prepareMoveActions(delta / 60.0f, false);
    playLayer->processMoveActionsStep(delta / 60.0f, true);
    playLayer->m_effectManager->postMoveActions();
}

void TrajectoryPredictionService::traceInputPath(PlayLayer* playLayer,
                                                 PlayerObject* previewPlayer,
                                                 PlayerObject* sourcePlayer,
                                                 bool holdingInput) {
    if (!playLayer || !previewPlayer || !sourcePlayer) {
        return;
    }

    auto state = capturePlayerState(sourcePlayer);
    applyPlayerState(previewPlayer, state);

    bool isSecondPlayer = playLayer->m_player2 == sourcePlayer;
    previewPlayer->m_isSecondPlayer = isSecondPlayer;
    previewPlayer->m_isPlatformer = sourcePlayer->m_isPlatformer;
    previewPlayer->m_playEffects = false;

    previewPlayer->m_touchedRings.clear();
    for (auto const& ringId : sourcePlayer->m_touchedRings) {
        previewPlayer->m_touchedRings.insert(ringId);
    }
    if (previewPlayer->m_touchingRings) {
        previewPlayer->m_touchingRings->removeAllObjects();
    }

    previewPlayer->m_potentialSlopeMap.clear();
    for (auto const& [key, value] : sourcePlayer->m_potentialSlopeMap) {
        previewPlayer->m_potentialSlopeMap.insert({key, value});
    }

    bool probing = m_context.probeFrames > 0;
    int frameCount = std::clamp(GucciEngine::get()->pathLength, 0, kMaxTraceFrames);
    if (probing) {
        // An agency probe only needs to see whether the two futures separate
        // at all, which happens on the first frame or not at all -- a few
        // frames of margin is enough and keeps it cheap.
        frameCount = std::clamp(m_context.probeFrames, 2, kMaxTraceFrames);
    } else if (!GucciEngine::get()->pathPreview && GucciEngine::get()->survivalIndicator) {
        frameCount = std::clamp(GucciEngine::get()->indicatorLookahead + 5, 5, kMaxTraceFrames);
    }
    m_context.traceCancelled = false;
    m_context.holdingTrace = holdingInput;
    m_context.touchingPads.clear();
    m_context.frameTouchingPads.clear();

    if (holdingInput) {
        previewPlayer->pushButton(static_cast<PlayerButton>(1));
    } else {
        previewPlayer->releaseButton(static_cast<PlayerButton>(1));
    }

    if (playLayer->m_levelSettings->m_platformerMode) {
        if (sourcePlayer->m_isGoingLeft) {
            previewPlayer->pushButton(static_cast<PlayerButton>(2));
        } else {
            previewPlayer->pushButton(static_cast<PlayerButton>(3));
        }
    }

    auto* drawNode = ensureDrawNode();
    int survivedFrames = frameCount;

    auto* gbe = GucciEngine::get();
    bool const moving = gbe->pathMovingObjects;
    int const moveInterval = std::max(1, gbe->pathMoveStepInterval);
    std::optional<GJGameState> savedGameState;
    if (moving) {
        savedGameState = playLayer->m_gameState;
        snapshotMovedObjects(playLayer);
    }

    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        // Level first, then the player -- the order Silicate steps them in.
        if (moving && frameIndex % moveInterval == 0) {
            stepMoveActions(playLayer, m_context.stepDelta * static_cast<float>(moveInterval));
        }
        CCPoint previousPosition = previewPlayer->getPosition();

        if (holdingInput) {
            if (isSecondPlayer) {
                m_context.holdPathP2[frameIndex] = previousPosition;
            } else {
                m_context.holdPathP1[frameIndex] = previousPosition;
            }
        }

        previewPlayer->m_collisionLogTop->removeAllObjects();
        previewPlayer->m_collisionLogBottom->removeAllObjects();
        previewPlayer->m_collisionLogLeft->removeAllObjects();
        previewPlayer->m_collisionLogRight->removeAllObjects();

        previewPlayer->update(m_context.stepDelta);
        previewPlayer->updateRotation(m_context.stepDelta);
        previewPlayer->updatePlayerScale();

        m_context.frameTouchingPads.clear();
        // checkCollisions returns 1 when that collision killed the player.
        // Silicate reads this directly (trajectory/trajectory.cpp:204). It is a
        // SECOND signal here, not the only one: TrajectoryPreviewPlayLayer::
        // destroyPlayer already records every fork death and stops it reaching
        // the real death handler. (Build -k's note claimed fork deaths went
        // unnoticed during ordinary play. That was wrong -- that hook was
        // missed. The check stays; it is guarded so a death is never counted
        // twice.)
        int const collisionResult =
            playLayer->checkCollisions(previewPlayer, m_context.stepDelta, false);
        m_context.touchingPads = m_context.frameTouchingPads;

        // Guarded so a death already reported through the destroyPlayer hook
        // (the Pathfinder probe path) is not counted a second time. No killer
        // object here -- the return value does not carry one -- so the killer
        // id stays whatever that path recorded.
        if (collisionResult == 1 && !m_context.traceCancelled)
            this->noteSimulatedDeath(previewPlayer);

        if (m_context.traceCancelled) {
            survivedFrames = frameIndex;
            if (GucciEngine::get()->pathPreview && !probing) {
                drawPredictionBounds(previewPlayer);
            }
            break;
        }

        cocos2d::ccColor4F lineColor =
            holdingInput ? (isSecondPlayer ? m_holdColorP2 : m_holdColor) : m_releaseColor;

        if (!holdingInput) {
            CCPoint heldAt = isSecondPlayer ? m_context.holdPathP2[frameIndex]
                                            : m_context.holdPathP1[frameIndex];
            bool overlapsHoldPath = (heldAt == previousPosition);
            if (overlapsHoldPath) {
                lineColor = isSecondPlayer ? m_overlapColorP2 : m_overlapColor;
            }
            // The same comparison the overlap colour has always made, kept as
            // a distance instead of a yes/no. This is the agency measurement.
            if (probing && frameIndex < m_context.holdSurvivedFrames[isSecondPlayer ? 1 : 0]) {
                float dx = previousPosition.x - heldAt.x;
                float dy = previousPosition.y - heldAt.y;
                float gap = std::sqrt(dx * dx + dy * dy);
                if (gap > m_context.probeMaxDivergence) {
                    m_context.probeMaxDivergence = gap;
                }
                m_context.probeComparedFrames = frameIndex + 1;
            }
        }

        if (frameIndex >= frameCount - 40 && frameCount > 0) {
            lineColor.a = static_cast<float>(frameCount - frameIndex) / 40.0f;
        }

        if (drawNode && GucciEngine::get()->pathPreview && !probing) {
            drawNode->drawSegment(previousPosition, previewPlayer->getPosition(), 0.6f, lineColor);
        }
    }

    // Every exit from the loop lands here, so the level always goes back.
    if (moving) {
        restoreMovedObjects(playLayer);
        if (savedGameState) {
            playLayer->m_gameState = *savedGameState;
        }
    }

    m_context.touchingPads.clear();
    m_context.frameTouchingPads.clear();
    m_context.holdingTrace = false;

    int playerIndex = isSecondPlayer ? 1 : 0;
    if (holdingInput) {
        m_context.holdSurvivedFrames[playerIndex] = survivedFrames;
    } else {
        m_context.releaseSurvivedFrames[playerIndex] = survivedFrames;
    }
}

// Silicate's extrapolation asks its trajectory for exactly one step ahead
// (simulate with m_maxLength = 0). This is the same thing on our fork.
//
// The fork starts from the source's PHYSICS position, m_position, not
// getPosition(). On an extrapolated frame the drawn position has been moved to
// an in-between point; predicting from there would compound frame on frame.
// Silicate made the same switch (its getPosition() line is commented out above
// the m_position one).
//
// Setup mirrors traceInputPath's rather than calling it, because that function
// also resets the context the path preview, the survival indicator and the
// agency probe all read from -- this must not disturb any of them.
bool TrajectoryPredictionService::predictStep(PlayLayer* playLayer,
                                              PlayerObject* source,
                                              bool holding,
                                              cocos2d::CCPoint& outPos,
                                              float& outRot) {
    if (!playLayer || !source || m_context.activeSimulation) {
        return false;
    }
    if (!m_context.previewPlayers[0]) {
        attach(playLayer);
    }
    bool const isSecondPlayer = playLayer->m_player2 == source;
    auto* preview = m_context.previewPlayers[isSecondPlayer ? 1 : 0];
    if (!preview) {
        return false;
    }

    unsigned int const savedProgress = playLayer->m_gameState.m_currentProgress;
    double const savedLevelTime = playLayer->m_gameState.m_levelTime;
    double const savedTotalTime = playLayer->m_gameState.m_totalTime;
    unsigned int const savedCommandIndex = playLayer->m_gameState.m_commandIndex;

    m_context.activeSimulation = true;

    applyPlayerState(preview, capturePlayerState(source));
    preview->setPosition(source->m_position);
    preview->m_isSecondPlayer = isSecondPlayer;
    preview->m_isPlatformer = source->m_isPlatformer;
    preview->m_playEffects = false;

    preview->m_touchedRings.clear();
    for (auto const& ringId : source->m_touchedRings) {
        preview->m_touchedRings.insert(ringId);
    }
    if (preview->m_touchingRings) {
        preview->m_touchingRings->removeAllObjects();
    }
    preview->m_potentialSlopeMap.clear();
    for (auto const& [key, value] : source->m_potentialSlopeMap) {
        preview->m_potentialSlopeMap.insert({key, value});
    }

    if (holding) {
        preview->pushButton(static_cast<PlayerButton>(1));
    } else {
        preview->releaseButton(static_cast<PlayerButton>(1));
    }
    if (playLayer->m_levelSettings->m_platformerMode) {
        preview->pushButton(static_cast<PlayerButton>(source->m_isGoingLeft ? 2 : 3));
    }

    // One tick in GD's player-physics units, which count 1.0 per 1/60 s: at
    // 240 TPS a tick is 0.25. PlayerObject::update and checkCollisions both
    // take it in those units -- Silicate steps every fork by
    // getPhysicsDt() * 60 (trajectory.cpp, m_delta), and updateCamera is
    // called with dt * 60 for the same reason. This used to pass 1/tps, a
    // sixtieth of a tick, so Frame Extrapolation predicted almost no movement.
    double const tps = GucciEngine::get()->updater.m_tps;
    float const dt = tps > 1.0 ? static_cast<float>(60.0 / tps) : m_context.stepDelta;

    preview->m_collisionLogTop->removeAllObjects();
    preview->m_collisionLogBottom->removeAllObjects();
    preview->m_collisionLogLeft->removeAllObjects();
    preview->m_collisionLogRight->removeAllObjects();
    preview->update(dt);
    preview->updateRotation(dt);
    preview->updatePlayerScale();
    playLayer->checkCollisions(preview, dt, false);

    outPos = preview->getPosition();
    outRot = preview->getRotation();

    playLayer->m_gameState.m_currentProgress = savedProgress;
    playLayer->m_gameState.m_levelTime = savedLevelTime;
    playLayer->m_gameState.m_totalTime = savedTotalTime;
    playLayer->m_gameState.m_commandIndex = savedCommandIndex;
    m_context.activeSimulation = false;
    return true;
}

// ---- sub-tick preview ----------------------------------------------------------
//
// anticroom's SubtickPreview asks his trajectory two things: where the player
// is part of the way through a tick (extrapolate), and what the hold and
// release paths look like if the input changes right there
// (extrapolateBranch). GucciBot's trajectory is its own design, so these are
// the same two questions answered on this fork -- same setup as predictStep,
// same step order as traceInputPath, none of the path preview's context
// (hold paths, survived frames, probe results) touched.

namespace {
    // The fork walks the real collision code, which advances these.
    struct ForkStateGuard {
        PlayLayer* pl;
        unsigned int progress;
        double levelTime;
        double totalTime;
        unsigned int commandIndex;
        explicit ForkStateGuard(PlayLayer* p)
            : pl(p),
              progress(p->m_gameState.m_currentProgress),
              levelTime(p->m_gameState.m_levelTime),
              totalTime(p->m_gameState.m_totalTime),
              commandIndex(p->m_gameState.m_commandIndex) {}
        ~ForkStateGuard() {
            pl->m_gameState.m_currentProgress = progress;
            pl->m_gameState.m_levelTime = levelTime;
            pl->m_gameState.m_totalTime = totalTime;
            pl->m_gameState.m_commandIndex = commandIndex;
        }
    };
} // namespace

float TrajectoryPredictionService::tickUnits() const {
    double const tps = GucciEngine::get()->updater.m_tps;
    return tps > 1.0 ? static_cast<float>(60.0 / tps) : m_context.stepDelta;
}

PlayerObject* TrajectoryPredictionService::prepareFork(PlayLayer* playLayer,
                                                      PlayerObject* source) {
    if (!m_context.previewPlayers[0]) {
        attach(playLayer);
    }
    bool const isSecondPlayer = playLayer->m_player2 == source;
    auto* fork = m_context.previewPlayers[isSecondPlayer ? 1 : 0];
    if (!fork) {
        return nullptr;
    }

    applyPlayerState(fork, capturePlayerState(source));
    fork->setPosition(source->m_position);
    fork->m_isSecondPlayer = isSecondPlayer;
    fork->m_isPlatformer = source->m_isPlatformer;
    fork->m_playEffects = false;

    fork->m_touchedRings.clear();
    for (auto const& ringId : source->m_touchedRings) {
        fork->m_touchedRings.insert(ringId);
    }
    if (fork->m_touchingRings) {
        fork->m_touchingRings->removeAllObjects();
    }
    fork->m_potentialSlopeMap.clear();
    for (auto const& [key, value] : source->m_potentialSlopeMap) {
        fork->m_potentialSlopeMap.insert({key, value});
    }

    // The buttons exactly as the real player has them. Pressing or releasing
    // to match (what predictStep does) would buffer a fresh jump the real
    // player never made.
    fork->m_holdingButtons = source->m_holdingButtons;
    fork->m_jumpBuffered = source->m_jumpBuffered;
    fork->m_stateJumpBuffered = source->m_stateJumpBuffered;
    fork->m_holdingLeft = source->m_holdingLeft;
    fork->m_holdingRight = source->m_holdingRight;
    return fork;
}

void TrajectoryPredictionService::stepFork(PlayLayer* playLayer, PlayerObject* fork, float delta) {
    fork->m_collisionLogTop->removeAllObjects();
    fork->m_collisionLogBottom->removeAllObjects();
    fork->m_collisionLogLeft->removeAllObjects();
    fork->m_collisionLogRight->removeAllObjects();
    fork->update(delta);
    fork->updateRotation(delta);
    fork->updatePlayerScale();
    // A fork death also arrives through the destroyPlayer hook; this is the
    // second signal, guarded the same way traceInputPath guards it.
    if (playLayer->checkCollisions(fork, delta, false) == 1 && !m_context.traceCancelled) {
        this->noteSimulatedDeath(fork);
    }
}

bool TrajectoryPredictionService::extrapolateSubtick(PlayLayer* playLayer,
                                                     PlayerObject* source,
                                                     float fraction,
                                                     SubtickPose& out) {
    if (!playLayer || !source || m_context.activeSimulation) {
        return false;
    }
    auto* fork = prepareFork(playLayer, source);
    if (!fork) {
        return false;
    }

    ForkStateGuard guard(playLayer);
    m_context.activeSimulation = true;
    m_context.traceCancelled = false;

    this->stepFork(playLayer, fork, this->tickUnits() * std::clamp(fraction, 0.f, 1.f));

    out.position = fork->getPosition();
    out.hitbox = fork->getObjectRect();
    out.innerHitbox = fork->getObjectRect(0.3f, 0.3f);
    out.rotation = fork->getRotation();
    out.died = m_context.traceCancelled;

    m_context.traceCancelled = false;
    m_context.activeSimulation = false;
    return true;
}

void TrajectoryPredictionService::traceSubtickBranch(PlayLayer* playLayer,
                                                     PlayerObject* source,
                                                     float fraction,
                                                     bool hold,
                                                     cocos2d::CCDrawNode* node,
                                                     cocos2d::ccColor4F color,
                                                     float width) {
    if (!playLayer || !source || !node || m_context.activeSimulation) {
        return;
    }
    auto* fork = prepareFork(playLayer, source);
    if (!fork) {
        return;
    }

    ForkStateGuard guard(playLayer);
    m_context.activeSimulation = true;
    m_context.traceCancelled = false;
    m_context.processedOrbs.clear();

    auto* gbe = GucciEngine::get();
    float const tick = this->tickUnits();
    fraction = std::clamp(fraction, 0.f, 1.f);
    bool const moving = gbe->pathMovingObjects;
    int const moveInterval = std::max(1, gbe->pathMoveStepInterval);
    std::optional<GJGameState> savedGameState;
    if (moving) {
        savedGameState = playLayer->m_gameState;
        snapshotMovedObjects(playLayer);
    }

    // The split tick: level first, then the player up to the split, then the
    // input, then the rest of the tick -- the CBF engine's own order.
    if (moving) {
        stepMoveActions(playLayer, tick);
    }
    this->stepFork(playLayer, fork, tick * fraction);
    CCPoint from = fork->getPosition();
    if (!m_context.traceCancelled) {
        if (hold) {
            fork->pushButton(static_cast<PlayerButton>(1));
        } else {
            fork->releaseButton(static_cast<PlayerButton>(1));
            fork->m_jumpBuffered = false;
        }
        this->stepFork(playLayer, fork, tick * (1.f - fraction));
        node->drawSegment(from, fork->getPosition(), width, color);
    }

    int const frames = std::clamp(gbe->pathLength, 0, kMaxTraceFrames);
    for (int i = 1; i < frames && !m_context.traceCancelled; ++i) {
        if (moving && i % moveInterval == 0) {
            stepMoveActions(playLayer, tick * static_cast<float>(moveInterval));
        }
        from = fork->getPosition();
        this->stepFork(playLayer, fork, tick);
        cocos2d::ccColor4F c = color;
        if (i >= frames - 40) {
            c.a *= static_cast<float>(frames - i) / 40.0f;
        }
        node->drawSegment(from, fork->getPosition(), width, c);
    }

    if (moving) {
        restoreMovedObjects(playLayer);
        if (savedGameState) {
            playLayer->m_gameState = *savedGameState;
        }
    }
    m_context.processedOrbs.clear();
    m_context.traceCancelled = false;
    m_context.activeSimulation = false;
}

int TrajectoryPredictionService::survivesFor(PlayLayer* playLayer,
                                             PlayerObject* source,
                                             int frames,
                                             int input) {
    if (!playLayer || !source || frames <= 0 || m_context.activeSimulation) {
        return -1;
    }
    auto* fork = prepareFork(playLayer, source);
    if (!fork) {
        return -1;
    }

    ForkStateGuard guard(playLayer);
    m_context.activeSimulation = true;
    m_context.traceCancelled = false;
    m_context.processedOrbs.clear();

    if (input > 0) {
        fork->pushButton(static_cast<PlayerButton>(1));
    } else if (input < 0) {
        fork->releaseButton(static_cast<PlayerButton>(1));
        fork->m_jumpBuffered = false;
    }

    auto* gbe = GucciEngine::get();
    float const tick = this->tickUnits();
    bool const moving = gbe->pathMovingObjects;
    int const moveInterval = std::max(1, gbe->pathMoveStepInterval);
    std::optional<GJGameState> savedGameState;
    if (moving) {
        savedGameState = playLayer->m_gameState;
        snapshotMovedObjects(playLayer);
    }

    int survived = 0;
    for (; survived < frames; ++survived) {
        if (moving && survived % moveInterval == 0) {
            stepMoveActions(playLayer, tick * static_cast<float>(moveInterval));
        }
        this->stepFork(playLayer, fork, tick);
        if (m_context.traceCancelled) {
            break;
        }
    }

    if (moving) {
        restoreMovedObjects(playLayer);
        if (savedGameState) {
            playLayer->m_gameState = *savedGameState;
        }
    }
    m_context.processedOrbs.clear();
    m_context.traceCancelled = false;
    m_context.activeSimulation = false;
    return survived;
}

int TrajectoryPredictionService::survivesScript(PlayLayer* playLayer,
                                                PlayerObject* source,
                                                int frames,
                                                std::vector<std::pair<int, bool>> const& events) {
    if (!playLayer || !source || frames <= 0 || m_context.activeSimulation) {
        return -1;
    }
    auto* fork = prepareFork(playLayer, source);
    if (!fork) {
        return -1;
    }

    ForkStateGuard guard(playLayer);
    m_context.activeSimulation = true;
    m_context.traceCancelled = false;
    m_context.processedOrbs.clear();

    auto* gbe = GucciEngine::get();
    float const tick = this->tickUnits();
    bool const moving = gbe->pathMovingObjects;
    int const moveInterval = std::max(1, gbe->pathMoveStepInterval);
    std::optional<GJGameState> savedGameState;
    if (moving) {
        savedGameState = playLayer->m_gameState;
        snapshotMovedObjects(playLayer);
    }

    size_t next = 0;
    int survived = 0;
    for (; survived < frames; ++survived) {
        while (next < events.size() && events[next].first <= survived) {
            if (events[next].second) {
                fork->pushButton(static_cast<PlayerButton>(1));
            } else {
                fork->releaseButton(static_cast<PlayerButton>(1));
            }
            next++;
        }
        if (moving && survived % moveInterval == 0) {
            stepMoveActions(playLayer, tick * static_cast<float>(moveInterval));
        }
        this->stepFork(playLayer, fork, tick);
        if (m_context.traceCancelled) {
            break;
        }
    }

    if (moving) {
        restoreMovedObjects(playLayer);
        if (savedGameState) {
            playLayer->m_gameState = *savedGameState;
        }
    }
    m_context.processedOrbs.clear();
    m_context.traceCancelled = false;
    m_context.activeSimulation = false;
    return survived;
}

void TrajectoryPredictionService::setOverlaySuppressed(bool suppressed) {
    if (m_overlaySuppressed == suppressed) {
        return;
    }
    m_overlaySuppressed = suppressed;
    if (!m_drawNode) {
        return;
    }
    if (suppressed) {
        m_drawNode->setVisible(false);
    } else {
        m_context.dirty = true;  // the next updatePreview redraws and shows it
    }
}

bool TrajectoryPredictionService::probeAgency(PlayLayer* playLayer,
                                              PlayerObject* source,
                                              AgencyResult& out,
                                              int frames) {
    out = AgencyResult{};
    if (!playLayer || !source || m_context.activeSimulation) {
        return false;
    }
    if (!m_context.previewPlayers[0]) {
        attach(playLayer);
    }

    bool isSecondPlayer = playLayer->m_player2 == source;
    auto* preview = m_context.previewPlayers[isSecondPlayer ? 1 : 0];
    if (!preview) {
        return false;
    }

    // Same save/restore discipline rebuildPreview uses -- the fork walks the
    // real collision code, which touches these.
    unsigned int savedProgress = playLayer->m_gameState.m_currentProgress;
    double savedLevelTime = playLayer->m_gameState.m_levelTime;
    double savedTotalTime = playLayer->m_gameState.m_totalTime;
    unsigned int savedCommandIndex = playLayer->m_gameState.m_commandIndex;

    // Step the fork at the engine's real physics step, not at whatever delta
    // was left over from the last PlayerObject::update. Outside a search those
    // are the same thing. Inside one they are not, and a fork stepped too far
    // ploughs straight into the nearest geometry on its first frame -- which
    // is exactly what it was doing: every probe reported both branches dying
    // at frame 0, so nothing was ever measured.
    float savedStep = m_context.stepDelta;
    double tps = GucciEngine::get()->updater.m_tps;
    if (tps > 1.0) {
        // GD's player-physics units (1.0 per 1/60 s), as in predictStep.
        // Was 1/tps -- a sixtieth of a tick. At that speed a branch could
        // almost never die inside the 6-frame probe, so "one branch survives
        // and the other doesn't" was a signal Pathfinder almost never got.
        m_context.stepDelta = (float)(60.0 / tps);
    }

    m_lastProbeStep = m_context.stepDelta;

    m_context.activeSimulation = true;
    m_context.processedOrbs.clear();
    m_context.probeFrames = std::clamp(frames, 2, 60);
    m_context.probeMaxDivergence = 0.0f;
    m_context.probeComparedFrames = 0;

    // Hold first, then release: the release pass compares itself against the
    // hold path recorded by the first, so the order is load-bearing.
    traceInputPath(playLayer, preview, source, true);
    m_context.processedOrbs.clear();
    traceInputPath(playLayer, preview, source, false);

    m_context.probeFrames = 0;
    m_context.stepDelta = savedStep;

    playLayer->m_gameState.m_currentProgress = savedProgress;
    playLayer->m_gameState.m_levelTime = savedLevelTime;
    playLayer->m_gameState.m_totalTime = savedTotalTime;
    playLayer->m_gameState.m_commandIndex = savedCommandIndex;
    m_context.activeSimulation = false;

    int index = isSecondPlayer ? 1 : 0;
    out.divergence = m_context.probeMaxDivergence;
    out.holdSurvived = m_context.holdSurvivedFrames[index];
    out.releaseSurvived = m_context.releaseSurvivedFrames[index];
    // Identical futures are bit-identical, not approximately equal -- the two
    // passes run the same code from the same state. Any real separation, or
    // one branch dying while the other doesn't, means the input mattered.
    out.matters = out.divergence > 0.0001f || out.holdSurvived != out.releaseSurvived;

    // The probe has overwritten the survived-frame slots and the hold path the
    // real preview relies on, so make it rebuild rather than draw stale data.
    auto* gb = GucciEngine::get();
    if (gb->pathPreview || gb->survivalIndicator) {
        m_context.dirty = true;
    }
    return true;
}

int TrajectoryPredictionService::getSurvivedFrames(bool player2, bool held) const {
    int playerIndex = player2 ? 1 : 0;
    return held ? m_context.holdSurvivedFrames[playerIndex]
                : m_context.releaseSurvivedFrames[playerIndex];
}

void TrajectoryPredictionService::rebuildPreview(PlayLayer* playLayer) {
    if (!playLayer || !m_context.previewPlayers[0]) {
        return;
    }

    auto* drawNode = ensureDrawNode();
    if (!drawNode) {
        return;
    }

    unsigned int savedProgress = playLayer->m_gameState.m_currentProgress;
    double savedLevelTime = playLayer->m_gameState.m_levelTime;
    double savedTotalTime = playLayer->m_gameState.m_totalTime;
    unsigned int savedCommandIndex = playLayer->m_gameState.m_commandIndex;

    m_context.activeSimulation = true;
    m_context.processedOrbs.clear();
    drawNode->clear();
    drawNode->setVisible(!m_overlaySuppressed);

    traceInputPath(playLayer, m_context.previewPlayers[0], playLayer->m_player1, true);
    traceInputPath(playLayer, m_context.previewPlayers[0], playLayer->m_player1, false);

    m_context.processedOrbs.clear();
    if (playLayer->m_gameState.m_isDualMode && playLayer->m_player2 &&
        m_context.previewPlayers[1]) {
        traceInputPath(playLayer, m_context.previewPlayers[1], playLayer->m_player2, true);
        traceInputPath(playLayer, m_context.previewPlayers[1], playLayer->m_player2, false);
    }

    if (GucciEngine::get()->survivalIndicator) {
        if (playLayer->m_player1) {
            drawSurvivalIndicator(playLayer->m_player1, false);
        }
        if (playLayer->m_gameState.m_isDualMode && playLayer->m_player2) {
            drawSurvivalIndicator(playLayer->m_player2, true);
        }
    }

    playLayer->m_gameState.m_currentProgress = savedProgress;
    playLayer->m_gameState.m_levelTime = savedLevelTime;
    playLayer->m_gameState.m_totalTime = savedTotalTime;
    playLayer->m_gameState.m_commandIndex = savedCommandIndex;

    m_context.activeSimulation = false;
    m_context.dirty = false;

    if (playLayer->m_player1) {
        m_context.watchKeys[0] = buildWatchKey(playLayer->m_player1);
    }
    if (playLayer->m_player2) {
        m_context.watchKeys[1] = buildWatchKey(playLayer->m_player2);
    }
}

void TrajectoryPredictionService::updatePreview(PlayLayer* playLayer) {
    if (!playLayer) {
        return;
    }

    auto* gb = GucciEngine::get();
    bool wantsPreview = gb->pathPreview || gb->survivalIndicator;
    bool wantsSimulation = wantsPreview || gb->pfAgencyDebug;
    if (!wantsSimulation) {
        m_context.dirty = true;
        clearOverlay();
        return;
    }

    float dt = CCDirector::sharedDirector()->getDeltaTime();
    for (int i = 0; i < 2; ++i) {
        if (m_context.indicatorFlashTimer[i] > 0.0f) {
            m_context.indicatorFlashTimer[i] =
                std::max(0.0f, m_context.indicatorFlashTimer[i] - dt);
        }
    }
    m_context.indicatorPulsePhase += dt;

    if (!m_context.previewPlayers[0]) {
        attach(playLayer);
    }

    if (m_context.activeSimulation) {
        return;
    }

    bool needsRebuild = m_context.dirty;
    if (!needsRebuild && playLayer->m_player1) {
        needsRebuild = watchChanged(buildWatchKey(playLayer->m_player1), m_context.watchKeys[0]);
    }
    if (!needsRebuild && playLayer->m_gameState.m_isDualMode && playLayer->m_player2) {
        needsRebuild = watchChanged(buildWatchKey(playLayer->m_player2), m_context.watchKeys[1]);
    }

    if (needsRebuild && wantsPreview) {
        rebuildPreview(playLayer);
    }

    if (gb->pfAgencyDebug && playLayer->m_player1) {
        AgencyResult agency;
        if (probeAgency(playLayer, playLayer->m_player1, agency, kAgencyProbeFrames)) {
            gb->pfAgencyMatters = agency.matters;
            gb->pfAgencyDivergence = agency.divergence;
            gb->pfAgencyHoldSurvived = agency.holdSurvived;
            gb->pfAgencyReleaseSurvived = agency.releaseSurvived;
            gb->pfAgencyValid = true;
        }
    } else {
        gb->pfAgencyValid = false;
    }
}
void TrajectoryPredictionService::simulateCollisionBatch(GJBaseGameLayer* layer,
                                                         PlayerObject* player,
                                                         gd::vector<GameObject*>* objects,
                                                         int objectCount,
                                                         float dt) {
    if (!layer || !player || !objects) {
        return;
    }

    std::vector<EffectGameObject*> padObjects;
    std::vector<RingObject*> orbObjects;
    gd::vector<GameObject*> filteredObjects;
    filteredObjects.reserve(objectCount);

    for (int index = 0; index < objectCount; ++index) {
        GameObject* object = (*objects)[index];
        if (!object) {
            continue;
        }
        // GD's hidden anti-cheat spike is not level geometry, and every death
        // check elsewhere in this codebase already ignores it -- noclip,
        // auto-retry, prevent-death, the search's own death capture. The fork
        // was the one path that didn't. During a Pathfinder search the spike
        // can sit on the player; the real run ignores it, but the fork copies
        // the player's position and died on it at frame 0 of every probe,
        // so nothing was ever measured.
        if (object == layer->m_anticheatSpike) {
            continue;
        }

        auto type = object->m_objectType;
        if (type == GameObjectType::Solid || type == GameObjectType::Hazard ||
            type == GameObjectType::AnimatedHazard || type == GameObjectType::Slope) {
            filteredObjects.push_back(object);
            continue;
        }

        if (kInteractivePortalIds.contains(object->m_objectID)) {
            filteredObjects.push_back(object);
        } else if (isSimulatedPad(type)) {
            padObjects.push_back(static_cast<EffectGameObject*>(object));
        } else if (isSimulatedOrb(type)) {
            orbObjects.push_back(static_cast<RingObject*>(object));
        }
    }

    layer->GJBaseGameLayer::collisionCheckObjects(
        player, &filteredObjects, static_cast<int>(filteredObjects.size()), dt);

    CCRect playerRect = player->getObjectRect();
    for (auto* object : filteredObjects) {
        if (!object || !kInteractivePortalIds.contains(object->m_objectID)) {
            continue;
        }

        if (!playerRect.intersectsRect(object->getObjectRect())) {
            continue;
        }

        applyPortalHint(player, object->m_objectID);
    }

    playerRect = player->getObjectRect();
    for (auto* pad : padObjects) {
        if (!pad || !playerRect.intersectsRect(pad->getObjectRect())) {
            continue;
        }

        m_context.frameTouchingPads.insert(pad);
        if (m_context.touchingPads.contains(pad)) {
            continue;
        }

        auto snapshot = captureActivation(pad);
        bool savedNoEffects = pad->m_hasNoEffects;
        pad->m_hasNoEffects = true;
        if (pad->m_objectType == GameObjectType::GravityPad) {
            layer->GJBaseGameLayer::gravBumpPlayer(player, pad);
        } else {
            layer->GJBaseGameLayer::bumpPlayer(player, pad);
        }
        pad->m_hasNoEffects = savedNoEffects;
        restoreActivation(pad, snapshot);
    }

    if (!m_context.holdingTrace) {
        return;
    }

    playerRect = player->getObjectRect();
    for (auto* orb : orbObjects) {
        if (!orb || m_context.processedOrbs.contains(orb)) {
            continue;
        }

        if (!playerRect.intersectsRect(orb->getObjectRect())) {
            continue;
        }

        m_context.processedOrbs.insert(orb);
        auto snapshot = captureActivation(orb);
        bool savedNoEffects = orb->m_hasNoEffects;
        orb->m_hasNoEffects = true;
        m_context.processingOrbTouch = true;
        layer->GJBaseGameLayer::playerTouchedRing(player, orb);
        m_context.processingOrbTouch = false;
        orb->m_hasNoEffects = savedNoEffects;
        restoreActivation(orb, snapshot);
    }
}

bool TrajectoryPredictionService::handleActivationCheck(PlayerObject* player,
                                                        EffectGameObject* object) {
    if (!player || !object) {
        return false;
    }

    if (kInteractivePortalIds.contains(object->m_objectID)) {
        applyPortalHint(player, object->m_objectID);
        return false;
    }

    return isSimulatedPad(object->m_objectType);
}

void TrajectoryPredictionService::handleTouchedTrigger(PlayerObject* player,
                                                       EffectGameObject* object) {
    if (!player || !object) {
        return;
    }

    if (kInteractivePortalIds.contains(object->m_objectID)) {
        applyPortalHint(player, object->m_objectID);
    }
}

class $modify(TrajectoryPreviewPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        TrajectoryPredictionService::get().updatePreview(this);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto& service = TrajectoryPredictionService::get();
        service.attach(this);
        service.markDirty();

        auto* gb = GucciEngine::get();
        gb->accuracyGoodClicks = 0;
        gb->accuracyTotalClicks = 0;
        gb->currentStreak = 0;
        gb->bestStreak = 0;
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        auto& service = TrajectoryPredictionService::get();
        if (service.isActiveSimulation() || service.ownsPreviewPlayer(player)) {
            service.noteSimulatedDeath(player, gameObject);
            return;
        }

        GucciEngine::get()->currentStreak = 0;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void onQuit() {
        auto& service = TrajectoryPredictionService::get();
        service.clearOverlay();
        service.detach();
        PlayLayer::onQuit();
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (TrajectoryPredictionService::get().isActiveSimulation()) {
            return;
        }

        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(TrajectoryPreviewPauseLayer, PauseLayer) {
    void goEdit() {
        auto& service = TrajectoryPredictionService::get();
        service.clearOverlay();
        service.detach();
        PauseLayer::goEdit();
    }
};

class $modify(TrajectoryPreviewBaseLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player,
                               gd::vector<GameObject*>* objects,
                               int objectCount,
                               float dt) {
        auto& service = TrajectoryPredictionService::get();
        if (!service.isActiveSimulation()) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        service.simulateCollisionBatch(this, player, objects, objectCount, dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        auto& service = TrajectoryPredictionService::get();
        if (!service.isActiveSimulation()) {
            return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
        }

        return service.handleActivationCheck(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        auto& service = TrajectoryPredictionService::get();
        if (service.isActiveSimulation() && !service.isProcessingOrbTouch()) {
            return;
        }

        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        auto& service = TrajectoryPredictionService::get();
        if (!service.isActiveSimulation()) {
            GJBaseGameLayer::playerTouchedTrigger(player, object);
            return;
        }

        service.handleTouchedTrigger(player, object);
    }
    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            GJBaseGameLayer::activateSFXTrigger(object);
        }
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            GJBaseGameLayer::activateSongEditTrigger(object);
        }
    }

    void gameEventTriggered(GJGameEvent event, int value1, int value2) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            GJBaseGameLayer::gameEventTriggered(event, value1, value2);
        }
    }
};

class $modify(TrajectoryPreviewPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);
        TrajectoryPredictionService::get().captureFrameDelta(dt);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        auto& service = TrajectoryPredictionService::get();
        if (!service.isActiveSimulation()) {
            // A spider teleport skips the frames in between, so the trail
            // buffer records the dash as a span rather than two far-apart
            // points.
            if (!service.ownsPreviewPlayer(this))
                ::Bot::get()->trailBuffer().saveSpiderDash(this, from, to);
            PlayerObject::playSpiderDashEffect(from, to);
        }
    }

    void incrementJumps() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            PlayerObject::incrementJumps();
        }
    }

    void playBumpEffect(int objectType, GameObject* player) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            PlayerObject::playBumpEffect(objectType, player);
        }
    }

    void spawnCircle() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            PlayerObject::spawnCircle();
        }
    }

    void spawnDualCircle() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            PlayerObject::spawnDualCircle();
        }
    }

    void addAllParticles() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            PlayerObject::addAllParticles();
        }
    }
};

class $modify(TrajectoryPreviewHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            HardStreak::addPoint(point);
        }
    }
};

class $modify(TrajectoryPreviewGameObject, GameObject) {
    void playShineEffect() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            GameObject::playShineEffect();
        }
    }

    void activateObject() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            GameObject::activateObject();
        }
    }
};

class $modify(TrajectoryPreviewEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int unk, const gd::vector<int>* groups) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            EffectGameObject::triggerObject(layer, unk, groups);
        }
    }

    void triggerActivated(float value) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            EffectGameObject::triggerActivated(value);
        }
    }

    void playTriggerEffect() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            EffectGameObject::playTriggerEffect();
        }
    }
};
class $modify(TrajectoryPreviewRingObject, RingObject) {
    void spawnCircle() {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            RingObject::spawnCircle();
        }
    }

    void powerOnObject(int state) {
        if (!TrajectoryPredictionService::get().isActiveSimulation()) {
            RingObject::powerOnObject(state);
        }
    }
};

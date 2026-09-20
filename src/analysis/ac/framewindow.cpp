#include "framewindow.hpp"

#include "cbf.hpp"

#include <limits>

#include <Geode/Geode.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <matjson.hpp>


using namespace geode::prelude;

using Clock = std::chrono::steady_clock;

static constexpr uint32_t MAX_STEPS_PER_LEG = 200000;

static Clock::time_point g_deadline;

// Last frame seen by the nominal-leg trace, to spot a leg stepping out of
// step with the capture. Diagnostic only.
static uint32_t g_traceLastFrame = UINT32_MAX;

static std::ofstream g_fwLog;

void fwFileLog(std::string const& line) {
    if (!g_fwLog.is_open()) {
        auto path = geode::Mod::get()->getSaveDir() / "guccibot_fw.log";
        // Appends rather than truncates: the log used to be wiped on every
        // launch, so testing and then relaunching destroyed the run we were
        // about to read -- which happened, and cost a round trip. Each session
        // gets a banner instead so runs stay separable.
        bool const fresh = !std::filesystem::exists(path);
        g_fwLog.open(path, std::ios::out | std::ios::app);
        if (!fresh && g_fwLog.is_open())
            g_fwLog << "\n===== new session =====\n";
        geode::log::info("[fw] log file at: {}", path.string());
    }
    if (g_fwLog.is_open()) {
        g_fwLog << line << "\n";
        g_fwLog.flush();
    }
}

void gucci::fwEngineLog(std::string const& line) {
    fwFileLog(line);
}

// Mirrored to a file as well as the console. Geode's console log is not
// persisted on this machine, so console-only output means every question about
// what a run actually did gets answered by guessing at the code instead of
// reading what happened. Written to guccibot_fw.log in the mod's save
// directory, truncated each launch, and only while Verbose Log is on.
void fwFileLog(std::string const& line);

#define FWWARN(...)                                  do {                                                    auto const fww_ = fmt::format(__VA_ARGS__);         geode::log::warn("{}", fww_);                       fwFileLog("WARN " + fww_);                      } while (0)

#define FWLOG(...)                                           do {                                                         if (m_verbose->inner()) {                                    auto const fwl_ = fmt::format(__VA_ARGS__);              geode::log::info("{}", fwl_);                            fwFileLog(fwl_);                                     }                                                    } while (0)


cocos2d::ccColor3B FrameWindowAnalyzer::colorForWindow(int window) {
    if (window <= 0) return {130, 130, 130};

    switch (window) {
        case 1:
            return {255, 60, 60};
        case 2:
            return {255, 140, 40};
        case 3:
            return {255, 205, 50};
        case 4:
            return {195, 230, 60};
        case 5:
        case 6:
            return {120, 225, 85};
        default:
            return {70, 215, 140};
    }
}

float FrameWindowAnalyzer::progress() const {
    if (m_total == 0) return 0.0f;
    return static_cast<float>(m_index) / static_cast<float>(m_total);
}

static void logPlayerState(char const* tag, PlayerObject* p) {
    if (!p) return;
    auto it = p->m_holdingButtons.find(1);
    int const held =
        it == p->m_holdingButtons.end() ? -1 : (it->second ? 1 : 0);
    geode::log::info(
        "[fw][state] {} pos=({:.2f},{:.2f}) yvel={:.4f} held={} up={} grav={:.3f} "
        "ground={} g2={} g3={} g4={} dart={} side={} locked={} vehicle={:.2f} "
        "speed={:.3f}",
        tag, p->getPositionX(), p->getPositionY(), p->m_yVelocity, held,
        p->m_isUpsideDown, p->m_gravityMod, p->m_isOnGround, p->m_isOnGround2,
        p->m_isOnGround3, p->m_isOnGround4, p->m_isDart, p->m_isSideways,
        p->m_inputsLocked, p->m_vehicleSize, p->m_playerSpeed);
}

static char gamemodeOf(PlayerObject* p) {
    if (!p) return '?';
    if (p->m_isRobot) return 'R';
    if (p->m_isSpider) return 'S';
    if (p->m_isSwing) return 'W';
    if (p->m_isDart) return 'V';
    if (p->m_isBird) return 'U';
    if (p->m_isBall) return 'B';
    if (p->m_isShip) return 'H';
    return 'C';
}

bool FrameWindowAnalyzer::isSetupMode(char gamemode) {
    return gamemode == 'H' || gamemode == 'W';
}

bool FrameWindowAnalyzer::isHoldMode(char gamemode) {
    return gamemode == 'U' || gamemode == 'V' || gamemode == 'R';
}

bool FrameWindowAnalyzer::setupVarying(FrameWindowMark const& mk) const {
    if (isSetupMode(mk.gamemode)) return true;
    return m_setupHoldModes->inner() && isHoldMode(mk.gamemode);
}

static bool namedColor(char code, cocos2d::ccColor3B& out) {
    switch (code) {
        case 'r': out = {255, 0, 0}; return true;
        case 'g': out = {0, 255, 0}; return true;
        case 'b': out = {0, 0, 255}; return true;
        case 'y': out = {255, 255, 0}; return true;
        case 'o': out = {255, 128, 0}; return true;
        case 'p': out = {255, 0, 255}; return true;
        case 'l': out = {96, 171, 239}; return true;
        case 'a': out = {150, 50, 255}; return true;
        case 'd': out = {255, 150, 255}; return true;
        case 'j': out = {50, 255, 192}; return true;
        case 's': out = {255, 220, 65}; return true;
        case 'c': out = {0, 255, 255}; return true;
        default: return false;
    }
}

std::vector<std::pair<std::string, cocos2d::ccColor3B>>
FrameWindowAnalyzer::parseColored(std::string const& text) {
    std::vector<std::pair<std::string, cocos2d::ccColor3B>> runs;
    std::vector<cocos2d::ccColor3B> stack{{255, 255, 255}};
    std::string cur;

    auto flush = [&]() {
        if (!cur.empty()) {
            runs.emplace_back(cur, stack.back());
            cur.clear();
        }
    };

    for (size_t i = 0; i < text.size();) {
        if (text[i] != '<') {
            cur += text[i++];
            continue;
        }
        size_t const close = text.find('>', i);
        if (close == std::string::npos) {
            cur += text[i++];
            continue;
        }
        std::string const tag = text.substr(i + 1, close - i - 1);

        if (tag == "/c") {
            flush();
            if (stack.size() > 1) stack.pop_back();
            i = close + 1;
            continue;
        }
        if (tag.size() >= 2 && tag[0] == 'c') {
            std::string body = tag.substr(1);
            if (!body.empty() && body[0] == '#') body.erase(body.begin());

            cocos2d::ccColor3B col{255, 255, 255};
            bool ok = false;
            if (body.size() == 1) {
                ok = namedColor(body[0], col);
            } else if (body.size() == 6 &&
                       body.find_first_not_of("0123456789abcdefABCDEF") ==
                           std::string::npos) {
                unsigned const v =
                    static_cast<unsigned>(std::stoul(body, nullptr, 16));
                col = {static_cast<GLubyte>((v >> 16) & 0xFF),
                       static_cast<GLubyte>((v >> 8) & 0xFF),
                       static_cast<GLubyte>(v & 0xFF)};
                ok = true;
            }
            if (ok) {
                flush();
                stack.push_back(col);
                i = close + 1;
                continue;
            }
        }
        cur += text[i++];
    }

    flush();
    return runs;
}

FrameWindowTier const* FrameWindowAnalyzer::visibleTierFor(
    FrameWindowMark const& mk) const {
    auto const* tier = this->tierFor(this->displayWindow(mk));
    return tier && tier->showInHud ? tier : nullptr;
}

FrameWindowTier const* FrameWindowAnalyzer::tierForFrames(float frames) const {
    if (!(frames > 0.f)) return nullptr;
    return this->tierFor(static_cast<int>(std::floor(frames)));
}

void FrameWindowAnalyzer::splitSlots(int64_t slots, int& tickShift,
                                     double& fraction) const {
    int64_t const per = std::max<int64_t>(1, m_fine);
    if (per <= 1) {
        tickShift = slots;
        fraction = 0.0;
        return;
    }

    tickShift = static_cast<int>(
        (slots >= 0 ? slots : slots - (per - 1)) / per);
    fraction = static_cast<double>(slots - tickShift * per) /
               static_cast<double>(per);
}

static bool isDashOrb(GameObjectType type) {
    return type == GameObjectType::DashRing ||
           type == GameObjectType::GravityDashRing;
}

static bool isNonDashOrb(GameObjectType type) {
    switch (type) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::CustomRing:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

static void classifyOrbs(PlayerObject* player, bool& outDash, bool& outNonDash) {
    outDash = false;
    outNonDash = false;
    if (!player || !player->m_touchingRings) return;
    for (auto* obj : CCArrayExt<GameObject*>(player->m_touchingRings)) {
        if (!obj) continue;
        if (isDashOrb(obj->m_objectType))
            outDash = true;
        else if (isNonDashOrb(obj->m_objectType))
            outNonDash = true;
    }
}

static bool isMeasurableInput(slc::Action const& action) {
    using Type = slc::ActionType;
    switch (action.m_type) {
        case Type::Jump:
        case Type::Left:
        case Type::Right:
            return true;
        default:
            return false;
    }
}

FrameWindowAnalyzer::StepResult FrameWindowAnalyzer::stepToward(PlayLayer* pl,
                                                               uint32_t until) {
    auto& updater = Bot::get()->updater();

    if (m_stageId == Stage::Probe && updater.getFrame() > until)
        FWWARN(
            "[fw] leg target {} is behind the current frame {} -- the shift "
            "was judged on a short run",
            until, updater.getFrame());

    uint32_t guard = 0;
    m_trackingPath = m_stageId == Stage::Capture ||
                     (!m_pathDiverged && m_phase == Phase::Nominal &&
                      m_shift == 0);
    while (updater.getFrame() < until) {
        if (m_probeDied || pl->m_playerDied) {
            if (m_noclip) {
                if (m_stageId == Stage::Capture)
                    this->noteCaptureDeath(updater.getFrame());
                m_probeDied = false;
                pl->m_playerDied = false;
            } else {
                if (!m_probeDied)
                    FWLOG(
                        "[fw][death] m_playerDied set without a destroyPlayer "
                        "call (stage={} frame={})",
                        m_stage, updater.getFrame());
                return StepResult::Died;
            }
        }
        if (++guard > MAX_STEPS_PER_LEG) return StepResult::Reached;

        bool const logHold = m_stageId == Stage::Probe && guard <= 2;
        int holdBefore = -1;
        if (logHold && pl->m_player1) {
            auto it = pl->m_player1->m_holdingButtons.find(1);
            holdBefore =
                it == pl->m_player1->m_holdingButtons.end() ? -1
                                                            : (it->second ? 1 : 0);
        }

        int batch = 1;
        if (m_stageId != Stage::Capture && !m_trackingPath) {
            batch = std::clamp(m_stepBatch->inner(), 1, 64);
            batch = static_cast<int>(
                std::min<uint32_t>(batch, until - updater.getFrame()));

            if (auto const& next =
                    Bot::get()->replaySystem().getCurrentQueuedInput();
                next.has_value()) {
                int64_t const room = static_cast<int64_t>(next->m_frame) -
                                     static_cast<int64_t>(updater.getFrame());
                if (room > 0)
                    batch = static_cast<int>(
                        std::min<int64_t>(batch, room));
            }

            batch = std::max(1, batch);
        }

        auto const stepStart = Clock::now();
        updater.stepOnce();
        if (batch > 1) {
            updater.m_analysisBatch = batch;
            cocos2d::CCScheduler::get()->update(updater.getPhysicsDt() * batch);
            updater.m_analysisBatch = 0;
        } else {
            cocos2d::CCScheduler::get()->update(updater.getPhysicsDt());
        }
        m_stepNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                           Clock::now() - stepStart)
                           .count();
        m_stepCount += batch;
        guard += batch - 1;

        cbf::Engine::get()->flushOrphaned();

        if (pl->m_player1) {
            uint32_t const cf = updater.getFrame();
            if (m_stageId == Stage::Capture) {
                if (m_capturePath.size() <= cf)
                    m_capturePath.resize(cf + 1,
                                         cocos2d::CCPoint(-1e9f, -1e9f));
                m_capturePath[cf] = pl->m_player1->getPosition();
                this->checkCaptureAgainstTrail(pl, cf);
            } else if (m_phase == Phase::Nominal && m_shift == 0 &&
                       cf < m_capturePath.size() &&
                       m_capturePath[cf].x > -1e8f) {
                auto const np = pl->m_player1->getPosition();
                float const dx = np.x - m_capturePath[cf].x;
                float const dy = np.y - m_capturePath[cf].y;

                // Full trace of a nominal leg against the captured path, not
                // just the first frame that differs. A nominal leg replays the
                // macro at its own timing, so it is supposed to retrace the
                // capture exactly -- when it does not, the shape of the drift
                // says what kind of fault it is. A single step's worth
                // appearing at once means a lost or doubled physics step;
                // drift that grows every frame means a state difference.
                //
                // g_traceLastFrame catches the other half: if the frame
                // counter advances by anything other than 1 between steps, the
                // leg ran a different number of steps than the capture did,
                // which is invisible in position alone.
                int const advanced =
                    g_traceLastFrame == UINT32_MAX
                        ? 1
                        : static_cast<int>(cf) -
                              static_cast<int>(g_traceLastFrame);
                g_traceLastFrame = cf;

                FWLOG(
                    "[fw][trace] click {} frame={} adv={} leg=({:.3f},{:.3f}) "
                    "capture=({:.3f},{:.3f}) d=({:.4f},{:.4f}) yvel={:.4f} "
                    "ground={} hold={}",
                    m_index + 1, cf, advanced, np.x, np.y, m_capturePath[cf].x,
                    m_capturePath[cf].y, dx, dy, pl->m_player1->m_yVelocity,
                    pl->m_player1->m_isOnGround ? 1 : 0,
                    [&] {
                        auto it = pl->m_player1->m_holdingButtons.find(1);
                        return it == pl->m_player1->m_holdingButtons.end()
                                   ? -1
                                   : (it->second ? 1 : 0);
                    }());

                if (advanced != 1)
                    FWWARN(
                        "[fw][trace] click {}: frame jumped {} -> {} ({} "
                        "frames) inside a leg -- the leg is not stepping in "
                        "step with the capture",
                        m_index + 1, cf - advanced, cf, advanced);

                if (!m_pathDiverged && dx * dx + dy * dy > 0.0025f) {
                    m_pathDiverged = true;
                    FWWARN(
                        "[fw][diverge] click {} first differs at frame {}: "
                        "leg=({:.2f},{:.2f}) capture=({:.2f},{:.2f}) "
                        "d=({:.3f},{:.3f})",
                        m_index + 1, cf, np.x, np.y, m_capturePath[cf].x,
                        m_capturePath[cf].y, dx, dy);
                }
            }
        }

        if (logHold && pl->m_player1) {
            auto it = pl->m_player1->m_holdingButtons.find(1);
            int const holdAfter =
                it == pl->m_player1->m_holdingButtons.end() ? -1
                                                            : (it->second ? 1 : 0);
            FWLOG("[fw][hold] frame={} step={} hold {} -> {} yvel={:.3f}",
                  updater.getFrame(), guard, holdBefore, holdAfter,
                  pl->m_player1->m_yVelocity);
            logPlayerState("after-step", pl->m_player1);
        }

        if (m_stageId == Stage::Capture) {
            uint32_t const now = updater.getFrame();
            while (m_index < m_samples.size() &&
                   m_samples[m_index].frame <= now) {
                auto& s = m_samples[m_index];
                auto* sp = s.player2 ? pl->m_player2 : pl->m_player1;
                if (sp) {
                    s.position = sp->getPosition();
                    s.gamemode = gamemodeOf(sp);
                    if (!s.release) classifyOrbs(sp, s.orbDash, s.orbNonDash);
                    FWLOG(
                        "[fw][capture] sample {}/{} frame={} now={} p{} {} "
                        "gm={} pos=({:.1f},{:.1f}) orbD={} orbN={}",
                        m_index + 1, m_samples.size(), s.frame, now,
                        s.player2 ? 2 : 1, s.release ? "release" : "press",
                        s.gamemode, s.position.x, s.position.y, s.orbDash,
                        s.orbNonDash);
                } else {
                    FWLOG(
                        "[fw][capture] sample {}/{} frame={} NO PLAYER OBJECT "
                        "(p{})",
                        m_index + 1, m_samples.size(), s.frame,
                        s.player2 ? 2 : 1);
                }
                m_index++;
            }

            if (now - m_captureHeartbeat >= 500) {
                m_captureHeartbeat = now;
                FWLOG("[fw][capture] frame={} captured={}/{} dead={}", now,
                      m_index, m_samples.size(), pl->m_playerDied);
            }
        }

        if ((guard & 0x1F) == 0 && Clock::now() >= g_deadline) {
            return StepResult::OutOfBudget;
        }
    }

    if (m_noclip) {
        m_probeDied = false;
        pl->m_playerDied = false;
        return StepResult::Reached;
    }
    return (m_probeDied || pl->m_playerDied) ? StepResult::Died
                                             : StepResult::Reached;
}

bool FrameWindowAnalyzer::onSuppressedDeath(cocos2d::CCNode* player,
                                            cocos2d::CCNode* killer) {
    if (!m_running) return false;

    if (!m_probeDied) {
        auto& updater = Bot::get()->updater();
        auto* obj = typeinfo_cast<GameObject*>(killer);
        auto* p = typeinfo_cast<PlayerObject*>(player);

        char const* source = "arg";
        if (!obj && p && p->m_collidedObject) {
            obj = p->m_collidedObject;
            source = "collided";
        }

        m_lastKillerId = obj ? obj->m_objectID : -1;
        m_lastKillerPos = obj ? obj->getPosition()
                         : killer ? killer->getPosition()
                                  : cocos2d::CCPoint{};

        FWLOG(
            "[fw][death] stage={} frame={} killer=id:{} type:{} pos=({:.1f},"
            "{:.1f}) via={} sides=[T{} B{} L{} R{}] slope={} player=({:.1f},"
            "{:.1f}) gm={} yvel={:.3f} ground={}",
            m_stage, updater.getFrame(), m_lastKillerId,
            obj ? static_cast<int>(obj->m_objectType) : -1, m_lastKillerPos.x,
            m_lastKillerPos.y, source, p ? p->m_lastCollisionTop : -1,
            p ? p->m_lastCollisionBottom : -1, p ? p->m_lastCollisionLeft : -1,
            p ? p->m_lastCollisionRight : -1, p ? p->m_isOnSlope : false,
            p ? p->getPositionX() : -1.f, p ? p->getPositionY() : -1.f,
            p ? gamemodeOf(p) : '?', p ? p->m_yVelocity : 0.0,
            p ? p->m_isOnGround : false);
    }

    m_probeDied = true;
    return true;
}

void FrameWindowAnalyzer::notePress(uint32_t frame, bool player2,
                                    bool splittable, bool onGround,
                                    double yVelocity) {
    if (!m_running || !m_testActive) return;
    if (frame != m_legPressFrame) return;
    if (m_index >= m_samples.size()) return;
    if (player2 != m_samples[m_index].player2) return;

    m_legPressSeen = true;
    m_legPressBuffered = !splittable;
    m_legPressOnGround = onGround;
    m_legPressYVel = yVelocity;
}

void FrameWindowAnalyzer::releaseCheckpoint() {
    if (m_cpObject) {
        m_cpObject->release();
        m_cpObject = nullptr;
    }
    m_haveCheckpoint = false;
}

bool FrameWindowAnalyzer::collectSamples() {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;

    m_samples.clear();
    m_originalFrames.clear();
    m_originalFrames.reserve(actions.size());
    for (auto const& a : actions)
        m_originalFrames.push_back(static_cast<uint32_t>(a.m_frame));

    for (size_t i = 0; i < actions.size(); i++) {
        if (!isMeasurableInput(actions[i])) continue;
        Sample s;
        s.frame = static_cast<uint32_t>(actions[i].m_frame);
        s.player2 = actions[i].m_player2;
        s.release = !actions[i].m_holding;
        s.type = actions[i].m_type;
        s.actionIndex = i;
        m_samples.push_back(s);
    }

    std::stable_sort(m_samples.begin(), m_samples.end(),
                     [](Sample const& a, Sample const& b) {
                         return a.frame < b.frame;
                     });

    return !m_samples.empty();
}

void FrameWindowAnalyzer::foldSwiftClicks() {
    std::vector<Sample> kept;
    kept.reserve(m_samples.size());

    int folded = 0;
    for (size_t i = 0; i < m_samples.size(); i++) {
        auto const& s = m_samples[i];
        if (s.release) {
            bool sameFramePress = false;
            for (size_t j = i; j-- > 0;) {
                if (m_samples[j].frame != s.frame) break;
                if (!m_samples[j].release &&
                    m_samples[j].player2 == s.player2 &&
                    m_samples[j].type == s.type) {
                    sameFramePress = true;
                    break;
                }
            }
            if (sameFramePress) {
                folded++;
                continue;
            }
        }
        kept.push_back(s);
    }

    FWLOG("[fw][samples] swift-click fold: {} -> {} ({} folded)",
          m_samples.size(), kept.size(), folded);
    m_filtered += folded;

    m_samples.swap(kept);
}

void FrameWindowAnalyzer::applyOrbAwareSkip() {
    bool const testShip = m_testShipReleases->inner();
    bool const allReleases = m_testAllReleases->inner();
    bool const orbSkip = m_orbAwareReleaseSkip->inner();

    std::vector<Sample> kept;
    kept.reserve(m_samples.size());

    bool lastOrbNonDash[2] = {false, false};

    for (auto const& s : m_samples) {
        int const p = s.player2 ? 1 : 0;

        if (!s.release) {
            lastOrbNonDash[p] = s.orbNonDash && !s.orbDash;
            kept.push_back(s);
            continue;
        }

        char const gm = s.gamemode;
        if (!allReleases && gm != 'V' && gm != 'H' && gm != 'W' && gm != 'R')
            continue;
        if ((gm == 'H' || gm == 'W') && !testShip) continue;
        if (orbSkip && gm == 'R' && lastOrbNonDash[p]) continue;

        kept.push_back(s);
    }

    FWLOG("[fw][samples] release filter: {} -> {} (ship={} all={} orbSkip={})",
          m_samples.size(), kept.size(), testShip, allReleases, orbSkip);
    m_filtered += static_cast<int>(m_samples.size() - kept.size());

    m_samples.swap(kept);
}

size_t FrameWindowAnalyzer::nextSampleFor(size_t idx) const {
    for (size_t i = idx + 1; i < m_samples.size(); i++)
        if (m_samples[i].player2 == m_samples[idx].player2) return i;
    return NO_INDEX;
}

size_t FrameWindowAnalyzer::prevActionFor(size_t ai, bool player2) const {
    auto const& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;
    for (size_t k = ai; k-- > 0;)
        if (k < actions.size() && isMeasurableInput(actions[k]) &&
            actions[k].m_player2 == player2)
            return k;
    return NO_INDEX;
}

size_t FrameWindowAnalyzer::pairedReleaseFor(size_t ai) const {
    auto const& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;
    if (ai >= actions.size() || !actions[ai].m_holding) return NO_INDEX;
    for (size_t k = ai + 1; k < actions.size(); k++) {
        if (!isMeasurableInput(actions[k])) continue;
        if (actions[k].m_player2 != actions[ai].m_player2) continue;
        if (actions[k].m_type != actions[ai].m_type) continue;
        return actions[k].m_holding ? NO_INDEX : k;
    }
    return NO_INDEX;
}

size_t FrameWindowAnalyzer::nextActionFor(size_t ai, bool player2) const {
    auto const& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;
    for (size_t k = ai + 1; k < actions.size(); k++)
        if (isMeasurableInput(actions[k]) && actions[k].m_player2 == player2)
            return k;
    return NO_INDEX;
}

long FrameWindowAnalyzer::negRoom(size_t idx) const {
    for (size_t i = idx; i-- > 0;) {
        if (m_samples[i].player2 == m_samples[idx].player2 &&
            m_samples[i].type == m_samples[idx].type) {
            return static_cast<long>(m_samples[idx].frame) -
                   static_cast<long>(m_samples[i].frame) - 1;
        }
    }
    return static_cast<long>(m_samples[idx].frame);
}

long FrameWindowAnalyzer::posRoom(size_t idx) const {
    for (size_t i = idx + 1; i < m_samples.size(); i++) {
        if (m_samples[i].player2 == m_samples[idx].player2 &&
            m_samples[i].type == m_samples[idx].type) {
            return static_cast<long>(m_samples[i].frame) -
                   static_cast<long>(m_samples[idx].frame) - 1;
        }
    }
    return static_cast<long>(m_sweep);
}

void FrameWindowAnalyzer::muteAudio() {
    if (m_mutedAudio) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;

    m_savedMusicVolume = engine->m_musicVolume;
    m_savedSfxVolume = engine->m_sfxVolume;
    engine->setBackgroundMusicVolume(0.f);
    engine->setEffectsVolume(0.f);
    m_mutedAudio = true;
}

void FrameWindowAnalyzer::unmuteAudio() {
    if (!m_mutedAudio) return;
    auto* engine = FMODAudioEngine::sharedEngine();
    if (engine) {
        engine->setBackgroundMusicVolume(m_savedMusicVolume);
        engine->setEffectsVolume(m_savedSfxVolume);
    }
    m_mutedAudio = false;
}

FrameWindowAnalyzer::Report FrameWindowAnalyzer::start(PlayLayer* pl) {
    Report report;

    auto bot = Bot::get();
    auto& updater = bot->updater();
    auto& rs = bot->replaySystem();
    auto& pf = bot->practiceFix();

    if (m_running) {
        report.message = "Already running.";
        return report;
    }
    if (!pl) {
        report.message = "Enter a level first.";
        return report;
    }
    if (rs.m_actionAtom.m_actions.empty()) {
        report.message = "No macro loaded. Load one to analyse.";
        return report;
    }
    if (!bot->isPlaying()) {
        report.message = "Switch to playback mode first.";
        return report;
    }
    if (updater.m_preventDeath) {
        report.message =
            "Turn off Prevent Death first: with it on, every shifted frame "
            "survives and every window reads as the maximum.";
        return report;
    }
    if (double const runTps = updater.m_tps, macroTps = rs.m_initialTPS;
        macroTps > 0.0 && std::abs(runTps - macroTps) > 0.5) {
        report.message = fmt::format(
            "TPS is {:.0f} but this macro was recorded at {:.0f}. Windows are "
            "counted in frames at the running rate, so they would come out "
            "{:.2g}x off and the macro may not even replay. Set TPS to {:.0f}.",
            runTps, macroTps, macroTps / std::max(1.0, runTps), macroTps);
        log::error("[fw][start] REFUSED: {}", report.message);
        return report;
    }

    this->clear();

    m_sweep = std::clamp(m_sweepRange->inner(), 1, MAX_SWEEP);
    m_horizon = std::max(MIN_HORIZON, m_maxFrames->inner());
    m_slack_ = std::max(0, m_slack->inner());
    m_recovery = std::max(0, m_recoveryRange->inner());
    m_algo = static_cast<Algorithm>(m_algorithm->inner());

    if (!this->collectSamples()) {
        report.message = "The macro has no clicks to measure.";
        return report;
    }
    this->foldSwiftClicks();

    FWLOG(
        "[fw][start] level=\"{}\" id={} replay=\"{}\" tps={:.0f} macroTps={:.0f} "
        "actions={} samples={} "
        "sweep={} horizon={} slack={} recovery={} algo={} fullRange={} "
        "budget={}ms",
        pl->m_level ? pl->m_level->m_levelName : std::string{"?"},
        pl->m_level ? pl->m_level->m_levelID.value() : 0, rs.m_replayName,
        updater.m_tps, rs.m_initialTPS,
        rs.m_actionAtom.m_actions.size(), m_samples.size(), m_sweep, m_horizon,
        m_slack_, m_recovery, static_cast<int>(m_algo),
        m_fullRangeSweep->inner(), m_budgetMs->inner());

    m_running = true;
    m_runStart = Clock::now();
    m_restoreFailed = false;
    m_haveLastTickCall = false;
    m_capturePath.clear();
    m_captureNoclip = false;
    m_camHasTarget = false;
    m_captureDeathFrame = UINT32_MAX;
    m_captureDeaths.clear();
    m_trailChecked = false;
    m_trailDiverged = false;
    m_trailCursor = 0;
    m_triedHardReset = false;
    m_resyncFailures = 0;
    m_resyncGaveUp = false;
    m_pathDiverged = false;
    m_baseTps = Bot::get()->updater().m_tps;
    m_resultsTps = m_baseTps;
    m_probeDied = false;
    m_captureHeartbeat = 0;
    m_legCounter = 0;
    m_index = 0;
    m_total = m_samples.size();
    m_stageId = Stage::Capture;
    m_status = "Capturing";
    m_stage = "capture";

    m_trailP1 = bot->trailBuffer().stream(0);
    m_trailP2 = bot->trailBuffer().stream(1);

    m_wasPaused = updater.isPaused();
    updater.setPaused(true);
    updater.m_predicting = true;
    this->muteAudio();

    m_savedCanDie = updater.m_canDie;
    m_savedExpectsDeath = updater.m_expectsDeath;
    updater.m_canDie = false;
    updater.m_expectsDeath = false;

    pf.clearStoredFrames();
    pf.removeAll();
    pl->resetLevel();
    rs.onReset(0);

    report.ok = true;
    report.message = fmt::format("Analysing {} inputs.", m_total);
    return report;
}

bool FrameWindowAnalyzer::cameraLocked() const {
    return m_running && m_analysisVisuals->inner() && m_lockCamera->inner();
}

bool FrameWindowAnalyzer::hideSpawnEffects() const {
    return m_running && m_analysisVisuals->inner() &&
           m_hideSpawnEffects->inner();
}

void FrameWindowAnalyzer::setCameraTarget(cocos2d::CCPoint p) {
    if (!m_camHasTarget) {
        m_camFrom = p;
        m_camTo = p;
        m_camHasTarget = true;
        m_camStart = std::chrono::steady_clock::now();
        return;
    }

    if (std::abs(p.x - m_camTo.x) < 0.5f && std::abs(p.y - m_camTo.y) < 0.5f)
        return;

    m_camFrom = this->cameraPoint();
    m_camTo = p;
    m_camStart = std::chrono::steady_clock::now();
}

cocos2d::CCPoint FrameWindowAnalyzer::cameraPoint() {
    if (!m_camHasTarget) return m_camTo;

    constexpr double kSlide = 0.3;
    double const elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      m_camStart)
            .count();

    if (elapsed >= kSlide) return m_camTo;

    float t = static_cast<float>(elapsed / kSlide);
    t = 1.f - (1.f - t) * (1.f - t) * (1.f - t);

    return {m_camFrom.x + (m_camTo.x - m_camFrom.x) * t,
            m_camFrom.y + (m_camTo.y - m_camFrom.y) * t};
}

bool FrameWindowAnalyzer::setupChainable(Sample const& s) {
    if (s.release) return false;
    return s.gamemode != 'V';
}

void FrameWindowAnalyzer::detectSetupGroups() {
    m_setupGroups.clear();
    m_setupGroupCursor = 0;
    m_inSetupGroup.assign(m_samples.size(), false);
    m_setupRange.assign(m_samples.size(), {0, 0});
    m_sampleBand.assign(m_samples.size(), {0, 0});
    if (!m_jointSetupSweep->inner()) return;

    size_t start = 0;
    while (start < m_samples.size()) {
        if (!setupChainable(m_samples[start])) {
            start++;
            continue;
        }

        size_t end = start + 1;
        bool trimmed = false;
        long combos = 2L * static_cast<long>(m_sweep) + 1;

        while (end < m_samples.size()) {
            if (!setupChainable(m_samples[end])) break;
            if (m_samples[end].player2 != m_samples[start].player2) break;

            size_t const cai = m_samples[end].actionIndex;
            bool const adjacent =
                cai > 0 && m_samples[end - 1].actionIndex == cai - 1;
            if (!adjacent) break;

            long const room = static_cast<long>(m_samples[end].frame) -
                              static_cast<long>(m_originalFrames[cai - 1]) - 1;
            if (room >= static_cast<long>(m_sweep)) break;

            long const range = std::clamp<long>(room, 0, m_sweep);
            long const grown = combos * (2 * range + 1);
            if (grown > MAX_SETUP_COMBOS) {
                trimmed = true;
                break;
            }
            combos = grown;
            end++;
        }

        size_t const count = end - start;
        if (count >= 2) {
            m_setupGroups.push_back({start, count});
            for (size_t i = start; i < end; i++) m_inSetupGroup[i] = true;
            FWLOG(
                "[fw][setup] group: samples {}..{} (frames {}..{}), up to {} "
                "joint legs{}",
                start + 1, end, m_samples[start].frame,
                m_samples[end - 1].frame, combos,
                trimmed ? " -- cut short here, the next input would take it "
                          "past the leg budget"
                        : "");
        }
        start = end;
    }
}

void FrameWindowAnalyzer::applySetupFrames(SetupGroup const& sg,
                                           int leaderShift,
                                           std::vector<int> const& combo) {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;
    size_t const aiF = m_samples[sg.first].actionIndex;

    std::vector<size_t> groupAi(sg.count);
    for (size_t j = 0; j < sg.count; j++)
        groupAi[j] = m_samples[sg.first + j].actionIndex;

    for (size_t k = 0; k < actions.size(); k++) {
        int64_t shift = 0;
        if (k >= aiF) {
            shift = leaderShift;
            for (size_t j = 1; j < sg.count; j++)
                if (k >= groupAi[j]) shift += combo[j - 1];
        }
        actions[k].m_frame = static_cast<uint64_t>(std::max<int64_t>(
            static_cast<int64_t>(m_originalFrames[k]) + shift, 0));
    }
}

void FrameWindowAnalyzer::beginSetupGroup(SetupGroup const& sg) {
    m_setupFollowerRange.assign(sg.count - 1, 0);
    for (size_t j = 1; j < sg.count; j++) {
        size_t const aiPrev = m_samples[sg.first + j - 1].actionIndex;
        size_t const aiCur = m_samples[sg.first + j].actionIndex;
        long const spacing = static_cast<long>(m_originalFrames[aiCur]) -
                             static_cast<long>(m_originalFrames[aiPrev]);
        m_setupFollowerRange[j - 1] = static_cast<int>(
            std::clamp<long>(spacing - 1, 0, static_cast<long>(m_sweep)));
    }

    m_setupLeaderShift = -m_maxNeg;
    m_setupCombo.assign(sg.count - 1, 0);
    for (size_t j = 0; j < m_setupCombo.size(); j++)
        m_setupCombo[j] = -m_setupFollowerRange[j];

    m_setupSurvivors.clear();
    if (m_setupRange.size() != m_samples.size())
        m_setupRange.assign(m_samples.size(), {0, 0});

    m_setupLegActive = false;
    m_setupBestFound = false;
    m_setupBestDeviation = std::numeric_limits<int>::max();
    m_setupBestLeaderShift = 0;
    m_setupBestCombo.assign(sg.count - 1, 0);

    long combos = m_maxNeg + m_maxPos + 1;
    for (int r : m_setupFollowerRange) combos *= (2 * r + 1);
    FWLOG(
        "[fw][setup] resolving group at samples {}..{}: leader range "
        "-{}..+{}, {} follower(s), up to {} joint legs",
        sg.first + 1, sg.first + sg.count, m_maxNeg, m_maxPos,
        sg.count - 1, combos);
}

void FrameWindowAnalyzer::beginSetupLeg(SetupGroup const& sg) {
    cbf::Engine::get()->disarm();

    if (!this->restoreToBranch()) return;

    this->applySetupFrames(sg, m_setupLeaderShift, m_setupCombo);

    size_t const lastAi = m_samples[sg.first + sg.count - 1].actionIndex;
    int totalShift = m_setupLeaderShift;
    for (int o : m_setupCombo) totalShift += o;
    int64_t const lastFrame =
        static_cast<int64_t>(m_originalFrames[lastAi]) + totalShift;

        auto const& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;
    size_t const relIdx = this->pairedReleaseFor(lastAi);

    int64_t gap = static_cast<int64_t>(m_horizon);
    for (size_t k = lastAi + 1; k < actions.size(); k++) {
        if (k == relIdx) continue;
        if (!isMeasurableInput(actions[k])) continue;
        gap = static_cast<int64_t>(actions[k].m_frame) - lastFrame;
        break;
    }

    int64_t reach = std::max<int64_t>(gap - m_slack_, 0);
    reach = std::clamp<int64_t>(reach, MIN_HORIZON,
                                static_cast<int64_t>(m_horizon));

    m_setupLegTarget =
        static_cast<uint32_t>(std::max<int64_t>(lastFrame + reach, 0));
    m_setupLegActive = true;
    m_legCounter++;
}

bool FrameWindowAnalyzer::advanceSetupCombo(SetupGroup const& sg) {
    for (size_t j = 0; j < m_setupCombo.size(); j++) {
        m_setupCombo[j]++;
        if (m_setupCombo[j] <= m_setupFollowerRange[j]) return true;
        m_setupCombo[j] = -m_setupFollowerRange[j];
    }

    m_setupLeaderShift++;
    if (m_setupLeaderShift <= m_maxPos) return true;

    return false;
}

void FrameWindowAnalyzer::deriveSetupRanges(SetupGroup const& sg) {
    for (size_t j = 0; j < sg.count; j++) {
        std::map<std::vector<int>, std::vector<int>> byOthers;

        for (auto const& [leader, combo] : m_setupSurvivors) {
            std::vector<int> key;
            key.reserve(sg.count - 1);
            int mine = 0;
            for (size_t k = 0; k < sg.count; k++) {
                int const coord = k == 0 ? leader : combo[k - 1];
                if (k == j)
                    mine = coord;
                else
                    key.push_back(coord);
            }
            byOthers[key].push_back(mine);
        }

        int lo = std::numeric_limits<int>::max();
        int hi = 0;
        for (auto& entry : byOthers) {
            auto& values = entry.second;
            std::sort(values.begin(), values.end());
            int best = 0;
            int run = 0;
            for (size_t i = 0; i < values.size(); i++) {
                if (i > 0 && values[i] == values[i - 1]) continue;
                run = (i > 0 && values[i] == values[i - 1] + 1) ? run + 1 : 1;
                best = std::max(best, run);
            }
            if (best <= 0) continue;
            lo = std::min(lo, best);
            hi = std::max(hi, best);
        }

        size_t const idx = sg.first + j;
        if (hi <= 0 || idx >= m_setupRange.size()) continue;
        m_setupRange[idx] = {lo, hi};

        FWLOG(
            "[fw][setup] sample {}: window is {}..{} frames depending on how "
            "the rest of the chain is set up ({} surviving alignments)",
            idx + 1, lo, hi, m_setupSurvivors.size());
    }
}

void FrameWindowAnalyzer::commitSetupGroup(SetupGroup const& sg) {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;

    this->deriveSetupRanges(sg);

    if (!m_setupBestFound) {
        FWLOG(
            "[fw][setup] group at samples {}..{}: no joint alignment "
            "survives, keeping the macro's original timing",
            sg.first + 1, sg.first + sg.count);
        return;
    }

    this->applySetupFrames(sg, m_setupBestLeaderShift, m_setupBestCombo);
    size_t const lastAi = m_samples[sg.first + sg.count - 1].actionIndex;
    for (size_t k = m_samples[sg.first].actionIndex; k <= lastAi; k++)
        m_originalFrames[k] = actions[k].m_frame;

    for (size_t j = 0; j < sg.count; j++) {
        size_t const ai = m_samples[sg.first + j].actionIndex;
        m_samples[sg.first + j].frame =
            static_cast<uint32_t>(m_originalFrames[ai]);
    }

    for (size_t k = 0; k < actions.size(); k++)
        actions[k].m_frame = m_originalFrames[k];

    std::string offsets;
    for (size_t j = 0; j < sg.count; j++) {
        int off = j == 0 ? m_setupBestLeaderShift : m_setupBestCombo[j - 1];
        offsets += fmt::format("{}{:+d}", j == 0 ? "" : " ", off);
    }
    FWLOG(
        "[fw][setup] group at samples {}..{}: resolved, per-input offsets "
        "[{}] (total deviation {} ticks from the macro)",
        sg.first + 1, sg.first + sg.count, offsets, m_setupBestDeviation);
}

void FrameWindowAnalyzer::beginClick() {
    m_desyncPending = false;
    m_recorded = m_samples[m_index].frame;

    long const nRoom = this->negRoom(m_index);
    long const pRoom = this->posRoom(m_index);

    size_t const cai = m_samples[m_index].actionIndex;
    long actionRoom = static_cast<long>(m_recorded);
    if (size_t const prev =
            this->prevActionFor(cai, m_samples[m_index].player2);
        prev != NO_INDEX)
        actionRoom = static_cast<long>(m_recorded) -
                     static_cast<long>(m_originalFrames[prev]) - 1;

    m_maxNeg = std::clamp<int64_t>(
        m_sweep, 0,
        std::max<int64_t>(
            0, std::min<int64_t>({nRoom, actionRoom,
                                  static_cast<long>(m_recorded)})));
    m_maxPos = m_sweep;
    m_clamped = m_maxNeg < m_sweep;

    int64_t const branchSigned =
        static_cast<int64_t>(m_recorded) - m_maxNeg - 1;
    m_branchFrame = branchSigned > 0 ? static_cast<uint32_t>(branchSigned) : 0u;

    m_phase = Phase::Nominal;
    m_shift = 0;
    m_low = 0;
    m_high = 0;
    m_validCount = 0;
    m_deadShifts.clear();
    m_bufferSurvivors = 0;
    m_bisect = false;
    m_scanning = false;
    m_islandSeed = 0;
    m_negCounting = true;
    m_posCounting = true;
    m_nominalDied = false;
    m_deadRun = 0;
    m_splitWindow = false;
    m_splitShift = 0;
    m_pathDiverged = false;
    m_forceFullSweep = false;
    m_fine = 1;
    cbf::Engine::get()->disarm();
    m_entryActive = false;
    m_entryDone = false;
    m_entryOffset = 0;
    m_entryQueue.clear();
    m_coarseWindow = 0;
    m_coarseBufferSurvivors = 0;
    m_tickBuffered.clear();
    m_fineCrossedNeighbour = false;
    m_testActive = false;
    m_recoveryActive = false;

    FWLOG(
        "[fw][click {}/{}] frame={} p{} {} negRoom={} posRoom={} maxNeg={} "
        "maxPos={} branch={} clamped={}",
        m_index + 1, m_samples.size(), m_recorded,
        m_samples[m_index].player2 ? 2 : 1,
        m_samples[m_index].release ? "release" : "press", nRoom, pRoom,
        m_maxNeg, m_maxPos, m_branchFrame,
        m_clamped ? "earlier side" : "no");

    this->setCameraTarget(m_samples[m_index].position);

    m_stageId = Stage::Advance;
}

void FrameWindowAnalyzer::hardResetToStart() {
    auto bot = Bot::get();
    auto& pf = bot->practiceFix();

    this->releaseCheckpoint();
    pf.clearStoredFrames();
    pf.removeAll();
    if (auto* pl = PlayLayer::get(); pl) pl->resetLevel();
    bot->replaySystem().onReset(0);
    m_probeDied = false;
    m_noclipNextAdvance = true;
    m_pathDiverged = false;
}

void FrameWindowAnalyzer::checkCaptureAgainstTrail(PlayLayer* pl,
                                                   uint32_t frame) {
    if (m_trailChecked || !pl || !pl->m_player1) return;

    auto const& trail = m_trailP1;
    if (trail.empty()) {
        m_trailChecked = true;
        return;
    }

    if (m_trailCursor >= trail.size()) return;
    while (m_trailCursor < trail.size() && trail[m_trailCursor].frame < frame)
        m_trailCursor++;
    if (m_trailCursor >= trail.size() || trail[m_trailCursor].frame != frame)
        return;

    auto const& rect = trail[m_trailCursor].rect;
    float const wantX = (rect.minX + rect.maxX) * 0.5f;
    float const wantY = (rect.minY + rect.maxY) * 0.5f;
    auto const got = pl->m_player1->getPosition();

    float const dx = got.x - wantX;
    float const dy = got.y - wantY;
    if (dx * dx + dy * dy <= TRAIL_TOLERANCE * TRAIL_TOLERANCE) return;

    m_trailChecked = true;
    m_trailDiverged = true;
    FWWARN(
        "[fw][capture] left the macro's recorded path at frame {}: the "
        "recording was at ({:.2f},{:.2f}), the capture is at ({:.2f},{:.2f}), "
        "off by ({:.2f},{:.2f}). The macro is not at fault -- the capture "
        "desynced, so nothing measured from here on means anything",
        frame, wantX, wantY, got.x, got.y, dx, dy);
}

void FrameWindowAnalyzer::captureStateBytes(PlayLayer* pl) {
    if (!m_statePlayerDiff->inner() || !pl || !pl->m_player1) {
        m_snapP1.clear();
        return;
    }
    auto const* raw = reinterpret_cast<unsigned char const*>(pl->m_player1);
    m_snapP1.assign(raw, raw + PLAYER_BYTES);
}

namespace {
struct PlayerField {
    size_t m_offset;
    size_t m_size;
    char const* m_name;
};

static PlayerField const s_playerFields[] = {
        {offsetof(PlayerObject, m_wasTeleported), sizeof(PlayerObject::m_wasTeleported), "m_wasTeleported"},
        {offsetof(PlayerObject, m_fixGravityBug), sizeof(PlayerObject::m_fixGravityBug), "m_fixGravityBug"},
        {offsetof(PlayerObject, m_reverseSync), sizeof(PlayerObject::m_reverseSync), "m_reverseSync"},
        {offsetof(PlayerObject, m_yVelocityBeforeSlope), sizeof(PlayerObject::m_yVelocityBeforeSlope), "m_yVelocityBeforeSlope"},
        {offsetof(PlayerObject, m_dashX), sizeof(PlayerObject::m_dashX), "m_dashX"},
        {offsetof(PlayerObject, m_dashY), sizeof(PlayerObject::m_dashY), "m_dashY"},
        {offsetof(PlayerObject, m_dashAngle), sizeof(PlayerObject::m_dashAngle), "m_dashAngle"},
        {offsetof(PlayerObject, m_dashStartTime), sizeof(PlayerObject::m_dashStartTime), "m_dashStartTime"},
        {offsetof(PlayerObject, m_slopeStartTime), sizeof(PlayerObject::m_slopeStartTime), "m_slopeStartTime"},
        {offsetof(PlayerObject, m_justPlacedStreak), sizeof(PlayerObject::m_justPlacedStreak), "m_justPlacedStreak"},
        {offsetof(PlayerObject, m_lastCollisionBottom), sizeof(PlayerObject::m_lastCollisionBottom), "m_lastCollisionBottom"},
        {offsetof(PlayerObject, m_lastCollisionTop), sizeof(PlayerObject::m_lastCollisionTop), "m_lastCollisionTop"},
        {offsetof(PlayerObject, m_lastCollisionLeft), sizeof(PlayerObject::m_lastCollisionLeft), "m_lastCollisionLeft"},
        {offsetof(PlayerObject, m_lastCollisionRight), sizeof(PlayerObject::m_lastCollisionRight), "m_lastCollisionRight"},
        {offsetof(PlayerObject, m_unk50C), sizeof(PlayerObject::m_unk50C), "m_unk50C"},
        {offsetof(PlayerObject, m_unk510), sizeof(PlayerObject::m_unk510), "m_unk510"},
        {offsetof(PlayerObject, m_slopeAngle), sizeof(PlayerObject::m_slopeAngle), "m_slopeAngle"},
        {offsetof(PlayerObject, m_slopeSlidingMaybeRotated), sizeof(PlayerObject::m_slopeSlidingMaybeRotated), "m_slopeSlidingMaybeRotated"},
        {offsetof(PlayerObject, m_quickCheckpointMode), sizeof(PlayerObject::m_quickCheckpointMode), "m_quickCheckpointMode"},
        {offsetof(PlayerObject, m_maybeSavedPlayerFrame), sizeof(PlayerObject::m_maybeSavedPlayerFrame), "m_maybeSavedPlayerFrame"},
        {offsetof(PlayerObject, m_scaleXRelated2), sizeof(PlayerObject::m_scaleXRelated2), "m_scaleXRelated2"},
        {offsetof(PlayerObject, m_groundYVelocity), sizeof(PlayerObject::m_groundYVelocity), "m_groundYVelocity"},
        {offsetof(PlayerObject, m_yVelocityRelated), sizeof(PlayerObject::m_yVelocityRelated), "m_yVelocityRelated"},
        {offsetof(PlayerObject, m_scaleXRelated3), sizeof(PlayerObject::m_scaleXRelated3), "m_scaleXRelated3"},
        {offsetof(PlayerObject, m_scaleXRelated4), sizeof(PlayerObject::m_scaleXRelated4), "m_scaleXRelated4"},
        {offsetof(PlayerObject, m_scaleXRelated5), sizeof(PlayerObject::m_scaleXRelated5), "m_scaleXRelated5"},
        {offsetof(PlayerObject, m_isCollidingWithSlope), sizeof(PlayerObject::m_isCollidingWithSlope), "m_isCollidingWithSlope"},
        {offsetof(PlayerObject, m_isBallRotating), sizeof(PlayerObject::m_isBallRotating), "m_isBallRotating"},
        {offsetof(PlayerObject, m_unk669), sizeof(PlayerObject::m_unk669), "m_unk669"},
        {offsetof(PlayerObject, m_collidingWithSlopeId), sizeof(PlayerObject::m_collidingWithSlopeId), "m_collidingWithSlopeId"},
        {offsetof(PlayerObject, m_slopeFlipGravityRelated), sizeof(PlayerObject::m_slopeFlipGravityRelated), "m_slopeFlipGravityRelated"},
        {offsetof(PlayerObject, m_slopeAngleRadians), sizeof(PlayerObject::m_slopeAngleRadians), "m_slopeAngleRadians"},
        {offsetof(PlayerObject, m_rotationSpeed), sizeof(PlayerObject::m_rotationSpeed), "m_rotationSpeed"},
        {offsetof(PlayerObject, m_rotateSpeed), sizeof(PlayerObject::m_rotateSpeed), "m_rotateSpeed"},
        {offsetof(PlayerObject, m_isRotating), sizeof(PlayerObject::m_isRotating), "m_isRotating"},
        {offsetof(PlayerObject, m_isBallRotating2), sizeof(PlayerObject::m_isBallRotating2), "m_isBallRotating2"},
        {offsetof(PlayerObject, m_hasGlow), sizeof(PlayerObject::m_hasGlow), "m_hasGlow"},
        {offsetof(PlayerObject, m_isHidden), sizeof(PlayerObject::m_isHidden), "m_isHidden"},
        {offsetof(PlayerObject, m_speedMultiplier), sizeof(PlayerObject::m_speedMultiplier), "m_speedMultiplier"},
        {offsetof(PlayerObject, m_yStart), sizeof(PlayerObject::m_yStart), "m_yStart"},
        {offsetof(PlayerObject, m_gravity), sizeof(PlayerObject::m_gravity), "m_gravity"},
        {offsetof(PlayerObject, m_trailingParticleLife), sizeof(PlayerObject::m_trailingParticleLife), "m_trailingParticleLife"},
        {offsetof(PlayerObject, m_unk648), sizeof(PlayerObject::m_unk648), "m_unk648"},
        {offsetof(PlayerObject, m_gameModeChangedTime), sizeof(PlayerObject::m_gameModeChangedTime), "m_gameModeChangedTime"},
        {offsetof(PlayerObject, m_padRingRelated), sizeof(PlayerObject::m_padRingRelated), "m_padRingRelated"},
        {offsetof(PlayerObject, m_maybeReducedEffects), sizeof(PlayerObject::m_maybeReducedEffects), "m_maybeReducedEffects"},
        {offsetof(PlayerObject, m_maybeIsFalling), sizeof(PlayerObject::m_maybeIsFalling), "m_maybeIsFalling"},
        {offsetof(PlayerObject, m_shouldTryPlacingCheckpoint), sizeof(PlayerObject::m_shouldTryPlacingCheckpoint), "m_shouldTryPlacingCheckpoint"},
        {offsetof(PlayerObject, m_playEffects), sizeof(PlayerObject::m_playEffects), "m_playEffects"},
        {offsetof(PlayerObject, m_maybeCanRunIntoBlocks), sizeof(PlayerObject::m_maybeCanRunIntoBlocks), "m_maybeCanRunIntoBlocks"},
        {offsetof(PlayerObject, m_hasGroundParticles), sizeof(PlayerObject::m_hasGroundParticles), "m_hasGroundParticles"},
        {offsetof(PlayerObject, m_hasShipParticles), sizeof(PlayerObject::m_hasShipParticles), "m_hasShipParticles"},
        {offsetof(PlayerObject, m_isOnGround3), sizeof(PlayerObject::m_isOnGround3), "m_isOnGround3"},
        {offsetof(PlayerObject, m_checkpointTimeout), sizeof(PlayerObject::m_checkpointTimeout), "m_checkpointTimeout"},
        {offsetof(PlayerObject, m_lastCheckpointTime), sizeof(PlayerObject::m_lastCheckpointTime), "m_lastCheckpointTime"},
        {offsetof(PlayerObject, m_lastJumpTime), sizeof(PlayerObject::m_lastJumpTime), "m_lastJumpTime"},
        {offsetof(PlayerObject, m_lastFlipTime), sizeof(PlayerObject::m_lastFlipTime), "m_lastFlipTime"},
        {offsetof(PlayerObject, m_flashTime), sizeof(PlayerObject::m_flashTime), "m_flashTime"},
        {offsetof(PlayerObject, m_flashDuration), sizeof(PlayerObject::m_flashDuration), "m_flashDuration"},
        {offsetof(PlayerObject, m_flashDelay), sizeof(PlayerObject::m_flashDelay), "m_flashDelay"},
        {offsetof(PlayerObject, m_lastSpiderFlipTime), sizeof(PlayerObject::m_lastSpiderFlipTime), "m_lastSpiderFlipTime"},
        {offsetof(PlayerObject, m_unkBool5), sizeof(PlayerObject::m_unkBool5), "m_unkBool5"},
        {offsetof(PlayerObject, m_maybeIsVehicleGlowing), sizeof(PlayerObject::m_maybeIsVehicleGlowing), "m_maybeIsVehicleGlowing"},
        {offsetof(PlayerObject, m_switchWaveTrailColor), sizeof(PlayerObject::m_switchWaveTrailColor), "m_switchWaveTrailColor"},
        {offsetof(PlayerObject, m_practiceDeathEffect), sizeof(PlayerObject::m_practiceDeathEffect), "m_practiceDeathEffect"},
        {offsetof(PlayerObject, m_accelerationOrSpeed), sizeof(PlayerObject::m_accelerationOrSpeed), "m_accelerationOrSpeed"},
        {offsetof(PlayerObject, m_snapDistance), sizeof(PlayerObject::m_snapDistance), "m_snapDistance"},
        {offsetof(PlayerObject, m_ringJumpRelated), sizeof(PlayerObject::m_ringJumpRelated), "m_ringJumpRelated"},
        {offsetof(PlayerObject, m_onFlyCheckpointTries), sizeof(PlayerObject::m_onFlyCheckpointTries), "m_onFlyCheckpointTries"},
        {offsetof(PlayerObject, m_maybeSpriteRelated), sizeof(PlayerObject::m_maybeSpriteRelated), "m_maybeSpriteRelated"},
        {offsetof(PlayerObject, m_useLandParticles0), sizeof(PlayerObject::m_useLandParticles0), "m_useLandParticles0"},
        {offsetof(PlayerObject, m_landParticlesAngle), sizeof(PlayerObject::m_landParticlesAngle), "m_landParticlesAngle"},
        {offsetof(PlayerObject, m_landParticleRelatedY), sizeof(PlayerObject::m_landParticleRelatedY), "m_landParticleRelatedY"},
        {offsetof(PlayerObject, m_playerStreak), sizeof(PlayerObject::m_playerStreak), "m_playerStreak"},
        {offsetof(PlayerObject, m_streakStrokeWidth), sizeof(PlayerObject::m_streakStrokeWidth), "m_streakStrokeWidth"},
        {offsetof(PlayerObject, m_disableStreakTint), sizeof(PlayerObject::m_disableStreakTint), "m_disableStreakTint"},
        {offsetof(PlayerObject, m_alwaysShowStreak), sizeof(PlayerObject::m_alwaysShowStreak), "m_alwaysShowStreak"},
        {offsetof(PlayerObject, m_slopeRotation), sizeof(PlayerObject::m_slopeRotation), "m_slopeRotation"},
        {offsetof(PlayerObject, m_currentSlopeYVelocity), sizeof(PlayerObject::m_currentSlopeYVelocity), "m_currentSlopeYVelocity"},
        {offsetof(PlayerObject, m_unk3d0), sizeof(PlayerObject::m_unk3d0), "m_unk3d0"},
        {offsetof(PlayerObject, m_blackOrbRelated), sizeof(PlayerObject::m_blackOrbRelated), "m_blackOrbRelated"},
        {offsetof(PlayerObject, m_unk3e0), sizeof(PlayerObject::m_unk3e0), "m_unk3e0"},
        {offsetof(PlayerObject, m_unk3e1), sizeof(PlayerObject::m_unk3e1), "m_unk3e1"},
        {offsetof(PlayerObject, m_isAccelerating), sizeof(PlayerObject::m_isAccelerating), "m_isAccelerating"},
        {offsetof(PlayerObject, m_isCurrentSlopeTop), sizeof(PlayerObject::m_isCurrentSlopeTop), "m_isCurrentSlopeTop"},
        {offsetof(PlayerObject, m_collidedTopMinY), sizeof(PlayerObject::m_collidedTopMinY), "m_collidedTopMinY"},
        {offsetof(PlayerObject, m_collidedBottomMaxY), sizeof(PlayerObject::m_collidedBottomMaxY), "m_collidedBottomMaxY"},
        {offsetof(PlayerObject, m_collidedLeftMaxX), sizeof(PlayerObject::m_collidedLeftMaxX), "m_collidedLeftMaxX"},
        {offsetof(PlayerObject, m_collidedRightMinX), sizeof(PlayerObject::m_collidedRightMinX), "m_collidedRightMinX"},
        {offsetof(PlayerObject, m_fadeOutStreak), sizeof(PlayerObject::m_fadeOutStreak), "m_fadeOutStreak"},
        {offsetof(PlayerObject, m_canPlaceCheckpoint), sizeof(PlayerObject::m_canPlaceCheckpoint), "m_canPlaceCheckpoint"},
        {offsetof(PlayerObject, m_hasCustomGlowColor), sizeof(PlayerObject::m_hasCustomGlowColor), "m_hasCustomGlowColor"},
        {offsetof(PlayerObject, m_maybeIsColliding), sizeof(PlayerObject::m_maybeIsColliding), "m_maybeIsColliding"},
        {offsetof(PlayerObject, m_jumpBuffered), sizeof(PlayerObject::m_jumpBuffered), "m_jumpBuffered"},
        {offsetof(PlayerObject, m_stateRingJump), sizeof(PlayerObject::m_stateRingJump), "m_stateRingJump"},
        {offsetof(PlayerObject, m_wasJumpBuffered), sizeof(PlayerObject::m_wasJumpBuffered), "m_wasJumpBuffered"},
        {offsetof(PlayerObject, m_wasRobotJump), sizeof(PlayerObject::m_wasRobotJump), "m_wasRobotJump"},
        {offsetof(PlayerObject, m_stateRingJump2), sizeof(PlayerObject::m_stateRingJump2), "m_stateRingJump2"},
        {offsetof(PlayerObject, m_touchedRing), sizeof(PlayerObject::m_touchedRing), "m_touchedRing"},
        {offsetof(PlayerObject, m_touchedCustomRing), sizeof(PlayerObject::m_touchedCustomRing), "m_touchedCustomRing"},
        {offsetof(PlayerObject, m_touchedGravityPortal), sizeof(PlayerObject::m_touchedGravityPortal), "m_touchedGravityPortal"},
        {offsetof(PlayerObject, m_maybeTouchedBreakableBlock), sizeof(PlayerObject::m_maybeTouchedBreakableBlock), "m_maybeTouchedBreakableBlock"},
        {offsetof(PlayerObject, m_touchedPad), sizeof(PlayerObject::m_touchedPad), "m_touchedPad"},
        {offsetof(PlayerObject, m_yVelocity), sizeof(PlayerObject::m_yVelocity), "m_yVelocity"},
        {offsetof(PlayerObject, m_fallSpeed), sizeof(PlayerObject::m_fallSpeed), "m_fallSpeed"},
        {offsetof(PlayerObject, m_isOnSlope), sizeof(PlayerObject::m_isOnSlope), "m_isOnSlope"},
        {offsetof(PlayerObject, m_wasOnSlope), sizeof(PlayerObject::m_wasOnSlope), "m_wasOnSlope"},
        {offsetof(PlayerObject, m_slopeVelocity), sizeof(PlayerObject::m_slopeVelocity), "m_slopeVelocity"},
        {offsetof(PlayerObject, m_maybeUpsideDownSlope), sizeof(PlayerObject::m_maybeUpsideDownSlope), "m_maybeUpsideDownSlope"},
        {offsetof(PlayerObject, m_isShip), sizeof(PlayerObject::m_isShip), "m_isShip"},
        {offsetof(PlayerObject, m_isBird), sizeof(PlayerObject::m_isBird), "m_isBird"},
        {offsetof(PlayerObject, m_isBall), sizeof(PlayerObject::m_isBall), "m_isBall"},
        {offsetof(PlayerObject, m_isDart), sizeof(PlayerObject::m_isDart), "m_isDart"},
        {offsetof(PlayerObject, m_isRobot), sizeof(PlayerObject::m_isRobot), "m_isRobot"},
        {offsetof(PlayerObject, m_isSpider), sizeof(PlayerObject::m_isSpider), "m_isSpider"},
        {offsetof(PlayerObject, m_isUpsideDown), sizeof(PlayerObject::m_isUpsideDown), "m_isUpsideDown"},
        {offsetof(PlayerObject, m_isDead), sizeof(PlayerObject::m_isDead), "m_isDead"},
        {offsetof(PlayerObject, m_isOnGround), sizeof(PlayerObject::m_isOnGround), "m_isOnGround"},
        {offsetof(PlayerObject, m_isGoingLeft), sizeof(PlayerObject::m_isGoingLeft), "m_isGoingLeft"},
        {offsetof(PlayerObject, m_isSideways), sizeof(PlayerObject::m_isSideways), "m_isSideways"},
        {offsetof(PlayerObject, m_isSwing), sizeof(PlayerObject::m_isSwing), "m_isSwing"},
        {offsetof(PlayerObject, m_reverseRelated), sizeof(PlayerObject::m_reverseRelated), "m_reverseRelated"},
        {offsetof(PlayerObject, m_maybeReverseSpeed), sizeof(PlayerObject::m_maybeReverseSpeed), "m_maybeReverseSpeed"},
        {offsetof(PlayerObject, m_maybeReverseAcceleration), sizeof(PlayerObject::m_maybeReverseAcceleration), "m_maybeReverseAcceleration"},
        {offsetof(PlayerObject, m_xVelocityRelated2), sizeof(PlayerObject::m_xVelocityRelated2), "m_xVelocityRelated2"},
        {offsetof(PlayerObject, m_isDashing), sizeof(PlayerObject::m_isDashing), "m_isDashing"},
        {offsetof(PlayerObject, m_dashFireFrame), sizeof(PlayerObject::m_dashFireFrame), "m_dashFireFrame"},
        {offsetof(PlayerObject, m_groundObjectMaterial), sizeof(PlayerObject::m_groundObjectMaterial), "m_groundObjectMaterial"},
        {offsetof(PlayerObject, m_vehicleSize), sizeof(PlayerObject::m_vehicleSize), "m_vehicleSize"},
        {offsetof(PlayerObject, m_playerSpeed), sizeof(PlayerObject::m_playerSpeed), "m_playerSpeed"},
        {offsetof(PlayerObject, m_unkUnused3), sizeof(PlayerObject::m_unkUnused3), "m_unkUnused3"},
        {offsetof(PlayerObject, m_isOnGround2), sizeof(PlayerObject::m_isOnGround2), "m_isOnGround2"},
        {offsetof(PlayerObject, m_lastLandTime), sizeof(PlayerObject::m_lastLandTime), "m_lastLandTime"},
        {offsetof(PlayerObject, m_platformerVelocityRelated), sizeof(PlayerObject::m_platformerVelocityRelated), "m_platformerVelocityRelated"},
        {offsetof(PlayerObject, m_maybeIsBoosted), sizeof(PlayerObject::m_maybeIsBoosted), "m_maybeIsBoosted"},
        {offsetof(PlayerObject, m_scaleXRelatedTime), sizeof(PlayerObject::m_scaleXRelatedTime), "m_scaleXRelatedTime"},
        {offsetof(PlayerObject, m_decreaseBoostSlide), sizeof(PlayerObject::m_decreaseBoostSlide), "m_decreaseBoostSlide"},
        {offsetof(PlayerObject, m_unkA29), sizeof(PlayerObject::m_unkA29), "m_unkA29"},
        {offsetof(PlayerObject, m_isLocked), sizeof(PlayerObject::m_isLocked), "m_isLocked"},
        {offsetof(PlayerObject, m_controlsDisabled), sizeof(PlayerObject::m_controlsDisabled), "m_controlsDisabled"},
        {offsetof(PlayerObject, m_hasEverJumped), sizeof(PlayerObject::m_hasEverJumped), "m_hasEverJumped"},
        {offsetof(PlayerObject, m_hasEverHitRing), sizeof(PlayerObject::m_hasEverHitRing), "m_hasEverHitRing"},
        {offsetof(PlayerObject, m_isSecondPlayer), sizeof(PlayerObject::m_isSecondPlayer), "m_isSecondPlayer"},
        {offsetof(PlayerObject, m_unkA99), sizeof(PlayerObject::m_unkA99), "m_unkA99"},
        {offsetof(PlayerObject, m_totalTime), sizeof(PlayerObject::m_totalTime), "m_totalTime"},
        {offsetof(PlayerObject, m_isBeingSpawnedByDualPortal), sizeof(PlayerObject::m_isBeingSpawnedByDualPortal), "m_isBeingSpawnedByDualPortal"},
        {offsetof(PlayerObject, m_audioScale), sizeof(PlayerObject::m_audioScale), "m_audioScale"},
        {offsetof(PlayerObject, m_unkAngle1), sizeof(PlayerObject::m_unkAngle1), "m_unkAngle1"},
        {offsetof(PlayerObject, m_yVelocityRelated3), sizeof(PlayerObject::m_yVelocityRelated3), "m_yVelocityRelated3"},
        {offsetof(PlayerObject, m_defaultMiniIcon), sizeof(PlayerObject::m_defaultMiniIcon), "m_defaultMiniIcon"},
        {offsetof(PlayerObject, m_swapColors), sizeof(PlayerObject::m_swapColors), "m_swapColors"},
        {offsetof(PlayerObject, m_switchDashFireColor), sizeof(PlayerObject::m_switchDashFireColor), "m_switchDashFireColor"},
        {offsetof(PlayerObject, m_followRelated), sizeof(PlayerObject::m_followRelated), "m_followRelated"},
        {offsetof(PlayerObject, m_unk838), sizeof(PlayerObject::m_unk838), "m_unk838"},
        {offsetof(PlayerObject, m_stateOnGround), sizeof(PlayerObject::m_stateOnGround), "m_stateOnGround"},
        {offsetof(PlayerObject, m_stateBoostX), sizeof(PlayerObject::m_stateBoostX), "m_stateBoostX"},
        {offsetof(PlayerObject, m_stateBoostY), sizeof(PlayerObject::m_stateBoostY), "m_stateBoostY"},
        {offsetof(PlayerObject, m_maybeStateForce2), sizeof(PlayerObject::m_maybeStateForce2), "m_maybeStateForce2"},
        {offsetof(PlayerObject, m_stateScale), sizeof(PlayerObject::m_stateScale), "m_stateScale"},
        {offsetof(PlayerObject, m_platformerXVelocity), sizeof(PlayerObject::m_platformerXVelocity), "m_platformerXVelocity"},
        {offsetof(PlayerObject, m_holdingRight), sizeof(PlayerObject::m_holdingRight), "m_holdingRight"},
        {offsetof(PlayerObject, m_holdingLeft), sizeof(PlayerObject::m_holdingLeft), "m_holdingLeft"},
        {offsetof(PlayerObject, m_leftPressedFirst), sizeof(PlayerObject::m_leftPressedFirst), "m_leftPressedFirst"},
        {offsetof(PlayerObject, m_scaleXRelated), sizeof(PlayerObject::m_scaleXRelated), "m_scaleXRelated"},
        {offsetof(PlayerObject, m_maybeHasStopped), sizeof(PlayerObject::m_maybeHasStopped), "m_maybeHasStopped"},
        {offsetof(PlayerObject, m_xVelocityRelated), sizeof(PlayerObject::m_xVelocityRelated), "m_xVelocityRelated"},
        {offsetof(PlayerObject, m_maybeGoingCorrectSlopeDirection), sizeof(PlayerObject::m_maybeGoingCorrectSlopeDirection), "m_maybeGoingCorrectSlopeDirection"},
        {offsetof(PlayerObject, m_isSliding), sizeof(PlayerObject::m_isSliding), "m_isSliding"},
        {offsetof(PlayerObject, m_maybeSlopeForce), sizeof(PlayerObject::m_maybeSlopeForce), "m_maybeSlopeForce"},
        {offsetof(PlayerObject, m_isOnIce), sizeof(PlayerObject::m_isOnIce), "m_isOnIce"},
        {offsetof(PlayerObject, m_physDeltaRelated), sizeof(PlayerObject::m_physDeltaRelated), "m_physDeltaRelated"},
        {offsetof(PlayerObject, m_isOnGround4), sizeof(PlayerObject::m_isOnGround4), "m_isOnGround4"},
        {offsetof(PlayerObject, m_maybeSlidingTime), sizeof(PlayerObject::m_maybeSlidingTime), "m_maybeSlidingTime"},
        {offsetof(PlayerObject, m_maybeSlidingStartTime), sizeof(PlayerObject::m_maybeSlidingStartTime), "m_maybeSlidingStartTime"},
        {offsetof(PlayerObject, m_changedDirectionsTime), sizeof(PlayerObject::m_changedDirectionsTime), "m_changedDirectionsTime"},
        {offsetof(PlayerObject, m_slopeEndTime), sizeof(PlayerObject::m_slopeEndTime), "m_slopeEndTime"},
        {offsetof(PlayerObject, m_isMoving), sizeof(PlayerObject::m_isMoving), "m_isMoving"},
        {offsetof(PlayerObject, m_platformerMovingLeft), sizeof(PlayerObject::m_platformerMovingLeft), "m_platformerMovingLeft"},
        {offsetof(PlayerObject, m_platformerMovingRight), sizeof(PlayerObject::m_platformerMovingRight), "m_platformerMovingRight"},
        {offsetof(PlayerObject, m_isSlidingRight), sizeof(PlayerObject::m_isSlidingRight), "m_isSlidingRight"},
        {offsetof(PlayerObject, m_maybeChangedDirectionAngle), sizeof(PlayerObject::m_maybeChangedDirectionAngle), "m_maybeChangedDirectionAngle"},
        {offsetof(PlayerObject, m_unkUnused2), sizeof(PlayerObject::m_unkUnused2), "m_unkUnused2"},
        {offsetof(PlayerObject, m_isPlatformer), sizeof(PlayerObject::m_isPlatformer), "m_isPlatformer"},
        {offsetof(PlayerObject, m_stateNoAutoJump), sizeof(PlayerObject::m_stateNoAutoJump), "m_stateNoAutoJump"},
        {offsetof(PlayerObject, m_stateDartSlide), sizeof(PlayerObject::m_stateDartSlide), "m_stateDartSlide"},
        {offsetof(PlayerObject, m_stateHitHead), sizeof(PlayerObject::m_stateHitHead), "m_stateHitHead"},
        {offsetof(PlayerObject, m_stateFlipGravity), sizeof(PlayerObject::m_stateFlipGravity), "m_stateFlipGravity"},
        {offsetof(PlayerObject, m_gravityMod), sizeof(PlayerObject::m_gravityMod), "m_gravityMod"},
        {offsetof(PlayerObject, m_stateForce), sizeof(PlayerObject::m_stateForce), "m_stateForce"},
        {offsetof(PlayerObject, m_affectedByForces), sizeof(PlayerObject::m_affectedByForces), "m_affectedByForces"},
        {offsetof(PlayerObject, m_playerSpeedAC), sizeof(PlayerObject::m_playerSpeedAC), "m_playerSpeedAC"},
        {offsetof(PlayerObject, m_fixRobotJump), sizeof(PlayerObject::m_fixRobotJump), "m_fixRobotJump"},
        {offsetof(PlayerObject, m_inputsLocked), sizeof(PlayerObject::m_inputsLocked), "m_inputsLocked"},
        {offsetof(PlayerObject, m_gv0123), sizeof(PlayerObject::m_gv0123), "m_gv0123"},
        {offsetof(PlayerObject, m_iconRequestID), sizeof(PlayerObject::m_iconRequestID), "m_iconRequestID"},
        {offsetof(PlayerObject, m_unkUnused), sizeof(PlayerObject::m_unkUnused), "m_unkUnused"},
        {offsetof(PlayerObject, m_isOutOfBounds), sizeof(PlayerObject::m_isOutOfBounds), "m_isOutOfBounds"},
        {offsetof(PlayerObject, m_fallStartY), sizeof(PlayerObject::m_fallStartY), "m_fallStartY"},
        {offsetof(PlayerObject, m_disablePlayerSqueeze), sizeof(PlayerObject::m_disablePlayerSqueeze), "m_disablePlayerSqueeze"},
        {offsetof(PlayerObject, m_robotAnimation1Enabled), sizeof(PlayerObject::m_robotAnimation1Enabled), "m_robotAnimation1Enabled"},
        {offsetof(PlayerObject, m_robotAnimation2Enabled), sizeof(PlayerObject::m_robotAnimation2Enabled), "m_robotAnimation2Enabled"},
        {offsetof(PlayerObject, m_spiderAnimationEnabled), sizeof(PlayerObject::m_spiderAnimationEnabled), "m_spiderAnimationEnabled"},
        {offsetof(PlayerObject, m_ignoreDamage), sizeof(PlayerObject::m_ignoreDamage), "m_ignoreDamage"},
        {offsetof(PlayerObject, m_enable22Changes), sizeof(PlayerObject::m_enable22Changes), "m_enable22Changes"},
        {offsetof(PlayerObject, m_enableImpulseFix), sizeof(PlayerObject::m_enableImpulseFix), "m_enableImpulseFix"},
};

std::string describePlayerBytes(size_t begin, size_t end) {
    std::string out;
    for (auto const& f : s_playerFields) {
        if (f.m_offset >= end || f.m_offset + f.m_size <= begin) continue;
        if (!out.empty()) out += ", ";
        out += f.m_name;
    }
    return out.empty() ? std::string("<unnamed/padding>") : out;
}
}  // namespace

void FrameWindowAnalyzer::reportStateDiff(PlayLayer* pl) {
    if (m_snapP1.size() != PLAYER_BYTES || !pl || !pl->m_player1) return;

    auto const* raw = reinterpret_cast<unsigned char const*>(pl->m_player1);

    struct Region {
        size_t m_begin;
        size_t m_end;
    };
    static Region const benign[] = {
        {0x10, 0x18},
        {offsetof(PlayerObject, m_objectRect),
         offsetof(PlayerObject, m_objectRect) + sizeof(cocos2d::CCRect)},
        {offsetof(PlayerObject, m_isObjectRectDirty),
         offsetof(PlayerObject, m_isOrientedBoxDirty) + 1},
        {offsetof(PlayerObject, m_sTransform),
         offsetof(PlayerObject, m_sTransform) +
             sizeof(cocos2d::CCAffineTransform)},
        {offsetof(PlayerObject, m_bTransformDirty),
         offsetof(PlayerObject, m_bTransformDirty) + 1},
    };

    auto isBenign = [](size_t begin, size_t end) {
        for (auto const& r : benign)
            if (begin >= r.m_begin && end <= r.m_end) return true;
        return false;
    };

    size_t i = 0x10;
    int runs = 0;
    int realRuns = 0;
    std::string out;
    while (i < PLAYER_BYTES) {
        if (raw[i] == m_snapP1[i]) {
            i++;
            continue;
        }
        size_t const start = i;
        while (i < PLAYER_BYTES && raw[i] != m_snapP1[i]) i++;
        runs++;
        if (isBenign(start, i)) continue;
        if (++realRuns <= 24)
            out += fmt::format("\n    +0x{:x}..0x{:x} ({} bytes) -> {}",
                               start, i - 1, i - start,
                               describePlayerBytes(start, i));
    }

    m_stateDiffRestores++;
    if (realRuns == 0) {
        if (runs > 0) m_stateDiffCachedOnly++;
        return;
    }

    m_stateDiffReal++;
    FWWARN(
        "[fw][statediff] click {}: {} differing run(s) in PlayerObject after "
        "restore, outside the lazily rebuilt caches{}{}",
        m_index + 1, realRuns, out,
        realRuns > 24 ? "\n    (more not listed)" : "");
}

void FrameWindowAnalyzer::reassertHeldButtons(uint32_t frame) {
    auto* pl = PlayLayer::get();
    if (!pl) return;

    auto reapply = [](PlayerObject* p, SavedPlayerCheckpoint const& cp) {
        if (!p) return;
        p->m_jumpBuffered = cp.m_jumpBuffered;
        p->m_wasJumpBuffered = cp.m_wasJumpBuffered;
        p->m_stateJumpBuffered = cp.m_stateJumpBuffered;
        p->m_holdingButtons = cp.m_holdingButtons;

        p->m_stateRingJump = cp.m_stateRingJump;
        p->m_stateRingJump2 = cp.m_stateRingJump2;

        p->m_isDashing = cp.m_isDashing;
        p->m_dashX = cp.m_dashX;
        p->m_dashY = cp.m_dashY;
        p->m_dashAngle = cp.m_dashAngle;
        p->m_dashStartTime = cp.m_dashStartTime;
        p->m_dashRing = cp.m_dashRing;
        p->m_yVelocity = cp.m_yVelocity;

        p->m_touchedPad = cp.m_touchedPad;
        p->m_wasRobotJump = cp.m_wasRobotJump;
    };

    reapply(pl->m_player1, m_saved.m_player1);
    reapply(pl->m_player2, m_saved.m_player2);

    this->reportStateDiff(pl);

    FWLOG(
        "[fw][restore] re-applied hold state at frame {} (p1 jumpBuffered={} "
        "dashing={})",
        frame, m_saved.m_player1.m_jumpBuffered, m_saved.m_player1.m_isDashing);
}

bool FrameWindowAnalyzer::restoreToBranch() {
    auto bot = Bot::get();
    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();

    auto* plBefore = PlayLayer::get();
    float const bx = plBefore && plBefore->m_player1
                         ? plBefore->m_player1->getPositionX() : -1.f;
    float const by = plBefore && plBefore->m_player1
                         ? plBefore->m_player1->getPositionY() : -1.f;
    uint32_t const bf = updater.getFrame();

    m_probeDied = false;
    m_restoring = true;
    pf.resetWithState(m_saved);
    m_restoring = false;
    bot->replaySystem().onReset(updater.getFrame());
    this->reassertHeldButtons(updater.getFrame());

    if (auto* pS = PlayLayer::get(); pS && pS->m_player1)
        logPlayerState("post-restore", pS->m_player1);

    if (auto* plAfter = PlayLayer::get(); plAfter && plAfter->m_player1) {
        FWLOG("[fw][restore] frame {}->{} (want {}) pos ({:.1f},{:.1f})->"
              "({:.1f},{:.1f}) want ({:.1f},{:.1f}) yvel={:.3f}",
              bf, updater.getFrame(), m_saved.m_frameOffset, bx, by,
              plAfter->m_player1->getPositionX(),
              plAfter->m_player1->getPositionY(),
              m_saved.m_player1.m_ccPosition.x, m_saved.m_player1.m_ccPosition.y,
              plAfter->m_player1->m_yVelocity);
    }

    if (updater.getFrame() != m_saved.m_frameOffset) {
        FWLOG(
            "[fw][restore] FAILED: frame is {} but checkpoint says {} -- the "
            "forced-state branch was bypassed",
            updater.getFrame(), m_saved.m_frameOffset);
        m_restoreFailed = true;
        return false;
    }

    if (auto* pl = PlayLayer::get(); pl && pl->m_player1) {
        auto const want = m_saved.m_player1.m_ccPosition;
        auto const got = pl->m_player1->getPosition();
        float const dx = got.x - want.x;
        float const dy = got.y - want.y;
        if (std::fabs(dx) > POSITION_EPSILON ||
            std::fabs(dy) > POSITION_EPSILON) {
            FWWARN(
                "[fw][restore] player landed at ({:.4f},{:.4f}) but the "
                "checkpoint holds ({:.4f},{:.4f}) -- off by ({:.4f},{:.4f}) "
                "at frame {}",
                got.x, got.y, want.x, want.y, dx, dy, m_saved.m_frameOffset);
        }
    }

    if (auto* pl = PlayLayer::get(); pl && pl->m_playerDied) {
        FWLOG("[fw][restore] FAILED: player still dead after restore at {}",
              m_saved.m_frameOffset);
        m_restoreFailed = true;
        return false;
    }

    return true;
}

bool FrameWindowAnalyzer::bufferShiftValid(int64_t shift) const {
    size_t const ai = m_samples[m_index].actionIndex;

    int tickShift = shift;
    double fraction = 0.0;
    this->splitSlots(shift, tickShift, fraction);

    int64_t const shifted = static_cast<int64_t>(m_recorded) + tickShift;

    if (shifted < 0) return false;

    bool const p2 = m_samples[m_index].player2;
    if (size_t const prev = this->prevActionFor(ai, p2);
        prev != NO_INDEX &&
        shifted <= static_cast<int64_t>(m_originalFrames[prev]))
        return false;
    if (size_t const next = this->nextActionFor(ai, p2);
        next != NO_INDEX &&
        shifted >= static_cast<int64_t>(m_originalFrames[next]))
        return false;

    if (size_t const rel = this->pairedReleaseFor(ai); rel != NO_INDEX) {
        int64_t const relShifted =
            static_cast<int64_t>(m_originalFrames[rel]) + tickShift;
        if (size_t const after = this->nextActionFor(rel, p2);
            after != NO_INDEX &&
            relShifted >= static_cast<int64_t>(m_originalFrames[after]))
            return false;
    }
    return true;
}

void FrameWindowAnalyzer::beginShift(int64_t shift, ShiftMode mode) {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;

    cbf::Engine::get()->disarm();
    m_releaseFrame = -1;

    if (!this->restoreToBranch()) return;

    size_t const ai = m_samples[m_index].actionIndex;

    int tickShift = shift;
    double fraction = 0.0;
    this->splitSlots(shift, tickShift, fraction);

    int64_t const shifted =
        static_cast<int64_t>(m_recorded) + m_entryOffset + tickShift;

    bool const thisP2 = m_samples[m_index].player2;

    if (m_fine > 1 && mode == ShiftMode::Retime) {
        mode = ShiftMode::Buffer;

        if (shift != 0 && !this->bufferShiftValid(shift)) {
            m_fineCrossedNeighbour = true;
            FWLOG(
                "[fw][leg {}] click {} slot={:+d} CROSSES A NEIGHBOURING INPUT "
                "-- the whole-frame pass would have refused this shift; it is "
                "still measured, so this click's window may reach past the "
                "input next to it",
                m_legCounter + 1, m_index + 1, shift);
        }
    }

    m_shiftMode = mode;
    m_tailArm.clear();
    if (mode == ShiftMode::Retime) {
        m_bufferTried = false;
        for (size_t k = 0; k < actions.size(); k++) {
            int64_t moved = static_cast<int64_t>(m_originalFrames[k]);
            if (m_entryActive && k >= m_entryPrevAi) moved += m_entryOffset;
            bool const tail = k >= ai && isMeasurableInput(actions[k]) &&
                              actions[k].m_player2 == thisP2;
            if (tail) moved += tickShift;
            moved = std::max<int64_t>(moved, 0);
            actions[k].m_frame = static_cast<uint64_t>(moved);
            if (tail && moved != shifted) m_tailArm.push_back(moved);
        }
    } else {
        for (size_t k = 0; k < actions.size(); k++) {
            int64_t moved = static_cast<int64_t>(m_originalFrames[k]);
            if (m_entryActive && k >= m_entryPrevAi) moved += m_entryOffset;
            actions[k].m_frame =
                static_cast<uint64_t>(std::max<int64_t>(moved, 0));
        }
        actions[ai].m_frame =
            static_cast<uint64_t>(std::max<int64_t>(shifted, 0));
        if (size_t const rel = this->pairedReleaseFor(ai); rel != NO_INDEX) {
            int64_t relMoved =
                static_cast<int64_t>(m_originalFrames[rel]) + tickShift;
            if (m_entryActive && rel >= m_entryPrevAi) relMoved += m_entryOffset;
            relMoved = std::max<int64_t>(relMoved, 0);
            actions[rel].m_frame = static_cast<uint64_t>(relMoved);
            m_releaseFrame = relMoved;
        }
    }
    size_t const relIdx = this->pairedReleaseFor(ai);

    int64_t gap = static_cast<int64_t>(m_horizon);
    for (size_t k = ai + 1; k < actions.size(); k++) {
        if (k == relIdx) continue;
        if (!isMeasurableInput(actions[k])) continue;
        gap = static_cast<int64_t>(actions[k].m_frame) - shifted;
        break;
    }

    int64_t reach = std::max<int64_t>(gap - m_slack_, 0);
    reach = std::clamp<int64_t>(reach, MIN_HORIZON,
                                static_cast<int64_t>(m_horizon));

    if (fraction > 0.0) {
        auto* eng = cbf::Engine::get();
        eng->arm(static_cast<uint32_t>(std::max<int64_t>(shifted, 0)), fraction);
        if (mode == ShiftMode::Retime) {
            for (int64_t f : m_tailArm)
                eng->arm(static_cast<uint32_t>(f), fraction);
        } else if (m_releaseFrame >= 0 && m_releaseFrame != shifted) {
            eng->arm(static_cast<uint32_t>(m_releaseFrame), fraction);
        }
    }

    m_pathDiverged = false;
    g_traceLastFrame = UINT32_MAX;
    m_shift = shift;
    m_legPressFrame = static_cast<uint32_t>(std::max<int64_t>(shifted, 0));
    m_legPressBuffered = false;
    m_legPressSeen = false;
    m_testTarget = static_cast<uint32_t>(std::max<int64_t>(shifted + reach, 0));
    m_testActive = true;
    m_stageId = Stage::Probe;
    m_legCounter++;

    FWLOG(
        "[fw][leg {}] click {} slot={:+d} -> tick{:+d} @ {:.4f} of the tick, "
        "release={} tail={} @ {:.4f}, reach={} target={} from={} mode={} "
        "slots={}",
        m_legCounter, m_index + 1, shift, tickShift, fraction, m_releaseFrame,
        m_tailArm.size(), fraction, reach,
        m_testTarget, Bot::get()->updater().getFrame(),
        mode == ShiftMode::Retime ? "retime" : "buffer", std::max<int64_t>(1, m_fine));
}

void FrameWindowAnalyzer::concludeShift(bool survived) {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;


    if (m_legPressSeen && m_legPressFrame != UINT32_MAX)
        m_tickBuffered[m_legPressFrame] = m_legPressBuffered;

    FWLOG(
        "[fw][leg {}] click {} shift={:+d} survived={} at frame={} recovery={} "
        "press={} split={} ground={} yvel={:.4f} end={}",
        m_legCounter, m_index + 1, m_shift, survived,
        Bot::get()->updater().getFrame(), m_recoveryActive,
        m_legPressSeen ? m_legPressFrame : UINT32_MAX,
        m_legPressSeen ? (m_legPressBuffered ? 0 : 1) : -1,
        m_legPressSeen ? (m_legPressOnGround ? 1 : 0) : -1,
        m_legPressSeen ? m_legPressYVel : 0.0, m_testTarget);

    for (size_t i = 0; i < actions.size(); i++)
        actions[i].m_frame = m_originalFrames[i];

    m_testActive = false;

    if (!survived && !m_recoveryActive && m_shiftMode == ShiftMode::Retime &&
        m_fine <= 1 && !m_bufferTried && m_shift != 0) {
        m_bufferTried = true;
        if (this->bufferShiftValid(m_shift)) {
            this->beginShift(m_shift, ShiftMode::Buffer);
            return;
        }
    }

    if (survived && m_shiftMode == ShiftMode::Buffer) {
        m_bufferSurvivors++;
        this->advanceSweep(true);
        return;
    }

    if (m_algo == Algorithm::RecoveryRange && !m_recoveryActive &&
        m_fine <= 1) {
        if (survived) {
            this->advanceSweep(true);
            return;
        }
        m_recoveryOffset = -m_recovery;
        this->beginRecovery();
        return;
    }

    if (m_recoveryActive) {
        m_recoveryActive = false;
        if (survived) {
            this->advanceSweep(true);
            return;
        }
        m_recoveryOffset++;
        if (m_recoveryOffset > m_recovery) {
            this->advanceSweep(false);
            return;
        }
        this->beginRecovery();
        return;
    }

    this->advanceSweep(survived);
}

void FrameWindowAnalyzer::advanceSweep(bool survived) {
    if (!survived && !m_recoveryActive &&
        m_deadShifts.size() < MAX_DEAD_TRACKED)
        m_deadShifts.push_back(m_shift);

    bool counting = true;
    if (m_phase == Phase::Earlier)
        counting = m_negCounting;
    else if (m_phase == Phase::Later)
        counting = m_posCounting;

    if (m_nominalDied) counting = true;

    if (survived && counting && !m_bisect) m_validCount++;

    if (m_nominalDied && survived && m_phase != Phase::Nominal) {
        if (m_validCount == 1) {
            m_low = m_shift;
            m_high = m_shift;
        } else {
            m_low = std::min(m_low, m_shift);
            m_high = std::max(m_high, m_shift);
        }
        FWLOG(
            "[fw][click {}] survivable alignment at shift {:+d} (fine={}x)",
            m_index + 1, m_shift, m_fine);
    }

    if (!survived) {
        if (m_phase == Phase::Earlier)
            m_negCounting = false;
        else if (m_phase == Phase::Later)
            m_posCounting = false;
    }

    if (m_phase == Phase::Nominal) {
        if (!survived) {
            m_nominalDied = true;
            m_forceFullSweep = this->captureDiedInSpan();
            if (m_entryActive) {
                FWLOG(
                    "[fw][click {}] entry {:+d}: the macro does not survive "
                    "this entry (died at frame {})",
                    m_index + 1, m_entryOffset,
                    Bot::get()->updater().getFrame());
            } else if (m_pathDiverged) {
                if (m_fine == 1) {
                    m_desynced++;
                    m_desyncPending = true;
                }
                FWWARN(
                    "[fw][click {}] RESTORE MISMATCH @ frame {}: the replay "
                    "left the captured path before dying (at frame {}). The "
                    "macro is fine; the checkpoint did not reproduce the "
                    "capture, so this window cannot be measured",
                    m_index + 1, m_recorded,
                    Bot::get()->updater().getFrame());
            } else {
                if (m_fine == 1) {
                    m_desynced++;
                    m_desyncPending = true;
                }
                FWWARN(
                    "[fw][click {}] DESYNC @ frame {}: the unmodified macro "
                    "died during its own probe (died at frame {}), killed by "
                    "object id {} at ({:.1f},{:.1f}). The player followed the "
                    "captured path exactly up to the death, so the state that "
                    "differs from the capture is not in the player -- look at "
                    "that object",
                    m_index + 1, m_recorded,
                    Bot::get()->updater().getFrame(), m_lastKillerId,
                    m_lastKillerPos.x, m_lastKillerPos.y);
            }
        } else {
            m_low = 0;
            m_high = 0;
        }

        if (!survived && !m_entryActive && !m_forceFullSweep) {
            m_low = 0;
            m_high = 0;
            m_validCount = 0;
            FWLOG(
                "[fw][click {}] sweep skipped: the macro's own timing does not "
                "reproduce here, so no shifted timing would mean anything "
                "either. Not probing the {} offsets either side",
                m_index + 1, m_maxNeg + m_maxPos);
            this->finishClick();
            return;
        }

        if (m_bisect && !m_scanning && (!survived || m_forceFullSweep)) {
            m_bisect = false;
            FWLOG(
                "[fw][click {}] bisect off: {} -- falling back to the linear "
                "sweep",
                m_index + 1,
                survived ? "capture died in this span" : "nominal died");
        }

        if (m_scanning) {
            if (survived) {
                m_scanning = false;
            } else {
                m_phase = Phase::Earlier;
                m_scanLastDead = -m_maxNeg - 1;
                this->beginShift(-m_maxNeg);
                return;
            }
        }

        if (m_bisect) {
            m_gallopping = true;
            m_gallopStep = m_scanStep;
            if (m_maxNeg > 0) {
                m_phase = Phase::Earlier;
                m_bisectLo = -m_maxNeg;
                m_bisectHi = 0;
                this->beginShift(this->nextBisectShift());
                return;
            }
            if (m_maxPos > 0) {
                m_phase = Phase::Later;
                m_bisectLo = 0;
                m_bisectHi = m_maxPos;
                this->beginShift(this->nextBisectShift());
                return;
            }
            m_validCount = 1;
            this->finishClick();
            return;
        }

        if (m_maxNeg > 0) {
            m_phase = Phase::Earlier;
            m_shift = -1;
        } else if (m_maxPos > 0) {
            m_phase = Phase::Later;
            m_shift = 1;
        } else {
            this->finishClick();
            return;
        }
        this->beginShift(m_shift);
        return;
    }

    if (m_scanning) {
        if (!survived) m_scanLastDead = m_shift;

        if (survived) {
            m_scanning = false;
            m_islandSeed = m_shift;
            m_low = m_shift;
            m_high = m_shift;
            FWLOG(
                "[fw][click {}] island scan: hit a survivor at shift {:+d} "
                "after {} legs -- bisecting its edges",
                m_index + 1, m_shift, m_legCounter);

            m_gallopping = true;
            m_gallopStep = m_scanStep;
            int64_t const floor =
                std::max<int64_t>(-m_maxNeg, m_scanLastDead + 1);
            if (m_shift > floor) {
                m_phase = Phase::Earlier;
                m_bisectLo = floor;
                m_bisectHi = m_shift;
                this->beginShift(this->nextBisectShift());
                return;
            }
            m_phase = Phase::Later;
            m_bisectLo = m_shift;
            m_bisectHi = m_maxPos;
            if (m_bisectLo < m_bisectHi) {
                this->beginShift(this->nextBisectShift());
                return;
            }
            m_validCount = 1;
            this->finishClick();
            return;
        }

        int64_t next = m_shift + m_scanStep;
        if (next == 0) next = m_scanStep;
        if (next <= m_maxPos) {
            this->beginShift(next);
            return;
        }

        m_low = 0;
        m_high = 0;
        m_validCount = 0;
        FWLOG(
            "[fw][click {}] island scan: nothing survives anywhere in "
            "-{}..+{} fine ticks ({} legs)",
            m_index + 1, m_maxNeg, m_maxPos, m_legCounter);
        this->finishClick();
        return;
    }

    if (m_phase == Phase::Earlier && m_bisect) {
        if (survived) {
            m_bisectHi = m_shift;
        } else {
            m_bisectLo = m_shift + 1;
            m_gallopping = false;
        }

        if (m_bisectLo < m_bisectHi) {
            this->beginShift(this->nextBisectShift());
            return;
        }

        m_low = m_bisectLo;
        FWLOG("[fw][click {}] search: earliest surviving shift {:+d}",
              m_index + 1, m_low);

        if (m_maxPos <= 0) {
            m_validCount = m_high - m_low + 1;
            this->finishClick();
            return;
        }

        m_phase = Phase::Later;
        m_gallopping = true;
        m_gallopStep = m_scanStep;
        m_bisectLo = m_islandSeed;
        m_bisectHi = m_maxPos;
        if (m_bisectLo >= m_bisectHi) {
            m_high = m_bisectLo;
            m_validCount = m_high - m_low + 1;
            this->finishClick();
            return;
        }
        this->beginShift(this->nextBisectShift());
        return;
    }

    if (m_phase == Phase::Later && m_bisect) {
        if (survived) {
            m_bisectLo = m_shift;
        } else {
            m_bisectHi = m_shift - 1;
            m_gallopping = false;
        }

        if (m_bisectLo < m_bisectHi) {
            this->beginShift(this->nextBisectShift());
            return;
        }

        m_high = m_bisectLo;
        m_validCount = m_high - m_low + 1;
        FWLOG(
            "[fw][click {}] search: latest surviving shift {:+d} -- window "
            "{}..{} = {} fine ticks in {} legs",
            m_index + 1, m_high, m_low, m_high, m_validCount, m_legCounter);
        this->finishClick();
        return;
    }

    if (m_phase == Phase::Earlier) {
        if (survived && m_negCounting) m_low = m_shift;
        this->noteSweepStep(survived, m_negCounting);

        bool const keepGoing = m_fullRangeSweep->inner() || survived ||
                               m_forceFullSweep ||
                               m_deadRun <= DEAD_RUN_MARGIN;
        if (keepGoing && m_shift - 1 >= -m_maxNeg) {
            this->beginShift(m_shift - 1);
            return;
        }

        if (m_maxPos <= 0) {
            this->finishClick();
            return;
        }
        m_phase = Phase::Later;
        m_deadRun = 0;
        this->beginShift(1);
        return;
    }

    if (survived && m_posCounting) m_high = m_shift;
    this->noteSweepStep(survived, m_posCounting);

    bool const keepGoing = m_fullRangeSweep->inner() || survived ||
                           m_forceFullSweep || m_deadRun <= DEAD_RUN_MARGIN;
    if (keepGoing && m_shift + 1 <= m_maxPos) {
        this->beginShift(m_shift + 1);
        return;
    }

    this->finishClick();
}

void FrameWindowAnalyzer::noteSweepStep(bool survived, bool counting) {
    if (!survived) {
        m_deadRun++;
        return;
    }

    m_deadRun = 0;
    if (counting || m_splitWindow) return;

    m_splitWindow = true;
    m_splitShift = m_shift;
    FWWARN(
        "[fw][click {}] shift {:+d} survives on the far side of a gap. The "
        "reported window is only the run of offsets touching the macro's own "
        "timing -- this input also works somewhere the window does not cover, "
        "so the number is the room around THIS macro's timing, not how tight "
        "the input is. A macro sitting in a narrow pocket beside the real "
        "window reads far tighter than the input actually is",
        m_index + 1, m_shift);
}

int64_t FrameWindowAnalyzer::nextBisectShift() const {
    if (m_phase == Phase::Earlier) {
        if (m_gallopping)
            return std::max(m_bisectLo, m_bisectHi - m_gallopStep);
        return m_bisectLo + (m_bisectHi - m_bisectLo) / 2;
    }
    if (m_gallopping)
        return std::min(m_bisectHi, m_bisectLo + m_gallopStep);
    return m_bisectLo + (m_bisectHi - m_bisectLo + 1) / 2;
}

void FrameWindowAnalyzer::noteCaptureDeath(uint32_t frame) {
    if (!m_captureDeaths.empty() && m_captureDeaths.back() == frame) return;
    if (m_captureDeaths.size() >= MAX_CAPTURE_DEATHS) return;

    m_captureDeaths.push_back(frame);
    if (m_captureDeaths.size() > 1)
        FWWARN(
            "[fw][capture] died again at frame {} ({} deaths so far) -- still "
            "running on noclip",
            frame, m_captureDeaths.size());
}

uint32_t FrameWindowAnalyzer::captureDeathInSpan() const {
    if (!m_captureNoclip || m_captureDeaths.empty()) return UINT32_MAX;

    uint32_t const lo = m_branchFrame;
    uint32_t const hi = m_index + 1 < m_samples.size()
                            ? m_samples[m_index + 1].frame
                            : m_recorded + m_horizon;

    for (uint32_t const d : m_captureDeaths)
        if (d >= lo && d <= hi) return d;
    return UINT32_MAX;
}

bool FrameWindowAnalyzer::captureDiedInSpan() const {
    return this->captureDeathInSpan() != UINT32_MAX;
}

bool FrameWindowAnalyzer::startSubframeProbe() {
    if (!m_subframeProbe->inner()) return false;
    if (m_fine > 1) return false;

    double const hz =
        static_cast<double>(std::clamp<int64_t>(m_cbfInputHz->inner(), 240,
                                               MAX_CBF_HZ));
    int64_t const mult = std::clamp<int64_t>(
        std::llround(hz / std::max(1.0, m_baseTps)), 1, MAX_CBF_SLOTS);

    if (mult < 2) {
        FWLOG(
            "[fw][click {}] CBF pass skipped: {:.0f} Hz gives under one "
            "sub-tick slot at {:.0f} TPS",
            m_index + 1, hz, m_baseTps);
        return false;
    }

    int const tight = std::max(0, m_tightThreshold->inner());
    bool const impossible = m_nominalDied && this->captureDiedInSpan();
    if (m_nominalDied && !impossible) return false;
    if (!impossible && !m_subframeAll->inner() && m_validCount > tight)
        return false;
    if (m_maxNeg <= 0 && m_maxPos <= 0) {
        FWLOG(
            "[fw][click {}] CBF pass skipped: no room either side of the "
            "input to move it into",
            m_index + 1);
        return false;
    }

    m_coarseWindow = m_validCount;
    m_coarseLow = m_low;
    m_coarseHigh = m_high;
    m_coarseNominalDied = m_nominalDied;
    m_coarseMaxNeg = m_maxNeg;
    m_coarseMaxPos = m_maxPos;
    m_coarseBufferSurvivors = m_bufferSurvivors;
    m_fine = mult;

    int const coarseReach =
        impossible ? std::max(IMPOSSIBLE_SWEEP_TICKS, m_sweep) : m_sweep;

    std::string const why =
        impossible ? std::string("no alignment on the frame grid")
        : m_subframeAll->inner()
            ? fmt::format("window {}", m_coarseWindow)
            : fmt::format("tight window {}", m_coarseWindow);

    FWLOG(
        "[fw][click {}] {} -- re-measuring with the CBF split: {} slots per "
        "tick ({:.0f} Hz at {:.0f} TPS, {:.4f} ms apart), +/-{} ticks",
        m_index + 1, why, mult, hz, m_baseTps, 1000.0 / (m_baseTps * mult),
        coarseReach);

    m_phase = Phase::Nominal;
    m_shift = 0;
    m_low = 0;
    m_high = 0;
    m_validCount = 0;
    m_deadShifts.clear();
    m_bufferSurvivors = 0;
    m_negCounting = true;
    m_posCounting = true;
    m_nominalDied = false;
    m_deadRun = 0;
    m_bisect = m_subframeBisect->inner() && !m_fullRangeSweep->inner();
    m_scanning = m_bisect && impossible;
    m_islandSeed = 0;
    {
        int64_t const pct = std::clamp(m_subframeScanPercent->inner(), 1, 100);
        m_scanStep = std::max<int64_t>(1, mult * pct / 100);
        FWLOG(
            "[fw][click {}] {}: stride {} fine ticks ({}% of a frame), edges "
            "bisected exactly -- gaps narrower than the stride can be stepped "
            "over",
            m_index + 1, m_scanning ? "island scan" : "edge walk", m_scanStep,
            pct);
    }
    m_maxNeg = std::min<int64_t>(m_maxNeg, coarseReach) * mult;
    m_maxPos = std::min<int64_t>(m_maxPos, coarseReach) * mult;
    if (impossible) {
        m_maxNeg = coarseReach * mult;
        m_maxPos = coarseReach * mult;
    }

    int64_t const room = (static_cast<int64_t>(m_recorded) -
                          static_cast<int64_t>(m_branchFrame)) *
                             mult -
                         1;
    m_maxNeg = std::min<int64_t>(m_maxNeg, std::max<int64_t>(room, 0));

    FWLOG("[fw][click {}] fine sweep range -{}..+{} fine ticks (branch {})",
          m_index + 1, m_maxNeg, m_maxPos, m_branchFrame);

    this->beginShift(0);
    return true;
}

void FrameWindowAnalyzer::beginRecovery() {
    auto& actions = Bot::get()->replaySystem().m_actionAtom.m_actions;

    size_t const recIdx = this->nextSampleFor(m_index);
    if (recIdx == NO_INDEX) {
        this->advanceSweep(false);
        return;
    }

    cbf::Engine::get()->disarm();

    if (!this->restoreToBranch()) return;

    size_t const ai = m_samples[m_index].actionIndex;
    bool const thisP2 = m_samples[m_index].player2;

    int tickShift = m_shift;
    double fraction = 0.0;
    this->splitSlots(m_shift, tickShift, fraction);

    for (size_t k = 0; k < actions.size(); k++) {
        int64_t moved = static_cast<int64_t>(m_originalFrames[k]);
        if (m_entryActive && k >= m_entryPrevAi) moved += m_entryOffset;
        if (k >= ai && isMeasurableInput(actions[k]) &&
            actions[k].m_player2 == thisP2)
            moved += tickShift;
        actions[k].m_frame = static_cast<uint64_t>(std::max<int64_t>(moved, 0));
    }

    int64_t const shifted =
        static_cast<int64_t>(m_recorded) + m_entryOffset + tickShift;
    if (fraction > 0.0)
        cbf::Engine::get()->arm(
            static_cast<uint32_t>(std::max<int64_t>(shifted, 0)), fraction);

    size_t const ni = m_samples[recIdx].actionIndex;
    int64_t const nextShifted = static_cast<int64_t>(m_samples[recIdx].frame) +
                                m_entryOffset + tickShift + m_recoveryOffset;
    actions[ni].m_frame =
        static_cast<uint64_t>(std::max<int64_t>(nextShifted, 0));

    size_t const recRelIdx = this->pairedReleaseFor(ni);

    int64_t recGap = static_cast<int64_t>(m_horizon);
    for (size_t k = ni + 1; k < actions.size(); k++) {
        if (k == recRelIdx) continue;
        if (!isMeasurableInput(actions[k])) continue;
        recGap = static_cast<int64_t>(actions[k].m_frame) - nextShifted;
        break;
    }

    int64_t recReach = std::max<int64_t>(recGap - m_slack_, 0);
    recReach = std::clamp<int64_t>(recReach, MIN_HORIZON,
                                   static_cast<int64_t>(m_horizon));

    m_legPressFrame = static_cast<uint32_t>(std::max<int64_t>(shifted, 0));
    m_legPressBuffered = false;
    m_legPressSeen = false;
    m_testTarget =
        static_cast<uint32_t>(std::max<int64_t>(nextShifted + recReach, 0));
    m_testActive = true;
    m_recoveryActive = true;
    m_stageId = Stage::Recover;
}

FrameWindowMark FrameWindowAnalyzer::buildMark() const {

    auto* pl = PlayLayer::get();
    float const levelLen =
        pl && pl->m_levelLength > 0.f ? pl->m_levelLength : 1.f;

    auto const& s = m_samples[m_index];

    int64_t low = m_low;
    int64_t high = m_high;
    int64_t maxNeg = m_maxNeg;
    int64_t maxPos = m_maxPos;
    bool nominalDied = m_nominalDied;

    FrameWindowMark mk;
    mk.frame = s.frame;
    mk.window =
        static_cast<int>(m_fine > 1 ? m_coarseWindow : m_validCount);
    if (m_fine > 1) {
        float const fineFrames = static_cast<float>(m_validCount) /
                                 static_cast<float>(m_fine);

        FWLOG(
            "[fw][click {}] CBF split, {} slots/tick: {} slots = {:.3f} "
            "frames ({:.3f} ms at {:.0f} TPS; whole-frame pass said {})",
            m_index + 1, m_fine, m_validCount, fineFrames,
            fineFrames / m_baseTps * 1000.0, m_baseTps, m_coarseWindow);

        bool const tickAligned = m_coarseWindow > 0 && m_validCount > 1 &&
                                 (m_validCount - 1) % m_fine == 0;
        if (tickAligned)
            FWLOG(
                "[fw][click {}] CBF pass adds nothing: both edges sit on tick "
                "boundaries ({} slots is exactly {} whole ticks), so the "
                "whole-frame window {} stands",
                m_index + 1, m_validCount, (m_validCount - 1) / m_fine,
                m_coarseWindow);

        if (m_validCount > 0 && !tickAligned) {
            mk.subframe = fineFrames;
            mk.cbf = true;
        } else {
            low = m_coarseLow;
            high = m_coarseHigh;
            maxNeg = m_coarseMaxNeg;
            maxPos = m_coarseMaxPos;
            nominalDied = m_coarseNominalDied;
        }
    }
    mk.low = low;
    mk.high = high;
    mk.clampedByNeighbour = m_clamped;
    int64_t const swept = maxNeg + maxPos + 1;
    int64_t const counted = m_fine > 1 && mk.cbf ? m_validCount : mk.window;
    mk.unbounded = swept > 0 && counted >= swept;

    if (!nominalDied) {
        mk.saturatedLow = maxNeg > 0 && low <= -maxNeg;
        mk.saturatedHigh = maxPos > 0 && high >= maxPos;
        if ((mk.saturatedLow || mk.saturatedHigh) && !mk.unbounded)
            FWWARN(
                "[fw][click {}] window {}..{} rests on the sweep limit ({}{}{}"
                "): that edge was never found, so {} is a floor, not the "
                "window. Raise Sweep Range to close it",
                m_index + 1, low, high, mk.saturatedLow ? "-" : "",
                mk.saturatedLow && mk.saturatedHigh ? " and " : "",
                mk.saturatedHigh ? "+" : "",
                mk.subframe > 0.f ? this->formatSubframe(mk.subframe)
                                  : std::to_string(mk.window));
    }
    mk.splitWindow = m_splitWindow;
    mk.splitShift = static_cast<int>(m_splitShift);
    mk.splitChecked = !m_bisect;

    mk.player2 = s.player2;
    mk.release = s.release;
    mk.gamemode = s.gamemode;
    mk.setupGroup =
        m_index < m_inSetupGroup.size() && m_inSetupGroup[m_index];
    if (m_index < m_setupRange.size()) {
        mk.setupLow = m_setupRange[m_index].first;
        mk.setupHigh = m_setupRange[m_index].second;
    }
    {
        float const frames =
            mk.subframe > 0.f ? mk.subframe : static_cast<float>(mk.window);
        mk.hz = frames > 0.f
                    ? std::min<int64_t>(std::llround(m_baseTps / frames),
                                        MAX_CBF_HZ)
                    : 0;
    }
    bool const provenImpossible =
        m_fine > 1 && m_validCount == 0 && this->captureDiedInSpan();
    mk.desynced = nominalDied && mk.subframe <= 0.f && !provenImpossible;
    mk.hidden = mk.desynced && this->captureDiedInSpan();
    if (mk.hidden)
        FWLOG(
            "[fw][click {}] no indicator: checkpoint sits inside the noclipped "
            "region (capture died at frame {})",
            m_index + 1, this->captureDeathInSpan());
    if (provenImpossible)
        FWLOG(
            "[fw][click {}] IMPOSSIBLE: no alignment survives on the frame "
            "grid or at {} slots per tick across +/-{} ticks",
            m_index + 1, m_fine, maxNeg / std::max<int64_t>(1, m_fine));
    mk.position = s.position;
    mk.percent = std::clamp(s.position.x / levelLen * 100.f, 0.f, 100.f);

    return mk;
}

void FrameWindowAnalyzer::finishClick() {
    if (!m_entryActive && this->startSubframeProbe()) return;

    FrameWindowMark mk = this->buildMark();

    if (m_desyncPending) {
        if (!mk.desynced) m_desynced = std::max(0, m_desynced - 1);
        m_desyncPending = false;
    }

    if (!m_entryActive && !m_entryDone && m_index < m_sampleBand.size())
        m_sampleBand[m_index] = {
            static_cast<int>(m_fine > 1 ? m_coarseLow : mk.low),
            static_cast<int>(m_fine > 1 ? m_coarseHigh : mk.high)};

    if (this->advanceEntryPass(mk)) return;

    if (m_entryDone) {
        mk = m_entryMark;
        if (m_entryMin != m_entryMax) {
            mk.setupLow = m_entryMin;
            mk.setupHigh = m_entryMax;
            FWLOG(
                "[fw][entry] click {}: window is {}..{} frames depending on "
                "how the previous input is timed{}",
                m_index + 1, m_entryMin, m_entryMax,
                m_entryFailed ? " (one entry fails outright)" : "");
        } else if (m_entryFailed) {
            FWLOG(
                "[fw][entry] click {}: window stays {} -- the other entry "
                "fails outright rather than narrowing it",
                m_index + 1, m_entryMin);
        }
    }

    if (m_fine <= 1 || mk.cbf) {
        std::vector<int64_t> holes;
        for (int64_t dead : m_deadShifts)
            if (dead > mk.low && dead < mk.high) holes.push_back(dead);

        if (!holes.empty()) {
            std::sort(holes.begin(), holes.end());
            std::string list;
            for (size_t i = 0; i < holes.size() && i < 8; i++)
                list += fmt::format("{}{:+d}", i ? ", " : "", holes[i]);
            if (holes.size() > 8) list += ", ...";

            mk.solid = false;
            mk.holes = static_cast<int>(holes.size());

            int64_t runLo = mk.low;
            int64_t runHi = mk.high;
            for (int64_t h : holes) {
                if (h < 0 && h + 1 > runLo) runLo = h + 1;
                if (h > 0 && h - 1 < runHi) runHi = h - 1;
            }

            FWWARN(
                "[fw][click {}] window {}..{} is NOT SOLID: {} sampled "
                "offset(s) inside it died ({}). Reported span {} is an upper "
                "bound; the unbroken run around the macro's timing is at most "
                "{}..{} = {} slots. The walk only bisects the edges, so hole "
                "widths are unknown",
                m_index + 1, mk.low, mk.high, holes.size(), list,
                mk.high - mk.low + 1, runLo, runHi, runHi - runLo + 1);
        }
    }


    int const bufferSurvivors =
        m_fine > 1 ? m_coarseBufferSurvivors : m_bufferSurvivors;

    mk.bufferAssisted = bufferSurvivors > 0;
    if (bufferSurvivors > 0) {
        char const gm = m_samples[m_index].gamemode;
        bool const heldThrust = gm == 'R' || gm == 'H' || gm == 'W' ||
                                gm == 'V' || gm == 'U';
        FWWARN(
            "[fw][click {}] {} offset(s) in this window survived only as an "
            "isolated shift -- this input moved on its own, with the rest of "
            "the macro left at the timing it was recorded on{}",
            m_index + 1, bufferSurvivors,
            heldThrust
                ? fmt::format(
                      ". Gamemode {} holds for sustained thrust, so check "
                      "these by eye: the hold keeps its recorded length, but "
                      "where it sits still changes what it thrusts through",
                      gm)
                : "");
    }

    FWLOG(
        "[fw][click {}] RESULT window={} low={} high={} clamped={} desync={} "
        "subframe={:.3f} solid={} holes={} split={} splitAt={:+d} satLow={} satHigh={} unbounded={} "
        "cbf={} fine={} gm={} bufAsst={} hz={} pos=({:.1f},{:.1f}) {:.2f}% "
        "-- trust={}",
        m_index + 1, mk.window, mk.low, mk.high, mk.clampedByNeighbour,
        mk.desynced, mk.subframe, mk.solid, mk.holes,
        mk.splitChecked ? (mk.splitWindow ? "true" : "false") : "unchecked",
        mk.splitShift, mk.saturatedLow,
        mk.saturatedHigh, mk.unbounded, mk.cbf, m_fine, mk.gamemode,
        mk.bufferAssisted, mk.hz, mk.position.x, mk.position.y, mk.percent,
        mk.desynced          ? "none (desynced)"
        : mk.window <= 0 && !(mk.subframe > 0.f)
                             ? "IMPOSSIBLE (nothing survives at any offset, "
                               "whole-frame or sub-tick)"
        : mk.splitWindow     ? "MACRO-RELATIVE (input also works outside this "
                               "band -- macro may sit in a pocket)"
        : !mk.splitChecked   ? "bisected (band assumed contiguous; a gap would "
                               "not have been seen)"
        : !mk.solid          ? "UPPER BOUND (band has holes)"
        : (mk.saturatedLow || mk.saturatedHigh)
                             ? "FLOOR (edge on sweep limit)"
        : mk.bufferAssisted  ? "buffer-assisted (input moved alone)"
                             : "exact to sampling resolution");

    if (mk.window <= 0) m_noclipNextAdvance = true;

    m_results.push_back(mk);
    if (mk.desynced)
        m_skipped++;
    else
        m_measured++;

    this->nextClick();
}

bool FrameWindowAnalyzer::advanceEntryPass(FrameWindowMark const& mk) {
    if (!m_entrySweep->inner()) return false;

    int const window =
        static_cast<int>(m_fine > 1 ? m_coarseWindow : m_validCount);

    if (!m_entryActive) {
        if (m_entryDone) return false;
        if (m_index == 0) return false;
        if (mk.desynced) return false;
        if (window <= 0) return false;
        if (window > std::max(0, m_tightThreshold->inner())) return false;

        size_t const prev = m_index - 1;
        if (prev >= m_sampleBand.size()) return false;
        if (m_samples[prev].player2 != m_samples[m_index].player2) return false;

        int const lo = std::clamp(m_sampleBand[prev].first, -m_sweep, 0);
        int const hi = std::clamp(m_sampleBand[prev].second, 0, m_sweep);

        m_entryQueue.clear();
        if (lo < 0) m_entryQueue.push_back(lo);
        if (hi > 0) m_entryQueue.push_back(hi);
        if (m_entryQueue.empty()) return false;

        m_entryPrevAi = m_samples[prev].actionIndex;

        int64_t const back =
            static_cast<int64_t>(m_samples[prev].frame) + lo - 1;
        m_entryBranch = static_cast<uint32_t>(std::max<int64_t>(back, 0));

        m_entryMark = mk;
        m_entryMin = window;
        m_entryMax = window;
        m_entryFailed = false;
        m_entryActive = true;

        FWLOG(
            "[fw][entry] click {}: window {} -- re-testing with the previous "
            "input pinned at {:+d} and {:+d} (branch {})",
            m_index + 1, window, lo, hi, m_entryBranch);
    } else if (window > 0) {
        m_entryMin = std::min(m_entryMin, window);
        m_entryMax = std::max(m_entryMax, window);
        FWLOG("[fw][entry] click {}: previous at {:+d} gives window {}",
              m_index + 1, m_entryOffset, window);
    } else {
        m_entryFailed = true;
        FWLOG(
            "[fw][entry] click {}: previous at {:+d} makes this input "
            "impossible -- left out of the span",
            m_index + 1, m_entryOffset);
    }

    if (m_entryQueue.empty()) {
        m_entryActive = false;
        m_entryDone = true;
        m_entryOffset = 0;
        return false;
    }

    m_entryOffset = m_entryQueue.back();
    m_entryQueue.pop_back();

    m_phase = Phase::Nominal;
    m_shift = 0;
    m_low = 0;
    m_high = 0;
    m_validCount = 0;
    m_deadShifts.clear();
    m_bufferSurvivors = 0;
    m_bisect = false;
    m_scanning = false;
    m_islandSeed = 0;
    m_negCounting = true;
    m_posCounting = true;
    m_nominalDied = false;
    m_deadRun = 0;
    m_splitWindow = false;
    m_splitShift = 0;
    m_forceFullSweep = false;
    m_fine = 1;
    m_coarseWindow = 0;
    m_coarseBufferSurvivors = 0;
    m_tickBuffered.clear();
    m_fineCrossedNeighbour = false;
    m_testActive = false;
    m_recoveryActive = false;
    m_bufferTried = false;
    m_maxNeg = std::min<int64_t>(m_maxNeg, m_sweep);
    m_maxPos = std::min<int64_t>(m_maxPos, m_sweep);
    cbf::Engine::get()->disarm();

    m_branchFrame = m_entryBranch;
    this->releaseCheckpoint();
    m_stageId = Stage::Rewind;
    return true;
}

void FrameWindowAnalyzer::abortClick(char const* why) {
    FWWARN("[fw][click {}] NOT MEASURED @ frame {}: {}", m_index + 1,
              m_recorded, why);
    m_skipped++;
    this->nextClick();
}

void FrameWindowAnalyzer::nextClick() {
    m_index++;
    if (m_index >= m_samples.size()) {
        std::string msg =
            fmt::format("Measured {} of {} inputs.", m_measured,
                        m_measured + m_skipped);
        if (m_skipped > 0)
            msg += fmt::format(" {} could not be measured.", m_skipped);
        if (m_filtered > 0)
            msg += fmt::format(" {} were filtered out before probing.",
                               m_filtered);
        this->finish(std::move(msg), true);
        return;
    }
    this->beginClick();
    m_stageId = Stage::Rewind;
}

void FrameWindowAnalyzer::finish(std::string message, bool ok) {
    auto bot = Bot::get();
    auto& updater = bot->updater();
    auto& rs = bot->replaySystem();
    auto& pf = bot->practiceFix();

    {
        int notSolid = 0, saturated = 0, unbounded = 0, desynced = 0,
            bufferAssisted = 0, cbf = 0, split = 0;
        for (auto const& mk : m_results) {
            if (mk.desynced) { desynced++; continue; }
            if (!mk.solid) notSolid++;
            if (mk.saturatedLow || mk.saturatedHigh) saturated++;
            if (mk.unbounded) unbounded++;
            if (mk.bufferAssisted) bufferAssisted++;
            if (mk.splitWindow) split++;
            if (mk.cbf) cbf++;
        }
        auto const& lg = [&](int n) { return n ? "WARN" : "ok"; };
        // more logging so when others playtest, I can debug easier okok although logs become MASSIVE on cbf counts :sob:               
        log::info(
            "[fw][audit] {} marks: {} desynced, {} not solid [{}], {} on the "
            "sweep limit [{}], {} split [{}], {} unbounded, {} buffer-assisted, "
            "{} sub-tick measured. Fine legs crossed a neighbouring input: {} [{}]. "
            "stepBatch={} inputHz={} sweep={} horizon={} slack={} tps={:.0f}",
            m_results.size(), desynced, notSolid, lg(notSolid), saturated,
            lg(saturated), split, lg(split), unbounded, bufferAssisted, cbf,
            m_fineCrossedNeighbour, lg(m_fineCrossedNeighbour ? 1 : 0),
            m_stepBatch->inner(), m_cbfInputHz->inner(), m_sweep, m_horizon,
            m_slack_, m_baseTps);
    }

    auto& actions = rs.m_actionAtom.m_actions;
    if (actions.size() == m_originalFrames.size()) {
        for (size_t i = 0; i < actions.size(); i++)
            actions[i].m_frame = m_originalFrames[i];
    }

    this->releaseCheckpoint();

    pf.clearStoredFrames();
    pf.removeAll();

    if (auto* pl = PlayLayer::get()) {
        pl->resetLevel();
        rs.onReset(0);
    }

    m_fine = 1;
    cbf::Engine::get()->disarm();

    updater.m_canDie = m_savedCanDie;
    updater.m_expectsDeath = m_savedExpectsDeath;
    updater.m_predicting = false;
    updater.setPaused(m_wasPaused);
    this->unmuteAudio();

    bot->trailBuffer().loadSamples(m_trailP1, m_trailP2);
    m_trailP1.clear();
    m_trailP2.clear();

    log::info(
        "[fw][finish] ok={} measured={} skipped={} desynced={} legs={} msg={}",
        ok, m_measured, m_skipped, m_desynced, m_legCounter, message);

    if (m_stateDiffRestores > 0) {
        log::info(
            "[fw][statediff] {} restores checked: {} matched byte for byte, "
            "{} differed only in cached node fields, {} had a real difference",
            m_stateDiffRestores,
            m_stateDiffRestores - m_stateDiffCachedOnly - m_stateDiffReal,
            m_stateDiffCachedOnly, m_stateDiffReal);
    }
    m_stateDiffRestores = 0;
    m_stateDiffCachedOnly = 0;
    m_stateDiffReal = 0;

    if (m_stepCount > 0) {
        double const ms = static_cast<double>(m_stepNanos) / 1'000'000.0;
        log::info(
            "[fw][perf] {} ticks in {:.0f}ms stepping ({:.1f}us/tick, batch={})",
            m_stepCount, ms,
            static_cast<double>(m_stepNanos) / 1000.0 /
                static_cast<double>(m_stepCount),
            std::clamp(m_stepBatch->inner(), 1, 64));
    }
    m_stepNanos = 0;
    m_stepCount = 0;

    lstar::Solver::get()->markDirty();

    m_running = false;
    m_stageId = Stage::Idle;
    m_stage = "idle";
    this->resetDisplay();
    m_status = std::move(message);
    m_generation++;

    if (!ok) FWWARN("[FrameWindow] {}", m_status);
}

void FrameWindowAnalyzer::cancel() {
    if (!m_running) return;
    this->finish("Cancelled.", false);
}

void FrameWindowAnalyzer::tick(PlayLayer* pl) {
    if (!m_running) return;

    if (!pl) {
        this->finish("Left the level.", false);
        return;
    }

    auto bot = Bot::get();
    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();

    auto const now = Clock::now();
    int64_t budgetMs = std::max(1, m_budgetMs->inner());
    if (m_adaptiveBudget->inner() && m_haveLastTickCall) {
        double const elapsedMs =
            std::chrono::duration<double, std::milli>(now - m_lastTickCall)
                .count();
        double const share =
            std::clamp(m_budgetSharePercent->inner(), 1, 90) / 100.0;
        int64_t const ceiling =
            std::max<int64_t>(budgetMs, std::max(1, m_maxBudgetMs->inner()));
        budgetMs = std::clamp<int64_t>(
            static_cast<int64_t>(elapsedMs * share), budgetMs, ceiling);
    }
    m_lastTickCall = now;
    m_haveLastTickCall = true;

    g_deadline = now + std::chrono::milliseconds(budgetMs);

    while (true) {
        if (Clock::now() >= g_deadline) return;

        if (m_restoreFailed) {
            this->finish("Checkpoint restore was bypassed. Stopping.", false);
            return;
        }

        switch (m_stageId) {
            case Stage::Capture: {
                m_noclip = m_captureNoclip;


                uint32_t const captureEnd =
                    m_samples.empty()
                        ? 0u
                        : m_samples.back().frame + 1u +
                              static_cast<uint32_t>(std::max(0, m_horizon));

                if (m_index >= m_samples.size() &&
                    updater.getFrame() >= captureEnd) {
                    this->applyOrbAwareSkip();

                    if (m_captureDeathFrame != UINT32_MAX) {
                        bool const hitchDuringSession =
                            updater.m_droppedTimeFrame != UINT32_MAX;
                        bool const confirmedBad =
                            hitchDuringSession || m_trailDiverged;

                        if (confirmedBad) {
                            size_t const before = m_samples.size();
                            std::erase_if(m_samples, [this](Sample const& s) {
                                return s.frame >= m_captureDeathFrame;
                            });
                            size_t const dropped = before - m_samples.size();
                            m_filtered += static_cast<int>(dropped);
                            if (dropped > 0)
                                FWWARN(
                                    "[fw][capture] dropping the {} input(s) "
                                    "recorded past frame {}: {}",
                                    dropped, m_captureDeathFrame,
                                    m_trailDiverged
                                        ? "the capture left the macro's own "
                                          "recorded path there, so nothing "
                                          "past that point can be reproduced"
                                        : "a frame hitch skipped game time "
                                          "during this session, which makes "
                                          "the recording after it a different "
                                          "run from a clean replay");
                        } else {
                            log::info(
                                "[fw][capture] the replay died at frame {} "
                                "and continued on noclip -- this is routine "
                                "for an input that only survives off the "
                                "frame grid, so the {} sample(s) recorded "
                                "after it are kept and probed normally",
                                m_captureDeathFrame,
                                std::count_if(
                                    m_samples.begin(), m_samples.end(),
                                    [this](Sample const& s) {
                                        return s.frame >= m_captureDeathFrame;
                                    }));
                        }

                        if (hitchDuringSession)
                            FWWARN(
                                "[fw][capture] a frame hitch skipped game time "
                                "at frame {} in this session. A macro recorded "
                                "across a hitch replays as a different run "
                                "-- re-record it on a quiet frame rate before "
                                "trusting anything measured here",
                                updater.m_droppedTimeFrame);
                    }

                    if (m_samples.empty()) {
                        this->finish("No measurable inputs after filtering.",
                                     false);
                        return;
                    }
                    m_noclip = false;
                    m_total = m_samples.size();
                    m_index = 0;
                    m_stage = "probe";
                    m_status = "Probing";

                    FWLOG("[fw][capture] DONE at frame {} -- {} inputs to probe",
                          updater.getFrame(), m_total);

                    this->detectSetupGroups();

                    pf.clearStoredFrames();
                    pf.removeAll();
                    pl->resetLevel();
                    bot->replaySystem().onReset(0);

                    this->beginClick();
                    m_stageId = Stage::Rewind;
                    continue;
                }


                uint32_t const last = captureEnd;
                auto const r = this->stepToward(pl, last);
                if (r == StepResult::OutOfBudget) return;
                if (r == StepResult::Died) {
                    uint32_t const deathFrame = updater.getFrame();
                    this->noteCaptureDeath(deathFrame);

                    if (!m_captureNoclip) {
                        m_captureNoclip = true;
                        m_captureDeathFrame = deathFrame;
                        m_noclip = true;
                        FWWARN(
                            "[fw][capture] DIED at frame {} after capturing "
                            "{}/{} samples -- continuing through it with "
                            "noclip",
                            deathFrame, m_index, m_samples.size());
                    }
                    continue;
                }
                if (r == StepResult::Reached && m_index < m_samples.size()) {
                    FWLOG(
                        "[fw][capture] STALLED: reached target {} at frame {} "
                        "but only {}/{} samples captured (next at {})",
                        last, updater.getFrame(), m_index, m_samples.size(),
                        m_samples[m_index].frame);
                    this->finish(
                        fmt::format(
                            "Capture stopped at frame {} with {} of {} inputs "
                            "recorded.",
                            updater.getFrame(), m_index, m_samples.size()),
                        false);
                    return;
                }
                continue;
            }

            case Stage::Advance: {
                m_noclip = m_noclipNextAdvance;
                auto const r = this->stepToward(pl, m_branchFrame);
                m_noclip = false;
                if (r != StepResult::OutOfBudget) m_noclipNextAdvance = false;
                if (r == StepResult::OutOfBudget) return;
                if (r == StepResult::Reached && m_pathDiverged) {
                    if (!m_triedHardReset && !m_resyncGaveUp) {
                        m_triedHardReset = true;
                        FWWARN(
                            "[fw][click {}] the advance to branch {} left the "
                            "captured path -- the checkpoint carried over from "
                            "an earlier click no longer reproduces the "
                            "capture. Replaying from frame 0 to re-sync",
                            m_index + 1, m_branchFrame);
                        this->hardResetToStart();
                        continue;
                    }
                    if (m_triedHardReset) {
                        m_resyncFailures++;
                        if (m_resyncFailures >= MAX_RESYNC_FAILURES &&
                            !m_resyncGaveUp) {
                            m_resyncGaveUp = true;
                            FWWARN(
                                "[fw][click {}] a clean replay from frame 0 "
                                "still does not reproduce the capture. The "
                                "run itself is not deterministic here, so "
                                "re-syncing is off for the rest of this pass "
                                "-- every remaining click that diverges is "
                                "reported as desynced instead",
                                m_index + 1);
                        }
                    }
                }
                if (r == StepResult::Died) {
                    if (!m_triedHardReset) {
                        m_triedHardReset = true;
                        FWLOG(
                            "[fw][click {}] could not reach branch {} from the "
                            "checkpoint -- replaying from frame 0",
                            m_index + 1, m_branchFrame);
                        this->hardResetToStart();
                        continue;
                    }
                    this->abortClick("died before reaching the branch point");
                    this->finish(
                        fmt::format("Measured {} of {} inputs. The macro "
                                    "cannot be replayed past frame {}.",
                                    m_measured, m_samples.size(),
                                    updater.getFrame()),
                        true);
                    return;
                }
                if (m_triedHardReset && !m_pathDiverged) m_resyncFailures = 0;
                m_triedHardReset = false;
                FWLOG("[fw][click {}] advanced to branch {} (frame now {})",
                      m_index + 1, m_branchFrame, updater.getFrame());
                m_stageId = Stage::Snapshot;
                continue;
            }

            case Stage::Rewind: {
                if (m_haveCheckpoint) {
                    if (!this->restoreToBranch()) continue;
                }

                if (updater.getFrame() > m_branchFrame) {
                    FWLOG(
                        "[fw][click {}] branch {} is behind the current frame "
                        "{} -- replaying from frame 0 to reach it",
                        m_index + 1, m_branchFrame, updater.getFrame());
                    this->hardResetToStart();
                }

                m_stageId = Stage::Advance;
                continue;
            }

            case Stage::Snapshot: {
                this->releaseCheckpoint();

                logPlayerState("at-snapshot", pl->m_player1);

                m_cpObject = pl->createCheckpoint();
                if (!m_cpObject) {
                    this->finish("Could not create a checkpoint.", false);
                    return;
                }
                m_cpObject->retain();
                // Upstream passes m_frameOnLastAttempt here because Silicate's
                // SavedCheckpoint carries TWO frame fields: m_attemptStartFrame
                // (this argument) and m_frame, which it fills itself from
                // getFrame(). GucciBot's SavedCheckpointState has only one,
                // m_frameOffset, and fills it FROM this argument -- and that
                // single field is what the restore path feeds back into
                // m_frameOnLastAttempt. Passing the attempt start therefore
                // restored every leg to frame 0: right position, wrong clock,
                // so each leg died within ~18 frames and every window measured
                // 0 with "IMPOSSIBLE". Pass the frame the snapshot is actually
                // taken at, which is what GucciBot's field means.
                m_saved = pf.createCheckpoint(m_cpObject, updater.getFrame());
                m_haveCheckpoint = true;
                this->captureStateBytes(pl);

                FWLOG(
                    "[fw][click {}] snapshot at frame {} (checkpoint frame {}, "
                    "attemptStart {})",
                    m_index + 1, updater.getFrame(), m_saved.m_frameOffset,
                    updater.m_frameOnLastAttempt);

                while (m_setupGroupCursor < m_setupGroups.size() &&
                       m_setupGroups[m_setupGroupCursor].first < m_index)
                    m_setupGroupCursor++;

                if (m_setupGroupCursor < m_setupGroups.size() &&
                    m_setupGroups[m_setupGroupCursor].first == m_index) {
                    this->beginSetupGroup(m_setupGroups[m_setupGroupCursor]);
                    m_stageId = Stage::ResolveSetup;
                    continue;
                }

                this->beginShift(0);
                continue;
            }

            case Stage::ResolveSetup: {
                auto const& sg = m_setupGroups[m_setupGroupCursor];

                if (m_setupLegActive) {
                    auto const r = this->stepToward(pl, m_setupLegTarget);
                    if (r == StepResult::OutOfBudget) return;
                    bool const survived = r != StepResult::Died;
                    m_setupLegActive = false;

                    if (survived) {
                        m_setupSurvivors.emplace_back(m_setupLeaderShift,
                                                      m_setupCombo);
                        int deviation = std::abs(m_setupLeaderShift);
                        for (int o : m_setupCombo) deviation += std::abs(o);
                        if (deviation < m_setupBestDeviation) {
                            m_setupBestFound = true;
                            m_setupBestDeviation = deviation;
                            m_setupBestLeaderShift = m_setupLeaderShift;
                            m_setupBestCombo = m_setupCombo;
                        }
                    }

                    if (!this->advanceSetupCombo(sg)) {
                        this->commitSetupGroup(sg);
                        m_setupGroupCursor++;
                        this->beginClick();
                        m_stageId = Stage::Rewind;
                    }
                    continue;
                }

                this->beginSetupLeg(sg);
                continue;
            }

            case Stage::Probe:
            case Stage::Recover: {
                if (!m_testActive) {
                    m_stageId = Stage::Rewind;
                    continue;
                }

                auto const r = this->stepToward(pl, m_testTarget);
                if (r == StepResult::OutOfBudget) return;

                this->concludeShift(r != StepResult::Died);
                continue;
            }

            case Stage::Finish:
            case Stage::Idle:
            default:
                return;
        }
    }
}

std::string FrameWindowAnalyzer::formatWindow(int window) {
    if (window <= 0) return "X";
    return fmt::format("{}", window);
}

FrameWindowTier const* FrameWindowAnalyzer::tierFor(int window) const {
    auto const& tiers = SLSettings::get()->frameWindow.tiers;
    for (auto const& t : tiers) {
        if (window >= t.minWindow && window <= t.maxWindow) return &t;
    }
    return nullptr;
}

namespace fwstore {
struct StoredMark {
    uint32_t frame = 0;
    int window = 0;
    int64_t low = 0;
    int64_t high = 0;
    bool clampedByNeighbour = false;
    bool unbounded = false;
    float subframe = 0.f;
    bool hidden = false;
    bool player2 = false;
    bool release = false;
    bool desynced = false;
    float percent = 0.f;
    bool cbf = false;
    bool bufferAssisted = false;
    bool setupGroup = false;
    char gamemode = 'C';
    int setupLow = 0;
    int setupHigh = 0;
    int64_t hz = 0;
    bool solid = true;
    int holes = 0;
    bool saturatedLow = false;
    bool saturatedHigh = false;
    bool splitWindow = false;
    int splitShift = 0;
    bool splitChecked = false;
    float x = 0.f;
    float y = 0.f;
};

struct StoredMessage {
    std::string text;
    int startIndex = 0;
    int span = 1;
    float scale = 0.6f;
    float offsetY = 60.f;
    bool enabled = true;
};

struct StoredResults {
    int version = 1;
    double tps = 240.0;
    std::string level;
    std::string replay;
    int measured = 0;
    int skipped = 0;
    int desynced = 0;
    std::vector<StoredMark> marks;
    std::vector<StoredMessage> messages;
};
}  // namespace fwstore

namespace fwstore {

// Upstream serialised these with glaze, which works by reflection. matjson is
// Geode's own and already a GucciBot dependency, so it is used here instead of
// pulling in another library -- but it needs every field named. That makes the
// mapping below load-bearing: a field added to the structs above and not added
// here is dropped silently on save. Keep the two in step.

static matjson::Value markToJson(StoredMark const& m) {
    return matjson::makeObject({
        {"frame", (int64_t)m.frame},
        {"window", (int64_t)m.window},
        {"low", m.low},
        {"high", m.high},
        {"clampedByNeighbour", m.clampedByNeighbour},
        {"unbounded", m.unbounded},
        {"subframe", (double)m.subframe},
        {"hidden", m.hidden},
        {"player2", m.player2},
        {"release", m.release},
        {"desynced", m.desynced},
        {"percent", (double)m.percent},
        {"cbf", m.cbf},
        {"bufferAssisted", m.bufferAssisted},
        {"setupGroup", m.setupGroup},
        {"gamemode", std::string(1, m.gamemode)},
        {"setupLow", (int64_t)m.setupLow},
        {"setupHigh", (int64_t)m.setupHigh},
        {"hz", m.hz},
        {"solid", m.solid},
        {"holes", (int64_t)m.holes},
        {"saturatedLow", m.saturatedLow},
        {"saturatedHigh", m.saturatedHigh},
        {"splitWindow", m.splitWindow},
        {"splitShift", (int64_t)m.splitShift},
        {"splitChecked", m.splitChecked},
        {"x", (double)m.x},
        {"y", (double)m.y},
    });
}

static matjson::Value messageToJson(StoredMessage const& m) {
    return matjson::makeObject({
        {"text", m.text},
        {"startIndex", (int64_t)m.startIndex},
        {"span", (int64_t)m.span},
        {"scale", (double)m.scale},
        {"offsetY", (double)m.offsetY},
        {"enabled", m.enabled},
    });
}

static std::string toJson(StoredResults const& r) {
    std::vector<matjson::Value> marks;
    marks.reserve(r.marks.size());
    for (auto const& m : r.marks) marks.push_back(markToJson(m));

    std::vector<matjson::Value> messages;
    messages.reserve(r.messages.size());
    for (auto const& m : r.messages) messages.push_back(messageToJson(m));

    return matjson::makeObject({
               {"version", (int64_t)r.version},
               {"tps", r.tps},
               {"level", r.level},
               {"replay", r.replay},
               {"measured", (int64_t)r.measured},
               {"skipped", (int64_t)r.skipped},
               {"desynced", (int64_t)r.desynced},
               {"marks", matjson::Value(marks)},
               {"messages", matjson::Value(messages)},
           })
        .dump();
}

// Every getter falls back to the struct's own default, so a sidecar written by
// an older build still loads with the fields it does have.
static int64_t jnum(matjson::Value const& v, std::string_view k, int64_t def) {
    auto r = v[k].as<int64_t>();
    return r.isOk() ? r.unwrap() : def;
}

static double jreal(matjson::Value const& v, std::string_view k, double def) {
    auto r = v[k].as<double>();
    return r.isOk() ? r.unwrap() : def;
}

static bool jbool(matjson::Value const& v, std::string_view k, bool def) {
    auto r = v[k].as<bool>();
    return r.isOk() ? r.unwrap() : def;
}

static std::string jstr(matjson::Value const& v, std::string_view k,
                        std::string def) {
    auto r = v[k].as<std::string>();
    return r.isOk() ? r.unwrap() : def;
}

static bool fromJson(std::string const& text, StoredResults& out) {
    auto parsed = matjson::parse(text);
    if (!parsed.isOk()) return false;
    auto const root = parsed.unwrap();
    if (!root.isObject()) return false;

    StoredResults d;
    out.version = (int)jnum(root, "version", d.version);
    out.tps = jreal(root, "tps", d.tps);
    out.level = jstr(root, "level", d.level);
    out.replay = jstr(root, "replay", d.replay);
    out.measured = (int)jnum(root, "measured", d.measured);
    out.skipped = (int)jnum(root, "skipped", d.skipped);
    out.desynced = (int)jnum(root, "desynced", d.desynced);

    auto const marks = root["marks"];
    if (marks.isArray()) {
        out.marks.reserve(marks.size());
        for (size_t i = 0; i < marks.size(); i++) {
            auto const& j = marks[i];
            StoredMark m;
            m.frame = (uint32_t)jnum(j, "frame", m.frame);
            m.window = (int)jnum(j, "window", m.window);
            m.low = jnum(j, "low", m.low);
            m.high = jnum(j, "high", m.high);
            m.clampedByNeighbour =
                jbool(j, "clampedByNeighbour", m.clampedByNeighbour);
            m.unbounded = jbool(j, "unbounded", m.unbounded);
            m.subframe = (float)jreal(j, "subframe", m.subframe);
            m.hidden = jbool(j, "hidden", m.hidden);
            m.player2 = jbool(j, "player2", m.player2);
            m.release = jbool(j, "release", m.release);
            m.desynced = jbool(j, "desynced", m.desynced);
            m.percent = (float)jreal(j, "percent", m.percent);
            m.cbf = jbool(j, "cbf", m.cbf);
            m.bufferAssisted = jbool(j, "bufferAssisted", m.bufferAssisted);
            m.setupGroup = jbool(j, "setupGroup", m.setupGroup);
            auto const gm = jstr(j, "gamemode", std::string(1, m.gamemode));
            if (!gm.empty()) m.gamemode = gm[0];
            m.setupLow = (int)jnum(j, "setupLow", m.setupLow);
            m.setupHigh = (int)jnum(j, "setupHigh", m.setupHigh);
            m.hz = jnum(j, "hz", m.hz);
            m.solid = jbool(j, "solid", m.solid);
            m.holes = (int)jnum(j, "holes", m.holes);
            m.saturatedLow = jbool(j, "saturatedLow", m.saturatedLow);
            m.saturatedHigh = jbool(j, "saturatedHigh", m.saturatedHigh);
            m.splitWindow = jbool(j, "splitWindow", m.splitWindow);
            m.splitShift = (int)jnum(j, "splitShift", m.splitShift);
            m.splitChecked = jbool(j, "splitChecked", m.splitChecked);
            m.x = (float)jreal(j, "x", m.x);
            m.y = (float)jreal(j, "y", m.y);
            out.marks.push_back(m);
        }
    }

    auto const messages = root["messages"];
    if (messages.isArray()) {
        out.messages.reserve(messages.size());
        for (size_t i = 0; i < messages.size(); i++) {
            auto const& j = messages[i];
            StoredMessage m;
            m.text = jstr(j, "text", m.text);
            m.startIndex = (int)jnum(j, "startIndex", m.startIndex);
            m.span = (int)jnum(j, "span", m.span);
            m.scale = (float)jreal(j, "scale", m.scale);
            m.offsetY = (float)jreal(j, "offsetY", m.offsetY);
            m.enabled = jbool(j, "enabled", m.enabled);
            out.messages.push_back(m);
        }
    }

    return true;
}

}  // namespace fwstore

bool FrameWindowAnalyzer::saveResults(std::filesystem::path const& path) const {
    fwstore::StoredResults out;
    out.tps = m_resultsTps > 0.0 ? m_resultsTps : 240.0;
    out.measured = m_measured;
    out.skipped = m_skipped;
    out.desynced = m_desynced;

    if (auto* pl = PlayLayer::get(); pl && pl->m_level)
        out.level = pl->m_level->m_levelName;
    out.replay = Bot::get()->replaySystem().m_replayName;

    out.marks.reserve(m_results.size());
    for (auto const& mk : m_results) {
        out.marks.push_back({mk.frame, mk.window, mk.low, mk.high,
                             mk.clampedByNeighbour, mk.unbounded, mk.subframe,
                             mk.hidden, mk.player2, mk.release, mk.desynced,
                             mk.percent, mk.cbf, mk.bufferAssisted,
                             mk.setupGroup, mk.gamemode, mk.setupLow,
                             mk.setupHigh, mk.hz, mk.solid, mk.holes,
                             mk.saturatedLow, mk.saturatedHigh,
                             mk.splitWindow, mk.splitShift, mk.splitChecked,
                             mk.position.x, mk.position.y});
    }

    out.messages.reserve(m_messages.size());
    for (auto const& m : m_messages)
        out.messages.push_back({m.text, m.startIndex, m.span, m.scale,
                                m.offsetY, m.enabled});

    auto const serialized = fwstore::toJson(out);
    if (serialized.empty()) return false;

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream fd(path);
    if (!fd) return false;
    fd << serialized;
    return true;
}

bool FrameWindowAnalyzer::loadResults(std::filesystem::path const& path) {
    std::ifstream fd(path);
    if (!fd) return false;

    std::string data((std::istreambuf_iterator<char>(fd)),
                     std::istreambuf_iterator<char>());

    fwstore::StoredResults in;
    if (!fwstore::fromJson(data, in)) return false;

    m_results.clear();
    m_results.reserve(in.marks.size());
    for (auto const& sm : in.marks) {
        FrameWindowMark mk;
        mk.frame = sm.frame;
        mk.window = sm.window;
        mk.low = sm.low;
        mk.high = sm.high;
        mk.clampedByNeighbour = sm.clampedByNeighbour;
        mk.unbounded = sm.unbounded;
        mk.subframe = sm.subframe;
        mk.hidden = sm.hidden;
        mk.player2 = sm.player2;
        mk.release = sm.release;
        mk.desynced = sm.desynced;
        mk.percent = sm.percent;
        mk.cbf = sm.cbf;
        mk.bufferAssisted = sm.bufferAssisted;
        mk.solid = sm.solid;
        mk.holes = sm.holes;
        mk.saturatedLow = sm.saturatedLow;
        mk.saturatedHigh = sm.saturatedHigh;
        mk.splitWindow = sm.splitWindow;
        mk.splitShift = sm.splitShift;
        mk.splitChecked = sm.splitChecked;
        mk.setupGroup = sm.setupGroup;
        mk.gamemode = sm.gamemode;
        mk.setupLow = sm.setupLow;
        mk.setupHigh = sm.setupHigh;
        mk.hz = sm.hz;
        mk.position = CCPoint{sm.x, sm.y};
        m_results.push_back(mk);
    }

    m_messages.clear();
    m_messages.reserve(in.messages.size());
    for (auto const& sm : in.messages)
        m_messages.push_back({sm.text, sm.startIndex, sm.span, sm.scale,
                              sm.offsetY, sm.enabled});

    lstar::Solver::get()->markDirty();
    m_measured = in.measured;
    m_skipped = in.skipped;
    m_desynced = in.desynced;
    m_resultsTps = in.tps > 0.0 ? in.tps : 240.0;
    m_generation++;
    this->resetDisplay();
    return true;
}

std::string FrameWindowAnalyzer::decorate(FrameWindowMark const& mk,
                                          std::string text) const {
    if (!m_markSetupVarying->inner()) return text;
    if (mk.setupGroup) return text + "~";
    if (this->setupVarying(mk)) return text + "^";
    return text;
}

std::string FrameWindowAnalyzer::formatSubframe(float frames) const {
    int const places = std::clamp(m_subframeDecimals->inner(), 0, 4);
    return fmt::format("{:.{}f}", frames, places);
}

void FrameWindowAnalyzer::refreshTiming(PlayLayer* pl) {
    FrameWindowMark const* mk = nullptr;
    if (m_timingMark >= 0 && m_timingMark < static_cast<int>(m_results.size()))
        mk = &m_results[m_timingMark];
    this->updateTiming(pl, mk);
}

void FrameWindowAnalyzer::updateTiming(PlayLayer* pl,
                                       FrameWindowMark const* mk) {
    auto* msNode = pl->m_uiLayer->getChildByID("framewindow-timing"_spr);
    auto* cbfNode = pl->m_uiLayer->getChildByID("framewindow-cbf"_spr);
    auto* rangeNode = pl->m_uiLayer->getChildByID("framewindow-setup"_spr);
    auto* hzNode = pl->m_uiLayer->getChildByID("framewindow-hz"_spr);

    float const frames =
        !mk ? 0.f
            : (mk->subframe > 0.f ? mk->subframe
                                  : static_cast<float>(mk->window));
    double const tps = m_resultsTps > 0.0 ? m_resultsTps : 240.0;
    double const ms = static_cast<double>(frames) / tps * 1000.0;
    int const places = std::clamp(m_subframeDecimals->inner(), 0, 4);

    int const hzValue =
        frames > 0.f ? static_cast<int>(std::llround(tps / frames)) : 0;

    bool const cbfReadout =
        mk && mk->cbf && mk->subframe > 0.f && m_cbfWholeMarkers->inner() &&
        mk->subframe <= static_cast<float>(
                            std::max(0, m_cbfReadoutThreshold->inner()));
    bool const msReadout = mk && m_showTiming->inner();

    bool const rangeReadout = mk && m_showSetupRange->inner() &&
                              mk->setupHigh > 0 &&
                              mk->setupLow != mk->setupHigh;

    bool const hzReadout = mk && m_showHzReadout->inner() && hzValue > 0 &&
                           (cbfReadout || msReadout || rangeReadout);

    if (!hzReadout && hzNode) {
        hzNode->removeFromParent();
        hzNode = nullptr;
    }
    if (!rangeReadout && rangeNode) {
        rangeNode->removeFromParent();
        rangeNode = nullptr;
    }
    if (!cbfReadout && cbfNode) {
        cbfNode->removeFromParent();
        cbfNode = nullptr;
    }
    if (!msReadout && msNode) {
        msNode->removeFromParent();
        msNode = nullptr;
    }
    if (!mk || (!cbfReadout && !msReadout && !rangeReadout)) return;

    auto const lift = [](float v) {
        return static_cast<GLubyte>(
            std::clamp(v + (1.f - v) * 0.10f, 0.f, 1.f) * 255.f);
    };
    auto const tint = [&](FrameWindowTier const* t) {
        return t ? ccColor3B{lift(t->color[0]), lift(t->color[1]),
                             lift(t->color[2])}
                 : ccColor3B{255, 255, 255};
    };

    ccColor3B const colour = tint(this->visibleTierFor(*mk));
    ccColor3B const fineColour =
        mk->subframe > 0.f ? tint(this->tierForFrames(mk->subframe)) : colour;

    auto const win = CCDirector::get()->getWinSize();

    auto place = [&](CCNode* existing, std::string const& id,
                     std::string const& text, float top, float scale,
                     ccColor3B tint) -> CCLabelBMFont* {
        auto* label = typeinfo_cast<CCLabelBMFont*>(existing);
        if (!label) {
            label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            label->setID(id);
            label->setAnchorPoint({1.f, 1.f});
            pl->m_uiLayer->addChild(label);
        } else {
            label->setString(text.c_str());
        }
        label->setScale(scale);
        label->setPosition({win.width - 8.f, top});
        label->setColor(tint);
        return label;
    };

    float top = win.height - 8.f;

    CCLabelBMFont* cbfLabel = nullptr;
    if (cbfReadout) {
        cbfLabel = place(
            cbfNode, "framewindow-cbf"_spr,
            fmt::format("({}) w/ CBF",
                        this->decorate(
                            *mk, this->formatSubframe(mk->subframe))),
            top, 0.6f, fineColour);
        top -= cbfLabel->getScaledContentSize().height + 4.f;
    }

    CCLabelBMFont* rangeLabel = nullptr;
    if (rangeReadout) {
        rangeLabel = place(rangeNode, "framewindow-setup"_spr,
                           fmt::format("Can be from {} to {}", mk->setupLow,
                                       mk->setupHigh),
                           top, 0.6f, colour);
        top -= rangeLabel->getScaledContentSize().height + 4.f;
    }

    CCLabelBMFont* msLabel = nullptr;
    if (msReadout) {
        msLabel = place(msNode, "framewindow-timing"_spr,
                        fmt::format("{:.{}f} ms", ms, places), top, 0.6f,
                        fineColour);
        top -= msLabel->getScaledContentSize().height + 3.f;
    }

    CCLabelBMFont* hzLabel = nullptr;
    if (hzReadout)
        hzLabel = place(hzNode, "framewindow-hz"_spr,
                        fmt::format("{} Hz", hzValue), top, 0.45f,
                        ccColor3B{255, 255, 255});

    if (m_timingMark == m_timingShown) return;
    m_timingShown = m_timingMark;

    float gap = 1.f;
    for (size_t i = static_cast<size_t>(m_timingMark) + 1;
         i < m_results.size(); i++) {
        auto const& next = m_results[i];
        if (next.unbounded || next.hidden) continue;
        if (next.desynced && !m_showDesynced->inner()) continue;
        if (!next.desynced && !this->visibleTierFor(next)) continue;
        gap = static_cast<float>(next.frame - mk->frame) /
              static_cast<float>(tps);
        break;
    }
    gap = std::clamp(gap, 0.2f, 3.f);

    for (CCLabelBMFont* label : {cbfLabel, rangeLabel, msLabel, hzLabel}) {
        if (!label) continue;
        label->stopAllActions();
        label->setOpacity(255);
        label->runAction(CCSequence::create(CCDelayTime::create(gap * 0.15f),
                                            CCFadeOut::create(gap * 0.85f),
                                            nullptr));
    }
}

std::vector<lstar::Input> FrameWindowAnalyzer::precisionInputs() const {
    std::vector<lstar::Input> out;
    out.reserve(m_results.size());

    for (auto const& mk : m_results) {
        if (mk.desynced || mk.hidden) continue;

        double const frames =
            mk.subframe > 0.f ? static_cast<double>(mk.subframe)
                              : static_cast<double>(mk.window);
        if (frames <= 0.0) continue;

        out.push_back({mk.frame, frames});
    }
    return out;
}

void FrameWindowAnalyzer::resetDisplay() {
    this->dropMarkers();
    m_lastFrame = 0;
    m_haveLastFrame = false;
    m_lastSpawn1P = -1;
    m_lastSpawn2P = -1;
    m_messageSpawned.assign(m_messages.size(), 0);
    m_hudCounts.clear();
    m_timingMark = -1;
    m_timingShown = -1;
    m_hudBuiltGeneration = UINT32_MAX;
    FrameWindowSound::stopAll();
}

cocos2d::CCNode* FrameWindowAnalyzer::markerContainer(PlayLayer* pl) {
    if (!pl || !pl->m_uiLayer) return nullptr;

    auto* container = pl->m_uiLayer->getChildByID("framewindow-markers"_spr);
    if (!container) {
        container = cocos2d::CCNode::create();
        container->setID("framewindow-markers"_spr);
        container->setZOrder(9998);
        pl->m_uiLayer->addChild(container);
    }
    return container;
}

void FrameWindowAnalyzer::spawnMarker(PlayLayer* pl, FrameWindowMark const& mk,
                                      FrameWindowTier const* tier) {
    auto* container = this->markerContainer(pl);
    if (!container) return;

    ccColor4F color{1.f, 1.f, 1.f, 1.f};

    bool const wholeOnly = mk.cbf && m_cbfWholeMarkers->inner();

    std::string text;
    if (mk.subframe > 0.f && !wholeOnly)
        text = this->formatSubframe(mk.subframe);
    else if (mk.desynced)
        text = "?";
    else
        text = formatWindow(mk.window);

    if (!mk.desynced) text = this->decorate(mk, std::move(text));

    if (tier) {
        color = {tier->color[0], tier->color[1], tier->color[2],
                 tier->color[3]};
        if (!tier->text.empty())
            text = mk.desynced ? tier->text
                               : this->decorate(mk, tier->text);
    } else {
        auto const c = colorForWindow(this->displayWindow(mk));
        color = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f};
    }

    if (mk.desynced) color = {0.51f, 0.51f, 0.51f, 0.6f};

    auto* node = CCNode::create();
    node->setPosition(mk.position);
    node->setZOrder(9999);

    // Circle skin sizes the ring by how tight the window is, so a 2-frame
    // click is visibly smaller than a 12-frame one. Off by default, in which
    // case every marker is the one configured size.
    auto const& fwcfg = SLSettings::get()->frameWindow;
    float radius = std::max(1.f, m_markerRadius->inner());
    if (fwcfg.circleSkin) {
        int const w = std::max(0, this->displayWindow(mk));
        radius = std::clamp(fwcfg.circleSkinDotRadius +
                                static_cast<float>(w) * fwcfg.circleSkinRadiusPerFrame,
                            1.f,
                            std::max(1.f, fwcfg.circleSkinMaxRadius));
    }
    auto* circle = CCDrawNode::create();
    CCPoint verts[64];
    for (int i = 0; i < 64; i++) {
        float const angle = static_cast<float>(i) * 6.2831853f / 64.f;
        verts[i] = CCPoint{radius * std::cos(angle), radius * std::sin(angle)};
    }

    float const a = color.a;
    circle->drawPolygon(verts, 64, {0.f, 0.f, 0.f, 0.f}, 4.f,
                        {0.f, 0.f, 0.f, a});
    circle->drawPolygon(verts, 64, {0.f, 0.f, 0.f, 0.f}, 2.f,
                        {color.r * a, color.g * a, color.b * a, a});
    node->addChild(circle);

    auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setAnchorPoint({1.f, 0.5f});
    label->setPosition({-(radius + 5.f), 0.f});
    label->setScale(std::max(0.05f, m_markerScale->inner()));
    label->setColor({static_cast<GLubyte>(color.r * 255),
                     static_cast<GLubyte>(color.g * 255),
                     static_cast<GLubyte>(color.b * 255)});
    label->setOpacity(static_cast<GLubyte>(color.a * 255));
    node->addChild(label);

    container->addChild(node);
    this->trackMarker(node, mk.position);
}

void FrameWindowAnalyzer::spawnMessages(PlayLayer* pl, uint32_t frame) {
    if (m_messages.empty() || m_results.empty()) return;

    if (m_messageSpawned.size() != m_messages.size())
        m_messageSpawned.assign(m_messages.size(), 0);

    auto* container = this->markerContainer(pl);
    if (!container) return;

    for (size_t mi = 0; mi < m_messages.size(); mi++) {
        auto const& msg = m_messages[mi];
        if (!msg.enabled || msg.text.empty()) continue;
        if (m_messageSpawned[mi]) continue;

        int const first =
            std::clamp(msg.startIndex, 0, static_cast<int>(m_results.size()) - 1);
        int const last = std::clamp(first + std::max(1, msg.span) - 1, first,
                                    static_cast<int>(m_results.size()) - 1);
        if (m_results[first].frame > frame) continue;

        m_messageSpawned[mi] = 1;

        auto const a = m_results[first].position;
        auto const b = m_results[last].position;
        cocos2d::CCPoint const mid{(a.x + b.x) * 0.5f,
                                   (a.y + b.y) * 0.5f + msg.offsetY};

        auto* node = cocos2d::CCNode::create();
        node->setPosition(mid);
        node->setZOrder(9999);

        float const scale = std::max(0.05f, msg.scale);
        float width = 0.f;
        std::vector<cocos2d::CCLabelBMFont*> parts;
        for (auto const& [runText, runColor] : parseColored(msg.text)) {
            auto* label =
                cocos2d::CCLabelBMFont::create(runText.c_str(), "bigFont.fnt");
            if (!label) continue;
            label->setAnchorPoint({0.f, 0.5f});
            label->setScale(scale);
            label->setColor(runColor);
            node->addChild(label);
            parts.push_back(label);
            width += label->getScaledContentSize().width;
        }

        float x = -width * 0.5f;
        for (auto* label : parts) {
            label->setPositionX(x);
            x += label->getScaledContentSize().width;
        }

        container->addChild(node);
        this->trackMarker(node, mid);
    }
}

void FrameWindowAnalyzer::trackMarker(cocos2d::CCNode* node,
                                      cocos2d::CCPoint world) {
    if (!node) return;
    node->retain();
    m_markerNodes.push_back({node, world});
}

void FrameWindowAnalyzer::dropMarkers() {
    for (auto& entry : m_markerNodes)
        if (entry.m_node) entry.m_node->release();
    m_markerNodes.clear();
}

void FrameWindowAnalyzer::cullOffscreen(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;

    CCSize const winSize = CCDirector::get()->getWinSize();
    constexpr float margin = 300.f;
    float const layerScale = pl->m_objectLayer->getScale();

    for (auto it = m_markerNodes.begin(); it != m_markerNodes.end();) {
        auto* node = it->m_node;
        if (!node || !node->getParent()) {
            if (node) node->release();
            it = m_markerNodes.erase(it);
            continue;
        }

        CCPoint const screen =
            pl->m_objectLayer->convertToWorldSpace(it->m_world);
        node->setPosition(screen);
        node->setScale(layerScale);

        if (screen.x < -margin || screen.x > winSize.width + margin ||
            screen.y < -margin || screen.y > winSize.height + margin) {
            node->removeFromParent();
            node->release();
            it = m_markerNodes.erase(it);
            continue;
        }
        ++it;
    }
}

void FrameWindowAnalyzer::rebuildHud(PlayLayer* pl) {
    if (auto* old = pl->m_uiLayer->getChildByID("framewindow-hud"_spr))
        old->removeFromParent();

    auto* hud = CCNode::create();
    hud->setID("framewindow-hud"_spr);
    hud->setAnchorPoint({0.f, 1.f});
    hud->setPosition({8.f, CCDirector::get()->getWinSize().height - 8.f});
    pl->m_uiLayer->addChild(hud);

    auto const& tiers = SLSettings::get()->frameWindow.tiers;

    auto const lift = [](float v) {
        return static_cast<GLubyte>(
            std::clamp(v + (1.f - v) * 0.10f, 0.f, 1.f) * 255.f);
    };

    float const scale = std::max(0.1f, SLSettings::get()->frameWindow.hudScale);
    float constexpr gutter = 6.f;

    struct Row {
        FrameWindowTier const* m_tier;
        CCLabelBMFont* m_name;
    };
    std::vector<Row> rows;
    float widest = 0.f;

    for (auto const& tier : tiers) {
        if (!tier.showInHud) continue;

        std::string const name =
            tier.text.empty()
                ? (tier.minWindow == tier.maxWindow
                       ? fmt::format("{}", tier.minWindow)
                       : fmt::format("{}-{}", tier.minWindow, tier.maxWindow))
                : tier.text;

        auto* label = CCLabelBMFont::create(
            fmt::format("{}:", name).c_str(), "bigFont.fnt");
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(scale);
        label->setColor({lift(tier.color[0]), lift(tier.color[1]),
                         lift(tier.color[2])});
        label->setOpacity(
            static_cast<GLubyte>(std::clamp(tier.color[3], 0.f, 1.f) * 255.f));

        widest = std::max(widest, label->getScaledContentSize().width);
        rows.push_back({&tier, label});
    }

    float y = 0.f;
    for (auto const& row : rows) {
        auto const& tier = *row.m_tier;

        row.m_name->setPosition({0.f, y});
        hud->addChild(row.m_name);

        auto* count = CCLabelBMFont::create("0", "bigFont.fnt");
        count->setAnchorPoint({0.f, 1.f});
        count->setPosition({widest + gutter, y});
        count->setScale(scale);
        count->setColor({lift(tier.color[0]), lift(tier.color[1]),
                         lift(tier.color[2])});
        count->setOpacity(
            static_cast<GLubyte>(std::clamp(tier.color[3], 0.f, 1.f) * 255.f));
        count->setID(fmt::format("framewindow-count-{}", tier.id));
        hud->addChild(count);

        y -= row.m_name->getContentSize().height * scale * 0.80f;
    }

    m_hudBuiltGeneration = m_generation;
    m_hudBuiltTiers = tiers.size();
}

void FrameWindowAnalyzer::refreshHudCounts(PlayLayer* pl) {
    auto* hud = pl->m_uiLayer->getChildByID("framewindow-hud"_spr);
    if (!hud) return;

    for (auto const& tier : SLSettings::get()->frameWindow.tiers) {
        auto* node = hud->getChildByID(fmt::format("framewindow-count-{}", tier.id));
        if (auto* label = typeinfo_cast<CCLabelBMFont*>(node)) {
            auto const it = m_hudCounts.find(tier.id);
            label->setString(
                fmt::format("{}", it == m_hudCounts.end() ? 0 : it->second)
                    .c_str());
        }
    }
}

void FrameWindowAnalyzer::recountUpTo(uint32_t frame) {
    m_hudCounts.clear();
    m_timingMark = -1;
    for (size_t i = 0; i < m_results.size(); i++) {
        auto const& mk = m_results[i];
        if (mk.frame > frame) continue;
        if (mk.desynced) continue;
        if (auto const* tier = this->visibleTierFor(mk)) {
            m_hudCounts[tier->id]++;
            m_timingMark = static_cast<int>(i);
        }
    }
}

int FrameWindowAnalyzer::displayWindow(FrameWindowMark const& mk) const {
    if (mk.subframe > 0.f && !(mk.cbf && m_cbfWholeMarkers->inner()))
        return std::max(1, static_cast<int>(std::floor(mk.subframe)));
    return mk.window;
}

void FrameWindowAnalyzer::updateDisplay(PlayLayer* pl) {
    auto& updater = Bot::get()->updater();
    uint32_t const frame = updater.getFrame();

    bool const markers = m_showMarkers->inner();
    bool const hud = m_showHud->inner();

    if (!m_haveLastFrame) {
        if (auto* c = pl->m_uiLayer->getChildByID("framewindow-markers"_spr))
            c->removeAllChildren();
        this->dropMarkers();
        m_lastSpawn1P = -1;
        m_lastSpawn2P = -1;
        m_messageSpawned.assign(m_messages.size(), 0);
        FrameWindowSound::stopAll();

        m_haveLastFrame = true;
        m_lastFrame = frame;
        this->recountUpTo(frame);
        if (hud) {
            this->rebuildHud(pl);
            this->refreshHudCounts(pl);
        }
        this->refreshTiming(pl);
        return;
    }

    if (frame < m_lastFrame) {
        if (auto* c = pl->m_uiLayer->getChildByID("framewindow-markers"_spr))
            c->removeAllChildren();
        this->dropMarkers();
        m_lastSpawn1P = -1;
        m_lastSpawn2P = -1;
        m_messageSpawned.assign(m_messages.size(), 0);
        FrameWindowSound::stopAll();
        this->recountUpTo(frame);
        m_lastFrame = frame;
        if (hud) this->refreshHudCounts(pl);
        this->refreshTiming(pl);
        return;
    }

    if (frame > m_lastFrame) {
        double const tps = updater.m_tps;
        bool const seeking =
            static_cast<double>(frame - m_lastFrame) > std::max(1.0, tps);

        bool changed = false;
        for (size_t mi = 0; mi < m_results.size(); mi++) {
            auto const& mk = m_results[mi];
            if (mk.frame <= m_lastFrame) continue;
            if (mk.frame > frame) continue;

            if (mk.unbounded || mk.hidden) continue;

            if (mk.desynced && !m_showDesynced->inner()) continue;

            auto const* tier =
                mk.desynced ? nullptr : this->visibleTierFor(mk);
            if (!mk.desynced && !tier) continue;

            int& lastSpawn = mk.player2 ? m_lastSpawn2P : m_lastSpawn1P;
            if (markers && static_cast<int>(mk.frame) != lastSpawn) {
                lastSpawn = static_cast<int>(mk.frame);
                this->spawnMarker(pl, mk, tier);
            }

            if (tier) {
                m_hudCounts[tier->id]++;
                m_timingMark = static_cast<int>(mi);
                changed = true;

                if (!seeking && m_playSounds->inner() && !mk.desynced) {
                    std::string const clip =
                        tier->audioPath.empty()
                            ? fmt::format("{}f SFX.mp3",
                                          std::clamp<int>(
                                              this->displayWindow(mk), 0, 6))
                            : tier->audioPath;
                    FrameWindowSound::play(clip, m_soundVolume->inner());
                }
            }
        }

        if (markers) this->spawnMessages(pl, frame);

        m_lastFrame = frame;
        if (hud && changed) this->refreshHudCounts(pl);
        if (changed) this->refreshTiming(pl);
        if (markers) this->cullOffscreen(pl);
    }
}

static char const* stageName(int stage) {
    switch (stage) {
        case 0: return "idle";
        case 1: return "capture";
        case 2: return "setup";
        case 3: return "advance";
        case 4: return "snapshot";
        case 5: return "probe";
        case 6: return "recover";
        case 7: return "rewind";
        default: return "finish";
    }
}

void FrameWindowAnalyzer::updateProgressOverlay(PlayLayer* pl) {
    auto* node = pl->m_uiLayer->getChildByID("framewindow-progress"_spr);

    if (!m_running || !m_verbose->inner() || !m_analysisOverlay->inner()) {
        if (node) node->removeFromParent();
        return;
    }

    auto& updater = Bot::get()->updater();

    double const elapsed =
        std::chrono::duration<double>(Clock::now() - m_runStart).count();
    double const frac = m_total ? static_cast<double>(m_index) /
                                      static_cast<double>(m_total)
                                : 0.0;
    double const eta = frac > 0.001 ? elapsed / frac - elapsed : 0.0;

    double const usPerTick =
        m_stepCount > 0
            ? static_cast<double>(m_stepNanos) / 1000.0 /
                  static_cast<double>(m_stepCount)
            : 0.0;
    double const legsPerSec =
        elapsed > 0.1 ? static_cast<double>(m_legCounter) / elapsed : 0.0;

    int tickShift = m_shift;
    double fraction = 0.0;
    this->splitSlots(m_shift, tickShift, fraction);

    char const* phase = m_phase == Phase::Nominal   ? "nominal"
                        : m_phase == Phase::Earlier ? "earlier"
                        : m_phase == Phase::Later   ? "later"
                                                    : "subframe";

    std::string flags;
    auto flag = [&](bool on, char const* name) {
        if (on) flags += (flags.empty() ? "" : " "), flags += name;
    };
    flag(m_noclip, "noclip");
    flag(m_pathDiverged, "diverged");
    flag(m_clamped, "clamped");
    flag(m_nominalDied, "nomdied");
    flag(m_bisect, "bisect");
    flag(m_scanning, "scan");
    flag(m_gallopping, "gallop");
    flag(m_captureNoclip, "capdied");
    flag(m_restoreFailed, "RESTOREFAIL");
    flag(m_shiftMode == ShiftMode::Buffer, "buffer");
    if (flags.empty()) flags = "-";

    std::string const sample =
        m_index < m_samples.size()
            ? fmt::format("f{} p{} {} gm={}", m_samples[m_index].frame,
                          m_samples[m_index].player2 ? 2 : 1,
                          m_samples[m_index].release ? "rel" : "press",
                          m_samples[m_index].gamemode)
            : std::string("-");

    std::string extra;
    if (m_entryActive)
        extra += fmt::format("\nentry  prev{:+d}  {} left  span {}..{}",
                             m_entryOffset, m_entryQueue.size(), m_entryMin,
                             m_entryMax);
    if (m_stageId == Stage::ResolveSetup && !m_setupGroups.empty()) {
        std::string combo;
        for (int o : m_setupCombo) combo += fmt::format("{:+d} ", o);
        extra += fmt::format(
            "\nsetup  group {}/{}  leader {:+d}  combo [{}] survivors {}",
            m_setupGroupCursor + 1, m_setupGroups.size(), m_setupLeaderShift,
            combo, m_setupSurvivors.size());
    }

    std::string const text = fmt::format(
        "{}  click {}/{}  {:.0f}%  {:.0f}s  eta {:.0f}s\n"
        "at {}\n"
        "frame {}  branch {}  rec {}  target {}\n"
        "{}  shift {:+d} -> tick{:+d} @{:.3f}  range -{}..+{}\n"
        "win {}  low {}  high {}  valid {}  slots {}x\n"
        "leg {}  {:.1f}/s  ticks {}  {:.1f}us/tick\n"
        "done {} skipped {} desync {}\n"
        "{}{}",
        stageName(static_cast<int>(m_stageId)), m_index + 1, m_total,
        frac * 100.0, elapsed, eta, sample, updater.getFrame(), m_branchFrame,
        m_recorded, m_testTarget, phase, m_shift, tickShift, fraction, m_maxNeg,
        m_maxPos, m_fine > 1 ? m_coarseWindow : m_validCount, m_low, m_high,
        m_validCount, std::max<int64_t>(1, m_fine), m_legCounter, legsPerSec,
        m_stepCount, usPerTick, m_measured, m_skipped, m_desynced, flags,
        extra);

    auto* label = typeinfo_cast<cocos2d::CCLabelBMFont*>(node);
    if (!label) {
        label = cocos2d::CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        if (!label) return;
        label->setID("framewindow-progress"_spr);
        label->setAnchorPoint({0.f, 0.f});
        label->setScale(0.45f);
        label->setOpacity(220);
        pl->m_uiLayer->addChild(label, 1000);
    } else {
        label->setString(text.c_str());
    }

    label->setPosition({6.f, 6.f});
    label->setColor(m_restoreFailed ? cocos2d::ccColor3B{255, 110, 110}
                                    : cocos2d::ccColor3B{190, 230, 255});
}

void FrameWindowAnalyzer::render(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer || !pl->m_uiLayer) {
        m_hudBuiltGeneration = UINT32_MAX;
        m_haveLastFrame = false;
        return;
    }

    auto* markers = pl->m_uiLayer->getChildByID("framewindow-markers"_spr);
    auto* hud = pl->m_uiLayer->getChildByID("framewindow-hud"_spr);

    bool const want = m_enabled->inner() && !m_results.empty() && !m_running;

    if (!want) {
        if (markers) markers->removeFromParent();
        this->dropMarkers();
        if (hud) hud->removeFromParent();
        if (auto* t = pl->m_uiLayer->getChildByID("framewindow-timing"_spr))
            t->removeFromParent();
        m_hudBuiltGeneration = UINT32_MAX;
        m_haveLastFrame = false;
        return;
    }

    auto const& tiers = SLSettings::get()->frameWindow.tiers;
    if (!hud || m_hudBuiltGeneration != m_generation ||
        m_hudBuiltTiers != tiers.size()) {
        if (m_showHud->inner()) {
            this->rebuildHud(pl);
            this->refreshHudCounts(pl);
        }
    }

    if (!m_showHud->inner()) {
        if (auto* h = pl->m_uiLayer->getChildByID("framewindow-hud"_spr))
            h->removeFromParent();
        m_hudBuiltGeneration = UINT32_MAX;
    }

    if (!m_showMarkers->inner() && markers) {
        markers->removeAllChildren();
        this->dropMarkers();
    }

    this->updateDisplay(pl);
}


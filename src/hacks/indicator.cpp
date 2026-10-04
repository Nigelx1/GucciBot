// The Survival Indicator (see indicator.hpp), and the in-level readouts that
// go with it: the accuracy / streak line, the calibration's progress and the
// noclip accuracy (hacks/noclip_accuracy.hpp). Written fresh for GucciBot on
// 2026-10-03. The survival question is the fork service's, which runs on
// Absense's copies of the player (Absent, GPL-3 via Silicate); the readouts
// sit on the UI layer the way the HUD's do (hacks/hud.cpp, after Silicate's
// labels, by peony).
//
// When things happen:
// - The reading is worked out after the frame's ticks have run (a hook on the
//   scheduler's update, which runs the level's ticks), once per tick: the
//   key is the tick and the reset count, as the fork service's own is. A
//   click made before the next frame lands on the first tick of it, which is
//   the tick the reading was taken for.
// - The cue is decided at the same point, when the reading changes.
// - The drawing is rebuilt when the readouts' node is drawn, from the stored
//   reading, so nothing simulates while the scene is being drawn.

#include "hacks/indicator.hpp"

#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "analysis/pathfinder.hpp"
#include "analysis/trajectory.hpp"
#include "audio/clicks.hpp"
#include "core/GucciBot.hpp"
#include "hacks/noclip_accuracy.hpp"
#include "render/renderer.hpp"
#include "trainers/calibration.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace gucci::indicator {

    const char* styleName(int style) {
        switch (style) {
            case Ring: return "Ring";
            case Classic: return "Classic";
            case Converge: return "Converge";
            case Pulse: return "Pulse";
            default: return "?";
        }
    }

    namespace {

        using Clock = std::chrono::steady_clock;

        // Saved keys.
        constexpr const char* kKeyEnabled = "ind_enabled";
        constexpr const char* kKeyLookahead = "ind_lookahead";
        constexpr const char* kKeyStyle = "ind_style";
        constexpr const char* kKeyOpacity = "ind_opacity";
        constexpr const char* kKeySafe[3] = {"ind_safe_r", "ind_safe_g", "ind_safe_b"};
        constexpr const char* kKeyDanger[3] = {"ind_danger_r", "ind_danger_g", "ind_danger_b"};
        constexpr const char* kKeySound = "ind_sound";
        constexpr const char* kKeyFlash = "ind_flash";
        constexpr const char* kKeyAccuracyHud = "ind_accuracy_hud";
        constexpr const char* kKeyBestStreak = "ind_best_streak";

        constexpr const char* kNodeId = "nigelx1.guccibot/indicators";

        // How long the flash on a click lasts.
        constexpr float kFlashSec = 0.22f;
        // Two cues closer than this are one: a reading that flickers between
        // two ticks should not rattle.
        constexpr double kCueGapSec = 0.08;
        // The cue's pitch: this at the most room, rising to kCuePitchTight
        // when doing nothing dies the tick after the click would land.
        constexpr float kCuePitchEasy = 0.85f;
        constexpr float kCuePitchTight = 1.6f;

        // The player's size on screen at a zoom of 1 (a cube is 30 units).
        constexpr float kPlayerRadius = 24.f;

        struct Key {
            uint32_t progress = 0;
            uint32_t resets = 0;
            PlayLayer* layer = nullptr;
            bool operator==(Key const&) const = default;
        };

        struct State {
            Key key;
            bool haveKey = false;
            Reading now;
            // The cue's condition has been false since it last played.
            bool cueArmed = true;
            Clock::time_point lastCue{};
            // The flash: when the click came and what the reading said then.
            bool flashing = false;
            bool flashSafe = false;
            Clock::time_point flashAt{};
        };
        State g;

        bool jumpHeld(PlayerObject* p) {
            auto const it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            return it != p->m_holdingButtons.end() && it->second;
        }

        // Whether anyone but the player is driving the level, or the level is
        // being filmed: the indicator neither asks nor draws then.
        bool quiet(GucciEngine* gb) {
            return gb->isPlaying() || gb->analyzerOwnsRun() || gb->fwAnalyzing || Pathfinder::get()->active ||
                   absense::isRunning() || SLRenderer::get()->isRecording();
        }

        bool fakePlayer(PlayerObject* p) {
            return p && ::Bot::get()->trajectory().isFakePlayer(p);
        }

        // How many ticks the player's reaction to a sound takes in the current
        // gamemode, aiming at the early edge of its spread so a click a little
        // late still lands inside the window (0 without a calibration).
        int reactionTicks(GucciEngine* gb, GamemodeCalibration const& cal) {
            float const aimMs = std::max(0.f, cal.leadMs - cal.jitterMs);
            if (cal.sampleCount <= 0 || aimMs <= 0.f)
                return 0;
            auto const& upd = gb->updater;
            // Ticks per second of real time: the game's ticks, sped up or
            // slowed down with it.
            double const tickRate = std::max(1.0, (upd.m_tps > 0.0 ? upd.m_tps : 240.0) * upd.m_speedhack);
            return static_cast<int>(std::lround(aimMs / 1000.0 * tickRate));
        }

        // The click cue (see indicator.hpp). With a reaction time to allow
        // for, doing nothing is asked again over that time plus the window.
        void cue(GucciEngine* gb, PlayLayer* pl, PlayerObject* p, Reading const& r) {
            auto& cal = CalibrationService::get();
            // The metronome is playing its own clicks.
            if (!gb->indicatorSoundEnabled || cal.active) {
                g.cueArmed = true;
                return;
            }
            auto const& mode = cal.modes[CalibrationService::gamemodeIndexFor(p)];
            if (!mode.guideEnabled) {
                g.cueArmed = true;
                return;
            }

            auto& traj = TrajectoryPredictionService::get();
            int const window = r.lookahead;
            int const lead = std::clamp(reactionTicks(gb, mode), 0, kMaxLookahead);
            int const span = window + lead;
            int keep = r.keepTicks;
            if (lead > 0)
                keep = traj.survivesFor(pl, p, span, 0);
            // A click is needed when doing nothing dies before the window that
            // starts where the click would land is over.
            bool ready = keep >= 0 && keep < span;
            if (ready) {
                if (lead == 0) {
                    ready = r.safe;
                } else {
                    // The click a reaction time from now: offset 1 is the next
                    // tick (survivesScript counts as GucciBot's frames do).
                    std::vector<std::pair<int, bool>> const click{{1 + lead, !r.held}};
                    ready = traj.survivesScript(pl, p, span, click) >= span;
                }
            }
            if (!ready) {
                g.cueArmed = true;
                return;
            }
            auto const now = Clock::now();
            if (!g.cueArmed || std::chrono::duration<double>(now - g.lastCue).count() < kCueGapSec)
                return;
            g.cueArmed = false;
            g.lastCue = now;
            // How many ticks the click can come late by: from where it lands to
            // where doing nothing dies.
            float const slack = static_cast<float>(std::max(0, keep - lead));
            float const tight = 1.f - std::clamp(slack / static_cast<float>(std::max(1, window)), 0.f, 1.f);
            clicks::playClick(false, kCuePitchEasy + (kCuePitchTight - kCuePitchEasy) * tight);
        }

        // After the frame's ticks: a new reading when a tick has run.
        void afterFrame() {
            auto* gb = GucciEngine::get();
            auto* pl = PlayLayer::get();
            if (!gb->enabled || !gb->survivalIndicator || !pl || quiet(gb)) {
                g.now.valid = false;
                g.haveKey = false;
                return;
            }
            // The scheduler is also run from inside a reset (to refresh the
            // level without a tick) and by Find Best Tick's search; the level
            // is halfway between two states then and no copy should run.
            if (gb->updater.m_onlyRefresh || gb->updater.m_predicting)
                return;
            PlayerObject* p = pl->m_player1;
            if (!p || fakePlayer(p))
                return;
            if (p->m_isDead || pl->m_playerDied || pl->m_levelEndAnimationStarted || !pl->m_started) {
                g.now.valid = false;
                return;
            }
            Key const key{static_cast<uint32_t>(pl->m_gameState.m_currentProgress), gb->updater.m_resetCount, pl};
            if (g.haveKey && key == g.key)
                return;
            if (g.haveKey && (key.resets != g.key.resets || key.layer != g.key.layer))
                g.cueArmed = true;

            auto& traj = TrajectoryPredictionService::get();
            int const window = std::clamp(gb->indicatorLookahead, kMinLookahead, kMaxLookahead);
            bool const held = jumpHeld(p);
            int const click = traj.survivesFor(pl, p, window, held ? -1 : 1);
            int const keep = click < 0 ? -1 : traj.survivesFor(pl, p, window, 0);
            // The copies are busy (the path preview drawing, a search): the
            // next call tries again.
            if (click < 0 || keep < 0)
                return;

            g.key = key;
            g.haveKey = true;
            Reading r;
            r.valid = true;
            r.held = held;
            r.lookahead = window;
            r.clickTicks = click;
            r.keepTicks = keep;
            r.safe = click >= window;
            r.needed = keep < window;
            g.now = r;
            cue(gb, pl, p, r);
        }

        ccColor4F colourOf(GucciEngine* gb, bool safe, float alpha) {
            if (safe)
                return {gb->indicatorSafeColorR, gb->indicatorSafeColorG, gb->indicatorSafeColorB, alpha};
            return {gb->indicatorDangerColorR, gb->indicatorDangerColorG, gb->indicatorDangerColorB, alpha};
        }

        std::string accuracyLine(GucciEngine* gb) {
            std::string acc = "--";
            if (gb->accuracyTotalClicks > 0)
                acc = fmt::format("{:.0f}", 100.0 * gb->accuracyGoodClicks / gb->accuracyTotalClicks);
            return fmt::format("Accuracy: {}%  Streak: {} (Best: {})", acc, gb->currentStreak, gb->bestStreak);
        }

        // The readouts and the indicator's drawing, on the PlayLayer's UI
        // layer. Rebuilt every drawn frame from the stored reading.
        class IndicatorNode : public CCNode {
        public:
            // Cocos's two-step making: the node is handed back autoreleased,
            // or not at all when a part of it could not be made.
            static IndicatorNode* create() {
                auto* made = new IndicatorNode();
                bool const ok = made->CCNode::init() && made->build();
                if (!ok) {
                    delete made;
                    return nullptr;
                }
                made->autorelease();
                return made;
            }

            void visit() override {
                this->refresh();
                CCNode::visit();
            }

        private:
            CCDrawNode* m_draw = nullptr;
            CCLabelBMFont* m_label = nullptr;
            std::string m_text;
            float m_phase = 0.f;  // Pulse's
            Clock::time_point m_lastVisit;

            // The drawing goes under the text, which sits centred under its
            // top edge.
            bool build() {
                m_draw = CCDrawNode::create();
                m_label = CCLabelBMFont::create(" ", "chatFont.fnt");
                if (!m_draw || !m_label)
                    return false;
                m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
                m_label->setAlignment(kCCTextAlignmentCenter);
                m_label->setAnchorPoint({0.5f, 1.f});
                m_label->setScale(0.55f);
                this->addChild(m_draw);
                this->addChild(m_label);
                m_lastVisit = Clock::now();
                return true;
            }

            CCSize screen() {
                CCSize size = this->getParent() ? this->getParent()->getContentSize() : CCSize{};
                if (size.width <= 0.f || size.height <= 0.f)
                    size = CCDirector::get()->getWinSize();
                return size;
            }

            void refresh() {
                auto const now = Clock::now();
                float const dt = std::min(0.25f, std::chrono::duration<float>(now - m_lastVisit).count());
                m_lastVisit = now;

                m_draw->clear();
                auto* gb = GucciEngine::get();
                auto* pl = PlayLayer::get();
                if (!gb->enabled || !pl || SLRenderer::get()->isRecording()) {
                    m_label->setVisible(false);
                    return;
                }
                CCSize const size = screen();
                float top = size.height - 26.f;  // under GD's progress bar and percentage

                bool const show = gb->survivalIndicator && !quiet(gb) && g.now.valid && g.key.layer == pl &&
                                  pl->m_player1 && !fakePlayer(pl->m_player1);
                if (show) {
                    float const opacity = std::clamp(gb->indicatorOpacity, 0.05f, 1.f);
                    int const style = std::clamp(gb->indicatorStyle, 0, StyleCount - 1);
                    if (style == Classic) {
                        this->drawClassic(gb, {size.width / 2.f, top - 10.f}, opacity);
                        top -= 30.f;
                    } else {
                        this->drawAroundPlayer(gb, pl, style, opacity, dt);
                    }
                }

                this->drawReadouts(gb, size, top);
            }

            float flashLeft() {
                if (!g.flashing)
                    return 0.f;
                float const t = std::chrono::duration<float>(Clock::now() - g.flashAt).count();
                if (t >= kFlashSec) {
                    g.flashing = false;
                    return 0.f;
                }
                return 1.f - t / kFlashSec;
            }

            // 0 with room to spare, 1 when doing nothing dies on the next tick.
            static float tightness(Reading const& r) {
                if (!r.needed || r.lookahead <= 0)
                    return 0.f;
                return 1.f - std::clamp(static_cast<float>(r.keepTicks) / r.lookahead, 0.f, 1.f);
            }

            void drawFlash(GucciEngine* gb, CCPoint centre, float radius, float opacity) {
                float const f = flashLeft();
                if (!gb->indicatorFlashEnabled || f <= 0.f)
                    return;
                m_draw->drawDot(centre, radius * (1.f + 0.8f * (1.f - f)), colourOf(gb, g.flashSafe, 0.45f * f * opacity));
            }

            // Classic: a light at the top of the screen, with a bar under it
            // that empties as the room to click runs out.
            void drawClassic(GucciEngine* gb, CCPoint centre, float opacity) {
                Reading const& r = g.now;
                ccColor4F const fill = colourOf(gb, r.safe, opacity);
                m_draw->drawCircle(centre, 8.f, fill, 1.5f, ccColor4F{0.f, 0.f, 0.f, 0.6f * opacity}, 28);
                float const half = 30.f;
                float const y = centre.y - 15.f;
                m_draw->drawSegment({centre.x - half, y}, {centre.x + half, y}, 1.5f,
                                    ccColor4F{0.f, 0.f, 0.f, 0.45f * opacity});
                float const room = 1.f - tightness(r);
                if (room > 0.f)
                    m_draw->drawSegment({centre.x - half * room, y}, {centre.x + half * room, y}, 1.5f, fill);
                this->drawFlash(gb, centre, 10.f, opacity);
            }

            void drawAroundPlayer(GucciEngine* gb, PlayLayer* pl, int style, float opacity, float dt) {
                PlayerObject* p = pl->m_player1;
                if (!pl->m_objectLayer || !p->isVisible())
                    return;
                // Where the player is drawn (Frame Extrapolation's in-between
                // point included), carried from the level onto this node, and
                // how big the level is drawn here (the camera's zoom).
                CCPoint const at = p->getPosition();
                CCPoint const a = this->convertToNodeSpace(pl->m_objectLayer->convertToWorldSpace(at));
                CCPoint const b =
                    this->convertToNodeSpace(pl->m_objectLayer->convertToWorldSpace(at + CCPoint{30.f, 0.f}));
                float const zoom = std::max(0.05f, a.getDistance(b) / 30.f);
                float const radius = kPlayerRadius * std::max(0.3f, p->m_vehicleSize) * zoom;

                Reading const& r = g.now;
                float const tight = tightness(r);
                ccColor4F const colour = colourOf(gb, r.safe, opacity);
                ccColor4F const clear{0.f, 0.f, 0.f, 0.f};

                switch (style) {
                    case Ring:
                        // Thicker the less room there is.
                        m_draw->drawCircle(a, radius, clear, 1.5f + 2.5f * tight, colour, 40);
                        break;
                    case Converge: {
                        // Four corners closing in on the player as the room runs
                        // out; sitting wide when nothing is needed.
                        float const d = radius * (1.f + 1.2f * (1.f - tight));
                        float const arm = radius * 0.45f;
                        float const w = 1.5f;
                        for (int sx = -1; sx <= 1; sx += 2) {
                            for (int sy = -1; sy <= 1; sy += 2) {
                                CCPoint const corner{a.x + sx * d, a.y + sy * d};
                                m_draw->drawSegment(corner, {corner.x - sx * arm, corner.y}, w, colour);
                                m_draw->drawSegment(corner, {corner.x, corner.y - sy * arm}, w, colour);
                            }
                        }
                        break;
                    }
                    case Pulse: {
                        // A disc that beats faster the less room there is.
                        float const hz = 1.f + 5.f * tight;
                        m_phase = std::fmod(m_phase + dt * hz * 6.2831853f, 6.2831853f);
                        float const beat = 0.5f + 0.5f * std::sin(m_phase);
                        m_draw->drawDot(a, radius * (0.85f + 0.3f * beat), colourOf(gb, r.safe, 0.3f * opacity));
                        m_draw->drawCircle(a, radius * (0.85f + 0.3f * beat), clear, 1.f, colour, 40);
                        break;
                    }
                    default:
                        break;
                }
                this->drawFlash(gb, a, radius, opacity);
            }

            void drawReadouts(GucciEngine* gb, CCSize size, float top) {
                std::string text;
                auto add = [&](std::string const& line) {
                    if (!text.empty())
                        text += '\n';
                    text += line;
                };
                auto& cal = CalibrationService::get();
                if (cal.active)
                    add(fmt::format("Calibrating {}: click on each click sound ({} / {})",
                                    CalibrationService::gamemodeName(cal.calibratingMode), cal.repsDone,
                                    cal.repsTarget));
                if (gb->survivalIndicator && gb->accuracyHudEnabled && !gb->isPlaying())
                    add(accuracyLine(gb));
                if (gb->noclipAccuracyVisible && gb->noclipEnabled)
                    add(fmt::format("Noclip accuracy: {:.2f}% ({} {})", noclipacc::accuracy() * 100.f,
                                    noclipacc::hits(), noclipacc::hits() == 1 ? "death" : "deaths"));
                if (text.empty()) {
                    m_label->setVisible(false);
                    return;
                }
                // setString rebuilds the glyphs, so only on a change.
                if (text != m_text) {
                    m_label->setString(text.c_str());
                    m_text = std::move(text);
                }
                m_label->setPosition({size.width / 2.f, top});
                m_label->setVisible(true);
            }
        };

    } // namespace

    Reading const& reading() {
        return g.now;
    }

    void loadSettings() {
        auto* gb = GucciEngine::get();
        auto* mod = Mod::get();
        gb->survivalIndicator = mod->getSavedValue<bool>(kKeyEnabled, false);
        gb->indicatorLookahead =
            std::clamp(mod->getSavedValue<int>(kKeyLookahead, gb->indicatorLookahead), kMinLookahead, kMaxLookahead);
        gb->indicatorStyle = std::clamp(mod->getSavedValue<int>(kKeyStyle, gb->indicatorStyle), 0, StyleCount - 1);
        gb->indicatorOpacity = std::clamp(mod->getSavedValue<float>(kKeyOpacity, gb->indicatorOpacity), 0.05f, 1.f);
        float* safe[3] = {&gb->indicatorSafeColorR, &gb->indicatorSafeColorG, &gb->indicatorSafeColorB};
        float* danger[3] = {&gb->indicatorDangerColorR, &gb->indicatorDangerColorG, &gb->indicatorDangerColorB};
        for (int i = 0; i < 3; ++i) {
            *safe[i] = std::clamp(mod->getSavedValue<float>(kKeySafe[i], *safe[i]), 0.f, 1.f);
            *danger[i] = std::clamp(mod->getSavedValue<float>(kKeyDanger[i], *danger[i]), 0.f, 1.f);
        }
        gb->indicatorSoundEnabled = mod->getSavedValue<bool>(kKeySound, gb->indicatorSoundEnabled);
        gb->indicatorFlashEnabled = mod->getSavedValue<bool>(kKeyFlash, gb->indicatorFlashEnabled);
        gb->accuracyHudEnabled = mod->getSavedValue<bool>(kKeyAccuracyHud, gb->accuracyHudEnabled);
        gb->bestStreak = std::max(0, mod->getSavedValue<int>(kKeyBestStreak, 0));
    }

    void saveSettings() {
        auto* gb = GucciEngine::get();
        auto* mod = Mod::get();
        mod->setSavedValue<bool>(kKeyEnabled, gb->survivalIndicator);
        mod->setSavedValue<int>(kKeyLookahead, gb->indicatorLookahead);
        mod->setSavedValue<int>(kKeyStyle, gb->indicatorStyle);
        mod->setSavedValue<float>(kKeyOpacity, gb->indicatorOpacity);
        float const safe[3] = {gb->indicatorSafeColorR, gb->indicatorSafeColorG, gb->indicatorSafeColorB};
        float const danger[3] = {gb->indicatorDangerColorR, gb->indicatorDangerColorG, gb->indicatorDangerColorB};
        for (int i = 0; i < 3; ++i) {
            mod->setSavedValue<float>(kKeySafe[i], safe[i]);
            mod->setSavedValue<float>(kKeyDanger[i], danger[i]);
        }
        mod->setSavedValue<bool>(kKeySound, gb->indicatorSoundEnabled);
        mod->setSavedValue<bool>(kKeyFlash, gb->indicatorFlashEnabled);
        mod->setSavedValue<bool>(kKeyAccuracyHud, gb->accuracyHudEnabled);
        mod->setSavedValue<int>(kKeyBestStreak, gb->bestStreak);
    }

    void onRealClick(bool p2, bool pressed) {
        // The reading is player 1's.
        if (p2)
            return;
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (!gb->survivalIndicator || !pl || quiet(gb) || !g.now.valid || g.key.layer != pl ||
            g.key.resets != gb->updater.m_resetCount)
            return;
        Reading const& r = g.now;
        // A press while the reading had the button down (or a release with it
        // up) is not the click the reading was about: a second event before
        // the next tick.
        if (pressed == r.held)
            return;

        if (gb->indicatorFlashEnabled) {
            g.flashing = true;
            g.flashSafe = r.safe;
            g.flashAt = Clock::now();
        }

        // Scored only when something was at stake: a safe click that doing
        // nothing would have survived just as well says nothing either way.
        if (r.safe && !r.needed)
            return;
        gb->accuracyTotalClicks++;
        if (r.safe) {
            gb->accuracyGoodClicks++;
            gb->currentStreak++;
            if (gb->currentStreak > gb->bestStreak) {
                gb->bestStreak = gb->currentStreak;
                Mod::get()->setSavedValue<int>(kKeyBestStreak, gb->bestStreak);
            }
        } else {
            gb->currentStreak = 0;
        }
    }

    void resetStats(bool resetBest) {
        auto* gb = GucciEngine::get();
        gb->accuracyGoodClicks = 0;
        gb->accuracyTotalClicks = 0;
        gb->currentStreak = 0;
        if (resetBest) {
            gb->bestStreak = 0;
            Mod::get()->setSavedValue<int>(kKeyBestStreak, 0);
        }
    }

} // namespace gucci::indicator

// The scheduler's update is where the level's ticks run (engine_updater.cpp
// drives them from its own hook on it); the reading is taken once it is done.
class $modify(GBIndicatorScheduler, CCScheduler) {
    void update(float dt) override {
        CCScheduler::update(dt);
        gucci::indicator::afterFrame();
    }
};

// The readouts' node goes on the UI layer once the level is built, beside the
// HUD's. A new level starts the session's accuracy over (the best is kept).
class $modify(GBIndicatorPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        gucci::indicator::resetStats(false);
        if (m_uiLayer && !m_uiLayer->getChildByID(gucci::indicator::kNodeId)) {
            if (auto* node = gucci::indicator::IndicatorNode::create()) {
                node->setID(gucci::indicator::kNodeId);
                m_uiLayer->addChild(node, 99999);
            }
        }
        return true;
    }
};

$on_mod(Loaded) {
    gucci::indicator::loadSettings();
}

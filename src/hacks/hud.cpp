// The HUD's drawing (see hud.hpp). How each value is read from the game -
// the player fields, the game state's clocks, GD's random state at
// 0x6c2e90, the orb names and the velocity-unrounding check - is ported from
// Silicate's labels (silicate/src/label/label.cpp, by peony, GPL-3.0; Absense's
// copy at Absense/src/label/label.cpp is the same code). The frame, TPS, bot
// state, action index, intentional death, tick limit and last input readouts
// come from GucciBot's own engine.
//
// What differs from Silicate:
// - One label holds every readout, a line each, in one corner, instead of
//   one label per readout each with its own corner, font and size. The
//   corner, font, size and opacity are the HUD's (HudConfig).
// - The text is refreshed when the label is drawn (HudNode::visit), not from
//   the tick loop, so it always shows the state the frame on screen shows,
//   and nothing runs when the HUD is off.
// - "Ticks since last input" counts from GucciBot's input frame, which is
//   one ahead of Silicate's (CLAUDE.md section 4), and skips the TPS / death /
//   restart actions, which are not inputs.
// - Hidden while a video is being rendered, Silicate's default (it has a
//   switch to keep its labels in renders; GucciBot does not, yet).
// - Absense's look-ahead copies of the player are never read: while one
//   stands in for a real player the HUD keeps its last text.

#include "core/platform.hpp"
#include "hacks/hud.hpp"

#include "core/GucciBot.hpp"
#include "analysis/ac/shim.hpp"
#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

using namespace geode::prelude;

namespace gucci::hud {

    const char* groupName(Group group) {
        switch (group) {
            case Bot: return "Bot";
            case Player: return "Player";
            case Level: return "Level";
            default: return "";
        }
    }

    namespace {

        // Base sizes, before the HUD's scale: Silicate's 0.7 for chatFont, and
        // a bigFont size that comes out about as tall.
        constexpr float kChatFontScale = 0.7f;
        constexpr float kBigFontScale = 0.4f;
        // Gap between the text and the screen edge, in points (Silicate's).
        constexpr float kEdge = 8.f;

        // The game's address of its random state (Silicate's
        // getCurrentRandomState; engine_core.cpp saves it with checkpoints).
        constexpr uintptr_t kRandomStateOffset = 0x6c2e90;

        std::string_view orbName(RingObject* ring) {
            switch (ring->m_objectID) {
                case 36: return "Yellow";
                case 84: return "Blue";
                case 141: return "Pink";
                case 1022: return "Green";
                case 1330: return "Black";
                case 1333: return "Red";
                case 1594: return "Toggle";
                case 1704: return "Green Dash";
                case 1751: return "Pink Dash";
                case 3004: return "Spider";
                case 3027: return "Teleport";
                default: return "Unknown";
            }
        }

        // chizz's Velocity Unrounding stops GD rounding y-velocity, so the
        // readout shows more digits and says so (Silicate does the same).
        bool velocityUnrounded() {
            static Mod* mod = Loader::get()->getInstalledMod("chizz.velocity-unrounding");
            return mod && mod->isLoaded() && mod->getSettingValue<bool>("mod-enabled");
        }

        // "name: p1" or, in dual mode, "name: p1 / p2".
        template <typename F>
        std::string perPlayer(const char* name, PlayerObject* p1, PlayerObject* p2, F&& value) {
            if (p2)
                return fmt::format("{}: {} / {}", name, value(p1), value(p2));
            return fmt::format("{}: {}", name, value(p1));
        }

        std::string touchingOrbs(PlayerObject* player) {
            std::string out;
            auto* rings = player->m_touchingRings;
            unsigned const count = rings ? rings->count() : 0;
            for (unsigned i = 0; i < count; ++i) {
                auto* ring = static_cast<RingObject*>(rings->objectAtIndex(i));
                if (!ring || ring->hasBeenActivatedByPlayer(player))
                    continue;
                if (!out.empty())
                    out += ", ";
                out += orbName(ring);
                if (ring->m_objectID == 1594)
                    out += fmt::format(" ({})", ring->m_targetGroupID);
                if (ring->m_isMultiActivate)
                    out += "*";
            }
            return out.empty() ? std::string("None") : out;
        }

        // GucciBot records an input at getFrame() + 1 (CLAUDE.md section 4),
        // so the input frame "now" is one past the counter.
        std::string ticksSinceLastInput(GucciEngine* gb) {
            auto const& actions = gb->replay.m_actionAtom.m_actions;
            if (actions.empty())
                return "Last input: none in the macro";

            // Recording: the newest action. Playing: the newest one played.
            size_t end = actions.size();
            if (!gb->isRecording()) {
                if (gb->replay.m_inputIndex == 0)
                    return "Last input: waiting for the first";
                end = std::min(gb->replay.m_inputIndex, actions.size());
            }
            for (size_t i = end; i-- > 0;) {
                if (!actions[i].isInput())
                    continue;
                int64_t const now = static_cast<int64_t>(gb->updater.getFrame()) + 1;
                int64_t const since = now - static_cast<int64_t>(actions[i].m_frame);
                // After a step back the newest recorded input can sit ahead
                // of the counter until the recorder trims it.
                if (since < 0)
                    return fmt::format("Last input: {} ticks ahead", -since);
                return fmt::format("Ticks since last input: {}", since);
            }
            return "Last input: none yet";
        }

        std::string botState(GucciEngine* gb) {
            const char* mode = gb->isRecording() ? "Recording" : gb->isPlaying() ? "Playing" : "Idle";
            std::string out = fmt::format("State: {}", mode);
            if (gb->analyzerOwnsRun())
                out += " (Calculate)";
            if (gb->updater.m_paused)
                out += ", paused";
            return out;
        }

        std::string tickLimit(GucciUpdater const& upd) {
            // Real Time runs every tick that is due, with no cap (the tick
            // loop in engine_updater.cpp skips the limit then).
            if (upd.m_realTime)
                return "Tick limit: none (Real Time)";
            return fmt::format("Tick limit: {} per frame{}", upd.m_stepLimit,
                               upd.m_dynamicUpr ? " (dynamic)" : "");
        }

        // One readout's line. p2 is null outside dual mode.
        std::string readoutLine(bool HudConfig::* field, GucciEngine* gb, PlayLayer* pl, PlayerObject* p1,
                                PlayerObject* p2) {
            auto& upd = gb->updater;
            auto& gs = pl->m_gameState;

            if (field == &HudConfig::showFrame)
                return fmt::format("Frame: {}", upd.getFrame());
            if (field == &HudConfig::showTPS)
                return fmt::format("TPS: {:g}", upd.m_tps);
            if (field == &HudConfig::showState)
                return botState(gb);
            if (field == &HudConfig::showActionIndex)
                return fmt::format("Action: {} / {}", gb->replay.m_inputIndex, gb->replay.m_actionAtom.length());
            if (field == &HudConfig::showLastInput)
                return ticksSinceLastInput(gb);
            if (field == &HudConfig::showIntentional) {
                // Silicate's two meanings: while recording, whether a death
                // now would be kept as intentional; otherwise whether
                // playback is expecting one.
                if (gb->isRecording())
                    return fmt::format("Intentional death: {}", upd.m_canDie ? "armed" : "off");
                return fmt::format("Intentional death: {}", upd.m_expectsDeath ? "expected" : "no");
            }
            if (field == &HudConfig::showTickLimit)
                return tickLimit(upd);

            if (field == &HudConfig::showX)
                return perPlayer("X", p1, p2, [](PlayerObject* p) { return fmt::format("{:.6f}", p->m_position.x); });
            if (field == &HudConfig::showY)
                return perPlayer("Y", p1, p2, [](PlayerObject* p) { return fmt::format("{:.6f}", p->m_position.y); });
            if (field == &HudConfig::showXVel)
                return perPlayer("X vel", p1, p2,
                                 [](PlayerObject* p) { return fmt::format("{:.6f}", p->m_platformerXVelocity); });
            if (field == &HudConfig::showYVel) {
                if (velocityUnrounded())
                    return perPlayer("Y vel (unrounded)", p1, p2,
                                     [](PlayerObject* p) { return fmt::format("{:.8f}", p->m_yVelocity); });
                return perPlayer("Y vel", p1, p2, [](PlayerObject* p) { return fmt::format("{:.3f}", p->m_yVelocity); });
            }
            if (field == &HudConfig::showRot)
                return perPlayer("Rotation", p1, p2,
                                 [](PlayerObject* p) { return fmt::format("{:.3f}", p->getRotation()); });
            if (field == &HudConfig::showSpeed)
                return perPlayer("Speed", p1, p2, [](PlayerObject* p) { return fmt::format("{:.2f}", p->m_playerSpeed); });
            if (field == &HudConfig::showGravity)
                return perPlayer("Gravity", p1, p2,
                                 [](PlayerObject* p) { return std::string(p->m_isUpsideDown ? "Flipped" : "Normal"); });
            if (field == &HudConfig::showOnGround)
                return perPlayer("On ground", p1, p2,
                                 [](PlayerObject* p) { return std::string(p->m_isOnGround ? "yes" : "no"); });
            if (field == &HudConfig::showAlive)
                return perPlayer("Player", p1, p2,
                                 [](PlayerObject* p) { return std::string(p->m_isDead ? "Dead" : "Alive"); });
            if (field == &HudConfig::showOrbs)
                return perPlayer("Orbs", p1, p2, [](PlayerObject* p) { return touchingOrbs(p); });

            if (field == &HudConfig::showGameTick)
                return fmt::format("Game tick: {}", gs.m_currentProgress);
            if (field == &HudConfig::showLevelTime)
                return fmt::format("Level time: {:.6f}s", gs.m_levelTime);
            if (field == &HudConfig::showTimeWarp)
                return fmt::format("Time warp: {:.2f}x", gs.m_timeWarp);
            if (field == &HudConfig::showCheckpoints) {
                auto const& pf = gb->practiceFix;
                return fmt::format("Checkpoints: {} | step-back {} | platformer {}", pf.m_savedCheckpoints.size(),
                                   pf.m_storedFrames.size(), pf.m_platformerCheckpoints.size());
            }
            if (field == &HudConfig::showRandom) {
                uint64_t const rng = gdRandomState() ? *gdRandomState() : 0;
                return fmt::format("Random: {} | shake {} | teleport {}", rng, gb->replay.m_shakeRandomState,
                                   gb->replay.m_teleportRandomState);
            }
            return {};
        }

        // The node on the UI layer that owns the label. Refreshing from
        // visit() means the text is built only for frames that are drawn,
        // after that frame's ticks have run.
        class HudNode : public CCNode {
        public:
            static HudNode* create() {
                auto* node = new HudNode();
                if (node->init()) {
                    node->autorelease();
                    return node;
                }
                delete node;
                return nullptr;
            }

            void visit() override {
                this->refresh();
                CCNode::visit();
            }

        private:
            CCLabelBMFont* m_label = nullptr;
            bool m_bigFont = false;
            std::string m_text;

            void hide() {
                if (m_label)
                    m_label->setVisible(false);
            }

            void refresh() {
                auto* gb = GucciEngine::get();
                auto const& cfg = gb->hud;
                auto* pl = PlayLayer::get();
                if (!gb->enabled || !cfg.enabled || !pl || SLRenderer::get()->isRecording())
                    return this->hide();

                PlayerObject* p1 = pl->m_player1;
                PlayerObject* p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
                if (!p1)
                    return this->hide();

                // One of Absense's look-ahead copies is standing in for a real
                // player: keep the last text rather than show the copy's.
                auto& traj = ::Bot::get()->trajectory();
                bool const fake = traj.isFakePlayer(p1) || (p2 && traj.isFakePlayer(p2));

                if (!fake) {
                    std::string text;
                    for (auto const& r : kReadouts) {
                        if (!(cfg.*r.field))
                            continue;
                        if (!text.empty())
                            text += '\n';
                        text += readoutLine(r.field, gb, pl, p1, p2);
                    }
                    if (text.empty())
                        return this->hide();

                    if (!m_label || m_bigFont != cfg.bigFont) {
                        if (m_label)
                            m_label->removeFromParent();
                        m_bigFont = cfg.bigFont;
                        m_label = CCLabelBMFont::create(" ", m_bigFont ? "bigFont.fnt" : "chatFont.fnt");
                        if (!m_label)
                            return;
                        this->addChild(m_label);
                        m_text.clear();
                    }
                    // setString rebuilds the label's glyphs, so only when the
                    // text actually changed.
                    if (text != m_text) {
                        m_label->setString(text.c_str());
                        m_text = std::move(text);
                    }
                } else if (!m_label) {
                    return;
                }

                this->place(cfg);
                m_label->setVisible(true);
            }

            void place(HudConfig const& cfg) {
                CCSize size = this->getParent() ? this->getParent()->getContentSize() : CCSize{};
                if (size.width <= 0.f || size.height <= 0.f)
                    size = CCDirector::get()->getWinSize();

                int const anchor = std::clamp(cfg.anchor, 0, AnchorCount - 1);
                bool const right = anchor == TopRight || anchor == BottomRight;
                bool const top = anchor == TopLeft || anchor == TopRight;

                float const base = m_bigFont ? kBigFontScale : kChatFontScale;
                m_label->setScale(base * std::clamp(cfg.scale, kMinScale, kMaxScale));
                m_label->setOpacity(
                    static_cast<GLubyte>(std::clamp(cfg.opacity, kMinOpacity, kMaxOpacity) * 255.f));
                m_label->setAlignment(right ? kCCTextAlignmentRight : kCCTextAlignmentLeft);
                m_label->setAnchorPoint({right ? 1.f : 0.f, top ? 1.f : 0.f});
                m_label->setPosition({right ? size.width - kEdge : kEdge, top ? size.height - kEdge : kEdge});
            }
        };

        constexpr const char* kNodeId = "nigelx1.guccibot/hud";

    } // namespace

} // namespace gucci::hud

// The HUD's node goes on the UI layer once the level is built, above GD's
// own UI (Silicate's z-order for its labels).
class $modify(GBHudPlayLayerHook, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (gucci::GucciEngine::get()->enabled && m_uiLayer && !m_uiLayer->getChildByID(gucci::hud::kNodeId)) {
            if (auto* node = gucci::hud::HudNode::create()) {
                node->setID(gucci::hud::kNodeId);
                m_uiLayer->addChild(node, 100000);
            }
        }
        return true;
    }
};

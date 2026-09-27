// anticroom's SubtickPreview, ported 2026-09-27. Stepping, the step stride,
// the label and the "a press here steps the tick" rule are his; the drawing
// runs on GucciBot's trajectory. See the header.

#include "replay/subtick_preview.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/shim.hpp"
#include "analysis/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "render/renderer.hpp"
#include "replay/scbf_input.hpp"

using namespace geode::prelude;

namespace scbf {

    namespace {

        constexpr int MIN_SPLITS = 2;
        constexpr int MAX_SPLITS = 524288;
        // Holding the step key crosses about a whole tick per this many
        // repeats, however fine the splits.
        constexpr double REPEATS_PER_TICK = 100.0;
        // His keybind repeat rate default. GucciBot's step key repeats at the
        // OS rate and has no setting for it.
        constexpr double KEY_REPEAT_HZ = 30.0;

        // No shared hitbox palette in GucciBot, so the outlines use fixed
        // colours: the usual red for the player, orange when that point is a
        // death, a paler outline for the rotated box, blue for the inner one.
        constexpr ccColor4F kPlayer{1.f, 0.25f, 0.25f, 1.f};
        constexpr ccColor4F kDead{1.f, 0.55f, 0.1f, 1.f};
        constexpr ccColor4F kRotated{1.f, 0.6f, 0.6f, 0.6f};
        constexpr ccColor4F kInner{0.3f, 0.55f, 1.f, 1.f};
        constexpr float kWidth = 0.6f;  // the path preview's own line width

        void outline(CCDrawNode* node, CCRect const& rect, float angle, float width, ccColor4F color) {
            std::array<CCPoint, 4> corners = {
                CCPoint{rect.getMinX(), rect.getMinY()},
                CCPoint{rect.getMaxX(), rect.getMinY()},
                CCPoint{rect.getMaxX(), rect.getMaxY()},
                CCPoint{rect.getMinX(), rect.getMaxY()},
            };
            if (angle != 0.f) {
                CCPoint const mid{rect.getMidX(), rect.getMidY()};
                for (auto& c : corners)
                    c = c.rotateByAngle(mid, -CC_DEGREES_TO_RADIANS(angle));
            }
            node->drawPolygon(corners.data(), corners.size(), {0.f, 0.f, 0.f, 0.f}, width, color);
        }

        // Its own node beside the path preview's, one layer above it.
        CCDrawNode* previewNode(PlayLayer* pl, bool create) {
            auto* parent = pl->m_objectLayer;
            if (!parent)
                return nullptr;

            auto* node = static_cast<CCDrawNode*>(parent->getChildByID("subtick-preview"_spr));
            if (node || !create)
                return node;

            node = CCDrawNode::create();
            node->setID("subtick-preview"_spr);
            node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            parent->addChild(node, 10000);
            return node;
        }

    } // namespace

    SubtickPreview& SubtickPreview::get() {
        static SubtickPreview instance;
        return instance;
    }

    bool SubtickPreview::usable() const {
        auto* gb = gucci::GucciEngine::get();
        auto* pl = PlayLayer::get();
        auto& fw = ::Bot::get()->frameWindow();
        return gb->replay.m_subtickPreview && m_cbfInUse && pl && !pl->m_isPaused &&
               gb->enabled && gb->updater.isPaused() &&
               !gucci::SLRenderer::get()->isRecording() && !fw.running() && !fw.returning();
    }

    int SubtickPreview::splits() const {
        return std::clamp(gucci::GucciEngine::get()->replay.m_subtickSplits, MIN_SPLITS, MAX_SPLITS);
    }

    double SubtickPreview::fraction() const {
        if (!this->usable() || m_split <= 0)
            return 0.0;
        return static_cast<double>(m_split) / this->splits();
    }

    int SubtickPreview::stride() {
        auto const now = std::chrono::steady_clock::now();
        double const elapsed = std::chrono::duration<double>(now - m_lastStep).count();
        m_lastStep = now;

        // A lone press moves one split; held, the key accelerates so a tick
        // of any resolution is crossed in about REPEATS_PER_TICK repeats.
        if (elapsed > 2.0 / KEY_REPEAT_HZ) {
            m_carry = 0.0;
            return 1;
        }

        m_carry += static_cast<double>(this->splits()) / REPEATS_PER_TICK;
        int const n = std::max(1, static_cast<int>(m_carry));
        m_carry = std::max(0.0, m_carry - n);
        return n;
    }

    bool SubtickPreview::forward() {
        if (!this->usable())
            return true;

        m_split += this->stride();
        if (m_split < this->splits())
            return false;
        m_split = 0;
        return true;
    }

    bool SubtickPreview::back() {
        if (!this->usable() || m_split <= 0)
            return false;
        m_split = std::max(0, m_split - this->stride());
        return true;
    }

    void SubtickPreview::update(PlayLayer* pl) {
        auto& traj = gucci::TrajectoryPredictionService::get();
        if (!pl) {
            m_split = 0;
            m_dimmed = false;
            m_drawnSplit = -1;
            traj.setOverlaySuppressed(false);
            return;
        }

        auto* gb = gucci::GucciEngine::get();
        // His cbfInUse(): recording with CBF on, or a macro that has offsets.
        // Worked out once per frame here -- the macro scan is linear.
        m_cbfInUse = gb->replay.m_scbfRecording ||
                     (gb->replay.m_subtickPreview && scbf::hasOffsets(gb->replay.m_actionAtom.m_actions));

        uint32_t const frame = gb->updater.getFrame();
        if (frame != m_frame) {
            m_frame = frame;
            m_split = 0;
        }

        bool const previewing = this->usable() && m_split > 0;
        traj.setOverlaySuppressed(previewing);

        if (!previewing) {
            this->hide(pl);
            return;
        }

        // A real press made while stepped into the tick records at this split
        // (LiveRecorder::place reads fraction()); run the tick so it lands.
        auto const changesHold = [pl](PlayerButtonCommand const& cmd) {
            if (!scbf::isLivePress(cmd))
                return false;
            auto* player = cmd.m_isPlayer2 ? pl->m_player2 : pl->m_player1;
            if (!player)
                return false;
            auto const it = player->m_holdingButtons.find(static_cast<int>(cmd.m_button));
            bool const held = it != player->m_holdingButtons.end() && it->second;
            return held != cmd.m_isPush;
        };
        if (scbf::LiveRecorder::get().splitting() &&
            std::any_of(pl->m_queuedButtons.begin(), pl->m_queuedButtons.end(), changesHold))
            gb->updater.stepOnce();

        if (m_drawnSplit == m_split && m_drawnFrame == frame)
            return;
        this->draw(pl);
    }

    void SubtickPreview::hide(PlayLayer* pl) {
        if (auto* node = previewNode(pl, false))
            node->clear();
        if (pl->m_uiLayer) {
            if (auto* label = pl->m_uiLayer->getChildByID("subtick-label"_spr))
                label->removeFromParent();
        }

        if (m_dimmed) {
            if (pl->m_player1)
                pl->m_player1->setOpacity(255);
            if (pl->m_player2)
                pl->m_player2->setOpacity(255);
            m_dimmed = false;
        }
        m_drawnSplit = -1;
    }

    void SubtickPreview::draw(PlayLayer* pl) {
        auto* node = previewNode(pl, true);
        if (!node)
            return;

        node->clear();
        m_drawnSplit = m_split;
        m_drawnFrame = m_frame;

        auto& traj = gucci::TrajectoryPredictionService::get();
        float const f = static_cast<float>(this->fraction());

        auto const drawPlayer = [&](PlayerObject* real, bool p2) {
            if (!real)
                return;
            gucci::TrajectoryPredictionService::SubtickPose pose;
            if (!traj.extrapolateSubtick(pl, real, f, pose))
                return;

            ccColor4F const edge = pose.died ? kDead : kPlayer;
            outline(node, pose.hitbox, pose.rotation, kWidth, kRotated);
            outline(node, pose.hitbox, 0.f, kWidth, edge);
            outline(node, pose.innerHitbox, 0.f, kWidth, kInner);
            node->drawSegment(real->getPosition(), pose.position, kWidth, edge);

            if (pose.died)
                return;
            traj.traceSubtickBranch(pl, real, f, false, node, traj.releaseColor(), kWidth);
            traj.traceSubtickBranch(pl, real, f, true, node, traj.holdColor(p2), kWidth);
        };

        drawPlayer(pl->m_player1, false);
        if (pl->m_gameState.m_isDualMode)
            drawPlayer(pl->m_player2, true);

        if (pl->m_player1)
            pl->m_player1->setOpacity(127);
        if (pl->m_player2)
            pl->m_player2->setOpacity(127);
        m_dimmed = true;

        if (!pl->m_uiLayer)
            return;
        std::string const text = fmt::format(
            "tick {} + {}/{}  ({:.4f})", m_frame, m_split, this->splits(), this->fraction());

        auto* label = typeinfo_cast<CCLabelBMFont*>(pl->m_uiLayer->getChildByID("subtick-label"_spr));
        if (!label) {
            label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
            label->setID("subtick-label"_spr);
            label->setScale(0.6f);
            label->setOpacity(220);
            pl->m_uiLayer->addChild(label, 1001);
        } else {
            label->setString(text.c_str());
        }

        auto const win = CCDirector::sharedDirector()->getWinSize();
        label->setPosition({win.width / 2.f, win.height - 14.f});
    }

} // namespace scbf

// The menu on a touch screen: see ui/touch.hpp. Android and iOS only.

#include "ui/touch.hpp"

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_MOBILE)

#include <Geode/modify/CCTouchDispatcher.hpp>
#include <imgui-cocos.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace gucci::ui::touch {

    namespace {

        // How far (ImGui units) a finger moves before a press is a drag.
        constexpr float kDragStart = 10.f;
        // Room between the menu and the edge of the safe area.
        constexpr float kMargin = 6.f;

        enum class Phase { Idle, Pressed, Scrolling, Lifted, Passed };
        Phase g_phase = Phase::Idle;
        ImGuiID g_pressWindow = 0;   // the window the finger went down on
        ImGuiID g_scrollWindow = 0;  // the window it scrolls (or that is coasting)
        float g_velocity = 0.f;      // scroll units a second, for the coast after it lifts

        // The active id a scroll holds while the finger is down, as ImGui's
        // own scrollbars do: the press it took over can't act, and nothing
        // under the moving finger lights up.
        ImGuiID scrollId() {
            static ImGuiID const s_id = ImHashStr("##gucci_touch_scroll");
            return s_id;
        }

        // The window a vertical drag that started on `w` moves: `w` or the
        // nearest window it sits in with something to scroll, passing over the
        // ones the mouse wheel passes over too. Cards never scroll (they grow
        // to fit and take no wheel), so a drag on one moves the page.
        ImGuiWindow* scrollableFrom(ImGuiWindow* w) {
            while (w) {
                if (w->ScrollMax.y > 0.f && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse))
                    return w;
                w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr;
            }
            return nullptr;
        }

        // Whether the press that became a drag can be taken over. Nothing
        // pressed (empty space, text, a card: ImGui makes the window's move id
        // active there), or any kind of button: buttons, switches, choices and
        // list rows act when the finger lifts, so taking the press away just
        // means they don't. Not a slider or a text box (both claim arrow keys
        // when pressed, and both use the drag), a scrollbar, or an item that
        // takes the mouse wheel itself (the frame editor's timeline, which
        // pans, zooms and box-selects by dragging).
        bool canTakePress(ImGuiContext& g) {
            ImGuiID const id = g.ActiveId;
            ImGuiWindow* w = g.ActiveIdWindow;
            if (id == 0 || (w && id == w->MoveId))
                return true;
            if (g.ActiveIdUsingNavDirMask != 0 || ImGui::GetInputTextState(id))
                return false;
            if (w && (id == ImGui::GetWindowScrollbarID(w, ImGuiAxis_X) || id == ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y)))
                return false;
            return ImGui::GetKeyOwner(ImGuiKey_MouseWheelY) != id;
        }

        void startScroll(ImGuiContext& g) {
            ImGuiIO const& io = g.IO;
            float const dx = io.MousePos.x - io.MouseClickedPos[0].x;
            float const dy = io.MousePos.y - io.MouseClickedPos[0].y;
            if (std::max(std::fabs(dx), std::fabs(dy)) < kDragStart)
                return;  // still a tap
            // Decided once, here: a sideways drag stays the item's (a slider).
            ImGuiWindow* target =
                std::fabs(dy) > std::fabs(dx) ? scrollableFrom(ImGui::FindWindowByID(g_pressWindow)) : nullptr;
            if (!target || !canTakePress(g)) {
                g_phase = Phase::Passed;
                return;
            }
            ImGui::SetActiveID(scrollId(), target);
            g_scrollWindow = target->ID;
            g_phase = Phase::Scrolling;
            g_velocity = 0.f;
            // The page moves with the finger from where it went down.
            ImGui::SetScrollY(target, target->Scroll.y - dy);
        }

        void followFinger(ImGuiContext& g) {
            ImGuiWindow* target = ImGui::FindWindowByID(g_scrollWindow);
            if (!target || g.ActiveId != scrollId()) {
                g_phase = Phase::Passed;
                g_velocity = 0.f;
                return;
            }
            ImGui::KeepAliveID(scrollId());
            float const step = -g.IO.MouseDelta.y;
            ImGui::SetScrollY(target, target->Scroll.y + step);
            // Smoothed: touch events and frames don't arrive in step.
            if (g.IO.DeltaTime > 0.f)
                g_velocity += (step / g.IO.DeltaTime - g_velocity) * 0.3f;
        }

        // After a quick flick the page keeps going and slows to a stop (in
        // about a second), as phone lists do.
        void coast(ImGuiContext& g) {
            if (g_velocity == 0.f)
                return;
            ImGuiWindow* target = ImGui::FindWindowByID(g_scrollWindow);
            float const dt = g.IO.DeltaTime;
            if (!target || !target->WasActive || dt <= 0.f || (g_velocity < 0.f && target->Scroll.y <= 0.f) ||
                (g_velocity > 0.f && target->Scroll.y >= target->ScrollMax.y)) {
                g_velocity = 0.f;
                return;
            }
            ImGui::SetScrollY(target, target->Scroll.y + g_velocity * dt);
            g_velocity *= std::exp(-4.f * dt);
            if (std::fabs(g_velocity) < 20.f)
                g_velocity = 0.f;
        }

    } // namespace

    void frame() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx)
            return;
        ImGuiContext& g = *ctx;
        ImGuiIO const& io = g.IO;

        if (io.MouseClicked[0]) {
            // A new touch stops a coast, and may turn into a scroll.
            if (g.ActiveId == scrollId())
                ImGui::ClearActiveID();
            g_phase = Phase::Pressed;
            g_pressWindow = g.HoveredWindow ? g.HoveredWindow->ID : 0;
            g_velocity = 0.f;
            return;
        }
        if (io.MouseDown[0]) {
            if (g_phase == Phase::Pressed)
                startScroll(g);
            else if (g_phase == Phase::Scrolling)
                followFinger(g);
            return;
        }
        if (g_phase == Phase::Scrolling) {
            // Lifted. The scroll keeps the active id through this frame too,
            // so nothing under the finger takes the release as a click.
            ImGui::KeepAliveID(scrollId());
            g_phase = Phase::Lifted;
        } else {
            if (g_phase == Phase::Lifted && g.ActiveId == scrollId())
                ImGui::ClearActiveID();
            g_phase = Phase::Idle;
        }
        coast(g);
    }

    void screenArea(ImVec2* min, ImVec2* max) {
        ImVec2 const display = ImGui::GetIO().DisplaySize;
        // Geode's safe area is in GD's units, from the bottom left.
        CCRect const safe = geode::utils::getSafeAreaRect();
        ImVec2 a = ImGuiCocos::cocosToFrame(CCPoint(safe.getMinX(), safe.getMaxY()));
        ImVec2 b = ImGuiCocos::cocosToFrame(CCPoint(safe.getMaxX(), safe.getMinY()));
        if (!(b.x > a.x && b.y > a.y)) {
            a = ImVec2(0.f, 0.f);  // an empty or odd answer: the whole screen
            b = display;
        }
        *min = ImVec2(std::max(a.x, 0.f) + kMargin, std::max(a.y, 0.f) + kMargin);
        *max = ImVec2(std::min(b.x, display.x) - kMargin, std::min(b.y, display.y) - kMargin);
    }

} // namespace gucci::ui::touch

// imgui-cocos (its hooks.cpp) hands a touch to ImGui only when
// io.WantCaptureMouse says the menu is under it, and that was worked out at the
// last frame, from where the last touch was: a screen has no hovering. So the
// first tap on the menu after a tap anywhere else went to the game (and could
// press whatever the menu covered), and the first tap on the game after one on
// the menu was eaten. This runs before imgui-cocos and answers for the touch
// that is starting. An open popup still takes every tap (a tap outside closes
// it), and a finger already down keeps what it had.
class $modify(GBMenuTouch, CCTouchDispatcher) {
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("cocos2d::CCTouchDispatcher::touches", Priority::Early);
    }

    void touches(CCSet* touches, CCEvent* event, unsigned int type) {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (type == CCTOUCHBEGAN && touches && g && ImGuiCocos::get().isInitialized() && ImGuiCocos::get().isVisible()) {
            if (auto* touch = static_cast<CCTouch*>(touches->anyObject())) {
                // Between frames, Active means drawn in the last one.
                ImVec2 const at = ImGuiCocos::cocosToFrame(touch->getLocation());
                bool over = false;
                for (ImGuiWindow* w : g->Windows) {
                    if (w->Active && !w->Hidden && !(w->Flags & ImGuiWindowFlags_NoMouseInputs) &&
                        w->OuterRectClipped.ContainsWithPad(at, g->Style.TouchExtraPadding)) {
                        over = true;
                        break;
                    }
                }
                if (over)
                    g->IO.WantCaptureMouse = true;
                else if (!g->IO.MouseDown[0] && g->OpenPopupStack.Size == 0)
                    g->IO.WantCaptureMouse = false;
            }
        }
        CCTouchDispatcher::touches(touches, event, type);
    }
};

#endif

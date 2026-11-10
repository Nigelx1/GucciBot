// Pressing a menu control by its ID (ui/press.hpp), for Assistant Access.

#include "ui/press.hpp"

#include "ui/probe.hpp"

namespace gucci::ui::press {

    namespace detail {
        bool g_hooking = false;
    }

    namespace {

        using detail::g_hooking;

        enum class Phase { Idle, Arm, Pressing, Type, Release, Submit, SubmitUp, Done };

        struct State {
            Request req;
            Phase phase = Phase::Idle;
            int lastFrame = -1;  // the ImGui frame step() last moved on
            int waited = 0;      // frames in this phase
            bool seen = false;   // the item was drawn with the activation on it
            bool disabled = false;
            std::optional<Outcome> outcome;
        };
        State g;

        void hooks(bool on) {
            g_hooking = on;
            if (GImGui)
                GImGui->TestEngineHookItems = on || probe::on();
        }

        void finish(std::string status, bool ok, std::string detail = {}) {
            g.outcome = Outcome{std::move(status), ok, std::move(detail)};
            g.phase = Phase::Done;
            hooks(false);
        }

        // The activation ImGui processes at its next NewFrame, as
        // ActivateItemByID queues it (with the flags a text box needs).
        void activate(ImGuiID id, ImGuiActivateFlags flags) {
            ImGuiContext& ctx = *GImGui;
            ctx.NavNextActivateId = id;
            ctx.NavNextActivateFlags = flags;
        }

        void key(ImGuiKey k, bool down) {
            ImGui::GetIO().AddKeyEvent(k, down);
        }

    } // namespace

    bool start(Request const& r) {
        if (busy() || !GImGui || r.id == 0)
            return false;
        g = State{};
        g.req = r;
        g.phase = Phase::Arm;
        return true;
    }

    bool busy() {
        return g.phase != Phase::Idle && g.phase != Phase::Done;
    }

    std::optional<Outcome> take() {
        if (g.phase != Phase::Done)
            return std::nullopt;
        auto out = std::move(g.outcome);
        g = State{};
        return out;
    }

    void cancel() {
        if (!busy())
            return;
        if (g.phase == Phase::Submit || g.phase == Phase::SubmitUp)
            key(ImGuiKey_Enter, false);
        if (g.phase == Phase::Release)
            key(ImGuiKey_Delete, false);
        // An activation still queued (armed, not yet taken at a NewFrame)
        // would otherwise go through after the cancel, without the disabled
        // check Pressing makes.
        if (GImGui && GImGui->NavNextActivateId == g.req.id)
            GImGui->NavNextActivateId = 0;
        g = State{};
        hooks(false);
    }

    void step() {
        if (!busy() || !GImGui)
            return;
        ImGuiContext& ctx = *GImGui;
        if (ctx.FrameCount == g.lastFrame)
            return;  // once a frame
        g.lastFrame = ctx.FrameCount;
        ImGuiID const id = g.req.id;

        switch (g.phase) {
            case Phase::Arm:
                // This frame's drawing has begun: the activation comes in at
                // the next NewFrame, and the item drawn then takes it.
                ctx.LastActiveId = 0;  // so a press is told from an older one
                activate(id, g.req.text ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None);
                hooks(true);
                g.phase = Phase::Pressing;
                g.waited = 0;
                return;

            case Phase::Pressing:
                // The first step after arming is the press frame itself
                // (drawing not yet done); the one after that reads it.
                if (++g.waited < 2)
                    return;
                if (!g.seen)
                    return finish("not_drawn", false,
                                  "nothing with that ID was drawn the frame of the press (its page or popup shut, "
                                  "or a different ID now)");
                if (g.disabled)
                    return finish("refused_disabled", false, "the control is disabled (greyed out)");
                if (ctx.LastActiveId != id)
                    return finish("not_taken", false, "the item was drawn but didn't take the press (not pressable)");
                if (!g.req.text)
                    return finish("pressed", true);
                if (ctx.ActiveId != id)
                    return finish("no_focus", false, "it didn't open for typing (not a text box or slider)");
                // Everything is selected: what's typed replaces it.
                if (g.req.value.empty()) {
                    key(ImGuiKey_Delete, true);
                    g.phase = Phase::Release;
                } else {
                    ImGui::GetIO().AddInputCharactersUTF8(g.req.value.c_str());
                    g.phase = Phase::Type;
                }
                g.waited = 0;
                return;

            case Phase::Release:
                key(ImGuiKey_Delete, false);
                g.phase = Phase::Type;
                return;

            case Phase::Type:
                // The characters went in at this frame's NewFrame and are
                // being read now: one more frame and they're in the box.
                if (++g.waited < 2)
                    return;
                if (!g.req.submit)
                    return finish("typed", true, "left open for typing (submit false)");
                key(ImGuiKey_Enter, true);
                g.phase = Phase::Submit;
                g.waited = 0;
                return;

            case Phase::Submit:
                key(ImGuiKey_Enter, false);
                g.phase = Phase::SubmitUp;
                g.waited = 0;
                return;

            case Phase::SubmitUp:
                if (++g.waited < 2)
                    return;
                return finish("typed", true, ctx.ActiveId == id ? "Enter pressed; the box is still open" : "");

            default:
                return;
        }
    }

    void hookItemAdd(ImGuiContext* ctx, ImGuiID id, ImGuiLastItemData const* data) {
        if (g.phase != Phase::Pressing || id != g.req.id || ctx->NavActivateId != id)
            return;
        g.seen = true;
        ImGuiItemFlags const flags = data ? data->ItemFlags : ctx->CurrentItemFlags;
        if (flags & ImGuiItemFlags_Disabled) {
            // ImGui would press it anyway (activation by code skips the
            // check): taken back before the widget reads it.
            g.disabled = true;
            ctx->NavActivateId = ctx->NavActivateDownId = ctx->NavActivatePressedId = 0;
        }
    }

} // namespace gucci::ui::press

#pragma once

// Pressing a menu control by its ImGui ID (Assistant Access, 2026-11-10):
// gucci_ui_press (mcp/ui_press_tool.cpp). In a hidden test game no mouse
// reaches ImGui, so the press goes in through ImGui's own activation (what
// keyboard navigation uses, ActivateItemByID): the next frame, the widget
// with that ID sees itself pressed and reports it as clicked or changed, the
// way a real click does, whatever drew it (the kit, a raw MenuItem, a
// Selectable, a combo).
//
// ImGui's activation by code doesn't check BeginDisabled, so this does: the
// test-engine ItemAdd hook (ui/probe.cpp) sees the item as it's submitted and
// a disabled one (greyed out, Legit only) has the activation taken back
// before its widget reads it.
//
// A text box (or a slider, as Ctrl+click makes it one) is activated for
// typing with everything selected, the text goes in as typed characters the
// frame after, and Enter commits it, so whatever a box does on Enter or on
// letting go happens as it does for a person.
//
// Main thread only; one press at a time; nothing is saved.

#include <imgui.h>
#include <imgui_internal.h>

#include <optional>
#include <string>

namespace gucci::ui::press {

    struct Request {
        ImGuiID id = 0;
        bool text = false;        // type `value` into it instead of a click
        std::string value;        // the text (all of it replaces what's there)
        bool submit = true;       // Enter after the text
    };

    struct Outcome {
        // pressed: the widget took the press. typed: the text went in
        // (submitted or not). refused_disabled, not_drawn, no_focus: nothing
        // happened; cancelled.
        std::string status;
        bool ok = false;
        std::string detail;
    };

    // Starts a press; false while another is running.
    bool start(Request const& r);
    bool busy();
    // The finished press's outcome, once.
    std::optional<Outcome> take();
    void cancel();

    // ---- the shell (ui/shell.cpp's frame(), once a frame)
    void step();

    // ---- ui/probe.cpp's hooks
    namespace detail {
        extern bool g_hooking;
    }
    // The item hooks must stay on: a press is in flight.
    inline bool hooking() {
        return detail::g_hooking;
    }
    void hookItemAdd(ImGuiContext* ctx, ImGuiID id, ImGuiLastItemData const* data);

} // namespace gucci::ui::press

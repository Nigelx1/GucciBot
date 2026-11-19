// GucciBot (2026-11-18): gucci_key and gucci_editor, for test launches.
//
// gucci_key sends one key, with Ctrl and/or Shift held, the way Geode hands
// on a real one (Geode 5.10.1 and 6.0.0-alpha.2, loader/src/platform/windows/
// input.cpp, handleKeyboardEvent): KeyboardInputEvent first (where
// imgui-cocos takes keys on Geode 6 while ImGui wants the keyboard), then
// cocos's text input for Backspace, Delete and the arrows (where imgui-cocos
// hands ImGui a Backspace on every press), the keyboard dispatcher's
// modifiers (updateModifierKeys) and dispatchKeyboardMSG (where imgui-cocos
// does the same on Geode 5, and GucciBot's hotkeys and the game see it). A
// held key can repeat, once a frame, as Geode sends a held key's repeats. Nothing goes straight into ImGui,
// so what it does is what a real key does. The modifier goes down a frame
// before the key, as a hand presses it. (gucci_search's key ctrl_f_geode is
// the same for Ctrl+F alone.)
//
// gucci_editor reads the Frame Editor (ui/pages.hpp, editortest) and can give
// its page the keyboard focus (as a click in it does) or take it away, and
// click one of its bars with Ctrl held the same way: a hidden game's ImGui
// has the real cursor every frame, so the click goes in as what a click on
// that bar does to the selection, with Ctrl as the editor reads it.

#include "mcp_server.hpp"

#include "ui/press.hpp"  // imgui_internal.h, before GucciBot's header (its `log`)
#include "ui/pages.hpp"
#include "ui/touch.hpp"
#include "ui/ui.hpp"
#include "hacks/keybinds.hpp"
#include "hacks/safemode.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {

        matjson::Value editorJson() {
            auto const i = ui::pages::editortest::info();
            auto v = matjson::Value::object();
            v["focused"] = i.focused;
            v["keys_live"] = i.keysLive;
            v["items"] = i.items;
            v["selected"] = i.selected;
            v["can_undo"] = i.canUndo;
            v["can_redo"] = i.canRedo;
            v["message"] = i.message;
            v["ctrl_keys_taken"] = i.ctrlKeysTaken;
            v["keys_taken"] = i.keysTaken;
            v["sel_first_frame"] = i.selFirstFrame;
            v["view_start"] = i.viewStart;
            v["frames_per_px"] = i.framesPerPx;
            return v;
        }

        // The text box being typed in, if any: its text and selection.
        matjson::Value textBoxJson() {
            auto v = matjson::Value::object();
            ImGuiContext* g = ImGui::GetCurrentContext();
            v["want_text_input"] = g ? ImGui::GetIO().WantTextInput : false;
            bool const active = g && g->ActiveId != 0 && g->InputTextState.ID == g->ActiveId;
            v["active"] = active;
            if (active) {
                auto& st = g->InputTextState;
                v["text"] = std::string(st.TextA.Data ? st.TextA.Data : "", (size_t)std::max(0, st.CurLenA));
                v["has_selection"] = st.HasSelection();
                v["cursor"] = st.GetCursorPos();
                v["selection"] = std::vector<int>{st.GetSelectionStart(), st.GetSelectionEnd()};
            }
            return v;
        }

        matjson::Value stateJson(int code) {
            auto v = matjson::Value::object();
            auto* gb = GucciEngine::get();
            v["menu_open"] = ui::isOpen();
            v["page"] = ui::pageTitle();
            v["noclip"] = gb->noclipEnabled;
            v["macro_inputs"] = (int64_t)gb->replay.m_actionAtom.m_actions.size();
            v["in_level"] = PlayLayer::get() != nullptr;
            v["paused"] = PlayLayer::get() != nullptr && PlayLayer::get()->m_isPaused;
            v["editor"] = editorJson();
            v["text_box"] = textBoxJson();
            if (code > 0) {
                v["handed_on"] = (int64_t)keybinds::handedOn(code);
                v["kept"] = (int64_t)keybinds::kept(code);
            }
            return v;
        }

#if !defined(GEODE_IS_IOS)
        // `ctrl` and `shift`: held once this key has gone down or up (Geode's
        // KeyStateTracker after it). As Geode does (handleKeyboardEvent), the
        // event's own modifiers also count a Ctrl or Shift key as held on its
        // own event, its release too, and the dispatcher gets the tracker's.
        void geodeKey(enumKeyCodes key, bool down, bool ctrl, bool shift, bool repeat = false) {
            KeyboardModifier tracked = KeyboardModifier::None;
            if (ctrl)
                tracked |= KeyboardModifier::Control;
            if (shift)
                tracked |= KeyboardModifier::Shift;
            KeyboardModifier mods = tracked;
            if (key == KEY_LeftControl || key == KEY_RightContol || key == KEY_Control)
                mods |= KeyboardModifier::Control;
            if (key == KEY_LeftShift || key == KEY_RightShift || key == KEY_Shift)
                mods |= KeyboardModifier::Shift;
            KeyboardInputData data(key,
                                   !down    ? KeyboardInputData::Action::Release
                                   : repeat ? KeyboardInputData::Action::Repeat
                                            : KeyboardInputData::Action::Press,
                                   {0, 0}, 0.0, mods);
            if (KeyboardInputEvent(key).send(data) != ListenerResult::Propagate || data.key == KEY_Unknown)
                return;
            bool const isDown = data.action != KeyboardInputData::Action::Release;
            auto* kb = CCKeyboardDispatcher::get();
            auto* ime = CCIMEDispatcher::sharedDispatcher();
            // cocos's text input, as Geode feeds it (Geode 6 after the
            // modifiers and for Up, Down and Enter too; Geode 5 before them).
            auto text = [&] {
                if (!isDown)
                    return;
                switch (data.key) {
                    case KEY_Backspace: ime->dispatchDeleteBackward(); break;
                    case KEY_Delete: ime->dispatchDeleteForward(); break;
                    case KEY_Left:
                    case KEY_Right: ime->dispatchInsertText("a", 1, data.key); break;
#if GB_GEODE6
                    case KEY_Up:
                    case KEY_Down: ime->dispatchInsertText("a", 1, data.key); break;
                    case KEY_Enter: ime->dispatchInsertText("\n", 1, data.key); break;
#endif
                    default: break;
                }
            };
#if !GB_GEODE6
            text();
#endif
            kb->updateModifierKeys(shift, ctrl, false, false);
#if GB_GEODE6
            text();
#endif
            if (!ime->hasDelegate() || data.key == KEY_Escape || data.key == KEY_Enter)
                kb->dispatchKeyboardMSG(data.key, isDown, data.action == KeyboardInputData::Action::Repeat, 0.0);
        }
#endif

        // A letter or digit, space, escape, delete, backspace, enter, the
        // arrows (left, right, up, down), f1..f12; 0 when unknown.
        int keyCode(std::string name) {
            for (auto& c : name)
                c = (char)std::tolower((unsigned char)c);
            if (name.size() == 1 && std::isalnum((unsigned char)name[0]))
                return std::toupper((unsigned char)name[0]);
            if (name == "space")
                return (int)KEY_Space;
            if (name == "escape")
                return (int)KEY_Escape;
            if (name == "delete")
                return (int)KEY_Delete;
            if (name == "backspace")
                return (int)KEY_Backspace;
            if (name == "enter")
                return (int)KEY_Enter;
            if (name == "left")
                return (int)KEY_Left;
            if (name == "right")
                return (int)KEY_Right;
            if (name == "up")
                return (int)KEY_Up;
            if (name == "down")
                return (int)KEY_Down;
            if (name.size() >= 2 && name[0] == 'f') {
                int const n = std::atoi(name.c_str() + 1);
                if (n >= 1 && n <= 12)
                    return 0x6F + n;
            }
            return 0;
        }

        struct KeyJob {
            int code = 0;
            bool ctrl = false, shift = false;
            int step = 0, wait = 0;
            int repeats = 0, repeated = 0;  // held: repeats sent, one a frame
            int noclipKey = -1;
            bool keyDown = false, ctrlDown = false, shiftDown = false;  // ours, still down
            matjson::Value out = matjson::Value::object();
        };

        void restoreNoclip(KeyJob& j) {
            if (j.noclipKey >= 0) {
                ui::keys().onNoclip = j.noclipKey;
                j.noclipKey = -1;
            }
        }

        // Cut off (timed out, cancelled): what it put down comes up again, so
        // no later test runs with Ctrl held.
        void letGo(KeyJob& j) {
#if !defined(GEODE_IS_IOS)
            if (j.keyDown)
                geodeKey(static_cast<enumKeyCodes>(j.code), false, j.ctrlDown, j.shiftDown);
            if (j.ctrlDown)
                geodeKey(KEY_LeftControl, false, false, j.shiftDown);
            if (j.shiftDown)
                geodeKey(KEY_LeftShift, false, false, false);
#endif
            j.keyDown = j.ctrlDown = j.shiftDown = false;
            restoreNoclip(j);
        }

        std::optional<matjson::Value> pollKey(KeyJob& j) {
#if defined(GEODE_IS_IOS)
            (void)j;
            throw ToolError("no keyboard dispatcher on iOS");
#else
            auto const k = static_cast<enumKeyCodes>(j.code);
            switch (j.step) {
                case 0:  // the modifiers go down
                    j.out["before"] = stateJson(j.code);
                    if (j.shift)
                        geodeKey(KEY_LeftShift, true, false, true);
                    j.shiftDown = j.shift;
                    if (j.ctrl)
                        geodeKey(KEY_LeftControl, true, true, j.shift);
                    j.ctrlDown = j.ctrl;
                    j.step = 1;
                    return std::nullopt;
                case 1: {  // a frame later, the key
                    auto m = matjson::Value::object();
                    m["imgui_ctrl"] = ImGui::GetIO().KeyCtrl;
                    m["imgui_shift"] = ImGui::GetIO().KeyShift;
                    m["cocos_ctrl"] = CCKeyboardDispatcher::get()->getControlKeyPressed();
                    m["ctrl_held"] = ui::ctrlHeld();
                    j.out["while_held"] = m;
                    geodeKey(k, true, j.ctrl, j.shift);
                    j.keyDown = true;
                    j.step = j.repeats > 0 ? 4 : 2;
                    return std::nullopt;
                }
                case 4:  // held: a repeat a frame
                    geodeKey(k, true, j.ctrl, j.shift, true);
                    if (++j.repeated >= j.repeats)
                        j.step = 2;
                    return std::nullopt;
                case 2:  // up again, then the modifiers
                    geodeKey(k, false, j.ctrl, j.shift);
                    j.keyDown = false;
                    if (j.ctrl)
                        geodeKey(KEY_LeftControl, false, false, j.shift);
                    j.ctrlDown = false;
                    if (j.shift)
                        geodeKey(KEY_LeftShift, false, false, false);
                    j.shiftDown = false;
                    j.step = 3;
                    j.wait = 0;
                    return std::nullopt;
                default:
                    // A few frames: the editor acts at its next draw.
                    if (++j.wait < 4)
                        return std::nullopt;
                    // With nothing held: ImGui's Ctrl (stuck down after a
                    // Ctrl release on Geode 6) and ui::ctrlHeld().
                    j.out["after_imgui_ctrl"] = ImGui::GetIO().KeyCtrl;
                    j.out["after_imgui_shift"] = ImGui::GetIO().KeyShift;
                    j.out["after_imgui_alt"] = ImGui::GetIO().KeyAlt;
                    j.out["after_ctrl_held"] = ui::ctrlHeld();
                    j.out["after_shift_held"] = ui::shiftHeld();
                    j.out["after"] = stateJson(j.code);
                    restoreNoclip(j);
                    return j.out;
            }
#endif
        }

        struct EditorJob {
            matjson::Value args;
            int step = 0, wait = 0;
            bool ctrl = false, shift = false;
            bool ctrlDown = false, shiftDown = false;  // ours, still down
            matjson::Value out = matjson::Value::object();
        };

        void letGo(EditorJob& j) {
#if !defined(GEODE_IS_IOS)
            if (j.ctrlDown)
                geodeKey(KEY_LeftControl, false, false, j.shiftDown);
            if (j.shiftDown)
                geodeKey(KEY_LeftShift, false, false, false);
#endif
            j.ctrlDown = j.shiftDown = false;
        }

        std::optional<matjson::Value> pollEditor(EditorJob& j) {
            auto const& a = j.args;
            switch (j.step) {
                case 0:
                    j.out["before"] = editorJson();
                    if (a.contains("focus"))
                        ui::pages::editortest::focus(a["focus"].asBool().unwrapOr(true));
                    if (a["game_click"].asBool().unwrapOr(false))
                        ui::touch::noteGameTouch();
                    if (!a.contains("click") && !a.contains("wheel")) {
                        j.step = 3;
                        return std::nullopt;
                    }
#if !defined(GEODE_IS_IOS)
                    // Ctrl or Shift held a frame before the click or the
                    // wheel, as a hand does.
                    if (j.shift)
                        geodeKey(KEY_LeftShift, true, false, true);
                    j.shiftDown = j.shift;
                    if (j.ctrl)
                        geodeKey(KEY_LeftControl, true, true, j.shift);
                    j.ctrlDown = j.ctrl;
#endif
                    j.step = 1;
                    return std::nullopt;
                case 1:
                    if (a.contains("click"))
                        ui::pages::editortest::clickBar((int)a["click"].asInt().unwrapOr(0));
                    if (a.contains("wheel"))
                        ui::pages::editortest::wheel((float)a["wheel"].asDouble().unwrapOr(0.0));
                    j.step = 2;
                    j.wait = 0;
                    return std::nullopt;
                case 2:
                    if (++j.wait < 2)
                        return std::nullopt;
                    j.out["click_saw_ctrl"] = ui::pages::editortest::info().lastClickCtrl;
                    j.out["shift_held"] = ui::shiftHeld();
#if !defined(GEODE_IS_IOS)
                    if (j.ctrl)
                        geodeKey(KEY_LeftControl, false, false, j.shift);
                    j.ctrlDown = false;
                    if (j.shift)
                        geodeKey(KEY_LeftShift, false, false, false);
                    j.shiftDown = false;
#endif
                    j.step = 3;
                    j.wait = 0;
                    return std::nullopt;
                default:
                    if (++j.wait < 3)
                        return std::nullopt;
                    j.out["after"] = editorJson();
                    return j.out;
            }
        }

        matjson::Value prop(const char* type, const char* description) {
            auto p = matjson::Value::object();
            p["type"] = type;
            p["description"] = description;
            return p;
        }

    } // namespace

    void registerKeysTools(Server& server) {
        {
            auto props = matjson::Value::object();
            props["key"] = prop("string",
                                "a letter or digit, space, escape, delete, backspace, enter, left, right, up, down "
                                "or f1..f12");
            props["repeats"] = prop("integer", "held: this many repeats of the key, one a frame (0..30), before "
                                               "it goes up");
            props["ctrl"] = prop("boolean", "Ctrl held (down a frame before the key, up after it)");
            props["shift"] = prop("boolean", "Shift held, the same way");
            props["bind_noclip"] = prop("boolean",
                                        "Noclip's hotkey bound to this key for the call (put back after, never "
                                        "saved), so the answer's noclip shows whether a hotkey on it fired");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"key"};
            server.addTool({
                "gucci_key",
                "One key, with Ctrl and/or Shift, sent the way Geode hands on a real one (test launches only): "
                "KeyboardInputEvent, the keyboard dispatcher's modifiers, dispatchKeyboardMSG; nothing put "
                "straight into ImGui. Answers before and after: the menu, noclip, the macro's inputs, the Frame "
                "Editor (focused, selection, undo/redo, its message, Ctrl keys it took), the text box being typed "
                "in (text, selection), whether the level is paused, and handed_on/kept: that key's presses "
                "GucciBot's keyboard hook handed on to the game or kept from it. while_held: ImGui's Ctrl, the "
                "dispatcher's, and ui::ctrlHeld().",
                schema,
                [](matjson::Value const& a) -> matjson::Value {
                    if (!gucci::safemode::testLaunch())
                        throw ToolError("test launches only (tools/assist.py launch)");
                    auto job = std::make_shared<KeyJob>();
                    job->code = keyCode(a["key"].asString().unwrapOr(""));
                    if (job->code == 0)
                        throw ToolError("key: a letter or digit, space, escape, delete, backspace, enter, the "
                                        "arrows or f1..f12");
                    job->repeats = std::clamp((int)a["repeats"].asInt().unwrapOr(0), 0, 30);
                    job->ctrl = a["ctrl"].asBool().unwrapOr(false);
                    job->shift = a["shift"].asBool().unwrapOr(false);
                    if (a["bind_noclip"].asBool().unwrapOr(false)) {
                        job->noclipKey = ui::keys().onNoclip;
                        ui::keys().onNoclip = job->code;
                    }
                    throw Defer{[job]() { return pollKey(*job); }, [job] { letGo(*job); }, 120};
                },
            });
        }
        {
            auto props = matjson::Value::object();
            props["focus"] = prop("boolean",
                                  "true: the Frame Editor's page gets the keyboard focus, as a click in it does "
                                  "(Pages layout; gucci_layout focus for Windows); false: nothing has it");
            props["click"] = prop("integer",
                                  "bar n (the n-th click, from 0) clicked: what a left click on it does to the "
                                  "selection, with Ctrl as the editor reads it then");
            props["ctrl"] = prop("boolean",
                                 "with click or wheel: Ctrl held through the game's keyboard (as gucci_key does)");
            props["shift"] = prop("boolean", "with click or wheel: Shift held the same way");
            props["wheel"] = prop("number",
                                  "the mouse wheel turned this much over the lanes (Shift as the editor reads it): "
                                  "the answer's view_start and frames_per_px show the scroll or the zoom");
            props["game_click"] = prop("boolean",
                                       "as if a click had gone to the game (not the menu): the menu lets go of its "
                                       "focus at its next frame");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            server.addTool({
                "gucci_editor",
                "The Frame Editor (test launches only; it acts while its page is drawn). Answers before and "
                "after: focused (had the keyboard at its last draw), keys_live, items, selected, can_undo, "
                "can_redo, message, ctrl_keys_taken, keys_taken, sel_first_frame, view_start, frames_per_px. "
                "focus gives its page the keyboard focus or takes it away; "
                "game_click is a click on the game as the menu notes one; "
                "click (with ctrl) clicks bar n, and click_saw_ctrl says whether the click saw Ctrl held; "
                "wheel (with shift) turns the wheel over the lanes.",
                schema,
                [](matjson::Value const& a) -> matjson::Value {
                    if (!gucci::safemode::testLaunch())
                        throw ToolError("test launches only (tools/assist.py launch)");
                    auto job = std::make_shared<EditorJob>();
                    job->args = a;
                    job->ctrl = a["ctrl"].asBool().unwrapOr(false);
                    job->shift = a["shift"].asBool().unwrapOr(false);
#if defined(GEODE_IS_IOS)
                    if (job->ctrl || job->shift)
                        throw ToolError("no keyboard dispatcher on iOS");
#endif
                    throw Defer{[job]() { return pollEditor(*job); }, [job] { letGo(*job); }, 120};
                },
            });
        }
    }

} // namespace gucci::mcp

// GucciBot (2026-11-15): gucci_search, the menu's search box (ui/search.hpp)
// driven from a test launch. A hidden test game can't be typed into, so the
// query goes in as ImGui's own typed characters (ui/press.hpp, the way
// gucci_ui_press types into a box), a result is picked by pressing its row,
// and Escape, Back and Space go through cocos's own keyboard and keypad
// dispatchers (in the game's process, not as input to its window), so every
// hook on the way sees them as it would a key: imgui-cocos, GucciBot's
// hotkeys, the search's back-click guard, the game.
//
// One call does, in order, whatever it's given: query (typed into the box,
// left open for typing), key (sent while the box is open), pick (the n-th
// result pressed, then the jump waited for), clear.

#include "mcp_server.hpp"

// ImGui's internals (through ui/press.hpp) before GucciBot's header, which
// brings a `log` that imgui_internal.h's own would clash with.
#include "ui/press.hpp"
#include "ui/search.hpp"
#include "ui/ui.hpp"
#include "hacks/safemode.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <memory>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {

        enum class Step { Open, Type, Typing, Index, Key, KeyDown, KeyUp, KeySettle, Pick, Picking, Jump, Done };

        struct Job {
            matjson::Value args;
            Step step = Step::Open;
            int wait = 0;
            int builds = 0;
            std::string key;
            int noclipKey = -1;  // Noclip's own hotkey, while the F tests bind it to F
            matjson::Value out = matjson::Value::object();
            matjson::Value keyBefore;
        };

        matjson::Value gameState() {
            auto v = matjson::Value::object();
            auto* gb = GucciEngine::get();
            v["recording"] = gb->isRecording();
            v["macro_inputs"] = (int64_t)gb->replay.m_actionAtom.m_actions.size();
            auto* pl = PlayLayer::get();
            v["in_level"] = pl != nullptr;
            v["level_paused"] = pl ? pl->m_isPaused : false;
            v["frame"] = (int64_t)gb->updater.getFrame();
            v["noclip"] = gb->noclipEnabled;
            return v;
        }

        matjson::Value boxState() {
            auto v = matjson::Value::object();
            v["query"] = ui::search::query();
            v["box_active"] = ui::search::fieldActive();
            v["showing_results"] = ui::search::showing();
            v["page"] = ui::pageTitle();
            v["compact"] = ui::compactMode();
            v["menu_has_keyboard"] = ImGui::GetIO().WantCaptureKeyboard;
            return v;
        }

        matjson::Value resultsJson() {
            std::vector<matjson::Value> list;
            int n = 0;
            for (auto const& r : ui::search::currentResults()) {
                auto e = matjson::Value::object();
                e["n"] = n++;
                e["label"] = r.label;
                e["where"] = r.where;
                e["shown_as"] = r.label + " - " + r.where;
                e["kind"] = r.kind;
                e["score"] = r.score;
                list.push_back(std::move(e));
            }
            return matjson::Value(list);
        }

        matjson::Value jumpJson() {
            auto const j = ui::search::jumpState();
            auto v = matjson::Value::object();
            v["phase"] = j.phase;
            v["page"] = j.page;
            v["target"] = j.target;
            v["highlighted_kind"] = j.shownKind;
            v["highlighted_label"] = j.shownLabel;
            v["rect"] = std::vector<double>{j.rect.x, j.rect.y, j.rect.z, j.rect.w};
            v["in_view"] = j.inView;
            v["highlight_left_s"] = j.highlightLeft;
            v["frames"] = j.frames;
            return v;
        }

        void sendKey(std::string const& key, bool down) {
            auto* dir = CCDirector::get();
            if (key == "back") {
                if (down)
                    dir->getKeypadDispatcher()->dispatchKeypadMSG(kTypeBackClicked);
                return;
            }
#if !defined(GEODE_IS_IOS)
            enumKeyCodes const code = key == "escape" ? KEY_Escape : (key == "f" || key == "ctrl_f") ? KEY_F : KEY_Space;
            if (code == KEY_F)
                ImGui::GetIO().AddKeyEvent(ImGuiKey_F, down);
            dir->getKeyboardDispatcher()->dispatchKeyboardMSG(code, down, false, 0.0);
#else
            (void)down;
#endif
        }

        std::optional<matjson::Value> poll(Job& j) {
            auto const& a = j.args;
            switch (j.step) {
                case Step::Open:
                    // The box is in the menu: open it (the page or Compact
                    // Mode it was on) and give it a frame to be drawn.
                    if (!ui::isOpen())
                        ui::setOpen(true);
                    // Compact Mode on or off first, as its switch in Settings
                    // does (saved, as that switch saves it).
                    if (j.wait == 0 && a.contains("compact"))
                        ui::setCompactMode(a["compact"].asBool().unwrapOr(false));
                    if (++j.wait < 3)
                        return std::nullopt;
                    j.wait = 0;
                    j.out["before"] = boxState();
                    j.step = a.contains("query") ? Step::Type : a.contains("key") ? Step::Key
                             : a.contains("pick")                                ? Step::Pick
                                                                                  : Step::Done;
                    return std::nullopt;

                case Step::Type: {
                    if (ui::search::fieldId() == 0)
                        throw ToolError("the search box hasn't been drawn (is the menu showing?)");
                    if (ui::press::busy())
                        return std::nullopt;
                    // Already open for typing, ImGui won't take it again:
                    // let go first, as a click elsewhere would, and type the
                    // frame after next.
                    if (ui::search::fieldActive()) {
                        ImGui::ClearActiveID();
                        j.wait = 0;
                        return std::nullopt;
                    }
                    if (++j.wait < 2)
                        return std::nullopt;
                    ui::press::Request r;
                    r.id = ui::search::fieldId();
                    r.text = true;
                    r.value = a["query"].asString().unwrapOr("");
                    r.submit = false;  // left open, as while someone types
                    j.builds = ui::search::indexBuilds();
                    if (!ui::press::start(r))
                        throw ToolError("another press is running");
                    j.step = Step::Typing;
                    return std::nullopt;
                }

                case Step::Typing: {
                    auto o = ui::press::take();
                    if (!o)
                        return std::nullopt;
                    j.out["typed"] = o->status;
                    if (!o->ok) {
                        j.out["detail"] = o->detail;
                        j.out["after"] = boxState();
                        return j.out;
                    }
                    j.step = Step::Index;
                    j.wait = 0;
                    return std::nullopt;
                }

                case Step::Index:
                    // Clicked into, the box builds a fresh index; the results
                    // follow the frame after.
                    if (ui::search::indexBuilds() == j.builds && ++j.wait < 120)
                        return std::nullopt;
                    if (++j.wait < 3)
                        return std::nullopt;
                    j.out["index_size"] = ui::search::indexSize();
                    j.out["index_builds"] = ui::search::indexBuilds();
                    j.out["results"] = resultsJson();
                    j.out["message"] = ui::search::emptyMessage();
                    j.out["after_typing"] = boxState();
                    j.wait = 0;
                    j.step = a.contains("key") ? Step::Key : a.contains("pick") ? Step::Pick : Step::Done;
                    return std::nullopt;

                case Step::Key: {
                    j.key = a["key"].asString().unwrapOr("");
                    if (j.key != "escape" && j.key != "back" && j.key != "space" && j.key != "f" && j.key != "ctrl_f")
                        throw ToolError("key is escape, back, space, f or ctrl_f");
#if defined(GEODE_IS_IOS)
                    if (j.key != "back")
                        throw ToolError("no keyboard dispatcher on iOS");
#endif
                    // f and ctrl_f: Noclip's hotkey is F for the test (put
                    // back after it, never saved), so the answer's noclip
                    // shows whether a hotkey on F fired. ctrl_f holds Ctrl in
                    // ImGui a frame before F goes down, as a hand would.
                    bool const fKey = j.key == "f" || j.key == "ctrl_f";
                    if (fKey) {
                        j.noclipKey = ui::keys().onNoclip;
                        ui::keys().onNoclip = static_cast<int>(KEY_F);
                    }
                    auto before = boxState();
                    before["game"] = gameState();
                    j.keyBefore = before;
                    if (j.key == "ctrl_f") {
                        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
                        ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
                        j.step = Step::KeyDown;
                        return std::nullopt;
                    }
                    sendKey(j.key, true);
                    j.step = Step::KeyUp;
                    return std::nullopt;
                }

                case Step::KeyDown:
                    j.keyBefore["ctrl_held"] = ImGui::GetIO().KeyCtrl;
                    sendKey(j.key, true);
                    j.step = Step::KeyUp;
                    return std::nullopt;

                case Step::KeyUp:
                    sendKey(j.key, false);
                    if (j.key == "ctrl_f") {
                        ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
                        ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
                    }
                    j.step = Step::KeySettle;
                    j.wait = 0;
                    return std::nullopt;

                case Step::KeySettle: {
                    // A few frames: a press the game took is in the macro by then.
                    if (++j.wait < 6)
                        return std::nullopt;
                    auto k = matjson::Value::object();
                    k["key"] = j.key;
                    k["before"] = j.keyBefore;
                    auto after = boxState();
                    after["game"] = gameState();
                    k["after"] = after;
                    j.out["key"] = k;
                    if (j.noclipKey >= 0) {
                        ui::keys().onNoclip = j.noclipKey;
                        j.noclipKey = -1;
                    }
                    j.step = a.contains("pick") ? Step::Pick : Step::Done;
                    return std::nullopt;
                }

                case Step::Pick: {
                    int const n = (int)a["pick"].asInt().unwrapOr(0);
                    auto const ids = ui::search::resultIds();
                    if (n < 0 || n >= (int)ids.size() || ids[(size_t)n] == 0)
                        throw ToolError(fmt::format("no result {} is drawn ({} are)", n, ids.size()));
                    if (ui::press::busy())
                        return std::nullopt;
                    auto const res = ui::search::currentResults();
                    if ((size_t)n < res.size())
                        j.out["picked"] = res[(size_t)n].label + " - " + res[(size_t)n].where;
                    ui::press::Request r;
                    r.id = ids[(size_t)n];
                    if (!ui::press::start(r))
                        throw ToolError("another press is running");
                    j.step = Step::Picking;
                    return std::nullopt;
                }

                case Step::Picking: {
                    auto o = ui::press::take();
                    if (!o)
                        return std::nullopt;
                    j.out["pick_press"] = o->status;
                    if (!o->ok) {
                        j.out["detail"] = o->detail;
                        return j.out;
                    }
                    j.step = Step::Jump;
                    j.wait = 0;
                    return std::nullopt;
                }

                case Step::Jump: {
                    auto const s = ui::search::jumpState();
                    if (s.phase == "seeking" && ++j.wait < 60)
                        return std::nullopt;
                    j.out["jump"] = jumpJson();
                    j.out["after_pick"] = boxState();
                    j.step = Step::Done;
                    return std::nullopt;
                }

                case Step::Done:
                    if (a.contains("clear") && a["clear"].asBool().unwrapOr(false))
                        ui::search::clear();
                    j.out["now"] = boxState();
                    if (!j.out.contains("jump") && ui::search::jumpState().phase != "")
                        j.out["last_jump"] = jumpJson();
                    return j.out;
            }
            return std::nullopt;
        }

        matjson::Value schema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("query", "string", "typed into the box (all of it, over what's there), left open for typing");
            field("key", "string",
                  "escape, back, space, f or ctrl_f: sent through cocos's keyboard (escape, space, f) or keypad (back) "
                  "dispatcher (f and ctrl_f bind Noclip's hotkey to F for the test; ctrl_f holds Ctrl in ImGui first) "
                  "while the box is as the query left it; answers the box and the game (macro inputs, paused) "
                  "before and after");
            field("pick", "integer", "the n-th result (from 0) pressed; answers where the jump went");
            field("clear", "boolean", "empty the box at the end");
            field("compact", "boolean", "first switch Compact Mode on or off (saved, as its switch in Settings is)");
            auto s = matjson::Value::object();
            s["type"] = "object";
            s["properties"] = props;
            return s;
        }

    } // namespace

    void registerSearchTools(Server& server) {
        server.addTool({
            "gucci_search",
            "The menu's search box (test launches only). query: typed into it as ImGui characters (the menu is "
            "opened if it's shut); answers the results as the menu lists them ({label, where, shown_as, kind}), "
            "the no-results message, and the index size. key: escape, back or space sent while the box is open, "
            "through cocos's own dispatchers, with the box and the game before and after (a space the game took "
            "shows as a macro input while recording). pick: presses the n-th result's row and waits for the "
            "jump: the page, what was highlighted (the control, or the switch or card it sits under) and whether "
            "it's in view. clear: empties the box. compact: Compact Mode on or off first (saved, as its switch is).",
            schema(),
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                if (ui::press::busy())
                    throw ToolError("a press is still running");
                auto job = std::make_shared<Job>();
                job->args = a;
                throw Defer{[job]() { return poll(*job); }, [] { ui::press::cancel(); }, 400};
            },
        });
    }

} // namespace gucci::mcp

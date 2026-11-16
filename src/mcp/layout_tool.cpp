// GucciBot (2026-11-16): gucci_layout, the menu's Windows layout (ui/layout.hpp)
// for a test launch. A hidden test game can't be dragged with a mouse (and
// imgui-cocos gives ImGui the real cursor every frame, so none can be faked
// in), so a move goes in as what a finished drag does: the window's place is
// set and saved at its next draw, through the same code. Collapse likewise
// (what a click on a window's arrow does). The layout itself is switched with
// the real Settings > Look > Layout control (gucci_ui_press); `mode` is here
// for a test that can't reach it.
//
// One call does, in order, what it's given: mode, show, hide, move, collapse,
// reset; then answers the state as of the last frame drawn (call again after
// a frame or two to see a change land).

#include "mcp_server.hpp"

#include "hacks/safemode.hpp"
#include "ui/layout.hpp"
#include "ui/pages.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {
        matjson::Value prop(const char* type, const char* description) {
            auto p = matjson::Value::object();
            p["type"] = type;
            p["description"] = description;
            return p;
        }

        matjson::Value state() {
            auto out = matjson::Value::object();
            out["available"] = ui::layout::available();
            out["mode"] = ui::layout::windows() ? "windows" : "pages";
            out["open"] = ui::isOpen();
            out["compact"] = ui::compactMode();
            out["page"] = ui::pageTitle();
            auto shown = matjson::Value::array();
            for (auto const& p : ui::layout::shownPages())
                shown.push(p);
            out["shown"] = shown;
            auto list = matjson::Value::array();
            for (auto const& w : ui::layout::windowList()) {
                auto e = matjson::Value::object();
                e["key"] = w.key;
                e["title"] = w.title;
                e["page"] = w.page;
                e["page_window"] = w.pageWindow;
                e["drawn"] = w.drawn;
                e["x"] = w.rect.x;
                e["y"] = w.rect.y;
                e["w"] = w.rect.z;
                e["h"] = w.rect.w;
                e["collapsed"] = w.collapsed;
                e["folded"] = w.folded;
                e["placed"] = w.placed;
                e["saved_x"] = w.saved.x;
                e["saved_y"] = w.saved.y;
                list.push(e);
            }
            out["windows"] = list;
            out["focused"] = ui::layout::focusedKey();
            out["editor_keys_live"] = ui::pages::editorKeysLive();
            out["saved"] = ui::layout::savedText();
            out["saved_mode"] = Mod::get()->getSavedValue<std::string>("ui_layout", "");
            return out;
        }
    } // namespace

    void registerLayoutTools(Server& server) {
        auto props = matjson::Value::object();
        props["mode"] = prop("string", "pages or windows (as Settings > Look > Layout sets it)");
        props["show"] = prop("string", "a page's title: its windows show");
        props["hide"] = prop("string", "a page's title: its windows hide");
        props["move"] = prop("object", "{key, x, y}: a window put there, as a drag that ended there");
        props["collapse"] = prop("object", "{key, collapsed}: a window collapsed or opened, as its arrow does");
        props["focus"] = prop("string", "a window's key: it gets the keyboard focus, as a click in it does");
        props["reset"] = prop("boolean", "every window back to tiling, none collapsed (Tile them again)");
        auto schema = matjson::Value::object();
        schema["type"] = "object";
        schema["properties"] = props;

        server.addTool({
            "gucci_layout",
            "The menu's layout (Settings > Look > Layout), test launches only. Answers mode (pages or windows), "
            "available (false on phones), shown (pages whose windows show), windows (each card's window: key, "
            "title, page, drawn last frame, x/y/w/h, collapsed, placed by a drag, saved_x/y) and saved (the "
            "ui_windows saved value), focused (the key of the window with the keyboard focus) and editor_keys_live (the "
            "Frame Editor's shortcuts listened at its last draw). mode/show/hide/move/collapse/focus/reset change it "
            "at the next frame.",
            schema,
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                auto out = matjson::Value::object();
                if (a.contains("mode")) {
                    auto const m = a["mode"].asString().unwrapOr("");
                    if (m != "pages" && m != "windows")
                        throw ToolError("mode is pages or windows");
                    if (m == "windows" && !ui::layout::available())
                        throw ToolError("phones keep Pages");
                    ui::layout::setWindows(m == "windows");
                }
                if (a.contains("show"))
                    ui::layout::setShown(a["show"].asString().unwrapOr(""), true);
                if (a.contains("hide"))
                    ui::layout::setShown(a["hide"].asString().unwrapOr(""), false);
                if (a.contains("move")) {
                    auto const& m = a["move"];
                    auto const key = m["key"].asString().unwrapOr("");
                    ImVec2 const pos(static_cast<float>(m["x"].asDouble().unwrapOr(0.0)),
                                     static_cast<float>(m["y"].asDouble().unwrapOr(0.0)));
                    if (!ui::layout::requestMove(key, pos))
                        throw ToolError("no window has the key \"" + key + "\" (see windows[].key)");
                }
                if (a.contains("collapse")) {
                    auto const& c = a["collapse"];
                    auto const key = c["key"].asString().unwrapOr("");
                    if (!ui::layout::requestCollapse(key, c["collapsed"].asBool().unwrapOr(true)))
                        throw ToolError("no window has the key \"" + key + "\" (see windows[].key)");
                }
                if (a.contains("focus")) {
                    auto const key = a["focus"].asString().unwrapOr("");
                    if (!ui::layout::requestFocus(key))
                        throw ToolError("no window has the key \"" + key + "\" (see windows[].key)");
                }
                if (a.contains("reset") && a["reset"].asBool().unwrapOr(false))
                    ui::layout::resetPlaces();
                return state();
            },
        });
    }

} // namespace gucci::mcp

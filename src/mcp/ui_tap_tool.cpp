// GucciBot (2026-11-09): gucci_ui_tap, for driving any cocos UI from Assistant
// Access in a test launch: GD's, Geode's and other mods' buttons, with no mouse
// and nothing shown on the screen. gucci_scene lists the nodes (class, ID, text,
// box, path); this presses one:
// - activate: a menu item's callback, the way CCMenu runs it when a click ends
//   on it (selected, unselected, activate). A label or sprite inside a button
//   finds the button it is in.
// - touch: a touch at a point (or a node's middle) through GD's touch
//   dispatcher, began now and ended `frames` frames later, so whatever is on
//   top (a popup, a menu) takes it as a click there.
// - pause: GD's pause menu (in a level); back: the Escape/back key; set_text:
//   a text box's text, as typed.
// First written to drive Antic's Counter in an MCP-only launch
// (core/mcp_only.hpp). Test launches only: it presses whatever it is told to.

#include "mcp_server.hpp"

#include "hacks/safemode.hpp"
#include "mcp/scene.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {

        bool containsNoCase(std::string_view hay, std::string_view needle) {
            if (needle.empty())
                return true;
            auto const it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return it != hay.end();
        }

        std::string strArg(matjson::Value const& a, char const* key) {
            if (!a.contains(key))
                return {};
            if (auto s = a[key].asString())
                return *s;
            return {};
        }

        std::optional<double> numArg(matjson::Value const& a, char const* key) {
            if (!a.contains(key))
                return std::nullopt;
            if (auto d = a[key].asDouble())
                return *d;
            if (auto i = a[key].asInt())
                return static_cast<double>(*i);
            return std::nullopt;
        }

        // A node by its gucci_scene path: "0/2/1" from the running scene,
        // "overlay/0" from Geode's overlay (notifications).
        CCNode* byPath(std::string const& path) {
            std::string_view rest = path;
            CCNode* node = nullptr;
            if (rest.rfind("overlay", 0) == 0) {
                node = OverlayManager::get();
                rest.remove_prefix(7);
            } else if (rest.rfind("0", 0) == 0) {
                node = CCDirector::get()->getRunningScene();
                rest.remove_prefix(1);
            } else {
                throw ToolError("a path starts at 0 (the scene) or overlay, as gucci_scene lists them");
            }
            while (!rest.empty() && node) {
                if (rest[0] != '/')
                    throw ToolError("a path is child indices split by /, e.g. 0/1/3");
                rest.remove_prefix(1);
                auto const slash = rest.find('/');
                auto const part = std::string(rest.substr(0, slash));
                rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
                int idx = -1;
                try {
                    idx = std::stoi(part);
                } catch (...) {
                    throw ToolError("not a child index: " + part);
                }
                auto* children = node->getChildren();
                if (!children || idx < 0 || idx >= static_cast<int>(children->count()))
                    throw ToolError(fmt::format("no child {} there (the screen changed? list it again)", idx));
                node = static_cast<CCNode*>(children->objectAtIndex(idx));
            }
            if (!node)
                throw ToolError("no node at " + path);
            return node;
        }

        struct Filter {
            std::string id, cls, text;
            bool any() const {
                return !id.empty() || !cls.empty() || !text.empty();
            }
            bool matches(CCNode* node) const {
                if (!id.empty() && !containsNoCase(node->getID(), id))
                    return false;
                if (!cls.empty() && !containsNoCase(scene::className(node), cls))
                    return false;
                if (!text.empty()) {
                    bool found = false;
                    for (auto const& t : scene::textsUnder(node, 6))
                        if (containsNoCase(t, text)) {
                            found = true;
                            break;
                        }
                    if (!found)
                        return false;
                }
                return true;
            }
        };

        struct Match {
            CCNode* node;
            std::string path;
        };

        // Drawn nodes that match, depth first in drawing order. A level's
        // objects are not walked (KOCMOC has ~200k).
        void collect(CCNode* node, std::string const& path, int depth, Filter const& f, std::vector<Match>& out) {
            if (!node || depth > 64 || out.size() >= 200 || !node->isVisible())
                return;
            if (f.matches(node) && scene::drawn(node))
                out.push_back({node, path});
            auto* pl = PlayLayer::get();
            if (pl && node == pl->m_objectLayer)
                return;
            int i = 0;
            for (auto* child : CCArrayExt<CCNode*>(node->getChildren()))
                collect(child, path + "/" + std::to_string(i++), depth + 1, f, out);
        }

        matjson::Value describe(CCNode* node, std::string const& path) {
            auto e = matjson::Value::object();
            e["class"] = scene::className(node);
            if (!node->getID().empty())
                e["id"] = std::string(node->getID());
            if (!path.empty())
                e["path"] = path;
            e["box"] = scene::boxJson(scene::boxOf(node, false));
            auto const texts = scene::textsUnder(node, 6);
            if (!texts.empty())
                e["texts"] = texts;
            if (auto* item = typeinfo_cast<CCMenuItem*>(node))
                e["enabled"] = item->isEnabled();
            return e;
        }

        // The menu item a node is or is in.
        CCMenuItem* itemOf(CCNode* node) {
            for (int up = 0; node && up < 8; ++up, node = node->getParent())
                if (auto* item = typeinfo_cast<CCMenuItem*>(node))
                    return item;
            return nullptr;
        }

        // A text box a node is or holds.
        CCNode* textBoxOf(CCNode* node, int depth = 0) {
            if (!node || depth > 8)
                return nullptr;
            if (typeinfo_cast<TextInput*>(node) || typeinfo_cast<CCTextInputNode*>(node))
                return node;
            for (auto* child : CCArrayExt<CCNode*>(node->getChildren()))
                if (auto* found = textBoxOf(child, depth + 1))
                    return found;
            return nullptr;
        }

        // A touch in flight between its began and its end.
        struct Touch {
            Ref<CCSet> set;
            int framesLeft = 0;
            bool live = false;
        };
        Touch g_touch;

        void endTouch() {
            if (!g_touch.live)
                return;
            g_touch.live = false;
            if (g_touch.set)
                CCDirector::get()->getTouchDispatcher()->touchesEnded(g_touch.set, nullptr);
            g_touch.set = nullptr;
        }

        matjson::Value tapSchema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("action", "string",
                  "activate (default with a node: run a menu item's callback, as a click on it does), touch (a "
                  "click at x,y or on the node's middle, through GD's touch dispatcher), pause (GD's pause menu, "
                  "in a level), back (the Escape/back key) or set_text (a text box's text)");
            field("path", "string", "the node by its gucci_scene path (0/1/3...; overlay/... for Geode's overlay)");
            field("id", "string", "the node by a part of its ID (ignoring case)");
            field("class", "string", "the node by a part of its class (e.g. CCMenuItemSpriteExtra)");
            field("text", "string", "the node by a part of a text on or under it (a button's label)");
            field("index", "integer", "which of several matches (default 0, in drawing order)");
            field("x", "number", "touch: the point, in cocos units from the left (gucci_scene's boxes)");
            field("y", "number", "touch: the point, in cocos units from the bottom");
            field("frames", "integer", "touch: frames the touch is held before it ends (default 1)");
            field("value", "string", "set_text: the text");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            return schema;
        }

    } // namespace

    void registerUiTapTools(Server& server) {
        server.addTool({
            "gucci_ui_tap",
            "Press something on GD's screen (test launches only): a menu item activated as a click on it "
            "does, a touch at a point or on a node, GD's pause menu, the back key, or a text box's text. "
            "Find the node with gucci_scene first (path, id, class or text, as it lists them); a label "
            "inside a button finds the button. Answers with what it pressed, how many nodes matched and "
            "the first few of them. A touch ends `frames` frames later, so the answer comes then.",
            tapSchema(),
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                auto action = strArg(a, "action");
                auto out = matjson::Value::object();

                if (action == "pause") {
                    auto* pl = PlayLayer::get();
                    if (!pl)
                        throw ToolError("not in a level");
                    if (pl->m_isPaused)
                        throw ToolError("already paused");
                    pl->pauseGame(false);
                    out["paused"] = true;
                    return out;
                }
                if (action == "back") {
                    CCDirector::get()->getKeypadDispatcher()->dispatchKeypadMSG(kTypeBackClicked);
                    out["back"] = true;
                    return out;
                }

                // The node, when one is named.
                CCNode* target = nullptr;
                std::string targetPath;
                auto const path = strArg(a, "path");
                Filter f{strArg(a, "id"), strArg(a, "class"), strArg(a, "text")};
                if (!path.empty()) {
                    target = byPath(path);
                    targetPath = path;
                } else if (f.any()) {
                    std::vector<Match> found;
                    collect(CCDirector::get()->getRunningScene(), "0", 0, f, found);
                    collect(OverlayManager::get(), "overlay", 0, f, found);
                    out["matches"] = static_cast<int64_t>(found.size());
                    auto first = matjson::Value::array();
                    for (size_t i = 0; i < found.size() && i < 8; ++i)
                        first.push(describe(found[i].node, found[i].path));
                    out["first_matches"] = first;
                    auto const index = static_cast<int>(numArg(a, "index").value_or(0));
                    if (found.empty())
                        throw ToolError("nothing drawn matches (list the screen with gucci_scene)");
                    if (index < 0 || index >= static_cast<int>(found.size()))
                        throw ToolError(fmt::format("index {} of {} matches", index, found.size()));
                    target = found[index].node;
                    targetPath = found[index].path;
                }

                if (action.empty())
                    action = target ? "activate" : "touch";

                if (action == "activate") {
                    if (!target)
                        throw ToolError("activate needs a node (path, id, class or text)");
                    auto* item = itemOf(target);
                    if (!item)
                        throw ToolError("not a menu item and not in one (try action: touch)");
                    out["pressed"] = describe(item, item == target ? targetPath : std::string());
                    if (!item->isEnabled())
                        throw ToolError("that menu item is disabled");
                    // As CCMenu does it when a click ends on the item. Kept
                    // alive through its own callback (which may close its popup).
                    Ref<CCMenuItem> keep = item;
                    item->selected();
                    item->unselected();
                    item->activate();
                    out["activated"] = true;
                    return out;
                }

                if (action == "set_text") {
                    if (!target)
                        throw ToolError("set_text needs a node (path, id, class or text)");
                    auto* box = textBoxOf(target);
                    if (!box)
                        throw ToolError("no text box there");
                    auto const value = strArg(a, "value");
                    if (auto* input = typeinfo_cast<TextInput*>(box)) {
                        input->setString(value, true);
                    } else if (auto* node = typeinfo_cast<CCTextInputNode*>(box)) {
                        node->setString(value);
                        if (node->m_delegate)
                            node->m_delegate->textChanged(node);
                    }
                    out["text_box"] = describe(box, std::string());
                    out["set"] = value;
                    return out;
                }

                if (action == "touch") {
                    if (g_touch.live)
                        throw ToolError("a touch is still held");
                    float x = 0.f, y = 0.f;
                    if (auto const px = numArg(a, "x"), py = numArg(a, "y"); px && py) {
                        x = static_cast<float>(*px);
                        y = static_cast<float>(*py);
                    } else if (target) {
                        auto const b = scene::boxOf(target, false);
                        x = b.x + b.w / 2.f;
                        y = b.y + b.h / 2.f;
                        out["on"] = describe(target, targetPath);
                    } else {
                        throw ToolError("touch needs x and y, or a node");
                    }
                    auto const win = CCDirector::get()->getWinSize();
                    // A touch's own point is the view's (y down); its location
                    // (what handlers read) is that turned back into cocos units.
                    auto* touch = new CCTouch();
                    touch->autorelease();
                    touch->setTouchInfo(0, x, win.height - y);
                    g_touch.set = CCSet::create();
                    g_touch.set->addObject(touch);
                    g_touch.live = true;
                    g_touch.framesLeft = std::max(1, static_cast<int>(numArg(a, "frames").value_or(1)));
                    CCDirector::get()->getTouchDispatcher()->touchesBegan(g_touch.set, nullptr);
                    out["touch"] = std::vector<double>{x, y};
                    throw Defer{[out]() -> std::optional<matjson::Value> {
                                    if (--g_touch.framesLeft > 0)
                                        return std::nullopt;
                                    endTouch();
                                    auto done = out;
                                    done["ended"] = true;
                                    return done;
                                },
                                [] { endTouch(); }, 600};
                }

                throw ToolError("action is activate, touch, pause, back or set_text");
            },
        });
    }

} // namespace gucci::mcp

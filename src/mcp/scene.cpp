// GD's screens as data for Assistant Access (mcp/scene.hpp).

#include "mcp/scene.hpp"

#include "mcp/mcp_server.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/OverlayManager.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

using namespace geode::prelude;

namespace gucci::mcp::scene {

    namespace {

        matjson::Value obj() {
            return matjson::Value::object();
        }

        bool containsNoCase(std::string_view hay, std::string_view needle) {
            if (needle.empty())
                return true;
            auto const it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return it != hay.end();
        }

        // A label's or a text box's own text ("" for any other node). Text
        // boxes (CCTextInputNode, Geode's TextInput) hold a CCTextFieldTTF or a
        // label of their own, which this finds as the walk reaches them.
        std::string ownText(CCNode* node) {
            if (auto* bm = typeinfo_cast<CCLabelBMFont*>(node))
                return bm->getString() ? std::string(bm->getString()) : std::string();
            if (auto* ttf = typeinfo_cast<CCLabelTTF*>(node))
                return ttf->getString() ? std::string(ttf->getString()) : std::string();
            return "";
        }

        CCRect worldRect(CCNode* node, CCRect const& local) {
            return CCRectApplyAffineTransform(local, node->nodeToWorldTransform());
        }

        CCRect unite(CCRect const& a, CCRect const& b) {
            if (a.size.width <= 0.f && a.size.height <= 0.f && a.origin.x == 0.f && a.origin.y == 0.f)
                return b;
            float const x0 = std::min(a.getMinX(), b.getMinX());
            float const y0 = std::min(a.getMinY(), b.getMinY());
            float const x1 = std::max(a.getMaxX(), b.getMaxX());
            float const y1 = std::max(a.getMaxY(), b.getMaxY());
            return CCRect(x0, y0, x1 - x0, y1 - y0);
        }

        // A draw node's box is its vertices' (its content size is 0): what
        // Calculate's markers, the ghosts, the macro path and the hitboxes
        // draw with.
        bool drawNodeBox(CCDrawNode* dn, CCRect& out) {
            if (!dn->m_pBuffer || dn->m_nBufferCount <= 0)
                return false;
            float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
            for (GLsizei i = 0; i < dn->m_nBufferCount; ++i) {
                auto const& v = dn->m_pBuffer[i].vertices;
                x0 = std::min(x0, v.x);
                y0 = std::min(y0, v.y);
                x1 = std::max(x1, v.x);
                y1 = std::max(y1, v.y);
            }
            out = worldRect(dn, CCRect(x0, y0, x1 - x0, y1 - y0));
            return true;
        }

        CCRect rectOf(CCNode* node) {
            if (auto* dn = typeinfo_cast<CCDrawNode*>(node)) {
                CCRect r;
                if (drawNodeBox(dn, r))
                    return r;
            }
            auto const size = node->getContentSize();
            return worldRect(node, CCRect(0.f, 0.f, size.width, size.height));
        }

        Box toBox(CCRect const& r) {
            return Box{r.origin.x, r.origin.y, r.size.width, r.size.height};
        }

        bool onScreen(Box const& b) {
            auto const win = CCDirector::get()->getWinSize();
            // A point (a container with no size) counts by where it is.
            return b.x + b.w >= 0.f && b.y + b.h >= 0.f && b.x <= win.width && b.y <= win.height;
        }

        CCNode* overlayRoot() {
            return OverlayManager::get();
        }

        // The node with this ID under `root`, depth first.
        CCNode* findById(CCNode* root, std::string_view id, int depth = 0) {
            if (!root || depth > 64)
                return nullptr;
            if (root->getID() == id)
                return root;
            for (auto* child : CCArrayExt<CCNode*>(root->getChildren()))
                if (auto* found = findById(child, id, depth + 1))
                    return found;
            return nullptr;
        }

        float pxPerUnit() {
            auto* director = CCDirector::get();
            auto const win = director->getWinSize();
            auto* view = director->getOpenGLView();
            if (!view || win.width <= 0.f)
                return 1.f;
            return view->getFrameSize().width * geode::utils::getDisplayFactor() / win.width;
        }

        std::string fmtNum(float v) {
            return fmt::format("{:.1f}", v);
        }

        struct Walker {
            Options const& opt;
            bool text;
            PlayLayer* pl = PlayLayer::get();
            matjson::Value nodes = matjson::Value::array();
            std::string lines;
            int listed = 0;
            int notListed = 0;  // past `max`
            int objectsSkipped = 0;
            int visits = 0;
            bool finding = !opt.findId.empty() || !opt.findClass.empty() || !opt.findText.empty();

            bool matches(CCNode* node, std::string const& cls, std::string const& own) const {
                // The ID: a part of it, or any of several split by "|".
                if (!opt.findId.empty()) {
                    bool any = false;
                    std::string_view rest = opt.findId;
                    while (!any) {
                        auto const bar = rest.find('|');
                        any = containsNoCase(node->getID(), rest.substr(0, bar)) && !rest.substr(0, bar).empty();
                        if (bar == std::string_view::npos)
                            break;
                        rest = rest.substr(bar + 1);
                    }
                    if (!any)
                        return false;
                }
                if (!opt.findClass.empty() && !containsNoCase(cls, opt.findClass))
                    return false;
                if (!opt.findText.empty()) {
                    if (containsNoCase(own, opt.findText))
                        return true;
                    // A container whose labels say it (a notification, a popup).
                    for (auto const& t : textsUnder(node, 6))
                        if (containsNoCase(t, opt.findText))
                            return true;
                    return false;
                }
                return true;
            }

            void record(CCNode* node, std::string const& path, int depth, bool shown) {
                std::string const cls = className(node);
                std::string const own = ownText(node);
                if (finding && !matches(node, cls, own))
                    return;
                if (listed >= opt.max) {
                    notListed++;
                    return;
                }
                listed++;
                Box const box = toBox(rectOf(node));
                std::vector<std::string> under;
                if (finding && own.empty())
                    under = textsUnder(node, 8);
                if (text) {
                    std::string line(static_cast<size_t>(finding ? 0 : depth) * 2, ' ');
                    line += cls;
                    if (!node->getID().empty())
                        line += " #" + std::string(node->getID());
                    if (!own.empty())
                        line += " \"" + own + "\"";
                    for (auto const& t : under)
                        line += " \"" + t + "\"";
                    line += fmt::format(" @({},{}) {}x{}", fmtNum(box.x), fmtNum(box.y), fmtNum(box.w), fmtNum(box.h));
                    if (!shown)
                        line += " hidden";
                    else if (!onScreen(box))
                        line += " offscreen";
                    if (finding)
                        line += "  [" + path + "]";
                    lines += line + "\n";
                    return;
                }
                auto e = obj();
                e["path"] = path;
                e["depth"] = (int64_t)depth;
                e["class"] = cls;
                if (!node->getID().empty())
                    e["id"] = std::string(node->getID());
                if (node->getTag() != -1)
                    e["tag"] = (int64_t)node->getTag();
                e["z"] = (int64_t)node->getZOrder();
                e["box"] = boxJson(box);
                e["shown"] = shown;
                if (!onScreen(box))
                    e["offscreen"] = true;
                if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(node)) {
                    e["opacity"] = (int64_t)rgba->getDisplayedOpacity();
                    auto const c = rgba->getColor();
                    e["color"] = std::vector<int64_t>{c.r, c.g, c.b};
                }
                e["scale"] = (double)node->getScale();
                if (!own.empty())
                    e["text"] = own;
                if (!under.empty())
                    e["texts"] = under;
                if (auto* item = typeinfo_cast<CCMenuItem*>(node))
                    e["enabled"] = item->isEnabled();
                if (auto* dn = typeinfo_cast<CCDrawNode*>(node))
                    e["vertices"] = (int64_t)dn->m_nBufferCount;
                e["children"] = (int64_t)node->getChildrenCount();
                nodes.push(e);
            }

            void visit(CCNode* node, std::string const& path, int depth, bool parentsVisible) {
                if (!node || ++visits > 200000)
                    return;
                bool const visible = parentsVisible && node->isVisible();
                bool shown = visible;
                if (shown)
                    if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(node))
                        shown = rgba->getDisplayedOpacity() > 0;
                if (shown)
                    shown = onScreen(toBox(rectOf(node)));
                if (!opt.visibleOnly || shown)
                    record(node, path, depth, shown);
                // An invisible node's children aren't drawn either.
                if (depth >= opt.depth || (opt.visibleOnly && !visible))
                    return;
                bool const objectLayer = pl && node == pl->m_objectLayer && !opt.objects;
                int i = 0;
                for (auto* child : CCArrayExt<CCNode*>(node->getChildren())) {
                    std::string const childPath = path + "/" + std::to_string(i++);
                    if (objectLayer && child->getID().empty()) {
                        objectsSkipped += 1 + static_cast<int>(child->getChildrenCount());
                        continue;
                    }
                    visit(child, childPath, depth + 1, visible);
                }
            }
        };

    } // namespace

    std::string className(CCObject const* object) {
        return object ? std::string(geode::cocos::getObjectName(object)) : std::string();
    }

    std::vector<std::string> textsUnder(CCNode* node, int maxDepth) {
        std::vector<std::string> out;
        std::function<void(CCNode*, int)> walkText = [&](CCNode* n, int depth) {
            if (!n || depth > maxDepth || out.size() >= 64)
                return;
            if (auto t = ownText(n); !t.empty())
                out.push_back(std::move(t));
            for (auto* child : CCArrayExt<CCNode*>(n->getChildren()))
                walkText(child, depth + 1);
        };
        walkText(node, 0);
        return out;
    }

    Box boxOf(CCNode* node, bool deep) {
        if (!node)
            return {};
        CCRect r = rectOf(node);
        if (deep) {
            std::function<void(CCNode*, int)> grow = [&](CCNode* n, int depth) {
                if (depth > 12)
                    return;
                for (auto* child : CCArrayExt<CCNode*>(n->getChildren())) {
                    if (!child->isVisible())
                        continue;
                    CCRect const c = rectOf(child);
                    if (c.size.width > 0.f || c.size.height > 0.f)
                        r = unite(r, c);
                    grow(child, depth + 1);
                }
            };
            grow(node, 0);
        }
        return toBox(r);
    }

    bool offscreen(Box const& b) {
        auto const win = CCDirector::get()->getWinSize();
        // Half a unit of slack for rounding at the edges.
        return b.x < -0.5f || b.y < -0.5f || b.x + b.w > win.width + 0.5f || b.y + b.h > win.height + 0.5f;
    }

    matjson::Value boxJson(Box const& b) {
        auto round1 = [](float v) {
            return std::round(static_cast<double>(v) * 10.0) / 10.0;
        };
        auto arr = matjson::Value::array();
        arr.push(round1(b.x));
        arr.push(round1(b.y));
        arr.push(round1(b.w));
        arr.push(round1(b.h));
        return arr;
    }

    bool drawn(CCNode* node) {
        for (CCNode* n = node; n; n = n->getParent())
            if (!n->isVisible())
                return false;
        if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(node))
            return rgba->getDisplayedOpacity() > 0;
        return true;
    }

    matjson::Value gdLabels(PlayLayer* pl) {
        auto out = obj();
        if (!pl)
            return out;
        auto label = [](CCLabelBMFont* l) {
            auto e = obj();
            e["exists"] = l != nullptr;
            if (l) {
                // Shown: drawn, and on the screen at all (the attempts label
                // scrolls off with the level); offscreen: a part of it is off.
                Box const box = boxOf(l, false);
                e["shown"] = drawn(l) && onScreen(box) && box.w > 0.f;
                e["offscreen"] = offscreen(box);
                e["text"] = std::string(l->getString() ? l->getString() : "");
                e["box"] = boxJson(box);
            } else {
                e["shown"] = false;
            }
            return e;
        };
        out["percentage"] = label(pl->m_percentageLabel);
        out["attempts"] = label(pl->m_attemptLabel);
        auto bar = obj();
        bar["exists"] = pl->m_progressBar != nullptr;
        bar["shown"] = pl->m_progressBar && drawn(pl->m_progressBar);
        if (pl->m_progressBar)
            bar["box"] = boxJson(boxOf(pl->m_progressBar, false));
        out["progress_bar"] = bar;
        return out;
    }

    matjson::Value alertOf(CCNode* alert) {
        // The buttons' texts, and every other text in drawn order: the title
        // first, then the body's lines.
        std::vector<std::string> buttons;
        std::vector<std::string> texts;
        std::function<void(CCNode*, int, bool)> collect = [&](CCNode* n, int depth, bool inButton) {
            if (!n || depth > 16)
                return;
            bool const button = inButton || typeinfo_cast<CCMenuItem*>(n) != nullptr;
            if (auto t = ownText(n); !t.empty())
                (button ? buttons : texts).push_back(std::move(t));
            for (auto* c : CCArrayExt<CCNode*>(n->getChildren()))
                collect(c, depth + 1, button);
        };
        collect(alert, 0, false);
        auto e = obj();
        e["class"] = className(alert);
        if (!alert->getID().empty())
            e["id"] = std::string(alert->getID());
        e["title"] = texts.empty() ? std::string() : texts.front();
        std::string body;
        for (size_t i = 1; i < texts.size(); ++i)
            body += (body.empty() ? "" : "\n") + texts[i];
        e["body"] = body;
        e["buttons"] = buttons;
        e["shown"] = drawn(alert);
        return e;
    }

    matjson::Value alerts() {
        auto out = matjson::Value::array();
        auto scan = [&](CCNode* root) {
            if (!root)
                return;
            for (auto* child : CCArrayExt<CCNode*>(root->getChildren()))
                if (typeinfo_cast<FLAlertLayer*>(child))
                    out.push(alertOf(child));
        };
        scan(CCDirector::get()->getRunningScene());
        scan(overlayRoot());
        return out;
    }

    matjson::Value notifications() {
        auto out = matjson::Value::array();
        auto scan = [&](CCNode* root) {
            if (!root)
                return;
            for (auto* child : CCArrayExt<CCNode*>(root->getChildren())) {
                if (!typeinfo_cast<Notification*>(child))
                    continue;
                std::string text;
                for (auto const& t : textsUnder(child))
                    text += (text.empty() ? "" : "\n") + t;
                Box const box = boxOf(child, true);
                auto e = obj();
                e["text"] = text;
                e["box"] = boxJson(box);
                e["offscreen"] = offscreen(box);
                out.push(e);
            }
        };
        scan(overlayRoot());
        scan(CCDirector::get()->getRunningScene());
        return out;
    }

    matjson::Value walk(Options const& options, bool text) {
        auto* director = CCDirector::get();
        auto* running = director->getRunningScene();
        auto const win = director->getWinSize();
        auto out = obj();
        CCNode* shown = running;
        bool const transition = running && typeinfo_cast<CCTransitionScene*>(running) != nullptr;
        // The scene's first layer names the screen (MenuLayer, PlayLayer...).
        std::string screen;
        if (running && running->getChildrenCount() > 0)
            if (auto* first = typeinfo_cast<CCNode*>(running->getChildren()->objectAtIndex(0)))
                screen = className(first);
        out["scene"] = screen;
        out["transition"] = transition;
        out["win"] = std::vector<double>{win.width, win.height};
        out["px_per_unit"] = std::round(static_cast<double>(pxPerUnit()) * 1000.0) / 1000.0;
        auto* pl = PlayLayer::get();
        if (pl)
            out["gd"] = gdLabels(pl);
        out["popups"] = alerts();
        out["notifications"] = notifications();

        Walker w{options, text};
        auto const& root = options.root;
        bool const byId = root.rfind("id:", 0) == 0;
        if (root != "all" && root != "scene" && root != "overlay" && root != "ui" && !byId)
            throw ToolError("root is all, scene, overlay, ui or id:<node id>");
        if (root == "all" || root == "scene")
            w.visit(shown, "0", 0, true);
        if (root == "all" || root == "overlay")
            w.visit(overlayRoot(), "overlay", 0, true);
        if (root == "ui") {
            if (!pl || !pl->m_uiLayer)
                throw ToolError("not in a level (root ui is the level's UI layer)");
            w.visit(pl->m_uiLayer, "ui", 0, true);
        }
        if (byId) {
            auto const id = std::string_view(root).substr(3);
            CCNode* found = findById(running, id);
            if (!found)
                found = findById(overlayRoot(), id);
            if (!found)
                throw ToolError("no node with the ID \"" + std::string(id) + "\"");
            w.visit(found, "id", 0, found->getParent() ? drawn(found->getParent()) : true);
        }

        if (text)
            out["lines"] = w.lines;
        else
            out["nodes"] = w.nodes;
        auto trunc = obj();
        trunc["nodes"] = (int64_t)w.notListed;
        trunc["objects_skipped"] = (int64_t)w.objectsSkipped;
        out["listed"] = (int64_t)w.listed;
        out["truncated"] = trunc;
        return out;
    }

} // namespace gucci::mcp::scene

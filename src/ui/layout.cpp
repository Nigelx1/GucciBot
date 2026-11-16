// The Windows layout's state and its card windows (ui/layout.hpp). Written
// for GucciBot 2.0 (2026-11-16) from the shell's own menu window and the kit.

#include <imgui.h>
#include <imgui_internal.h>  // MovingWindow, FindWindowByName, BringWindowToDisplayFront (before Geode: its `log`)

#include "ui/layout.hpp"
#include "ui/look.hpp"
#include "ui/search.hpp"

#include <Geode/loader/Mod.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

namespace gucci::ui::layout {

    namespace {
        constexpr float kMargin = 8.f;  // from the screen's edges
        constexpr float kGap = 8.f;     // between windows

        struct Rec {
            std::string title;
            std::string page;
            bool pageWindow = false;
            ImVec2 pos{};             // where it was dragged to (placed)
            bool placed = false;
            bool collapsed = false;
            bool moving = false;      // being dragged last frame
            ImVec2 dragFrom{};
            bool folded = false;      // shown as its title bar for want of room
            int drawnFrame = -1;
            ImVec4 rect{};
            std::optional<ImVec2> moveTo;          // a test's move, at the next draw
            std::optional<bool> collapseTo;        // ... and collapse
            bool focusTo = false;                  // ... and focus
        };

        bool g_windows = false;
        std::vector<std::string> g_shown;
        std::map<std::string, Rec> g_recs;
        bool g_dirty = false;

        // This frame
        bool g_drawing = false;
        std::string g_page;
        std::map<std::string, int> g_counts;
        std::vector<bool> g_stack;  // per open card: it has its own window
        std::map<std::string, bool> g_pageHad;  // a page drew something outside its cards last frame
        std::string g_raise;  // a page just shown: its windows come to the front
        int g_raiseFrame = -1;

        // The tiling: columns across the screen, each new window in the
        // shortest one (masonry), so a page that fits the screen shows with
        // nothing over anything.
        float g_width = 340.f;
        float g_top = 60.f;
        float g_bottom = 600.f;
        std::vector<float> g_cols;  // each column's next free y
        int g_cascade = 0;          // windows past the screen's room so far
        int g_folds = 0;            // windows folded for want of room, this frame
        int g_foldsLast = 0;        // ... and last frame: room kept for their title bars
        ImGuiWindow* g_seekWin = nullptr;  // a search result's window, drawn this frame

        std::string const kSaveKey = "ui_windows";
        std::string const kModeKey = "ui_layout";

        matjson::Value toJson() {
            auto v = matjson::Value::object();
            auto shown = matjson::Value::array();
            for (auto const& p : g_shown)
                shown.push(p);
            v["shown"] = shown;
            auto w = matjson::Value::object();
            for (auto const& [key, r] : g_recs) {
                if (!r.placed && !r.collapsed)
                    continue;
                auto e = matjson::Value::object();
                e["x"] = std::round(r.pos.x);
                e["y"] = std::round(r.pos.y);
                e["p"] = r.placed;
                e["c"] = r.collapsed;
                w[key] = e;
            }
            v["w"] = w;
            return v;
        }

        void save() {
            geode::Mod::get()->setSavedValue<std::string>(kSaveKey, toJson().dump(matjson::NO_INDENTATION));
            g_dirty = false;
        }

        float columnX(size_t i) {
            return kMargin + static_cast<float>(i) * (g_width + kGap);
        }

        // Where the next tiled window goes, `want` high as its content asks;
        // `maxH` is how tall it may be there (less than `want`: it scrolls).
        // `fold`: the screen has no room left for it open, so it shows as its
        // title bar alone until it's opened (and then stays where it is).
        ImVec2 tile(float want, float& maxH, bool& fold) {
            fold = false;
            float const full = std::max(120.f, g_bottom - g_top);
            float const h = std::min(want, full);
            size_t best = 0;
            for (size_t i = 1; i < g_cols.size(); ++i)
                if (g_cols[i] < g_cols[best] - 0.5f)
                    best = i;
            float const y = g_cols[best];
            float const room = g_bottom - y;
            float const titleH = ImGui::GetFrameHeight();
            // Room kept at the bottom of each column for last frame's folded
            // title bars, by a window that would take the rest of it.
            float const cols = static_cast<float>(std::max<size_t>(1, g_cols.size()));
            float const keep = std::ceil(static_cast<float>(g_foldsLast) / cols) * (titleH + kGap);
            if (y + h <= g_bottom + 0.5f) {
                // It fits.
                maxH = full;
                g_cols[best] = y + h + kGap;
                return ImVec2(std::round(columnX(best)), std::round(y));
            }
            if (room - keep >= 160.f) {
                // Most of it does: the rest scrolls inside it.
                maxH = room - keep;
                g_cols[best] = y + maxH + kGap;
                return ImVec2(std::round(columnX(best)), std::round(y));
            }
            // Its title bar still fits under the others: folded there.
            ++g_folds;
            fold = true;
            maxH = full;
            if (room >= titleH) {
                g_cols[best] = y + titleH + kGap;
                return ImVec2(std::round(columnX(best)), std::round(y));
            }
            // Not even that: stacked a little apart from the top left, over
            // the others (collapse or drag some to sort it).
            int const k = 1 + (g_cascade++ % 8);
            float const off = 28.f * static_cast<float>(k);
            return ImVec2(std::round(kMargin + off), std::round(g_top + off));
        }

        // How tall a window's content asks it to be (last frame's), the
        // title bar alone while collapsed.
        float wanted(ImGuiWindow const* win, bool collapsed) {
            if (!win)
                return 90.f;
            if (collapsed)
                return win->TitleBarHeight;
            return win->ContentSizeIdeal.y + win->WindowPadding.y * 2.f + win->DecoOuterSizeY1;
        }

        // On the screen: the whole width if it fits, the title bar at least,
        // and under the bar (which stays in front and takes the clicks, so a
        // window left under it couldn't be seen or grabbed).
        ImVec2 inside(ImVec2 p, ImVec2 size) {
            ImVec2 const display = ImGui::GetIO().DisplaySize;
            float const titleH = ImGui::GetFrameHeight();
            float const maxY = std::max(0.f, display.y - titleH);
            p.x = std::clamp(p.x, 0.f, std::max(0.f, display.x - std::min(size.x, display.x)));
            p.y = std::clamp(p.y, std::min(g_top, maxY), maxY);
            return ImVec2(std::round(p.x), std::round(p.y));
        }

        bool movingIs(ImGuiWindow* w) {
            ImGuiContext const& g = *GImGui;
            return w && g.MovingWindow && g.MovingWindow->RootWindow == w;
        }

        bool ours(ImGuiWindow const* w) {
            if (!w)
                return false;
            char const* name = w->Name;
            return std::strstr(name, "###gbw:") || std::strcmp(name, "###gucci_bar") == 0 ||
                   std::strcmp(name, "###gucci_search_results") == 0;
        }

        // A card's (or a page's) window, begun.
        void beginWindow(std::string const& key, Rec& r) {
            ImGuiIO const& io = ImGui::GetIO();
            auto const& pal = look().pal;
            std::string const name = r.title + "###gbw:" + key;
            ImGuiWindow* win = ImGui::FindWindowByName(name.c_str());
            bool const moving = movingIs(win);
            ImVec2 const size = win ? win->Size : ImVec2(g_width, 90.f);
            float maxH = std::max(120.f, io.DisplaySize.y - 2.f * kMargin);

            if (r.moveTo) {
                r.pos = *r.moveTo;
                r.placed = true;
                r.moveTo.reset();
                g_dirty = true;
            }
            // A test's collapse: what a click on the window's arrow does (the
            // toggle ImGui applies in Begin), so it lands the same way.
            if (r.collapseTo) {
                if (win && win->Collapsed != *r.collapseTo) {
                    win->WantCollapseToggle = true;
                } else if (!win) {
                    r.collapsed = *r.collapseTo;
                    g_dirty = true;
                }
                r.collapseTo.reset();
            }
            // A search result in this card: open, and in front.
            bool const seek = !r.pageWindow && search::seekingCard(r.page, r.title);
            if (seek && r.collapsed) {
                r.collapsed = false;
                g_dirty = true;
            }
            bool fold = false;
            if (!r.placed && !moving) {
                ImVec2 const at = tile(wanted(win, r.collapsed), maxH, fold);
                // Sought while folded: opened where its column starts, and
                // it stays there.
                if (fold && seek) {
                    fold = false;
                    r.placed = true;
                    r.pos = ImVec2(at.x, g_top);
                    g_dirty = true;
                    ImGui::SetNextWindowPos(r.pos, ImGuiCond_Always);
                } else {
                    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
                }
            } else {
                // Dragged (or being dragged): as tall as the room below it,
                // so its last rows scroll into view rather than hang off the
                // bottom of the screen.
                float y = win ? win->Pos.y : r.pos.y;
                if (!moving) {
                    ImVec2 const want = inside(r.pos, size);
                    if (!win || std::abs(win->Pos.x - want.x) > 0.5f || std::abs(win->Pos.y - want.y) > 0.5f)
                        ImGui::SetNextWindowPos(want, ImGuiCond_Always);
                    y = want.y;
                }
                maxH = std::max(120.f, io.DisplaySize.y - kMargin - y);
            }
            bool const shut = r.collapsed || fold;
            if (!win || win->Collapsed != shut)
                ImGui::SetNextWindowCollapsed(shut, ImGuiCond_Always);
            if (seek || r.focusTo)
                ImGui::SetNextWindowFocus();
            r.focusTo = false;
            ImGui::SetNextWindowSize(ImVec2(g_width, 0.f), ImGuiCond_Always);
            ImGui::SetNextWindowSizeConstraints(ImVec2(g_width, 0.f), ImVec2(g_width, maxH));

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, mix(pal.backdrop, pal.accent, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_Border, outline(pal));
            ImGui::Begin(name.c_str(), nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            ImGuiWindow* now = ImGui::GetCurrentWindow();
            bool const movingNow = movingIs(now);
            if (movingNow && !r.moving)
                r.dragFrom = now->Pos;
            // A drag that ended somewhere else: it stays there. (A click that
            // didn't move it leaves a tiled window tiled.)
            if (r.moving && !movingNow &&
                (std::abs(now->Pos.x - r.dragFrom.x) > 0.5f || std::abs(now->Pos.y - r.dragFrom.y) > 0.5f)) {
                r.pos = now->Pos;
                r.placed = true;
                g_dirty = true;
            }
            r.moving = movingNow;
            if (fold) {
                // Folded for want of room, and opened with its arrow (or a
                // double-click): it stays open, moved up to the top of its
                // column, over the others, until it's dragged or tiled again.
                if (!now->Collapsed) {
                    r.placed = true;
                    r.collapsed = false;
                    r.pos = ImVec2(now->Pos.x, g_top);
                    g_dirty = true;
                    ImGui::BringWindowToDisplayFront(now);
                }
            } else if (now->Collapsed != r.collapsed) {
                r.collapsed = now->Collapsed;
                g_dirty = true;
            }
            r.folded = fold;
            if (g_raise == r.page && ImGui::GetFrameCount() - g_raiseFrame <= 1)
                ImGui::BringWindowToDisplayFront(now);
            if (seek)
                g_seekWin = now;
            r.drawnFrame = ImGui::GetFrameCount();
            r.rect = ImVec4(now->Pos.x, now->Pos.y, now->Size.x, now->Size.y);
        }

        void addShown(std::string const& page) {
            if (std::find(g_shown.begin(), g_shown.end(), page) == g_shown.end())
                g_shown.push_back(page);
        }
    } // namespace

    bool available() {
#if defined(GEODE_IS_MOBILE)
        return false;
#else
        return true;
#endif
    }

    bool windows() {
        return available() && g_windows;
    }

    void setWindows(bool on) {
        g_windows = on;
        geode::Mod::get()->setSavedValue<std::string>(kModeKey, on ? "windows" : "pages");
    }

    void load() {
        auto* mod = geode::Mod::get();
        g_windows = mod->getSavedValue<std::string>(kModeKey, "pages") == "windows";
        g_shown.clear();
        g_recs.clear();
        auto parsed = matjson::parse(mod->getSavedValue<std::string>(kSaveKey, ""));
        if (!parsed.isOk())
            return;
        auto const v = parsed.unwrap();
        if (!v.isObject())
            return;
        auto const& shown = v["shown"];
        for (size_t i = 0; shown.isArray() && i < shown.size(); ++i)
            if (auto s = shown[i].asString(); s.isOk())
                addShown(s.unwrap());
        auto const& w = v["w"];
        if (!w.isObject())
            return;
        for (auto const& [key, e] : w) {
            if (!e.isObject())
                continue;
            Rec r;
            r.pos.x = static_cast<float>(e["x"].asDouble().unwrapOr(0.0));
            r.pos.y = static_cast<float>(e["y"].asDouble().unwrapOr(0.0));
            r.placed = e["p"].asBool().unwrapOr(false);
            r.collapsed = e["c"].asBool().unwrapOr(false);
            g_recs[key] = r;
        }
    }

    bool shown(std::string const& page) {
        return std::find(g_shown.begin(), g_shown.end(), page) != g_shown.end();
    }

    void setShown(std::string const& page, bool on) {
        bool const was = shown(page);
        if (on && !was) {
            g_shown.push_back(page);
            g_dirty = true;
        } else if (!on && was) {
            g_shown.erase(std::find(g_shown.begin(), g_shown.end(), page));
            g_dirty = true;
        }
        if (on) {
            g_raise = page;
            g_raiseFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() + 1 : 0;
        }
        if (g_dirty && !g_drawing)
            save();
    }

    std::vector<std::string> shownPages() {
        return g_shown;
    }

    void frameBegin(float top) {
        ImVec2 const display = ImGui::GetIO().DisplaySize;
        g_width = std::round(std::clamp(display.x * 0.22f, 300.f, 380.f) * std::clamp(look().textScale, 0.8f, 1.4f));
        g_width = std::max(160.f, std::min(g_width, display.x - 2.f * kMargin));
        g_top = std::round(top);
        g_bottom = display.y - kMargin;
        int const n = std::max(1, static_cast<int>((display.x - 2.f * kMargin + kGap) / (g_width + kGap)));
        g_cols.assign(static_cast<size_t>(n), g_top);
        g_cascade = 0;
        g_foldsLast = g_folds;
        g_folds = 0;
        g_seekWin = nullptr;
    }

    void frameEnd() {
        // A search result's window over everything else of its page (they
        // came forward as the page was shown).
        if (g_seekWin)
            ImGui::BringWindowToDisplayFront(g_seekWin);
        g_seekWin = nullptr;
        if (g_dirty)
            save();
    }

    void pageBegin(const char* page) {
        g_page = page ? page : "";
        g_counts.clear();
        g_stack.clear();
        std::string const key = g_page + "/";
        if (g_pageHad[g_page]) {
            Rec& r = g_recs[key];
            r.title = g_page;
            r.page = g_page;
            r.pageWindow = true;
            beginWindow(key, r);
        } else {
            // Nothing outside its cards last frame: off screen, unseen, and
            // still drawn, to see whether that has changed.
            ImGui::SetNextWindowPos(ImVec2(-30000.f, -30000.f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(g_width, 200.f), ImGuiCond_Always);
            ImGui::Begin(("###gbw_quiet:" + key).c_str(), nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        }
        g_drawing = true;
    }

    void pageEnd() {
        g_drawing = false;
        // Anything laid out in it (the kit's gaps between cards aren't).
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        g_pageHad[g_page] = w->DC.CursorMaxPos.y > w->DC.CursorStartPos.y + 0.5f;
        ImGui::End();
        g_stack.clear();
    }

    bool drawingPage() {
        return g_drawing;
    }

    bool cardBegin(const char* title) {
        if (!g_drawing || !g_stack.empty()) {
            g_stack.push_back(false);
            return false;
        }
        std::string const t = title && *title ? std::string(title) : g_page;
        std::string const base = g_page + "/" + t;
        int const n = g_counts[base]++;
        std::string const key = base + "#" + std::to_string(n);
        Rec& r = g_recs[key];
        r.title = t;
        r.page = g_page;
        r.pageWindow = false;
        beginWindow(key, r);
        g_stack.push_back(true);
        return true;
    }

    bool cardEnd() {
        if (g_stack.empty())
            return false;
        bool const own = g_stack.back();
        g_stack.pop_back();
        if (own)
            ImGui::End();
        return own;
    }

    bool atPageLevel() {
        return g_drawing && g_stack.empty();
    }

    bool cardFocused(const char* title) {
        ImGuiContext const* g = ImGui::GetCurrentContext();
        if (!g_drawing || !g || !g->NavWindow || !g->NavWindow->RootWindow || !title)
            return false;
        std::string const tag = "###gbw:" + g_page + "/" + title + "#";
        return std::strstr(g->NavWindow->RootWindow->Name, tag.c_str()) != nullptr;
    }

    bool ownsFocus() {
        ImGuiContext const* g = ImGui::GetCurrentContext();
        if (!g || !g->NavWindow)
            return false;
        return ours(g->NavWindow->RootWindow);
    }

    std::vector<WindowInfo> windowList() {
        std::vector<WindowInfo> out;
        int const frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
        for (auto const& [key, r] : g_recs) {
            WindowInfo i;
            i.key = key;
            i.title = r.title;
            i.page = r.page;
            i.rect = r.rect;
            i.collapsed = r.collapsed;
            i.folded = r.folded;
            i.placed = r.placed;
            i.saved = r.pos;
            i.drawn = r.drawnFrame >= 0 && frame - r.drawnFrame <= 1;
            i.pageWindow = r.pageWindow;
            out.push_back(std::move(i));
        }
        return out;
    }

    bool requestMove(std::string const& key, ImVec2 pos) {
        auto it = g_recs.find(key);
        if (it == g_recs.end())
            return false;
        it->second.moveTo = pos;
        return true;
    }

    bool requestCollapse(std::string const& key, bool collapsed) {
        auto it = g_recs.find(key);
        if (it == g_recs.end())
            return false;
        it->second.collapseTo = collapsed;
        return true;
    }

    bool requestFocus(std::string const& key) {
        auto it = g_recs.find(key);
        if (it == g_recs.end())
            return false;
        it->second.focusTo = true;
        return true;
    }

    std::string focusedKey() {
        ImGuiContext const* g = ImGui::GetCurrentContext();
        if (!g || !g->NavWindow || !g->NavWindow->RootWindow)
            return "";
        char const* at = std::strstr(g->NavWindow->RootWindow->Name, "###gbw:");
        return at ? std::string(at + 7) : std::string();
    }

    std::string savedText() {
        return geode::Mod::get()->getSavedValue<std::string>(kSaveKey, "");
    }

    void resetPlaces() {
        for (auto& [key, r] : g_recs) {
            r.placed = false;
            r.collapsed = false;
            r.folded = false;
            r.collapseTo.reset();
            r.moveTo.reset();
        }
        save();
    }

} // namespace gucci::ui::layout

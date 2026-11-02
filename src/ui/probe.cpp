// The menu probe (ui/probe.hpp): one ImGui frame of GucciBot's interface as
// data, for Assistant Access.

#include "ui/probe.hpp"

#include <Geode/Geode.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <unordered_map>

namespace gucci::ui::probe {

    namespace detail {
        bool g_on = false;
    }

    namespace {

        using detail::g_on;

        using Ctx = std::vector<std::pair<std::string, std::string>>;

        // One thing the menu showed: a kit card, row, text or control, or an
        // ImGui item the kit didn't draw ("item").
        struct Element {
            int seq = 0;            // the order it was drawn in
            std::string win;        // its window's key (keyOf)
            std::string card;       // the card it's in ("" for none)
            std::string row;        // the row it's in
            std::string kind;
            std::string label;
            std::string text;
            std::string tone;
            matjson::Value value;   // null: none
            std::vector<std::string> options;
            std::vector<ImGuiID> ids;
            ImGuiID id = 0;
            Ctx ctx;
            bool enabled = true;
            bool visible = true;
            ImRect rect;
            float clipRight = FLT_MAX;  // its window's clip edge, for the lint
            matjson::Value extra = matjson::Value::object();
        };

        // An item ImGui's hooks saw (everything with an ID).
        struct Item {
            int seq = 0;
            ImGuiID id = 0;
            std::string label;
            std::string win;
            std::string card;
            std::string row;
            Ctx ctx;
            ImRect rect;
            bool enabled = true;
            bool visible = true;
            ImGuiItemStatusFlags status = 0;
            bool claimed = false;  // a kit element is this item
        };

        struct Win {
            std::string name;
            std::string key;
            std::string role;
            std::string root;
            ImRect rect;
            bool focused = false;
            float scrollY = 0.f;
            float scrollMaxY = 0.f;
            float viewH = 0.f;
        };

        struct Capture {
            bool valid = false;
            int imguiFrame = 0;
            ImVec2 display{};
            std::vector<Element> elements;
            std::vector<Item> items;
            std::unordered_map<ImGuiID, size_t> itemAt;
            std::vector<Win> windows;
            std::string raw;  // the log, with window marks
            std::vector<std::pair<std::string, std::string>> text;  // window key: its text, first seen first
        };

        Capture g_cap;
        bool g_want = false;
        bool g_ready = false;
        int g_seq = 0;

        // What's open while the frame is drawn.
        std::vector<std::string> g_cards;
        std::vector<size_t> g_cardAt;
        std::vector<std::string> g_rows;
        std::vector<size_t> g_rowAt;
        std::vector<std::string> g_rowHints;
        Ctx g_ctx;
        int g_featureDepth = 0;
        int g_textDepth = 0;
        bool g_logOwned = false;
        void (*g_setClipboard)(ImGuiContext*, const char*) = nullptr;

        // LogSetNextTextDecoration keeps the pointers it's given until the
        // next text is logged: these hold them.
        std::string g_cardPrefix, g_cardSuffix, g_rowPrefix, g_rowSuffix, g_textPrefix, g_textSuffix;

        constexpr char kMarkOpen = '\x02';
        constexpr char kMarkClose = '\x03';

        std::string idText(ImGuiID id) {
            return fmt::format("0x{:08X}", id);
        }

        // A label as shown: up to "##".
        std::string shown(const char* label) {
            if (!label)
                return "";
            std::string_view s(label);
            auto const cut = s.find("##");
            return std::string(cut == std::string_view::npos ? s : s.substr(0, cut));
        }

        const char* toneName(Tone t) {
            switch (t) {
                case Tone::Accent: return "accent";
                case Tone::Good: return "good";
                case Tone::Warn: return "warn";
                case Tone::Bad: return "bad";
                case Tone::Muted: return "muted";
                default: return "plain";
            }
        }

        // A window's key: what the result calls the window it belongs to
        // (its root's). The menu, Compact Mode and the switched-off window by
        // role; popups by kind (only the top one of each kind is open at a
        // time); the rest (the in-level boxes) by name.
        std::string keyOf(ImGuiWindow* w) {
            if (!w)
                return "";
            ImGuiWindow* root = w->RootWindow ? w->RootWindow : w;
            std::string_view const name = root->Name ? root->Name : "";
            if (name == "GucciBot##menu")
                return "menu";
            if (name.ends_with("###compact"))
                return "compact";
            if (name.ends_with("###switchedoff"))
                return "switched_off";
            if (root->Flags & ImGuiWindowFlags_Tooltip)
                return "tooltip";
            if (root->Flags & ImGuiWindowFlags_Modal)
                return "modal";
            if (name.starts_with("##Combo_"))
                return "combo";
            if (name.starts_with("##Menu_"))
                return "submenu";
            if (root->Flags & ImGuiWindowFlags_Popup)
                return "popup";
            return std::string(name);
        }

        std::string roleOf(ImGuiWindow* w) {
            std::string_view const name = w->Name ? w->Name : "";
            if (w->Flags & ImGuiWindowFlags_ChildWindow) {
                if (name.find("/##page_") != std::string_view::npos)
                    return "page";
                if (name.find("/##macros_") != std::string_view::npos)
                    return "list";
                return "child";
            }
            std::string const key = keyOf(w);
            if (key == "menu" || key == "compact" || key == "switched_off" || key == "tooltip" || key == "modal" ||
                key == "combo" || key == "submenu" || key == "popup")
                return key;
            return "overlay";
        }

        // ---- the text log

        void mark(ImGuiWindow* w) {
            ImGuiContext& ctx = *GImGui;
            if (!ctx.LogEnabled || !w)
                return;
            ImGui::LogText("%c%s%c", kMarkOpen, keyOf(w).c_str(), kMarkClose);
        }

        // Every window ImGui ends finishes the log (End() calls LogFinish),
        // so it's started again at the next thing drawn. ImGui hands what it
        // logged to the clipboard function, which is this probe's own for
        // the capture frame.
        void ensureLog() {
            if (!g_on || !g_logOwned)
                return;
            ImGuiContext& ctx = *GImGui;
            if (ctx.LogEnabled || !ctx.CurrentWindow)
                return;
            ImGui::LogBegin(ImGuiLogType_Clipboard, 0);
            // Tree depth counts from 0: no tree node opens for the log.
            ctx.LogDepthRef = 0;
            mark(ctx.CurrentWindow);
        }

        void sinkClipboard(ImGuiContext* ctx, const char* text) {
            if (g_on && ctx && ctx->LogEnabled && ctx->LogType == ImGuiLogType_Clipboard &&
                text == ctx->LogBuffer.begin()) {
                g_cap.raw += text;
                return;
            }
            // Anything else (a copy in a text box) goes where it always did.
            if (g_setClipboard)
                g_setClipboard(ctx, text);
        }

        // Logged at a place on screen, as ImGui logs its own text (a new line
        // when it's below the last).
        void logAt(ImVec2 pos, std::string const& s) {
            ensureLog();
            if (!GImGui->LogEnabled || s.empty())
                return;
            ImGui::LogRenderedText(&pos, s.c_str(), s.c_str() + s.size());
        }

        void decorate(std::string const& prefix, std::string const& suffix) {
            ImGui::LogSetNextTextDecoration(prefix.empty() ? nullptr : prefix.c_str(),
                                            suffix.empty() ? nullptr : suffix.c_str());
        }

        // A decoration of ours still waiting (its text drew nothing) goes, so
        // it can't land on someone else's text.
        void undecorate() {
            ImGuiContext& ctx = *GImGui;
            for (std::string const* s : {&g_cardPrefix, &g_cardSuffix, &g_rowPrefix, &g_rowSuffix, &g_textPrefix,
                                         &g_textSuffix})
                if (ctx.LogNextPrefix == s->c_str() || ctx.LogNextSuffix == s->c_str()) {
                    ctx.LogNextPrefix = nullptr;
                    ctx.LogNextSuffix = nullptr;
                    return;
                }
        }

        // ---- recording

        Element& push(std::string kind) {
            ImGuiWindow* w = GImGui->CurrentWindow;
            Element e;
            e.seq = ++g_seq;
            e.win = keyOf(w);
            e.card = g_cards.empty() ? std::string() : g_cards.back();
            e.row = g_rows.empty() ? std::string() : g_rows.back();
            e.kind = std::move(kind);
            e.ctx = g_ctx;
            if (w)
                e.clipRight = w->ClipRect.Max.x;
            e.enabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
            g_cap.elements.push_back(std::move(e));
            return g_cap.elements.back();
        }

        // The rect, enabled and in view, from the item just drawn.
        void fromLastItem(Element& e) {
            auto const& last = GImGui->LastItemData;
            e.rect = last.Rect;
            e.enabled = (last.ItemFlags & ImGuiItemFlags_Disabled) == 0;
            e.visible = (last.StatusFlags & ImGuiItemStatusFlags_Visible) != 0;
        }

        void claim(ImGuiID id) {
            if (auto const it = g_cap.itemAt.find(id); it != g_cap.itemAt.end())
                g_cap.items[it->second].claimed = true;
        }

        Item& itemFor(ImGuiContext* ctx, ImGuiID id) {
            auto [it, fresh] = g_cap.itemAt.try_emplace(id, g_cap.items.size());
            if (fresh) {
                Item item;
                item.seq = ++g_seq;
                item.id = id;
                g_cap.items.push_back(std::move(item));
            }
            Item& item = g_cap.items[it->second];
            ImGuiWindow* w = ctx->CurrentWindow;
            item.win = keyOf(w);
            item.card = g_cards.empty() ? std::string() : g_cards.back();
            item.row = g_rows.empty() ? std::string() : g_rows.back();
            item.ctx = g_ctx;
            return item;
        }

        void begin() {
            g_want = false;
            g_ready = false;
            g_cap = Capture{};
            g_seq = 0;
            g_cards.clear();
            g_cardAt.clear();
            g_rows.clear();
            g_rowAt.clear();
            g_rowHints.clear();
            g_ctx.clear();
            g_featureDepth = 0;
            g_textDepth = 0;
            ImGuiContext& ctx = *GImGui;
            g_cap.imguiFrame = ctx.FrameCount;
            g_cap.display = ctx.IO.DisplaySize;
            g_on = true;
            ctx.TestEngineHookItems = true;
            auto& pio = ImGui::GetPlatformIO();
            g_setClipboard = pio.Platform_SetClipboardTextFn;
            pio.Platform_SetClipboardTextFn = sinkClipboard;
            // Someone else logging (nothing in GucciBot does): their log is
            // left alone, and the capture has no text.
            g_logOwned = !ctx.LogEnabled;
            ensureLog();
        }

        // The log split at its window marks, each window's text together.
        void splitText() {
            std::string key = "menu";
            std::string const& raw = g_cap.raw;
            size_t pos = 0;
            auto append = [&](std::string const& k, std::string_view part) {
                for (auto& [have, text] : g_cap.text)
                    if (have == k) {
                        text.append(part);
                        return;
                    }
                g_cap.text.emplace_back(k, std::string(part));
            };
            while (pos < raw.size()) {
                size_t const open = raw.find(kMarkOpen, pos);
                if (open == std::string::npos) {
                    append(key, std::string_view(raw).substr(pos));
                    break;
                }
                if (open > pos)
                    append(key, std::string_view(raw).substr(pos, open - pos));
                size_t const close = raw.find(kMarkClose, open + 1);
                if (close == std::string::npos)
                    break;
                key = raw.substr(open + 1, close - open - 1);
                pos = close + 1;
            }
            // Tidy: no spaces at line ends, at most one blank line, none at
            // either end; and windows that logged nothing go.
            for (auto& [k, text] : g_cap.text) {
                std::string out;
                size_t start = 0;
                int blanks = 0;
                while (start <= text.size()) {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                        end = text.size();
                    std::string line = text.substr(start, end - start);
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
                        line.pop_back();
                    if (line.empty()) {
                        if (!out.empty() && ++blanks <= 1)
                            out += '\n';
                    } else {
                        blanks = 0;
                        out += line + '\n';
                    }
                    start = end + 1;
                }
                while (!out.empty() && out.back() == '\n')
                    out.pop_back();
                text = std::move(out);
            }
            std::erase_if(g_cap.text, [](auto const& kt) { return kt.second.empty(); });
        }

        void finish() {
            ImGuiContext& ctx = *GImGui;
            undecorate();
            if (g_logOwned && ctx.LogEnabled && ctx.LogType == ImGuiLogType_Clipboard)
                ImGui::LogFinish();  // the last part, into sinkClipboard
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn = g_setClipboard;
            g_setClipboard = nullptr;
            ctx.TestEngineHookItems = false;
            g_on = false;

            for (ImGuiWindow* w : ctx.Windows) {
                if (!w || !w->Active || w->IsFallbackWindow)
                    continue;
                bool const child = (w->Flags & ImGuiWindowFlags_ChildWindow) != 0;
                std::string const role = roleOf(w);
                // Children only where they scroll (a page, the macro list).
                if (child && role == "child" && w->ScrollMax.y <= 0.f)
                    continue;
                Win info;
                info.name = w->Name;
                info.key = child ? role : keyOf(w);
                info.role = role;
                info.root = w->RootWindow && w->RootWindow != w ? keyOf(w) : "";
                info.rect = w->Rect();
                info.focused = ctx.NavWindow == w;
                info.scrollY = w->Scroll.y;
                info.scrollMaxY = w->ScrollMax.y;
                info.viewH = w->InnerRect.GetHeight();
                g_cap.windows.push_back(std::move(info));
            }
            splitText();
            g_cap.valid = true;
            g_ready = true;
        }

        // ---- the result

        matjson::Value rectJson(ImRect const& r) {
            auto arr = matjson::Value::array();
            arr.push((double)std::round(r.Min.x));
            arr.push((double)std::round(r.Min.y));
            arr.push((double)std::round(r.GetWidth()));
            arr.push((double)std::round(r.GetHeight()));
            return arr;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // Exact, then ignoring case, then a part (ignoring case).
        int matchRank(std::string const& have, std::string const& want) {
            if (want.empty())
                return 0;
            if (have == want)
                return 0;
            auto const a = lower(have);
            auto const b = lower(want);
            if (a == b)
                return 1;
            if (a.find(b) != std::string::npos)
                return 2;
            return -1;
        }

        std::string valueText(matjson::Value const& v) {
            if (v.isNull())
                return "";
            if (v.isString())
                return v.asString().unwrapOr("");
            return v.dump(matjson::NO_INDENTATION);
        }

        std::string ctxText(Ctx const& ctx) {
            std::string out;
            for (auto const& [k, v] : ctx)
                out += (out.empty() ? "" : ",") + k + "=" + v;
            return out;
        }

        // The elements in drawn order: the kit's, and every item ImGui saw
        // with a label that no kit element is.
        std::vector<Element> merged() {
            std::vector<Element> out = g_cap.elements;
            for (auto const& item : g_cap.items) {
                if (item.claimed || item.label.empty())
                    continue;
                Element e;
                e.seq = item.seq;
                e.win = item.win;
                e.card = item.card;
                e.row = item.row;
                e.kind = "item";
                e.label = item.label;
                e.id = item.id;
                e.ctx = item.ctx;
                e.enabled = item.enabled;
                e.visible = item.visible;
                e.rect = item.rect;
                if (item.status & ImGuiItemStatusFlags_Checkable)
                    e.extra["checked"] = (item.status & ImGuiItemStatusFlags_Checked) != 0;
                if (item.status & ImGuiItemStatusFlags_Openable)
                    e.extra["open"] = (item.status & ImGuiItemStatusFlags_Opened) != 0;
                if (item.status & ImGuiItemStatusFlags_Inputable)
                    e.extra["inputable"] = true;
                out.push_back(std::move(e));
            }
            std::stable_sort(out.begin(), out.end(), [](Element const& a, Element const& b) { return a.seq < b.seq; });
            return out;
        }

        bool windowWanted(std::string const& filter, std::string const& key) {
            if (filter.empty() || filter == "all")
                return true;
            if (filter == "overlays")
                return key != "menu" && key != "compact" && key != "switched_off" && key != "popup" &&
                       key != "submenu" && key != "combo" && key != "modal" && key != "tooltip";
            if (filter == "popups")
                return key == "popup" || key == "submenu" || key == "combo" || key == "modal" || key == "tooltip";
            return matchRank(key, filter) >= 0;
        }

        bool matches(Element const& e, Query const& q) {
            if (q.findId)
                return e.id == *q.findId || std::find(e.ids.begin(), e.ids.end(), *q.findId) != e.ids.end();
            auto ok = [](std::string const& have, std::string const& want) {
                return want.empty() || matchRank(have, want) >= 0;
            };
            if (!ok(e.win, q.findWindow) || !ok(e.card, q.findCard) || !ok(e.row, q.findRow) ||
                !ok(e.kind, q.findKind))
                return false;
            if (!q.findLabel.empty() && matchRank(e.label, q.findLabel) < 0 && matchRank(e.text, q.findLabel) < 0)
                return false;
            if (!q.findValue.empty()) {
                bool any = matchRank(valueText(e.value), q.findValue) >= 0;
                if (!any)
                    return false;
            }
            if (!q.findContext.empty()) {
                bool any = false;
                for (auto const& [k, v] : e.ctx)
                    any = any || matchRank(v, q.findContext) >= 0 || matchRank(k + "=" + v, q.findContext) >= 0;
                if (!any)
                    return false;
            }
            return true;
        }

        // The best-ranked matches (an exact label beats a part of one).
        std::vector<Element const*> findIn(std::vector<Element> const& all, Query const& q) {
            std::vector<Element const*> hits;
            for (auto const& e : all)
                if (matches(e, q) && (!q.visibleOnly || e.visible))
                    hits.push_back(&e);
            if (!q.findLabel.empty() && hits.size() > 1) {
                auto const rank = [&](Element const* e) {
                    int const r = matchRank(e->label, q.findLabel);
                    int const t = matchRank(e->text, q.findLabel);
                    return std::min(r < 0 ? 3 : r, t < 0 ? 3 : t);
                };
                int best = 3;
                for (auto const* e : hits)
                    best = std::min(best, rank(e));
                std::erase_if(hits, [&](Element const* e) { return rank(e) > best; });
            }
            if (q.findIndex) {
                if (*q.findIndex < 0 || *q.findIndex >= (int)hits.size())
                    return {};
                return {hits[(size_t)*q.findIndex]};
            }
            return hits;
        }

        std::string pathOf(Element const& e) {
            std::string out = e.win;
            if (!e.card.empty())
                out += " > " + e.card;
            if (!e.ctx.empty())
                out += " > " + e.ctx.front().second;
            else if (!e.row.empty() && e.row != e.label)
                out += " > " + e.row;
            std::string const name = !e.label.empty() ? e.label : e.text;
            if (!name.empty())
                out += " > " + name;
            return out;
        }

        matjson::Value elementJson(Element const& e, int n) {
            auto j = matjson::Value::object();
            j["n"] = (int64_t)n;
            j["win"] = e.win;
            if (!e.card.empty())
                j["card"] = e.card;
            if (!e.row.empty() && e.kind != "row")
                j["row"] = e.row;
            if (!e.ctx.empty()) {
                auto c = matjson::Value::object();
                for (auto const& [k, v] : e.ctx)
                    c[k] = v;
                j["ctx"] = c;
            }
            j["kind"] = e.kind;
            if (!e.label.empty())
                j["label"] = e.label;
            if (!e.text.empty())
                j["text"] = e.text;
            if (!e.tone.empty())
                j["tone"] = e.tone;
            if (!e.value.isNull())
                j["value"] = e.value;
            if (!e.options.empty())
                j["options"] = e.options;
            if (!e.ids.empty()) {
                std::vector<std::string> ids;
                for (ImGuiID id : e.ids)
                    ids.push_back(idText(id));
                j["ids"] = ids;
            }
            if (e.id)
                j["id"] = idText(e.id);
            j["enabled"] = e.enabled;
            j["visible"] = e.visible;
            j["rect"] = rectJson(e.rect);
            if (e.extra.isObject())
                for (auto const& [k, v] : e.extra)
                    j[k] = v;
            return j;
        }

        matjson::Value lintJson(std::vector<Element> const& all, Query const& q) {
            auto out = matjson::Value::array();
            auto issue = [&](Element const& e, int n, const char* problem, double by) {
                auto j = matjson::Value::object();
                j["n"] = (int64_t)n;
                j["problem"] = problem;
                if (by > 0.0)
                    j["by_px"] = std::round(by);
                j["path"] = pathOf(e);
                out.push(j);
            };
            static constexpr std::string_view kTexty[] = {"label", "paragraph", "hint", "section", "note",
                                                          "chip",  "button",    "row",  "card",    "macro_row"};
            for (size_t i = 0; i < all.size(); ++i) {
                auto const& e = all[i];
                if (!e.visible || !windowWanted(q.window, e.win))
                    continue;
                bool const texty = std::find(std::begin(kTexty), std::end(kTexty), e.kind) != std::end(kTexty);
                if (texty && e.clipRight < FLT_MAX && e.rect.Max.x > e.clipRight + 1.f)
                    issue(e, (int)i, "clipped_right", e.rect.Max.x - e.clipRight);
                if (e.extra.contains("clipped_options"))
                    issue(e, (int)i, "choice_option_clipped", 0.0);
            }
            // Overlaps: two things side by side in the same card and row.
            for (size_t i = 0; i < all.size(); ++i) {
                auto const& a = all[i];
                if (!a.visible || a.kind == "card" || a.kind == "row" || !windowWanted(q.window, a.win))
                    continue;
                for (size_t k = i + 1; k < all.size() && k < i + 40; ++k) {
                    auto const& b = all[k];
                    if (!b.visible || b.kind == "card" || b.kind == "row" || b.win != a.win || b.card != a.card ||
                        b.row != a.row)
                        continue;
                    float const ox = std::min(a.rect.Max.x, b.rect.Max.x) - std::max(a.rect.Min.x, b.rect.Min.x);
                    float const oy = std::min(a.rect.Max.y, b.rect.Max.y) - std::max(a.rect.Min.y, b.rect.Min.y);
                    if (ox > 2.f && oy > 2.f) {
                        auto j = matjson::Value::object();
                        j["n"] = (int64_t)i;
                        j["with"] = (int64_t)k;
                        j["problem"] = "overlaps";
                        j["by_px"] = std::round(std::min(ox, oy));
                        j["path"] = pathOf(a) + "  /  " + pathOf(b);
                        out.push(j);
                    }
                }
            }
            for (auto const& w : g_cap.windows) {
                if (!w.root.empty() || !windowWanted(q.window, w.key))
                    continue;
                if (w.rect.Min.x < -0.5f || w.rect.Min.y < -0.5f || w.rect.Max.x > g_cap.display.x + 0.5f ||
                    w.rect.Max.y > g_cap.display.y + 0.5f) {
                    auto j = matjson::Value::object();
                    j["window"] = w.key;
                    j["problem"] = "offscreen";
                    out.push(j);
                }
            }
            return out;
        }

        // The lines of a card's part of the menu's text: from its "## Title"
        // line to the next card's.
        std::string cardText(std::string const& text, std::string const& title) {
            std::string out;
            bool in = false;
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string_view const line(text.data() + start, end - start);
                if (line.starts_with("## ")) {
                    std::string_view name = line.substr(3);
                    if (auto const cut = name.find("  ("); cut != std::string_view::npos)
                        name = name.substr(0, cut);
                    in = matchRank(std::string(name), title) >= 0 && matchRank(std::string(name), title) <= 1;
                }
                if (in)
                    out.append(line).append("\n");
                start = end + 1;
            }
            return out;
        }

        std::string capLines(std::string const& text, int maxLines, int* cut) {
            int lines = 0;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t const nl = text.find('\n', pos);
                if (++lines > maxLines) {
                    int rest = 1;
                    for (size_t p = pos; (p = text.find('\n', p)) != std::string::npos; ++p)
                        rest++;
                    *cut += rest;
                    return text.substr(0, pos) + fmt::format("... ({} more lines)", rest);
                }
                if (nl == std::string::npos)
                    break;
                pos = nl + 1;
            }
            return text;
        }

        Win const* windowByKey(std::string const& key) {
            for (auto const& w : g_cap.windows)
                if (w.key == key || w.role == key)
                    return &w;
            for (int pass = 0; pass < 3; ++pass)
                for (auto const& w : g_cap.windows)
                    if (matchRank(w.name, key) == pass)
                        return &w;
            return nullptr;
        }

    } // namespace

    // ---- asked between frames

    void request() {
        g_want = true;
        g_ready = false;
    }

    bool ready() {
        return g_ready;
    }

    matjson::Value result(Query const& q) {
        auto out = matjson::Value::object();
        out["imgui_frame"] = (int64_t)g_cap.imguiFrame;
        out["display"] = std::vector<double>{std::round(g_cap.display.x), std::round(g_cap.display.y)};
        auto const all = merged();

        // The windows.
        auto windows = matjson::Value::array();
        for (auto const& w : g_cap.windows) {
            if (!windowWanted(q.window, w.root.empty() ? w.key : w.root))
                continue;
            auto j = matjson::Value::object();
            j["name"] = w.name;
            j["key"] = w.key;
            j["role"] = w.role;
            if (!w.root.empty())
                j["in"] = w.root;
            j["rect"] = rectJson(w.rect);
            j["focused"] = w.focused;
            j["scroll_y"] = std::round(w.scrollY);
            j["scroll_max_y"] = std::round(w.scrollMaxY);
            j["view_h"] = std::round(w.viewH);
            windows.push(j);
        }
        out["windows"] = windows;

        // The elements: the ones found, or a window's (or a card's).
        std::vector<Element const*> picked;
        if (q.finding) {
            picked = findIn(all, q);
        } else {
            for (auto const& e : all)
                if (windowWanted(q.window, e.win) && (q.card.empty() || matchRank(e.card, q.card) >= 0) &&
                    (!q.visibleOnly || e.visible))
                    picked.push_back(&e);
        }
        int truncated = 0;
        if ((int)picked.size() > q.max) {
            truncated = (int)picked.size() - q.max;
            picked.resize((size_t)q.max);
        }

        // Each element's n is its place in the whole frame, so a find and a
        // full read name the same element alike.
        auto indexOf = [&](Element const* e) { return (int)(e - all.data()); };
        int cutLines = 0;
        if (q.text) {
            std::string outline;
            if (q.finding) {
                for (auto const* e : picked) {
                    outline += fmt::format("[{}] {} {}", indexOf(e), e->kind, pathOf(*e));
                    if (!e->value.isNull())
                        outline += " = " + valueText(e->value);
                    if (!e->options.empty()) {
                        std::string opts;
                        for (auto const& o : e->options)
                            opts += (opts.empty() ? "" : " | ") + o;
                        outline += " {" + opts + "}";
                    }
                    if (!e->ctx.empty())
                        outline += " ctx(" + ctxText(e->ctx) + ")";
                    if (!e->enabled)
                        outline += " disabled";
                    if (!e->visible)
                        outline += " out-of-view";
                    if (e->id)
                        outline += " id=" + idText(e->id);
                    outline += "\n";
                }
                if (picked.empty())
                    outline = "(nothing matches)\n";
            } else {
                for (auto const& [key, text] : g_cap.text) {
                    if (!windowWanted(q.window, key))
                        continue;
                    std::string body = q.card.empty() ? text : cardText(text, q.card);
                    if (body.empty())
                        continue;
                    if (auto const* w = windowByKey(key == "menu" ? "page" : key))
                        outline += fmt::format("=== {}  scroll {:.0f}/{:.0f}  view {:.0f}\n", key, w->scrollY,
                                               w->scrollMaxY, w->viewH);
                    else
                        outline += fmt::format("=== {}\n", key);
                    outline += capLines(body, q.maxLines, &cutLines) + "\n";
                }
                if (outline.empty())
                    outline = "(nothing drawn: the menu is shut and nothing is over the level)\n";
            }
            out["outline"] = outline;
            out["matches"] = (int64_t)picked.size();
        } else {
            auto elements = matjson::Value::array();
            for (auto const* e : picked)
                elements.push(elementJson(*e, indexOf(e)));
            out["elements"] = elements;
            auto text = matjson::Value::object();
            for (auto const& [key, body] : g_cap.text)
                if (windowWanted(q.window, key)) {
                    std::string t = q.card.empty() ? body : cardText(body, q.card);
                    if (!t.empty())
                        text[key] = capLines(t, q.maxLines, &cutLines);
                }
            out["text"] = text;
        }
        if (q.lint)
            out["lint"] = lintJson(all, q);
        auto trunc = matjson::Value::object();
        trunc["elements"] = (int64_t)truncated;
        trunc["lines"] = (int64_t)cutLines;
        out["truncated"] = trunc;
        return out;
    }

    std::optional<WindowInfo> window(std::string const& key) {
        if (!g_cap.valid)
            return std::nullopt;
        auto const* w = windowByKey(key);
        if (!w)
            return std::nullopt;
        WindowInfo info;
        info.name = w->name;
        info.key = w->key;
        info.rect = ImVec4(w->rect.Min.x, w->rect.Min.y, w->rect.GetWidth(), w->rect.GetHeight());
        info.scrollY = w->scrollY;
        info.scrollMaxY = w->scrollMaxY;
        info.viewH = w->viewH;
        return info;
    }

    std::optional<ImVec4> cardRect(std::string const& title) {
        for (int pass = 0; pass < 3; ++pass)
            for (auto const& e : g_cap.elements)
                if (e.kind == "card" && matchRank(e.label, title) == pass)
                    return ImVec4(e.rect.Min.x, e.rect.Min.y, e.rect.GetWidth(), e.rect.GetHeight());
        return std::nullopt;
    }

    std::vector<Hit> find(Query const& q) {
        std::vector<Hit> out;
        auto const all = merged();
        for (auto const* e : findIn(all, q)) {
            Hit h;
            h.n = (int)(e - all.data());
            h.path = pathOf(*e);
            h.rect = ImVec4(e->rect.Min.x, e->rect.Min.y, e->rect.GetWidth(), e->rect.GetHeight());
            h.window = e->win;
            out.push_back(std::move(h));
        }
        return out;
    }

    std::vector<std::string> visibleCards() {
        std::vector<std::string> out;
        for (auto const& e : g_cap.elements)
            if (e.kind == "card" && e.visible && !e.label.empty())
                out.push_back(e.label);
        return out;
    }

    bool scrollTo(std::string const& windowName, float y) {
        if (!GImGui)
            return false;
        ImGuiWindow* w = ImGui::FindWindowByName(windowName.c_str());
        if (!w)
            return false;
        ImGui::SetScrollY(w, std::max(0.f, y));
        return true;
    }

    // ---- the shell

    FrameGuard::FrameGuard() {
        if (g_want && GImGui)
            begin();
    }

    FrameGuard::~FrameGuard() {
        if (g_on)
            finish();
    }

    Scope::Scope(const char* key, std::string value) {
        if (!g_on)
            return;
        active = true;
        g_ctx.emplace_back(key, std::move(value));
    }

    Scope::~Scope() {
        if (active && !g_ctx.empty())
            g_ctx.pop_back();
    }

    void macroRow(std::string const& name, std::string const& ext, std::string const& tag, bool selected) {
        if (!g_on)
            return;
        ensureLog();
        Element& e = push("macro_row");
        fromLastItem(e);
        e.label = name;
        e.id = GImGui->LastItemData.ID;
        e.extra["ext"] = ext;
        e.extra["tag"] = tag;
        e.extra["selected"] = selected;
        claim(e.id);
    }

    // ---- the kit

    void card(const char* title, const char* caption) {
        ensureLog();
        ImGuiWindow* w = GImGui->CurrentWindow;  // the card's own child window
        std::string const name = title ? title : "";
        Element& e = push("card");
        e.label = name;
        e.card = name;
        if (caption && *caption)
            e.extra["caption"] = caption;
        e.rect = w->Rect();
        ImRect const view = w->ParentWindow ? w->ParentWindow->InnerClipRect : w->Rect();
        bool const below = e.rect.Min.y >= view.Max.y;
        bool const above = e.rect.Max.y <= view.Min.y;
        e.visible = !below && !above;
        if (!e.visible)
            e.extra["out_of_view"] = below ? "below" : "above";
        g_cardAt.push_back(g_cap.elements.size() - 1);
        g_cards.push_back(name);
        // "## Title" in the text, and where it is when it's out of view.
        if (!name.empty()) {
            g_cardPrefix = "## ";
            g_cardSuffix = below ? "  (below the view)" : above ? "  (above the view)" : "";
            decorate(g_cardPrefix, g_cardSuffix);
        }
    }

    void endCard() {
        if (!g_cardAt.empty()) {
            // The card's child as the page placed it, now its height is known.
            auto& e = g_cap.elements[g_cardAt.back()];
            auto const& last = GImGui->LastItemData;
            if (last.Rect.GetWidth() > 0.f)
                e.rect = last.Rect;
            g_cardAt.pop_back();
        }
        if (!g_cards.empty())
            g_cards.pop_back();
    }

    void row(const char* label, const char* hint) {
        ensureLog();
        ImGuiWindow* w = GImGui->CurrentWindow;
        std::string const name = label ? shown(label) : std::string();
        Element& e = push("row");
        e.label = name;
        e.row = name;
        if (hint && *hint)
            e.extra["hint"] = hint;
        ImVec2 const start = ImGui::GetCursorScreenPos();
        float const h = ImGui::GetFrameHeight();
        e.rect = ImRect(start, ImVec2(start.x + ImGui::GetContentRegionAvail().x, start.y + h));
        bool const below = start.y >= w->ClipRect.Max.y;
        bool const above = start.y + h <= w->ClipRect.Min.y;
        e.visible = !below && !above;
        g_rowAt.push_back(g_cap.elements.size() - 1);
        g_rows.push_back(name);
        g_rowHints.push_back(hint ? hint : "");
        // "Label:" with the controls after it on the line, a feature's rows
        // indented, and "v " / "^ " in front of a row out of view.
        g_rowPrefix = std::string(static_cast<size_t>(g_featureDepth) * 2, ' ') + (below ? "v " : above ? "^ " : "");
        g_rowSuffix = ":";
        decorate(g_rowPrefix, g_rowSuffix);
    }

    void endRow() {
        if (g_rowAt.empty())
            return;
        auto& e = g_cap.elements[g_rowAt.back()];
        // The controls' group ends the row.
        e.rect.Max.y = std::max(e.rect.Max.y, GImGui->LastItemData.Rect.Max.y);
        std::string const hint = g_rowHints.back();
        if (!hint.empty()) {
            ensureLog();
            if (GImGui->LogEnabled)
                ImGui::LogText("  ~ %s", hint.c_str());
        }
        g_rowAt.pop_back();
        g_rows.pop_back();
        g_rowHints.pop_back();
    }

    void feature(bool open) {
        g_featureDepth = std::max(0, g_featureDepth + (open ? 1 : -1));
    }

    Text::Text(TextKind kind, const char* text, Tone tone) : kind(kind), text(text), tone(tone) {
        if (!g_on)
            return;
        if (g_textDepth++ > 0) {
            counted = true;
            return;
        }
        counted = true;
        active = true;
        ensureLog();
        if (!text || !*text)
            return;
        switch (kind) {
            case TextKind::Section:
                g_textPrefix = "-- ";
                g_textSuffix = " --";
                break;
            case TextKind::Hint:
                g_textPrefix = "~ ";
                g_textSuffix.clear();
                break;
            default:
                g_textPrefix = tone == Tone::Warn || tone == Tone::Bad ? "! " : "";
                g_textSuffix.clear();
                break;
        }
        if (!g_textPrefix.empty() || !g_textSuffix.empty())
            decorate(g_textPrefix, g_textSuffix);
    }

    Text::~Text() {
        if (!counted)
            return;
        --g_textDepth;
        if (!active || !g_on)
            return;
        undecorate();
        static constexpr const char* kKinds[] = {"section", "label", "paragraph", "hint"};
        Element& e = push(kKinds[static_cast<int>(kind)]);
        fromLastItem(e);
        e.text = text ? text : "";
        e.tone = toneName(tone);
    }

    Quiet::Quiet() {
        if (!g_on || !GImGui->LogEnabled)
            return;
        active = true;
        GImGui->LogEnabled = false;
    }

    Quiet::~Quiet() {
        if (active && g_on)
            GImGui->LogEnabled = true;
    }

    void note(const char* text, Tone tone, ImVec2 a, ImVec2 b) {
        Element& e = push("note");
        e.text = text ? text : "";
        e.tone = toneName(tone);
        e.rect = ImRect(a, b);
        e.visible = GImGui->CurrentWindow && e.rect.Overlaps(GImGui->CurrentWindow->ClipRect);
        const char* mark = tone == Tone::Warn || tone == Tone::Bad ? "! " : tone == Tone::Good ? "+ " : "> ";
        logAt(a, mark + e.text);
    }

    void chip(const char* text, Tone tone, ImVec2 a, ImVec2 b) {
        Element& e = push("chip");
        e.text = text ? text : "";
        e.tone = toneName(tone);
        e.rect = ImRect(a, b);
        e.visible = GImGui->CurrentWindow && e.rect.Overlaps(GImGui->CurrentWindow->ClipRect);
        logAt(a, "<" + e.text + ">");
    }

    void progress(float fraction, const char* overlay) {
        Element& e = push("progress");
        fromLastItem(e);
        e.value = (double)fraction;
        if (overlay)
            e.text = overlay;
    }

    void toggle(bool value) {
        Element& e = push("switch");
        fromLastItem(e);
        e.id = GImGui->LastItemData.ID;
        e.value = value;
        e.label = e.row;
        claim(e.id);
        logAt(e.rect.Min, value ? "[x]" : "[ ]");
    }

    void choice(int index, const char* const* options, int count) {
        Element& e = push("choice");
        fromLastItem(e);  // the Dummy over the whole control
        e.label = e.row;
        float const segment = count > 0 ? e.rect.GetWidth() / static_cast<float>(count) : 0.f;
        std::string line;
        auto clipped = matjson::Value::array();
        for (int i = 0; i < count; ++i) {
            std::string const opt = options[i] ? options[i] : "";
            e.options.push_back(opt);
            // Each segment's ID, as Choice makes it: PushID(i), then "##part".
            ImGui::PushID(i);
            ImGuiID const id = ImGui::GetID("##part");
            ImGui::PopID();
            e.ids.push_back(id);
            claim(id);
            line += (line.empty() ? "" : " ") + (i == index ? "(" + opt + ")" : opt);
            // Choice clips an option's text to its segment.
            if (ImGui::CalcTextSize(opt.c_str()).x > segment - 4.f)
                clipped.push(opt);
        }
        if (index >= 0 && index < count)
            e.value = e.options[(size_t)index];
        if (clipped.size() > 0)
            e.extra["clipped_options"] = clipped;
        logAt(e.rect.Min, line);
    }

    void dropdown(int index, const char* const* options, int count) {
        std::vector<std::string> list;
        for (int i = 0; i < count; ++i)
            list.emplace_back(options[i] ? options[i] : "");
        dropdown(index, list, nullptr);
    }

    void dropdown(int index, std::vector<std::string> const& options, const char* placeholder) {
        Element& e = push("dropdown");
        fromLastItem(e);
        e.id = ImGui::GetID("##pick");
        e.label = e.row;
        e.options = options;
        if (index >= 0 && index < (int)options.size())
            e.value = options[(size_t)index];
        else
            e.value = std::string();
        if (placeholder)
            e.extra["placeholder"] = placeholder;
        claim(e.id);
    }

    void slider(const char* widget, double value, double lo, double hi) {
        Element& e = push("slider");
        fromLastItem(e);
        e.id = ImGui::GetID(widget);
        e.label = e.row;
        e.value = value;
        e.extra["lo"] = lo;
        e.extra["hi"] = hi;
        claim(e.id);
    }

    void input(const char* widget, matjson::Value value, const char* placeholder) {
        Element& e = push("input");
        fromLastItem(e);
        e.id = ImGui::GetID(widget);
        // Outside a row (the list's search box), its placeholder names it.
        e.label = e.row.empty() && placeholder ? placeholder : e.row;
        e.value = std::move(value);
        if (placeholder)
            e.extra["placeholder"] = placeholder;
        claim(e.id);
    }

    void button(const char* label, Tone tone) {
        Element& e = push("button");
        fromLastItem(e);
        e.id = GImGui->LastItemData.ID;
        e.label = shown(label);
        e.tone = toneName(tone);
        claim(e.id);
        // ImGui logged it as [label]; a disabled one gets a "-".
        if (!e.enabled && GImGui->LogEnabled)
            ImGui::LogText("-");
    }

    void color(ImVec4 const& c) {
        Element& e = push("color");
        fromLastItem(e);
        e.id = ImGui::GetID("##colour");
        e.label = e.row;
        e.value = std::vector<double>{c.x, c.y, c.z, c.w};
        claim(e.id);
        auto const byte = [](float v) { return (int)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
        logAt(e.rect.Min, fmt::format("{{#{:02X}{:02X}{:02X}{:02X}}}", byte(c.x), byte(c.y), byte(c.z), byte(c.w)));
    }

    void key(std::string const& name, bool waiting) {
        // Key draws a kit Button: that element becomes the key.
        if (!g_cap.elements.empty() && g_cap.elements.back().kind == "button" &&
            g_cap.elements.back().id == GImGui->LastItemData.ID) {
            auto& e = g_cap.elements.back();
            e.kind = "key";
            e.label = e.row;
            e.value = name;
            e.extra["waiting"] = waiting;
            return;
        }
        Element& e = push("key");
        fromLastItem(e);
        e.id = GImGui->LastItemData.ID;
        e.label = e.row;
        e.value = name;
        e.extra["waiting"] = waiting;
        claim(e.id);
    }

    // ---- ImGui's hooks

    void hookItemAdd(ImGuiContext* ctx, ImGuiID id, ImRect const& bb, ImGuiLastItemData const* data) {
        ImGuiWindow* w = ctx->CurrentWindow;
        if (!w)
            return;
        // A window beginning: its text starts here.
        if (!data && id == w->ID) {
            if (!ctx->LogEnabled)
                ensureLog();
            else
                mark(w);
            return;
        }
        ensureLog();
        Item& item = itemFor(ctx, id);
        item.rect = bb;
        ImGuiItemFlags const flags = data ? data->ItemFlags : ctx->CurrentItemFlags;
        item.enabled = (flags & ImGuiItemFlags_Disabled) == 0;
        item.visible = bb.Overlaps(w->ClipRect);
    }

    void hookItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
        ImGuiWindow* w = ctx->CurrentWindow;
        if (!w || id == w->ID)
            return;
        Item& item = itemFor(ctx, id);
        if (item.rect.GetWidth() <= 0.f && item.rect.GetHeight() <= 0.f)
            item.rect = ctx->LastItemData.Rect;
        item.label = shown(label);
        item.status = flags;
    }

} // namespace gucci::ui::probe

// ImGui's test-engine hooks (imgui_internal.h, IMGUI_ENABLE_TEST_ENGINE):
// called only while GImGui->TestEngineHookItems is on, the capture frame.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item_data) {
    if (gucci::ui::probe::on())
        gucci::ui::probe::hookItemAdd(ctx, id, bb, item_data);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (gucci::ui::probe::on())
        gucci::ui::probe::hookItemInfo(ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) {
    return nullptr;
}

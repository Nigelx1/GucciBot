// The menu's search box: see ui/search.hpp.

#include "ui/search.hpp"

#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/pages.hpp"
#include "ui/probe.hpp"
#include "ui/touch.hpp"

#include <Geode/Geode.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

namespace gucci::ui::search {

    namespace detail {
        bool g_rec = false;
        bool g_indexing = false;
    }

    namespace {

        using detail::g_indexing;
        using detail::g_rec;

        // ---- the index

        struct Entry {
            std::string key;                 // where it is, exactly (scopes, label, its n-th time there)
            std::vector<std::string> chain;  // the card(s) and feature switches it sits in, then itself
            std::string kind;                // card, row, feature, button
            std::string page;
            std::string tab;                 // the page's tab it's on (Macro Tools has three), or ""
            std::string card;
            std::vector<std::string> features;  // the switches it sits under, outermost first
            std::string label;
            std::string hint;   // a row's hint, a card's caption
            std::string words;  // a row's buttons and options
            // ... all of it folded for matching (fold()), worked out once.
            std::string fLabel, fAlias, fWhere, fHint, fWords;
        };

        std::vector<Entry> g_index;
        std::vector<Entry> g_building;
        int g_builds = 0;
        bool g_indexWanted = false;
        double g_indexedAt = -100.0;

        // ---- what's being drawn (the index pass, or the live page)

        struct Open {
            std::string key;
            std::string title;
            ImGuiWindow* window = nullptr;
        };
        std::string g_page;
        std::string g_tab;
        std::vector<std::string> g_scopes;    // the scope keys open: cards, feature bodies
        std::vector<std::string> g_chain;     // the keys of the cards and features open
        std::vector<Open> g_cards;            // the cards open
        std::vector<std::string> g_features;  // the labels of the features open
        std::unordered_map<std::string, int> g_counts;
        struct RowOpen {
            std::string key;
            std::string label;
            std::string hint;
            size_t entry = SIZE_MAX;  // its index entry (the index pass)
        };
        std::vector<RowOpen> g_rows;
        std::string g_lastRowKey, g_lastRowLabel;
        size_t g_lastRowEntry = SIZE_MAX;
        int g_mute = 0;
        bool g_live = false;

        // ---- the box

        std::string g_query;
        std::string g_resultsFor = "\x01";
        int g_resultsBuild = -1;
        std::vector<Result> g_results;
        std::vector<size_t> g_resultEntries;
        std::vector<ImGuiID> g_resultIds;
        ImGuiID g_fieldId = 0;
        int g_fieldFrame = -100;  // the last frame the box was drawn
        bool g_fieldWasActive = false;
        bool g_clearPending = false;
        bool g_focusPending = false;
        int g_menuFocusedFrame = -100;  // the last frame the box was drawn in a focused menu
        // The Windows layout (ui/layout.hpp): the menu is many windows, and
        // the jump's view is the card's own window, not the page's.
        bool g_free = false;
        int g_focusOverrideFrame = -100;
        bool g_focusOverride = false;

        // ---- a jump

        struct Found {
            bool valid = false;
            ImRect rect;
            ImGuiWindow* window = nullptr;
            bool skipped = false;  // drawn inside a window ImGui skipped (scrolled out): only its own rect counts
        };
        struct Jump {
            std::string phase;  // "seeking", "found", "done", ""
            std::string page;
            std::string tab;        // the page's tab to select first, or ""
            bool tabTaken = false;  // ... asked for once
            std::string label;
            std::vector<std::string> chain;
            std::vector<std::string> kinds;   // per chain step: card, switch, control
            std::vector<bool> cards;          // per chain step: it's a card (its own rect is right when skipped)
            std::vector<std::string> labels;  // per chain step
            std::vector<Found> found;         // this frame
            int frames = 0;
            int shown = -1;                   // the chain step highlighted
            ImRect shownRect;
            bool inView = false;
            double highlightUntil = 0.0;
            // Where the deepest step drawn was last frame: a place counts only
            // once it holds still for a frame. On a page's first frame its
            // cards are new windows whose heights aren't known yet, so what's
            // in them is laid out too high (11-15: Show Trajectory "found" in
            // view at the Macro card's place, and never scrolled to).
            int lastBest = -1;
            float lastY = 0.f;
        };
        Jump g_jump;
        std::optional<std::string> g_pickPage;
        ImGuiWindow* g_pageWindow = nullptr;

        constexpr double kHighlight = 1.6;
        constexpr int kSeekFrames = 15;
        constexpr size_t kMaxResults = 40;

        std::string shown(const char* label) {
            if (!label)
                return "";
            std::string_view s(label);
            auto const cut = s.find("##");
            return std::string(cut == std::string_view::npos ? s : s.substr(0, cut));
        }

        // Lower case, words split by spaces, British spellings folded into
        // American ones (so "colour", an old label, still finds "Color").
        std::string fold(std::string_view in) {
            std::string s;
            s.reserve(in.size() + 2);
            for (unsigned char c : in)
                s.push_back(std::isalnum(c) ? (char)std::tolower(c) : ' ');
            static const std::pair<const char*, const char*> kSpell[] = {
                {"colour", "color"}, {"quantis", "quantiz"}, {"favourite", "favorite"}, {"behaviour", "behavior"},
                {"labell", "label"}, {"centre", "center"}, {"optimis", "optimiz"}, {"synchronis", "synchroniz"},
            };
            for (auto const& [from, to] : kSpell) {
                size_t at = 0;
                std::string_view const f(from);
                while ((at = s.find(f, at)) != std::string::npos) {
                    s.replace(at, f.size(), to);
                    at += std::string_view(to).size();
                }
            }
            // One space between words, and one at each end (so " word" finds
            // a word's start).
            std::string out = " ";
            for (char c : s) {
                if (c == ' ' && out.back() == ' ')
                    continue;
                out.push_back(c);
            }
            if (out.back() != ' ')
                out.push_back(' ');
            return out;
        }

        // Old names and the names people know from other menus, by the label
        // a control has now. The renames are this fall's (git log of src/ui
        // since 2026-10-02, and the 2026-11-15 menu sweep).
        std::string const& aliasesFor(std::string const& label) {
            static const std::unordered_map<std::string, std::string> kAliases = {
                {"Show Trajectory", "path preview trajectory"},     // "Path preview" in beta.5 (921c160)
                {"Search distance", "sweep range"},                 // 9477ea3
                {"Audio pitch follows speed", "speedhack audio pitch"},  // 691b74c
                {"Recording and playback", "playback"},             // the "Playback" card (f77b297)
                {"At the playhead", "playhead labeling"},           // "Playhead labelling" (492de32)
                {"Ticks per frame, at most", "max upr upr"},        // the sweep: Max UPR
                {"Pacing", "dynamic upr upr fps"},                  // ... and Dynamic UPR
                {"Frame pacing", "upr fps"},
                {"Lock delta", "lock delta mode"},                  // 2ded9ee
                {"Follow the planned route", "follow the planned route ai"},  // 19d579d
                {"Exact start flags", "exact start flags test"},    // 35cb1e9
                {"Tick rate", "tps tickrate ticks per second"},
                {"Game speed", "speedhack speed hack"},
                {"Frame stepper", "frame advance frame step"},
                {"Noclip", "no clip"},
                {"Show hitboxes", "hitbox"},
                {"Hitborders", "hitbox borders"},
            };
            static const std::string kNone;
            auto const it = kAliases.find(label);
            return it == kAliases.end() ? kNone : it->second;
        }

        std::string scope() {
            return g_scopes.empty() ? "p:" + g_page : g_scopes.back();
        }

        std::string counted(std::string const& base) {
            int const n = g_counts[base]++;
            return base + "#" + std::to_string(n);
        }

        void resetDrawState(std::string const& page) {
            g_page = page;
            g_tab.clear();
            g_scopes.clear();
            g_chain.clear();
            g_cards.clear();
            g_features.clear();
            g_counts.clear();
            g_rows.clear();
            g_lastRowKey.clear();
            g_lastRowLabel.clear();
            g_lastRowEntry = SIZE_MAX;
        }

        Entry& addEntry(std::string key, std::string kind, std::string label, std::string hint) {
            Entry e;
            e.key = std::move(key);
            e.chain = g_chain;
            e.chain.push_back(e.key);
            e.kind = std::move(kind);
            e.page = g_page;
            e.tab = g_tab;
            // The card it's in: the innermost open (a card's own entry is
            // added before it's pushed, so this is its parent's, if any).
            e.card = g_cards.empty() ? std::string() : g_cards.back().title;
            e.features = g_features;
            e.label = std::move(label);
            e.hint = std::move(hint);
            g_building.push_back(std::move(e));
            return g_building.back();
        }

        // ---- the jump, while the live page draws

        int chainStep(std::string const& key) {
            if (g_jump.phase != "seeking" && g_jump.phase != "found")
                return -1;
            for (size_t i = 0; i < g_jump.chain.size(); ++i)
                if (g_jump.chain[i] == key)
                    return (int)i;
            return -1;
        }

        void note(std::string const& key, ImRect const& rect, ImGuiWindow* window, bool skipped) {
            int const step = chainStep(key);
            if (step < 0 || (size_t)step >= g_jump.found.size())
                return;
            auto& f = g_jump.found[(size_t)step];
            if (f.valid)
                return;  // the first time it's drawn
            f.valid = true;
            f.rect = rect;
            f.window = window;
            f.skipped = skipped;
        }

        bool highlighting(std::string const& key) {
            if (g_jump.phase != "found" || g_jump.shown < 0 || (size_t)g_jump.shown >= g_jump.chain.size())
                return false;
            return g_jump.chain[(size_t)g_jump.shown] == key && ImGui::GetTime() < g_jump.highlightUntil;
        }

        // The outline round what was jumped to: the accent, fading out (held
        // steady, then gone, with Animations off).
        void drawHighlight(ImGuiWindow* window, ImRect r) {
            if (!window)
                return;
            auto const& pal = look().pal;
            double const left = g_jump.highlightUntil - ImGui::GetTime();
            float alpha = 1.f;
            if (look().effects) {
                float const t = (float)std::clamp(left / kHighlight, 0.0, 1.0);
                // A quick pulse, then fading with the time left.
                float const pulse = 0.75f + 0.25f * (float)std::cos(ImGui::GetTime() * 9.0);
                alpha = std::min(1.f, t * 1.6f) * pulse;
            }
            r.Expand(ImVec2(5.f, 3.f));
            ImRect clip = window->Rect();
            if (window->ParentWindow)
                clip.ClipWith(window->ParentWindow->ClipRect);
            auto* dl = window->DrawList;
            dl->PushClipRect(clip.Min, clip.Max, false);
            ImVec4 fill = pal.accent;
            fill.w = 0.14f * alpha;
            ImVec4 edge = pal.accent;
            edge.w = alpha;
            dl->AddRectFilled(r.Min, r.Max, ImGui::GetColorU32(fill), pal.radius);
            dl->AddRect(r.Min, r.Max, ImGui::GetColorU32(edge), pal.radius, 0, 2.f);
            dl->PopClipRect();
        }

        // ---- matching

        struct Words {
            std::vector<std::string> list;
            std::string whole;
        };
        Words splitQuery(std::string const& q) {
            Words w;
            w.whole = fold(q);
            std::string cur;
            for (char c : w.whole) {
                if (c == ' ') {
                    if (!cur.empty())
                        w.list.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            return w;
        }

        // How well one word is in a field: 3 a whole word, 2 a word's start,
        // 1 inside a word (3 letters or more), 0 not.
        int wordIn(std::string const& field, std::string const& word) {
            if (field.find(" " + word + " ") != std::string::npos)
                return 3;
            if (field.find(" " + word) != std::string::npos)
                return 2;
            if (word.size() >= 3 && field.find(word) != std::string::npos)
                return 1;
            return 0;
        }

        int scoreOf(Entry const& e, Words const& q) {
            if (q.list.empty())
                return 0;
            int total = 0;
            for (auto const& w : q.list) {
                int best = 0;
                auto take = [&](std::string const& field, int weight) {
                    int const m = wordIn(field, w);
                    if (m)
                        best = std::max(best, weight * m);
                };
                take(e.fLabel, 40);
                take(e.fAlias, 30);
                take(e.fWords, 14);
                take(e.fWhere, 10);
                take(e.fHint, 6);
                if (!best)
                    return 0;  // every word has to be somewhere
                total += best;
            }
            // The whole query as the label, or the start of it, or as an old
            // name: those first.
            std::string const& whole = q.whole;  // " words "
            if (e.fLabel == whole)
                total += 400;
            else if (e.fLabel.starts_with(whole.substr(0, whole.size() - 1)))
                total += 150;
            else if (e.fAlias.find(whole.substr(0, whole.size() - 1)) != std::string::npos)
                total += 120;
            // A switch with options, then a row, a card, a button.
            if (e.kind == "feature")
                total += 12;
            else if (e.kind == "row")
                total += 8;
            else if (e.kind == "card")
                total += 4;
            else if (e.kind == "hotkey")
                total -= 20;  // the switch itself comes before its key in Keybinds
            return total;
        }

        std::string whereOf(Entry const& e) {
            std::string w = e.page;
            if (!e.tab.empty() && e.tab != e.page)
                w += " > " + e.tab;
            if (!e.card.empty())
                w += " > " + e.card;
            for (auto const& f : e.features)
                w += " > " + f;
            return w;
        }

        void refreshResults() {
            if (g_resultsFor == g_query && g_resultsBuild == g_builds)
                return;
            g_resultsFor = g_query;
            g_resultsBuild = g_builds;
            g_results.clear();
            g_resultEntries.clear();
            Words const q = splitQuery(g_query);
            if (q.list.empty())
                return;
            std::vector<std::pair<int, size_t>> scored;
            for (size_t i = 0; i < g_index.size(); ++i)
                if (int const s = scoreOf(g_index[i], q); s > 0)
                    scored.emplace_back(s, i);
            std::stable_sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            std::unordered_set<std::string> seen;
            for (auto const& [s, i] : scored) {
                Entry const& e = g_index[i];
                std::string const where = whereOf(e);
                // The same control twice in one place (a label drawn twice
                // in a card) is one result.
                if (!seen.insert(e.label + "\x1f" + where).second)
                    continue;
                Result r;
                r.label = e.label;
                r.where = where;
                r.kind = e.kind;
                r.hint = e.hint;
                r.score = s;
                g_results.push_back(std::move(r));
                g_resultEntries.push_back(i);
                if (g_results.size() >= kMaxResults)
                    break;
            }
        }

        void pick(size_t n) {
            if (n >= g_resultEntries.size() || g_resultEntries[n] >= g_index.size())
                return;
            Entry const& e = g_index[g_resultEntries[n]];
            g_jump = Jump{};
            g_jump.phase = "seeking";
            g_jump.page = e.page;
            g_jump.tab = e.tab;
            g_jump.label = e.label;
            g_jump.chain = e.chain;
            g_jump.found.resize(e.chain.size());
            // What each step of the chain is, for the answer: the card(s),
            // the switches, then the control (looked up in the index).
            for (auto const& key : e.chain) {
                std::string kind = "card", label;
                for (auto const& x : g_index)
                    if (x.key == key) {
                        kind = x.kind;
                        label = x.label;
                        break;
                    }
                g_jump.kinds.push_back(key == e.key ? "control" : kind == "card" ? "card" : "switch");
                g_jump.cards.push_back(kind == "card");
                g_jump.labels.push_back(label);
            }
            g_pickPage = e.page;
            g_clearPending = true;
        }

        bool anyTextActive() {
            ImGuiContext* g = ImGui::GetCurrentContext();
            return g && g->ActiveId != 0 && ImGui::GetIO().WantTextInput;
        }

    } // namespace

    // ---- the kit

    void card(const char* title, const char* caption) {
        std::string const name = title ? title : "";
        std::string const key = counted(scope() + "/c:" + name);
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        if (g_indexing && !name.empty())
            addEntry(key, "card", name, caption ? caption : "");
        if (g_live)
            note(key, w->Rect(), w, w->SkipItems);
        g_cards.push_back({key, name, w});
        g_scopes.push_back(key);
        g_chain.push_back(key);
    }

    void endCard() {
        if (g_cards.empty())
            return;
        Open const c = g_cards.back();
        if (g_live && highlighting(c.key))
            drawHighlight(c.window, c.window->Rect());
        g_cards.pop_back();
        if (!g_scopes.empty())
            g_scopes.pop_back();
        if (!g_chain.empty())
            g_chain.pop_back();
    }

    void row(const char* label, const char* hint) {
        RowOpen r;
        r.label = shown(label);
        r.hint = hint ? hint : "";
        r.key = counted(scope() + "/r:" + r.label);
        if (g_indexing && !r.label.empty()) {
            addEntry(r.key, "row", r.label, r.hint);
            r.entry = g_building.size() - 1;
        }
        g_rows.push_back(std::move(r));
    }

    void endRow(ImVec2 start, float bottom) {
        if (g_rows.empty())
            return;
        RowOpen const r = std::move(g_rows.back());
        g_rows.pop_back();
        g_lastRowKey = r.key;
        g_lastRowLabel = r.label;
        g_lastRowEntry = r.entry;
        if (!g_live)
            return;
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        ImRect const rect(start, ImVec2(std::max(start.x + 1.f, w->WorkRect.Max.x), std::max(bottom, start.y + 1.f)));
        note(r.key, rect, w, w->SkipItems);
        if (highlighting(r.key))
            drawHighlight(w, rect);
    }

    void featureBody(bool open) {
        if (open) {
            // The row just ended is the feature's switch.
            if (g_indexing && g_lastRowEntry < g_building.size())
                g_building[g_lastRowEntry].kind = "feature";
            g_scopes.push_back(g_lastRowKey + "/f");
            g_chain.push_back(g_lastRowKey);
            g_features.push_back(g_lastRowLabel);
        } else {
            if (!g_scopes.empty())
                g_scopes.pop_back();
            if (!g_chain.empty())
                g_chain.pop_back();
            if (!g_features.empty())
                g_features.pop_back();
        }
    }

    void button(const char* label) {
        if (g_mute > 0)
            return;
        std::string const name = shown(label);
        if (name.empty())
            return;
        // Inside a row, its words belong to the row.
        if (!g_rows.empty()) {
            if (g_indexing && g_rows.back().entry < g_building.size())
                g_building[g_rows.back().entry].words += " " + name;
            return;
        }
        std::string const key = counted(scope() + "/b:" + name);
        if (g_indexing)
            addEntry(key, "button", name, "");
        if (g_live) {
            ImGuiWindow* w = ImGui::GetCurrentWindow();
            ImRect const rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            note(key, rect, w, w->SkipItems);
            if (highlighting(key))
                drawHighlight(w, rect);
        }
    }

    void hotkey() {
        if (g_indexing && !g_rows.empty() && g_rows.back().entry < g_building.size() &&
            g_building[g_rows.back().entry].kind == "row")
            g_building[g_rows.back().entry].kind = "hotkey";
    }

    void tabBegin(const char* label) {
        g_tab = label ? label : "";
        g_scopes.push_back(scope() + "/t:" + g_tab);
    }

    void tabEnd() {
        if (!g_scopes.empty())
            g_scopes.pop_back();
        g_tab.clear();
    }

    bool selectTab(const char* label) {
        if (g_jump.phase != "seeking" || g_jump.tabTaken || g_jump.tab.empty() || g_jump.page != g_page ||
            g_jump.tab != (label ? label : ""))
            return false;
        g_jump.tabTaken = true;
        return true;
    }

    Mute::Mute() {
        ++g_mute;
    }
    Mute::~Mute() {
        --g_mute;
    }

    // ---- the shell

    void frameBegin() {
        // A text box let go of by Escape or a pick: emptied once ImGui has
        // let go of it (an active box keeps its own copy of the text).
        if (g_clearPending) {
            ImGuiContext* g = ImGui::GetCurrentContext();
            if (g && g_fieldId != 0 && g->ActiveId == g_fieldId)
                ImGui::ClearActiveID();
            g_query.clear();
            g_clearPending = false;
        }
        if (g_jump.phase == "found" && ImGui::GetTime() >= g_jump.highlightUntil)
            g_jump.phase = "done";
    }

    bool wantIndex() {
        return g_indexWanted && !probe::on();
    }

    void indexBegin() {
        g_building.clear();
        g_indexing = true;
        g_rec = true;
        g_live = false;
    }

    void indexPage(const char* title) {
        resetDrawState(title ? title : "");
    }

    void indexEnd() {
        g_indexing = false;
        g_rec = false;
        for (auto& e : g_building) {
            e.fLabel = fold(e.label);
            e.fAlias = fold(aliasesFor(e.label));
            std::string where = e.page + " " + e.tab + " " + e.card;
            for (auto const& f : e.features)
                where += " " + f;
            e.fWhere = fold(where);
            e.fHint = fold(e.hint);
            e.fWords = fold(e.words);
        }
        g_index = std::move(g_building);
        g_building.clear();
        g_indexWanted = false;
        g_indexedAt = ImGui::GetTime();
        ++g_builds;
        resetDrawState("");
    }

    namespace {
        // What counts as in view for something found: the page's scrolling
        // child, or (Windows layout) the screen, inside the card's own window.
        bool viewFor(Found const& f, ImRect* view) {
            if (!g_free) {
                if (!g_pageWindow)
                    return false;
                *view = g_pageWindow->InnerClipRect;
                return true;
            }
            ImVec2 const display = ImGui::GetIO().DisplaySize;
            *view = ImRect(ImVec2(0.f, 0.f), display);
            if (f.window && f.window->RootWindow)
                view->ClipWith(f.window->RootWindow->InnerClipRect);
            return true;
        }
    } // namespace

    void setFreeWindows(bool on) {
        g_free = on;
    }

    void menuFocused(bool focused) {
        g_focusOverride = focused;
        g_focusOverrideFrame = ImGui::GetFrameCount();
    }

    std::string jumpPage() {
        return g_jump.phase == "seeking" || g_jump.phase == "found" ? g_jump.page : std::string();
    }

    bool seekingCard(std::string const& page, std::string const& title) {
        if (g_jump.phase != "seeking" || g_jump.page != page)
            return false;
        for (size_t i = 0; i < g_jump.labels.size() && i < g_jump.cards.size(); ++i)
            if (g_jump.cards[i] && g_jump.labels[i] == title)
                return true;
        return false;
    }

    void livePageBegin(const char* title) {
        g_pageWindow = ImGui::GetCurrentWindow();
        std::string const page = title ? title : "";
        bool const jumping = (g_jump.phase == "seeking" || g_jump.phase == "found");
        if (jumping && g_jump.page != page) {
            // Another page is showing (the pick switches it a frame later;
            // past that, the person went elsewhere).
            if (g_jump.phase == "found" || g_jump.frames > 3)
                g_jump.phase = "done";
            else
                ++g_jump.frames;
            return;
        }
        if (!jumping)
            return;
        resetDrawState(page);
        for (auto& f : g_jump.found)
            f = Found{};
        g_live = true;
        g_rec = true;
    }

    void livePageEnd() {
        if (!g_live)
            return;
        g_live = false;
        g_rec = false;
        if (g_jump.phase == "found") {
            // Where the highlight is now, for Assistant Access's answer.
            if (g_jump.shown >= 0 && (size_t)g_jump.shown < g_jump.found.size()) {
                auto const& f = g_jump.found[(size_t)g_jump.shown];
                ImRect view;
                if (f.valid && viewFor(f, &view)) {
                    g_jump.shownRect = f.rect;
                    g_jump.inView = f.rect.Max.y > view.Min.y && f.rect.Min.y < view.Max.y;
                }
            }
            return;
        }
        if (g_jump.phase != "seeking")
            return;
        ++g_jump.frames;
        // The deepest step of the chain drawn with its place known.
        int best = -1;
        for (int i = (int)g_jump.found.size() - 1; i >= 0; --i) {
            auto const& f = g_jump.found[(size_t)i];
            bool const card = g_jump.cards[(size_t)i];
            // A card's own rect is right even when ImGui skipped what's in
            // it; anything else drawn in a skipped card isn't laid out.
            if (f.valid && (card || !f.skipped)) {
                best = i;
                break;
            }
        }
        if (best < 0) {
            if (g_jump.frames > kSeekFrames)
                g_jump.phase = "done";
            return;
        }
        auto const& f = g_jump.found[(size_t)best];
        bool const isCard = g_jump.cards[(size_t)best];
        ImRect view;
        bool const hasView = viewFor(f, &view);
        bool const fits = f.rect.GetHeight() <= view.GetHeight();
        bool const inView = hasView && f.rect.Min.y >= view.Min.y - 1.f &&
                            (fits ? f.rect.Max.y <= view.Max.y + 1.f : f.rect.Min.y <= view.Min.y + view.GetHeight() * 0.5f);
        // A card skipped (scrolled out) is scrolled to first; what's in it
        // is laid out the frame after, and the jump goes on to that.
        bool const deeper = isCard && f.skipped && best + 1 < (int)g_jump.found.size();
        bool const still = best == g_jump.lastBest && std::abs(f.rect.Min.y - g_jump.lastY) < 1.f;
        g_jump.lastBest = best;
        g_jump.lastY = f.rect.Min.y;
        if (inView && !deeper && !still && g_jump.frames <= kSeekFrames)
            return;  // look again next frame, once the layout has settled
        if (inView && !deeper) {
            g_jump.phase = "found";
            g_jump.shown = best;
            g_jump.shownRect = f.rect;
            g_jump.inView = true;
            g_jump.highlightUntil = ImGui::GetTime() + kHighlight;
            return;
        }
        if (g_jump.frames > kSeekFrames) {
            // Shown where it is, in view or not.
            g_jump.phase = "found";
            g_jump.shown = best;
            g_jump.shownRect = f.rect;
            g_jump.inView = inView;
            g_jump.highlightUntil = ImGui::GetTime() + kHighlight;
            return;
        }
        // Every window that scrolls between it and the page, innermost first,
        // puts it a quarter of the way down.
        ImRect r = f.rect;
        ImGuiWindow* w = isCard && f.window ? f.window->ParentWindow : f.window;
        for (; w; w = w->ParentWindow) {
            if (w->ScrollMax.y > 0.f) {
                // ... or higher, for a tall one that fits whole only so
                // (11-16: the Trim card, 338 high, hung 2 px off the bottom).
                float const viewH = w->InnerRect.GetHeight();
                float ratio = 0.25f;
                if (viewH > 0.f && r.GetHeight() < viewH)
                    ratio = std::clamp((viewH - r.GetHeight() - 8.f) / viewH, 0.f, 0.25f);
                ImGui::SetScrollFromPosY(w, r.Min.y - w->Pos.y, ratio);
                r = w->Rect();
            }
            if (w == g_pageWindow || !(w->Flags & ImGuiWindowFlags_ChildWindow))
                break;
        }
    }

    std::optional<std::string> takePick() {
        auto p = std::move(g_pickPage);
        g_pickPage.reset();
        return p;
    }

    // ---- the box

    void field(float width) {
        auto* g = ImGui::GetCurrentContext();
        // Ctrl+F puts the cursor in it, when nothing else is being typed in
        // and the menu has the keyboard (its window or a child is focused).
        // A real keyboard's Ctrl+F comes through focusBox() (keybinds.cpp
        // reads cocos's Ctrl): imgui-cocos never hands ImGui those keys going
        // down while no box has the keyboard, on Geode 5 and 6 alike. The
        // check here takes a Ctrl+F put straight into ImGui (Assistant
        // Access's gucci_search key ctrl_f).
        bool const menuFocused = ImGui::GetFrameCount() == g_focusOverrideFrame
                                     ? g_focusOverride
                                     : ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        g_menuFocusedFrame = menuFocused ? ImGui::GetFrameCount() : -100;
        if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
            menuFocused)
            g_focusPending = true;
        if (g_focusPending) {
            ImGui::SetKeyboardFocusHere();
            g_focusPending = false;
        }
        ImGui::PushID("##gucci_search");
        // Escape empties it (and, empty, lets go); Enter picks the first result.
        ImGuiInputTextFlags const flags = ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_EnterReturnsTrue;
        bool const enter = kit::InputText("box", &g_query, "Search the menu", flags, width > 0.f ? width : 0.f);
        g_fieldId = g->LastItemData.ID;
        g_fieldFrame = ImGui::GetFrameCount();
        bool const active = g->ActiveId == g_fieldId;
        // A fresh index each time it's clicked into (a second or two apart),
        // so what the pages show now is what's found.
        if (active && !g_fieldWasActive && ImGui::GetTime() - g_indexedAt > 2.0)
            g_indexWanted = true;
        if (!g_query.empty() && g_index.empty())
            g_indexWanted = true;
        g_fieldWasActive = active;
        ImGui::PopID();
        if (enter && !g_query.empty()) {
            refreshResults();
            if (!g_results.empty())
                pick(0);
        }
    }

    bool showing() {
        return !g_query.empty();
    }

    void results() {
        refreshResults();
        auto const& pal = look().pal;
        g_resultIds.assign(g_results.size(), 0);
        kit::BeginCard("Search", nullptr);
        if (g_index.empty()) {
            kit::Hint("Looking through the menu...");
        } else if (g_results.empty()) {
            kit::Paragraph(emptyMessage().c_str(), Tone::Muted);
        } else {
            for (size_t i = 0; i < g_results.size(); ++i) {
                auto const& r = g_results[i];
                ImGui::PushID((int)i);
                ImVec2 const at = ImGui::GetCursorScreenPos();
                float const lineH = ImGui::GetTextLineHeight();
                float rowH = lineH + 4.f;
#if defined(GEODE_IS_MOBILE)
                // A finger's height, as the sidebar's rows are on a phone.
                rowH = std::max(rowH, 34.f);
#endif
                // The label and where it lives sit in the row's middle: the
                // Selectable draws the label, where it lives follows, muted.
                float const textY = std::round((rowH - lineH) * 0.5f);
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
                bool const picked = ImGui::Selectable(fmt::format("{}##result", r.label).c_str(), false,
                                                      ImGuiSelectableFlags_None, ImVec2(0.f, rowH));
                ImGui::PopStyleVar();
                if (picked)
                    pick(i);
                g_resultIds[i] = ImGui::GetItemID();
                ImGui::PushClipRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);
                std::string const where = "- " + r.where;
                ImVec2 const whereAt = ImVec2(at.x + ImGui::CalcTextSize(r.label.c_str()).x + 8.f, at.y + textY);
                ImGui::GetWindowDrawList()->AddText(whereAt, ImGui::GetColorU32(pal.muted), where.c_str());
                ImGui::PopClipRect();
                if (!r.hint.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", r.hint.c_str());
                ImGui::PopID();
            }
            if (g_results.size() >= kMaxResults)
                kit::Hint("More match: type more of the name.");
        }
        kit::EndCard();
    }

    // ---- Assistant Access

    std::string query() {
        return g_query;
    }

    std::vector<Result> currentResults() {
        refreshResults();
        return g_results;
    }

    std::string emptyMessage() {
        refreshResults();
        if (g_query.empty() || !g_results.empty() || g_index.empty())
            return "";
        return fmt::format("Nothing in the menu matches \"{}\".", g_query);
    }

    ImGuiID fieldId() {
        // Drawn this frame or the last (the tools run between frames).
        return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - g_fieldFrame <= 1 ? g_fieldId : 0;
    }

    std::vector<ImGuiID> resultIds() {
        return g_resultIds;
    }

    bool fieldActive() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        return g && fieldId() != 0 && g->ActiveId == g_fieldId;
    }

    void requestIndex() {
        g_indexWanted = true;
    }

    int indexSize() {
        return (int)g_index.size();
    }

    int indexBuilds() {
        return g_builds;
    }

    JumpState jumpState() {
        JumpState s;
        s.phase = g_jump.phase;
        s.page = g_jump.page;
        s.target = g_jump.label;
        s.frames = g_jump.frames;
        if (g_jump.shown >= 0 && (size_t)g_jump.shown < g_jump.kinds.size()) {
            s.shownKind = g_jump.kinds[(size_t)g_jump.shown];
            s.shownLabel = g_jump.labels[(size_t)g_jump.shown];
            ImRect const& r = g_jump.shownRect;
            s.rect = ImVec4(r.Min.x, r.Min.y, r.GetWidth(), r.GetHeight());
            s.inView = g_jump.inView;
            s.highlightLeft = g_jump.phase == "found" ? (float)std::max(0.0, g_jump.highlightUntil - ImGui::GetTime()) : 0.f;
        }
        return s;
    }

    void clear() {
        g_clearPending = true;
    }

    namespace {
        bool menuHasKeyboard() {
            return ImGui::GetCurrentContext() && ImGui::GetFrameCount() - g_menuFocusedFrame <= 1;
        }
    } // namespace

    bool ctrlFIsMine(bool cocosCtrl) {
        // Not ImGui's Ctrl as well: on Geode 6 it sticks down after Ctrl is let
        // go, and a plain F then opened the box (ui::ctrlHeld).
        return menuHasKeyboard() && cocosCtrl;
    }

    void focusBox() {
        if (menuHasKeyboard() && !ImGui::GetIO().WantTextInput)
            g_focusPending = true;
    }

    bool takeBack() {
        // Escape can come twice for one press (as a key, then as cocos's
        // back-click): the second, after the box let go, is taken too.
        static int s_tookAt = -1;
        int const frame = ImGui::GetFrameCount();
        if (!anyTextActive())
            return frame == s_tookAt;
        s_tookAt = frame;
        // The search box empties, as Escape does there; any other box just
        // lets go (ImGui's own Escape, if it came too, put its text back).
        if (fieldActive())
            g_clearPending = true;
        // Only the box lets go: the window keeps its focus (not
        // touch::letGoOfKeyboard, which drops that too), so after Escape in
        // one of the Frame Editor's own fields its keys (Ctrl+Z, the arrows,
        // Delete: ui/pages.hpp) are still its own. A phone's keyboard still
        // goes away, as nothing wants text input any more.
        if (ImGuiContext* g = ImGui::GetCurrentContext(); g && g->ActiveId != 0)
            ImGui::ClearActiveID();
        return true;
    }

} // namespace gucci::ui::search

// Back while a menu text box is being typed in: the box lets go, and the
// game doesn't go back (pause a level, leave a menu). Escape reaches cocos as
// a keyboard key, which imgui-cocos keeps from the game while ImGui wants the
// keyboard but, on Geode 5, doesn't hand to ImGui either (its key map there
// has no Escape), so it did nothing in a box: taken here first. A phone's back button (and, on
// some platforms, Escape as well) is cocos's keypad back-click. iOS has no
// back button, and its keyboard modify headers don't build.
#if !defined(GEODE_IS_IOS)
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCKeypadDispatcher.hpp>

class $modify(GBSearchKeys, CCKeyboardDispatcher) {
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG", Priority::First);
    }

    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double time) {
        if (key == KEY_Escape && down && ImGui::GetCurrentContext() && gucci::ui::search::takeBack())
            return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, time);
    }
};

class $modify(GBSearchKeypad, CCKeypadDispatcher) {
    bool dispatchKeypadMSG(ccKeypadMSGType type) {
        if (type == kTypeBackClicked && ImGui::GetCurrentContext() && gucci::ui::search::takeBack())
            return true;
        // The Frame Editor took this press's Escape to clear its selection
        // (ui/pages.hpp, editorKey): its back-click, where one comes too, isn't a pause.
        if (type == kTypeBackClicked && gucci::ui::pages::editorTookBack())
            return true;
        return CCKeypadDispatcher::dispatchKeypadMSG(type);
    }
};
#endif

// An arrow while a menu text box is typed in, on Windows: Geode hands
// cocos's text input an "a" alongside the arrow (GD's own text fields read
// the key from it and move their cursor). Geode 5 does it for Left and Right,
// and imgui-cocos 9764333 types that "a" into the ImGui box. Geode 6
// (6.0.0-alpha.2, input.cpp) does it for Up and Down too, and imgui-cocos
// 934958c leaves out only Left and Right, so Up or Down typed the "a" there.
// Taken here first, only while ImGui has the keyboard, when imgui-cocos hands
// it to no cocos field anyway; the arrow key itself still reaches ImGui and
// moves the cursor.
#if defined(GEODE_IS_WINDOWS)
#include <Geode/modify/CCIMEDispatcher.hpp>

class $modify(GBMenuArrowText, CCIMEDispatcher) {
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("cocos2d::CCIMEDispatcher::dispatchInsertText", Priority::First);
    }

    void dispatchInsertText(char const* text, int len, enumKeyCodes keys) {
        if ((keys == KEY_Left || keys == KEY_Right || keys == KEY_Up || keys == KEY_Down) && ImGui::GetCurrentContext() &&
            ImGui::GetIO().WantCaptureKeyboard)
            return;
        CCIMEDispatcher::dispatchInsertText(text, len, keys);
    }
};
#endif

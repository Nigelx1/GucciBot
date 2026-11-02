// The event recorder for Assistant Access (mcp/events.hpp).

// ImGui's internals first: core/GucciBot.hpp opens geode::prelude for every
// file after it, and then imgui_internal.h's own log() reads as geode::log.
#include <imgui.h>
#include <imgui_internal.h>

#include "mcp/events.hpp"

#include "mcp/mcp_server.hpp"
#include "mcp/scene.hpp"

#include "analysis/ac/framewindow.hpp"
#include "core/GucciBot.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/OverlayManager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>

using namespace geode::prelude;

namespace gucci::mcp::events {

    namespace {

        struct Event {
            uint64_t seq = 0;
            int64_t frame = 0;
            int64_t tick = -1;
            double ms = 0.0;
            std::string kind;
            std::string text;
            matjson::Value data;
        };

        constexpr size_t kRing = 2000;
        std::deque<Event> g_ring;
        uint64_t g_seq = 0;
        std::chrono::steady_clock::time_point const g_start = std::chrono::steady_clock::now();

        // What the last tick saw. Nodes are only ever looked up among the
        // children there are now, so one that's gone is never touched again:
        // only the words kept for it are.
        struct Seen {
            bool started = false;
            CCScene* scene = nullptr;
            PlayLayer* level = nullptr;
            bool dead = false;
            bool complete = false;
            int64_t frame = 0;
            int mode = 0;
            std::string owner;
            std::map<CCNode*, std::string> notes;   // notification: its text
            std::map<CCNode*, std::string> alerts;  // alert: its title
            std::vector<ImGuiID> popups;
            std::string report;
            bool reportOk = false;
            std::string render;  // the last render's record, as one string
        };
        Seen g;

        char const* modeName(int m) {
            switch (m) {
                case 1: return "record";
                case 2: return "play";
                default: return "idle";
            }
        }

        int modeOf(GucciEngine* gb) {
            return gb->isRecording() ? 1 : gb->isPlaying() ? 2 : 0;
        }

        std::string popupName(ImGuiContext& ctx, ImGuiID id) {
            for (auto const& p : ctx.OpenPopupStack)
                if (p.PopupId == id && p.Window)
                    return p.Window->Name;
            return fmt::format("0x{:08X}", id);
        }

        // The notifications and alerts on screen now, against the last tick's.
        void watchNodes(bool first) {
            std::map<CCNode*, std::string> notes;
            std::map<CCNode*, std::string> alerts;
            auto scan = [&](CCNode* root) {
                if (!root)
                    return;
                for (auto* child : CCArrayExt<CCNode*>(root->getChildren())) {
                    if (typeinfo_cast<Notification*>(child)) {
                        std::string text;
                        for (auto const& t : scene::textsUnder(child))
                            text += (text.empty() ? "" : "\n") + t;
                        auto const it = g.notes.find(child);
                        if (it == g.notes.end() || it->second != text) {
                            auto const box = scene::boxOf(child, true);
                            auto data = matjson::Value::object();
                            data["box"] = scene::boxJson(box);
                            data["offscreen"] = scene::offscreen(box);
                            if (it != g.notes.end())
                                data["updated"] = true;
                            add("notification", text, data);
                        }
                        notes[child] = text;
                    } else if (typeinfo_cast<FLAlertLayer*>(child)) {
                        auto info = scene::alertOf(child);
                        auto const title = info["title"].asString().unwrapOr("");
                        if (!g.alerts.contains(child)) {
                            std::string text = title;
                            if (auto const body = info["body"].asString().unwrapOr(""); !body.empty())
                                text += (text.empty() ? "" : "\n") + body;
                            add("alert", text, info);
                        }
                        alerts[child] = title;
                    }
                }
            };
            scan(OverlayManager::get());
            scan(CCDirector::get()->getRunningScene());
            if (!first)
                for (auto const& [node, title] : g.alerts)
                    if (!alerts.contains(node)) {
                        auto data = matjson::Value::object();
                        data["title"] = title;
                        add("alert_closed", title, data);
                    }
            g.notes = std::move(notes);
            g.alerts = std::move(alerts);
        }

        void watchPopups(bool first) {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            if (!ctx)
                return;
            std::vector<ImGuiID> now;
            for (auto const& p : ctx->OpenPopupStack)
                now.push_back(p.PopupId);
            if (!first && now != g.popups) {
                auto opened = matjson::Value::array();
                auto closed = matjson::Value::array();
                for (ImGuiID id : now)
                    if (std::find(g.popups.begin(), g.popups.end(), id) == g.popups.end())
                        opened.push(popupName(*ctx, id));
                for (ImGuiID id : g.popups)
                    if (std::find(now.begin(), now.end(), id) == now.end())
                        closed.push(fmt::format("0x{:08X}", id));
                if (opened.size() > 0 || closed.size() > 0) {
                    auto data = matjson::Value::object();
                    data["opened"] = opened;
                    data["closed"] = closed;
                    data["open"] = (int64_t)now.size();
                    add("imgui_popup", opened.size() > 0 ? "opened" : "closed", data);
                }
            }
            g.popups = std::move(now);
        }

    } // namespace

    void add(std::string kind, std::string text, matjson::Value data) {
        Event e;
        e.seq = ++g_seq;
        e.frame = (int64_t)GucciEngine::get()->updater.getFrame();
        if (auto* pl = PlayLayer::get())
            e.tick = (int64_t)pl->m_gameState.m_currentProgress;
        e.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
        e.kind = std::move(kind);
        e.text = std::move(text);
        e.data = std::move(data);
        g_ring.push_back(std::move(e));
        while (g_ring.size() > kRing)
            g_ring.pop_front();
    }

    uint64_t lastSeq() {
        return g_seq;
    }

    void tick() {
        bool const first = !g.started;
        g.started = true;
        auto* gb = GucciEngine::get();

        // The screen: a new scene once its transition is over.
        if (auto* running = CCDirector::get()->getRunningScene();
            running && running != g.scene && !typeinfo_cast<CCTransitionScene*>(running)) {
            g.scene = running;
            std::string layer;
            if (running->getChildrenCount() > 0)
                layer = scene::className(static_cast<CCObject*>(running->getChildren()->objectAtIndex(0)));
            auto data = matjson::Value::object();
            data["layer"] = layer;
            if (!first)
                add("scene", layer, data);
        }

        // The level, and player 1 in it.
        auto* pl = PlayLayer::get();
        if (pl != g.level) {
            if (g.level && !first)
                add("level_quit", "");
            if (pl) {
                auto data = matjson::Value::object();
                std::string name;
                if (pl->m_level) {
                    name = pl->m_level->m_levelName;
                    data["id"] = (int64_t)pl->m_level->m_levelID.value();
                }
                data["name"] = name;
                if (!first)
                    add("level_open", name, data);
            }
            g.level = pl;
            g.dead = false;
            g.complete = false;
            g.frame = (int64_t)gb->updater.getFrame();
        }
        if (pl && pl->m_player1) {
            auto* p = pl->m_player1;
            int64_t const frame = (int64_t)gb->updater.getFrame();
            bool const dead = p->m_isDead;
            if (dead && !g.dead && !first) {
                auto data = matjson::Value::object();
                data["x"] = (double)p->getPositionX();
                data["y"] = (double)p->getPositionY();
                if (gb->m_levelLength > 0.f)
                    data["percent"] = (double)(p->getPositionX() / gb->m_levelLength * 100.f);
                add("death", "", data);
            } else if (!dead && g.dead) {
                add("respawn", "");
            } else if (frame < g.frame && !first) {
                auto data = matjson::Value::object();
                data["from"] = g.frame;
                add("reset", "", data);
            }
            if (pl->m_hasCompletedLevel && !g.complete && !first)
                add("complete", "");
            g.dead = dead;
            g.complete = pl->m_hasCompletedLevel;
            g.frame = frame;
        }

        // What GucciBot is doing.
        if (int const mode = modeOf(gb); mode != g.mode) {
            if (!first) {
                auto data = matjson::Value::object();
                data["from"] = modeName(g.mode);
                data["to"] = modeName(mode);
                add("mode", modeName(mode), data);
            }
            g.mode = mode;
        }
        if (auto const owner = runOwner(); owner != g.owner) {
            if (!first) {
                if (!g.owner.empty())
                    add("run_end", g.owner);
                if (!owner.empty())
                    add("run_start", owner);
            }
            g.owner = owner;
        }
        if (gb->fwAcReport != g.report || gb->fwAcOk != g.reportOk) {
            if (!first && !gb->fwAcReport.empty()) {
                auto data = matjson::Value::object();
                data["ok"] = gb->fwAcOk;
                add("calculate", gb->fwAcReport, data);
            }
            g.report = gb->fwAcReport;
            g.reportOk = gb->fwAcOk;
        }
        {
            auto const& r = SLRenderer::get()->m_lastRender;
            std::string const key =
                fmt::format("{}|{}|{}|{}|{}", r.success, r.path, r.fileSize, r.duration, r.width);
            if (key != g.render) {
                if (!first && (!r.path.empty() || r.pending)) {
                    auto data = matjson::Value::object();
                    data["success"] = r.success;
                    data["path"] = r.path;
                    data["width"] = (int64_t)r.width;
                    data["height"] = (int64_t)r.height;
                    data["fps"] = (int64_t)r.fps;
                    data["seconds"] = r.duration;
                    data["bytes"] = (int64_t)r.fileSize;
                    add("render", r.success ? "finished" : "failed", data);
                }
                g.render = key;
            }
        }

        watchNodes(first);
        watchPopups(first);
    }

    matjson::Value read(uint64_t since, std::vector<std::string> const& kinds, size_t limit) {
        auto out = matjson::Value::object();
        auto list = matjson::Value::array();
        uint64_t cursor = g_seq;
        bool more = false;
        for (auto const& e : g_ring) {
            if (e.seq <= since)
                continue;
            if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), e.kind) == kinds.end())
                continue;
            if (list.size() >= limit) {
                more = true;
                break;
            }
            auto j = matjson::Value::object();
            j["seq"] = (int64_t)e.seq;
            j["frame"] = e.frame;
            if (e.tick >= 0)
                j["tick"] = e.tick;
            j["ms"] = std::round(e.ms * 10.0) / 10.0;
            j["kind"] = e.kind;
            if (!e.text.empty())
                j["text"] = e.text;
            if (e.data.isObject() && e.data.size() > 0)
                j["data"] = e.data;
            list.push(j);
            cursor = e.seq;
        }
        uint64_t const oldest = g_ring.empty() ? g_seq + 1 : g_ring.front().seq;
        out["seq"] = (int64_t)(more ? cursor : g_seq);
        out["latest"] = (int64_t)g_seq;
        out["more"] = more;
        out["dropped"] = (int64_t)(oldest > since + 1 ? oldest - since - 1 : 0);
        out["events"] = list;
        return out;
    }

} // namespace gucci::mcp::events

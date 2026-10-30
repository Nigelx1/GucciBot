// The update checker, back (Nigel, 2026-10-19). The first one (1.6.5) crashed
// the game on launch: a geode::Task coroutine awaited an arc web future, and
// arc had no context to wake it in (engine_core.cpp keeps the post-mortem).
// This one awaits only inside an arc coroutine, spawned on Geode's arc runtime
// (the handle dropped, so it detaches and runs to the end), and hands the reply
// to the main thread. Not async::spawn / TaskHolder: this MSVC hits an internal
// compiler error instantiating them (C1001, arc Pollable.hpp:174; themes.cpp
// has the same note).
//
// Once per launch, when the main menu first shows, if "Check for updates" is
// on (Settings). The newest release on GitHub decides; pre-releases count when
// this build is one, or when the setting allows them. Found mid-level, the news
// waits for the main menu: a popup there could cost an attempt.

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace geode::prelude;

namespace gucci::updates {

    namespace {
        constexpr const char* kReleasesApi = "https://api.github.com/repos/Nigelx1/GucciBot/releases?per_page=10";

        bool g_checked = false;

        struct News {
            std::string version;
            std::string url;
        };
        std::optional<News> g_news;

        void showNews() {
            if (!g_news || PlayLayer::get() || LevelEditorLayer::get())
                return;
            News const n = *g_news;
            g_news.reset();
            createQuickPopup("GucciBot Update",
                             fmt::format("<cy>GucciBot {}</c> is out. You have {}.", n.version,
                                         Mod::get()->getVersion().toVString()),
                             "Later", "Get It", [url = n.url](auto, bool get) {
                                 if (get)
                                     web::openLinkInBrowser(url);
                             });
        }

        void onReleases(web::WebResponse res) {
            // Back safely: the guard set in checkOnce comes down, and on disk
            // right away. Left to the save at exit, a crash later in the
            // session left it up and the next launch switched the checker off
            // by mistake (2026-10-23, polish item 43).
            Mod::get()->setSavedValue("update_check_inflight", false);
            (void)Mod::get()->saveData();
            if (!res.ok()) {
                log::info("[GucciBot] update check: HTTP {}", res.code());
                return;
            }
            auto parsed = res.json();
            if (!parsed || !parsed.unwrap().isArray())
                return;
            auto const mine = Mod::get()->getVersion();
            bool const wantPre =
                mine.getTag().has_value() || Mod::get()->getSavedValue<bool>("update_prereleases", false);
            for (auto const& rel : parsed.unwrap()) {
                if (rel["draft"].asBool().unwrapOr(false))
                    continue;
                if (rel["prerelease"].asBool().unwrapOr(false) && !wantPre)
                    continue;
                std::string tag = rel["tag_name"].asString().unwrapOr("");
                if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
                    tag.erase(0, 1);
                auto const ver = VersionInfo::parse(tag);
                if (!ver)
                    continue;  // a tag Geode can't read (2.alpha.3): the next one decides
                if (mine < ver.unwrap()) {
                    g_news = News{ver.unwrap().toVString(),
                                  rel["html_url"].asString().unwrapOr("https://github.com/Nigelx1/GucciBot/releases")};
                    log::info("[GucciBot] update check: {} is out", g_news->version);
                    showNews();
                } else {
                    log::info("[GucciBot] update check: up to date ({})", mine.toVString());
                }
                return;
            }
        }
        arc::Future<int> checkTask() {
            web::WebResponse res = co_await web::WebRequest()
                                       .userAgent("GucciBot")
                                       .header("Accept", "application/vnd.github+json")
                                       .timeout(std::chrono::seconds(15))
                                       .get(kReleasesApi);
            geode::queueInMainThread([res = std::move(res)]() mutable { onReleases(std::move(res)); });
            co_return 0;
        }
    } // namespace

    void checkOnce() {
        if (g_checked)
            return;
        g_checked = true;
        auto* mod = Mod::get();
        if (!mod->getSavedValue<bool>("update_check", true))
            return;
        // A crash guard, for the first checker's sake: the flag goes to disk
        // before the request and comes down when the reply is back. Found still
        // up at launch, the last check never finished (it may have taken the
        // game down with it), so the checker switches itself off and says so
        // rather than risk it again. Quitting mid-check trips it too; the
        // switch in Settings puts it back.
        if (mod->getSavedValue<bool>("update_check_inflight", false)) {
            mod->setSavedValue("update_check_inflight", false);
            mod->setSavedValue("update_check", false);
            (void)mod->saveData();
            log::warn("[GucciBot] update check: the last one never finished; switched off");
            Notification::create("GucciBot's update check didn't finish last time, so it's off now "
                                 "(Settings > Check for updates).",
                                 NotificationIcon::Warning, 6.f)
                ->show();
            return;
        }
        mod->setSavedValue("update_check_inflight", true);
        (void)mod->saveData();
        // The handle is a temporary: dropping it detaches the task.
        (void)geode::async::runtime().spawn(checkTask());
    }

    void menuShown() {
        checkOnce();
        showNews();
    }

} // namespace gucci::updates

class $modify(GBUpdateMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init())
            return false;
        gucci::updates::menuShown();
        return true;
    }
};

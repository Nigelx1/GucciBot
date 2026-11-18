// GucciBot (2026-11-17): gucci_trainer_lane, for checking the Trainer's lane
// (trainers/lane.hpp) in a hidden test game: load a trainer macro, put the
// bar where a shot needs it, file presses and releases a set number of ticks
// off the clock, set the lane's settings for the launch (never saved), and
// read back what the lane holds. Everything but the read is refused outside a
// test launch.

#include "mcp_server.hpp"

#include "core/GucciBot.hpp"
#include "hacks/safemode.hpp"
#include "trainers/lane.hpp"
#include "trainers/trainer_core.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {
        namespace tr = gucci::trainers;
        namespace ln = gucci::trainers::lane;

        double num(matjson::Value const& a, char const* key, double def) {
            if (!a.contains(key))
                return def;
            if (auto d = a[key].asDouble())
                return *d;
            if (auto i = a[key].asInt())
                return (double)*i;
            return def;
        }

        bool flag(matjson::Value const& a, char const* key, bool def) {
            if (!a.contains(key))
                return def;
            return a[key].asBool().unwrapOr(def);
        }

        std::string text(matjson::Value const& a, char const* key) {
            if (!a.contains(key))
                return {};
            return a[key].asString().unwrapOr("");
        }

        char const* verdictName(lanerules::Verdict v) {
            switch (v) {
                case lanerules::Verdict::Perfect: return "perfect";
                case lanerules::Verdict::Inside: return "inside";
                case lanerules::Verdict::Outside: return "outside";
                case lanerules::Verdict::Missed: return "missed";
            }
            return "";
        }

        matjson::Value schema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("kind", "string", "\"trainer\" (default) or \"jmf\"");
            field("select", "string", "load this saved macro (its name) into the Trainer");
            field("ext", "string", "with select: the file's extension (\".gbr6\", ...)");
            field("bar", "boolean", "the click bar on or off (this launch)");
            field("seek", "number", "away from the level: put the bar's clock here (seconds)");
            field("paused", "boolean", "away from the level: pause or run the bar's clock");
            field("press", "boolean", "file a press (true) or a release (false) as the player's");
            field("offset_ticks", "number", "with press: this many ticks off the lane's clock now (default 0)");
            field("at_sec", "number", "with press: at this time on the clock instead");
            field("style", "string", "\"lane\" or \"classic\" (this launch)");
            field("lead_ticks", "integer", "the lane's lead (this launch)");
            field("size", "number", "the lane's size (this launch)");
            field("in_level", "boolean", "the lane over the level (this launch)");
            field("place", "string", "\"top\" or \"bottom\" (this launch)");
            field("width", "number", "the lane's width in the level, of the screen (this launch)");
            field("flash_age", "number", "make the last verdict this many seconds old (its pop, pulse, fade)");
            field("flash_hold", "boolean", "with flash_age: hold it at that age until a flash_age call without it");
            field("notes", "boolean", "list the notes in view with their tiers and results");
            auto s = matjson::Value::object();
            s["type"] = "object";
            s["properties"] = props;
            return s;
        }
    } // namespace

    void registerTrainerLaneTools(Server& server) {
        server.addTool({
            "gucci_trainer_lane",
            "The Trainer's lane (an osu!taiko-style click trainer): load a trainer macro, place or run the "
            "bar's clock, file presses and releases some ticks off the clock as the player's, set the lane's "
            "settings for this launch (never saved), and read what it holds: its clock, notes, counted windows, "
            "tally, the last verdict and whether it is drawn over the level. All but reading: test launches only.",
            schema(),
            [](matjson::Value const& a) {
                auto const kind = text(a, "kind") == "jmf" ? tr::Kind::Jupiter : tr::Kind::Any;
                bool const writes = a.contains("select") || a.contains("bar") || a.contains("seek") ||
                                    a.contains("paused") || a.contains("press") || a.contains("style") ||
                                    a.contains("lead_ticks") || a.contains("size") || a.contains("in_level") ||
                                    a.contains("place") || a.contains("width") ||
                                    a.contains("flash_age");
                if (writes && !gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                tr::ensureLoaded();
                auto s = tr::state(kind);
                auto& set = ln::settings();
                if (a.contains("select")) {
                    if (!tr::selectMacro(text(a, "select"), text(a, "ext")))
                        throw ToolError("that macro didn't load into the Trainer");
                }
                if (a.contains("bar"))
                    s.barEnabled = flag(a, "bar", true);
                if (a.contains("style"))
                    set.style = text(a, "style") == "classic" ? ln::Style::Classic : ln::Style::Lane;
                if (a.contains("lead_ticks"))
                    set.leadTicks = std::clamp((int)num(a, "lead_ticks", set.leadTicks), 24, 960);
                if (a.contains("size"))
                    set.size = std::clamp((float)num(a, "size", set.size), 0.6f, 2.f);
                if (a.contains("in_level"))
                    set.inLevel = flag(a, "in_level", true);
                if (a.contains("place"))
                    set.place = text(a, "place") == "bottom" ? ln::Place::Bottom : ln::Place::Top;
                if (a.contains("width"))
                    set.levelWidth = std::clamp((float)num(a, "width", set.levelWidth), 0.3f, 1.f);
                if (a.contains("seek"))
                    tr::seek(kind, num(a, "seek", 0.0));
                if (a.contains("paused"))
                    tr::setPaused(kind, flag(a, "paused", true));
                if (a.contains("press")) {
                    double const tps = ln::view(kind).tps;
                    double const at = a.contains("at_sec") ? num(a, "at_sec", 0.0)
                                                           : ln::now(kind) + num(a, "offset_ticks", 0.0) / tps;
                    tr::injectForTest(kind, flag(a, "press", true), at);
                }

                if (a.contains("flash_age"))
                    ln::ageFlashForTest(kind, std::max(0.0, num(a, "flash_age", 0.0)), flag(a, "flash_hold", false));

                auto out = matjson::Value::object();
                auto const v = ln::view(kind);
                double const now = ln::now(kind);
                out["now_sec"] = now;
                out["pos_sec"] = s.posSec;
                out["paused"] = s.paused;
                out["bar"] = s.barEnabled;
                out["following_level"] = tr::followsLevel(kind);
                out["shown_in_level"] = ln::shownInLevel(kind);
                out["page_visible"] = s.pageVisible;
                out["style"] = set.style == ln::Style::Lane ? "lane" : "classic";
                out["lead_ticks"] = (int64_t)set.leadTicks;
                out["tps"] = v.tps;
                out["macro"] = GucciEngine::get()->trainerMacroName;
                out["notes_total"] = (int64_t)v.notes->size();
                out["notes_with_window"] = (int64_t)v.counted;
                out["windows_in_file"] = (int64_t)s.macro.windowsCounted;
                out["holding"] = (int64_t)v.holding;
                auto tally = matjson::Value::object();
                tally["perfect"] = (int64_t)v.tally.perfect;
                tally["inside"] = (int64_t)v.tally.inside;
                tally["outside"] = (int64_t)v.tally.outside;
                tally["missed"] = (int64_t)v.tally.missed;
                out["tally"] = tally;
                auto fl = matjson::Value::object();
                fl["text"] = v.flash.text;
                fl["verdict"] = verdictName(v.flash.verdict);
                fl["release"] = v.flash.release;
                fl["age_sec"] = ln::steadyNow() - v.flash.atReal;
                out["last"] = fl;
                if (flag(a, "notes", false)) {
                    auto arr = matjson::Value::array();
                    double const lead = set.leadTicks / (v.tps > 0.0 ? v.tps : 240.0);
                    for (size_t i = 0; i < v.notes->size(); ++i) {
                        auto const& n = (*v.notes)[i];
                        if (n.releaseSec < now - 1.0 || n.pressSec > now + lead + 0.2)
                            continue;
                        auto const& r = (*v.results)[i];
                        auto e = matjson::Value::object();
                        e["i"] = (int64_t)i;
                        e["press_sec"] = n.pressSec;
                        e["release_sec"] = n.releaseSec;
                        e["press_window"] = (double)n.pressWindow;
                        e["release_window"] = (double)n.releaseWindow;
                        e["tier"] = (int64_t)n.pressTier;
                        e["tail"] = (int64_t)n.tail;
                        e["early"] = (int64_t)n.pressAllow.early;
                        e["late"] = (int64_t)n.pressAllow.late;
                        e["pressed"] = r.pressed;
                        e["press_delta"] = (int64_t)r.pressDelta;
                        e["press_verdict"] = r.pressed ? verdictName(r.pressVerdict) : "";
                        e["released"] = r.released;
                        e["missed"] = r.missed;
                        arr.push(e);
                        if (arr.size() >= 40)
                            break;
                    }
                    out["notes"] = arr;
                }
                return out;
            },
        });
    }

} // namespace gucci::mcp

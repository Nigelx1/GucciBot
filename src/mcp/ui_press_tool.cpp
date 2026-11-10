// GucciBot (2026-11-10): gucci_ui_press, pressing one of GucciBot's own menu
// controls by the ID gucci_ui lists for it, in a test launch. A hidden test
// game gets no mouse into ImGui, so the 2026-11-10 test pass couldn't press
// a single menu button; this goes in through ImGui's activation instead
// (ui/press.hpp): the widget sees itself clicked the next frame, as it would
// for a person.
//
// One call: a capture of the menu (ui/probe.hpp) finds the control and what
// it is; a disabled one is refused there and then (and again by the press
// itself, in case it changed); the press runs; a second capture reads the
// control again and any popup that opened. A dropdown with an option is two
// presses: the dropdown, then the option in the list it opened.
//
// It presses exactly what it's told. A control that opens a file dialog or a
// folder opens it: not something to press in a hidden test game.

#include "mcp_server.hpp"

#include "hacks/legit.hpp"
#include "hacks/safemode.hpp"
#include "ui/press.hpp"
#include "ui/probe.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <memory>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        std::string idText(uint32_t id) {
            return fmt::format("0x{:08X}", id);
        }

        uint32_t parseId(matjson::Value const& v) {
            if (v.isNumber())
                return (uint32_t)v.asInt().unwrapOr(0);
            auto const s = v.asString().unwrapOr("");
            try {
                size_t used = 0;
                auto const n = std::stoul(s, &used, 16);
                if (used == s.size() && n != 0)
                    return (uint32_t)n;
            } catch (...) {
            }
            throw ToolError("id is an ImGui ID as gucci_ui lists it, e.g. \"0x8F3A12C0\"");
        }

        // The last capture's element with this ID (a control's own, or one of
        // a choice's segments), as gucci_ui's json gives it.
        std::optional<matjson::Value> elementById(uint32_t id) {
            ui::probe::Query q;
            q.finding = true;
            q.findId = id;
            q.text = false;
            q.max = 4;
            auto r = ui::probe::result(q);
            if (!r.contains("elements") || r["elements"].size() == 0)
                return std::nullopt;
            return r["elements"][size_t(0)];
        }

        // An item in the open dropdown list by its text: exactly (ignoring
        // case) first, then a part of it.
        std::optional<uint32_t> comboItem(std::string const& option) {
            ui::probe::Query q;
            q.finding = true;
            q.findWindow = "combo";
            q.findLabel = option;
            q.text = false;
            q.max = 50;
            auto r = ui::probe::result(q);
            if (!r.contains("elements"))
                return std::nullopt;
            std::optional<uint32_t> part;
            for (auto const& e : r["elements"]) {
                if (!e.contains("id"))
                    continue;
                uint32_t const id = (uint32_t)std::stoul(e["id"].asString().unwrapOr("0"), nullptr, 16);
                if (lower(e["label"].asString().unwrapOr("")) == lower(option))
                    return id;
                if (!part)
                    part = id;
            }
            return part;
        }

        // What the popups show after the press (an opened "..." menu, a
        // dropdown's list, a confirm), as gucci_ui's outline.
        std::string popupText() {
            ui::probe::Query q;
            q.window = "popups";
            q.maxLines = 60;
            auto r = ui::probe::result(q);
            auto const s = r["outline"].asString().unwrapOr("");
            return s.starts_with("(") ? std::string() : s;
        }

        // An option of a choice or dropdown: its name (exactly, then ignoring
        // case) or its index.
        int optionIndex(matjson::Value const& el, matjson::Value const& opt) {
            std::vector<std::string> options;
            if (el.contains("options"))
                for (auto const& o : el["options"])
                    options.push_back(o.asString().unwrapOr(""));
            if (opt.isNumber()) {
                int const i = (int)opt.asInt().unwrapOr(-1);
                if (i < 0 || i >= (int)options.size())
                    throw ToolError(fmt::format("option {} is out of range (0 to {})", i, (int)options.size() - 1));
                return i;
            }
            auto const want = opt.asString().unwrapOr("");
            for (int pass = 0; pass < 2; ++pass)
                for (size_t i = 0; i < options.size(); ++i)
                    if (pass == 0 ? options[i] == want : lower(options[i]) == lower(want))
                        return (int)i;
            std::string list;
            for (auto const& o : options)
                list += (list.empty() ? "" : ", ") + o;
            throw ToolError("no option \"" + want + "\". Options: " + list);
        }

        // A value worth saying: the probe gives a control with none an empty
        // object.
        bool hasValue(matjson::Value const& el) {
            if (!el.contains("value"))
                return false;
            auto const& v = el["value"];
            return !(v.isObject() && v.size() == 0) && !v.isNull();
        }

        // Scrolls the window a control scrolled out of view sits in (the page,
        // or the Saved macros list) so its middle is in view. False when
        // neither holds it.
        bool scrollIntoView(matjson::Value const& el) {
            if (!el.contains("rect") || el["rect"].size() < 4)
                return false;
            double const x = el["rect"][size_t(0)].asDouble().unwrapOr(0.0);
            double const y = el["rect"][size_t(1)].asDouble().unwrapOr(0.0);
            double const h = el["rect"][size_t(3)].asDouble().unwrapOr(0.0);
            bool const inList = el.contains("ctx") && el["ctx"].contains("macro");
            for (char const* key : inList ? std::initializer_list<char const*>{"macros", "page"}
                                          : std::initializer_list<char const*>{"page", "macros"}) {
                auto const w = ui::probe::live(key);
                if (!w || x < w->rect.x || x > w->rect.x + w->rect.z)
                    continue;
                // The rect is where the capture laid it out, at the window's
                // scroll then.
                float const target = w->scrollY + (float)(y + h * 0.5) - (w->rect.y + w->viewH * 0.5f);
                return ui::probe::scrollTo(w->name, std::clamp(target, 0.f, w->scrollMaxY));
            }
            return false;
        }

        enum class Step { Look, Scroll, Press, Settle, PickLook, PickPress, After };

        struct Job {
            uint32_t asked = 0;
            matjson::Value args;
            Step step = Step::Look;
            bool requested = false;
            bool scrolled = false;
            int settle = 0;
            ui::press::Request req;
            matjson::Value before;
            std::string pickOption;  // a dropdown's option, pressed in its list
            matjson::Value out = matjson::Value::object();
        };

        // Ask for a capture once; true when it's in.
        bool captured(Job& j) {
            if (!j.requested) {
                ui::probe::request();
                j.requested = true;
                return false;
            }
            if (!ui::probe::ready())
                return false;
            j.requested = false;
            return true;
        }

        std::optional<matjson::Value> poll(Job& j) {
            switch (j.step) {
                case Step::Look: {
                    if (!captured(j))
                        return std::nullopt;
                    auto el = elementById(j.asked);
                    if (!el)
                        throw ToolError("no control with ID " + idText(j.asked) +
                                        " is drawn now (read the IDs with gucci_ui; a page or popup has to be "
                                        "open for its controls to be there)");
                    // Scrolled away, the control isn't drawn at all (its card
                    // is skipped): scrolled into view first, then looked at again.
                    if (!(*el)["visible"].asBool().unwrapOr(true) && !j.scrolled) {
                        j.scrolled = true;
                        if (scrollIntoView(*el)) {
                            j.out["scrolled"] = true;
                            j.step = Step::Scroll;
                            j.settle = 3;
                            return std::nullopt;
                        }
                    }
                    j.before = *el;
                    auto const kind = el->contains("kind") ? (*el)["kind"].asString().unwrapOr("") : "";
                    auto const& a = j.args;
                    j.out["id"] = idText(j.asked);
                    j.out["kind"] = kind;
                    for (char const* k : {"label", "card", "row", "win", "ctx"})
                        if (el->contains(k))
                            j.out[k] = (*el)[k];
                    if (hasValue(*el))
                        j.out["before"] = (*el)["value"];
                    if (!(*el)["enabled"].asBool().unwrapOr(true)) {
                        j.out["pressed"] = false;
                        j.out["status"] = "refused_disabled";
                        j.out["detail"] = std::string("the control is disabled (grayed out)") +
                                          (gucci::legit::on() ? "; Legit only is on" : "");
                        return j.out;
                    }
                    if (!(*el)["visible"].asBool().unwrapOr(true))
                        j.out["note"] = "still out of view after a scroll; pressed anyway";

                    j.req = {};
                    j.req.id = j.asked;
                    if (a.contains("text")) {
                        bool const typable = kind == "input" || kind == "slider" ||
                                             (kind == "item" && el->contains("inputable"));
                        if (!typable)
                            throw ToolError("text is for a text box or a slider; this is a " + kind);
                        if (!a["text"].isString())
                            throw ToolError("text is a string");
                        j.req.text = true;
                        j.req.value = a["text"].asString().unwrapOr("");
                        j.req.submit = !a.contains("submit") || a["submit"].asBool().unwrapOr(true);
                    } else if (a.contains("option")) {
                        if (kind == "choice") {
                            int const i = optionIndex(*el, a["option"]);
                            auto const ids = (*el)["ids"];
                            if (i >= (int)ids.size())
                                throw ToolError("that choice has no ID for option " + std::to_string(i));
                            j.req.id = parseId(ids[(size_t)i]);
                            j.out["option"] = (*el)["options"][(size_t)i];
                        } else if (kind == "dropdown") {
                            int const i = optionIndex(*el, a["option"]);
                            j.pickOption = (*el)["options"][(size_t)i].asString().unwrapOr("");
                            j.out["option"] = j.pickOption;
                        } else {
                            throw ToolError("option is for a choice or a dropdown; this is a " + kind);
                        }
                    } else if (a.contains("on")) {
                        if (kind != "switch")
                            throw ToolError("on is for a switch; this is a " + kind);
                        bool const want = a["on"].asBool().unwrapOr(true);
                        if ((*el)["value"].asBool().unwrapOr(!want) == want) {
                            j.out["pressed"] = false;
                            j.out["status"] = "already";
                            j.out["detail"] = want ? "already on" : "already off";
                            return j.out;
                        }
                    }
                    if (kind == "choice" && !a.contains("option"))
                        throw ToolError("a choice needs option (its name or index)");
                    if (!ui::press::start(j.req))
                        throw ToolError("another press is still running");
                    j.step = Step::Press;
                    return std::nullopt;
                }

                case Step::Press:
                case Step::PickPress: {
                    auto o = ui::press::take();
                    if (!o)
                        return std::nullopt;
                    bool const pick = j.step == Step::PickPress;
                    j.out["status"] = o->status;
                    if (!o->detail.empty())
                        j.out["detail"] = o->detail;
                    j.out["pressed"] = o->ok;
                    if (!o->ok && !pick) {
                        return j.out;
                    }
                    if (!j.pickOption.empty() && !pick) {
                        // The list opens over the next frame or two.
                        j.step = Step::PickLook;
                        j.settle = 2;
                        return std::nullopt;
                    }
                    j.step = Step::Settle;
                    j.settle = 2;  // what it opened or changed, laid out
                    return std::nullopt;
                }

                case Step::PickLook: {
                    if (j.settle > 0) {
                        --j.settle;
                        return std::nullopt;
                    }
                    if (!captured(j))
                        return std::nullopt;
                    auto const id = comboItem(j.pickOption);
                    if (!id) {
                        j.out["pressed"] = false;
                        j.out["status"] = "not_drawn";
                        j.out["detail"] = "the dropdown didn't open a list with \"" + j.pickOption + "\" in it";
                        return j.out;
                    }
                    j.out["option_id"] = idText(*id);
                    ui::press::Request r;
                    r.id = *id;
                    if (!ui::press::start(r))
                        throw ToolError("another press is still running");
                    j.step = Step::PickPress;
                    return std::nullopt;
                }

                case Step::Scroll:
                    if (j.settle > 0) {
                        --j.settle;
                        return std::nullopt;
                    }
                    j.step = Step::Look;
                    return std::nullopt;

                case Step::Settle:
                    if (j.settle > 0) {
                        --j.settle;
                        return std::nullopt;
                    }
                    j.step = Step::After;
                    return std::nullopt;

                case Step::After: {
                    if (!captured(j))
                        return std::nullopt;
                    if (auto el = elementById(j.asked)) {
                        if (hasValue(*el))
                            j.out["after"] = (*el)["value"];
                        j.out["enabled_after"] = (*el)["enabled"];
                    } else {
                        j.out["after_note"] = "the control isn't drawn any more (its popup closed or the page changed)";
                    }
                    auto const pops = popupText();
                    if (!pops.empty())
                        j.out["popups"] = pops;
                    return j.out;
                }
            }
            return std::nullopt;
        }

        matjson::Value pressSchema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("id", "string",
                  "the control's ImGui ID as gucci_ui lists it (\"0x8F3A12C0\"; a choice's id or one of its ids)");
            field("option", "string",
                  "a choice or dropdown: the option to pick, by its name (or its index as a number)");
            field("on", "boolean", "a switch: the state wanted (pressed only if it isn't already)");
            field("text", "string",
                  "a text box (or a slider, typed as Ctrl+click makes it a box): the whole text, typed over what's "
                  "there");
            field("submit", "boolean", "with text: press Enter after it (default true)");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"id"};
            return schema;
        }

    } // namespace

    void registerUiPressTools(Server& server) {
        server.addTool({
            "gucci_ui_press",
            "Press one of GucciBot's menu controls by its ID (test launches only), the way a click on it "
            "does: a button, a switch (on: the state wanted), a choice's option, a dropdown's option, a menu "
            "item, a macro row's \"...\", a tab, or a text box's text (typed, then Enter). Read the IDs "
            "with gucci_ui (format json, or find); one scrolled out of view is scrolled into view first. A disabled control (grayed out, Legit only) is refused "
            "and says so. Answers {status: pressed, typed, already, refused_disabled, not_drawn, not_taken "
            "or no_focus; pressed; before; after; scrolled; popups: what opened}. It presses exactly what it's told: "
            "a control that opens a file dialog or a folder opens it, so leave those alone in a hidden game.",
            pressSchema(),
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                if (!a.contains("id"))
                    throw ToolError("id is needed (a control's ID from gucci_ui)");
                if (ui::press::busy())
                    throw ToolError("another press is still running");
                auto job = std::make_shared<Job>();
                job->asked = parseId(a["id"]);
                job->args = a;
                throw Defer{[job]() { return poll(*job); }, [] { ui::press::cancel(); }, 240};
            },
        });
    }

} // namespace gucci::mcp

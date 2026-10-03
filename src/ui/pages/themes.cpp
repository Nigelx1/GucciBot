// The Themes page: pick the theme the menu wears, and make, edit and delete
// your own. Written fresh for the 2026-10 menu.
//
// The page is in one of two states. The picker shows every theme as a tile
// (click one to use it) and lists the custom ones with Edit and Delete. The
// editor takes the page's place while a custom theme is open: every field of
// the theme, worn live by the menu through themes::preview() until Save or
// Cancel.
//
// The theme's own BIG BRRRR track comes from the system file picker. GitHub
// issue #1 was a use-after-free in file-picker code, so that part is careful
// about lifetimes; see pickTrack().

#include "ui/pages.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/themes.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace gucci::ui::pages {

    namespace {

        namespace fs = std::filesystem;
        namespace th = gucci::ui::themes;
        namespace gfile = geode::utils::file;
        using th::Theme;

        // Narrowest a theme tile gets before the grid drops a column.
        constexpr float kTileMinWidth = 200.f;

        std::string trim(std::string_view s) {
            auto const isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!s.empty() && isSpace(s.front()))
                s.remove_prefix(1);
            while (!s.empty() && isSpace(s.back()))
                s.remove_suffix(1);
            return std::string(s);
        }

        std::string lowerAscii(std::string s) {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        std::string fileName(const fs::path& p) {
            return geode::utils::string::pathToString(p.filename());
        }

        // ------------------------------------------------------------ state

        // A line under the theme list: what the last save or delete did.
        struct Notice {
            std::string text;
            Tone tone = Tone::Muted;
        };
        Notice s_notice;

        struct Editor {
            bool open = false;
            // Which opening of the editor this is. A track pick that finishes
            // after its editor closed (or another one opened) is dropped.
            unsigned session = 0;
            std::string previousName;   // the name it is saved under; empty for a new theme
            Theme draft;
            std::string extensionText;  // the extension field as typed
            th::TrackChange track;      // what Save does to the theme's own track
            std::string trackNote;      // why the last picked file wasn't taken
            std::string error;          // why the last Save failed
            int startFrom = 0;          // new themes: whose look the draft was seeded from
        };
        Editor s_editor;
        unsigned s_sessions = 0;

        // The delete dialog. The button that asks for it sits inside a card,
        // but the dialog has to be opened and drawn from the same ImGui id
        // scope, so the page opens it on its own level.
        std::string s_deleteName;
        bool s_askDelete = false;

        // ------------------------------------------------------------ the track picker
        //
        // Rules, after issue #1:
        //  - file::pick returns an arc future, so it is awaited only from an
        //    arc coroutine (pickTask) running on Geode's arc runtime, never
        //    from a geode::Task (that mix was the crash family behind #1/#3;
        //    see the note in core/engine_core.cpp).
        //  - Nothing ever cancels a pending pick. On Windows the dialog runs
        //    on a thread that points into the pick's own coroutine frame, so
        //    aborting it mid-dialog would free that frame under the thread.
        //    The task's handle is dropped as soon as it is spawned, which in
        //    arc detaches it (it runs to the end and cleans itself up), and a
        //    second pick never starts while one is pending.
        //  - The result reaches the page by value: the task queues a main
        //    thread call that captures only the result and the editor session
        //    number and drops them into s_picked. The page takes it from there
        //    on its next frame, and only if the editor that asked is still the
        //    one open. Nothing captures page locals.
        //  - Not geode::async::spawn / TaskHolder: this MSVC hits an internal
        //    compiler error (C1001, arc Pollable.hpp:174) instantiating them.

        struct Picked {
            bool ready = false;
            unsigned session = 0;
            std::optional<fs::path> path;  // nullopt: the dialog was cancelled
            std::string error;
        };
        Picked s_picked;
        bool s_pickPending = false;  // main thread only: set on spawn, cleared when the result lands

        bool picking() {
            return s_pickPending;
        }

        arc::Future<int> pickTask(unsigned session) {
            gfile::FilePickOptions options;
            options.filters.push_back({"MP3 audio", {"*.mp3"}});
            gfile::PickResult result = co_await gfile::pick(gfile::PickMode::OpenFile, std::move(options));
            Picked got;
            got.ready = true;
            got.session = session;
            if (result.isOk())
                got.path = std::move(result).unwrap();
            else
                got.error = std::move(result).unwrapErr();
            geode::queueInMainThread([got = std::move(got)]() mutable {
                s_picked = std::move(got);
                s_pickPending = false;
            });
            co_return 0;
        }

        void pickTrack(unsigned session) {
            if (s_pickPending)
                return;
            s_pickPending = true;
            // The handle is a temporary: dropping it detaches the task.
            (void)geode::async::runtime().spawn(pickTask(session));
        }

        // Hands a finished pick to the editor that asked for it.
        void takePicked() {
            if (!s_picked.ready)
                return;
            Picked got = std::move(s_picked);
            s_picked = Picked{};
            Editor& ed = s_editor;
            if (!ed.open || got.session != ed.session)
                return;
            if (!got.error.empty()) {
                ed.trackNote = "The file picker failed: " + got.error;
                return;
            }
            if (!got.path)
                return;
            if (lowerAscii(geode::utils::string::pathToString(got.path->extension())) != ".mp3") {
                ed.trackNote = "BIG BRRRR plays .mp3 files. Pick one of those.";
                return;
            }
            std::error_code ec;
            if (!fs::is_regular_file(*got.path, ec)) {
                ed.trackNote = "That file couldn't be found.";
                return;
            }
            ed.track.copyFrom = *got.path;
            ed.track.remove = false;
            ed.trackNote.clear();
        }

        // ------------------------------------------------------------ editor helpers

        // A new theme's name and extension: the first "Custom", "Custom 2", ...
        // and ".custom", ".custom2", ... nobody has taken.
        void freshIdentity(Theme& t) {
            for (int n = 1; n < 1000; ++n) {
                std::string const name = n == 1 ? std::string("Custom") : fmt::format("Custom {}", n);
                if (th::nameProblem(name).empty()) {
                    t.name = name;
                    break;
                }
            }
            for (int n = 1; n < 1000; ++n) {
                std::string const ext = n == 1 ? std::string(".custom") : fmt::format(".custom{}", n);
                if (th::extensionProblem(ext).empty()) {
                    t.extension = ext;
                    break;
                }
            }
            t.title = t.name;
        }

        // "Start from": another theme's look and words, keeping the draft's own
        // name, extension and BIG BRRRR settings.
        void copyLookAndWords(Theme& to, const Theme& from) {
            to.accent = from.accent;
            to.background = from.background;
            to.card = from.card;
            to.text = from.text;
            to.textMuted = from.textMuted;
            to.radius = from.radius;
            to.opacity = from.opacity;
            to.pulse = from.pulse;
            to.snow = from.snow;
            to.subtitle = from.subtitle;
            to.brand = from.brand;
            to.badge = from.badge;
            to.quotes = from.quotes;
        }

        void openEditor(const Theme* existing) {
            s_editor = Editor{};
            s_editor.open = true;
            s_editor.session = ++s_sessions;
            if (existing) {
                s_editor.draft = *existing;
                s_editor.previousName = existing->name;
            } else {
                s_editor.draft = th::starterCustom();
                freshIdentity(s_editor.draft);
            }
            s_editor.extensionText = s_editor.draft.extension;
        }

        void closeEditor() {
            s_editor = Editor{};
            th::endPreview();
        }

        void problem(const std::string& text) {
            if (text.empty())
                return;
            UseFont f(Font::Small);
            kit::Paragraph(text.c_str(), Tone::Bad);
        }

        void textRow(const char* label, const char* hint, std::string* value, const char* placeholder = nullptr) {
            kit::RowBegin(label, hint);
            kit::InputText("text", value, placeholder);
            kit::RowEnd();
        }

        enum class EditAction { None, Save, SaveAndUse, Cancel };

        // Save (and, for a theme not in use, Save and use) and Cancel, with
        // the reason Save is greyed out when it is.
        EditAction actionBar(bool offerUse, const std::string& blocked) {
            EditAction act = EditAction::None;
            ImGui::BeginDisabled(!blocked.empty());
            if (offerUse) {
                if (kit::Button("Save and use", Tone::Accent))
                    act = EditAction::SaveAndUse;
                ImGui::SameLine();
                if (kit::Button("Save"))
                    act = EditAction::Save;
            } else if (kit::Button("Save", Tone::Accent)) {
                act = EditAction::Save;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (kit::Button("Cancel", Tone::Muted))
                act = EditAction::Cancel;
            if (!blocked.empty()) {
                kit::Gap(0.25f);
                kit::Note(blocked.c_str(), Tone::Warn);
            }
            return act;
        }

        // ------------------------------------------------------------ editor

        void drawEditor() {
            Editor& ed = s_editor;
            Theme& d = ed.draft;
            bool const isNew = ed.previousName.empty();
            bool const inUse = !isNew && th::activeId() == kCustomThemeId && th::activeCustomName() == ed.previousName;

            d.extension = th::sanitizeExtension(ed.extensionText);
            d.title = trim(d.name);
            std::string const nameWhy = th::nameProblem(d.name, ed.previousName);
            std::string const extWhy = th::extensionProblem(d.extension, ed.previousName);
            // Save isn't held back by a pending pick: a result that lands after
            // this editor closed is dropped by its session check.
            std::string blocked;
            if (!nameWhy.empty() || !extWhy.empty())
                blocked = "Fix the name or extension below to save.";

            EditAction act = EditAction::None;

            {
                std::string const heading = isNew ? std::string("New custom theme") : "Editing " + ed.previousName;
                kit::BeginCard(heading.c_str(),
                               "The menu wears your changes as you make them. Nothing is saved until you press Save.");
                EditAction const top = actionBar(!inUse, blocked);
                if (top != EditAction::None)
                    act = top;
                if (!ed.error.empty()) {
                    kit::Gap(0.25f);
                    kit::Note(ed.error.c_str(), Tone::Bad);
                }
                kit::EndCard();
            }

            kit::BeginCard("Name and file", nullptr);
            if (isNew) {
                auto const& builtins = th::builtins();
                auto const& customs = th::customs();
                std::vector<std::string> names;
                names.reserve(1 + builtins.size() + customs.size());
                names.emplace_back("Starter (GucciBot's look)");
                for (auto const& t : builtins)
                    names.push_back(t.name);
                for (auto const& t : customs)
                    names.push_back(t.name + " (yours)");
                kit::RowBegin("Start from", "Copies that theme's colours and words into this one");
                int choice = ed.startFrom;
                if (kit::Dropdown("start", &choice, names) && choice != ed.startFrom) {
                    size_t const index = static_cast<size_t>(choice);
                    if (index == 0)
                        copyLookAndWords(d, th::starterCustom());
                    else if (index - 1 < builtins.size())
                        copyLookAndWords(d, builtins[index - 1]);
                    else if (index - 1 - builtins.size() < customs.size())
                        copyLookAndWords(d, customs[index - 1 - builtins.size()]);
                    ed.startFrom = choice;
                }
                kit::RowEnd();
            }
            kit::RowBegin("Name", "What the theme list shows");
            kit::InputText("name", &d.name, "My theme");
            if (ImGui::IsItemDeactivated())
                d.name = trim(d.name);
            problem(nameWhy);
            kit::RowEnd();
            kit::RowBegin("Macro extension", "Macros recorded while this theme is in use save with it");
            kit::InputText("extension", &ed.extensionText, ".mytheme");
            if (ImGui::IsItemDeactivated()) {
                std::string const clean = th::sanitizeExtension(ed.extensionText);
                if (!clean.empty())
                    ed.extensionText = clean;
            }
            if (!extWhy.empty())
                problem(extWhy);
            else if (ed.extensionText != d.extension)
                kit::Hint(("Saves as " + d.extension).c_str());
            kit::RowEnd();
            kit::EndCard();

            kit::BeginCard("Look", nullptr);
            kit::Section("Colours");
            kit::ColorRow("Accent", "Buttons, highlights and headings", &d.accent, false);
            kit::ColorRow("Background", "The window behind everything", &d.background, false);
            kit::ColorRow("Cards", "The panels the controls sit on", &d.card, false);
            kit::ColorRow("Text", nullptr, &d.text, false);
            kit::ColorRow("Muted text", "Hints and secondary text", &d.textMuted, false);
            kit::Section("Shape");
            kit::SliderRow("Corner radius", nullptr, &d.radius, 0.f, 14.f, "%.0f px");
            kit::SliderRow("Opacity", "How solid the window is", &d.opacity, 0.3f, 1.f, "%.2f");
            kit::SwitchRow("Pulse", "The accent colour breathes", &d.pulse);
            kit::EndCard();

            kit::BeginCard("Words", nullptr);
            textRow("Subtitle", "Under the menu's heading", &d.subtitle);
            textRow("Brand", "The short tag on the status line", &d.brand, "Brrr.");
            textRow("Badge", "The Credits page badge", &d.badge);
            const char* const slotNames[th::kSlotCount] = {"Replay quote", "Tools quote", "Credits quote"};
            for (int s = 0; s < th::kSlotCount; ++s) {
                ImGui::PushID(s);
                th::Quote& q = d.quotes[static_cast<size_t>(s)];
                kit::Section(slotNames[s]);
                textRow("Quote", "With its quotation marks", &q.text);
                textRow("By", "Who said it; may be empty", &q.by, "-- Gucci Mane, probably");
                kit::Quote(q.text.c_str(), q.by.c_str());
                ImGui::PopID();
            }
            kit::EndCard();

            kit::BeginCard("BIG BRRRR", "The drop this theme plays. Without a track of its own it plays GucciBot's.");
            {
                std::string status;
                if (!ed.track.copyFrom.empty())
                    status = fileName(ed.track.copyFrom) + ", copied in when you save";
                else if (ed.track.remove)
                    status = "Removed when you save; GucciBot's drop plays instead";
                else if (d.hasTrack)
                    status = "It has its own .mp3";
                else
                    status = "None of its own: GucciBot's drop plays";
                bool const busy = picking();
                bool const hasOne = !ed.track.copyFrom.empty() || (d.hasTrack && !ed.track.remove);
                kit::RowBegin("Track", status.c_str());
                ImGui::BeginDisabled(busy);
                const char* const pickLabel = busy     ? "Waiting for the file picker...##pick"
                                              : hasOne ? "Choose another .mp3##pick"
                                                       : "Choose an .mp3##pick";
                if (kit::Button(pickLabel))
                    pickTrack(ed.session);
                ImGui::EndDisabled();
                if (!ed.track.copyFrom.empty()) {
                    ImGui::SameLine();
                    if (kit::Button("Don't use it", Tone::Muted))
                        ed.track.copyFrom.clear();
                } else if (ed.track.remove) {
                    ImGui::SameLine();
                    if (kit::Button("Keep it"))
                        ed.track.remove = false;
                } else if (d.hasTrack) {
                    ImGui::SameLine();
                    if (kit::Button("Remove", Tone::Muted))
                        ed.track.remove = true;
                }
                problem(ed.trackNote);
                kit::RowEnd();

                bool const ownTrack = !ed.track.copyFrom.empty() || (d.hasTrack && !ed.track.remove);
                ImGui::BeginDisabled(!ownTrack);
                kit::RowBegin("BPM", ownTrack ? "The track's tempo" : "Used with a track of its own");
                if (kit::InputDouble("bpm", &d.bpm, 1.0, "%.1f"))
                    d.bpm = std::clamp(d.bpm, 1.0, 999.0);
                kit::RowEnd();
                kit::RowBegin("Drop starts at", "Seconds into the track where playback starts");
                if (kit::InputDouble("drop", &d.dropOffsetSec, 0.1, "%.3f"))
                    d.dropOffsetSec = std::clamp(d.dropOffsetSec, 0.0, 3600.0);
                kit::RowEnd();
                ImGui::EndDisabled();
            }
            kit::EndCard();

            ImGui::PushID("bottom");
            kit::BeginCard(nullptr);
            EditAction const bottom = actionBar(!inUse, blocked);
            if (bottom != EditAction::None)
                act = bottom;
            kit::EndCard();
            ImGui::PopID();

            // Act only now, with everything drawn: Save reloads the theme list
            // and closing resets the state the code above was drawing from.
            if (act == EditAction::Cancel) {
                closeEditor();
                return;
            }
            if (act == EditAction::Save || act == EditAction::SaveAndUse) {
                d.name = trim(d.name);
                d.title = d.name;
                d.extension = th::sanitizeExtension(ed.extensionText);
                std::string const why = th::saveCustom(d, ed.previousName, ed.track);
                if (!why.empty()) {
                    ed.error = why;
                } else {
                    std::string const saved = d.name;
                    bool const use = act == EditAction::SaveAndUse;
                    if (use)
                        th::setActiveCustom(saved);
                    // Also when only saved: renaming the theme in use changes
                    // the name the choice is persisted under.
                    gucci::ui::saveSettings();
                    s_notice = {use || inUse ? fmt::format("Saved {}. It's the theme in use.", saved)
                                             : fmt::format("Saved {}. Click it to use it.", saved),
                                Tone::Good};
                    closeEditor();
                    return;
                }
            }

            th::preview(d);
        }

        // ------------------------------------------------------------ picker

        // A theme's colours in miniature: its window, a card on it, an accent
        // dot and two lines of text.
        void swatch(ImDrawList* dl, ImVec2 a, ImVec2 b, const Theme& t) {
            float const round = std::min(std::clamp(t.radius, 0.f, 14.f) * 0.6f, (b.y - a.y) * 0.5f);
            dl->AddRectFilled(a, b, u32(withAlpha(t.background, 1.f)), round);
            ImVec2 const ca(a.x + 4.f, a.y + 4.f);
            ImVec2 const cb(b.x - 4.f, b.y - 4.f);
            if (cb.x - ca.x >= 12.f && cb.y - ca.y >= 8.f) {
                dl->AddRectFilled(ca, cb, u32(withAlpha(t.card, 1.f)), std::max(0.f, round - 2.f));
                float const midY = std::round((ca.y + cb.y) * 0.5f);
                float const dot = std::min(5.f, (cb.y - ca.y) * 0.5f - 1.f);
                ImVec2 const centre(ca.x + 3.f + dot, midY);
                dl->AddCircleFilled(centre, dot, u32(withAlpha(t.accent, 1.f)));
                float const lx = centre.x + dot + 4.f;
                float const rx = cb.x - 4.f;
                if (rx - lx > 4.f) {
                    dl->AddRectFilled(ImVec2(lx, midY - 3.f), ImVec2(rx, midY - 1.f), u32(withAlpha(t.text, 1.f)), 1.f);
                    dl->AddRectFilled(ImVec2(lx, midY + 1.f), ImVec2(lx + (rx - lx) * 0.65f, midY + 3.f),
                                      u32(withAlpha(t.textMuted, 1.f)), 1.f);
                }
            }
            dl->AddRect(a, b, u32(look().pal.line), round);
        }

        // One theme as a clickable tile: its swatch, its name and the
        // extension its macros save with. The theme in use is outlined in the
        // accent colour and tagged. True when clicked.
        bool tile(const Theme& t, bool inUse, float width) {
            auto const& pal = look().pal;
            float const pad = 7.f;
            float const nameHeight = ImGui::GetTextLineHeight();
            float extHeight = nameHeight;
            {
                UseFont f(Font::Small);
                extHeight = ImGui::GetTextLineHeight();
            }
            float const height = std::round(nameHeight + extHeight + 2.f + pad * 2.f);
            ImVec2 const a = ImGui::GetCursorScreenPos();
            ImVec2 const b(a.x + width, a.y + height);
            bool const clicked = ImGui::InvisibleButton("tile", ImVec2(std::max(1.f, width), height));
            bool const hovered = ImGui::IsItemHovered();
            bool const held = ImGui::IsItemActive();
            if (hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            auto* dl = ImGui::GetWindowDrawList();
            ImVec4 fill = inUse ? mix(pal.raised, pal.accent, 0.16f) : pal.raised;
            if (hovered)
                fill = mix(fill, pal.ink, held ? 0.10f : 0.05f);
            dl->AddRectFilled(a, b, u32(fill), pal.radius);
            dl->AddRect(a, b, u32(inUse ? pal.accent : pal.line), pal.radius, 0, inUse ? 1.5f : 1.f);

            ImVec2 const sa(a.x + pad, a.y + pad);
            ImVec2 const sb(sa.x + 54.f, b.y - pad);
            swatch(dl, sa, sb, t);

            float const textX = sb.x + 9.f;
            float const extY = a.y + pad + nameHeight + 2.f;
            float extRight = b.x - pad;
            dl->PushClipRect(ImVec2(textX, a.y), ImVec2(b.x - pad, b.y), true);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, a.y + pad), u32(pal.ink), t.name.c_str());
            dl->PopClipRect();
            {
                UseFont f(Font::Small);
                if (inUse) {
                    const char* const tag = "in use";
                    float const tagWidth = ImGui::CalcTextSize(tag).x;
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(b.x - pad - tagWidth, extY), u32(pal.accent),
                                tag);
                    extRight -= tagWidth + 8.f;
                }
                if (extRight > textX) {
                    dl->PushClipRect(ImVec2(textX, a.y), ImVec2(extRight, b.y), true);
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, extY), u32(pal.muted),
                                t.extension.c_str());
                    dl->PopClipRect();
                }
            }
            return clicked;
        }

        void drawPicker() {
            auto const& builtins = th::builtins();
            auto const& customs = th::customs();
            int const activeId = th::activeId();
            std::string const activeCustom = th::activeCustomName();

            // What was clicked, acted on once both lists are drawn (Reload
            // replaces the custom list, and nothing may do that mid-loop).
            int useBuiltin = -1;
            std::string useCustom;
            std::string editCustom;
            bool startNew = false;

            kit::BeginCard("Built-in themes", "Click one to use it. A theme also sets the file extension your macros save with.");
            {
                float const spacing = ImGui::GetStyle().ItemSpacing.x;
                float const avail = kit::Avail();
                int const columns = std::max(1, static_cast<int>((avail + spacing) / (kTileMinWidth + spacing)));
                float const width = std::max(1.f, std::floor((avail - spacing * static_cast<float>(columns - 1)) /
                                                             static_cast<float>(columns)));
                for (size_t i = 0; i < builtins.size(); ++i) {
                    Theme const& t = builtins[i];
                    if (static_cast<int>(i) % columns != 0)
                        ImGui::SameLine();
                    ImGui::PushID(t.id);
                    if (tile(t, activeId == t.id, width))
                        useBuiltin = t.id;
                    ImGui::PopID();
                }
            }
            kit::EndCard();

            kit::BeginCard("Your themes", "Kept in the customthemes folder in GucciBot's save folder.");
            if (customs.empty())
                kit::Hint("None yet.");
            {
                ImGuiStyle const& style = ImGui::GetStyle();
                float const editWidth = ImGui::CalcTextSize("Edit").x + style.FramePadding.x * 2.f;
                float const deleteWidth = ImGui::CalcTextSize("Delete").x + style.FramePadding.x * 2.f;
                for (Theme const& t : customs) {
                    ImGui::PushID(t.name.c_str());
                    float const width =
                        std::max(120.f, kit::Avail() - editWidth - deleteWidth - style.ItemSpacing.x * 2.f);
                    float const top = ImGui::GetCursorPosY();
                    if (tile(t, activeId == kCustomThemeId && t.name == activeCustom, width))
                        useCustom = t.name;
                    float const buttonY = top + std::max(0.f, (ImGui::GetItemRectSize().y - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(buttonY);
                    if (kit::Button("Edit"))
                        editCustom = t.name;
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(buttonY);
                    if (kit::Button("Delete", Tone::Muted)) {
                        s_deleteName = t.name;
                        s_askDelete = true;
                    }
                    ImGui::PopID();
                }
            }
            kit::Gap(0.3f);
            if (kit::Button("New custom theme", Tone::Accent))
                startNew = true;
            ImGui::SameLine();
            if (kit::Button("Open folder")) {
                std::error_code ec;
                fs::create_directories(th::customThemesDir(), ec);
                (void)gfile::openFolder(th::customThemesDir());
            }
            ImGui::SameLine();
            if (kit::Button("Reload"))
                th::loadCustoms();
            if (!s_notice.text.empty()) {
                kit::Gap(0.25f);
                kit::Note(s_notice.text.c_str(), s_notice.tone);
            }
            kit::EndCard();

            if (useBuiltin >= 0) {
                s_notice = {};
                if (activeId != useBuiltin) {
                    th::setActive(useBuiltin);
                    gucci::ui::saveSettings();
                }
            } else if (!useCustom.empty()) {
                s_notice = {};
                if (activeId != kCustomThemeId || activeCustom != useCustom) {
                    th::setActiveCustom(useCustom);
                    gucci::ui::saveSettings();
                }
            }
            if (!editCustom.empty()) {
                if (const Theme* t = th::findCustom(editCustom)) {
                    s_notice = {};
                    openEditor(t);
                }
            } else if (startNew) {
                s_notice = {};
                openEditor(nullptr);
            }
        }

        void drawDeleteDialog() {
            constexpr const char* kId = "##delete-theme";
            if (s_askDelete) {
                kit::OpenConfirm(kId);
                s_askDelete = false;
            }
            std::string message;
            if (const Theme* t = th::findCustom(s_deleteName)) {
                bool const inUse = th::activeId() == kCustomThemeId && th::activeCustomName() == t->name;
                message = fmt::format("{} will be deleted{}. This can't be undone.{}", t->name,
                                      t->hasTrack ? ", along with its own BIG BRRRR track" : "",
                                      inUse ? " It's the theme in use, so the menu goes back to GucciBot." : "");
            } else {
                message = "That theme is already gone.";
            }
            int const answer = kit::Confirm(kId, "Delete this theme?", message.c_str(), "Delete", Tone::Bad);
            if (answer == 1) {
                std::string const name = s_deleteName;
                s_deleteName.clear();
                std::string const why = th::deleteCustom(name);
                // deleteCustom may have made GucciBot the theme in use.
                gucci::ui::saveSettings();
                s_notice = why.empty() ? Notice{fmt::format("Deleted {}.", name), Tone::Good} : Notice{why, Tone::Bad};
            } else if (answer == -1) {
                s_deleteName.clear();
            }
        }

    } // namespace

    void themes() {
        takePicked();
        if (s_editor.open)
            drawEditor();
        else
            drawPicker();
        drawDeleteDialog();
    }

} // namespace gucci::ui::pages

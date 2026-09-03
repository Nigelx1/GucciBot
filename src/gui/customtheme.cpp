#include "core/GucciBot.hpp"
#include "gui/gui.hpp"

#include <Geode/Geode.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace geode::prelude;

namespace gucci {

    namespace fs = std::filesystem;

    // This list is duplicated (not shared) across several files. Confirmed
    // complete 2026-09-03 while adding Waka/Youngsta/Knockerz (this
    // comment previously named five locations and MISSED a sixth --
    // engine_core.cpp's reloadMacroList() has its own separate isBuiltin
    // check, not just the macro-set population right above it):
    //   - gui.cpp: currentThemeExtension(), the macro-tag lookup switch
    //     (two copies -- one plain, one with colors), kThemePresets[], the
    //     preset-index-to-BotTheme if/else chain, and the botName/
    //     subtitle/brandTag/badge/quote (x3) chains keyed off activeTheme.
    //   - gui.hpp: the BotTheme enum itself.
    //   - brr_format.cpp: getThemeExtension() and its TWO disk-scan lists
    //     (replayNameTaken(), loadFromDisk()).
    //   - engine_core.cpp: reloadMacroList()'s macro-set population AND
    //     its own separate isBuiltin bool check.
    //   - GucciBot.hpp: one std::unordered_set<std::string> per theme.
    //   - allKnownMacroExtensions() below.
    //   - about.md's Themes table.
    // Adding a new built-in theme needs EVERY one of these updated; missing
    // one is a confirmed repeat failure mode on this project -- re-grep for
    // every existing extension string (not just this list) rather than
    // trusting this comment to still be complete, the way this one wasn't.
    static bool isBuiltinExtension(const std::string& ext) {
        static const char* kBuiltin[] = {"brrr",
                                         "toosii",
                                         "ja",
                                         "giddey",
                                         "bam",
                                         "sexyy",
                                         "juice",
                                         "butler",
                                         "saweetie",
                                         "maybach",
                                         "romo",
                                         "grizzley",
                                         "redkingdom",
                                         "lemonade",
                                         "icebrrr",
                                         "waka",
                                         "youngsta",
                                         "knockerz"};
        for (auto* e : kBuiltin)
            if (ext == e)
                return true;
        return false;
    }

    static ImVec4 jsonToColor(const matjson::Value& v, ImVec4 fallback) {
        if (!v.isArray())
            return fallback;
        auto get = [&](size_t i, float def) -> float {
            if (i >= v.size())
                return def;
            auto r = v[i].as<double>();
            return r.isOk() ? (float)r.unwrap() : def;
        };
        return ImVec4(
            get(0, fallback.x), get(1, fallback.y), get(2, fallback.z), get(3, fallback.w));
    }
    static matjson::Value colorToJson(ImVec4 c) {
        return matjson::Value(std::vector<matjson::Value>{matjson::Value((double)c.x),
                                                          matjson::Value((double)c.y),
                                                          matjson::Value((double)c.z),
                                                          matjson::Value((double)c.w)});
    }
    static std::string jsonStr(const matjson::Value& v, std::string_view key, std::string def) {
        auto r = v[key].as<std::string>();
        return r.isOk() ? r.unwrap() : def;
    }
    static double jsonNum(const matjson::Value& v, std::string_view key, double def) {
        auto r = v[key].as<double>();
        return r.isOk() ? r.unwrap() : def;
    }
    static bool jsonBool(const matjson::Value& v, std::string_view key, bool def) {
        auto r = v[key].as<bool>();
        return r.isOk() ? r.unwrap() : def;
    }

    matjson::Value CustomTheme::toJson() const {
        return matjson::makeObject({
            {"name", name},
            {"extension", extension},
            {"accent", colorToJson(accent)},
            {"bg", colorToJson(bg)},
            {"card", colorToJson(card)},
            {"textPrimary", colorToJson(textPrimary)},
            {"textSecondary", colorToJson(textSecondary)},
            {"cornerRadius", (double)cornerRadius},
            {"bgOpacity", (double)bgOpacity},
            {"subtitle", subtitle},
            {"brandTag", brandTag},
            {"quoteReplayText", quoteReplay.text},
            {"quoteReplayAttr", quoteReplay.attribution},
            {"quoteToolsText", quoteTools.text},
            {"quoteToolsAttr", quoteTools.attribution},
            {"quoteCreditsText", quoteCredits.text},
            {"quoteCreditsAttr", quoteCredits.attribution},
            {"creditsBadge", creditsBadge},
            {"bpm", bpm},
            {"dropOffsetSec", dropOffsetSec},
            {"hasAudio", hasAudio},
        });
    }

    CustomTheme CustomTheme::fromJson(const matjson::Value& v) {
        CustomTheme t;
        t.name = jsonStr(v, "name", "Custom");
        t.extension = jsonStr(v, "extension", "custom");
        t.accent = jsonToColor(v["accent"], t.accent);
        t.bg = jsonToColor(v["bg"], t.bg);
        t.card = jsonToColor(v["card"], t.card);
        t.textPrimary = jsonToColor(v["textPrimary"], t.textPrimary);
        t.textSecondary = jsonToColor(v["textSecondary"], t.textSecondary);
        t.cornerRadius = (float)jsonNum(v, "cornerRadius", t.cornerRadius);
        t.bgOpacity = (float)jsonNum(v, "bgOpacity", t.bgOpacity);
        t.subtitle = jsonStr(v, "subtitle", t.subtitle);
        t.brandTag = jsonStr(v, "brandTag", t.brandTag);
        t.quoteReplay.text = jsonStr(v, "quoteReplayText", t.quoteReplay.text);
        t.quoteReplay.attribution = jsonStr(v, "quoteReplayAttr", t.quoteReplay.attribution);
        t.quoteTools.text = jsonStr(v, "quoteToolsText", t.quoteTools.text);
        t.quoteTools.attribution = jsonStr(v, "quoteToolsAttr", t.quoteTools.attribution);
        t.quoteCredits.text = jsonStr(v, "quoteCreditsText", t.quoteCredits.text);
        t.quoteCredits.attribution = jsonStr(v, "quoteCreditsAttr", t.quoteCredits.attribution);
        t.creditsBadge = jsonStr(v, "creditsBadge", t.creditsBadge);
        t.bpm = jsonNum(v, "bpm", t.bpm);
        t.dropOffsetSec = jsonNum(v, "dropOffsetSec", t.dropOffsetSec);
        t.hasAudio = jsonBool(v, "hasAudio", t.hasAudio);
        return t;
    }

    std::vector<std::string> allKnownMacroExtensions() {
        std::vector<std::string> exts = {".brrr",
                                         ".toosii",
                                         ".ja",
                                         ".giddey",
                                         ".bam",
                                         ".sexyy",
                                         ".juice",
                                         ".butler",
                                         ".saweetie",
                                         ".maybach",
                                         ".romo",
                                         ".grizzley",
                                         ".redkingdom",
                                         ".lemonade",
                                         ".icebrrr",
                                         ".waka",
                                         ".youngsta",
                                         ".knockerz"};
        if (auto* ui = MenuInterface::get()) {
            for (auto& t : ui->customThemes)
                exts.push_back("." + t.extension);
        }
        return exts;
    }

    CustomTheme* MenuInterface::getActiveCustomTheme() {
        if (activeTheme != THEME_CUSTOM)
            return nullptr;
        for (auto& t : customThemes)
            if (t.name == activeCustomThemeName)
                return &t;
        return nullptr;
    }

    std::string MenuInterface::deriveCustomThemeExtension(const std::string& name) const {
        std::string base;
        for (char c : name) {
            if (std::isalnum((unsigned char)c))
                base += (char)std::tolower((unsigned char)c);
        }
        if (base.empty())
            base = "custom";
        if (base.size() > 24)
            base = base.substr(0, 24);

        std::string candidate = base;
        int suffix = 1;
        auto collides = [&](const std::string& ext) {
            if (isBuiltinExtension(ext))
                return true;
            for (auto& t : customThemes)
                if (t.name != name && t.extension == ext)
                    return true;
            return false;
        };
        while (collides(candidate)) {
            candidate = base + std::to_string(++suffix);
        }
        return candidate;
    }

    std::string MenuInterface::sanitizeCustomExtension(const std::string& raw,
                                                       const std::string& excludeName,
                                                       bool* ok) const {
        std::string clean;
        for (char c : raw) {
            if (std::isalnum((unsigned char)c))
                clean += (char)std::tolower((unsigned char)c);
        }
        if (clean.size() > 24)
            clean = clean.substr(0, 24);
        if (clean.empty()) {
            *ok = false;
            return clean;
        }
        if (isBuiltinExtension(clean)) {
            *ok = false;
            return clean;
        }
        for (auto& t : customThemes) {
            if (t.name != excludeName && t.extension == clean) {
                *ok = false;
                return clean;
            }
        }
        *ok = true;
        return clean;
    }

    void MenuInterface::loadCustomThemes() {
        customThemes.clear();
        std::error_code ec;
        auto dir = getCustomThemesDir();
        if (!fs::exists(dir, ec))
            return;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            std::ifstream fin(entry.path());
            std::stringstream ss;
            ss << fin.rdbuf();
            auto parsed = matjson::parse(ss.str());
            if (!parsed.isOk()) {
                log::warn("[GucciBot] Custom theme file failed to parse, skipping: {}",
                          entry.path().string());
                continue;
            }
            customThemes.push_back(CustomTheme::fromJson(parsed.unwrap()));
        }
        std::sort(customThemes.begin(), customThemes.end(), [](auto& a, auto& b) {
            return a.name < b.name;
        });
    }

    void MenuInterface::saveCustomTheme(CustomTheme& t) {
        if (t.extension.empty())
            t.extension = deriveCustomThemeExtension(t.name);
        auto it = std::find_if(customThemes.begin(), customThemes.end(), [&](auto& x) {
            return x.name == t.name;
        });
        if (it != customThemes.end())
            *it = t;
        else
            customThemes.push_back(t);

        auto dir = getCustomThemesDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f(dir / (t.extension + ".json"));
        f << t.toJson().dump();
        if (!f.good()) {
            log::warn("[GucciBot] Failed to write custom theme file: {}",
                      (dir / (t.extension + ".json")).string());
            Notification::create(
                "Saved, but couldn't write the theme file to disk -- it may not survive a restart.",
                NotificationIcon::Warning)
                ->show();
        }
    }

    void MenuInterface::openCustomThemeEditor(const CustomTheme* existing) {
        customThemeEditorOpen = true;
        if (existing) {
            customThemeEditIsNew = false;
            customThemeEditOriginalName = existing->name;
            customThemeEditOriginalExtension = existing->extension;
            customThemeEditBuffer = *existing;
        } else {
            customThemeEditIsNew = true;
            customThemeEditOriginalName.clear();
            customThemeEditOriginalExtension.clear();
            customThemeEditBuffer = CustomTheme{};
            customThemeEditBuffer.name = "My Theme";
        }
        auto copyBuf = [](char* dst, size_t n, const std::string& s) {
            snprintf(dst, n, "%s", s.c_str());
        };
        copyBuf(cteName, sizeof(cteName), customThemeEditBuffer.name);
        copyBuf(cteExtension, sizeof(cteExtension), existing ? existing->extension : std::string());
        copyBuf(cteSubtitle, sizeof(cteSubtitle), customThemeEditBuffer.subtitle);
        copyBuf(cteBrandTag, sizeof(cteBrandTag), customThemeEditBuffer.brandTag);
        copyBuf(
            cteQuoteReplayText, sizeof(cteQuoteReplayText), customThemeEditBuffer.quoteReplay.text);
        copyBuf(cteQuoteReplayAttr,
                sizeof(cteQuoteReplayAttr),
                customThemeEditBuffer.quoteReplay.attribution);
        copyBuf(
            cteQuoteToolsText, sizeof(cteQuoteToolsText), customThemeEditBuffer.quoteTools.text);
        copyBuf(cteQuoteToolsAttr,
                sizeof(cteQuoteToolsAttr),
                customThemeEditBuffer.quoteTools.attribution);
        copyBuf(cteQuoteCreditsText,
                sizeof(cteQuoteCreditsText),
                customThemeEditBuffer.quoteCredits.text);
        copyBuf(cteQuoteCreditsAttr,
                sizeof(cteQuoteCreditsAttr),
                customThemeEditBuffer.quoteCredits.attribution);
        copyBuf(cteCreditsBadge, sizeof(cteCreditsBadge), customThemeEditBuffer.creditsBadge);

        std::error_code ec;
        auto stagingPath = getCustomThemesDir() / "_editing_brrr.mp3";
        fs::remove(stagingPath, ec);
        if (existing && existing->hasAudio) {
            fs::create_directories(getCustomThemesDir(), ec);
            fs::copy_file(
                getCustomThemesDir() / (existing->extension + "_brrr.mp3"), stagingPath, ec);
        }
    }

    static geode::Task<bool> importCustomThemeAudioTask() {
        auto pickResult = co_await geode::utils::file::pick(
            geode::utils::file::PickMode::OpenFile,
            geode::utils::file::FilePickOptions{std::nullopt, {{"Audio Files", {"mp3"}}}});
        if (pickResult.isErr())
            co_return false;
        auto pathOpt = pickResult.unwrap();
        if (!pathOpt.has_value())
            co_return false;

        auto* ui = MenuInterface::get();
        auto dir = ui->getCustomThemesDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto dest = dir / "_editing_brrr.mp3";
        fs::copy_file(*pathOpt, dest, fs::copy_options::overwrite_existing, ec);
        co_return !ec;
    }
    // Must stay a stored static, never an unstored temporary -- Task<T>'s own
    // coroutine promise only holds a weak_ptr to its Handle, so this static is
    // the only thing keeping an in-flight file-picker Task alive; an unstored
    // temporary caused a real crash elsewhere in this codebase (use-after-free
    // while the OS picker's background thread was still running). Poll
    // isFinished() to observe completion -- Task::listen()'s callback is a
    // no-op in this Geode version.
    static geode::Task<bool> s_customThemeAudioTask;
    static void importCustomThemeAudio() {
        s_customThemeAudioTask = importCustomThemeAudioTask();
    }
    void pollCustomThemeAudioImportTask() {
        if (s_customThemeAudioTask.isFinished()) {
            auto* ok = s_customThemeAudioTask.getFinishedValue();
            auto* ui = MenuInterface::get();
            if (ok && *ok) {
                ui->customThemeEditBuffer.hasAudio = true;
                Notification::create("Track imported -- Save to keep it", NotificationIcon::Success)
                    ->show();
            } else {
                Notification::create("Import failed or cancelled", NotificationIcon::Warning)
                    ->show();
            }
            s_customThemeAudioTask = {};
        }
    }

    void MenuInterface::drawCustomThemeEditorPopup() {
        pollCustomThemeAudioImportTask();
        if (customThemeEditorOpen)
            ImGui::OpenPopup("Custom Theme Editor");
        ImGui::SetNextWindowSize(ImVec2(480, 560), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
                "Custom Theme Editor", nullptr, ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::TextColored(theme.getAccent(),
                           "%s",
                           customThemeEditIsNew ? "New Custom Theme" : "Edit Custom Theme");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));

        Widgets::SectionHeader("Identity", theme);
        ImGui::Text("Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteName", cteName, sizeof(cteName));
        ImGui::Text("File Suffix (optional)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteExtension", cteExtension, sizeof(cteExtension));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("What your saved macros end in, like .brrr or .toosii. Leave blank to "
                           "auto-generate one from the name.");
        ImGui::TextWrapped("Type it with or without the leading dot -- either works, only the "
                           "letters/numbers are kept.");
        ImGui::PopStyleColor();
        ImGui::Text("Subtitle");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteSubtitle", cteSubtitle, sizeof(cteSubtitle));
        ImGui::Text("Brand Tag (corner word)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteBrandTag", cteBrandTag, sizeof(cteBrandTag));

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Colors", theme);
        ImGui::Text("Accent");
        ImGui::SameLine();
        ImGui::ColorEdit4("##cteAccent",
                          (float*)&customThemeEditBuffer.accent,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::Text("Background");
        ImGui::SameLine();
        ImGui::ColorEdit4("##cteBg",
                          (float*)&customThemeEditBuffer.bg,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::Text("Card");
        ImGui::SameLine();
        ImGui::ColorEdit4("##cteCard",
                          (float*)&customThemeEditBuffer.card,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::Text("Text");
        ImGui::SameLine();
        ImGui::ColorEdit4("##cteText",
                          (float*)&customThemeEditBuffer.textPrimary,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::Text("Text Secondary");
        ImGui::SameLine();
        ImGui::ColorEdit4("##cteText2",
                          (float*)&customThemeEditBuffer.textSecondary,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        Widgets::StyledSliderFloat(
            "Corner Radius", &customThemeEditBuffer.cornerRadius, 0.f, 16.f, theme, true);
        Widgets::StyledSliderFloat(
            "Background Opacity", &customThemeEditBuffer.bgOpacity, 0.3f, 1.f, theme, true);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Quotes", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Shown on the Replay tab, Tools tab, and at the end of Credits -- matches "
            "how every built-in theme's flavor quotes work.");
        ImGui::PopStyleColor();
        ImGui::Text("Replay tab quote");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQR", cteQuoteReplayText, sizeof(cteQuoteReplayText));
        ImGui::Text("Attribution");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQRA", cteQuoteReplayAttr, sizeof(cteQuoteReplayAttr));
        ImGui::Text("Tools tab quote");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQT", cteQuoteToolsText, sizeof(cteQuoteToolsText));
        ImGui::Text("Attribution");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQTA", cteQuoteToolsAttr, sizeof(cteQuoteToolsAttr));
        ImGui::Text("Credits closing quote");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQC", cteQuoteCreditsText, sizeof(cteQuoteCreditsText));
        ImGui::Text("Attribution");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteQCA", cteQuoteCreditsAttr, sizeof(cteQuoteCreditsAttr));
        ImGui::Text("Credits badge (short tag line)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cteBadge", cteCreditsBadge, sizeof(cteCreditsBadge));

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Big Brrr Track", theme);
        bool stagedAudioExists = fs::exists(getCustomThemesDir() / "_editing_brrr.mp3");
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(stagedAudioExists ? "A track is set for this theme."
                                             : "No track imported yet -- BIG BRRRR will use the "
                                               "default GucciBot track until one is.");
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Import Track (mp3)", ImVec2(-1, 28), theme, anim))
            importCustomThemeAudio();
        if (stagedAudioExists) {
            double bpmD = customThemeEditBuffer.bpm;
            if (ImGui::InputDouble("BPM", &bpmD, 1.0, 5.0, "%.1f"))
                customThemeEditBuffer.bpm = std::max(20.0, bpmD);
            double offD = customThemeEditBuffer.dropOffsetSec;
            if (ImGui::InputDouble("Drop offset (seconds)", &offD, 0.1, 1.0, "%.2f"))
                customThemeEditBuffer.dropOffsetSec = std::max(0.0, offD);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Where BIG BRRRR starts playback -- skips a slow intro the same way the "
                "built-in tracks do. 0 plays from the very start.");
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0, 12));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));

        float bw = (ImGui::GetContentRegionAvail().x - 16) / (customThemeEditIsNew ? 2.f : 3.f);
        bool canSave = cteName[0] != 0;
        if (!canSave)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        bool saveClicked = Widgets::StyledButton("Save", ImVec2(bw, 30), theme, anim);
        if (!canSave)
            ImGui::PopStyleVar();
        if (saveClicked && canSave) {
            customThemeEditBuffer.name = cteName;
            customThemeEditBuffer.subtitle = cteSubtitle;
            customThemeEditBuffer.brandTag = cteBrandTag[0] ? cteBrandTag : "Brrr.";
            customThemeEditBuffer.quoteReplay = {cteQuoteReplayText, cteQuoteReplayAttr};
            customThemeEditBuffer.quoteTools = {cteQuoteToolsText, cteQuoteToolsAttr};
            customThemeEditBuffer.quoteCredits = {cteQuoteCreditsText, cteQuoteCreditsAttr};
            customThemeEditBuffer.creditsBadge =
                cteCreditsBadge[0] ? cteCreditsBadge : "Custom Theme";
            customThemeEditBuffer.hasAudio = stagedAudioExists;

            bool wasActive =
                activeTheme == THEME_CUSTOM && activeCustomThemeName == customThemeEditOriginalName;

            std::string typedExt(cteExtension);
            bool extensionRejected = false;
            if (!typedExt.empty()) {
                bool ok = false;
                std::string sanitized =
                    sanitizeCustomExtension(typedExt, customThemeEditOriginalName, &ok);
                if (!ok) {
                    Notification::create("That suffix is taken or invalid -- try another.",
                                         NotificationIcon::Error)
                        ->show();
                    extensionRejected = true;
                } else {
                    customThemeEditBuffer.extension = sanitized;
                }
            } else {
                bool renamedName = !customThemeEditIsNew &&
                                   customThemeEditOriginalName != customThemeEditBuffer.name;
                if (customThemeEditIsNew || renamedName)
                    customThemeEditBuffer.extension.clear();
                else
                    customThemeEditBuffer.extension = customThemeEditOriginalExtension;
            }

            if (!extensionRejected) {
                std::string oldExtForCleanup = customThemeEditOriginalExtension;

                std::error_code ec;
                auto stagingPath = getCustomThemesDir() / "_editing_brrr.mp3";
                if (stagedAudioExists) {
                    if (customThemeEditBuffer.extension.empty())
                        customThemeEditBuffer.extension =
                            deriveCustomThemeExtension(customThemeEditBuffer.name);
                    fs::create_directories(getCustomThemesDir(), ec);
                    fs::copy_file(stagingPath,
                                  getCustomThemesDir() /
                                      (customThemeEditBuffer.extension + "_brrr.mp3"),
                                  fs::copy_options::overwrite_existing,
                                  ec);
                }

                saveCustomTheme(customThemeEditBuffer);

                if (!customThemeEditIsNew && !oldExtForCleanup.empty() &&
                    oldExtForCleanup != customThemeEditBuffer.extension) {
                    fs::remove(getCustomThemesDir() / (oldExtForCleanup + ".json"), ec);
                    fs::remove(getCustomThemesDir() / (oldExtForCleanup + "_brrr.mp3"), ec);
                }
                fs::remove(stagingPath, ec);

                if (wasActive || customThemeEditIsNew) {
                    activeTheme = THEME_CUSTOM;
                    activeCustomThemeName = customThemeEditBuffer.name;
                    theme.accentColor = customThemeEditBuffer.accent;
                    theme.bgColor = customThemeEditBuffer.bg;
                    theme.cardColor = customThemeEditBuffer.card;
                    theme.textPrimary = customThemeEditBuffer.textPrimary;
                    theme.textSecondary = customThemeEditBuffer.textSecondary;
                    theme.cornerRadius = customThemeEditBuffer.cornerRadius;
                    theme.bgOpacity = customThemeEditBuffer.bgOpacity;
                    theme.activePreset = -1;
                    saveSettings();
                }
                customThemeEditorOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine(0, 8);
        if (Widgets::StyledButton("Cancel", ImVec2(bw, 30), theme, anim)) {
            std::error_code ec;
            fs::remove(getCustomThemesDir() / "_editing_brrr.mp3", ec);
            customThemeEditorOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if (!customThemeEditIsNew) {
            ImGui::SameLine(0, 8);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.15f, 0.15f, 0.9f));
            if (Widgets::StyledButton("Delete", ImVec2(bw, 30), theme, anim)) {
                std::error_code ec;
                fs::remove(getCustomThemesDir() / "_editing_brrr.mp3", ec);
                deleteCustomTheme(customThemeEditOriginalName);
                saveSettings();
                customThemeEditorOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::EndPopup();
    }

    void MenuInterface::deleteCustomTheme(const std::string& name) {
        auto it = std::find_if(customThemes.begin(), customThemes.end(), [&](auto& x) {
            return x.name == name;
        });
        if (it == customThemes.end())
            return;
        std::error_code ec;
        fs::remove(getCustomThemesDir() / (it->extension + ".json"), ec);
        fs::remove(getCustomThemesDir() / (it->extension + "_brrr.mp3"), ec);
        if (activeTheme == THEME_CUSTOM && activeCustomThemeName == name) {
            activeTheme = THEME_GUCCI;
            activeCustomThemeName.clear();
        }
        customThemes.erase(it);
    }

} // namespace gucci

// The interface's shell: the window, its pages, the in-game overlay, saved
// settings, and the ImGui hook the whole mod draws from. Written from scratch
// on 2026-10-01 to replace the old menu (see ui.hpp).

#include "ui/ui.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/state.hpp"
#include "ui/themes.hpp"

#include "absense/glue.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/shim.hpp"
#include "core/GucciBot.hpp"
#include "hacks/autoclicker.hpp"
#include "render/renderer.hpp"
#include "tools/macro_check.hpp"

#include <Geode/Geode.hpp>
#include <imgui-cocos.hpp>

#include <algorithm>
#include <cctype>

using namespace geode::prelude;

namespace gucci::ui {

    // ---------------------------------------------------------------- state

    namespace {
        bool g_open = false;
        bool g_compact = false;
        int g_page = 0;
        Keys g_keys;
        int* g_captureTarget = nullptr;
        int* g_justCaptured = nullptr;

        struct KeySlot {
            const char* saveKey;
            const char* label;
            int Keys::*field;
        };
        // Saved names are the old menu's, so existing bindings carry over.
        const KeySlot kKeySlots[] = {
            {"key_menu", "Open the menu", &Keys::onMenu},
            {"key_frame_advance", "Pause / frame advance", &Keys::onFrameAdvance},
            {"key_frame_step", "Step one frame", &Keys::onFrameStep},
            {"key_back_step", "Step back one frame", &Keys::onBackStep},
            {"key_replay_toggle", "Start / stop playback", &Keys::onReplayToggle},
            {"key_noclip", "Noclip", &Keys::onNoclip},
            {"key_safe_mode", "Safe mode", &Keys::onSafeMode},
            {"key_audio_pitch", "Audio pitch with speed", &Keys::onAudioPitch},
            {"key_rng_lock", "RNG lock", &Keys::onRngLock},
            {"key_layout_mode", "Layout mode", &Keys::onLayoutMode},
            {"key_no_mirror", "No mirror effect", &Keys::onNoMirror},
            {"key_autoclicker", "Autoclicker", &Keys::onAutoclicker},
            {"key_intentional_death", "Record an intentional death", &Keys::onIntentionalDeath},
            {"key_auto_flip", "Auto flip on death", &Keys::onAutoFlip},
            {"key_prevent_death", "Prevent death", &Keys::onPreventDeath},
            {"key_mirror_inputs", "Mirror inputs", &Keys::onMirrorInputs},
            {"key_compact_mode", "Compact mode", &Keys::onCompactMode},
            {"key_trajectory", "Path preview", &Keys::onTrajectory},
            {"key_hitboxes", "Hitboxes", &Keys::onHitboxes},
        };

        // Engine switches the engine does not load itself (engine_core's
        // loader covers the rest); read here at startup, same keys as before.
        struct SavedSwitch {
            const char* key;
            bool GucciEngine::*field;
        };
        const SavedSwitch kUiLoadedSwitches[] = {
            {"hack_hide_attempts", &GucciEngine::hackHideAttempts},
            {"hack_hide_percentage", &GucciEngine::hackHidePercentage},
            {"hack_no_flash", &GucciEngine::hackNoSpikeFlash},
            {"hack_auto_retry", &GucciEngine::hackAutoRetry},
            {"hack_respawn_instant", &GucciEngine::hackRespawnInstant},
            {"hack_force_platformer", &GucciEngine::hackForcePlatformer},
        };

        void loadSettings() {
            auto* mod = Mod::get();
            for (auto const& slot : kKeySlots)
                g_keys.*(slot.field) = static_cast<int>(mod->getSavedValue<int64_t>(slot.saveKey, g_keys.*(slot.field)));
            int const themeId = static_cast<int>(mod->getSavedValue<int64_t>("active_theme", 0));
            std::string const custom = mod->getSavedValue<std::string>("active_custom_theme", "");
            if (themeId == kCustomThemeId && !custom.empty())
                themes::setActiveCustom(custom);
            else
                themes::setActive(themes::builtin(themeId) ? themeId : 0);
            g_compact = mod->getSavedValue<bool>("ui_compact_mode", false);
            look().textScale = static_cast<float>(mod->getSavedValue<double>("ui_text_scale", 1.0));
            look().effects = mod->getSavedValue<bool>("ui_effects", true);
            look().accentCycle = mod->getSavedValue<bool>("theme_glow_cycle", false);
            auto* gb = GucciEngine::get();
            for (auto const& s : kUiLoadedSwitches)
                gb->*(s.field) = mod->getSavedValue<bool>(s.key, gb->*(s.field));
        }

        void saveEngineSwitch(const char* key, bool value) {
            Mod::get()->setSavedValue(key, value);
        }
    } // namespace

    void saveSettings() {
        auto* mod = Mod::get();
        for (auto const& slot : kKeySlots)
            mod->setSavedValue<int64_t>(slot.saveKey, g_keys.*(slot.field));
        mod->setSavedValue<int64_t>("active_theme", themes::activeId());
        mod->setSavedValue("active_custom_theme", themes::activeCustomName());
        mod->setSavedValue("ui_compact_mode", g_compact);
        mod->setSavedValue<double>("ui_text_scale", look().textScale);
        mod->setSavedValue("ui_effects", look().effects);
        mod->setSavedValue("theme_glow_cycle", look().accentCycle);
    }

    bool isOpen() {
        return g_open;
    }

    void setOpen(bool open) {
        if (g_open && !open)
            saveSettings();
        g_open = open;
    }

    void toggleOpen() {
        setOpen(!g_open);
    }

    bool compactMode() {
        return g_compact;
    }

    void setCompactMode(bool on) {
        g_compact = on;
        Mod::get()->setSavedValue("ui_compact_mode", on);
    }

    Keys& keys() {
        return g_keys;
    }

    bool capturingKey() {
        return g_captureTarget != nullptr;
    }

    bool feedKeyCapture(int keyCode) {
        if (!g_captureTarget)
            return false;
        *g_captureTarget = keyCode == static_cast<int>(enumKeyCodes::KEY_Escape) ? 0 : keyCode;
        g_justCaptured = g_captureTarget;
        g_captureTarget = nullptr;
        saveSettings();
        return true;
    }

    std::string keyName(int k) {
        if (k == 0)
            return "Unbound";
        if ((k >= '0' && k <= '9') || (k >= 'A' && k <= 'Z'))
            return std::string(1, static_cast<char>(k));
        if (k >= 0x70 && k <= 0x87)
            return "F" + std::to_string(k - 0x6F);
        if (k >= 0x60 && k <= 0x69)
            return "Numpad " + std::to_string(k - 0x60);
        switch (k) {
            case 0x08: return "Backspace";
            case 0x09: return "Tab";
            case 0x0D: return "Enter";
            case 0x10: return "Shift";
            case 0x11: return "Ctrl";
            case 0x12: return "Alt";
            case 0x13: return "Pause";
            case 0x14: return "Caps Lock";
            case 0x1B: return "Escape";
            case 0x20: return "Space";
            case 0x21: return "Page Up";
            case 0x22: return "Page Down";
            case 0x23: return "End";
            case 0x24: return "Home";
            case 0x25: return "Left";
            case 0x26: return "Up";
            case 0x27: return "Right";
            case 0x28: return "Down";
            case 0x2D: return "Insert";
            case 0x2E: return "Delete";
            case 0x6A: return "Numpad *";
            case 0x6B: return "Numpad +";
            case 0x6D: return "Numpad -";
            case 0x6E: return "Numpad .";
            case 0x6F: return "Numpad /";
            case 0xA0: return "Left Shift";
            case 0xA1: return "Right Shift";
            case 0xA2: return "Left Ctrl";
            case 0xA3: return "Right Ctrl";
            case 0xA4: return "Left Alt";
            case 0xA5: return "Right Alt";
            case 0xBA: return ";";
            case 0xBB: return "=";
            case 0xBC: return ",";
            case 0xBD: return "-";
            case 0xBE: return ".";
            case 0xBF: return "/";
            case 0xC0: return "`";
            case 0xDB: return "[";
            case 0xDC: return "\\";
            case 0xDD: return "]";
            case 0xDE: return "'";
            default: break;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Key %d", k);
        return buf;
    }

    int activeThemeId() {
        return themes::activeId();
    }

    std::string macroExtension() {
        return themes::active().extension;
    }

    std::vector<std::string> knownMacroExtensions() {
        std::vector<std::string> out;
        auto add = [&](std::string const& ext) {
            if (!ext.empty() && std::find(out.begin(), out.end(), ext) == out.end())
                out.push_back(ext);
        };
        add(macroExtension());
        for (auto const& t : themes::builtins())
            add(t.extension);
        for (auto const& t : themes::customs())
            add(t.extension);
        return out;
    }

    std::string activeCustomThemeName() {
        return themes::activeId() == kCustomThemeId ? themes::activeCustomName() : std::string();
    }

    std::string activeCustomThemeExtension() {
        return themes::activeId() == kCustomThemeId ? themes::active().extension : std::string();
    }

    std::filesystem::path customThemesDir() {
        return themes::customThemesDir();
    }

    ThemeAudio activeThemeAudio() {
        return {};
    }

    namespace detail {
        int* captureTarget() {
            return g_captureTarget;
        }
        void setCaptureTarget(int* target) {
            g_captureTarget = target;
        }
        bool takeCaptured(int* target) {
            if (g_justCaptured != target)
                return false;
            g_justCaptured = nullptr;
            return true;
        }
    } // namespace detail

    // ---------------------------------------------------------------- pages

    namespace {

        bool engineSwitch(const char* label, const char* hint, bool* value, const char* saveKey) {
            bool const changed = kit::SwitchRow(label, hint, value);
            if (changed && saveKey)
                saveEngineSwitch(saveKey, *value);
            return changed;
        }

        std::filesystem::path replaysDir() {
            return Mod::get()->getSaveDir() / "replays";
        }

        void loadMacroByName(std::string const& name) {
            auto* gb = GucciEngine::get();
            std::error_code ec;
            for (auto const& ext : knownMacroExtensions()) {
                auto path = replaysDir() / (name + ext);
                if (std::filesystem::exists(path, ec)) {
                    gb->replay.load(path);
                    gb->replayName = name;
                    return;
                }
            }
        }

        void pageMacro() {
            auto* gb = GucciEngine::get();
            auto& actions = gb->replay.m_actionAtom.m_actions;

            kit::BeginCard("Macro", "Record a run, or play one back.");
            int mode = gb->isRecording() ? 1 : gb->isPlaying() ? 2 : 0;
            const char* modes[] = {"Off", "Record", "Play"};
            if (kit::ChoiceRow("Mode", nullptr, &mode, modes, 3))
                gb->setMode(mode == 1 ? GucciEngine::Mode::Recording
                            : mode == 2 ? GucciEngine::Mode::Playing
                                        : GucciEngine::Mode::Idle);
            kit::RowBegin("Name", nullptr);
            kit::InputText("name", &gb->replayName, "macro name");
            kit::RowEnd();

            kit::RowBegin("File", nullptr);
            bool const canSave = !actions.empty() && !gb->replayName.empty();
            ImGui::BeginDisabled(!canSave);
            if (kit::Button("Save", Tone::Accent)) {
                std::error_code ec;
                std::filesystem::create_directories(replaysDir(), ec);
                auto path = replaysDir() / (gb->replayName + macroExtension());
                if (gb->replayBackupsEnabled)
                    gb->replay.backupExisting(path);
                gb->replay.save(path);
                gb->reloadMacroList();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool const canCalc = PlayLayer::get() && !actions.empty();
            ImGui::BeginDisabled(!canCalc);
            if (kit::Button("Calculate"))
                gb->analyzeFrameWindows();
            ImGui::EndDisabled();
            kit::RowEnd();
            kit::Hint(fmt::format("{} actions  |  {:.0f} TPS{}", actions.size(), gb->updater.m_tps,
                                  canCalc ? "" : "  |  Calculate needs a level open and a macro")
                          .c_str());
            if (!gb->startPosWarning.empty())
                kit::Note(gb->startPosWarning.c_str(), Tone::Warn);
            kit::EndCard();

            kit::BeginCard("Speed", nullptr);
            float tps = static_cast<float>(gb->updater.m_tps);
            kit::RowBegin("Tick rate", "Physics ticks per second");
            if (kit::InputFloat("tps", &tps, 1.f, "%.0f") && tps >= 1.f) {
                gb->updater.setTps(tps);
                Mod::get()->setSavedValue<double>("eng_tick_rate", tps);
            }
            kit::RowEnd();
            float speed = static_cast<float>(gb->updater.m_speedhack);
            if (kit::SliderRow("Game speed", nullptr, &speed, 0.05f, 2.f, "%.2fx")) {
                gb->updater.m_speedhack = speed;
                Mod::get()->setSavedValue<double>("eng_speed", speed);
            }
            kit::EndCard();

            kit::BeginCard("Saved macros", nullptr);
            static std::string s_filter;
            kit::InputText("filter", &s_filter, "search");
            if (kit::Button("Refresh"))
                gb->reloadMacroList();
            ImGui::SameLine();
            static std::string s_checkReport;
            if (kit::Button("Check macro")) {
                auto report = macrocheck::check(actions);
                s_checkReport = report.ok() ? "No problems found." : fmt::format("{} problem(s):", report.findings.size());
                for (size_t i = 0; i < report.findings.size() && i < 20; ++i) {
                    auto const& f = report.findings[i];
                    s_checkReport += fmt::format("\nframe {}{}: {}", f.frame, f.lane >= 3 ? " (P2)" : "", macrocheck::name(f.problem));
                }
            }
            if (!s_checkReport.empty())
                kit::Note(s_checkReport.c_str(), Tone::Muted);
            std::string lowered = s_filter;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });
            ImGui::BeginChild("##macros", ImVec2(0.f, 200.f), ImGuiChildFlags_FrameStyle);
            for (auto const& name : gb->storedMacros) {
                std::string n = name;
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!lowered.empty() && n.find(lowered) == std::string::npos)
                    continue;
                if (ImGui::Selectable(name.c_str(), name == gb->replayName) && !gb->isRecording())
                    loadMacroByName(name);
            }
            ImGui::EndChild();
            kit::EndCard();
        }

        void pageHacks() {
            auto* gb = GucciEngine::get();
            auto& upd = gb->updater;
            kit::BeginCard("Gameplay", nullptr);
            engineSwitch("Noclip", "Pass through hazards", &gb->noclipEnabled, "hack_noclip");
            engineSwitch("Safe mode", "No progress or stats are saved", &gb->protectedMode, nullptr);
            engineSwitch("Instant respawn", nullptr, &gb->hackRespawnInstant, "hack_respawn_instant");
            engineSwitch("Auto retry", nullptr, &gb->hackAutoRetry, "hack_auto_retry");
            engineSwitch("Force platformer", nullptr, &gb->hackForcePlatformer, "hack_force_platformer");
            engineSwitch("RNG lock", "Same random triggers every attempt", &gb->rngLocked, nullptr);
            kit::EndCard();

            kit::BeginCard("Visual", nullptr);
            engineSwitch("Layout mode", nullptr, &gb->layoutMode, "hack_layout_mode");
            engineSwitch("No mirror effect", nullptr, &gb->noMirrorEffect, "hack_no_mirror");
            engineSwitch("Hide attempts", nullptr, &gb->hackHideAttempts, "hack_hide_attempts");
            engineSwitch("Hide percentage", nullptr, &gb->hackHidePercentage, "hack_hide_percentage");
            engineSwitch("No death flash", nullptr, &gb->hackNoSpikeFlash, "hack_no_flash");
            engineSwitch("Audio pitch follows speed", nullptr, &gb->audioPitchEnabled, "hack_audio_pitch");
            kit::EndCard();

            kit::BeginCard("Engine", nullptr);
            engineSwitch("Lock delta", "Fixed physics step per tick", &upd.m_lockDelta, "feat_lock_delta");
            engineSwitch("Backwards stepping", "Step back while paused", &upd.m_backwardsStepping, "feat_backwards_step");
            engineSwitch("Auto flip on death", nullptr, &upd.m_autoFlipOnDeath, "feat_auto_flip");
            engineSwitch("Speedhack audio", nullptr, &upd.m_speedhackAudio, "feat_speedhack_audio");
            engineSwitch("Scroll speed fix", nullptr, &upd.m_ssbFix, "feat_scroll_speed_fix");
            kit::EndCard();

            kit::BeginCard("Autoclicker", nullptr);
            auto* ac = Autoclicker::get();
            kit::SwitchRow("On", nullptr, &ac->enabled);
            kit::SwitchRow("Only while holding", nullptr, &ac->onlyWhileHolding);
            kit::SliderRow("Hold ticks", nullptr, &ac->p1.holdTicks, 1, 30);
            kit::SliderRow("Release ticks", nullptr, &ac->p1.releaseTicks, 1, 30);
            kit::EndCard();
        }

        void pagePathfinder() {
            kit::BeginCard("Pathfinder", "Finds a way through the level on its own.");
            bool fromStart = absense::startFromBeginning();
            if (kit::SwitchRow("Start from the beginning", nullptr, &fromStart))
                absense::setStartFromBeginning(fromStart);
            auto const s = absense::status();
            kit::RowBegin("", nullptr);
            if (!s.running && !absense::startPending()) {
                ImGui::BeginDisabled(!PlayLayer::get());
                if (kit::Button("Start", Tone::Accent, -1.f))
                    absense::requestStart(fromStart);
                ImGui::EndDisabled();
            } else if (kit::Button("Stop", Tone::Bad, -1.f)) {
                absense::stopPathfinder();
            }
            kit::RowEnd();
            if (s.running || s.bestProgress > 0.f) {
                kit::Progress(s.bestProgress, fmt::format("best {:.1f}%", s.bestProgress * 100.f).c_str());
                kit::Hint(fmt::format("{}  |  {} decisions, {} times back, {:.0f} s", s.phase, s.decisions, s.backtracks, s.seconds).c_str());
            }
            if (!s.message.empty())
                kit::Note(s.message.c_str(), Tone::Muted);
            kit::EndCard();
        }

        void pageRender() {
            auto* sl = SLRenderer::get();
            auto* mod = Mod::get();
            kit::BeginCard("Render", "Records the level to a video with FFmpeg.");
            if (sl->isRecording()) {
                kit::Chip("Rendering", Tone::Bad);
                if (kit::Button("Stop", Tone::Bad, -1.f))
                    sl->flushAndStop();
            } else if (sl->m_startOnNextLevel) {
                kit::Chip("Waiting for a level", Tone::Accent);
                if (kit::Button("Cancel", Tone::Plain, -1.f))
                    sl->m_startOnNextLevel = false;
            } else if (PlayLayer::get()) {
                if (kit::Button("Start", Tone::Accent, -1.f)) {
                    sl->loadSettingsFromGeode();
                    sl->queueStart();
                }
            } else if (kit::Button("Render the next level I open", Tone::Accent, -1.f)) {
                sl->m_startOnNextLevel = true;
            }
            kit::EndCard();

            kit::BeginCard("Video", nullptr);
            int width = static_cast<int>(mod->getSavedValue<int64_t>("render_width", 1920));
            int height = static_cast<int>(mod->getSavedValue<int64_t>("render_height", 1080));
            int fps = static_cast<int>(mod->getSavedValue<int64_t>("render_fps", 60));
            kit::RowBegin("Width", nullptr);
            if (kit::InputInt("w", &width, 0) && width > 0)
                mod->setSavedValue<int64_t>("render_width", width);
            kit::RowEnd();
            kit::RowBegin("Height", nullptr);
            if (kit::InputInt("h", &height, 0) && height > 0)
                mod->setSavedValue<int64_t>("render_height", height);
            kit::RowEnd();
            kit::RowBegin("FPS", nullptr);
            if (kit::InputInt("fps", &fps, 0) && fps > 0)
                mod->setSavedValue<int64_t>("render_fps", fps);
            kit::RowEnd();
            bool audio = mod->getSavedValue<bool>("render_include_audio", true);
            if (kit::SwitchRow("Include audio", nullptr, &audio))
                mod->setSavedValue("render_include_audio", audio);
            kit::EndCard();
        }

        void pageSettings() {
            kit::BeginCard("Theme", nullptr);
            std::vector<std::string> names;
            std::vector<int> ids;
            std::vector<std::string> customNames;
            int current = 0;
            for (auto const& t : themes::builtins()) {
                if (t.id == themes::activeId())
                    current = static_cast<int>(names.size());
                names.push_back(t.name);
                ids.push_back(t.id);
                customNames.emplace_back();
            }
            for (auto const& t : themes::customs()) {
                if (themes::activeId() == kCustomThemeId && t.name == themes::activeCustomName())
                    current = static_cast<int>(names.size());
                names.push_back(t.name + " (custom)");
                ids.push_back(kCustomThemeId);
                customNames.push_back(t.name);
            }
            kit::RowBegin("Theme", "Also sets the macro file extension");
            if (kit::Dropdown("theme", &current, names)) {
                if (ids[static_cast<size_t>(current)] == kCustomThemeId)
                    themes::setActiveCustom(customNames[static_cast<size_t>(current)]);
                else
                    themes::setActive(ids[static_cast<size_t>(current)]);
                saveSettings();
            }
            kit::RowEnd();
            kit::SliderRow("Text size", nullptr, &look().textScale, 0.8f, 1.4f, "%.2fx");
            kit::SwitchRow("Cycle the accent colour", nullptr, &look().accentCycle);
            kit::SwitchRow("Animations", nullptr, &look().effects);
            bool compact = g_compact;
            if (kit::SwitchRow("Compact mode", "A small quick-controls window instead of this one", &compact))
                setCompactMode(compact);
            kit::EndCard();

            kit::BeginCard("Keybinds", "Click a key, then press the new one. Right-click unbinds.");
            for (auto const& slot : kKeySlots)
                if (kit::KeyRow(slot.label, nullptr, &(g_keys.*(slot.field))))
                    saveSettings();
            kit::EndCard();
        }

        void pageCredits() {
            kit::BeginCard("Credits", nullptr);
            struct Line {
                const char* who;
                const char* what;
            };
            const Line lines[] = {
                {"Nigelx1", "Creator and owner of GucciBot; every idea, every theme, every decision is his call."},
                {"Claude", "Wrote the code. Essentially the whole codebase, not a euphemism."},
                {"Juice", "Designed the frame-window algorithm GucciBot ran on through 1.7 and the marker shapes in 1.8; lead co-tester."},
                {"anticroom", "Calculate is his analyzer, ported in near-verbatim; sub-tick CBF recording and the render intro card too."},
                {"NaN GD", "The L* precision formula (nandl.pages.dev)."},
                {"C0nscious", "Implemented NaN's formula as Frame Window Counter (MIT)."},
                {"peony", "Silicate."},
                {"Absent", "Absense; the Pathfinder's default engine is his."},
                {"ToastexGD", "Built ToastyReplay, the project GucciBot started as."},
                {"GWDdoS", "Astral, and the cleanup that got the repo public-ready."},
                {"Bogdaner09", "Click Indicators inspiration."},
                {"Gucci Mane", "He's the truth. Brrr."},
            };
            for (auto const& l : lines) {
                {
                    UseFont f(Font::Strong);
                    ImGui::TextUnformatted(l.who);
                }
                kit::Hint(l.what);
                kit::Gap(0.25f);
            }
            kit::Hint("GucciBot is free software under the GNU General Public License v3.0.");
            kit::EndCard();
        }

        struct Page {
            const char* title;
            void (*draw)();
        };
        const Page kPages[] = {
            {"Macro", pageMacro},
            {"Hacks", pageHacks},
            {"Pathfinder", pagePathfinder},
            {"Render", pageRender},
            {"Settings", pageSettings},
            {"Credits", pageCredits},
        };
        constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));

        // ------------------------------------------------------------ windows

        void drawMain() {
            auto const& pal = look().pal;
            ImGuiIO const& io = ImGui::GetIO();
            ImGui::SetNextWindowSize(ImVec2(720.f, 500.f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(520.f, 360.f), ImVec2(FLT_MAX, FLT_MAX));
            bool open = true;
            if (!ImGui::Begin("GucciBot##menu", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                          ImGuiWindowFlags_NoScrollbar)) {
                ImGui::End();
                return;
            }

            // Sidebar
            ImGui::BeginChild("##side", ImVec2(150.f, 0.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            {
                UseFont f(Font::Title);
                ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
                ImGui::TextUnformatted(themes::active().name.c_str());
                ImGui::PopStyleColor();
            }
            {
                UseFont f(Font::Small);
                ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
                ImGui::TextUnformatted(MOD_VERSION);
                ImGui::PopStyleColor();
            }
            kit::Gap(0.6f);
            for (int i = 0; i < kPageCount; ++i) {
                bool const selected = g_page == i;
                ImVec2 const a = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(kPages[i].title, selected, ImGuiSelectableFlags_None, ImVec2(0.f, 26.f)))
                    g_page = i;
                if (selected)
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x - 4.f, a.y + 4.f), ImVec2(a.x - 1.f, a.y + 22.f),
                                                              u32(pal.accent), 1.5f);
            }
            float const closeY = ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 6.f;
            if (closeY > ImGui::GetCursorPosY())
                ImGui::SetCursorPosY(closeY);
            if (kit::Button("Close", Tone::Muted, -1.f))
                open = false;
            ImGui::EndChild();

            ImGui::SameLine();

            // Page
            ImGui::BeginChild("##page", ImVec2(0.f, 0.f));
            {
                UseFont f(Font::Display);
                ImGui::TextUnformatted(kPages[g_page].title);
            }
            kit::Gap(0.3f);
            kPages[g_page].draw();
            ImGui::EndChild();

            ImGui::End();
            if (!open)
                setOpen(false);
        }

        void drawCompact() {
            auto* gb = GucciEngine::get();
            ImGui::SetNextWindowPos(ImVec2(12.f, 60.f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Always);
            bool open = true;
            if (ImGui::Begin("GucciBot##compact", &open, ImGuiWindowFlags_NoCollapse)) {
                int mode = gb->isRecording() ? 1 : gb->isPlaying() ? 2 : 0;
                const char* modes[] = {"Off", "Rec", "Play"};
                if (kit::Choice("mode", &mode, modes, 3))
                    gb->setMode(mode == 1 ? GucciEngine::Mode::Recording
                                : mode == 2 ? GucciEngine::Mode::Playing
                                            : GucciEngine::Mode::Idle);
                kit::InputText("name", &gb->replayName, "macro name");
                bool const canSave = !gb->replay.m_actionAtom.empty() && !gb->replayName.empty();
                ImGui::BeginDisabled(!canSave);
                if (kit::Button("Save", Tone::Accent, -1.f)) {
                    auto dir = replaysDir();
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    gb->replay.save(dir / (gb->replayName + macroExtension()));
                    gb->reloadMacroList();
                }
                ImGui::EndDisabled();
                if (kit::Button("Full menu", Tone::Plain, -1.f))
                    setCompactMode(false);
            }
            ImGui::End();
            if (!open)
                setOpen(false);
        }

        // ------------------------------------------------------------ overlay

        void drawOverlay() {
            auto const s = absense::status();
            if (!s.running || !PlayLayer::get())
                return;
            ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGuiWindowFlags const flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            if (ImGui::Begin("##pathfinderOverlay", nullptr, flags)) {
                UseFont f(Font::Small);
                ImGui::Text("Pathfinder: %s  |  now %.1f%%  best %.1f%%", s.phase, s.progress * 100.f, s.bestProgress * 100.f);
            }
            ImGui::End();
        }

        void frame() {
            // anticroom's analyzer is driven from here, outside the game's
            // update (it pauses and single-steps the updater itself). Its
            // results are saved the moment a run finishes.
            {
                static bool s_wasRunning = false;
                auto& analyzer = ::Bot::get()->frameWindow();
                bool const running = analyzer.running();
                if (s_wasRunning && !running)
                    GucciEngine::get()->saveAcFrameWindowResults();
                s_wasRunning = running;
                analyzer.tick(PlayLayer::get());
                if (auto* pl = PlayLayer::get())
                    analyzer.updateProgressOverlay(pl);
            }

            look().refresh();
            drawOverlay();
            if (!g_open)
                return;
            if (g_compact)
                drawCompact();
            else
                drawMain();
        }

    } // namespace

} // namespace gucci::ui

$on_mod(Loaded) {
    ImGuiCocos::get()
        .setup([] {
            ImGui::GetIO().IniFilename = nullptr;
            gucci::ui::look().loadFonts();
            gucci::ui::themes::loadCustoms();
            gucci::ui::loadSettings();
        })
        .draw([] { gucci::ui::frame(); });
}

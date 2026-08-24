#pragma once

#include <imgui-cocos.hpp>
#include <matjson.hpp>
#include <filesystem>
#include <unordered_map>
#include <string>

#include "frame_editor.hpp"

struct ThemePreset {
    const char* name;
    ImVec4 accent, bg, card, textPrimary, textSecondary;
    float cornerRadius;
    float bgOpacity;
};

struct ThemeEngine {
    ImVec4 accentColor    = ImVec4(0.788f, 0.659f, 0.298f, 1.0f);
    ImVec4 bgColor        = ImVec4(0.051f, 0.051f, 0.051f, 0.96f);
    ImVec4 cardColor      = ImVec4(0.078f, 0.078f, 0.078f, 1.0f);
    ImVec4 textPrimary    = ImVec4(0.941f, 0.910f, 0.816f, 1.0f);
    ImVec4 textSecondary  = ImVec4(0.478f, 0.447f, 0.376f, 1.0f);
    float  bgOpacity      = 0.96f;
    float  cornerRadius   = 5.0f;
    float  textScale      = 1.0f;
    bool   glowCycleEnabled = false;
    float  glowCycleRate    = 0.5f;
    int    activePreset     = 0;

    ImVec4 computeCycleColor(float rate) const;
    ImVec4 getAccent() const;
    ImVec4 getGlowAccent() const;
    ImU32  getAccentU32(float alpha = 1.0f) const;
    ImU32  getAccentDimU32(float factor = 0.3f) const;
    ImU32  getTextU32() const;
    ImU32  getTextSecondaryU32() const;
    ImU32  getCardU32() const;
    void   applyToImGuiStyle();
    void   resetDefaults();
    void   applyPreset(int index);
    static const ThemePreset* getPresets();
    static int getPresetCount();
};

enum AnimDirection {
    ANIM_CENTER, ANIM_FROM_LEFT, ANIM_FROM_RIGHT, ANIM_FROM_TOP, ANIM_FROM_BOTTOM
};

struct AnimationState {
    float openProgress = 0.0f;
    bool  opening = false;
    bool  closing = false;
    float tabTransition = 1.0f;
    int   transitionFromTab = -1;
    std::unordered_map<ImGuiID, float> toggleAnims;
    std::unordered_map<ImGuiID, float> hoverAnims;
    struct ModuleAnimData { float progress = 0.0f; float height = 0.0f; };
    std::unordered_map<const void*, ModuleAnimData> moduleAnims;
    float animSpeed = 8.0f;
    AnimDirection openDirection = ANIM_CENTER;
    ImVec2 smoothCursorPos = ImVec2(0, 0);
    bool   cursorPosInitialized = false;
    void   update(float dt);
    float  easeOutCubic(float t);
    float  easeInOutQuad(float t);
};

struct KeybindSet {
    int menu = 0xA4, frameAdvance = 0x56, frameStep = 0x43;
    int replayToggle = 0, noclip = 0, safeMode = 0;
    int trajectory = 0, audioPitch = 0, rngLock = 0, hitboxes = 0;
    int layoutMode = 0, noMirror = 0, autoclicker = 0;
        int intentionalDeath = 0;
    int backStep = 0;
    int autoFlip = 0;
    int preventDeath = 0;
    int mirrorInputs = 0;
    int compactMode = 0;
};

enum BotTheme {
    THEME_GUCCI,
    THEME_TOOSII,
    THEME_TOOSII_SYRACUSE,
    THEME_TOOSII_SACSTATE,
    THEME_JA,
    THEME_GIDDEY,
    THEME_BAM,
    THEME_SEXYY,
    THEME_JUICE,
    THEME_BUTLER,
    THEME_SAWEETIE,
    THEME_MAYBACH,
    THEME_ROMO,
    THEME_GRIZZLEY,
    // Sentinel, not a real compile-time theme -- "which custom theme" is
    // tracked separately (MenuInterface::activeCustomThemeName), since the
    // custom list is a runtime, user-editable, unbounded set that can't
    // live in the fixed kThemePresets array or a fixed enum value the way
    // every built-in theme above does.
    THEME_CUSTOM
};

// Every extension a saved macro might have -- the 12 built-in ones plus
// whatever custom themes currently exist. Several places (gui.cpp,
// engine_core.cpp, brr_format.cpp) scan disk for "does a file with any
// known macro extension exist" and used to do it against a fixed 12-entry
// compile-time list; custom themes' extensions aren't known at compile
// time, so those scans need this instead. Defined once in customtheme.cpp
// rather than duplicated per file.
std::vector<std::string> allKnownMacroExtensions();

// One user-authored quote, matching the (text, attribution) pairs every
// built-in theme's GucciQuote calls already use.
struct CustomThemeQuote {
    std::string text;
    std::string attribution;
};

// User-created theme, per Nigel's "create your own theme" (2026-08-24,
// full scope: colors + identity + quotes + its own Big Brrr track). Unlike
// the built-in ThemePreset entries (compiled into kThemePresets, gui.cpp),
// these are created/edited/deleted at runtime and persisted as one JSON
// file per theme in Mod::get()->getSaveDir()/"customthemes" (mirroring
// BotSettingsPreset's one-file-per-preset convention, GucciBot.hpp/
// engine_core.cpp) -- except using real matjson serialization instead of
// that convention's hand-rolled unescaped string writer, since quote text
// is freeform user input very likely to contain literal quote characters.
struct CustomTheme {
    std::string name;      // shown in the preset list AND as the bot name/title
    std::string extension; // derived from name (sanitized), used for save files

    ImVec4 accent        = ImVec4(0.788f,0.659f,0.298f,1.f);
    ImVec4 bg            = ImVec4(0.051f,0.051f,0.051f,0.96f);
    ImVec4 card           = ImVec4(0.078f,0.078f,0.078f,1.f);
    ImVec4 textPrimary    = ImVec4(0.941f,0.910f,0.816f,1.f);
    ImVec4 textSecondary  = ImVec4(0.478f,0.447f,0.376f,1.f);
    float  cornerRadius   = 5.f;
    float  bgOpacity      = 0.96f;

    std::string subtitle     = "Frame perfect. Custom theme.";
    std::string brandTag     = "Brrr.";
    CustomThemeQuote quoteReplay  {"\"Custom, and proud of it.\"", "-- probably"};
    CustomThemeQuote quoteTools   {"\"My theme, my rules.\"", "-- probably"};
    CustomThemeQuote quoteCredits {"\"I built this one myself.\"", ""};
    std::string creditsBadge = "Custom | Made | By You";

    double bpm           = 140.0;
    double dropOffsetSec = 0.0;
    bool   hasAudio       = false; // whether a Big Brrr track was imported for this theme

    matjson::Value toJson() const;
    static CustomTheme fromJson(const matjson::Value& v);
};

class MenuInterface {
public:
    static MenuInterface* get();

    ImFont *fontBody=nullptr, *fontSmall=nullptr, *fontHeading=nullptr, *fontTitle=nullptr;
    bool shown=false, previouslyShown=false, setupComplete=false;

    int activeTab=0, previousTab=-1;
    int mainSubTab=0;

    BotTheme activeTheme = THEME_GUCCI;

    std::vector<CustomTheme> customThemes;
    std::string activeCustomThemeName; // which entry of customThemes is active, when activeTheme==THEME_CUSTOM
    // Returns the active custom theme, or nullptr if activeTheme != THEME_CUSTOM
    // or the named theme is missing (deleted out from under an active
    // session, a corrupt/missing file, etc.) -- every call site that reads
    // from this treats null as "fall back to GucciBot's own defaults",
    // never as a crash.
    CustomTheme* getActiveCustomTheme();
    std::filesystem::path getCustomThemesDir() const { return Mod::get()->getSaveDir() / "customthemes"; }
    // Derives a save-file extension from a theme name (lowercase, alnum
    // only, falls back to "customN" if the sanitized name is empty),
    // disambiguated against every built-in extension and every OTHER
    // custom theme's extension so two themes never collide on disk.
    std::string deriveCustomThemeExtension(const std::string& name) const;
    void loadCustomThemes(); // called once at startup
    void saveCustomTheme(CustomTheme& t); // creates or overwrites by name
    void deleteCustomTheme(const std::string& name);

    // "Create your own theme" editor state (Settings > Theme). Free-text
    // fields use fixed char buffers rather than std::string, matching this
    // codebase's existing InputText convention everywhere else (e.g.
    // macroNameBuffer, presetNameBuf) instead of introducing a different
    // pattern just for this one feature. Non-text fields (colors, BPM,
    // etc) are held directly on a staging CustomTheme instead, and only
    // written back into customThemes on an explicit Save.
    bool customThemeEditorOpen = false;
    bool customThemeEditIsNew = true;
    std::string customThemeEditOriginalName; // empty when creating new; used to detect renames on save
    CustomTheme customThemeEditBuffer;
    char cteName[64]={0};
    char cteSubtitle[160]={0};
    char cteBrandTag[32]={0};
    char cteQuoteReplayText[256]={0}, cteQuoteReplayAttr[128]={0};
    char cteQuoteToolsText[256]={0}, cteQuoteToolsAttr[128]={0};
    char cteQuoteCreditsText[256]={0}, cteQuoteCreditsAttr[128]={0};
    char cteCreditsBadge[128]={0};
    void openCustomThemeEditor(const CustomTheme* existing); // pass nullptr to create new
    void drawCustomThemeEditorPopup();

    ThemeEngine theme;
    AnimationState anim;
    KeybindSet keybinds;

    float ambientTime = 0.0f;
    bool  ambientWavesEnabled = true;

    int* rebindTarget = nullptr;

    FrameEditor frameEditor;

    char macroNameBuffer[256]={0}; bool macroNameReady=false;
    char rngBuffer[32]="1";       bool rngBufferInit=false;
    float tempTickRate=240.f, tempGameSpeed=1.f;

        int  renderPresetIndex=1;
    char renderNameBuf[256]="";
    char renderWidthBuf[16]="1920";
    char renderHeightBuf[16]="1080";
    char renderFpsBuf[16]="60";
    char renderCodecBuf[64]="";
    char renderBitrateBuf[16]="30";
    char renderExtBuf[16]=".mp4";
    char renderArgsBuf[256]="-pix_fmt yuv420p";
    char renderPixFmtBuf[24]="yuv420p";
    bool renderColorFix=true;
    bool megaHackLook=false;
    // Small corner panel (record/play, TPS/speed, frame step, the handful
    // of toggles you'd actually want mid-attempt) instead of the full tabbed
    // window, so the bot can stay open while actually playing a level
    // without blocking the view -- see drawCompactWindow().
    bool compactMode=false;
    float compactTempTickRate=240.f, compactTempGameSpeed=1.f;
    char renderVideoArgsBuf[256]="colorspace=all=bt709:iall=bt470bg:fast=1";
    char renderAudioArgsBuf[256]="";
    char renderAudioCodecBuf[64]="aac";
    char outputFolderBuf[512]="";
    char renderAudioBitrateBuf[16]="192k";
    char renderSecondsAfterBuf[16]="3";
    bool renderIncludeAudio=true;
    bool renderSplitAudioTracks=false;
    bool renderIncludeClicks=false;
    float renderSfxVol=1.f, renderMusicVol=1.f;
    bool renderHideEndscreen=false, renderHideLevelComplete=false;
    bool renderBufsInit=false;
    bool advancedWarningAccepted=false, showAdvancedWarning=false;
    char backupCodecBuf[64]="";
    char backupBitrateBuf[16]="30";
    char backupExtBuf[16]=".mp4";
    char backupArgsBuf[256]="-pix_fmt yuv420p";
    char backupVideoArgsBuf[256]="colorspace=all=bt709:iall=bt470bg:fast=1";
    char backupAudioArgsBuf[256]="";
    char backupSecondsAfterBuf[16]="3";

    ImVec2 windowPos=ImVec2(-1,-1); bool windowPosInitialized=false;
    ImVec2 windowSize=ImVec2(580.f,540.f);
    bool jupiterClickBarPageOpen=false;
    bool trainerClickBarPageOpen=false;

    void initialize();
    void drawInterface();
    void saveSettings();
    void loadSettings();

private:
    float tabIndicatorX=-1.f;
    bool replayListDirty=true, replayRefreshQueued=true, replayDirTimeValid=false;
    std::filesystem::file_time_type replayDirLastWriteTime{};
    char replayRenameBuffer[256]={0};
    std::string replayRenameOriginalName, replayRenameError;
    bool replayRenamePopupRequested=false, replayRenameFocusInput=false;
    bool replayActionPopupRequested=false;
    std::string replayActionMacroName;
    bool replayDeletePopupRequested=false;
    std::string replayDeleteName, replayDeleteError;
    bool replayActionIsBRR=false;

    int clickPackIndex=0, clickPackIndexP2=0;
    bool clickPacksScanned=false;

    void drawBackdrop();
    void drawAmbientWaves(ImDrawList* dl, ImVec2 panelMin, ImVec2 panelMax);
    void drawMainWindow();
    void drawMegaHackWindow();
    void drawCompactWindow();
    void drawRenderCompletePopup();
    void drawTitleBar();
    void drawTabBar();
    void drawTabContent();
    void drawStatusBar();
    void drawMainSubTabBar();
    void drawReplayTab();
    void drawToolsTab();
    void drawFrameWindowsTab();
    void drawRenderTab();
    void drawClicksTab();
    void drawAutoclickerTab();
    void drawSettingsTab();
    void drawCreditsTab();
    void drawHudTab();
    void drawMoreHacksTab();
    void drawIndicatorsTab();
    void drawJupiterTab();
    void drawJupiterClickTrainerPage();
    void drawTrainerTab();
    void drawTrainerClickTrainerPage();
    void loadRenderSettings();
    void markReplayListDirty(bool queueRefresh=true);
    void refreshReplayListIfNeeded(bool force);
    bool hasReplayDirectoryChanged() const;
    void captureReplayDirectoryTimestamp();
    void switchTab(int newTab);
};

std::string getKeyName(int code);

namespace Widgets {
    bool ToggleSwitch(const char* label, bool* value, ThemeEngine& theme, AnimationState& anim);
    bool StyledButton(const char* label, ImVec2 size, ThemeEngine& theme, AnimationState& anim, float roundingOverride=-1.f);
    bool StyledSliderFloat(const char* label, float* value, float min, float max, ThemeEngine& theme, bool allowManualInput=false);
    bool StyledSliderInt(const char* label, int* value, int min, int max, ThemeEngine& theme);
    void SectionHeader(const char* text, ThemeEngine& theme);
    bool ModuleCard(const char* name, const char* description, bool* enabled, ThemeEngine& theme, AnimationState& anim, int* keybind=nullptr);
    bool ModuleCardBegin(const char* name, const char* description, bool* enabled, ThemeEngine& theme, AnimationState& anim, int* keybind=nullptr);
    void ModuleCardEnd();
    void StatusBadge(const char* text, ImVec4 color);
    bool PillButton(const char* label, bool active, float width, ThemeEngine& theme, AnimationState& anim);
    void KeybindButton(const char* label, int* keyCode, ThemeEngine& theme, AnimationState& anim);
    void GucciQuote(const char* quote, const char* attr, ThemeEngine& theme);
}

void displayOverlayBranding();

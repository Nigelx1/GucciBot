#pragma once

#include <imgui-cocos.hpp>
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
    THEME_JUICE
};

class MenuInterface {
public:
    static MenuInterface* get();

    ImFont *fontBody=nullptr, *fontSmall=nullptr, *fontHeading=nullptr, *fontTitle=nullptr;
    bool shown=false, previouslyShown=false, setupComplete=false;

    int activeTab=0, previousTab=-1;
    int mainSubTab=0;

    BotTheme activeTheme = THEME_GUCCI;

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
    void drawHacksTab();
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

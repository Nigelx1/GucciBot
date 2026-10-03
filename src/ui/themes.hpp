#pragma once

// Themes: GucciBot's built-in rapper themes and the user's custom themes. For
// now a theme is its name, its macro file extension and its colours; the
// numbers persisted as "active_theme" are the ones the old menu used, so a
// saved choice keeps meaning the same theme.

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace gucci::ui::themes {

    struct Theme {
        int id = 0;
        std::string name;
        std::string extension;  // dot-prefixed
        std::string subtitle;
        ImVec4 accent{0.788f, 0.659f, 0.298f, 1.f};
        ImVec4 background{0.051f, 0.051f, 0.051f, 0.96f};
        ImVec4 card{0.078f, 0.078f, 0.078f, 1.f};
        ImVec4 text{0.941f, 0.910f, 0.816f, 1.f};
        ImVec4 textMuted{0.478f, 0.447f, 0.376f, 1.f};
        float radius = 5.f;
        float opacity = 0.96f;
        bool pulse = false;
        bool custom = false;
    };

    const std::vector<Theme>& builtins();
    const Theme* builtin(int id);

    // Custom themes found in customThemesDir() (name and extension).
    std::vector<Theme>& customs();
    void loadCustoms();
    std::filesystem::path customThemesDir();

    const Theme& active();
    int activeId();
    const std::string& activeCustomName();
    void setActive(int id);
    void setActiveCustom(const std::string& name);

} // namespace gucci::ui::themes

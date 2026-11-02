#pragma once

// Pictures for the menu's art: a PNG from the mod's resources made into a
// texture ImGui draws (ImDrawList::AddImage and the rest), the way imgui-cocos
// makes its font atlas one (a CCTexture2D from raw pixels), so it works on
// every platform. Main thread only, with GL current: from the menu's draw.
// Written fresh for JupiterBot's backdrop (2026-11-02).

#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cocos2d {
    class CCTexture2D;
}

namespace gucci::ui::art {

    class Texture {
    public:
        Texture() = default;
        ~Texture();
        Texture(Texture const&) = delete;
        Texture& operator=(Texture const&) = delete;

        // Reads `file` (a PNG, ui/png.hpp) from the mod's resources folder
        // into a texture, smooth when scaled and clamped at its edges. False,
        // with why, if it can't.
        bool load(std::string_view file, std::string& error);
        // Lets the texture go (its GL texture too). Safe when not loaded.
        void release();

        bool ready() const {
            return m_texture != nullptr;
        }
        int width() const {
            return m_width;
        }
        int height() const {
            return m_height;
        }
        // For ImGui: asked each time, so a texture cocos remade (a phone's
        // GL context coming back) is the one drawn.
        ImTextureID id() const;

    private:
        cocos2d::CCTexture2D* m_texture = nullptr;
        int m_width = 0;
        int m_height = 0;
        // Kept while the texture lives on Android, where cocos may upload it
        // again from these after the app comes back to the front.
        std::vector<uint8_t> m_keep;
    };

} // namespace gucci::ui::art

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

    // How Texture::load makes a picture into a texture. (Out here rather
    // than inside Texture: clang won't take a nested class with defaults as
    // a default argument of the class's own function.)
    struct LoadOptions {
        // The most pixels its longer side will be drawn across. A picture
        // twice that or more is halved first (png::fit), each new pixel the
        // average of four, until it isn't: bilinear filtering reads only four
        // pixels, so a picture drawn far smaller than it is comes out jagged
        // (a 256-pixel silhouette drawn 24 tall did). Mipmaps would fix that
        // too, but on Android cocos puts a texture back without them when the
        // app returns, and drawn with a mipmap filter it would come out black.
        // 0: as it is.
        int fit = 0;
        // Only its alpha counts: every pixel is made white, so drawn with a
        // tint it is exactly that color, with no dark fringe where its edge
        // meets the clear pixels round it (a packager may store those black).
        bool mask = false;
    };

    class Texture {
    public:
        Texture() = default;
        ~Texture();
        Texture(Texture const&) = delete;
        Texture& operator=(Texture const&) = delete;

        // Reads `file` (a PNG, ui/png.hpp) from the mod's resources folder
        // into a texture, smooth when scaled and clamped at its edges. False,
        // with why, if it can't.
        bool load(std::string_view file, std::string& error, LoadOptions const& options = {});
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
        // The picture's own size, before LoadOptions::fit halved it.
        int sourceWidth() const {
            return m_sourceWidth;
        }
        int sourceHeight() const {
            return m_sourceHeight;
        }
        // The part of the picture with anything drawn in it (png::drawnBounds),
        // as texture coordinates: left, top, right, bottom in x, y, z, w. All
        // of it, (0, 0, 1, 1), when nothing is.
        ImVec4 drawnUv() const {
            return m_drawn;
        }
        // For ImGui: asked each time, so a texture cocos remade (a phone's
        // GL context coming back) is the one drawn.
        ImTextureID id() const;
        // For a cocos sprite (CCSprite::createWithTexture), which holds on to
        // it. Keep this Texture loaded for as long as such a sprite may show:
        // on Android cocos may upload the texture again from the pixels this
        // keeps. Straight alpha, which a sprite blends right on its own.
        cocos2d::CCTexture2D* texture() const {
            return m_texture;
        }

    private:
        cocos2d::CCTexture2D* m_texture = nullptr;
        int m_width = 0;
        int m_height = 0;
        int m_sourceWidth = 0;
        int m_sourceHeight = 0;
        ImVec4 m_drawn{0.f, 0.f, 1.f, 1.f};
        // Kept while the texture lives on Android, where cocos may upload it
        // again from these after the app comes back to the front.
        std::vector<uint8_t> m_keep;
    };

} // namespace gucci::ui::art

// GucciBot's mark (ui/brand.hpp).

#include "ui/brand.hpp"
#include "ui/art.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace gucci::ui::brand {

    namespace {

        // White on clear, 256 x 256: the cone, its scoop and the cherry, with
        // gaps between the three (the art's dark outlines) so they stay apart
        // down to 24 pixels.
        constexpr char const* kSilhouette = "cone_silhouette_256.png";

        struct Mark {
            art::Texture tex;
            int halvings = -1;    // what png::fit did to the picture for the size it's drawn at
            bool failed = false;  // it couldn't load: nothing is drawn, and it isn't tried again every frame
        };
        // Never destroyed: at exit cocos may be gone before a static would
        // let its texture go (as with the backdrops' art).
        Mark& mark() {
            static Mark* m = new Mark();
            return *m;
        }

        // The whole logo with its rounded corners clear, 256 x 256.
        constexpr char const* kPauseLogo = "pause_button_256.png";

        // The pause button's logo: a texture for each time it's been halved
        // (png::fit) for the size it's drawn at, nine at most for 256 pixels.
        // Kept for the whole run, never let go: a pause menu's sprite holds
        // its texture, and on Android cocos may upload it again from the
        // pixels art::Texture keeps.
        struct PauseLogo {
            int halvings = 0;
            art::Texture tex;
        };
        std::vector<PauseLogo*>& pauseLogos() {
            static auto* logos = new std::vector<PauseLogo*>();
            return *logos;
        }
        int g_pauseSource = 0;       // the picture's longer side, once it has loaded
        bool g_pauseFailed = false;  // it couldn't load: the plain button for the rest of the run

    } // namespace

    cocos2d::CCSprite* pauseSprite(float points) {
        using namespace cocos2d;
        if (points <= 0.f || g_pauseFailed)
            return nullptr;
        // The pixels it's drawn across: the screen's for that many points (a
        // Mac's Retina ones too), or GD's own art's at its texture quality
        // (1, 2 or 4 a point) where that's more.
        auto* director = CCDirector::sharedDirector();
        float perPoint = director->getContentScaleFactor();
        if (auto* view = director->getOpenGLView(); view && director->getWinSize().height > 0.f)
            perPoint = std::max(perPoint, view->getFrameSize().height * geode::utils::getDisplayFactor() /
                                              director->getWinSize().height);
        int const need = static_cast<int>(std::ceil(points * perPoint));
        PauseLogo* logo = nullptr;
        if (g_pauseSource > 0)
            for (PauseLogo* made : pauseLogos())
                if (made->halvings == png::halvings(g_pauseSource, need))
                    logo = made;
        if (!logo) {
            auto* made = new PauseLogo();
            std::string error;
            if (!made->tex.load(kPauseLogo, error, {.fit = need})) {
                geode::log::error("[GucciBot] The pause button's logo: {}", error);
                delete made;  // nothing was made, so nothing for cocos to let go
                g_pauseFailed = true;
                return nullptr;
            }
            g_pauseSource = std::max(made->tex.sourceWidth(), made->tex.sourceHeight());
            made->halvings = png::halvings(g_pauseSource, need);
            pauseLogos().push_back(made);
            logo = made;
        }
        auto* sprite = CCSprite::createWithTexture(logo->tex.texture());
        if (!sprite)
            return nullptr;
        float const side = std::max(sprite->getContentSize().width, sprite->getContentSize().height);
        if (side > 0.f)
            sprite->setScale(points / side);
        return sprite;
    }

    float drawMark(ImDrawList* dl, ImVec2 at, float height, ImU32 tint) {
        Mark& m = mark();
        if (m.failed || !dl || height <= 0.f)
            return 0.f;
        // The pixels the whole picture spans: an ImGui unit is a pixel on a
        // computer (a Mac's Retina pixels too: imgui-cocos counts them) and
        // deviceScale() of them on a phone, and the cone is most of the
        // picture's height (drawnUv). Made again only when that changes how
        // often it's halved (Text size, Menu size).
        ImVec4 const uv = m.tex.drawnUv();
        float const share = m.tex.ready() ? std::max(0.05f, uv.w - uv.y) : 1.f;
        int const need = static_cast<int>(std::ceil(height * look().deviceScale() / share));
        int const side = std::max(m.tex.sourceWidth(), m.tex.sourceHeight());
        if (!m.tex.ready() || png::halvings(side, need) != m.halvings) {
            std::string error;
            if (!m.tex.load(kSilhouette, error, {.fit = need, .mask = true})) {
                geode::log::error("[GucciBot] The sidebar's mark: {}", error);
                m.failed = true;
                return 0.f;
            }
            m.halvings = png::halvings(std::max(m.tex.sourceWidth(), m.tex.sourceHeight()), need);
        }
        ImVec4 const drawn = m.tex.drawnUv();
        float const wide = (drawn.z - drawn.x) * static_cast<float>(m.tex.sourceWidth());
        float const tall = (drawn.w - drawn.y) * static_cast<float>(m.tex.sourceHeight());
        if (wide <= 0.f || tall <= 0.f)
            return 0.f;
        float const width = height * wide / tall;
        dl->AddImage(m.tex.id(), at, ImVec2(at.x + width, at.y + height), ImVec2(drawn.x, drawn.y),
                     ImVec2(drawn.z, drawn.w), tint);
        return width;
    }

} // namespace gucci::ui::brand

// GucciBot's mark (ui/brand.hpp).

#include "ui/brand.hpp"
#include "ui/art.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>

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

    } // namespace

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

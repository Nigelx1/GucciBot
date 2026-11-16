// Share my windows' picture (src/ui/share_draw.cpp) and the PNG writer it
// saves with (png::encode in src/ui/png.cpp). Run from the repo root
// (run_tests.bat).
//
// 1. png::encode: pictures of a few sizes, RGB and RGBA, decode (png::decode)
//    back to the same pixels, and a picture whose size doesn't match its
//    pixels is refused.
// 2. The card: drawn with the menu's fonts (resources/Roboto-*.ttf) and the
//    cone (resources/brand), with a score and without one, in a dark theme
//    and a light one, at 1600 x 900: the size is right, the accent runs along
//    the top, the panels are drawn in the surface color, there's text in
//    them (ink pixels), and a band's color is on it. Each is written to
//    build/share_sample_*.png to look at.
// 3. A card with no fonts is refused with a reason.

#include "ui/png.hpp"
#include "ui/share_draw.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace png = gucci::ui::png;
namespace share = gucci::ui::share;

namespace {

    int g_failed = 0;

    void fail(std::string const& what) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed;
    }

    std::vector<uint8_t> readFile(std::string const& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void checkEncode() {
        for (int keep = 0; keep < 2; ++keep)
            for (auto const [w, h] : {std::pair{1, 1}, {7, 3}, {64, 64}, {333, 17}}) {
                png::Pixels px;
                px.width = w;
                px.height = h;
                px.rgba.resize(static_cast<size_t>(w) * h * 4);
                unsigned seed = 12345u + w * 7u + h;
                for (size_t i = 0; i < px.rgba.size(); ++i) {
                    seed = seed * 1103515245u + 12345u;
                    px.rgba[i] = static_cast<uint8_t>(seed >> 16);
                }
                if (!keep)
                    for (size_t i = 3; i < px.rgba.size(); i += 4)
                        px.rgba[i] = 255;
                std::string error;
                auto const file = png::encode(px, keep != 0, error);
                if (file.empty()) {
                    fail("encode " + std::to_string(w) + "x" + std::to_string(h) + ": " + error);
                    continue;
                }
                png::Pixels back;
                if (!png::decode(file.data(), file.size(), back, error)) {
                    fail("decode what encode wrote: " + error);
                    continue;
                }
                if (back.width != w || back.height != h || back.rgba != px.rgba)
                    fail("encode/decode round trip changed the pixels at " + std::to_string(w) + "x" +
                         std::to_string(h) + (keep ? " RGBA" : " RGB"));
            }
        png::Pixels bad;
        bad.width = 4;
        bad.height = 4;
        bad.rgba.resize(10);
        std::string error;
        if (!png::encode(bad, false, error).empty() || error.empty())
            fail("encode took a picture whose size doesn't match its pixels");
    }

    share::Card sampleCard(bool light, bool score) {
        share::Card c;
        if (light) {
            c.theme.backdrop = {242, 242, 246};
            c.theme.surface = {255, 255, 255};
            c.theme.raised = {232, 232, 238};
            c.theme.ink = {20, 20, 28};
            c.theme.muted = {110, 110, 122};
            c.theme.line = {210, 210, 220};
            c.theme.accent = {40, 110, 230};
        }
        c.level = "KOCMOC UNLEASHED";
        c.details = "ID 87665224  \xC2\xB7  KOCMOC UNLEASHED New  \xC2\xB7  448 inputs  \xC2\xB7  240 TPS";
        c.bands = {{"1", {255, 60, 60, 255}, 9},    {"2", {255, 140, 40, 255}, 23},
                   {"3", {255, 220, 60, 255}, 41},  {"4-5", {120, 220, 90, 255}, 67},
                   {"6-10", {255, 255, 255, 255}, 84}};
        c.countedLine = share::grouped(224) + " windows counted";
        for (int i = 0; i < 8; ++i) {
            share::Tight t;
            t.number = 4 + i * 37;
            t.kind = i % 3 == 2 ? "P2 release" : "press";
            t.percent = 3.2f + i * 9.7f;
            t.frames = i < 2 ? "0.44" : "1";
            t.ms = (i < 2 ? 0.44 : 1.0) / 240.0 * 1000.0;
            t.color = {255, 60, 60, 255};
            c.tight.push_back(t);
        }
        c.haveScore = score;
        c.score = "123.62";
        c.scoreCaption = "precision to finish from 0% in about 24 hours";
        c.noScore = "No L* for these windows.";
        c.date = "2026-11-15";
        c.regularFont = readFile("resources/Roboto-Regular.ttf");
        c.boldFont = readFile("resources/Roboto-Bold.ttf");
        std::string error;
        auto const mark = readFile("resources/brand/cone_silhouette_256.png");
        if (!png::decode(mark.data(), mark.size(), c.mark, error))
            fail("the cone didn't decode: " + error);
        return c;
    }

    bool near(uint8_t const* p, share::Rgba c, int slack = 3) {
        return std::abs(p[0] - c.r) <= slack && std::abs(p[1] - c.g) <= slack && std::abs(p[2] - c.b) <= slack;
    }

    void checkCard(bool light, bool score, char const* name) {
        auto const card = sampleCard(light, score);
        if (card.regularFont.empty()) {
            fail("resources/Roboto-Regular.ttf missing (run from the repo root)");
            return;
        }
        png::Pixels px;
        std::string error;
        if (!share::draw(card, px, error)) {
            fail(std::string(name) + ": draw refused: " + error);
            return;
        }
        if (px.width != 1600 || px.height != 900 || px.rgba.size() != 1600u * 900u * 4u) {
            fail(std::string(name) + ": wrong size");
            return;
        }
        auto const at = [&](int x, int y) { return px.rgba.data() + (static_cast<size_t>(y) * px.width + x) * 4; };
        if (!near(at(800, 2), card.theme.accent))
            fail(std::string(name) + ": no accent along the top");
        if (!near(at(60, 24), card.theme.backdrop))
            fail(std::string(name) + ": the backdrop isn't the theme's");
        // Inside the bands panel, away from text: the surface.
        if (!near(at(600, 790), card.theme.surface))
            fail(std::string(name) + ": the bands panel isn't the surface color");
        // Text: ink pixels in the header and both panels; the first band's red.
        int inkHeader = 0, inkBands = 0, inkTight = 0, red = 0, accent = 0;
        for (int y = 0; y < 900; ++y)
            for (int x = 0; x < 1600; ++x) {
                auto const* p = at(x, y);
                if (near(p, card.theme.ink, 10)) {
                    if (y < 212 && x < 1100)
                        inkHeader++;
                    else if (y > 240 && y < 808 && x < 640)
                        inkBands++;
                    else if (y > 240 && y < 808 && x > 668)
                        inkTight++;
                }
                if (near(p, {255, 60, 60}, 4))
                    red++;
                if (y > 44 && y < 212 && x > 1132 && near(p, card.theme.accent, 6))
                    accent++;
            }
        if (inkHeader < 500 || inkBands < 300 || inkTight < 500)
            fail(std::string(name) + ": too little text (header " + std::to_string(inkHeader) + ", bands " +
                 std::to_string(inkBands) + ", tightest " + std::to_string(inkTight) + ")");
        if (red < 200)
            fail(std::string(name) + ": the first band's color isn't on it");
        if (score && accent < 300)
            fail(std::string(name) + ": the score isn't drawn in the accent");
        if (!score && accent > 50)
            fail(std::string(name) + ": accent in the score panel with no score");
        auto const file = png::encode(px, false, error);
        if (file.empty()) {
            fail(std::string(name) + ": encode: " + error);
            return;
        }
        std::string const out = std::string("build/share_sample_") + name + ".png";
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<char const*>(file.data()),
                                                   static_cast<std::streamsize>(file.size()));
        std::printf("  %s: %zu bytes -> %s\n", name, file.size(), out.c_str());
    }

} // namespace

int main() {
    checkEncode();
    checkCard(false, true, "dark");
    checkCard(true, true, "light");
    checkCard(false, false, "noscore");
    {
        share::Card none;
        png::Pixels px;
        std::string error;
        if (share::draw(none, px, error) || error.empty())
            fail("a card with no font was drawn");
    }
    if (g_failed) {
        std::printf("%d share check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("share: all checks passed\n");
    return 0;
}

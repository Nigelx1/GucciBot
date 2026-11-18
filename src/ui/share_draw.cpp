// Share my windows' picture (ui/share_draw.hpp), drawn in software.

#include "ui/share_draw.hpp"

// imgui's copy of stb_truetype, private to this file: imgui_draw.cpp compiles
// its own the same way (static), so the two never meet.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace gucci::ui::share {

    namespace {

        // ---- the canvas

        struct Canvas {
            int w = 0, h = 0;
            std::vector<uint8_t>& px;

            // `c` over the pixel at (x, y), `cover` of it (0 to 1).
            void blend(int x, int y, Rgba c, float cover) {
                if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0.f)
                    return;
                float const a = std::min(cover, 1.f) * (c.a / 255.f);
                uint8_t* d = px.data() + (static_cast<size_t>(y) * w + x) * 4;
                d[0] = static_cast<uint8_t>(std::lround(d[0] + (c.r - d[0]) * a));
                d[1] = static_cast<uint8_t>(std::lround(d[1] + (c.g - d[1]) * a));
                d[2] = static_cast<uint8_t>(std::lround(d[2] + (c.b - d[2]) * a));
                d[3] = 255;
            }

            void fill(Rgba c) {
                for (size_t i = 0; i < px.size(); i += 4) {
                    px[i] = c.r;
                    px[i + 1] = c.g;
                    px[i + 2] = c.b;
                    px[i + 3] = 255;
                }
            }
        };

        // How far (x, y) is outside a rounded rect: negative inside.
        float roundRectDistance(float x, float y, float x0, float y0, float x1, float y1, float r) {
            float const cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
            float const hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
            r = std::clamp(r, 0.f, std::min(hx, hy));
            float const qx = std::fabs(x - cx) - (hx - r);
            float const qy = std::fabs(y - cy) - (hy - r);
            float const ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
        }

        // A rounded rect, filled (stroke 0) or only a band `stroke` wide just
        // inside its edge, with soft (antialiased) edges.
        void roundRect(Canvas& cv, float x0, float y0, float x1, float y1, float r, Rgba c, float stroke = 0.f) {
            if (x1 <= x0 || y1 <= y0)
                return;
            int const ix0 = static_cast<int>(std::floor(x0)), iy0 = static_cast<int>(std::floor(y0));
            int const ix1 = static_cast<int>(std::ceil(x1)), iy1 = static_cast<int>(std::ceil(y1));
            for (int y = iy0; y < iy1; ++y)
                for (int x = ix0; x < ix1; ++x) {
                    float const d = roundRectDistance(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
                    float cover = std::clamp(0.5f - d, 0.f, 1.f);
                    if (stroke > 0.f)
                        cover -= std::clamp(0.5f - (d + stroke), 0.f, 1.f);
                    cv.blend(x, y, c, cover);
                }
        }

        void dot(Canvas& cv, float cx, float cy, float radius, Rgba c) {
            roundRect(cv, cx - radius, cy - radius, cx + radius, cy + radius, radius, c);
        }

        // ---- text

        // The next code point of UTF-8 `s` from `i` (moved past it); a broken
        // byte reads as '?'.
        uint32_t nextCodepoint(std::string_view s, size_t& i) {
            auto const b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
            uint8_t const c = b(i);
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (len == 0 || i + len > s.size()) {
                i++;
                return '?';
            }
            uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < len; ++k) {
                if ((b(i + k) & 0xC0) != 0x80) {
                    i++;
                    return '?';
                }
                cp = (cp << 6) | (b(i + k) & 0x3F);
            }
            i += len;
            return cp;
        }

        struct Face {
            stbtt_fontinfo info{};
            bool ok = false;
            int ascent = 0, descent = 0;

            bool load(std::vector<uint8_t> const& bytes) {
                if (bytes.empty())
                    return false;
                int const offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
                if (offset < 0 || !stbtt_InitFont(&info, bytes.data(), offset))
                    return false;
                int gap = 0;
                stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
                ok = true;
                return true;
            }

            float scale(float px) const { return stbtt_ScaleForPixelHeight(&info, px); }
            // How far a line's top is above its baseline, at `px`.
            float ascentAt(float px) const { return ascent * scale(px); }

            int glyph(uint32_t cp) const {
                int g = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
                if (g == 0 && cp != '?')
                    g = stbtt_FindGlyphIndex(&info, '?');
                return g;
            }

            float width(std::string_view text, float px) const {
                float const sc = scale(px);
                float x = 0.f;
                int prev = -1;
                for (size_t i = 0; i < text.size();) {
                    int const g = glyph(nextCodepoint(text, i));
                    if (prev >= 0)
                        x += stbtt_GetGlyphKernAdvance(&info, prev, g) * sc;
                    int advance = 0, lsb = 0;
                    stbtt_GetGlyphHMetrics(&info, g, &advance, &lsb);
                    x += advance * sc;
                    prev = g;
                }
                return x;
            }

            // Draws `text` with its line's top at `top`; returns its width.
            float draw(Canvas& cv, std::string_view text, float px, float x, float top, Rgba c) const {
                float const sc = scale(px);
                float const baseline = std::round(top + ascentAt(px));
                float pen = x;
                int prev = -1;
                std::vector<uint8_t> bitmap;
                for (size_t i = 0; i < text.size();) {
                    int const g = glyph(nextCodepoint(text, i));
                    if (prev >= 0)
                        pen += stbtt_GetGlyphKernAdvance(&info, prev, g) * sc;
                    float const shift = pen - std::floor(pen);
                    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    stbtt_GetGlyphBitmapBoxSubpixel(&info, g, sc, sc, shift, 0.f, &x0, &y0, &x1, &y1);
                    int const gw = x1 - x0, gh = y1 - y0;
                    if (gw > 0 && gh > 0) {
                        bitmap.assign(static_cast<size_t>(gw) * gh, 0);
                        stbtt_MakeGlyphBitmapSubpixel(&info, bitmap.data(), gw, gh, gw, sc, sc, shift, 0.f, g);
                        int const ox = static_cast<int>(std::floor(pen)) + x0;
                        int const oy = static_cast<int>(baseline) + y0;
                        for (int yy = 0; yy < gh; ++yy)
                            for (int xx = 0; xx < gw; ++xx)
                                if (uint8_t const v = bitmap[static_cast<size_t>(yy) * gw + xx])
                                    cv.blend(ox + xx, oy + yy, c, v / 255.f);
                    }
                    int advance = 0, lsb = 0;
                    stbtt_GetGlyphHMetrics(&info, g, &advance, &lsb);
                    pen += advance * sc;
                    prev = g;
                }
                return pen - x;
            }

            // `text`, cut short with an ellipsis until it fits in `maxWidth`.
            std::string fit(std::string text, float px, float maxWidth) const {
                if (width(text, px) <= maxWidth)
                    return text;
                std::string const dots = "\xE2\x80\xA6";
                while (!text.empty()) {
                    // Back one whole code point.
                    size_t cut = text.size() - 1;
                    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80)
                        cut--;
                    text.resize(cut);
                    while (!text.empty() && text.back() == ' ')
                        text.pop_back();
                    if (width(text + dots, px) <= maxWidth)
                        return text + dots;
                }
                return dots;
            }

            // The biggest size from `px` down to `smallest` at which `text`
            // fits in `maxWidth` (smallest if none does).
            float sizeToFit(std::string_view text, float px, float smallest, float maxWidth) const {
                while (px > smallest && width(text, px) > maxWidth)
                    px -= 1.f;
                return std::max(px, smallest);
            }

            // `text` wrapped at spaces into lines no wider than `maxWidth`,
            // at most `maxLines` of them (the last cut short if need be).
            std::vector<std::string> wrap(std::string const& text, float px, float maxWidth, int maxLines) const {
                std::vector<std::string> lines;
                std::string line;
                size_t i = 0;
                while (i <= text.size()) {
                    size_t const space = std::min(text.find(' ', i), text.size());
                    std::string const word = text.substr(i, space - i);
                    std::string const tried = line.empty() ? word : line + " " + word;
                    if (!line.empty() && width(tried, px) > maxWidth) {
                        lines.push_back(line);
                        line = word;
                    } else {
                        line = tried;
                    }
                    i = space + 1;
                }
                if (!line.empty())
                    lines.push_back(line);
                if (static_cast<int>(lines.size()) > maxLines) {
                    std::string rest = lines[static_cast<size_t>(maxLines) - 1];
                    for (size_t k = static_cast<size_t>(maxLines); k < lines.size(); ++k)
                        rest += " " + lines[k];
                    lines.resize(static_cast<size_t>(maxLines));
                    lines.back() = fit(rest, px, maxWidth);
                }
                return lines;
            }
        };

        // ---- the mark

        // The mark's drawn part, `height` pixels tall at (x, top), tinted `c`
        // (its own alpha is the shape). Returns how wide it was drawn.
        float drawMark(Canvas& cv, png::Pixels const& mark, float x, float top, float height, Rgba c) {
            int l = 0, t = 0, r = 0, b = 0;
            if (mark.width <= 0 || !png::drawnBounds(mark, l, t, r, b))
                return 0.f;
            int const sw = r - l, sh = b - t;
            int const dh = std::max(1, static_cast<int>(std::lround(height)));
            int const dw = std::max(1, static_cast<int>(std::lround(sw * height / sh)));
            double const step = static_cast<double>(sh) / dh;
            int const ox = static_cast<int>(std::lround(x)), oy = static_cast<int>(std::lround(top));
            for (int y = 0; y < dh; ++y) {
                int const sy0 = t + static_cast<int>(y * step);
                int const sy1 = std::max(sy0 + 1, t + static_cast<int>((y + 1) * step));
                for (int xx = 0; xx < dw; ++xx) {
                    int const sx0 = l + static_cast<int>(xx * step);
                    int const sx1 = std::max(sx0 + 1, l + static_cast<int>((xx + 1) * step));
                    unsigned sum = 0, n = 0;
                    for (int sy = sy0; sy < std::min(sy1, b); ++sy)
                        for (int sx = sx0; sx < std::min(sx1, r); ++sx) {
                            sum += mark.rgba[(static_cast<size_t>(sy) * mark.width + sx) * 4 + 3];
                            n++;
                        }
                    if (n)
                        cv.blend(ox + xx, oy + y, c, sum / (255.f * n));
                }
            }
            return static_cast<float>(dw);
        }

        // ---- colors

        Rgba mix(Rgba a, Rgba b, float t) {
            auto const m = [&](uint8_t x, uint8_t y) {
                return static_cast<uint8_t>(std::lround(x + (y - x) * std::clamp(t, 0.f, 1.f)));
            };
            return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
        }

        // A band's color drawn on the card: its own alpha flattened onto the
        // card, so a see-through band still shows.
        Rgba solid(Rgba c, Rgba under) {
            float const a = std::max(c.a / 255.f, 0.35f);
            return mix(under, {c.r, c.g, c.b, 255}, a);
        }

    } // namespace

    std::string grouped(long long n) {
        std::string digits = std::to_string(n < 0 ? -n : n);
        std::string out;
        int k = 0;
        for (size_t i = digits.size(); i-- > 0;) {
            out.insert(out.begin(), digits[i]);
            if (++k % 3 == 0 && i > 0)
                out.insert(out.begin(), ',');
        }
        return n < 0 ? "-" + out : out;
    }

    bool draw(Card const& card, png::Pixels& out, std::string& error) {
        if (card.width < 320 || card.height < 180 || card.width > 8192 || card.height > 8192) {
            error = "the picture's size is out of range";
            return false;
        }
        Face regular, bold;
        if (!regular.load(card.regularFont)) {
            error = "couldn't read the menu's font";
            return false;
        }
        if (!bold.load(card.boldFont))
            bold = regular;

        out.width = card.width;
        out.height = card.height;
        out.rgba.assign(static_cast<size_t>(card.width) * card.height * 4, 0);
        Canvas cv{card.width, card.height, out.rgba};

        Theme const& th = card.theme;
        float const k = card.width / 1600.f;  // everything below is laid out for 1600 x 900
        float const ky = card.height / 900.f;
        auto const X = [&](float v) { return v * k; };
        auto const Y = [&](float v) { return v * ky; };
        float const fs = std::min(k, ky);  // font scale
        float const radius = std::clamp(th.radius, 0.f, 16.f) * 2.2f * fs;
        float const edge = std::max(1.f, 1.5f * fs);

        cv.fill(th.backdrop);
        // The accent along the top edge, red under a warning (Card::warning).
        Rgba const warnRed{214, 48, 49};
        bool const warned = !card.warning.empty();
        roundRect(cv, 0.f, 0.f, static_cast<float>(card.width), Y(warned ? 12.f : 6.f), 0.f,
                  warned ? warnRed : th.accent);

        float const margin = X(48.f);
        float const right = card.width - margin;

        auto const panel = [&](float x0, float y0, float x1, float y1) {
            roundRect(cv, x0, y0, x1, y1, radius, th.surface);
            roundRect(cv, x0, y0, x1, y1, radius, th.line, edge);
        };

        // ---- header (left) and the score (right)
        float const scoreX0 = X(1132.f);
        float const headTop = Y(44.f), headBottom = Y(212.f);
        {
            float const maxW = scoreX0 - margin - X(32.f);
            float y = headTop + Y(4.f);
            if (warned) {
                // White on a red pill, in the kicker's place.
                float const px = bold.sizeToFit(card.warning, 20.f * fs, 14.f * fs, maxW - X(20.f));
                std::string const text = bold.fit(card.warning, px, maxW - X(20.f));
                float const w = bold.width(text, px);
                roundRect(cv, margin - X(10.f), y - Y(5.f), margin + w + X(10.f), y + 20.f * fs + Y(3.f),
                          Y(6.f), warnRed);
                bold.draw(cv, text, px, margin, y + (20.f * fs - px) * 0.5f, Rgba{255, 255, 255});
            } else {
                bold.draw(cv, card.kicker, 20.f * fs, margin, y, th.accent);
            }
            y += Y(32.f);
            float const px = bold.sizeToFit(card.level, 66.f * fs, 40.f * fs, maxW);
            std::string const level = bold.fit(card.level.empty() ? "Unknown level" : card.level, px, maxW);
            bold.draw(cv, level, px, margin, y + (66.f * fs - px) * 0.5f, th.ink);
            y += Y(84.f);
            regular.draw(cv, regular.fit(card.details, 24.f * fs, maxW), 24.f * fs, margin, y, th.muted);
        }
        {
            panel(scoreX0, headTop, right, headBottom);
            float const pad = X(28.f);
            float const inner = right - scoreX0 - pad * 2.f;
            float const x = scoreX0 + pad;
            float y = headTop + Y(18.f);
            if (card.haveScore) {
                // The label and the number on one baseline.
                float const labelPx = 30.f * fs;
                float const valueX = x + bold.width(card.scoreLabel, labelPx) + X(16.f);
                float const px = bold.sizeToFit(card.score, 72.f * fs, 36.f * fs, right - pad - valueX);
                float const valueTop = y + (72.f * fs - px) * 0.6f;
                float const baseline = valueTop + bold.ascentAt(px);
                bold.draw(cv, card.scoreLabel, labelPx, x, baseline - bold.ascentAt(labelPx), th.muted);
                bold.draw(cv, card.score, px, valueX, valueTop, th.accent);
                y += Y(88.f);
                auto const lines = regular.wrap(card.scoreCaption, 19.f * fs, inner, 2);
                for (auto const& line : lines) {
                    regular.draw(cv, line, 19.f * fs, x, y, th.muted);
                    y += Y(25.f);
                }
            } else {
                bold.draw(cv, card.scoreLabel, 26.f * fs, x, y, th.muted);
                y += Y(40.f);
                auto const lines = regular.wrap(card.noScore, 20.f * fs, inner, 3);
                for (auto const& line : lines) {
                    regular.draw(cv, line, 20.f * fs, x, y, th.ink);
                    y += Y(27.f);
                }
            }
        }

        // ---- the bands
        float const bodyTop = Y(240.f), bodyBottom = Y(808.f);
        float const split = X(640.f), gap = X(28.f);
        float const pad = X(32.f);
        {
            float const x0 = margin, x1 = split;
            panel(x0, bodyTop, x1, bodyBottom);
            bold.draw(cv, "Windows", 28.f * fs, x0 + pad, bodyTop + Y(26.f), th.ink);
            float const top = bodyTop + Y(86.f);
            float const bottom = bodyBottom - Y(64.f);
            size_t const n = card.bands.size();
            if (n == 0) {
                regular.draw(cv, "No bands to count them in.", 22.f * fs, x0 + pad, top, th.muted);
            } else {
                float const rowH = std::clamp((bottom - top) / n, Y(26.f), Y(60.f));
                float const px = std::clamp(rowH * 0.46f, 14.f * fs, 25.f * fs);
                int most = 1;
                for (auto const& b : card.bands)
                    most = std::max(most, b.count);
                float nameW = 0.f, countW = 0.f;
                for (auto const& b : card.bands) {
                    nameW = std::max(nameW, regular.width(b.name, px));
                    countW = std::max(countW, bold.width(grouped(b.count), px));
                }
                nameW = std::min(nameW, X(170.f));
                float const sw = px * 0.42f;  // the swatch's radius
                float const nameX = x0 + pad + sw * 2.f + X(14.f);
                float const barX0 = nameX + nameW + X(18.f);
                float const barX1 = x1 - pad - countW - X(18.f);
                for (size_t i = 0; i < n; ++i) {
                    auto const& b = card.bands[i];
                    float const rowTop = top + rowH * i;
                    float const mid = rowTop + rowH * 0.5f;
                    float const textTop = mid - px * 0.62f;
                    Rgba const c = solid(b.color, th.surface);
                    dot(cv, x0 + pad + sw, mid, sw, c);
                    dot(cv, x0 + pad + sw, mid, sw, th.line);  // a thin ring so white shows on white
                    dot(cv, x0 + pad + sw, mid, sw - edge, c);
                    regular.draw(cv, regular.fit(b.name, px, nameW), px, nameX, textTop, th.ink);
                    float const barH = std::max(4.f, rowH * 0.22f);
                    if (barX1 > barX0) {
                        roundRect(cv, barX0, mid - barH * 0.5f, barX1, mid + barH * 0.5f, barH * 0.5f, th.raised);
                        if (b.count > 0) {
                            float const len = std::max(barH, (barX1 - barX0) * b.count / most);
                            roundRect(cv, barX0, mid - barH * 0.5f, barX0 + len, mid + barH * 0.5f, barH * 0.5f, c);
                            // Its edge in the line color, so a white band shows on a white card.
                            roundRect(cv, barX0, mid - barH * 0.5f, barX0 + len, mid + barH * 0.5f, barH * 0.5f,
                                      th.line, edge * 0.75f);
                        }
                    }
                    std::string const count = grouped(b.count);
                    bold.draw(cv, count, px, x1 - pad - bold.width(count, px), textTop, b.count ? th.ink : th.muted);
                }
            }
            regular.draw(cv, regular.fit(card.countedLine, 21.f * fs, x1 - x0 - pad * 2.f), 21.f * fs, x0 + pad,
                         bodyBottom - Y(50.f), th.muted);
        }

        // ---- the tightest windows
        {
            float const x0 = split + gap, x1 = right;
            panel(x0, bodyTop, x1, bodyBottom);
            bold.draw(cv, "Tightest windows", 28.f * fs, x0 + pad, bodyTop + Y(26.f), th.ink);
            float const colClick = x0 + pad;
            float const colWhere = x0 + pad + X(330.f);
            float const colWindow = x0 + pad + X(500.f);
            float const colTimeR = x1 - pad;
            float const headPx = 18.f * fs;
            float const headY = bodyTop + Y(86.f);
            regular.draw(cv, "CLICK", headPx, colClick, headY, th.muted);
            regular.draw(cv, "WHERE", headPx, colWhere, headY, th.muted);
            regular.draw(cv, "WINDOW", headPx, colWindow, headY, th.muted);
            regular.draw(cv, "TIME", headPx, colTimeR - regular.width("TIME", headPx), headY, th.muted);
            float const top = headY + Y(34.f);
            float const bottom = bodyBottom - Y(24.f);
            size_t const n = card.tight.size();
            if (n == 0) {
                regular.draw(cv, "No windows counted.", 22.f * fs, colClick, top + Y(12.f), th.muted);
            } else {
                float const rowH = std::min(Y(57.f), (bottom - top) / n);
                float const px = std::clamp(rowH * 0.44f, 14.f * fs, 25.f * fs);
                for (size_t i = 0; i < n; ++i) {
                    auto const& t = card.tight[i];
                    float const rowTop = top + rowH * i;
                    if (i > 0)
                        roundRect(cv, colClick, rowTop, colTimeR, rowTop + edge, 0.f, th.line);
                    float const textTop = rowTop + rowH * 0.5f - px * 0.62f;
                    std::string const num = "#" + grouped(t.number);
                    float const nw = bold.draw(cv, num, px, colClick, textTop, th.ink);
                    regular.draw(cv, regular.fit(t.kind, px * 0.85f, colWhere - colClick - nw - X(30.f)), px * 0.85f,
                                 colClick + nw + X(12.f), textTop + px * 0.1f, th.muted);
                    char where[32];
                    std::snprintf(where, sizeof where, "%.1f%%", t.percent);
                    regular.draw(cv, where, px, colWhere, textTop, th.ink);
                    Rgba const c = solid(t.color, th.surface);
                    float const sw = px * 0.3f;
                    float const mid = rowTop + rowH * 0.5f;
                    dot(cv, colWindow + sw, mid, sw, th.line);
                    dot(cv, colWindow + sw, mid, sw - edge, c);
                    bool const one = t.frames == "1";
                    std::string const frames = t.frames + (one ? " frame" : " frames");
                    bold.draw(cv, frames, px, colWindow + sw * 2.f + X(12.f), textTop, th.ink);
                    char ms[32];
                    std::snprintf(ms, sizeof ms, t.ms < 10.0 ? "%.2f ms" : "%.1f ms", t.ms);
                    regular.draw(cv, ms, px, colTimeR - regular.width(ms, px), textTop, th.ink);
                }
            }
        }

        // ---- footer
        {
            float const top = Y(838.f);
            float const h = Y(30.f);
            float x = margin;
            float const markW = drawMark(cv, card.mark, x, top, h, th.accent);
            if (markW > 0.f)
                x += markW + X(12.f);
            float const px = 21.f * fs;
            float const textTop = top + h * 0.5f - px * 0.6f;
            regular.draw(cv, card.footer, px, x, textTop, th.muted);
            if (!card.date.empty())
                regular.draw(cv, card.date, px, right - regular.width(card.date, px), textTop, th.muted);
        }
        return true;
    }

} // namespace gucci::ui::share

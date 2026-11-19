#pragma once

// GucciBot (2026-11-15): the picture Share my windows saves (ui/share.hpp),
// drawn here in software: a 1600 x 900 card in the menu's look with a
// macro's frame windows on it, for Discord or YouTube. Nigel's ask
// (2026-11-11): the level name, the tightest windows and the precision score.
//
// No Geode, no GL and no ImGui here, so it runs on any thread (the game's
// stays free while it draws) and the offline tests draw the same picture
// (tools/run_share_tests.cpp). Text is drawn with stb_truetype (imgui's
// copy, compiled into this file only) from the font files' bytes.

#include "ui/png.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gucci::ui::share {

    struct Rgba {
        uint8_t r = 0, g = 0, b = 0, a = 255;
    };

    // The theme's colors, as the menu has them (ui/look.hpp's Palette).
    struct Theme {
        Rgba backdrop{14, 14, 18}, surface{26, 26, 32}, raised{38, 38, 46};
        Rgba ink{235, 235, 240}, muted{150, 150, 162}, line{56, 56, 66};
        Rgba accent{212, 175, 55}, onAccent{20, 20, 20};
        float radius = 6.f;  // the menu's corner radius, in its own units
    };

    // One band of the in-level legend and how many windows it holds.
    struct Band {
        std::string name;
        Rgba color;
        int count = 0;
    };

    // One of the tightest windows.
    struct Tight {
        int number = 0;      // the click's number (presses so far)
        std::string kind;    // "press", "P2 release", ...
        float percent = 0.f; // how far through the level, 0 to 100
        std::string frames;  // "1", "0.44", "12+"
        double ms = 0.0;     // the window in milliseconds
        Rgba color;          // its band's color
    };

    struct Card {
        int width = 1600;
        int height = 900;
        Theme theme;

        std::string kicker = "FRAME WINDOWS";  // the small line over the level name
        std::string level;                     // the level's name
        std::string details;                   // "ID 1234 · macro name · 448 inputs · 240 TPS"
        // GucciBot (2026-11-17): set when the windows aren't legit (GD 2.2091's
        // Ignore Damage was on while they were counted): drawn in red in the
        // kicker's place, with the top edge red, so a picture can't pass for
        // a legit one.
        std::string warning;

        std::vector<Band> bands;
        std::string countedLine;  // "1,234 windows counted"
        std::vector<Tight> tight;

        bool haveScore = false;
        std::string scoreLabel = "L*";
        std::string score;         // "123.62"
        std::string scoreCaption;  // "precision to finish from 0% in about 24 hours"
        std::string noScore;       // what the panel says when there's no score

        std::string footer = "Made with GucciBot";
        std::string date;  // bottom right

        // The fonts' files (Roboto Regular and Bold), and the mark drawn
        // before the footer (white on clear, tinted with the accent); an
        // empty mark draws none.
        std::vector<uint8_t> regularFont;
        std::vector<uint8_t> boldFont;
        png::Pixels mark;
    };

    // Draws `card` into `out` (RGBA, opaque, top row first). False, with why,
    // when a font won't load or the size is unusable.
    bool draw(Card const& card, png::Pixels& out, std::string& error);

    // `n` with thousands separators: 1234 -> "1,234".
    std::string grouped(long long n);

} // namespace gucci::ui::share

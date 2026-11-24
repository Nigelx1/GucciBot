#pragma once

// Which Calculate band a window goes in (FrameWindowAnalyzer::tierForValue,
// which the markers, the legend, Copy info and Share my windows' picture all
// go through). Pure, so tools/run_bandpick_tests.cpp checks it outside the
// game.
//
// GucciBot (2026-10-23): a band's edges can be decimals, like 0.3-0.5 for
// sub-tick (CBF) windows (anticroom's antics-counter beta.5). A band whose
// edges are both whole numbers holds whole windows, as it always did: `whole`
// is the window's whole count (a sub-tick 1.3 counts as 1). A band with a
// decimal edge holds `shown`, the value as the marker prints it (rounded by
// the caller to the places it prints, so two markers that both read 0.50 land
// in the same band).
//
// Nigel's D13 (2026-11-22): once a band has a decimal edge, a sub-tick value
// can fall in a gap between two bands (bands 1.5-2 and 3: 2.02, 2.29 and 2.38
// landed in no band in the 11-18 test pass), and it got no marker color, no
// legend count and no Copy info count. Such a value now counts in the band
// below it, rounding down as a whole band takes a sub-tick 1.3 as 1, so no
// click goes missing. Only sub-tick values (a fraction as printed) and only
// between two bands: a whole window, or a value above the top band or below
// the lowest, lands where it did before.

#include <cmath>
#include <cstddef>

namespace gucci::bandpick {

    struct Edges {
        float min = 0.f;
        float max = 0.f;
    };

    // An edge is a float: 0.3 is stored a hair off 0.3, so allow a hair.
    constexpr double kEdgeSlack = 1e-5;

    inline bool wholeBand(Edges e) {
        return e.min == std::floor(e.min) && e.max == std::floor(e.max);
    }

    // The band's own rule.
    inline bool holds(Edges e, int whole, double shown) {
        if (wholeBand(e)) {
            double const w = whole;
            return w >= e.min && w <= e.max;
        }
        return shown >= e.min - kEdgeSlack && shown <= e.max + kEdgeSlack;
    }

    // The window is past the band's top, by the band's own rule.
    inline bool pastTop(Edges e, int whole, double shown) {
        if (wholeBand(e))
            return static_cast<double>(whole) > e.max;
        return shown > e.max + kEdgeSlack;
    }

    // The window is short of the band's bottom, by the band's own rule.
    inline bool shortOfBottom(Edges e, int whole, double shown) {
        if (wholeBand(e))
            return static_cast<double>(whole) < e.min;
        return shown < e.min - kEdgeSlack;
    }

    // The band for a window among `n` bands, edgeAt(i) giving band i's edges:
    // the first that holds it, in the list's order; else, for a sub-tick value
    // sitting between a band below it and a band above it, the band below (the
    // one with the highest top under it; the first of those on a tie); else -1.
    template <class EdgeAt>
    int pick(size_t n, EdgeAt&& edgeAt, int whole, double shown) {
        for (size_t i = 0; i < n; i++)
            if (holds(edgeAt(i), whole, shown))
                return static_cast<int>(i);
        if (shown == std::floor(shown))
            return -1;  // a whole window: no band, as before
        int below = -1;
        float belowTop = 0.f;
        bool above = false;
        for (size_t i = 0; i < n; i++) {
            Edges const e = edgeAt(i);
            if (shortOfBottom(e, whole, shown)) {
                above = true;
            } else if (pastTop(e, whole, shown) && (below < 0 || e.max > belowTop)) {
                below = static_cast<int>(i);
                belowTop = e.max;
            }
        }
        return above ? below : -1;
    }

} // namespace gucci::bandpick

// Possible positions' math: see positions_core.hpp. Written fresh for
// GucciBot (2026-11-19); the idea is NaN GD's.

#include "tools/positions_core.hpp"

#include <algorithm>
#include <cmath>

namespace gucci::positions {

    Window windowFor(bool usable, int64_t low, int64_t high, bool cbf, double slotsPerTick) {
        Window w;
        if (!usable || low > high || (cbf && !(slotsPerTick > 0.0)))
            return w;
        auto toTicks = [&](int64_t v) -> int64_t {
            if (!cbf)
                return v;
            // Toward zero: a sub-tick edge between two ticks goes to the one
            // nearer the input.
            return static_cast<int64_t>(std::trunc(static_cast<double>(v) / slotsPerTick));
        };
        int64_t lo = std::min<int64_t>(toTicks(low), 0);
        int64_t hi = std::max<int64_t>(toTicks(high), 0);
        // A window wider than this is a count that ran away; the copies
        // wouldn't hold a meaningful path that far anyway.
        constexpr int64_t kFar = 240;
        w.low = static_cast<int>(std::max<int64_t>(lo, -kFar));
        w.high = static_cast<int>(std::min<int64_t>(hi, kFar));
        w.fromMark = true;
        return w;
    }

    std::vector<int> pickShifts(Window const& w, int maxShifts) {
        int const lo = std::min(w.low, 0);
        int const hi = std::max(w.high, 0);
        std::vector<int> out;
        int const count = hi - lo + 1;
        maxShifts = std::max(maxShifts, 3);
        if (count <= maxShifts) {
            for (int s = lo; s <= hi; ++s)
                out.push_back(s);
            return out;
        }
        // Evenly over the window, its two ends and 0 among them.
        for (int k = 0; k < maxShifts; ++k) {
            double const t = static_cast<double>(k) / static_cast<double>(maxShifts - 1);
            out.push_back(lo + static_cast<int>(std::lround(t * (hi - lo))));
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        if (!std::binary_search(out.begin(), out.end(), 0)) {
            // The one nearest 0 becomes 0 (never an end: lo < 0 < hi here).
            size_t best = 1;
            for (size_t k = 1; k + 1 < out.size(); ++k)
                if (std::abs(out[k]) < std::abs(out[best]))
                    best = k;
            out[best] = 0;
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
        return out;
    }

    bool shiftAllowed(std::vector<Input> const& inputs, size_t i, int shift, uint32_t now) {
        if (i >= inputs.size())
            return false;
        int64_t const at = static_cast<int64_t>(inputs[i].frame) + shift;
        if (at <= static_cast<int64_t>(now))
            return false;
        if (shift < 0 && i > 0 && at <= static_cast<int64_t>(inputs[i - 1].frame))
            return false;
        if (shift > 0 && i + 1 < inputs.size() && at >= static_cast<int64_t>(inputs[i + 1].frame))
            return false;
        return true;
    }

    uint32_t horizonFor(std::vector<Input> const& inputs, size_t i, int pastNext, int cap) {
        if (i >= inputs.size())
            return 0;
        uint32_t const at = inputs[i].frame;
        uint32_t const capped = at + static_cast<uint32_t>(std::max(cap, 1));
        uint32_t h = at + static_cast<uint32_t>(std::max(pastNext, 1));
        if (i + 1 < inputs.size())
            h = inputs[i + 1].frame + static_cast<uint32_t>(std::max(pastNext, 0));
        return std::min(h, capped);
    }

    std::vector<std::pair<int, bool>> scriptFor(std::vector<Input> const& inputs, size_t i, int shift,
                                                uint32_t now, uint32_t horizon) {
        std::vector<std::pair<int, bool>> out;
        for (size_t k = 0; k < inputs.size(); ++k) {
            int64_t f = inputs[k].frame;
            if (k == i)
                f += shift;
            if (f <= static_cast<int64_t>(now) || f > static_cast<int64_t>(horizon))
                continue;
            out.push_back({static_cast<int>(f - static_cast<int64_t>(now)), inputs[k].press});
        }
        std::stable_sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        return out;
    }

    int countedPoints(int lived, int ticks, uint32_t now, uint32_t nextFrame) {
        if (ticks <= 0 || lived < 0)
            return -1;
        if (lived >= ticks)
            return ticks;
        // The next input beyond this run's end is no next input for it.
        if (nextFrame <= now || nextFrame > now + static_cast<uint32_t>(ticks))
            return -1;
        int const toNext = static_cast<int>(nextFrame - now);
        if (lived < toNext)
            return -1;
        return toNext;
    }

    Lane envelope(std::vector<Run> const& runs, size_t nominal, EnvelopeOptions const& opt) {
        Lane lane;
        if (nominal >= runs.size() || runs[nominal].points.empty())
            return lane;
        Run const& nom = runs[nominal];
        size_t const n = nom.points.size();
        std::vector<Sample> s(n);
        for (size_t k = 0; k < n; ++k)
            s[k] = {nom.points[k].x, nom.points[k].y, nom.points[k].y};
        lane.runs = 1;
        for (size_t r = 0; r < runs.size(); ++r) {
            if (r == nominal || runs[r].points.empty())
                continue;
            lane.runs++;
            Run const& run = runs[r];
            for (size_t k = 0; k < n; ++k) {
                int64_t const frame = static_cast<int64_t>(nom.firstFrame) + static_cast<int64_t>(k);
                int64_t const at = frame - static_cast<int64_t>(run.firstFrame);
                if (at < 0)
                    continue;
                if (at >= static_cast<int64_t>(run.points.size()))
                    break;
                Point const& p = run.points[static_cast<size_t>(at)];
                if (std::fabs(p.x - s[k].x) > opt.maxXGap) {
                    lane.xBreaks++;
                    break;
                }
                s[k].yMin = std::min(s[k].yMin, p.y);
                s[k].yMax = std::max(s[k].yMax, p.y);
            }
        }
        size_t first = n, last = 0;
        for (size_t k = 0; k < n; ++k) {
            if (s[k].width() > opt.minWidth) {
                first = std::min(first, k);
                last = k;
            }
        }
        if (first == n)
            return Lane{};
        first = first > 0 ? first - 1 : 0;
        last = std::min(last + 1, n - 1);
        lane.startFrame = nom.firstFrame + static_cast<uint32_t>(first);
        lane.samples.assign(s.begin() + static_cast<std::ptrdiff_t>(first),
                            s.begin() + static_cast<std::ptrdiff_t>(last + 1));
        lane.x0 = lane.x1 = lane.samples.front().x;
        lane.y0 = lane.samples.front().yMin;
        lane.y1 = lane.samples.front().yMax;
        for (auto const& v : lane.samples) {
            lane.x0 = std::min(lane.x0, v.x);
            lane.x1 = std::max(lane.x1, v.x);
            lane.y0 = std::min(lane.y0, v.yMin);
            lane.y1 = std::max(lane.y1, v.yMax);
            lane.maxWidth = std::max(lane.maxWidth, v.width());
        }
        return lane;
    }

    std::vector<Sample> thin(std::vector<Sample> const& s, size_t from, size_t to, float minStep) {
        std::vector<Sample> out;
        to = std::min(to, s.size());
        if (from >= to)
            return out;
        out.push_back(s[from]);
        for (size_t k = from + 1; k < to; ++k) {
            Sample const& v = s[k];
            bool const lastOne = k + 1 == to;
            if (!lastOne && std::fabs(v.x - out.back().x) < minStep) {
                out.back().yMin = std::min(out.back().yMin, v.yMin);
                out.back().yMax = std::max(out.back().yMax, v.yMax);
                continue;
            }
            out.push_back(v);
        }
        return out;
    }

    std::pair<size_t, size_t> visibleRange(std::vector<Sample> const& s, float x0, float x1) {
        size_t from = s.size(), to = 0;
        for (size_t k = 0; k < s.size(); ++k) {
            if (s[k].x >= x0 && s[k].x <= x1) {
                from = std::min(from, k);
                to = k + 1;
            }
        }
        if (from >= to)
            return {0, 0};
        from = from > 0 ? from - 1 : 0;
        to = std::min(to + 1, s.size());
        return {from, to};
    }

    std::vector<Quad> fillQuads(std::vector<Sample> const& s, float inset, float minFill) {
        std::vector<Quad> out;
        if (s.size() < 2)
            return out;
        out.reserve(s.size() - 1);
        auto side = [inset](Sample const& v, float& lo, float& hi) {
            lo = v.yMin + inset;
            hi = v.yMax - inset;
            if (lo > hi)
                lo = hi = (v.yMin + v.yMax) * 0.5f;
        };
        for (size_t k = 0; k + 1 < s.size(); ++k) {
            Sample const& l = s[k];
            Sample const& r = s[k + 1];
            if (l.width() < minFill && r.width() < minFill)
                continue;
            float l0, l1, r0, r1;
            side(l, l0, l1);
            side(r, r0, r1);
            out.push_back({{l.x, l0}, {r.x, r0}, {r.x, r1}, {l.x, l1}});
        }
        return out;
    }

} // namespace gucci::positions

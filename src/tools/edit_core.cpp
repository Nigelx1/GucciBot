// The Frame Editor's rules (see edit_core.hpp for the contract). Ported from
// Absense's macro editor (replay/edit_core.cpp, by Absent; GPL via Silicate):
// the click model, the lane and attempt rules behind the ranges, and where an
// inserted action goes among the actions on its frame. Rewritten over
// GucciBot's gb::Action, with whole-list drafts in place of Absense's hunks.

#include "tools/edit_core.hpp"

#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <utility>

namespace gucci::edit {

    namespace {

        using T = gb::ActionType;

        bool isInput(gb::Action const& x) {
            return macrocheck::laneOf(x) >= 0;
        }
        bool isPress(gb::Action const& x) {
            return isInput(x) && x.m_holding;
        }

        std::string num(int64_t v) {
            return std::to_string(v);
        }
        std::string signedNum(int64_t v) {
            return v > 0 ? "+" + std::to_string(v) : std::to_string(v);
        }

        size_t lowerIndex(Actions const& a, uint32_t t) {
            return static_cast<size_t>(
                std::lower_bound(a.begin(), a.end(), t, [](gb::Action const& x, uint32_t v) { return x.m_frame < v; }) -
                a.begin());
        }

        // ------------------------------------------------------------ wording

        const char* resetNoun(T t) {
            switch (t) {
                case T::Death: return "death";
                case T::Restart: return "restart";
                case T::RestartFull: return "full restart";
                default: return "reset";
            }
        }

        // What reset sits on `frame` (the first one, if there are several).
        const char* resetNounAt(Actions const& a, uint32_t frame) {
            for (size_t i = lowerIndex(a, frame); i < a.size() && a[i].m_frame == frame; ++i)
                if (macrocheck::isReset(a[i]))
                    return resetNoun(a[i].m_type);
            return "reset";
        }

        std::string itemNoun(Item const& it) {
            switch (it.kind) {
                case Kind::Click: return "click";
                case Kind::Press: return "press";
                case Kind::Release: return "release";
            }
            return "input";
        }

        // "the P1 Jump click", "3 clicks", "4 inputs".
        std::string describe(Model const& m, std::vector<uint32_t> const& items) {
            if (items.size() == 1) {
                Item const& it = m.items[items[0]];
                return std::string("the ") + laneName(it.lane) + " " + itemNoun(it);
            }
            bool allClicks = true;
            for (uint32_t i : items)
                allClicks = allClicks && m.items[i].kind == Kind::Click;
            return num(static_cast<int64_t>(items.size())) + (allClicks ? " clicks" : " inputs");
        }

        std::string prevWhy(Actions const& a, Model const& m, Item const& p) {
            std::string const who = laneName(p.lane);
            switch (p.kind) {
                case Kind::Click: return "the " + who + " click before it ends at " + num(lastFrame(a, p));
                case Kind::Press: {
                    uint32_t const end = holdEnd(a, m, p);
                    if (end == kLastFrame)
                        return "the " + who + " press at " + num(firstFrame(a, p)) + " before it is never released";
                    return "the " + who + " press at " + num(firstFrame(a, p)) + " before it is held until " + num(end);
                }
                case Kind::Release: return "the " + who + " release before it is at " + num(firstFrame(a, p));
            }
            return "";
        }

        std::string nextWhy(Actions const& a, Item const& n) {
            std::string const who = laneName(n.lane);
            switch (n.kind) {
                case Kind::Click: return "the next " + who + " click starts at " + num(firstFrame(a, n));
                case Kind::Press: return "the next " + who + " press is at " + num(firstFrame(a, n));
                case Kind::Release: return "the next " + who + " release is at " + num(firstFrame(a, n));
            }
            return "";
        }

        // ------------------------------------------------------------ attempts
        //
        // A death or restart in the macro is a wall: playback lets go of every
        // button there, so a click moved across one would be cut in two, and
        // Absense found nothing on a reset's own frame plays. GucciBot's
        // playback order on that frame is not Silicate's (macro_check.hpp says
        // why it is not established), so the editor stays on the safe side and
        // never puts an input on a reset's frame either.

        struct Attempt {
            int64_t lo = kFirstFrame, hi = kLastFrame;
            bool hasBefore = false, hasAfter = false;
            uint32_t resetBefore = 0, resetAfter = 0;
        };

        Attempt attemptAround(Model const& m, uint64_t frame) {
            Attempt at;
            auto it = std::lower_bound(m.resetFrames.begin(), m.resetFrames.end(), frame);
            if (it != m.resetFrames.begin()) {
                at.hasBefore = true;
                at.resetBefore = *(it - 1);
                at.lo = std::max<int64_t>(at.lo, static_cast<int64_t>(at.resetBefore) + 1);
            }
            if (it != m.resetFrames.end()) {
                at.hasAfter = true;
                at.resetAfter = *it;
                at.hi = static_cast<int64_t>(at.resetAfter) - 1;
            }
            return at;
        }

        // Whether a reset on a[i]'s own frame comes before it in the list. The
        // recorder writes what is let go of at a respawn after the death on
        // that frame, so such an input belongs to the attempt the reset
        // starts, not the one it ends (Absense's afterResetOnTick).
        bool afterResetOnFrame(Actions const& a, uint32_t i) {
            uint32_t const t = a[i].m_frame;
            for (size_t j = lowerIndex(a, t); j < i; ++j)
                if (macrocheck::isReset(a[j]))
                    return true;
            return false;
        }

        Attempt itemAttempt(Actions const& a, Model const& m, Item const& it) {
            uint32_t const t = firstFrame(a, it);
            return attemptAround(m, afterResetOnFrame(a, it.key()) ? uint64_t(t) + 1 : t);
        }

        std::string attemptStartWhy(Actions const& a, uint32_t r) {
            return std::string("the ") + resetNounAt(a, r) + " at " + num(r) + " comes before it";
        }
        std::string attemptEndWhy(Actions const& a, uint32_t r) {
            return std::string("the ") + resetNounAt(a, r) + " at " + num(r) + " ends its attempt";
        }

        void raiseLo(Range& r, int64_t lo, std::string why) {
            if (lo > r.lo) {
                r.lo = lo;
                r.loWhy = std::move(why);
            }
        }
        void lowerHi(Range& r, int64_t hi, std::string why) {
            if (hi < r.hi) {
                r.hi = hi;
                r.hiWhy = std::move(why);
            }
        }

        size_t lanePos(Model const& m, Item const& it, uint32_t itemIndex) {
            auto const& lane = m.lanes[static_cast<size_t>(it.lane)];
            return static_cast<size_t>(std::lower_bound(lane.begin(), lane.end(), itemIndex) - lane.begin());
        }

        // Sorted, unique item indices for `keys`; unknown keys are dropped.
        std::vector<uint32_t> itemsOf(Model const& m, std::span<uint32_t const> keys) {
            std::vector<uint32_t> out;
            out.reserve(keys.size());
            for (uint32_t k : keys) {
                uint32_t const it = itemOfKey(m, k);
                if (it != kNone)
                    out.push_back(it);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        // ------------------------------------------------------------ assembly
        //
        // Absense's Builder, cut down: the actions kept never change order,
        // and each inserted one goes into its frame's group by class. A
        // release goes first on its frame and a press last, so a release and a
        // press of one lane on the same frame always read as a re-click (the
        // only way the lane rule lets two items touch). A press and release on
        // one frame (a tap) go in together, ahead of a press already there
        // that starts a hold, so the tap does not land inside that hold.

        enum class Place : uint8_t { Start, End, Tap, Chain };

        struct Ins {
            gb::Action act;
            Place place = Place::End;
        };

        struct Built {
            Actions out;
            std::vector<uint32_t> insAt;  // new index of each insert
        };

        Built assemble(Actions const& a, std::vector<uint8_t> const& removed, std::vector<Ins> const& ins) {
            Built b;
            size_t const n = a.size();
            std::vector<uint32_t> order(ins.size());
            std::iota(order.begin(), order.end(), 0u);
            std::stable_sort(order.begin(), order.end(),
                             [&](uint32_t x, uint32_t y) { return ins[x].act.m_frame < ins[y].act.m_frame; });
            b.out.reserve(n + ins.size());
            b.insAt.assign(ins.size(), kNone);

            struct Key {
                uint32_t slot;
                uint8_t rank;
                uint32_t root, self, id;
            };
            std::vector<uint32_t> kept;
            std::vector<Key> keys;
            std::vector<uint32_t> keyOf(ins.size(), kNone);

            size_t i = 0, g = 0;
            constexpr uint64_t kEnd = std::numeric_limits<uint64_t>::max();
            while (i < n || g < order.size()) {
                if (i < n && removed[i]) {
                    ++i;
                    continue;
                }
                uint64_t const tk = i < n ? a[i].m_frame : kEnd;
                uint64_t const ti = g < order.size() ? ins[order[g]].act.m_frame : kEnd;
                if (tk < ti) {
                    b.out.push_back(a[i++]);
                    continue;
                }
                // One frame's group: what is kept on it and what goes in.
                kept.clear();
                size_t j = i;
                while (j < n && a[j].m_frame == ti) {
                    if (!removed[j])
                        kept.push_back(static_cast<uint32_t>(j));
                    ++j;
                }
                size_t gEnd = g;
                while (gEnd < order.size() && ins[order[gEnd]].act.m_frame == ti)
                    ++gEnd;
                uint32_t const K = static_cast<uint32_t>(kept.size());

                keys.clear();
                for (size_t q = g; q < gEnd; ++q) {
                    uint32_t const id = order[q];
                    Ins const& in = ins[id];
                    Key k{K, 4, static_cast<uint32_t>(q), static_cast<uint32_t>(q), id};
                    switch (in.place) {
                        case Place::Start:
                            k.slot = 0;
                            k.rank = 0;
                            break;
                        case Place::End: break;
                        case Place::Tap: {
                            int const lane = macrocheck::laneOf(in.act);
                            for (uint32_t s = 0; s < K; ++s) {
                                gb::Action const& y = a[kept[s]];
                                if (!isPress(y) || macrocheck::laneOf(y) != lane)
                                    continue;
                                bool releasedHere = false;
                                for (uint32_t s2 = s + 1; s2 < K; ++s2) {
                                    gb::Action const& z = a[kept[s2]];
                                    if (macrocheck::laneOf(z) == lane) {
                                        releasedHere = !z.m_holding;
                                        break;
                                    }
                                }
                                if (!releasedHere) {
                                    k.slot = s;
                                    k.rank = 2;
                                    break;
                                }
                            }
                            break;
                        }
                        case Place::Chain: {
                            // Straight after its tap: every caller inserts the
                            // tap's release right after the tap.
                            if (id > 0 && keyOf[id - 1] != kNone && keyOf[id - 1] < keys.size()) {
                                Key const& tap = keys[keyOf[id - 1]];
                                k.slot = tap.slot;
                                k.rank = tap.rank;
                                k.root = tap.root;
                            }
                            break;
                        }
                    }
                    keyOf[id] = static_cast<uint32_t>(keys.size());
                    keys.push_back(k);
                }
                std::stable_sort(keys.begin(), keys.end(), [](Key const& x, Key const& y) {
                    return std::tie(x.slot, x.rank, x.root, x.self) < std::tie(y.slot, y.rank, y.root, y.self);
                });
                size_t gk = 0;
                for (uint32_t s = 0; s <= K; ++s) {
                    while (gk < keys.size() && keys[gk].slot == s) {
                        b.insAt[keys[gk].id] = static_cast<uint32_t>(b.out.size());
                        b.out.push_back(ins[keys[gk].id].act);
                        ++gk;
                    }
                    if (s < K)
                        b.out.push_back(a[kept[s]]);
                }
                i = j;
                g = gEnd;
            }
            return b;
        }

        gb::Action retimed(gb::Action x, int64_t delta) {
            x.m_frame = static_cast<uint32_t>(static_cast<int64_t>(x.m_frame) + delta);
            return x;
        }

        gb::Action makeInput(uint32_t frame, int lane, bool press) {
            gb::Action x;
            x.m_frame = frame;
            x.m_type = static_cast<T>(lane % 3 + 1);
            x.m_holding = press;
            x.m_player2 = lane >= 3;
            return x;
        }

        // Inserts `it`'s actions at `pressAt` / `releaseAt` (an item without
        // that action ignores the frame), placed by class. Returns the insert
        // id of its first action.
        uint32_t insertItem(std::vector<Ins>& ins, Actions const& a, Item const& it, uint32_t pressAt,
                            uint32_t releaseAt) {
            uint32_t const id = static_cast<uint32_t>(ins.size());
            if (it.kind == Kind::Click) {
                gb::Action p = a[it.press];
                gb::Action r = a[it.release];
                p.m_frame = pressAt;
                r.m_frame = releaseAt;
                bool const tap = pressAt == releaseAt;
                ins.push_back({p, tap ? Place::Tap : Place::End});
                ins.push_back({r, tap ? Place::Chain : Place::Start});
            } else if (it.kind == Kind::Press) {
                gb::Action p = a[it.press];
                p.m_frame = pressAt;
                ins.push_back({p, Place::End});
            } else {
                gb::Action r = a[it.release];
                r.m_frame = releaseAt;
                ins.push_back({r, Place::Start});
            }
            return id;
        }

        void markRemoved(std::vector<uint8_t>& removed, Item const& it) {
            if (it.press != kNone)
                removed[it.press] = 1;
            if (it.release != kNone)
                removed[it.release] = 1;
        }

        // ------------------------------------------------------------ the check

        Draft refuse(std::string why) {
            Draft d;
            d.error = std::move(why);
            return d;
        }

        // Check Macro's findings that `after` has and `before` does not, by
        // problem, frame and lane (indices move with every edit).
        std::optional<macrocheck::Finding> newProblem(Actions const& before, Actions const& after) {
            using Sig = std::tuple<uint8_t, uint32_t, int>;
            auto sigs = [](macrocheck::Report const& r) {
                std::vector<Sig> s;
                s.reserve(r.findings.size());
                for (auto const& f : r.findings)
                    s.emplace_back(static_cast<uint8_t>(f.problem), f.frame, f.lane);
                std::sort(s.begin(), s.end());
                return s;
            };
            auto const ra = macrocheck::check(after);
            if (ra.findings.empty())
                return std::nullopt;
            auto const had = sigs(macrocheck::check(before));
            auto const has = sigs(ra);
            std::vector<Sig> extra;
            std::set_difference(has.begin(), has.end(), had.begin(), had.end(), std::back_inserter(extra));
            if (extra.empty())
                return std::nullopt;
            for (auto const& f : ra.findings)
                if (std::get<0>(extra.front()) == static_cast<uint8_t>(f.problem) &&
                    std::get<1>(extra.front()) == f.frame && std::get<2>(extra.front()) == f.lane)
                    return f;
            return std::nullopt;
        }

        // The last step of every planner: a draft that changes nothing, or
        // leaves a problem the macro did not have, is refused.
        Draft finish(Actions const& a, Draft d) {
            if (d.result.size() == a.size() &&
                std::equal(a.begin(), a.end(), d.result.begin(), [](auto const& x, auto const& y) { return same(x, y); }))
                return refuse("That changes nothing.");
            if (auto f = newProblem(a, d.result)) {
                std::string where = f->lane >= 0 ? std::string(laneName(f->lane)) + " at frame " : "frame ";
                return refuse("That would break the macro (" + where + num(f->frame) + ": " +
                              macrocheck::name(f->problem) + "), so nothing was changed.");
            }
            d.ok = true;
            std::sort(d.selection.begin(), d.selection.end());
            return d;
        }

        std::optional<std::string> notEditable(Model const& m) {
            if (!m.sorted)
                return "This macro has actions out of frame order, and the editor cannot place edits among them. "
                       "Check Macro on the Macro page lists where.";
            return std::nullopt;
        }

    } // namespace

    const char* laneName(int lane) {
        static const char* const kNames[kLanes] = {"P1 Jump", "P1 Left", "P1 Right", "P2 Jump", "P2 Left", "P2 Right"};
        return lane >= 0 && lane < kLanes ? kNames[lane] : "?";
    }

    // ---------------------------------------------------------------- model

    Model build(Actions const& a) {
        Model m;
        m.items.reserve(a.size() / 2 + 1);
        std::array<uint32_t, kLanes> open;
        open.fill(kNone);
        for (uint32_t i = 0; i < a.size(); ++i) {
            gb::Action const& x = a[i];
            if (i > 0 && x.m_frame < a[i - 1].m_frame)
                m.sorted = false;
            m.lastFrame = std::max(m.lastFrame, x.m_frame);
            int const lane = macrocheck::laneOf(x);
            if (lane < 0) {
                m.events.push_back(i);
                if (macrocheck::isReset(x)) {
                    // A death or restart lets go of every button (Check
                    // Macro's rule): what was held stays a lone press.
                    m.resetFrames.push_back(x.m_frame);
                    open.fill(kNone);
                }
                continue;
            }
            if (x.m_holding) {
                // Pressed again while held: the first press stays a lone press.
                Item it;
                it.press = i;
                it.kind = Kind::Press;
                it.lane = static_cast<int8_t>(lane);
                open[lane] = static_cast<uint32_t>(m.items.size());
                m.lanes[lane].push_back(open[lane]);
                m.items.push_back(it);
            } else if (open[lane] != kNone) {
                Item& it = m.items[open[lane]];
                it.release = i;
                it.kind = Kind::Click;
                open[lane] = kNone;
            } else {
                Item it;
                it.release = i;
                it.kind = Kind::Release;
                it.lane = static_cast<int8_t>(lane);
                m.lanes[lane].push_back(static_cast<uint32_t>(m.items.size()));
                m.items.push_back(it);
            }
        }
        if (!m.sorted)
            std::sort(m.resetFrames.begin(), m.resetFrames.end());
        for (auto const& it : m.items) {
            if (it.kind == Kind::Click)
                m.clicks++;
            else if (it.kind == Kind::Press)
                m.lonePresses++;
            else
                m.loneReleases++;
        }
        return m;
    }

    uint32_t firstFrame(Actions const& a, Item const& it) {
        return a[it.key()].m_frame;
    }

    uint32_t lastFrame(Actions const& a, Item const& it) {
        return it.release != kNone ? a[it.release].m_frame : a[it.press].m_frame;
    }

    uint32_t holdEnd(Actions const& a, Model const& m, Item const& it) {
        if (it.kind != Kind::Press)
            return lastFrame(a, it);
        uint32_t const t = a[it.press].m_frame;
        auto r = std::lower_bound(m.resetFrames.begin(), m.resetFrames.end(), t);
        if (r == m.resetFrames.end())
            return kLastFrame;
        return *r > t ? *r - 1 : t;
    }

    uint32_t itemOfKey(Model const& m, uint32_t key) {
        auto it = std::lower_bound(m.items.begin(), m.items.end(), key,
                                   [](Item const& x, uint32_t k) { return x.key() < k; });
        if (it == m.items.end() || it->key() != key)
            return kNone;
        return static_cast<uint32_t>(it - m.items.begin());
    }

    // ---------------------------------------------------------------- ranges

    Range moveRange(Actions const& a, Model const& m, std::span<uint32_t const> keys) {
        std::vector<uint32_t> const sel = itemsOf(m, keys);
        if (sel.empty())
            return Range{};
        std::vector<uint8_t> chosen(m.items.size(), 0);
        int64_t minFirst = std::numeric_limits<int64_t>::max(), maxLast = 0;
        for (uint32_t i : sel) {
            chosen[i] = 1;
            minFirst = std::min<int64_t>(minFirst, firstFrame(a, m.items[i]));
            maxLast = std::max<int64_t>(maxLast, lastFrame(a, m.items[i]));
        }

        Range r;
        r.lo = static_cast<int64_t>(kFirstFrame) - minFirst;
        r.loWhy = "frame 1 is the first frame playback reads";
        r.hi = static_cast<int64_t>(kLastFrame) - maxLast;
        r.hiWhy = "that is the last frame a macro can reach";

        // The lane rule, per run of selected items between two that stay: the
        // run keeps its own spacing, so only its ends can meet anything.
        for (int lane = 0; lane < kLanes; ++lane) {
            auto const& L = m.lanes[static_cast<size_t>(lane)];
            for (size_t j = 0; j < L.size(); ++j) {
                if (!chosen[L[j]])
                    continue;
                size_t k = j;
                while (k + 1 < L.size() && chosen[L[k + 1]])
                    ++k;
                Item const& x = m.items[L[j]];
                Item const& y = m.items[L[k]];
                if (j > 0) {
                    Item const& p = m.items[L[j - 1]];
                    bool const touch = p.endsWithRelease() && x.startsWithPress();
                    int64_t const minAt = static_cast<int64_t>(holdEnd(a, m, p)) + (touch ? 0 : 1);
                    raiseLo(r, minAt - firstFrame(a, x), prevWhy(a, m, p));
                }
                if (k + 1 < L.size()) {
                    Item const& n = m.items[L[k + 1]];
                    bool const touch = y.endsWithRelease() && n.startsWithPress();
                    int64_t const maxAt = static_cast<int64_t>(firstFrame(a, n)) - (touch ? 0 : 1);
                    lowerHi(r, maxAt - lastFrame(a, y), nextWhy(a, n));
                }
                j = k;
            }
        }

        // The attempt rule, per item.
        for (uint32_t i : sel) {
            Item const& it = m.items[i];
            Attempt const at = itemAttempt(a, m, it);
            if (at.hasBefore)
                raiseLo(r, at.lo - firstFrame(a, it), attemptStartWhy(a, at.resetBefore));
            if (at.hasAfter)
                lowerHi(r, at.hi - lastFrame(a, it), attemptEndWhy(a, at.resetAfter));
        }

        // Where it is now is always allowed, even if it already breaks a rule.
        r.lo = std::min<int64_t>(r.lo, 0);
        r.hi = std::max<int64_t>(r.hi, 0);
        return r;
    }

    Range edgeRange(Actions const& a, Model const& m, uint32_t key, Edge e) {
        uint32_t const idx = itemOfKey(m, key);
        if (idx == kNone || m.items[idx].kind != Kind::Click)
            return Range{};
        Item const& x = m.items[idx];
        auto const& L = m.lanes[static_cast<size_t>(x.lane)];
        size_t const pos = lanePos(m, x, idx);
        Item const* p = pos > 0 ? &m.items[L[pos - 1]] : nullptr;
        Item const* n = pos + 1 < L.size() ? &m.items[L[pos + 1]] : nullptr;
        int64_t const pf = a[x.press].m_frame;
        int64_t const rf = a[x.release].m_frame;
        Attempt const at = itemAttempt(a, m, x);

        Range r;
        if (e == Edge::Press) {
            r.lo = kFirstFrame;
            r.loWhy = "frame 1 is the first frame playback reads";
            if (p)
                raiseLo(r, static_cast<int64_t>(holdEnd(a, m, *p)) + (p->endsWithRelease() ? 0 : 1), prevWhy(a, m, *p));
            if (at.hasBefore)
                raiseLo(r, at.lo, attemptStartWhy(a, at.resetBefore));
            r.hi = rf;
            r.hiWhy = "the release is at " + num(rf);
            r.lo = std::min(r.lo, pf);
            r.hi = std::max(r.hi, pf);
        } else {
            r.lo = pf;
            r.loWhy = "the press is at " + num(pf);
            r.hi = kLastFrame;
            r.hiWhy = "that is the last frame a macro can reach";
            if (n)
                lowerHi(r, static_cast<int64_t>(firstFrame(a, *n)) - (n->startsWithPress() ? 0 : 1), nextWhy(a, *n));
            if (at.hasAfter)
                lowerHi(r, at.hi, attemptEndWhy(a, at.resetAfter));
            r.lo = std::min(r.lo, rf);
            r.hi = std::max(r.hi, rf);
        }
        return r;
    }

    // ---------------------------------------------------------------- edits

    Draft move(Actions const& a, Model const& m, std::span<uint32_t const> keys, int64_t delta) {
        if (auto why = notEditable(m))
            return refuse(*why);
        std::vector<uint32_t> const sel = itemsOf(m, keys);
        if (sel.empty())
            return refuse("Select something to move first.");
        if (delta == 0)
            return refuse("That changes nothing.");
        Range const r = moveRange(a, m, keys);
        if (delta < r.lo)
            return refuse("It can move at most " + num(-r.lo) + " frames earlier: " + r.loWhy + ".");
        if (delta > r.hi)
            return refuse("It can move at most " + num(r.hi) + " frames later: " + r.hiWhy + ".");

        std::vector<uint8_t> removed(a.size(), 0);
        std::vector<Ins> ins;
        std::vector<uint32_t> firsts;
        for (uint32_t i : sel) {
            Item const& it = m.items[i];
            markRemoved(removed, it);
            uint32_t const p = it.press != kNone ? retimed(a[it.press], delta).m_frame : 0;
            uint32_t const q = it.release != kNone ? retimed(a[it.release], delta).m_frame : 0;
            firsts.push_back(insertItem(ins, a, it, p, q));
        }
        Built b = assemble(a, removed, ins);
        Draft d;
        d.result = std::move(b.out);
        for (uint32_t id : firsts)
            d.selection.push_back(b.insAt[id]);
        d.label = "Moved " + describe(m, sel) + " by " + signedNum(delta);
        return finish(a, std::move(d));
    }

    Draft setEdges(Actions const& a, Model const& m, uint32_t key, uint32_t press, uint32_t release) {
        if (auto why = notEditable(m))
            return refuse(*why);
        uint32_t const idx = itemOfKey(m, key);
        if (idx == kNone)
            return refuse("That input is not in the macro any more.");
        Item const& it = m.items[idx];
        if (it.kind != Kind::Click)
            return refuse("Only a click has a press and a release to set; move this one instead.");
        if (press > release)
            return refuse("The press has to come before the release, or on the same frame.");
        Range const pr = edgeRange(a, m, key, Edge::Press);
        Range const rr = edgeRange(a, m, key, Edge::Release);
        if (static_cast<int64_t>(press) < pr.lo)
            return refuse("The press can't go before " + num(pr.lo) + ": " + pr.loWhy + ".");
        if (static_cast<int64_t>(release) > rr.hi)
            return refuse("The release can't go after " + num(rr.hi) + ": " + rr.hiWhy + ".");

        uint32_t const oldP = a[it.press].m_frame, oldR = a[it.release].m_frame;
        std::vector<uint8_t> removed(a.size(), 0);
        markRemoved(removed, it);
        std::vector<Ins> ins;
        uint32_t const id = insertItem(ins, a, it, press, release);
        Built b = assemble(a, removed, ins);
        Draft d;
        d.result = std::move(b.out);
        d.selection.push_back(b.insAt[id]);
        std::string const who = laneName(it.lane);
        if (press != oldP && release == oldR)
            d.label = "Moved the " + who + " press to " + num(press);
        else if (press == oldP && release != oldR)
            d.label = "Moved the " + who + " release to " + num(release);
        else
            d.label = "Set the " + who + " click to " + num(press) + " - " + num(release);
        return finish(a, std::move(d));
    }

    Draft remove(Actions const& a, Model const& m, std::span<uint32_t const> keys) {
        if (auto why = notEditable(m))
            return refuse(*why);
        std::vector<uint32_t> const sel = itemsOf(m, keys);
        if (sel.empty())
            return refuse("Select something to delete first.");
        std::vector<uint8_t> removed(a.size(), 0);
        for (uint32_t i : sel)
            markRemoved(removed, m.items[i]);
        Built b = assemble(a, removed, {});
        Draft d;
        d.result = std::move(b.out);
        d.label = "Deleted " + describe(m, sel);
        return finish(a, std::move(d));
    }

    Draft addClick(Actions const& a, Model const& m, int lane, uint32_t press, uint32_t hold) {
        if (auto why = notEditable(m))
            return refuse(*why);
        if (lane < 0 || lane >= kLanes)
            return refuse("Pick a lane for the click.");
        if (press < kFirstFrame)
            return refuse("Frame 1 is the first frame playback reads.");
        if (static_cast<uint64_t>(press) + hold > kLastFrame)
            return refuse("That runs past the last frame a macro can reach.");
        uint32_t const release = press + hold;

        auto r = std::lower_bound(m.resetFrames.begin(), m.resetFrames.end(), press);
        if (r != m.resetFrames.end() && *r <= release)
            return refuse(std::string("The ") + resetNounAt(a, *r) + " at " + num(*r) +
                          " is in the way: nothing is put on or across one.");

        // The lane rule: the new click [press, release] may only touch another
        // item as a re-click (a release, then a press, on one frame).
        for (uint32_t i : m.lanes[static_cast<size_t>(lane)]) {
            Item const& y = m.items[i];
            uint32_t const yf = firstFrame(a, y);
            uint32_t const ye = holdEnd(a, m, y);
            bool const before = ye < press || (ye == press && y.endsWithRelease());
            bool const after = yf > release || (yf == release && y.startsWithPress());
            if (before || after)
                continue;
            std::string const who = laneName(lane);
            switch (y.kind) {
                case Kind::Click:
                    return refuse(who + " already has a click from " + num(yf) + " to " + num(ye) + " there.");
                case Kind::Press:
                    return refuse(who + " is pressed at " + num(yf) +
                                  (ye == kLastFrame ? " and never released." : " and held until " + num(ye) + "."));
                case Kind::Release:
                    return refuse(who + " already has a release at " + num(yf) + ".");
            }
        }

        std::vector<uint8_t> removed(a.size(), 0);
        std::vector<Ins> ins;
        bool const tap = hold == 0;
        ins.push_back({makeInput(press, lane, true), tap ? Place::Tap : Place::End});
        ins.push_back({makeInput(release, lane, false), tap ? Place::Chain : Place::Start});
        Built b = assemble(a, removed, ins);
        Draft d;
        d.result = std::move(b.out);
        d.selection.push_back(b.insAt[0]);
        d.label = std::string("Added a ") + laneName(lane) + " click at " + num(press);
        return finish(a, std::move(d));
    }

    // ---------------------------------------------------------------- history

    size_t History::Step::bytes() const {
        return sizeof(Step) + (before.size() + after.size()) * sizeof(gb::Action) + label.size() +
               (selBefore.size() + selAfter.size()) * sizeof(uint32_t);
    }

    void History::record(Actions const& before, Actions const& after, std::string label,
                         std::vector<uint32_t> selectionBefore, std::vector<uint32_t> selectionAfter) {
        size_t const n = before.size(), k = after.size();
        size_t pre = 0;
        while (pre < n && pre < k && same(before[pre], after[pre]))
            ++pre;
        size_t suf = 0;
        while (suf < n - pre && suf < k - pre && same(before[n - 1 - suf], after[k - 1 - suf]))
            ++suf;
        Step s;
        s.lo = static_cast<uint32_t>(pre);
        s.before.assign(before.begin() + static_cast<std::ptrdiff_t>(pre),
                        before.begin() + static_cast<std::ptrdiff_t>(n - suf));
        s.after.assign(after.begin() + static_cast<std::ptrdiff_t>(pre),
                       after.begin() + static_cast<std::ptrdiff_t>(k - suf));
        s.label = std::move(label);
        s.selBefore = std::move(selectionBefore);
        s.selAfter = std::move(selectionAfter);
        for (auto const& r : m_redo)
            m_bytes -= r.bytes();
        m_redo.clear();
        m_bytes += s.bytes();
        m_undo.push_back(std::move(s));
        trim();
    }

    bool History::swap(Actions& a, uint32_t lo, Actions const& expect, Actions const& with) {
        if (static_cast<size_t>(lo) + expect.size() > a.size())
            return false;
        auto const at = a.begin() + lo;
        if (!std::equal(expect.begin(), expect.end(), at, [](auto const& x, auto const& y) { return same(x, y); }))
            return false;
        if (expect.size() == with.size()) {
            std::copy(with.begin(), with.end(), at);
        } else {
            a.erase(at, at + static_cast<std::ptrdiff_t>(expect.size()));
            a.insert(a.begin() + lo, with.begin(), with.end());
        }
        return true;
    }

    bool History::undo(Actions& a, std::vector<uint32_t>& selection, std::string& label) {
        if (m_undo.empty())
            return false;
        Step& s = m_undo.back();
        if (!swap(a, s.lo, s.after, s.before)) {
            clear();
            return false;
        }
        selection = s.selBefore;
        label = s.label;
        m_redo.push_back(std::move(s));
        m_undo.pop_back();
        return true;
    }

    bool History::redo(Actions& a, std::vector<uint32_t>& selection, std::string& label) {
        if (m_redo.empty())
            return false;
        Step& s = m_redo.back();
        if (!swap(a, s.lo, s.before, s.after)) {
            clear();
            return false;
        }
        selection = s.selAfter;
        label = s.label;
        m_undo.push_back(std::move(s));
        m_redo.pop_back();
        return true;
    }

    std::string const& History::undoLabel() const {
        static std::string const none;
        return m_undo.empty() ? none : m_undo.back().label;
    }

    std::string const& History::redoLabel() const {
        static std::string const none;
        return m_redo.empty() ? none : m_redo.back().label;
    }

    void History::clear() {
        m_undo.clear();
        m_redo.clear();
        m_bytes = 0;
    }

    void History::trim() {
        size_t drop = 0;
        while (drop < m_undo.size() && (m_undo.size() - drop > kMaxSteps || m_bytes > kMaxBytes)) {
            m_bytes -= m_undo[drop].bytes();
            ++drop;
        }
        if (drop)
            m_undo.erase(m_undo.begin(), m_undo.begin() + static_cast<std::ptrdiff_t>(drop));
    }

    // ---------------------------------------------------------------- misc

    bool same(gb::Action const& x, gb::Action const& y) {
        return x.m_frame == y.m_frame && x.m_type == y.m_type && x.m_holding == y.m_holding &&
               x.m_player2 == y.m_player2 && std::bit_cast<uint64_t>(x.m_tps) == std::bit_cast<uint64_t>(y.m_tps) &&
               std::bit_cast<uint64_t>(x.m_subtick) == std::bit_cast<uint64_t>(y.m_subtick);
    }

    uint64_t fingerprint(Actions const& a) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
            h ^= h >> 29;
        };
        for (auto const& x : a) {
            mix(uint64_t(x.m_frame) | (uint64_t(static_cast<uint8_t>(x.m_type)) << 32) |
                (uint64_t(x.m_holding) << 40) | (uint64_t(x.m_player2) << 41));
            mix(std::bit_cast<uint64_t>(x.m_tps));
            mix(std::bit_cast<uint64_t>(x.m_subtick));
        }
        mix(a.size());
        return h;
    }

} // namespace gucci::edit

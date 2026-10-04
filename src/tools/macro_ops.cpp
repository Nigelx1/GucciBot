#include "tools/macro_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace gucci::macroops {

    namespace fs = std::filesystem;

    namespace {

        bool isInput(gb::Action const& a) {
            return macrocheck::laneOf(a) >= 0;
        }

        void sortByFrame(Actions& a) {
            std::stable_sort(a.begin(), a.end(), [](gb::Action const& x, gb::Action const& y) {
                return x.m_frame < y.m_frame;
            });
        }

        std::string num(double v) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        bool validTps(double tps) {
            return std::isfinite(tps) && tps > 0.0;
        }

        // The problems Check Macro finds on frames lo..hi.
        uint32_t problemsIn(macrocheck::Report const& r, uint32_t lo, uint32_t hi) {
            uint32_t n = 0;
            for (auto const& f : r.findings)
                n += (f.frame >= lo && f.frame <= hi) ? 1 : 0;
            return n;
        }

        // Lanes still held after every action before index `end` (a death or
        // restart lets go of everything), as Check Macro walks them.
        std::array<bool, 6> heldBefore(Actions const& a, size_t end) {
            std::array<bool, 6> held{};
            for (size_t i = 0; i < end && i < a.size(); ++i) {
                if (macrocheck::isReset(a[i])) {
                    held.fill(false);
                    continue;
                }
                int const lane = macrocheck::laneOf(a[i]);
                if (lane >= 0)
                    held[lane] = a[i].m_holding;
            }
            return held;
        }

        gb::Action input(uint32_t frame, int lane, bool press) {
            gb::Action x;
            x.m_frame = frame;
            x.m_type = static_cast<gb::ActionType>(lane % 3 + 1);
            x.m_holding = press;
            x.m_player2 = lane >= 3;
            return x;
        }

        gb::Action tpsAction(uint32_t frame, double tps) {
            gb::Action x;
            x.m_frame = frame;
            x.m_type = gb::ActionType::TPS;
            x.m_tps = tps;
            return x;
        }

        // Refuses a result with more problems than its sources had.
        void judge(Outcome& o, uint32_t sourceProblems) {
            o.report = macrocheck::check(o.macro.actions);
            uint32_t const found = static_cast<uint32_t>(o.report.findings.size());
            if (found > sourceProblems) {
                o.ok = false;
                o.message = "The result would have " + std::to_string(found - sourceProblems) +
                            " problem(s) the source didn't (a press without its release, or the reverse), "
                            "so nothing was written.";
                return;
            }
            o.carriedProblems = found;
            o.ok = true;
        }

    } // namespace

    // ------------------------------------------------------------------ files

    Macro fromFile(GBR6File const& f) {
        Macro m;
        m.header = f.header;

        // Exactly GucciReplaySystem::load's order: P1's stream, P2's, the
        // deaths, then the sub-tick offsets onto the first input they fit.
        gb::ActionAtom atom;
        for (auto const& in : f.p1Inputs)
            atom.addAction(in.frame, static_cast<gb::ActionType>(in.button), in.pressed, false);
        for (auto const& in : f.p2Inputs)
            atom.addAction(in.frame, static_cast<gb::ActionType>(in.button), in.pressed, true);
        for (auto const& d : f.deaths)
            atom.addAction(d.frame, static_cast<gb::ActionType>(d.type), false, false);
        for (auto const& st : f.subticks) {
            for (auto& a : atom.m_actions) {
                if (a.m_frame == st.frame && static_cast<uint8_t>(a.m_type) == st.button &&
                    a.m_holding == st.pressed && a.m_player2 == st.player2) {
                    a.m_subtick = st.offset;
                    break;
                }
            }
        }
        for (auto const& t : f.tpsChanges) {
            // Playback never asks for frame 0, so a change there would sit
            // unmatched at the front of the list; it is the starting rate.
            if (t.frame == 0)
                m.header.tps = static_cast<float>(t.tps);
            else
                atom.m_actions.push_back(tpsAction(t.frame, t.tps));
        }
        m.actions = std::move(atom.m_actions);
        sortByFrame(m.actions);
        return m;
    }

    GBR6File toFile(Macro const& m) {
        GBR6Header hdr = m.header;
        hdr.flags &= static_cast<uint8_t>(~(GBR6_HAS_LEVELNAME | GBR6_HAS_DEATHS | GBR6_HAS_SUBTICK | GBR6_HAS_TPS));
        if (!hdr.levelName.empty())
            hdr.flags |= GBR6_HAS_LEVELNAME;

        std::vector<GBR6Input> p1, p2;
        for (auto const& a : m.actions) {
            if (!a.isInput())
                continue;
            GBR6Input in;
            in.frame = a.m_frame;
            in.button = static_cast<uint8_t>(a.m_type);
            in.pressed = a.m_holding;
            in.player2 = a.m_player2;
            (a.m_player2 ? p2 : p1).push_back(in);
        }
        auto f = GBR6File::fromInputs(hdr, std::move(p1), std::move(p2));

        for (auto const& a : m.actions) {
            if (macrocheck::isReset(a))
                f.deaths.push_back({a.m_frame, static_cast<uint8_t>(a.m_type)});
            else if (a.m_type == gb::ActionType::TPS && a.m_frame > 0 && validTps(a.m_tps))
                f.tpsChanges.push_back({a.m_frame, a.m_tps});
            else if (a.isInput() && a.m_subtick > 0.0 && a.m_subtick < 1.0)
                f.subticks.push_back(
                    {a.m_frame, static_cast<uint8_t>(a.m_type), a.m_holding, a.m_player2, a.m_subtick});
        }
        if (!f.deaths.empty())
            f.header.flags |= GBR6_HAS_DEATHS;
        if (!f.subticks.empty())
            f.header.flags |= GBR6_HAS_SUBTICK;
        if (!f.tpsChanges.empty())
            f.header.flags |= GBR6_HAS_TPS;
        return f;
    }

    std::optional<Macro> load(fs::path const& path, std::string* error) {
        auto fail = [&](std::string why) -> std::optional<Macro> {
            if (error)
                *error = std::move(why);
            return std::nullopt;
        };
        std::error_code ec;
        if (!fs::exists(path, ec))
            return fail("The file isn't there any more.");
        auto f = GBR6File::loadFromPath(path);
        if (!f)
            return fail("It isn't a GucciBot (GBR6) macro, or the file is damaged.");
        return fromFile(*f);
    }

    bool save(Macro const& m, fs::path const& path, std::string* error) {
        auto const bytes = toFile(m).serialize();
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out)
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out) {
                std::error_code ec;
                fs::remove(tmp, ec);
                if (error)
                    *error = "Couldn't write " + tmp.filename().string() + ".";
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            if (error)
                *error = "Couldn't replace " + path.filename().string() + ".";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------ facts

    Stats stats(Macro const& m) {
        Stats s;
        for (auto const& a : m.actions) {
            if (a.isInput()) {
                s.inputs++;
                s.subticks += (a.m_subtick > 0.0 && a.m_subtick < 1.0) ? 1 : 0;
                s.player2 |= a.m_player2;
            } else if (macrocheck::isReset(a)) {
                s.resets++;
            } else if (a.m_type == gb::ActionType::TPS) {
                s.tpsChanges++;
            }
            s.lastFrame = std::max(s.lastFrame, a.m_frame);
        }
        auto const r = macrocheck::check(m.actions);
        s.clicks = r.clicks;
        s.problems = static_cast<uint32_t>(r.findings.size());
        return s;
    }

    double tpsBefore(Macro const& m, uint32_t frame) {
        double tps = m.header.tps > 0.f ? static_cast<double>(m.header.tps) : 240.0;
        for (auto const& a : m.actions) {
            if (a.m_frame >= frame)
                break;
            if (a.m_type == gb::ActionType::TPS && validTps(a.m_tps))
                tps = a.m_tps;
        }
        return tps;
    }

    std::vector<TpsChange> tpsChanges(Macro const& m) {
        std::vector<TpsChange> out;
        for (auto const& a : m.actions)
            if (a.m_type == gb::ActionType::TPS)
                out.push_back({a.m_frame, a.m_tps});
        return out;
    }

    bool setTpsChange(Macro& m, uint32_t frame, double tps, std::string* error) {
        if (frame < kFirstFrame) {
            if (error)
                *error = "Frame 0 is the start: change the starting TPS in Metadata instead.";
            return false;
        }
        if (!validTps(tps)) {
            if (error)
                *error = "The TPS has to be a number above 0.";
            return false;
        }
        for (auto& a : m.actions) {
            if (a.m_type == gb::ActionType::TPS && a.m_frame == frame) {
                a.m_tps = tps;
                return true;
            }
        }
        auto at = std::upper_bound(m.actions.begin(), m.actions.end(), frame,
                                   [](uint32_t f, gb::Action const& a) { return f < a.m_frame; });
        m.actions.insert(at, tpsAction(frame, tps));
        return true;
    }

    bool removeTpsChange(Macro& m, uint32_t frame) {
        auto it = std::find_if(m.actions.begin(), m.actions.end(), [frame](gb::Action const& a) {
            return a.m_type == gb::ActionType::TPS && a.m_frame == frame;
        });
        if (it == m.actions.end())
            return false;
        m.actions.erase(it);
        return true;
    }

    // ------------------------------------------------------------------ trim

    Outcome trim(Macro const& src, uint32_t start, uint32_t end, bool rebase) {
        Outcome o;
        if (end < start) {
            o.message = "The end frame is before the start frame.";
            return o;
        }
        // end + 1 has to fit: a hold carried past the end is let go of there.
        end = std::min<uint32_t>(end, 0xFFFFFFFDu);
        Actions const& a = src.actions;

        auto const first = std::lower_bound(a.begin(), a.end(), start,
                                            [](gb::Action const& x, uint32_t f) { return x.m_frame < f; });
        auto const last = std::upper_bound(a.begin(), a.end(), end,
                                           [](uint32_t f, gb::Action const& x) { return f < x.m_frame; });
        size_t const i0 = static_cast<size_t>(first - a.begin());
        size_t const i1 = static_cast<size_t>(last - a.begin());

        bool anyInput = false;
        for (size_t i = i0; i < i1 && !anyInput; ++i)
            anyInput = a[i].isInput();
        auto const inherited = heldBefore(a, i0);
        bool const anyInherited = std::find(inherited.begin(), inherited.end(), true) != inherited.end();
        if (!anyInput && !anyInherited) {
            o.message = "There are no inputs between those frames.";
            return o;
        }

        Actions out;
        out.reserve(i1 - i0 + 12);
        // A button held into the range is pressed again where it starts. It
        // goes before the range's own actions on that frame, so a release
        // there still ends it.
        for (int lane = 0; lane < 6; ++lane)
            if (inherited[lane])
                out.push_back(input(start, lane, true));

        // The recorder writes a release after a respawn let go of a button
        // (Check Macro's ReleaseAfterReset). Cut away from the death before
        // it, it would be a release with nothing to end, so it is left out.
        // A release the source itself has no press for stays: that problem
        // is the source's, and is counted as carried over.
        auto const srcReport = macrocheck::check(a);
        std::vector<bool> srcOrphan(a.size(), false);
        for (auto const& f : srcReport.findings)
            if (f.problem == macrocheck::Problem::OrphanRelease && f.index < a.size())
                srcOrphan[f.index] = true;
        auto held = inherited;
        uint32_t dropped = 0;
        for (size_t i = i0; i < i1; ++i) {
            auto const& x = a[i];
            if (macrocheck::isReset(x)) {
                held.fill(false);
            } else if (int const lane = macrocheck::laneOf(x); lane >= 0) {
                if (!x.m_holding && !held[lane] && !srcOrphan[i]) {
                    dropped++;
                    continue;
                }
                held[lane] = x.m_holding;
            }
            out.push_back(x);
        }

        // A button still held at the end is let go of on end + 1 -- if the
        // source lets go of it later (or a death or restart does). One the
        // source holds to its very end stays held, as it was.
        for (int lane = 0; lane < 6; ++lane) {
            if (!held[lane])
                continue;
            bool endsLater = false;
            for (size_t i = i1; i < a.size() && !endsLater; ++i)
                endsLater = macrocheck::isReset(a[i]) || (macrocheck::laneOf(a[i]) == lane && !a[i].m_holding);
            if (endsLater)
                out.push_back(input(end + 1, lane, false));
        }

        o.macro.header = src.header;
        o.macro.header.tps = static_cast<float>(tpsBefore(src, start));
        if (rebase && start > kFirstFrame) {
            uint32_t const shift = start - kFirstFrame;
            for (auto& x : out)
                x.m_frame -= shift;
        }
        o.macro.actions = std::move(out);
        sortByFrame(o.macro.actions);

        judge(o, problemsIn(srcReport, start, end));
        if (!o.ok)
            return o;
        std::string notes;
        if (anyInherited)
            notes += "A button held into the range is pressed again on its first frame. ";
        if (dropped)
            notes += std::to_string(dropped) + " release(s) that only followed a death before the range were left out. ";
        if (rebase && start > kFirstFrame)
            notes += "Frame " + std::to_string(start) + " is now frame 1. ";
        if (o.carriedProblems)
            notes += std::to_string(o.carriedProblems) + " problem(s) in that range came from the source. ";
        o.message = notes;
        return o;
    }

    // ------------------------------------------------------------------ merge

    Outcome merge(Macro const& a, Macro const& b, uint32_t gap) {
        Outcome o;
        bool aInputs = std::any_of(a.actions.begin(), a.actions.end(), isInput);
        bool bInputs = std::any_of(b.actions.begin(), b.actions.end(), isInput);
        if (!aInputs || !bInputs) {
            o.message = !aInputs ? "Macro A has no inputs." : "Macro B has no inputs.";
            return o;
        }

        uint32_t const lastA = a.actions.back().m_frame;
        uint32_t const lastB = b.actions.back().m_frame;
        uint64_t const base64 = static_cast<uint64_t>(lastA) + gap;
        if (base64 + lastB + 2 > 0xFFFFFFFEull) {
            o.message = "The two together run past the last frame a macro can hold.";
            return o;
        }
        uint32_t const base = static_cast<uint32_t>(base64);

        Actions out = a.actions;
        out.reserve(a.actions.size() + b.actions.size() + 8);

        // A's open holds end where B begins, before any of B's actions there.
        // B's frame 0 is `base`, and nothing of B's sits there (playback
        // never asks for frame 0), but with no gap that is also A's last
        // frame -- then the release goes one frame later, still ahead of B's
        // own actions on that frame.
        auto const held = heldBefore(a.actions, a.actions.size());
        bool const anyHeld = std::find(held.begin(), held.end(), true) != held.end();
        uint32_t const releaseAt = std::max(base, lastA + 1);
        for (int lane = 0; lane < 6; ++lane)
            if (held[lane])
                out.push_back(input(releaseAt, lane, false));

        // B ran at its own header's TPS from its first frame; A may end at
        // another. The change is handed over with B's frame 1 (on the tick
        // that was B's first), so that tick already runs at B's rate.
        double const endA = tpsBefore(a, 0xFFFFFFFFu);
        double const startB = b.header.tps > 0.f ? static_cast<double>(b.header.tps) : 240.0;
        bool const tpsJoin = std::fabs(endA - startB) > 1e-9;
        if (tpsJoin)
            out.push_back(tpsAction(base + kFirstFrame, startB));

        for (auto x : b.actions) {
            x.m_frame += base;
            out.push_back(x);
        }

        o.macro.header = a.header;
        o.macro.actions = std::move(out);
        sortByFrame(o.macro.actions);

        uint32_t const srcProblems = static_cast<uint32_t>(macrocheck::check(a.actions).findings.size() +
                                                           macrocheck::check(b.actions).findings.size());
        judge(o, srcProblems);
        if (!o.ok)
            return o;

        std::string notes = "B starts at frame " + std::to_string(base) + ". ";
        if (anyHeld)
            notes += "A ended holding a button; it's let go of before B starts. ";
        if (tpsJoin)
            notes += "TPS changes from " + num(endA) + " to " + num(startB) + " where B starts. ";
        if (!a.header.levelName.empty() && !b.header.levelName.empty() &&
            a.header.levelName != b.header.levelName)
            notes += "They were recorded on different levels ('" + a.header.levelName + "' and '" +
                     b.header.levelName + "'); the result says A's. ";
        if (a.header.rngSeed != b.header.rngSeed)
            notes += "They started with different random seeds; the result keeps A's, so random "
                     "triggers in B's part may play differently. ";
        if (o.carriedProblems)
            notes += std::to_string(o.carriedProblems) + " problem(s) came from the sources. ";
        o.message = notes;
        return o;
    }

    // ------------------------------------------------------------------ diff

    Diff diff(Macro const& a, Macro const& b, uint32_t moveWindow) {
        Diff d;
        d.startTpsDiffers = a.header.tps != b.header.tps;
        d.levelDiffers = a.header.levelName != b.header.levelName;
        d.seedDiffers = a.header.rngSeed != b.header.rngSeed;

        // Inputs, one group per lane and direction.
        for (int lane = 0; lane < 6; ++lane) {
            for (int dir = 0; dir < 2; ++dir) {
                bool const press = dir == 0;
                auto pick = [&](Actions const& src) {
                    std::vector<gb::Action const*> v;
                    for (auto const& x : src)
                        if (macrocheck::laneOf(x) == lane && x.m_holding == press)
                            v.push_back(&x);
                    std::stable_sort(v.begin(), v.end(), [](auto* x, auto* y) { return x->m_frame < y->m_frame; });
                    return v;
                };
                auto const A = pick(a.actions);
                auto const B = pick(b.actions);

                std::vector<gb::Action const*> ua, ub;
                size_t i = 0, j = 0;
                while (i < A.size() && j < B.size()) {
                    if (A[i]->m_frame == B[j]->m_frame) {
                        d.matched++;
                        if (A[i]->m_subtick != B[j]->m_subtick) {
                            DiffItem it;
                            it.kind = DiffKind::SubtickDiffers;
                            it.type = A[i]->m_type;
                            it.lane = lane;
                            it.press = press;
                            it.frameA = it.frameB = A[i]->m_frame;
                            it.valueA = A[i]->m_subtick;
                            it.valueB = B[j]->m_subtick;
                            d.items.push_back(it);
                            d.subtick++;
                        }
                        ++i, ++j;
                    } else if (A[i]->m_frame < B[j]->m_frame) {
                        ua.push_back(A[i++]);
                    } else {
                        ub.push_back(B[j++]);
                    }
                }
                ua.insert(ua.end(), A.begin() + i, A.end());
                ub.insert(ub.end(), B.begin() + j, B.end());

                auto only = [&](gb::Action const* x, bool inA) {
                    DiffItem it;
                    it.kind = inA ? DiffKind::OnlyA : DiffKind::OnlyB;
                    it.type = x->m_type;
                    it.lane = lane;
                    it.press = press;
                    (inA ? it.frameA : it.frameB) = x->m_frame;
                    d.items.push_back(it);
                    (inA ? d.onlyA : d.onlyB)++;
                };
                i = 0, j = 0;
                while (i < ua.size() && j < ub.size()) {
                    uint32_t const fa = ua[i]->m_frame, fb = ub[j]->m_frame;
                    uint32_t const dist = fa > fb ? fa - fb : fb - fa;
                    if (dist <= moveWindow) {
                        DiffItem it;
                        it.kind = DiffKind::Moved;
                        it.type = ua[i]->m_type;
                        it.lane = lane;
                        it.press = press;
                        it.frameA = fa;
                        it.frameB = fb;
                        d.items.push_back(it);
                        d.moved++;
                        ++i, ++j;
                    } else if (fa < fb) {
                        only(ua[i++], true);
                    } else {
                        only(ub[j++], false);
                    }
                }
                for (; i < ua.size(); ++i)
                    only(ua[i], true);
                for (; j < ub.size(); ++j)
                    only(ub[j], false);
            }
        }

        // Deaths, restarts and TPS changes: the same type on the same frame
        // is the same event.
        auto events = [](Actions const& src) {
            std::vector<gb::Action const*> v;
            for (auto const& x : src)
                if (!x.isInput())
                    v.push_back(&x);
            std::stable_sort(v.begin(), v.end(), [](auto* x, auto* y) {
                return x->m_frame != y->m_frame ? x->m_frame < y->m_frame : x->m_type < y->m_type;
            });
            return v;
        };
        auto const EA = events(a.actions);
        auto const EB = events(b.actions);
        auto onlyEvent = [&](gb::Action const* x, bool inA) {
            DiffItem it;
            it.kind = inA ? DiffKind::OnlyA : DiffKind::OnlyB;
            it.type = x->m_type;
            (inA ? it.frameA : it.frameB) = x->m_frame;
            (inA ? it.valueA : it.valueB) = x->m_tps;
            d.items.push_back(it);
            (inA ? d.onlyA : d.onlyB)++;
        };
        size_t i = 0, j = 0;
        while (i < EA.size() && j < EB.size()) {
            auto const* x = EA[i];
            auto const* y = EB[j];
            if (x->m_frame == y->m_frame && x->m_type == y->m_type) {
                if (x->m_type == gb::ActionType::TPS && x->m_tps != y->m_tps) {
                    DiffItem it;
                    it.kind = DiffKind::TpsDiffers;
                    it.type = x->m_type;
                    it.frameA = it.frameB = x->m_frame;
                    it.valueA = x->m_tps;
                    it.valueB = y->m_tps;
                    d.items.push_back(it);
                    d.tps++;
                }
                ++i, ++j;
            } else if (x->m_frame < y->m_frame || (x->m_frame == y->m_frame && x->m_type < y->m_type)) {
                onlyEvent(EA[i++], true);
            } else {
                onlyEvent(EB[j++], false);
            }
        }
        for (; i < EA.size(); ++i)
            onlyEvent(EA[i], true);
        for (; j < EB.size(); ++j)
            onlyEvent(EB[j], false);

        std::stable_sort(d.items.begin(), d.items.end(), [](DiffItem const& x, DiffItem const& y) {
            uint32_t const fx = x.kind == DiffKind::Moved ? std::min(x.frameA, x.frameB) : x.frame();
            uint32_t const fy = y.kind == DiffKind::Moved ? std::min(y.frameA, y.frameB) : y.frame();
            return fx < fy;
        });
        return d;
    }

} // namespace gucci::macroops

#pragma once

// The queues' pure parts (Juice's idea, 2026-11-17): enter level IDs or pick
// saved macros, leave the PC on, come back to macros and frame windows. Two
// queues run through src/tools/queue.cpp, which drives the game; everything
// here is plain C++ so tools/run_queue_tests.cpp can test it offline:
//
// - parseLevelIds: what the Pathfinder queue's box takes (IDs split by
//   spaces, commas, semicolons or lines).
// - the queue's saved state (queue_pf.txt / queue_fw.txt in the mod's save
//   folder), as text: one line a field, tab-separated, so a game that closes
//   mid-queue can show the results and carry on.
// - afterRestart: a level that was running when the game closed is tried
//   once more, then marked and passed over (it may be the one that crashed it).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace gucci::queue {

    enum class Kind { Pathfinder, FrameWindows };

    enum class Status {
        Pending,  // not tried yet
        Running,  // being worked on (saved like this so a crash is seen on the next launch)
        Beaten,   // Pathfinder: the level was beaten. Frame windows: counted
        NotBeaten,  // Pathfinder: ran out of time or stopped; the furthest attempt is kept
        Failed,   // couldn't download, couldn't open, didn't start, crashed: `note` says
        Skipped,  // the queue was stopped before this one, or the user took it out
    };

    inline const char* statusKey(Status s) {
        switch (s) {
            case Status::Pending: return "pending";
            case Status::Running: return "running";
            case Status::Beaten: return "beaten";
            case Status::NotBeaten: return "not_beaten";
            case Status::Failed: return "failed";
            case Status::Skipped: return "skipped";
        }
        return "pending";
    }

    inline Status statusFromKey(std::string_view k) {
        for (Status s : {Status::Pending, Status::Running, Status::Beaten, Status::NotBeaten, Status::Failed,
                         Status::Skipped})
            if (k == statusKey(s))
                return s;
        return Status::Pending;
    }

    struct Item {
        // Pathfinder queue: the level's ID (1 to 22 and 5001 to 5004 are
        // RobTop's levels). Frame-window queue: 0, and `macro` is the input.
        int levelId = 0;
        std::string levelName;  // once known
        Status status = Status::Pending;
        int tries = 0;           // times it was started (a crash mid-level is one)
        double bestPercent = 0;  // Pathfinder: the furthest it got, 0..100
        double seconds = 0;      // time spent on it
        std::string macro;       // the macro saved (or, frame windows, the one counted)
        int windows = -1;        // windows counted (-1: none counted)
        double lstar = -1;       // L* of those windows (-1: not worked out)
        std::string note;        // why it failed, or what happened
    };

    struct State {
        Kind kind = Kind::Pathfinder;
        int limitMinutes = 30;     // Pathfinder: time per level
        bool countWindows = true;  // Pathfinder: count frame windows on a win
        bool running = false;      // the queue was going (not stopped by the user) when saved
        std::vector<Item> items;

        int left() const {
            return (int)std::count_if(items.begin(), items.end(), [](Item const& i) {
                return i.status == Status::Pending || i.status == Status::Running;
            });
        }
    };

    // RobTop's levels, opened from the game's own files: 1 Stereo Madness to
    // 22 Dash, and The Tower's floors 5001 to 5004.
    inline bool isMainLevel(int id) {
        return (id >= 1 && id <= 22) || (id >= 5001 && id <= 5004);
    }

    // Level IDs from what the user pasted: digits split by anything else that
    // is a space, comma, semicolon or line break. Anything else in a piece
    // (letters, a minus) makes that piece go to `bad` instead. A repeat is
    // dropped (the first stays), and so is 0.
    inline std::vector<int> parseLevelIds(std::string_view text, std::vector<std::string>* bad = nullptr) {
        std::vector<int> out;
        auto const sep = [](char c) {
            return c == ' ' || c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t';
        };
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && sep(text[i]))
                ++i;
            size_t j = i;
            while (j < text.size() && !sep(text[j]))
                ++j;
            if (j > i) {
                std::string_view tok = text.substr(i, j - i);
                // A "#" in front (as the game shows IDs) is fine.
                if (tok.size() > 1 && tok.front() == '#')
                    tok.remove_prefix(1);
                bool digits = tok.size() <= 10;
                for (char c : tok)
                    if (c < '0' || c > '9')
                        digits = false;
                long long v = 0;
                if (digits)
                    for (char c : tok)
                        v = v * 10 + (c - '0');
                if (!digits || v <= 0 || v > 2147483647LL) {
                    if (bad && !(digits && v == 0))
                        bad->emplace_back(text.substr(i, j - i));
                } else if (std::find(out.begin(), out.end(), (int)v) == out.end()) {
                    out.push_back((int)v);
                }
            }
            i = j;
        }
        return out;
    }

    // A level's name made safe for a file name, with its ID: "Bloodbath
    // (10565740)". The Pathfinder queue saves its win under this name.
    inline std::string macroNameFor(std::string_view levelName, int id) {
        static constexpr std::string_view kIllegal = "\\/:*?\"<>|";
        std::string base;
        for (char c : levelName) {
            if ((unsigned char)c < 0x20 || kIllegal.find(c) != std::string_view::npos)
                continue;
            base += c;
        }
        while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
            base.pop_back();
        size_t const first = base.find_first_not_of(' ');
        base = first == std::string::npos ? std::string() : base.substr(first);
        if (base.empty())
            base = "Level";
        return base + " (" + std::to_string(id) + ")";
    }

    // The level ID a queue-made macro's name carries ("Bloodbath (10565740)",
    // also with " furthest" or " 2" after it), or 0.
    inline int idFromMacroName(std::string_view name) {
        size_t const close = name.rfind(')');
        if (close == std::string_view::npos)
            return 0;
        size_t const open = name.rfind('(', close);
        if (open == std::string_view::npos || close - open < 2 || close - open > 11)
            return 0;
        long long v = 0;
        for (size_t i = open + 1; i < close; ++i) {
            if (name[i] < '0' || name[i] > '9')
                return 0;
            v = v * 10 + (name[i] - '0');
        }
        // Only a bare number or a " furthest" / " <n>" after it.
        std::string_view rest = name.substr(close + 1);
        if (!rest.empty() && rest != " furthest") {
            if (rest.size() < 2 || rest[0] != ' ')
                return 0;
            for (size_t i = 1; i < rest.size(); ++i)
                if (rest[i] < '0' || rest[i] > '9')
                    return 0;
        }
        return v > 0 && v <= 2147483647LL ? (int)v : 0;
    }

    // ---- saved state

    namespace detail {
        // One field: tabs and line breaks would break the line, so they go.
        inline std::string clean(std::string_view s) {
            std::string out(s);
            for (char& c : out)
                if (c == '\t' || c == '\n' || c == '\r')
                    c = ' ';
            return out;
        }
        inline std::vector<std::string> split(std::string_view line, char by) {
            std::vector<std::string> out;
            size_t i = 0;
            while (true) {
                size_t const j = line.find(by, i);
                out.emplace_back(line.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
                if (j == std::string_view::npos)
                    break;
                i = j + 1;
            }
            return out;
        }
        inline double num(std::string const& s, double fallback) {
            if (s.empty())
                return fallback;
            char* end = nullptr;
            double const v = std::strtod(s.c_str(), &end);
            return end && *end == '\0' ? v : fallback;
        }
        inline std::string fmtNum(double v) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.4f", v);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
            return s;
        }
    }  // namespace detail

    inline std::string serialize(State const& st) {
        using namespace detail;
        std::string out = "GBQUEUE\t1\n";
        out += std::string("kind\t") + (st.kind == Kind::Pathfinder ? "pathfinder" : "frame_windows") + "\n";
        out += "limit\t" + std::to_string(st.limitMinutes) + "\n";
        out += std::string("count_windows\t") + (st.countWindows ? "1" : "0") + "\n";
        out += std::string("running\t") + (st.running ? "1" : "0") + "\n";
        for (auto const& it : st.items) {
            out += "item\t" + std::to_string(it.levelId) + "\t" + statusKey(it.status) + "\t" +
                   std::to_string(it.tries) + "\t" + fmtNum(it.bestPercent) + "\t" + fmtNum(it.seconds) + "\t" +
                   std::to_string(it.windows) + "\t" + fmtNum(it.lstar) + "\t" + clean(it.macro) + "\t" +
                   clean(it.levelName) + "\t" + clean(it.note) + "\n";
        }
        return out;
    }

    // False when it isn't a queue file (st is left as it was).
    inline bool parse(std::string_view text, State& st) {
        using namespace detail;
        auto lines = split(text, '\n');
        if (lines.empty())
            return false;
        auto head = split(lines[0], '\t');
        if (head.size() < 2 || head[0] != "GBQUEUE")
            return false;
        State out;
        for (size_t n = 1; n < lines.size(); ++n) {
            std::string line = lines[n];
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            auto f = split(line, '\t');
            if (f[0] == "kind" && f.size() >= 2) {
                out.kind = f[1] == "frame_windows" ? Kind::FrameWindows : Kind::Pathfinder;
            } else if (f[0] == "limit" && f.size() >= 2) {
                out.limitMinutes = std::clamp((int)num(f[1], 30), 1, 24 * 60);
            } else if (f[0] == "count_windows" && f.size() >= 2) {
                out.countWindows = f[1] == "1";
            } else if (f[0] == "running" && f.size() >= 2) {
                out.running = f[1] == "1";
            } else if (f[0] == "item" && f.size() >= 11) {
                Item it;
                it.levelId = (int)num(f[1], 0);
                it.status = statusFromKey(f[2]);
                it.tries = (int)num(f[3], 0);
                it.bestPercent = num(f[4], 0);
                it.seconds = num(f[5], 0);
                it.windows = (int)num(f[6], -1);
                it.lstar = num(f[7], -1);
                it.macro = f[8];
                it.levelName = f[9];
                it.note = f[10];
                out.items.push_back(std::move(it));
            }
        }
        st = std::move(out);
        return true;
    }

    // What a launch does with a queue the last one left. A level that was
    // running when the game closed goes back in line for one more try; one
    // that was already tried twice is marked and passed over (it may be what
    // closed the game). Returns how many are left.
    inline int afterRestart(State& st) {
        for (auto& it : st.items) {
            if (it.status != Status::Running)
                continue;
            if (it.tries >= 2) {
                it.status = Status::Failed;
                it.note = "The game closed twice while this ran";
            } else {
                it.status = Status::Pending;
                it.note = "The game closed while this ran: it gets one more try";
            }
        }
        return st.left();
    }

    // "12 s", "4 min 05 s", "1 h 02 min".
    inline std::string formatSeconds(double s) {
        long long const t = s < 0 ? 0 : (long long)(s + 0.5);
        char buf[48];
        if (t < 60)
            std::snprintf(buf, sizeof buf, "%lld s", t);
        else if (t < 3600)
            std::snprintf(buf, sizeof buf, "%lld min %02lld s", t / 60, t % 60);
        else
            std::snprintf(buf, sizeof buf, "%lld h %02lld min", t / 3600, (t / 60) % 60);
        return buf;
    }

}  // namespace gucci::queue

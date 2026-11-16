// Share my windows (ui/share.hpp): what goes on the picture, the thread that
// makes it, and the button.

#include "ui/share.hpp"

#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"
#include "ui/share_draw.hpp"
#include "ui/ui.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace gucci::ui::share {

    namespace {

        namespace fs = std::filesystem;

        // How many of the tightest windows the picture lists.
        constexpr size_t kTightest = 8;

        Status g_status;

        // Everything the worker needs, copied on the game's thread.
        struct Job {
            Card card;
            fs::path regularFont, boldFont, mark;  // read on the worker
            fs::path out;
            bool solve = false;  // solve L* on the worker (the card's is stale or missing)
            std::vector<lstar::Input> inputs;
            lstar::Settings settings;
            std::chrono::steady_clock::time_point started;
        };

        Rgba rgba(ImVec4 const& c) {
            auto const to = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
            return {to(c.x), to(c.y), to(c.z), 255};
        }

        Rgba rgba(std::array<float, 4> const& c) {
            auto const to = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
            return {to(c[0]), to(c[1]), to(c[2]), to(c[3])};
        }

        // A band's name as the legend shows it.
        std::string bandName(FrameWindowTier const& t) {
            if (!t.text.empty())
                return t.text;
            return t.minWindow == t.maxWindow ? fmt::format("{:g}", t.minWindow)
                                              : fmt::format("{:g}-{:g}", t.minWindow, t.maxWindow);
        }

        // A mark's window as L* counts it (precisionInputs with sub-tick
        // windows): a ~N first, then the sub-tick window Calculate measured,
        // else the whole count. So the tightest windows are the ones the
        // score finds tightest, 0.44 before 0.77 even where the markers in the
        // level print both as 1 (Whole-frame markers).
        double windowOf(FrameWindowMark const& mk) {
            if (mk.dependent > 0.f)
                return mk.dependent;
            return mk.subframe > 0.f ? mk.subframe : mk.window;
        }

        // A window in frames: whole as "3", else to two places ("0.44", "1.5").
        std::string frames(double v) {
            if (std::fabs(v - std::round(v)) < 0.005)
                return fmt::format("{}", static_cast<long long>(std::llround(v)));
            std::string s = fmt::format("{:.2f}", v);
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            return s;
        }

        // The target time as words: "24 hours", "90 minutes".
        std::string duration(double seconds) {
            if (std::fmod(seconds, 3600.0) == 0.0)
                return fmt::format("{:g} {}", seconds / 3600.0, seconds == 3600.0 ? "hour" : "hours");
            if (std::fmod(seconds, 60.0) == 0.0)
                return fmt::format("{:g} minutes", seconds / 60.0);
            return fmt::format("{:g} seconds", seconds);
        }

        std::tm localNow() {
            std::time_t const now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            return tm;
        }

        // A file name from a macro's name: the characters no system takes
        // in a name dropped, and never empty.
        std::string safeName(std::string name) {
            std::string out;
            for (char c : name)
                if (static_cast<unsigned char>(c) >= 32 && std::string_view("\\/:*?\"<>|").find(c) == std::string_view::npos)
                    out += c;
            while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
                out.pop_back();
            while (!out.empty() && out.front() == ' ')
                out.erase(out.begin());
            // Cut long names at 80 bytes, backed up to the start of a
            // character: half a UTF-8 character makes the path throw on Windows.
            if (out.size() > 80) {
                size_t cut = 80;
                while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
                    --cut;
                out.resize(cut);
                while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
                    out.pop_back();
            }
            return out.empty() ? "macro" : out;
        }

        // A UTF-8 name as a path (fs::path(std::string) reads it in the ANSI
        // code page on Windows).
        fs::path u8path(std::string const& s) { return fs::path(std::u8string(s.begin(), s.end())); }

        fs::path sharesDir() { return geode::Mod::get()->getSaveDir() / "shares"; }

        std::vector<uint8_t> readFile(fs::path const& path) {
            std::ifstream in(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        // The worker: everything slow. Returns "" or why it failed.
        std::string make(Job& job) {
            Card& card = job.card;
            card.regularFont = readFile(job.regularFont);
            card.boldFont = readFile(job.boldFont);
            if (!job.mark.empty()) {
                auto const bytes = readFile(job.mark);
                std::string ignored;  // no mark is fine: the footer is drawn without it
                if (bytes.empty() || !png::decode(bytes.data(), bytes.size(), card.mark, ignored))
                    card.mark = {};
            }
            if (job.solve) {
                double const value = lstar::solveLikeSolver(job.inputs, job.settings);
                card.haveScore = value > 0.0 && std::isfinite(value);
                card.score = fmt::format("{:.3f}", value);
            }

            png::Pixels px;
            std::string error;
            if (!draw(card, px, error))
                return error;
            auto const file = png::encode(px, false, error);
            if (file.empty())
                return error;

            std::error_code ec;
            fs::create_directories(job.out.parent_path(), ec);
            if (ec)
                return "couldn't make the shares folder: " + ec.message();
            fs::path tmp = job.out;
            tmp += ".part";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return "couldn't write in the shares folder";
                out.write(reinterpret_cast<char const*>(file.data()), static_cast<std::streamsize>(file.size()));
                if (!out)
                    return "couldn't write the picture (is the disk full?)";
            }
            fs::rename(tmp, job.out, ec);
            if (ec) {
                fs::remove(tmp, ec);
                return "couldn't save the picture: " + ec.message();
            }
            return "";
        }

        // Fills a job from the game's state. "" or why there's nothing to share.
        std::string collect(Job& job) {
            auto* gb = GucciEngine::get();
            auto& fw = ::Bot::get()->frameWindow();
            auto const& results = fw.results();
            bool const haveMacro = !gb->replay.m_actionAtom.m_actions.empty();
            if (fw.running())
                return "Share my windows waits for the run to finish.";
            if (results.empty())
                return haveMacro ? "Nothing to share yet: this macro has no windows. Press Calculate first."
                                 : "Nothing to share yet: load a macro and press Calculate first.";

            // The marks the legend counts, and each one's click number.
            std::vector<int> numberOf(results.size(), 0);
            for (auto const& in : fw.allInputs(true))
                if (in.mark >= 0 && in.mark < static_cast<int>(results.size()))
                    numberOf[static_cast<size_t>(in.mark)] = in.number;
            struct Counted {
                size_t index;
                double value;
                FrameWindowTier const* tier;
            };
            std::vector<Counted> counted;
            for (size_t i = 0; i < results.size(); ++i)
                if (auto const* tier = fw.legendTierFor(results[i]))
                    counted.push_back({i, windowOf(results[i]), tier});
            if (counted.empty())
                return "Nothing to share: none of these windows show on the legend (each is desynced, hidden, or in "
                       "no band that's shown).";

            Card& card = job.card;
            auto const& pal = look().pal;
            card.theme.backdrop = rgba(pal.backdrop);
            card.theme.surface = rgba(pal.surface);
            card.theme.raised = rgba(pal.raised);
            card.theme.ink = rgba(pal.ink);
            card.theme.muted = rgba(pal.muted);
            card.theme.line = pal.cardEdge.on ? rgba(pal.cardEdge.outer) : rgba(pal.line);
            card.theme.accent = rgba(pal.accent);
            card.theme.onAccent = rgba(pal.onAccent);
            card.theme.radius = pal.radius;

            // The level the macro was recorded on, with the open level's ID
            // when that's the one (an ID of 0 is a level of your own). The
            // open level alone when the macro has no level name.
            std::string level = gb->loadedMacroLevelName;
            int levelId = 0;
            if (auto* pl = PlayLayer::get(); pl && pl->m_level) {
                std::string const open = pl->m_level->m_levelName.c_str();
                if (level.empty() || level == open) {
                    level = open;
                    levelId = pl->m_level->m_levelID.value();
                }
            }
            card.level = level.empty() ? "Unknown level" : level;

            int presses = 0;
            for (auto const& in : fw.allInputs(false))
                presses = std::max(presses, in.number);
            double const tps = fw.resultsTps();
            std::string const dot = "  \xC2\xB7  ";
            std::string details;
            if (levelId > 0)
                details = fmt::format("ID {}", levelId) + dot;
            if (!gb->replayName.empty())
                details += gb->replayName + dot;
            details += fmt::format("{} click{}", grouped(presses), presses == 1 ? "" : "s") + dot +
                       fmt::format("{:g} TPS", tps);
            card.details = details;

            for (auto const& [tier, count] : fw.legendTotals())
                card.bands.push_back({bandName(tier), rgba(tier.color), count});
            card.countedLine = fmt::format("{} window{} counted", grouped(static_cast<long long>(counted.size())),
                                           counted.size() == 1 ? "" : "s");

            std::stable_sort(counted.begin(), counted.end(),
                             [](Counted const& a, Counted const& b) { return a.value < b.value; });
            for (size_t i = 0; i < counted.size() && i < kTightest; ++i) {
                auto const& c = counted[i];
                auto const& mk = results[c.index];
                Tight t;
                t.number = numberOf[c.index];
                t.kind = fmt::format("{}{}", mk.player2 ? "P2 " : "", mk.release ? "release" : "press");
                t.percent = mk.percent;
                t.frames = (mk.dependent > 0.f ? "~" : "") + frames(c.value) + (mk.wide && !mk.manual ? "+" : "");
                t.ms = c.value / (tps > 0.0 ? tps : 240.0) * 1000.0;
                t.color = rgba(c.tier->color);
                card.tight.push_back(t);
            }

            // L*, with the Precision card's settings (as its Solve L* does).
            auto const& s = SLSettings::get()->frameWindow;
            lstar::Settings ls;
            ls.m_tps = tps;
            ls.m_respawnSeconds = s.lstarRespawn;
            ls.m_targetSeconds = s.lstarTarget > 0.0 ? s.lstarTarget : 86400.0;
            ls.m_nerve = s.lstarNerve;
            ls.m_fatigue = s.lstarFatigue;
            ls.m_cps = s.lstarCps;
            ls.m_useNerve = s.lstarUseNerve;
            ls.m_useFatigue = s.lstarUseFatigue;
            ls.m_useCps = s.lstarUseCps;
            auto* solver = lstar::Solver::get();
            if (!solver->running() && !solver->dirty() && solver->result().m_ok) {
                // The number the Precision card shows right now.
                card.haveScore = solver->result().m_value > 0.0;
                card.score = fmt::format("{:.3f}", solver->result().m_value);
            } else {
                job.solve = true;
                job.inputs = fw.precisionInputs();
                job.settings = ls;
            }
            std::vector<std::string> extras;
            if (s.lstarUseNerve)
                extras.push_back("Nerve");
            if (s.lstarUseFatigue)
                extras.push_back("Fatigue");
            if (s.lstarUseCps)
                extras.push_back("CPS");
            std::string extra;
            for (size_t i = 0; i < extras.size(); ++i)
                extra += (i ? (i + 1 == extras.size() ? " and " : ", ") : "") + extras[i];
            card.scoreCaption = fmt::format("NaN GD's precision number: to finish from 0% in about {}{}",
                                            duration(ls.m_targetSeconds),
                                            extra.empty() ? "" : fmt::format(", with {} on", extra));
            card.noScore = "No L* for these windows.";

            bool const gbot = disguised();
            card.footer = branded("Made with GucciBot");
            std::tm const tm = localNow();
            char date[32];
            std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
            card.date = date;

            auto const res = geode::Mod::get()->getResourcesDir();
            job.regularFont = res / "Roboto-Regular.ttf";
            job.boldFont = res / "Roboto-Bold.ttf";
            if (!gbot)  // the cone is GucciBot's: none on GBot's picture
                job.mark = res / "cone_silhouette_256.png";

            char stamp[32];
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", &tm);
            std::string base = fmt::format("{} windows {}", safeName(gb->replayName), stamp);
            fs::path out = sharesDir() / u8path(base + ".png");
            std::error_code ec;
            for (int n = 2; fs::exists(out, ec) && n < 100; ++n)
                out = sharesDir() / u8path(base + fmt::format(" ({})", n) + ".png");
            job.out = out;
            return "";
        }

    } // namespace

    Status const& status() { return g_status; }

    std::string whyNot() {
        // The checks collect() starts with, without the copying.
        auto* gb = GucciEngine::get();
        auto& fw = ::Bot::get()->frameWindow();
        if (fw.running())
            return "Share my windows waits for the run to finish.";
        if (fw.results().empty())
            return !gb->replay.m_actionAtom.m_actions.empty()
                       ? "Nothing to share yet: this macro has no windows. Press Calculate first."
                       : "Nothing to share yet: load a macro and press Calculate first.";
        for (auto const& mk : fw.results())
            if (fw.legendTierFor(mk))
                return "";
        return "Nothing to share: none of these windows show on the legend (each is desynced, hidden, or in no band "
               "that's shown).";
    }

    std::string start() {
        if (g_status.busy)
            return "A picture is already being made.";
        auto job = std::make_shared<Job>();
        job->started = std::chrono::steady_clock::now();
        // collect() builds the file's path; a name the system can't take
        // throws, and this runs inside the menu's frame, so catch it here.
        try {
            if (std::string why = collect(*job); !why.empty())
                return why;
        } catch (std::exception const& e) {
            return std::string("Couldn't share: ") + e.what();
        } catch (...) {
            return "Couldn't share this macro's windows.";
        }
        g_status.gameMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job->started).count();
        g_status.busy = true;
        std::thread([job]() {
            std::string error;
            try {
                error = make(*job);
            } catch (std::exception const& e) {
                error = std::string("couldn't make the picture: ") + e.what();
            } catch (...) {
                error = "couldn't make the picture";
            }
            double const seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - job->started).count();
            geode::queueInMainThread([job, error, seconds]() {
                auto const& c = job->card;
                g_status.level = c.level;
                g_status.details = c.details;
                g_status.score = c.haveScore ? c.score : "";
                g_status.bands.clear();
                for (auto const& b : c.bands)
                    g_status.bands.push_back(fmt::format("{}: {}", b.name, b.count));
                g_status.tight.clear();
                for (auto const& t : c.tight)
                    g_status.tight.push_back(fmt::format("#{} {}, {:.1f}%, {} frame{}, {:.2f} ms", t.number, t.kind,
                                                         t.percent, t.frames, t.frames == "1" ? "" : "s", t.ms));
                g_status.busy = false;
                g_status.done = true;
                g_status.ok = error.empty();
                g_status.error = error;
                g_status.seconds = seconds;
                if (error.empty()) {
                    g_status.path = geode::utils::string::pathToString(job->out);
                    g_status.saved++;
                    geode::log::info("[share] saved {} in {:.2f} s", g_status.path, seconds);
                } else {
                    geode::log::warn("[share] failed: {}", error);
                }
            });
        }).detach();
        return "";
    }

    void button() {
        bool const busy = g_status.busy;
        std::string const why = busy ? "" : whyNot();
        ImGui::BeginDisabled(busy || !why.empty());
        if (kit::Button(busy ? "Saving..." : "Share my windows")) {
            if (std::string const failed = start(); !failed.empty()) {
                g_status.done = true;
                g_status.ok = false;
                g_status.error = failed;
            }
        }
        ImGui::EndDisabled();
    }

    void notes() {
        if (g_status.busy)
            return;
        if (std::string const why = whyNot(); !why.empty()) {
            kit::Hint(why.c_str());
            return;
        }
        if (!g_status.done) {
            kit::Hint("Share my windows: a picture of these windows (each band's count, the tightest ones and L*) "
                      "for Discord or YouTube.");
            return;
        }
        if (!g_status.ok) {
            kit::Note(fmt::format("Couldn't save the picture: {}", g_status.error).c_str(), Tone::Warn);
            return;
        }
        kit::Note(fmt::format("Saved to {}", g_status.path).c_str(), Tone::Muted);
#if defined(GEODE_IS_DESKTOP)
        if (kit::Button("Open folder")) {
            std::error_code ec;
            fs::create_directories(sharesDir(), ec);
            (void)geode::utils::file::openFolder(sharesDir());
        }
#endif
    }

} // namespace gucci::ui::share

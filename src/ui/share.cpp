// Share my windows (ui/share.hpp): what goes on the picture, the thread that
// makes it, and the button.

#include "ui/share.hpp"
#include "core/geode_compat.hpp" // gucci::pathToString

#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"
#include "ui/share_draw.hpp"
#include "ui/ui.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"
#include "core/GucciBot.hpp"
#include "core/gbr6_format.hpp"
#include "tools/share_bundle.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <tuple>
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
            // The macro file next to the picture (Nigel, 2026-11-15: "an image
            // plus like... a full macro+fwc thing u can load up"): the macro
            // and its sidecars as saved, read and packed on the worker
            // (tools/share_bundle). No macro file when bundleSkip says why.
            fs::path macroFile;
            std::vector<uint8_t> macroBytes;  // read on the game's thread, after it was checked (sameAsSaved)
            std::string macroName;  // its file name in the .zip, UTF-8
            std::vector<std::pair<std::string, fs::path>> sidecars;  // ".fwac" -> replays/sidecars/<file>.fwac
            std::string bundleSkip;
            std::string madeWith;
            fs::path outZip;
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

        fs::path sharesDir() { return gucci::stdPath(geode::Mod::get()->getSaveDir()) / "shares"; }

        std::vector<uint8_t> readFile(fs::path const& path) {
            std::ifstream in(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        // Written whole or not at all: to <file>.part, then renamed.
        std::string writeWhole(fs::path const& to, std::vector<uint8_t> const& bytes, char const* what) {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            if (ec)
                return "couldn't make the shares folder: " + ec.message();
            fs::path tmp = to;
            tmp += ".part";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return "couldn't write in the shares folder";
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out) {
                    out.close();
                    fs::remove(tmp, ec);
                    return fmt::format("couldn't write the {} (is the disk full?)", what);
                }
            }
            fs::rename(tmp, to, ec);
            if (ec) {
                std::error_code rmEc;
                fs::remove(tmp, rmEc);
                return fmt::format("couldn't save the {}: {}", what, ec.message());
            }
            return "";
        }

        // GucciBot (2026-11-16, the ui2 review): the file holds the macro in
        // memory. The Frame Editor's edits stay in memory until Save, and a
        // name typed into the name field (not saved yet) can point at another
        // macro's file; either way the windows on the picture were measured
        // on clicks that file doesn't have. Compares what the loader would
        // make of the file (GucciReplaySystem::load: inputs, sub-ticks, TPS
        // changes) with the inputs and TPS changes in memory.
        bool sameAsSaved(std::vector<uint8_t> const& bytes, std::vector<gb::Action> const& actions) {
            if (bytes.size() < 4 || std::memcmp(bytes.data(), "GBR6", 4) != 0)
                return false;
            auto f = GBR6File::deserialize(bytes.data(), bytes.size());
            if (!f)
                return false;
            // (frame, button or TPS, held, player 2, sub-tick or TPS rate)
            using Key = std::tuple<uint32_t, int, bool, bool, double>;
            std::vector<Key> file, mem;
            auto const sub = [](double v) { return v > 0.0 && v < 1.0 ? v : 0.0; };
            std::vector<gb::Action> loaded;
            for (auto const* list : {&f->p1Inputs, &f->p2Inputs})
                for (auto const& in : *list) {
                    gb::Action a;
                    a.m_frame = in.frame;
                    a.m_type = static_cast<gb::ActionType>(in.button);
                    a.m_holding = in.pressed;
                    a.m_player2 = in.player2;
                    loaded.push_back(a);
                }
            for (auto const& st : f->subticks)
                for (auto& a : loaded)
                    if (a.m_frame == st.frame && (uint8_t)a.m_type == st.button && a.m_holding == st.pressed &&
                        a.m_player2 == st.player2) {
                        a.m_subtick = st.offset;
                        break;
                    }
            for (auto const& a : loaded)
                file.emplace_back(a.m_frame, (int)a.m_type, a.m_holding, a.m_player2, sub(a.m_subtick));
            for (auto const& t : f->tpsChanges)
                if (t.frame > 0)
                    file.emplace_back(t.frame, (int)gb::ActionType::TPS, false, false, t.tps);
            for (auto const& a : actions) {
                if (a.isInput())
                    mem.emplace_back(a.m_frame, (int)a.m_type, a.m_holding, a.m_player2, sub(a.m_subtick));
                else if (a.m_type == gb::ActionType::TPS && a.m_frame > 0)
                    mem.emplace_back(a.m_frame, (int)gb::ActionType::TPS, false, false, a.m_tps);
            }
            if (file.size() != mem.size())
                return false;
            std::sort(file.begin(), file.end());
            std::sort(mem.begin(), mem.end());
            for (size_t i = 0; i < file.size(); ++i) {
                auto const& [ff, ft, fh, fp, fv] = file[i];
                auto const& [mf, mt, mh, mp, mv] = mem[i];
                if (ff != mf || ft != mt || fh != mh || fp != mp || std::abs(fv - mv) > 1e-9)
                    return false;
            }
            return true;
        }

        // The worker's macro file: the macro and its sidecars, read now and
        // packed. Returns "" or why it failed.
        std::string makeBundle(Job const& job) {
            bundle::Share share;
            share.macroFile = job.macroName;
            share.macro = job.macroBytes;
            if (share.macro.empty())
                return "couldn't read the macro file";
            for (auto const& [ext, path] : job.sidecars) {
                std::error_code ec;
                if (!fs::exists(path, ec))
                    continue;  // gone since (a Clear): left out
                share.sidecars.push_back({ext, readFile(path)});
            }
            share.level = job.card.level;
            share.madeWith = job.madeWith;
            std::string error;
            auto const zip = bundle::pack(share, error);
            if (zip.empty())
                return "couldn't pack the macro: " + error;
            return writeWhole(job.outZip, zip, "macro file");
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
            return writeWhole(job.out, file, "picture");
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
            // GucciBot (2026-11-17): GD 2.2091's Ignore Damage was on while
            // these were counted (FrameWindowAnalyzer::gdIgnoreDamage).
            if (fw.gdIgnoreDamage())
                card.warning = "NOT LEGIT: GD'S IGNORE DAMAGE WAS ON WHILE THESE WERE COUNTED";

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

            auto const res = gucci::stdPath(geode::Mod::get()->getResourcesDir());
            job.regularFont = res / "Roboto-Regular.ttf";
            job.boldFont = res / "Roboto-Bold.ttf";
            if (!gbot)  // the cone is GucciBot's: none on GBot's picture
                job.mark = res / "cone_silhouette_256.png";

            // The macro file: the macro and its sidecars as saved. The windows
            // on the picture are the ones in its .fwac (each change to them is
            // written there as it's made, saveAcFrameWindowResults). A macro
            // not saved yet, or windows that belong to no file yet (a new
            // recording made over a loaded macro), have no file to send; the
            // picture is still made.
            std::error_code fec;
            fs::path const macroPath = gb->replay.getCurrentPath();
            if (gb->replayName.empty() || macroPath.empty() || !fs::is_regular_file(macroPath, fec)) {
                job.bundleSkip = "the macro isn't saved. Save it, then share again for a file others can load.";
            } else if (gb->fwWindowsForgotten) {
                job.bundleSkip = "these windows aren't saved with a macro yet. Save it, then share again for a file "
                                 "others can load.";
            } else if (job.macroBytes = readFile(macroPath);
                       !sameAsSaved(job.macroBytes, gb->replay.m_actionAtom.m_actions)) {
                job.macroBytes.clear();
                job.bundleSkip = "the macro has changes that aren't saved (or the name box names another macro). "
                                 "Save it, then share again for a file others can load.";
            } else {
                job.macroFile = macroPath;
                std::string const file = gucci::pathToString(macroPath.filename());
                // Its sidecars are named after the whole file name
                // (replays/sidecars/<file>.fwac); one more dot is another
                // file's (<file>.bak.fwac, a backup's), left out.
                std::string const prefix = file + ".";
                for (auto const& e : fs::directory_iterator(macroPath.parent_path() / "sidecars", fec)) {
                    std::error_code typeEc;
                    if (!e.is_regular_file(typeEc))
                        continue;
                    std::string const n = gucci::pathToString(e.path().filename());
                    if (n.size() <= prefix.size() || n.compare(0, prefix.size(), prefix) != 0)
                        continue;
                    std::string const ext = n.substr(file.size());
                    if (bundle::sidecarExtOk(ext))
                        job.sidecars.emplace_back(ext, e.path());
                }
                job.macroName = bundle::fileNameOk(file)
                                    ? file
                                    : safeName(gb->replayName) +
                                          gucci::pathToString(macroPath.extension());
                job.madeWith = gbot ? std::string("GBot")
                                    : "GucciBot " + geode::Mod::get()->getVersion().toVString();
            }

            char stamp[32];
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", &tm);
            std::string base = fmt::format("{} windows {}", safeName(gb->replayName), stamp);
            // The picture and the macro file share a name, and neither may
            // replace a file already there.
            auto const named = [&](std::string const& b) {
                job.out = sharesDir() / u8path(b + ".png");
                job.outZip = sharesDir() / u8path(b + ".zip");
            };
            named(base);
            std::error_code ec;
            for (int n = 2; (fs::exists(job.out, ec) || fs::exists(job.outZip, ec)) && n < 100; ++n)
                named(base + fmt::format(" ({})", n));
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
            std::string bundleError = job->bundleSkip;
            if (bundleError.empty()) {
                try {
                    bundleError = makeBundle(*job);
                } catch (std::exception const& e) {
                    bundleError = std::string("couldn't make the macro file: ") + e.what();
                } catch (...) {
                    bundleError = "couldn't make the macro file";
                }
            }
            double const seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - job->started).count();
            geode::queueInMainThread([job, error, bundleError, seconds]() {
                auto const& c = job->card;
                g_status.level = c.level;
                g_status.details = c.details;
                g_status.warning = c.warning;
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
                    g_status.path = gucci::pathToString(job->out);
                    g_status.folder = gucci::pathToString(job->out.parent_path());
                    g_status.pictureName = gucci::pathToString(job->out.filename());
                    g_status.saved++;
                    geode::log::info("[share] saved {} in {:.2f} s", g_status.path, seconds);
                } else {
                    geode::log::warn("[share] failed: {}", error);
                }
                g_status.bundleOk = bundleError.empty();
                g_status.bundleError = bundleError;
                g_status.bundlePath = g_status.bundleOk ? gucci::pathToString(job->outZip) : "";
                g_status.bundleName = g_status.bundleOk ? gucci::pathToString(job->outZip.filename()) : "";
                if (g_status.bundleOk)
                    geode::log::info("[share] saved {} ({} sidecar file(s) found)", g_status.bundlePath,
                                     job->sidecars.size());
                else
                    geode::log::info("[share] no macro file: {}", bundleError);
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
            kit::Hint(branded("Share my windows: a picture of these windows (each band's count, the tightest ones and "
                              "L*) for Discord or YouTube, and a .zip with the macro and its windows that anyone with "
                              "GucciBot can load.")
                          .c_str());
            return;
        }
        if (!g_status.ok) {
            kit::Note(fmt::format("Couldn't save the picture: {}", g_status.error).c_str(), Tone::Warn);
            if (g_status.bundleOk)
                kit::Note(fmt::format("The macro file was saved: {}", g_status.bundlePath).c_str(), Tone::Muted);
            return;
        }
        if (g_status.bundleOk) {
            // Both files, on the one line.
            kit::Note(fmt::format("Saved to {}: {} and {}", g_status.folder, g_status.pictureName, g_status.bundleName)
                          .c_str(),
                      Tone::Muted);
            kit::Hint(branded("The .zip holds the macro and its windows: anyone with GucciBot loads it with Import a "
                              "macro (Saved macros) and sees the same windows.")
                          .c_str());
        } else {
            kit::Note(fmt::format("Saved to {}", g_status.path).c_str(), Tone::Muted);
            kit::Hint(fmt::format("No macro file with it: {}", g_status.bundleError).c_str());
        }
#if defined(GEODE_IS_DESKTOP)
        if (kit::Button("Open folder")) {
            std::error_code ec;
            fs::create_directories(sharesDir(), ec);
            (void)geode::utils::file::openFolder(sharesDir());
        }
#endif
    }

} // namespace gucci::ui::share

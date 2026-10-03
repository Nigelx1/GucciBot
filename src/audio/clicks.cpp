// Click sounds: jump presses and releases, played from a click pack through
// the game's own FMOD system. Written from scratch for GucciBot by Claude on
// 2026-10-02/03 (clicks.hpp says what a pack is and how it is laid out).
// Nothing in it comes from ToastyReplay.

#include "audio/clicks.hpp"
#include "audio/playsound.hpp"

#include "absense/glue.hpp"
#include "core/GucciBot.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <map>
#include <random>
#include <string_view>
#include <system_error>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace gucci::clicks {

    namespace {

        constexpr char const* kDefaultPack = "Clickbot";
        // Built-in sounds are resource files named clickpack_<pack>_<folder>_<n>.
        constexpr std::string_view kResourcePrefix = "clickpack_";
        // Sounds playing at once; past this the oldest one is cut short.
        constexpr size_t kMaxVoices = 16;

        enum Kind : int { Click, Release, SoftClick, SoftRelease, KindCount };

        // Each kind's folder in a pack, then the name other bots' packs use.
        constexpr std::array<std::array<char const*, 2>, KindCount> kFolders{{
            {"clicks", "hardClicks"},
            {"releases", "hardReleases"},
            {"softClicks", "microClicks"},
            {"softReleases", "microReleases"},
        }};

        // What plays when a pack has none of the kind asked for.
        constexpr std::array<Kind, KindCount> kStandIn{SoftClick, SoftRelease, Click, Release};

        using FileSet = std::array<std::vector<fs::path>, KindCount>;

        struct Bank {
            std::vector<FMOD::Sound*> sounds;
            int last = -1;  // the one played last, so it isn't picked twice running
        };

        struct Pack {
            std::string name;                // the pack whose sounds play
            bool missing = false;            // the chosen one is gone; this is the stand-in
            bool builtIn = false;
            FMOD::System* system = nullptr;  // what made the sounds; null = none made yet
            std::vector<FMOD::Sound*> owned; // every sound once (the banks share them)
            std::array<std::array<Bank, KindCount>, 2> banks;   // [player][kind]
            std::array<std::array<int, KindCount>, 2> files{};  // sound files found
            int unreadable = 0;
        };

        struct BuiltIn {
            std::string name;
            FileSet files;
        };

        // When a player last pressed and let go, for the soft-click window.
        struct Timing {
            bool down = false;
            bool havePress = false;
            bool haveRelease = false;
            double pressMs = 0.0;
            double releaseMs = 0.0;
        };

        FMOD::System* g_system = nullptr;
        std::map<std::string, Pack> g_packs;  // keyed by the name the settings hold
        std::vector<FMOD::Channel*> g_voices;
        std::vector<BuiltIn> g_builtIns;
        std::vector<std::string> g_names;
        bool g_namesValid = false;
        std::array<Timing, 2> g_timing;
        PlayLayer* g_clockLayer = nullptr;
        int g_clockAttempt = -1;

        // ---- names and files

        // Paths go to FMOD and the interface as UTF-8: path::string() would use
        // the ANSI code page and throw on names it cannot hold.
        std::string utf8(fs::path const& p) {
            auto const s = p.u8string();
            return std::string(reinterpret_cast<char const*>(s.data()), s.size());
        }

        fs::path fromUtf8(std::string const& s) {
            return fs::path(std::u8string(reinterpret_cast<char8_t const*>(s.data()), s.size()));
        }

        std::string lowerAscii(std::string s) {
            for (auto& c : s) {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        }

        bool sameName(std::string const& a, std::string const& b) {
            return lowerAscii(a) == lowerAscii(b);
        }

        bool isSoundFile(fs::path const& p) {
            std::wstring ext = p.extension().wstring();
            for (auto& c : ext)
                c = static_cast<wchar_t>(std::towlower(c));
            return ext == L".wav" || ext == L".mp3" || ext == L".ogg" || ext == L".flac" || ext == L".aif" ||
                   ext == L".aiff";
        }

        // A folder name, not a path that could lead out of clickpacks/.
        bool plainName(std::string const& name) {
            return !name.empty() && name != "." && name != ".." &&
                   name.find_first_of("/\\:") == std::string::npos;
        }

        int kindNamed(std::string const& folder) {
            std::string const want = lowerAscii(folder);
            for (int k = 0; k < KindCount; ++k) {
                for (char const* alias : kFolders[k]) {
                    if (lowerAscii(alias) == want)
                        return k;
                }
            }
            return -1;
        }

        // The sound files directly inside a folder, in name order. Packs often
        // keep a background noise track beside loose clicks; skipNoise leaves
        // it out.
        std::vector<fs::path> soundsIn(fs::path const& dir, bool skipNoise) {
            std::vector<fs::path> found;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code typeEc;
                if (!it->is_regular_file(typeEc) || !isSoundFile(it->path()))
                    continue;
                if (skipNoise && lowerAscii(utf8(it->path().stem())).find("noise") != std::string::npos)
                    continue;
                found.push_back(it->path());
            }
            std::sort(found.begin(), found.end());
            return found;
        }

        // One kind's folder inside a pack (or player) folder; empty if it has none.
        fs::path folderOf(fs::path const& root, Kind kind) {
            for (char const* name : kFolders[kind]) {
                std::error_code ec;
                if (fs::is_directory(root / name, ec))
                    return root / name;
            }
            return {};
        }

        FileSet filesOf(fs::path const& root) {
            FileSet set;
            for (int k = 0; k < KindCount; ++k) {
                fs::path const dir = folderOf(root, static_cast<Kind>(k));
                if (!dir.empty())
                    set[k] = soundsIn(dir, false);
            }
            // No clicks folder: the clicks sit loose in the pack's own folder.
            if (folderOf(root, Click).empty())
                set[Click] = soundsIn(root, true);
            return set;
        }

        // The packs in the mod's resources, read from their file names. The
        // default pack comes first, the rest by name.
        std::vector<BuiltIn> scanBuiltIns() {
            std::vector<BuiltIn> packs;
            std::error_code ec;
            fs::path const dir = Mod::get()->getResourcesDir();
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code typeEc;
                if (!it->is_regular_file(typeEc) || !isSoundFile(it->path()))
                    continue;
                std::string const stem = utf8(it->path().stem());
                if (!stem.starts_with(kResourcePrefix))
                    continue;
                // <pack>_<folder>_<n>; the pack's own name may hold underscores.
                std::string const rest = stem.substr(kResourcePrefix.size());
                auto const numberAt = rest.rfind('_');
                if (numberAt == std::string::npos || numberAt == 0)
                    continue;
                auto const folderAt = rest.rfind('_', numberAt - 1);
                if (folderAt == std::string::npos || folderAt == 0)
                    continue;
                int const kind = kindNamed(rest.substr(folderAt + 1, numberAt - folderAt - 1));
                if (kind < 0)
                    continue;
                std::string const name = rest.substr(0, folderAt);
                auto pack = std::find_if(packs.begin(), packs.end(), [&](BuiltIn const& b) { return b.name == name; });
                if (pack == packs.end())
                    pack = packs.insert(packs.end(), BuiltIn{name, {}});
                pack->files[kind].push_back(it->path());
            }
            for (auto& pack : packs) {
                for (auto& files : pack.files)
                    std::sort(files.begin(), files.end());
            }
            std::sort(packs.begin(), packs.end(), [](BuiltIn const& a, BuiltIn const& b) {
                bool const aDefault = a.name == kDefaultPack;
                bool const bDefault = b.name == kDefaultPack;
                if (aDefault != bDefault)
                    return aDefault;
                return lowerAscii(a.name) < lowerAscii(b.name);
            });
            return packs;
        }

        BuiltIn const* builtInNamed(std::string const& name) {
            for (auto const& pack : g_builtIns) {
                if (sameName(pack.name, name))
                    return &pack;
            }
            return nullptr;
        }

        // ---- loading

        FMOD::System* audioSystem() {
            auto* engine = FMODAudioEngine::sharedEngine();
            return engine ? engine->m_system : nullptr;
        }

        void unload(Pack& pack) {
            // Sounds made by an earlier sound system went away with it.
            if (pack.system && pack.system == g_system) {
                for (auto* sound : pack.owned)
                    sound->release();
            }
            pack.owned.clear();
            pack.system = nullptr;
        }

        // Find a pack's files and, when the game's sound system is up, open
        // each once. A pack that is gone is stood in for by the default one.
        Pack load(std::string const& wanted, FMOD::System* sys) {
            Pack pack;
            std::array<FileSet, 2> sets;  // [player]
            bool shared = true;           // player 2 plays player 1's sounds
            std::error_code ec;
            fs::path const root = plainName(wanted) ? packsDir() / fromUtf8(wanted) : fs::path();

            if (auto const* b = builtInNamed(wanted)) {
                pack.name = b->name;
                pack.builtIn = true;
                sets[0] = b->files;
            } else if (!root.empty() && fs::is_directory(root, ec)) {
                pack.name = wanted;
                if (fs::is_directory(root / "player1", ec)) {
                    sets[0] = filesOf(root / "player1");
                    if (fs::is_directory(root / "player2", ec)) {
                        sets[1] = filesOf(root / "player2");
                        shared = false;
                    }
                } else {
                    sets[0] = filesOf(root);
                }
            } else {
                pack.missing = true;
                auto const* standIn = builtInNamed(kDefaultPack);
                if (!standIn && !g_builtIns.empty())
                    standIn = &g_builtIns.front();
                pack.name = standIn ? standIn->name : wanted;
                pack.builtIn = standIn != nullptr;
                if (standIn)
                    sets[0] = standIn->files;
            }

            pack.system = sys;
            int warned = 0;
            for (int player = 0; player < (shared ? 1 : 2); ++player) {
                for (int k = 0; k < KindCount; ++k) {
                    pack.files[player][k] = static_cast<int>(sets[player][k].size());
                    if (!sys)
                        continue;
                    for (auto const& file : sets[player][k]) {
                        // Decoded into memory up front: a click has to start
                        // the moment it is asked for.
                        FMOD::Sound* sound = nullptr;
                        FMOD_RESULT const res = sys->createSound(
                            utf8(file).c_str(), FMOD_DEFAULT | FMOD_LOOP_OFF | FMOD_CREATESAMPLE, nullptr, &sound);
                        if (res != FMOD_OK || !sound) {
                            ++pack.unreadable;
                            if (warned++ < 5)
                                log::warn("Click sounds: could not open {} (FMOD error {})", utf8(file),
                                          static_cast<int>(res));
                            continue;
                        }
                        pack.owned.push_back(sound);
                        pack.banks[player][k].sounds.push_back(sound);
                    }
                }
            }
            if (shared) {
                pack.banks[1] = pack.banks[0];
                pack.files[1] = pack.files[0];
            }

            if (sys) {
                auto const& b = pack.banks[0];
                log::info("Click sounds: loaded {}{}{}: {} clicks, {} releases, {} soft clicks, {} soft releases{}",
                          pack.name, pack.missing ? fmt::format(" (standing in for {})", wanted) : std::string(),
                          shared ? "" : " (per player)", b[Click].sounds.size(), b[Release].sounds.size(),
                          b[SoftClick].sounds.size(), b[SoftRelease].sounds.size(),
                          pack.unreadable ? fmt::format(", {} unreadable", pack.unreadable) : std::string());
            }
            return pack;
        }

        // The pack a player's sounds come from, loaded the first time it is
        // needed. Packs neither player uses any more are let go.
        Pack& packFor(bool p2) {
            FMOD::System* const sys = audioSystem();
            if (sys != g_system) {
                // The first sound system, or a new one: anything made with the
                // old one is already gone, so drop it without releasing.
                g_packs.clear();
                g_voices.clear();
                g_system = sys;
            }

            auto const& s = settings();
            std::string const& wanted = (p2 && s.separateP2) ? s.p2Pack : s.pack;
            for (auto it = g_packs.begin(); it != g_packs.end();) {
                bool const used = it->first == s.pack || (s.separateP2 && it->first == s.p2Pack);
                if (used) {
                    ++it;
                    continue;
                }
                unload(it->second);
                it = g_packs.erase(it);
            }

            auto it = g_packs.find(wanted);
            // Found before the sound system was up: nothing was opened, look again.
            if (it != g_packs.end() && !it->second.system && sys) {
                g_packs.erase(it);
                it = g_packs.end();
            }
            if (it == g_packs.end()) {
                packNames();  // the built-in packs are known
                it = g_packs.emplace(wanted, load(wanted, sys)).first;
            }
            return it->second;
        }

        // ---- playing

        std::mt19937& rng() {
            static std::mt19937 gen{std::random_device{}()};
            return gen;
        }

        FMOD::Sound* pick(Bank& bank) {
            int const count = static_cast<int>(bank.sounds.size());
            if (count == 0)
                return nullptr;
            int i = 0;
            if (count > 1) {
                bool const avoidLast = bank.last >= 0 && bank.last < count;
                i = std::uniform_int_distribution<int>(0, count - (avoidLast ? 2 : 1))(rng());
                if (avoidLast && i >= bank.last)
                    ++i;
            }
            bank.last = i;
            return bank.sounds[static_cast<size_t>(i)];
        }

        FMOD::Sound* soundFor(Pack& pack, bool p2, Kind kind) {
            auto& banks = pack.banks[p2 ? 1 : 0];
            if (auto* sound = pick(banks[kind]))
                return sound;
            return pick(banks[kStandIn[kind]]);
        }

        void play(FMOD::Sound* sound) {
            if (!sound || !g_system)
                return;
            std::erase_if(g_voices, [](FMOD::Channel* voice) {
                bool playing = false;
                return voice->isPlaying(&playing) != FMOD_OK || !playing;
            });
            while (g_voices.size() >= kMaxVoices) {
                g_voices.front()->stop();
                g_voices.erase(g_voices.begin());
            }
            // Started paused so the volume is in place before the first sample.
            FMOD::Channel* voice = nullptr;
            if (g_system->playSound(sound, nullptr, true, &voice) != FMOD_OK || !voice)
                return;
            voice->setVolumeRamp(false);
            voice->setVolume(std::clamp(settings().volume, 0.f, 2.f));
            voice->setPaused(false);
            g_voices.push_back(voice);
        }

        // ---- timing

        void forgetTimes() {
            for (auto& t : g_timing) {
                t.havePress = false;
                t.haveRelease = false;
            }
        }

        // Milliseconds: the game's clock in a level, the wall clock elsewhere.
        // A new attempt (or level) starts the soft-click timing over, since the
        // game's clock goes back with it.
        double nowMs() {
            if (auto* pl = PlayLayer::get()) {
                if (pl != g_clockLayer || pl->m_attempts != g_clockAttempt) {
                    g_clockLayer = pl;
                    g_clockAttempt = pl->m_attempts;
                    forgetTimes();
                }
                auto const& updater = GucciEngine::get()->updater;
                double const tps = updater.m_tps > 0.0 ? updater.m_tps : 240.0;
                return static_cast<double>(updater.getFrame()) * 1000.0 / tps;
            }
            if (g_clockLayer) {
                g_clockLayer = nullptr;
                forgetTimes();
            }
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        }

        void onButton(bool p2, bool pressed) {
            auto& t = g_timing[p2 ? 1 : 0];
            double const now = nowMs();
            int const softMs = settings().softMs;
            auto const within = [&](bool have, double since) {
                double const gap = now - since;
                return softMs > 0 && have && gap >= 0.0 && gap < static_cast<double>(softMs);
            };

            Kind kind = Click;
            if (pressed) {
                kind = within(t.haveRelease, t.releaseMs) ? SoftClick : Click;
                t.down = true;
                t.havePress = true;
                t.pressMs = now;
            } else {
                // Resets let go of buttons whether or not they were down; only
                // a release of a press that was heard makes a sound.
                if (!t.down)
                    return;
                kind = within(t.havePress, t.pressMs) ? SoftRelease : Release;
                t.down = false;
                t.haveRelease = true;
                t.releaseMs = now;
            }
            Pack& pack = packFor(p2);
            play(soundFor(pack, p2, kind));
        }

    } // namespace

    Settings& settings() {
        static Settings s = [] {
            Settings v;
            auto* mod = Mod::get();
            v.enabled = mod->getSavedValue<bool>("clicks_enabled", v.enabled);
            v.pack = mod->getSavedValue<std::string>("clicks_pack", v.pack);
            v.volume = std::clamp(static_cast<float>(mod->getSavedValue<double>("clicks_volume", v.volume)), 0.f, 2.f);
            v.softMs = std::clamp(mod->getSavedValue<int>("clicks_soft_ms", v.softMs), 0, 1000);
            v.duringPlayback = mod->getSavedValue<bool>("clicks_playback", v.duringPlayback);
            v.inRenders = mod->getSavedValue<bool>("clicks_render", v.inRenders);
            v.separateP2 = mod->getSavedValue<bool>("clicks_p2_separate", v.separateP2);
            v.p2Pack = mod->getSavedValue<std::string>("clicks_p2_pack", v.p2Pack);
            return v;
        }();
        return s;
    }

    void saveSettings() {
        auto const& s = settings();
        auto* mod = Mod::get();
        mod->setSavedValue<bool>("clicks_enabled", s.enabled);
        mod->setSavedValue<std::string>("clicks_pack", s.pack);
        mod->setSavedValue<double>("clicks_volume", s.volume);
        mod->setSavedValue<int>("clicks_soft_ms", s.softMs);
        mod->setSavedValue<bool>("clicks_playback", s.duringPlayback);
        mod->setSavedValue<bool>("clicks_render", s.inRenders);
        mod->setSavedValue<bool>("clicks_p2_separate", s.separateP2);
        mod->setSavedValue<std::string>("clicks_p2_pack", s.p2Pack);
    }

    fs::path packsDir() {
        fs::path dir = Mod::get()->getSaveDir() / "clickpacks";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    std::vector<std::string> const& packNames() {
        if (g_namesValid)
            return g_names;
        g_namesValid = true;
        g_builtIns = scanBuiltIns();
        g_names.clear();
        for (auto const& pack : g_builtIns)
            g_names.push_back(pack.name);

        std::vector<std::string> own;
        std::error_code ec;
        for (fs::directory_iterator it(packsDir(), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeEc;
            if (!it->is_directory(typeEc))
                continue;
            std::string name = utf8(it->path().filename());
            if (builtInNamed(name))
                continue;  // a built-in pack of the same name wins
            own.push_back(std::move(name));
        }
        std::sort(own.begin(), own.end(),
                  [](std::string const& a, std::string const& b) { return lowerAscii(a) < lowerAscii(b); });
        g_names.insert(g_names.end(), own.begin(), own.end());
        return g_names;
    }

    void refresh() {
        for (auto& [name, pack] : g_packs)
            unload(pack);
        g_packs.clear();
        g_voices.clear();
        g_namesValid = false;
        packNames();
    }

    PackStatus status(bool p2) {
        Pack& pack = packFor(p2);
        PackStatus st;
        st.name = pack.name;
        st.missing = pack.missing;
        st.builtIn = pack.builtIn;
        st.audioReady = g_system != nullptr;
        int const player = p2 ? 1 : 0;
        auto const count = [&](Kind k) {
            return pack.system ? static_cast<int>(pack.banks[player][k].sounds.size()) : pack.files[player][k];
        };
        st.clicks = count(Click);
        st.releases = count(Release);
        st.softClicks = count(SoftClick);
        st.softReleases = count(SoftRelease);
        st.unreadable = pack.unreadable;
        return st;
    }

    void playClick(bool p2) {
        Pack& pack = packFor(p2);
        play(soundFor(pack, p2, Click));
    }

} // namespace gucci::clicks

namespace gucci {

    void triggerClickAudio(bool p2, int button, bool pressed) {
        if (button != 1)
            return;
        auto const& s = clicks::settings();
        if (!s.enabled)
            return;
        auto* gb = GucciEngine::get();
        if (gb->isPlaying() && !s.duringPlayback)
            return;
        if (!s.inRenders && SLRenderer::get()->isRecording())
            return;
        // The analyzer's legs and the pathfinder's tries are their inputs, not
        // the player's, and they come by the thousand.
        if (gb->analyzerOwnsRun() || absense::isRunning())
            return;
        clicks::onButton(p2, pressed);
    }

} // namespace gucci

// Open the chosen packs once the game is up, so the first click of a session
// doesn't stall while they load.
$on_game(Loaded) {
    auto const& s = gucci::clicks::settings();
    if (!s.enabled)
        return;
    gucci::clicks::status(false);
    if (s.separateP2)
        gucci::clicks::status(true);
}

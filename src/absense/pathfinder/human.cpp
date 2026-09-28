#include "human.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#include "absense/compat/reference.hpp"
#include "absense/compat/devlog.hpp"

using namespace geode::prelude;

namespace absense::human {

namespace {

std::string fold(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    s.erase(0, i);
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

Reference& Reference::get() {
    static Reference instance;
    return instance;
}

void Reference::close() {
    m_events.clear();
    m_source.clear();
    m_lastSecond = 0.0;
    m_presses = 0;
}

void Reference::open(PlayLayer* pl, const slc::ActionAtom* loaded, double loadedTps) {
    close();
    if (!pl || !pl->m_level) return;
    const std::string name = fold(std::string(pl->m_level->m_levelName));

    // The folder: the file named after the level.
    if (!name.empty()) {
        const auto folder = Mod::get()->getPersistentDir() / "replays";
        std::error_code ec;
        std::filesystem::path exact, prefix;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".slc") continue;
            const std::string stem = fold(entry.path().stem().string());
            if (stem == name) {
                exact = entry.path();
                break;
            }
            if (prefix.empty() && stem.size() > name.size() && stem.compare(0, name.size(), name) == 0 &&
                std::isspace((unsigned char)stem[name.size()])) {
                prefix = entry.path();
            }
        }
        const auto path = !exact.empty() ? exact : prefix;
        if (!path.empty()) {
            if (auto r = absense::reference::load(path)) {
                m_tps = r->tps > 0.0 ? r->tps : 240.0;
                // Frames count at whatever rate is current and a type 7
                // event changes it from its frame on: the macro's own time
                // adds up segment by segment.
                double rate = m_tps, segStart = 0.0;
                uint64_t segFrame = 0;
                for (const auto& e : r->events) {
                    const double sec = segStart + (double)(e.frame - segFrame) / rate;
                    if (e.type == 7 && e.tps > 0.0) {
                        segStart = sec;
                        segFrame = e.frame;
                        rate = e.tps;
                        continue;
                    }
                    if (e.type != 1 || e.player2) continue;
                    m_events.push_back(Ev{sec, e.holding});
                    if (e.holding) m_presses++;
                }
                m_source = path.filename().string();
            }
        }
    }

    // Failing that, what the player has loaded.
    if (m_events.empty() && loaded && !loaded->m_actions.empty()) {
        // The rate it is playing at now is all the game can say about the
        // first frames (slc3 does not keep a starting rate once loaded);
        // its own TPS actions carry the rest.
        m_tps = loadedTps > 0.0 ? loadedTps : 240.0;
        double rate = m_tps, segStart = 0.0;
        uint64_t segFrame = 0;
        for (const auto& a : loaded->m_actions) {
            const double sec = segStart + (double)(a.m_frame - segFrame) / rate;
            if (a.m_type == slc::ActionType::TPS && a.m_tps > 0.0) {
                segStart = sec;
                segFrame = a.m_frame;
                rate = a.m_tps;
                continue;
            }
            if (a.m_type != slc::ActionType::Jump || a.m_player2) continue;
            m_events.push_back(Ev{sec, a.m_holding});
            if (a.m_holding) m_presses++;
        }
        m_source = "the loaded macro";
    }

    std::stable_sort(m_events.begin(), m_events.end(), [](const Ev& a, const Ev& b) { return a.second < b.second; });
    m_lastSecond = m_events.empty() ? 0.0 : m_events.back().second;
    if (!m_events.empty()) {
        devlog::logf(devlog::Cat::AbsensePathfinder, "human macro: %s (%zu presses, %.0f tps, %.1f s)", m_source.c_str(),
                     m_presses, m_tps, m_lastSecond);
    }
}

bool Reference::script(uint64_t tick, int count, double gameTps, int shift, bool startHeld,
                       std::vector<TickInput>& out) const {
    if (m_events.empty() || count <= 0 || gameTps <= 0.0) return false;
    // Shifted: the human's script from `shift` ticks earlier / later.
    const double t0 = (double)tick - (double)shift;
    const double s0 = t0 / gameTps;  // ... in the macro's own time
    if (m_lastSecond < s0) return false;  // the human's inputs end before here

    out.assign((size_t)count, TickInput{0, startHeld});
    std::vector<char> touched((size_t)count, 0);

    // The button as the human had it at the first tick.
    auto it = std::lower_bound(m_events.begin(), m_events.end(), s0,
                               [](const Ev& e, double s) { return e.second < s; });
    bool cur = it == m_events.begin() ? false : std::prev(it)->holding;
    if (cur != startHeld) {
        out[0] = cur ? TickInput{1, true} : TickInput{0, false};
        touched[0] = 1;
    }
    for (; it != m_events.end(); ++it) {
        const long long o = std::llround(it->second * gameTps - t0);
        if (o >= count) break;
        const size_t at = (size_t)std::max<long long>(o, 0);
        TickInput& t = out[at];
        if (it->holding) {
            if (!touched[at] || !t.held) t.presses = (uint8_t)std::min(2, t.presses + 1);
            t.held = true;
        } else {
            t.held = false;
        }
        touched[at] = 1;
    }
    // The button stays where it was between events.
    bool held = startHeld;
    for (size_t i = 0; i < out.size(); i++) {
        if (touched[i]) {
            held = out[i].held;
        } else {
            out[i].held = held;
        }
    }
    return true;
}

}  // namespace absense::human

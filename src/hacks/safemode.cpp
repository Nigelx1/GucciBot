#include "hacks/safemode.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "absense/glue.hpp"
#include "analysis/pathfinder.hpp"
#include "hacks/autoclicker.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/hash.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

using namespace geode::prelude;

namespace gucci::safemode {

    namespace {

        // The owner's password: the hex of 10,000 rounds of SHA-256 over
        // kSalt + the password. Empty: nothing unlocks.
        constexpr const char* kOwnerTarget = "4a9a35d0a6ff41536fd6169500a07d495a010d7ad87dc82e6c770b14afe52f98";
        constexpr const char* kSalt = "guccibot-unlock:";
        constexpr int kRounds = 10000;
        constexpr int kHalf = 5000;

        using Digest = std::array<uint8_t, 32>;

        Digest rounds(std::span<uint8_t const> seed, int n) {
            Digest h = geode::sha256(seed).data;
            for (int i = 1; i < n; ++i)
                h = geode::sha256(std::span<uint8_t const>(h.data(), h.size())).data;
            return h;
        }

        std::string hex(Digest const& d) {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s;
            for (auto b : d) {
                s += kDigits[b >> 4];
                s += kDigits[b & 15];
            }
            return s;
        }

        bool unhex(std::string const& s, Digest& out) {
            if (s.size() != 64)
                return false;
            auto nib = [](char c) -> int {
                return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            };
            for (size_t i = 0; i < 32; ++i) {
                int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
                if (hi < 0 || lo < 0)
                    return false;
                out[i] = static_cast<uint8_t>(hi * 16 + lo);
            }
            return true;
        }

        // The 5,000-round midpoint is what an unlocked machine keeps; it only
        // counts if 5,000 more rounds reach the target.
        bool tokenOpens(Digest const& token) {
            return *kOwnerTarget && hex(rounds(std::span<uint8_t const>(token.data(), token.size()), kRounds - kHalf)) ==
                                        kOwnerTarget;
        }

        struct State {
            bool loaded = false;
            bool safe = true;
            bool hidden = false;
            bool green = false;
            bool unlocked = false;
        };

        State& st() {
            static State s;
            if (!s.loaded) {
                s.loaded = true;
                auto* mod = Mod::get();
                s.safe = mod->getSavedValue<bool>("safe_mode_auto", true);
                s.hidden = mod->getSavedValue<bool>("cheat_dot_hidden", false);
                s.green = mod->getSavedValue<bool>("cheat_dot_green", false);
                Digest token{};
                s.unlocked = unhex(mod->getSavedValue<std::string>("owner_unlock", ""), token) && tokenOpens(token);
            }
            return s;
        }

        // A cheat was on at some point in this attempt (reset with the level).
        bool g_tainted = false;

    } // namespace

    bool autoSafeMode() {
        return st().safe;
    }

    void setAutoSafeMode(bool on) {
        st().safe = on;
        Mod::get()->setSavedValue("safe_mode_auto", on);
    }

    bool unlocked() {
        return st().unlocked;
    }

    bool unlock(std::string const& password) {
        std::string const seed = std::string(kSalt) + password;
        Digest const token =
            rounds(std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(seed.data()), seed.size()), kHalf);
        if (!tokenOpens(token))
            return false;
        Mod::get()->setSavedValue<std::string>("owner_unlock", hex(token));
        st().unlocked = true;
        return true;
    }

    void lock() {
        Mod::get()->setSavedValue<std::string>("owner_unlock", "");
        st().unlocked = false;
        setDotHidden(false);
    }

    bool dotHidden() {
        return st().unlocked && st().hidden;
    }

    void setDotHidden(bool hidden) {
        if (hidden && !st().unlocked)
            return;
        st().hidden = hidden;
        Mod::get()->setSavedValue("cheat_dot_hidden", hidden);
    }

    bool greenDot() {
        return st().green;
    }

    void setGreenDot(bool on) {
        st().green = on;
        Mod::get()->setSavedValue("cheat_dot_green", on);
    }

    bool cheatingNow() {
        auto* gb = GucciEngine::get();
        if (gb->standingDown || botswitch::takenOut())
            return false;
        auto const& u = gb->updater;
        return gb->isPlaying()                       // a macro playing
               // The pathfinders record as they play, so isPlaying() never saw
               // them: a pathfinder run on an online level could save a real
               // completion. Both count, and so does Calculate.
               || absense::isRunning() || gucci::Pathfinder::get()->active || gb->analyzerOwnsRun()
               || std::abs(u.m_tps - 240.0) > 1e-6     // physics at another rate
               || std::abs(u.m_speedhack - 1.0) > 1e-6 // speedhack
               || gb->noclipEnabled || u.m_preventDeath
               || u.m_paused // frame advance
               || Autoclicker::get()->enabled;
    }

} // namespace gucci::safemode

using namespace gucci;

namespace {
    // A run safe mode keeps from counting: auto safe mode on, and a cheat on at
    // some point in this attempt (or right now).
    bool shielded() {
        return safemode::autoSafeMode() && (safemode::g_tainted || safemode::cheatingNow());
    }
} // namespace

// GD's own test-mode flag is never left changed. GD sets it for a start position
// (a startpos switch resets around it), and the first version, which overrode it
// every frame, let a startpos run save its death % and crashed a startpos switch
// (dihmaster, 2026-10-12). Instead, as QOLMod's safe mode does it: the flag is
// raised only while levelComplete runs, the new-best popup is skipped, and
// GJGameLevel::savePercentage (where a death's % is saved) is skipped.
class $modify(GB7SafeModePlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* red = nullptr;
        CCDrawNode* green = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        safemode::g_tainted = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        // The dots: top left, on the PlayLayer itself rather than its UI layer
        // (which some mods hide), so recordings and renders show them. Red while
        // a cheat is on with auto safe mode off; green, if wanted, while none is.
        auto const win = CCDirector::get()->getWinSize();
        auto make = [&](ccColor4F color, const char* id) {
            auto* dot = CCDrawNode::create();
            dot->drawDot(ccp(9.f, win.height - 9.f), 4.5f, color);
            dot->setVisible(false);
            dot->setID(id);
            this->addChild(dot, 1000);
            return dot;
        };
        m_fields->red = make(ccc4f(1.f, 0.16f, 0.16f, 1.f), "cheat-dot"_spr);
        m_fields->green = make(ccc4f(0.2f, 0.85f, 0.35f, 1.f), "clean-dot"_spr);
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        safemode::g_tainted = safemode::cheatingNow(); // a new attempt
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        bool const now = safemode::cheatingNow();
        if (now)
            safemode::g_tainted = true;
        bool const cheated = now || safemode::g_tainted; // this attempt
        if (auto* red = m_fields->red)
            red->setVisible(cheated && !safemode::autoSafeMode() && !safemode::dotHidden());
        if (auto* green = m_fields->green)
            green->setVisible(!cheated && safemode::greenDot());
    }

    void levelComplete() {
        bool const was = m_isTestMode;
        if (shielded())
            m_isTestMode = true; // only raised, and only for this call
        PlayLayer::levelComplete();
        m_isTestMode = was;
    }

    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        if (shielded())
            return;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }
};

class $modify(GB7SafeModeLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (PlayLayer::get() && shielded())
            return;
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

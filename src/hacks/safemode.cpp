#include "hacks/safemode.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "absense/glue.hpp"
#include "analysis/pathfinder.hpp"
#include "hacks/autoclicker.hpp"
#include "trainers/click_indicators.hpp"
#include "trainers/trainer_core.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
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

    // Anything that shows the way through the level counts as a cheat too
    // (Nigel, 2026-10-19: it takes the memory out of memory levels): Click
    // Indicators, the Survival Indicator and its click cue, the path preview,
    // the macro's path, its clicks near you (Practice range), and the
    // trainers' ghosts in their own levels -- the macro's, and your best
    // attempt's.
    bool showsTheWay() {
        auto* gb = GucciEngine::get();
        if (clickind::active() || gb->pathPreview)
            return true;
        // The Survival Indicator says, a tick at a time, whether a click now
        // lives: the path preview's question, asked for you (Nigel,
        // 2026-10-28). The cue only plays while it's on.
        if (gb->survivalIndicator)
            return true;
        if (gb->showMacroPath && !gb->replay.m_pathSamples.empty())
            return true;
        if (gb->practiceRangeEnabled && !gb->replay.m_actionAtom.m_actions.empty())
            return true;
        using trainers::Kind;
        if (trainers::followsLevel(Kind::Jupiter) &&
            ((gb->jupiterGhostEnabled && !gb->jupiterMacro.pathSamples.empty()) || gb->jupiterBestGhostEnabled))
            return true;
        if (trainers::followsLevel(Kind::Any) &&
            ((gb->trainerGhostEnabled && !gb->trainerMacro.pathSamples.empty()) || gb->trainerBestGhostEnabled))
            return true;
        return false;
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
               || gb->forcedPlatformer // a level played as platformer that isn't one
               || showsTheWay()       // the way through shown in the level: no memory left to test
               || u.m_paused // frame advance
               || Autoclicker::get()->enabled;
    }

} // namespace gucci::safemode

using namespace gucci;

namespace {
    // A run safe mode keeps from counting: the Safe mode switch on (Hacks >
    // Gameplay - nothing counts, cheat or not), or auto safe mode on and a
    // cheat on at some point in this attempt (or right now).
    // GucciBot (2026-10-23): the switch did nothing until now. It set
    // protectedMode and nothing read it - a half-port since the -bz menu (the
    // old menu only saved it), and it wasn't saved either, so it was off again
    // at every launch. Silicate's SafeMode.cpp is the full version; the stats,
    // achievements and attempt counts it also keeps are at the end of this file.
    bool shielded() {
        if (GucciEngine::get()->protectedMode)
            return true;
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
        // (Red: a cheat with nothing keeping the run from counting.)
        if (auto* red = m_fields->red)
            red->setVisible(cheated && !safemode::autoSafeMode() && !GucciEngine::get()->protectedMode &&
                            !safemode::dotHidden());
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

// GucciBot (2026-10-23): the rest of Silicate's safe mode (its
// src/hooks/SafeMode.cpp, by peony, GPL-3), under the same rule as the rest of
// this file: while a run is kept from counting, GD's stats aren't added to,
// achievements aren't reported, and the level's attempts and jumps and the
// session's attempt counts stay where they were. Hooked very late, as Silicate
// does, so only GD's own change is undone.
//
// Silicate's version is written for GD 2.2091. It also keeps the level's
// practice attempts and raises a "speed changed" flag around a death and a
// completion, both through raw offsets into 2.2091's classes. Neither is
// ported: 2.2081 has no practice attempt count (2.209 added it), and the
// offsets don't hold here; a death's percent and a completion are kept from
// counting above, the way QOLMod does it.
namespace {
    // Puts a value back as it was when the scope ends (Silicate's KeepValue).
    template <class T>
    class KeepValue {
    public:
        explicit KeepValue(T& value) : m_value(value), m_saved(value) {}
        ~KeepValue() {
            m_value = m_saved;
        }
        KeepValue(KeepValue const&) = delete;
        KeepValue& operator=(KeepValue const&) = delete;

    private:
        T& m_value;
        T m_saved;
    };
} // namespace

class $modify(GB7SafeModeCounts, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::resetLevel", Priority::VeryLate);
        (void)self.setHookPriority("PlayLayer::updateAttempts", Priority::VeryLate);
    }

    // A new attempt: the level's attempt count. (Silicate keeps the session's
    // in updateAttempts, which a reset calls; both are kept in both here, in
    // case 2.2081 counts them in the other one.)
    void resetLevel() {
        if (!m_level || !shielded())
            return PlayLayer::resetLevel();
        auto* gm = GameManager::get();
        KeepValue attempts(m_level->m_attempts);
        KeepValue session(gm->m_sessionAttempts);
        KeepValue session2(gm->m_sessionAttempts2);
        KeepValue normal(gm->m_sessionNormalAttempts);
        PlayLayer::resetLevel();
    }

    void updateAttempts() {
        if (!m_level || !shielded())
            return PlayLayer::updateAttempts();
        auto* gm = GameManager::get();
        KeepValue attempts(m_level->m_attempts);
        KeepValue session(gm->m_sessionAttempts);
        KeepValue session2(gm->m_sessionAttempts2);
        KeepValue normal(gm->m_sessionNormalAttempts);
        PlayLayer::updateAttempts();
    }
};

class $modify(GB7SafeModeJumps, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::incrementJumps", Priority::VeryLate);
    }

    void incrementJumps() {
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_level || !shielded())
            return PlayerObject::incrementJumps();
        KeepValue jumps(pl->m_level->m_jumps);
        PlayerObject::incrementJumps();
    }
};

// Every stat GD adds to in a level: jumps, attempts, orbs, stars and the rest.
class $modify(GB7SafeModeStats, GameStatsManager) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameStatsManager::incrementStat", Priority::VeryLate);
    }

    void incrementStat(char const* key, int amount) {
        if (PlayLayer::get() && shielded())
            return;
        GameStatsManager::incrementStat(key, amount);
    }
};

// Silicate hooks AchievementManager::reportAchievementWithID, which has no
// Windows address in 2.2081's bindings; GameManager's has one on every
// platform.
class $modify(GB7SafeModeAchievements, GameManager) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameManager::reportAchievementWithID", Priority::VeryLate);
    }

    void reportAchievementWithID(char const* key, int percent, bool dontNotify) {
        if (PlayLayer::get() && shielded())
            return;
        GameManager::reportAchievementWithID(key, percent, dontNotify);
    }
};

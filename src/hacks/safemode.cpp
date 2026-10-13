#include "hacks/safemode.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "hacks/autoclicker.hpp"

#include <Geode/Geode.hpp>
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
        // kSalt + the password (Nigel made it with a Python one-liner, so the
        // password itself never left his PC). Empty: nothing unlocks.
        constexpr const char* kOwnerTarget = "";
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
            bool unlocked = false;
        };

        State& st() {
            static State s;
            if (!s.loaded) {
                s.loaded = true;
                auto* mod = Mod::get();
                s.safe = mod->getSavedValue<bool>("safe_mode_auto", true);
                s.hidden = mod->getSavedValue<bool>("cheat_dot_hidden", false);
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

    bool cheatingNow() {
        auto* gb = GucciEngine::get();
        if (gb->standingDown || botswitch::takenOut())
            return false;
        auto const& u = gb->updater;
        return gb->isPlaying()                       // a macro, Calculate or the pathfinder
               || std::abs(u.m_tps - 240.0) > 1e-6     // physics at another rate
               || std::abs(u.m_speedhack - 1.0) > 1e-6 // speedhack
               || gb->noclipEnabled || u.m_preventDeath
               || u.m_paused // frame advance
               || Autoclicker::get()->enabled;
    }

} // namespace gucci::safemode

using namespace gucci;

// GD's own test-mode value is kept in `real`; while safe mode shields an
// attempt the flag is set, and the label is always drawn from `real`, so the
// shield never shows.
class $modify(GB7SafeModePlayLayer, PlayLayer) {
    struct Fields {
        bool real = false;
        CCDrawNode* dot = nullptr;
    };

    void shield() {
        bool const on = safemode::autoSafeMode() && (safemode::g_tainted || safemode::cheatingNow());
        m_isTestMode = on ? true : m_fields->real;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        safemode::g_tainted = false;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        m_fields->real = m_isTestMode;
        // The red dot, top right, in the game's own UI so recordings and renders
        // show it. Shown only with auto safe mode off (postUpdate).
        auto* dot = CCDrawNode::create();
        auto const win = CCDirector::get()->getWinSize();
        dot->drawDot(ccp(win.width - 7.f, win.height - 7.f), 3.5f, ccc4f(1.f, 0.16f, 0.16f, 1.f));
        dot->setVisible(false);
        dot->setID("cheat-dot"_spr);
        if (m_uiLayer)
            m_uiLayer->addChild(dot, 1000);
        else
            this->addChild(dot, 1000);
        m_fields->dot = dot;
        return true;
    }

    void resetLevel() {
        m_isTestMode = m_fields->real; // GD sees its own value through the reset
        PlayLayer::resetLevel();
        m_fields->real = m_isTestMode;
        safemode::g_tainted = safemode::cheatingNow(); // a new attempt
        shield();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        bool const now = safemode::cheatingNow();
        if (now)
            safemode::g_tainted = true;
        shield();
        if (auto* dot = m_fields->dot)
            dot->setVisible(!safemode::autoSafeMode() && !safemode::dotHidden() && (now || safemode::g_tainted));
    }

    // Saving happens here (and possibly a moment after, which the flag still
    // covers): shield first in case the cheat started this very tick.
    void levelComplete() {
        shield();
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        shield();
        PlayLayer::destroyPlayer(player, object);
    }

    void updateTestModeLabel() {
        bool const shielded = m_isTestMode;
        m_isTestMode = m_fields->real;
        PlayLayer::updateTestModeLabel();
        m_isTestMode = shielded;
    }
};

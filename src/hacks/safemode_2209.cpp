// See safemode_2209.hpp. GucciBot (2026-11-16, safety2209 lane): safe mode's
// half for GD 2.2091's new systems, Windows only (the raw fields and the speed
// keys' midhook have Windows numbers only: core/gd_addresses.hpp, Lane B).
//
// Ported in part from Silicate 1.1.2 (peony and contributors, GPL-3; its GD
// 2.2091 port, src/hooks/SafeMode.cpp and src/hooks/TimeSpeedControl.cpp):
// the NoProgress guard around a death, the AchievementManager hook, GD's speed
// held at 1x in applyTimeWarp, the speed slider and keys driving the bot's
// speed. The rest (playtime through pause and quit, the death-data writers,
// the upload, platformer saves, leaderboard times, Ignore Damage) is
// GucciBot's own, from a read of the 2.2091 executable (safety2209 lane notes).
//
// Nothing here has run in a GD 2.2091 game yet (no in-game 2.2091 test had
// passed when it was written).

#include "hacks/safemode_2209.hpp"

#if GB_SAFETY_2209

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "core/gd_addresses.hpp"
#include "core/mcp_only.hpp"
#include "hacks/safemode.hpp"
#include "hooks/util_midhook.hpp"
#include "mcp/mcp_server.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/AchievementManager.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/binding/Slider.hpp>
#include <Geode/binding/SliderThumb.hpp>
#include <Geode/binding/SliderTouchLogic.hpp>
#include <Geode/modify/AchievementManager.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/TimeSpeedControl.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>

using namespace geode::prelude;
using gucci::GucciEngine;
namespace v2091 = gucci::gdaddr::v2091;

// The named fields this file leans on, at the offsets GD's own code uses for
// them in the 2.2091 executable (levelComplete's test-mode gate reads
// +0x3328 before every progress write; the level at +0x878 and the players at
// +0xdb8 / +0xdc0 in resetLevel and dtCommitPlaytime). A bindings change that
// moved one fails the build here instead of reading the wrong memory.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(GJBaseGameLayer, m_isTestMode) == 0x3328, "GJBaseGameLayer::m_isTestMode moved");
static_assert(offsetof(GJBaseGameLayer, m_level) == 0x878, "GJBaseGameLayer::m_level moved");
static_assert(offsetof(GJBaseGameLayer, m_player1) == 0xdb8, "GJBaseGameLayer::m_player1 moved");
static_assert(offsetof(GJBaseGameLayer, m_player2) == 0xdc0, "GJBaseGameLayer::m_player2 moved");
#pragma clang diagnostic pop

namespace {

    // ---- the byte checks: each raw field is used only once GD's code that
    // reads it is found where it should be (core/gd_addresses.hpp).

    bool siteHolds(gucci::gdaddr::GuardedSite const& s) {
        auto const* at = reinterpret_cast<void const*>(geode::base::get() + s.offset);
        if (std::memcmp(at, s.expect.begin(), s.expect.size()) == 0)
            return true;
        log::error("[safety 2.2091] '{}': GD's code at {:#x} isn't what it should be (another GD version, or "
                   "the address is wrong); what needs it stays off",
                   s.name, s.offset);
        return false;
    }

    struct Checks {
        bool flags = false;       // PlayLayer +0x39f9 / +0x39fa
        bool speed = false;       // GJBaseGameLayer +0x3560 and +0x3564
        bool ignoreOff = false;   // the fields resetLevel clears to turn Ignore Damage off
    };

    Checks const& checks() {
        static Checks const c = [] {
            Checks c;
            c.flags = siteHolds(v2091::kCheckFlags);
            c.speed = siteHolds(v2091::kCheckSpeed) && siteHolds(v2091::kCheckMusicSpeed);
            c.ignoreOff = c.flags && siteHolds(v2091::kCheckIgnoreDamageOff);
            log::info("[safety 2.2091] byte checks: flags {}, speed {}, Ignore Damage {}", c.flags, c.speed,
                      c.ignoreOff);
            return c;
        }();
        return c;
    }

    template <class T>
    T& field(void* base, uintptr_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
    }

    // GucciBot is in charge (not standing down for another bot, not switched
    // off with Disable bot): Silicate's Bot::isEnabled().
    bool live() {
        auto* gb = GucciEngine::get();
        return !gb->standingDown && !gucci::botswitch::takenOut();
    }

    // What each guard did this launch (safety2209_probe).
    struct Counts {
        int deathsShielded = 0;
        int playtimeShielded = 0;
        int deathWritesBlocked = 0;
        int uploadCalls = 0;
        int uploadsBlocked = 0;
        int uploadsPassed = 0;
        int scoresBlocked = 0;
        int achievementsBlocked = 0;
        int platformerSavesBlocked = 0;
        int platformerLoadsRefused = 0;
        int journalBlocked = 0;
        int speedPinned = 0;
    } g_counts;

    // A test launch's control run (safety2209_probe "noflag_on"): NoProgress
    // leaves GD's no-progress flag alone, so a test can see what GD would have
    // written without it (the writers' own holds below still apply).
    bool g_testNoFlag = false;

    // A few lines in the log, not one per frame.
    void note(char const* what) {
        static int s_lines = 0;
        if (s_lines < 40) {
            ++s_lines;
            log::info("[safety 2.2091] {}", what);
        }
    }

}  // namespace

namespace gucci::safemode {

    NoProgress::NoProgress(PlayLayer* pl, bool on, bool testMode) {
        if (!on || !pl || !checks().flags || (g_testNoFlag && testLaunch()))
            return;
        m_pl = pl;
        m_wasSpeed = field<bool>(pl, v2091::kPlSpeedChanged);
        field<bool>(pl, v2091::kPlSpeedChanged) = true;
        if (testMode) {
            m_raisedTest = true;
            m_wasTest = pl->m_isTestMode;
            pl->m_isTestMode = true;
        }
    }

    NoProgress::~NoProgress() {
        if (!m_pl)
            return;
        field<bool>(m_pl, v2091::kPlSpeedChanged) = m_wasSpeed;
        if (m_raisedTest)
            m_pl->m_isTestMode = m_wasTest;
    }

    bool gdIgnoreDamageOn() {
        auto* pl = PlayLayer::get();
        return pl && checks().flags && field<bool>(pl, v2091::kPlIgnoreDamageUsed);
    }

    void gdIgnoreDamageOff() {
        auto* pl = PlayLayer::get();
        if (!pl || !checks().ignoreOff || !field<bool>(pl, v2091::kPlIgnoreDamageUsed))
            return;
        // As GD's resetLevel turns it off (0x518cc2): the attempt's flag, the
        // layer's request, both players' own flag, then the label.
        field<bool>(pl, v2091::kPlIgnoreDamageUsed) = false;
        field<bool>(pl, v2091::kGblIgnoreDamage) = false;
        if (pl->m_player1)
            field<bool>(pl->m_player1, v2091::kPlayerIgnoreDamage) = false;
        if (pl->m_player2)
            field<bool>(pl->m_player2, v2091::kPlayerIgnoreDamage) = false;
        pl->updateTestModeLabel();
    }

    bool gdOwnCheat() {
        auto* pl = PlayLayer::get();
        if (!pl)
            return false;
        // GD's Ignore Damage (Nigel, 2026-11-16: a cheat, like noclip).
        if (checks().flags && field<bool>(pl, v2091::kPlIgnoreDamageUsed))
            return true;
        // GD's own speed (Nigel, 2026-11-16: any speed but 1x is a cheat).
        // While GucciBot is in charge it's held at 1x (applyTimeWarp, below)
        // and the slider moves GucciBot's speed, which safe mode reads
        // already; this catches the frames before it's put back.
        if (checks().speed && std::abs(field<float>(pl, v2091::kGblSpeed) - 1.f) > 1e-6f)
            return true;
        return false;
    }

    std::string probeCounts() {
        auto const& c = g_counts;
        return fmt::format(
            "checks flags={} speed={} ignoreOff={}; deathsShielded={} playtimeShielded={} deathWritesBlocked={} "
            "uploadCalls={} uploadsBlocked={} uploadsPassed={} scoresBlocked={} achievementsBlocked={} "
            "platformerSavesBlocked={} platformerLoadsRefused={} journalBlocked={} speedPinned={}",
            checks().flags, checks().speed, checks().ignoreOff, c.deathsShielded, c.playtimeShielded,
            c.deathWritesBlocked, c.uploadCalls, c.uploadsBlocked, c.uploadsPassed, c.scoresBlocked,
            c.achievementsBlocked, c.platformerSavesBlocked, c.platformerLoadsRefused, c.journalBlocked,
            c.speedPinned);
    }

}  // namespace gucci::safemode

using namespace gucci;

// ---- 1. Death data and playtime (Nigel's rule 1: follow safe mode)
//
// GD 2.2091 tracks every death (PlayLayer::destroyPlayer -> GJGameLevel's
// dtBumpRun / dtBumpBucket / dtAppendOrder and PlayLayer::dtBumpCell, then
// the level's pending-upload flag +0x7b8) and the level's playtime
// (dtCommitPlaytime, from destroyPlayer, levelComplete, pauseGame and onQuit,
// into the level's +0x740 / +0x744). Each skips when PlayLayer's +0x39f9 or
// +0x39fa is set. While a run is shielded, the death and each playtime commit
// run with +0x39fa raised (Silicate's NoProgress), so GD's own checks keep
// them out; the writers themselves are also held back, in case GD reaches
// one another way. (levelComplete raises it in hacks/safemode.cpp.)
class $modify(GB2209SafeDeath, PlayLayer) {
    static void onModify(auto& self) {
        // Right before GD's own: noclip and the rest decide first whether
        // there is a death at all.
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::VeryLate);
        (void)self.setHookPriority("PlayLayer::dtCommitPlaytime", Priority::VeryLate);
        (void)self.setHookPriority("PlayLayer::resetLevel", Priority::VeryLate);
        gucci::mcponly::keep(self);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool const shield = safemode::runShielded();
        if (shield)
            ++g_counts.deathsShielded;
        safemode::NoProgress guard(this, shield);
        PlayLayer::destroyPlayer(player, object);
    }

    void dtCommitPlaytime() {
        bool const shield = safemode::runShielded();
        if (shield)
            ++g_counts.playtimeShielded;
        // The flag only: GD's playtime skip doesn't read the test-mode flag.
        safemode::NoProgress guard(this, shield, false);
        PlayLayer::dtCommitPlaytime();
    }

    // The level's practice attempt count, which 2.209 added: kept where it
    // was through a shielded reset, as safemode.cpp keeps the normal one
    // (Silicate 1.1.2's resetLevel keeps both).
    void resetLevel() {
        static bool const s_checked = siteHolds(v2091::kCheckPracticeAttempts);
        if (!s_checked || !m_level || !safemode::runShielded())
            return PlayLayer::resetLevel();
        auto& practice = field<int>(m_level, v2091::kLevelPracticeAttempts);
        int const was = practice;
        PlayLayer::resetLevel();
        practice = was;
    }

    int dtBumpCell(int a, int b, int c) {
        // destroyPlayer ignores what it returns.
        if (safemode::runShielded()) {
            ++g_counts.deathWritesBlocked;
            return 0;
        }
        return PlayLayer::dtBumpCell(a, b, c);
    }
};

class $modify(GB2209SafeDeathLevel, GJGameLevel) {
    static void onModify(auto& self) {
        gucci::mcponly::keep(self);
    }

    static bool hold() {
        if (!PlayLayer::get() || !safemode::runShielded())
            return false;
        ++g_counts.deathWritesBlocked;
        return true;
    }

    void dtBumpRun(gd::string& s, int a, int b) {
        if (hold())
            return;
        GJGameLevel::dtBumpRun(s, a, b);
    }

    void dtBumpBucket(gd::string& s, int a) {
        if (hold())
            return;
        GJGameLevel::dtBumpBucket(s, a);
    }

    void dtAppendOrder(gd::string& s, int a, bool b) {
        if (hold())
            return;
        GJGameLevel::dtAppendOrder(s, a, b);
    }

    // ---- Platformer leaderboard times and their input recording (rule 1)
    //
    // levelComplete hands GD the time with the attempt's input recording
    // (GJBaseGameLayer's m_recordString) through saveNewScore. A shielded
    // completion runs with GD's test-mode flag up, which should already keep
    // it out; this holds it back too.
    void saveNewScore(int value, int type, int ticks, int clicks, int coins, gd::string inputs, bool save) {
        if (PlayLayer::get() && safemode::runShielded()) {
            ++g_counts.scoresBlocked;
            note("held back a leaderboard time and its input recording (safe mode)");
            return;
        }
        GJGameLevel::saveNewScore(value, type, ticks, clicks, coins, inputs, save);
    }
};

// GD sends the death data when a level's page opens
// (LevelInfoLayer::onEnterTransitionDidFinish), with no PlayLayer around, and
// only from a signed-in account. What it sends was kept clean above; on top
// of that, nothing is sent from a test launch, or while the Safe mode switch
// is on (nothing counts then). Hooked by address: the bindings give it no
// return type.
namespace {
    using UploadFn = void (*)(GameLevelManager*);
    bool g_uploadHooked = false;

    bool holdUpload() {
        return safemode::forced() || GucciEngine::get()->protectedMode ||
               (PlayLayer::get() && safemode::runShielded());
    }

    void uploadDeathDataHook(GameLevelManager* self) {
        ++g_counts.uploadCalls;
        if (holdUpload()) {
            ++g_counts.uploadsBlocked;
            note("held back GD's death-data upload (test launch or safe mode)");
            return;
        }
        ++g_counts.uploadsPassed;
        // A call to the hooked address from inside the hook goes on to GD's own
        // (Geode's manual hooks).
        reinterpret_cast<UploadFn>(geode::base::get() + v2091::kUploadDeathData.offset)(self);
    }
}  // namespace

$execute {
    if (!siteHolds(v2091::kUploadDeathData))
        return;
    auto res = Mod::get()->hook(reinterpret_cast<void*>(geode::base::get() + v2091::kUploadDeathData.offset),
                                &uploadDeathDataHook, "GameLevelManager::uploadDeathData",
                                tulip::hook::TulipConvention::Thiscall);
    if (res.isErr()) {
        log::error("[safety 2.2091] couldn't hook uploadDeathData: {}", res.unwrapErr());
        return;
    }
    g_uploadHooked = true;
}

namespace {
    bool probeUpload() {
        if (!g_uploadHooked)
            return false;
        // Through the hook: the hooked address runs the hook first.
        reinterpret_cast<UploadFn>(geode::base::get() + v2091::kUploadDeathData.offset)(
            GameLevelManager::sharedState());
        return true;
    }
}  // namespace

// ---- Achievements (Silicate 1.1.2: AchievementManager::reportAchievementWithID)
//
// 2.209 reports some achievements to AchievementManager directly, past
// GameManager's (whose hook is in hacks/safemode.cpp). Hooked very late, as
// Silicate does.
class $modify(GB2209SafeAchievements, AchievementManager) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("AchievementManager::reportAchievementWithID", Priority::VeryLate);
        gucci::mcponly::keep(self);
    }

    void reportAchievementWithID(char const* id, int percent, bool dontNotify) {
        if (PlayLayer::get() && safemode::runShielded()) {
            ++g_counts.achievementsBlocked;
            return;
        }
        AchievementManager::reportAchievementWithID(id, percent, dontNotify);
    }
};

// ---- 4. Platformer saves (Nigel's rule 4)
//
// GD keeps a platformer level's checkpoints between visits: writePersistentSave
// (from onQuit, the practice switch, removing checkpoints, and every game save
// through GameManager / LocalLevelManager::encodeDataTo) and the save journal
// (GameLevelManager::writePlatformerSaveJournal, from onQuit). None is written
// for a visit in which the bot or a cheat was on at any point -- not just this
// attempt, as a checkpoint reached with a cheat stays in the save after the
// cheat is off. A legit visit saves as normal, with the Safe mode switch on
// too (Nigel, 2026-11-17: "yes", legit platformer runs keep their progress;
// only bot or cheat runs don't). Loading a save is refused while GucciBot
// records or plays: the macro starts the level from its start.
class $modify(GB2209PlatformerSave, PlayLayer) {
    struct Fields {
        bool cheatedVisit = false;
    };

    static void onModify(auto& self) {
        gucci::mcponly::keep(self);
    }

    bool cheatedNow() {
        bool const cheat = live() && (safemode::cheatingNow() || safemode::tainted());
        if (cheat)
            m_fields->cheatedVisit = true;
        // Not runShielded(): the Safe mode switch (and a test launch's forced
        // safe mode) shield every run, legit or not, and rule 4 keeps a legit
        // run's save. A test launch writes none of GD's save files anyway (the
        // test-save guard, and the journal hook below).
        return m_fields->cheatedVisit;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        (void)cheatedNow();
    }

    void writePersistentSave() {
        if (cheatedNow()) {
            ++g_counts.platformerSavesBlocked;
            note("no platformer save: the bot or a cheat was on in this visit");
            return;
        }
        PlayLayer::writePersistentSave();
    }

    void loadPersistentCheckpoint(gd::string& save) {
        auto* gb = GucciEngine::get();
        if (live() && (gb->isRecording() || gb->isPlaying())) {
            ++g_counts.platformerLoadsRefused;
            note("platformer save not loaded: GucciBot is recording or playing");
            Loader::get()->queueInMainThread([] {
                Notification::create("Platformer save not loaded: GucciBot is recording or playing a macro.",
                                     NotificationIcon::Info, 3.f)
                    ->show();
            });
            return;
        }
        PlayLayer::loadPersistentCheckpoint(save);
    }
};

class $modify(GB2209PlatformerJournal, GameLevelManager) {
    static void onModify(auto& self) {
        gucci::mcponly::keep(self);
    }

    void writePlatformerSaveJournal(GJGameLevel* level) {
        bool hold = safemode::testLaunch();  // a test launch writes none of GD's saves
        if (auto* pl = PlayLayer::get())
            hold = hold || static_cast<GB2209PlatformerSave*>(pl)->cheatedNow();
        if (hold) {
            ++g_counts.journalBlocked;
            return;
        }
        GameLevelManager::writePlatformerSaveJournal(level);
    }
};

// ---- 2. GD's own speed (Nigel's rule 2, Silicate 1.1.2's way)
//
// While GucciBot is in charge, GD's own speed stays 1x: GucciBot's speed is the
// only one, so a macro records and plays at one known speed. GD's speed slider
// (pause menu) and its speed keys move GucciBot's speed instead, in GD's 0.1x
// steps from 0.1x to 2x. Any speed but 1x is a cheat (safemode.cpp reads
// GucciBot's speed already). With GucciBot switched off, GD's slider and keys
// are GD's again.
namespace {
    constexpr double kMinSpeed = 0.1;
    constexpr double kMaxSpeed = 2.0;
    constexpr double kSpeedSteps = 10.0;

    double roundToStep(double speed) {
        return std::round(speed * kSpeedSteps) / kSpeedSteps;
    }

    std::string formatSpeed(double speed) {
        auto text = fmt::format("{:.2f}", speed);
        if (text.ends_with('0'))
            text.pop_back();
        return text + "x";
    }

    void setBotSpeed(double speed) {
        GucciEngine::get()->updater.m_speedhack = speed;
        Mod::get()->setSavedValue<double>("eng_speed", speed);  // as the menu's Game speed box saves it
    }
}  // namespace

class $modify(GB2209SpeedPin, GJBaseGameLayer) {
    static void onModify(auto& self) {
        gucci::mcponly::keep(self);
    }

    void applyTimeWarp(float timeWarp) {
        if (live() && checks().speed) {
            auto& speed = field<float>(this, v2091::kGblSpeed);
            auto& music = field<float>(this, v2091::kGblMusicSpeed);
            if (speed != 1.f || music != 1.f) {
                speed = 1.f;
                music = 1.f;
                ++g_counts.speedPinned;
                if (auto* pl = PlayLayer::get(); pl && static_cast<GJBaseGameLayer*>(pl) == this)
                    pl->updateTestModeLabel();
            }
        }
        GJBaseGameLayer::applyTimeWarp(timeWarp);
    }
};

class $modify(GB2209SpeedSlider, TimeSpeedControl) {
    static void onModify(auto& self) {
        gucci::mcponly::keep(self);
    }

    bool init(GJBaseGameLayer* gameLayer) {
        if (!TimeSpeedControl::init(gameLayer))
            return false;
        if (live()) {
            double const speed = GucciEngine::get()->updater.m_speedhack;
            float const value =
                static_cast<float>((std::clamp(speed, kMinSpeed, kMaxSpeed) - kMinSpeed) / (kMaxSpeed - kMinSpeed));
            if (m_slider)
                m_slider->setValue(value);
            if (m_speedLabel)
                m_speedLabel->setString(formatSpeed(speed).c_str());
        }
        // GD's own handler (timeSliderChanged) has no return type in the
        // bindings, so it can't be hooked: the slider's thumb calls this one
        // instead, which hands over to GD's while GucciBot is switched off.
        // Left alone if GD's can't be reached that way.
        if (siteHolds(v2091::kTimeSliderChanged) && m_slider && m_slider->m_touchLogic &&
            m_slider->m_touchLogic->m_thumb)
            m_slider->m_touchLogic->m_thumb->setTarget(this, menu_selector(GB2209SpeedSlider::onGucciSlider));
        return true;
    }

    void onGucciSlider(CCObject* sender) {
        if (!live()) {
            // GD's own handler, by address (the bindings' has no return type).
            static bool const s_gdHandler = siteHolds(v2091::kTimeSliderChanged);
            if (s_gdHandler)
                reinterpret_cast<void (*)(TimeSpeedControl*, CCObject*)>(geode::base::get() +
                                                                          v2091::kTimeSliderChanged.offset)(this, sender);
            return;
        }
        float const value = static_cast<SliderThumb*>(sender)->getValue();
        double const speed = roundToStep(value * (kMaxSpeed - kMinSpeed) + kMinSpeed);
        setBotSpeed(speed);
        if (m_speedLabel)
            m_speedLabel->setString(formatSpeed(speed).c_str());
    }
};

// The speed up / down keys (UILayer::handleAction, actions 10022 / 10023),
// right before GD adds the step to its own speed: the step goes to GucciBot's
// speed and GD's speed code is skipped (Silicate 1.1.2's speedKeybind
// midhook, same address and bytes).
namespace {
    void speedKeybindMidhook(SafetyHookContext& ctx) {
        if (!live())
            return;
        double const step = ctx.xmm0.f32[0];
        double const speed = GucciEngine::get()->updater.m_speedhack;
        setBotSpeed(std::max(kMinSpeed, roundToStep(speed + step)));
        ctx.rip = geode::base::get() + v2091::kSpeedKeybindDone.offset;
    }
}  // namespace

$execute {
    auto const base = geode::base::get();
    // The spot it jumps to is checked too, before anything goes in.
    if (!standDownBot() && !siteHolds(v2091::kSpeedKeybindDone)) {
        ++g_midhookAttempts;
        ++g_midhookFailures;
        return;
    }
    util_guarded_midhook(base + v2091::kSpeedKeybind.offset, v2091::kSpeedKeybind.expect, v2091::kSpeedKeybind.name,
                         speedKeybindMidhook);
}

// ---- Assistant Access's safety2209_probe (tests only)

namespace {
    uint32_t fnv(std::string const& s) {
        uint32_t h = 2166136261u;
        for (unsigned char c : s)
            h = (h ^ c) * 16777619u;
        return h;
    }

    template <class S>
    std::string strInfo(S const& from) {
        std::string const s(from);  // gd::string or std::string
        return fmt::format("{}:{:08x}", s.size(), fnv(s));
    }

    template <class T>
    T* findNode(CCNode* node) {
        if (!node)
            return nullptr;
        if (auto* hit = typeinfo_cast<T*>(node))
            return hit;
        for (auto* child : CCArrayExt<CCNode*>(node->getChildren()))
            if (auto* hit = findNode<T>(child))
                return hit;
        return nullptr;
    }
}  // namespace

std::string gucci::safemode::probeLevel() {
    std::string out;
    auto* pl = PlayLayer::get();
    // The last level read stays readable after the level is left (playtime
    // through quit).
    static geode::Ref<GJGameLevel> s_last;
    if (pl && pl->m_level)
        s_last = pl->m_level;
    if (!pl && s_last)
        out += "(left the level) ";
    if (s_last) {
        auto* l = s_last.data();
        out += fmt::format(
            "level id={} type={} deathsPerPercent={} startPosDeaths={} sessionAttempts={} startPosHistory={} "
            "deathMarkers={} pending={} sessionLog={} compressedSessionLog={} sessionPlay={} totalPlay={} "
            "practiceAttempts={} attempts={} jumps={} normal={} practice={} orbs={} newNormal2={} attemptTime={} "
            "bestTime={} inputsTime={} platformerSaveReplay={}; ",
            l->m_levelID.value(), static_cast<int>(l->m_levelType), strInfo(l->m_deathsPerPercent),
            strInfo(l->m_startPosDeathCounts), strInfo(l->m_currentSessionAttempts),
            strInfo(l->m_startPosAttemptHistory), strInfo(l->m_deathMarkers), l->m_deathDataPendingUpload,
            strInfo(l->m_sessionLog), strInfo(l->m_compressedSessionLog), l->m_sessionPlayTime, l->m_totalPlayTime,
            l->m_practiceAttempts, l->m_attempts.value(), l->m_jumps.value(), l->m_normalPercent.value(),
            l->m_practicePercent, l->m_orbCompletion.value(), l->m_newNormalPercent2.value(),
            l->m_attemptTime.value(), l->m_bestTime, strInfo(l->m_inputsTime), strInfo(l->m_platformerSaveReplay));
        if (pl)
            out += fmt::format("layer gdSpeed={} gdMusicSpeed={} ignoreDamage={} speedFlag={} testMode={} gdOwnCheat={}; ",
                               checks().speed ? field<float>(pl, v2091::kGblSpeed) : -1.f,
                               checks().speed ? field<float>(pl, v2091::kGblMusicSpeed) : -1.f,
                               checks().flags && field<bool>(pl, v2091::kPlIgnoreDamageUsed),
                               checks().flags && field<bool>(pl, v2091::kPlSpeedChanged), pl->m_isTestMode,
                               safemode::gdOwnCheat());
        if (l->m_levelType == GJLevelType::Main)
            out += fmt::format("completedMain={}; ",
                               GameStatsManager::sharedState()->hasCompletedMainLevel(l->m_levelID.value()));
    } else {
        out += "no level; ";
    }
    auto* gsm = GameStatsManager::sharedState();
    out += "stats";
    for (int k = 1; k <= 40; ++k) {
        int const v = gsm->getStat(std::to_string(k).c_str());
        if (v)
            out += fmt::format(" {}={}", k, v);
    }
    out += fmt::format("; completedLevels={}", gsm->m_completedLevels ? gsm->m_completedLevels->count() : 0u);
    auto* am = AchievementManager::sharedState();
    out += fmt::format("; achievements={}", am->m_reportedAchievements ? am->m_reportedAchievements->count() : 0u);
    out += fmt::format("; botSpeed={}", GucciEngine::get()->updater.m_speedhack);
    return out;
}

// Actions (a test launch only; mcp/test_settings.cpp checks):
//   upload             GD's death-data upload through the guard (held back)
//   noflag_on/_off     the control run's switch (g_testNoFlag above)
//   pause              PlayLayer::pauseGame, as GD's pause button
//   resume             the pause menu's Resume
//   complete           PlayLayer::levelComplete, as the end of the level calls it
//   speed_key:<id>     UILayer::handleAction(<id>) down and up, as GD's keybinds
//   slider:<0..1>      a TimeSpeedControl made for the level, its slider set and moved
//   gd_speed:<x>       GD's own speed written as its slider would (no GucciBot hook)
//   ignore_damage      GD's Ignore Damage turned on, as its keybind does
std::string gucci::safemode::probeAction(std::string const& action) {
    auto* pl = PlayLayer::get();
    auto arg = [&](char const* prefix) -> std::optional<std::string> {
        std::string_view const p = prefix;
        if (action.size() > p.size() && action.compare(0, p.size(), p) == 0)
            return action.substr(p.size());
        return std::nullopt;
    };
    if (action == "upload")
        return probeUpload() ? "" : "the upload guard isn't in (see the log)";
    if (action == "noflag_on" || action == "noflag_off") {
        g_testNoFlag = action == "noflag_on";
        return "";
    }
    if (!pl)
        return "no level is open";
    if (action == "pause") {
        pl->pauseGame(false);
        return "";
    }
    if (action == "resume") {
        auto* pause = findNode<PauseLayer>(CCDirector::get()->getRunningScene());
        if (!pause)
            return "no pause menu on screen";
        pause->onResume(nullptr);
        return "";
    }
    if (action == "complete") {
        pl->levelComplete();
        return "";
    }
    if (auto id = arg("speed_key:")) {
        if (!pl->m_uiLayer)
            return "no UILayer";
        int const n = std::atoi(id->c_str());
        pl->m_uiLayer->handleAction(n, true, 0.0);
        pl->m_uiLayer->handleAction(n, false, 0.0);
        return "";
    }
    if (auto v = arg("slider:")) {
        auto* control = TimeSpeedControl::create(pl);
        if (!control || !control->m_slider || !control->m_slider->m_touchLogic ||
            !control->m_slider->m_touchLogic->m_thumb)
            return "TimeSpeedControl or its slider wasn't made";
        pl->addChild(control);
        control->m_slider->setValue(static_cast<float>(std::atof(v->c_str())));
        control->m_slider->m_touchLogic->m_thumb->activate();
        std::string const label = control->m_speedLabel ? control->m_speedLabel->getString() : "";
        control->removeFromParent();
        log::info("[safety 2.2091] probe slider: label after the move '{}'", label);
        return "";
    }
    if (auto v = arg("gd_speed:")) {
        if (!checks().speed)
            return "the speed byte check failed";
        float const speed = static_cast<float>(std::atof(v->c_str()));
        field<float>(pl, v2091::kGblSpeed) = speed;
        field<float>(pl, v2091::kGblMusicSpeed) = speed;
        return "";
    }
    if (action == "ignore_damage") {
        if (!checks().ignoreOff)
            return "the Ignore Damage byte check failed";
        // As UILayer::handleAction's Ignore Damage keybind writes them (0x671ce7).
        field<bool>(pl, v2091::kGblIgnoreDamage) = true;
        field<bool>(pl, v2091::kPlIgnoreDamageUsed) = true;
        if (pl->m_player1)
            field<bool>(pl->m_player1, v2091::kPlayerIgnoreDamage) = true;
        if (pl->m_player2)
            field<bool>(pl->m_player2, v2091::kPlayerIgnoreDamage) = true;
        pl->updateTestModeLabel();
        return "";
    }
    return "unknown action '" + action + "'";
}

#endif  // GB_SAFETY_2209

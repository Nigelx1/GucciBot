// See fw_settings_store.hpp. Written fresh for GucciBot (2026-10); the
// limits are the ones anticroom's own settings tab clamps to (his Silicate
// fork, src/ui/manager.cpp, the Frame Windows tab) wherever he set one.

#include "analysis/ac/fw_settings_store.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace gucci::fwstore {

    namespace {

        using FW = FrameWindowSettings;
        using FWA = FrameWindowAnalyzer;

        template <typename T>
        struct Field {
            char const* key;
            T FW::* member;
            T lo;
            T hi;
        };

        struct Flag {
            char const* key;
            bool FW::* member;
        };

        // Every bool the analyzer, the hooks or the renderer reads.
        //
        // Deliberately not stored, because nothing reads them: showLabels and
        // showTotals (anticroom's struct carries them, his analyzer never
        // looks at them). And labelApply / labelTest, which are not settings
        // at all: in his menu they are buttons wired through change-callbacks;
        // the Calculate page calls applyLabel() and testPlayhead() directly.
        constexpr Flag kFlags[] = {
            {"fwac_enabled", &FW::enabled},
            {"fwac_subframe_probe", &FW::subframeProbe},
            {"fwac_cbf_whole_markers", &FW::cbfWholeMarkers},
            {"fwac_cbf_tick_ground", &FW::cbfTickGround},
            {"fwac_lstar_use_nerve", &FW::lstarUseNerve},
            {"fwac_lstar_use_fatigue", &FW::lstarUseFatigue},
            {"fwac_lstar_use_cps", &FW::lstarUseCps},
            {"fwac_subframe_bisect", &FW::subframeBisect},
            {"fwac_subframe_all", &FW::subframeAll},
            {"fwac_joint_setup_sweep", &FW::jointSetupSweep},
            {"fwac_entry_sweep", &FW::entrySweep},
            {"fwac_show_setup_range", &FW::showSetupRange},
            {"fwac_mark_setup_varying", &FW::markSetupVarying},
            {"fwac_setup_hold_modes", &FW::setupHoldModes},
            {"fwac_show_hz", &FW::showHzReadout},
            {"fwac_analysis_visuals", &FW::analysisVisuals},
            {"fwac_lock_camera", &FW::lockCamera},
            {"fwac_hide_spawn", &FW::hideSpawnEffects},
            {"fwac_adaptive_budget", &FW::adaptiveBudget},
            {"fwac_full_range_sweep", &FW::fullRangeSweep},
            {"fwac_test_ship_releases", &FW::testShipReleases},
            {"fwac_test_all_releases", &FW::testAllReleases},
            {"fwac_orb_aware_skip", &FW::orbAwareReleaseSkip},
            {"fwac_verbose", &FW::verbose},
            {"fwac_analysis_overlay", &FW::analysisOverlay},
            {"fwac_state_player_diff", &FW::statePlayerDiff},
            {"fwac_show_markers", &FW::showMarkers},
            {"fwac_show_desynced", &FW::showDesynced},
            {"fwac_show_timing", &FW::showTiming},
            {"fwac_show_hud", &FW::showHud},
            {"fwac_play_sounds", &FW::playSounds},
            {"fwac_circle_skin", &FW::circleSkin},
            {"fwac_lstar_enabled", &FW::lstarEnabled},
            {"fwac_lstar_hud", &FW::lstarHud},
            {"fwac_dependent_search", &FW::dependentSearch},
            {"fwac_turbo", &FW::turbo},
            {"fwac_label_releases", &FW::labelReleases},
        };

        constexpr Field<int> kInts[] = {
            {"fwac_algorithm", &FW::algorithm, 0, 1},
            {"fwac_sweep_range", &FW::sweepRange, 1, FWA::MAX_SWEEP},
            {"fwac_max_frames", &FW::maxFrames, FWA::MIN_HORIZON, 2400},
            {"fwac_slack", &FW::slack, 0, 20},
            {"fwac_recovery_range", &FW::recoveryRange, 0, 20},
            {"fwac_cbf_readout_threshold", &FW::cbfReadoutThreshold, 0, 20},
            {"fwac_subframe_scan_percent", &FW::subframeScanPercent, 1, 100},
            {"fwac_tight_threshold", &FW::tightThreshold, 0, 20},
            {"fwac_budget_ms", &FW::budgetMs, 1, 100},
            {"fwac_budget_share", &FW::budgetSharePercent, 1, 90},
            {"fwac_max_budget_ms", &FW::maxBudgetMs, 1, 250},
            {"fwac_step_batch", &FW::stepBatch, 1, 64},
            // formatSubframe() prints at most 4 places whatever this says.
            {"fwac_subframe_decimals", &FW::subframeDecimals, 0, 4},
            {"fwac_turbo_budget_ms", &FW::turboBudgetMs, 1, 2000},
            // 0 is meaningful: labelling with 0 (and no CBF value) removes
            // the label from the input under the playhead.
            {"fwac_label_window", &FW::labelWindow, 0, kBandWindowMax},
            {"fwac_label_test_count", &FW::labelTestCount, 1, 100},
        };

        constexpr Field<int64_t> kInt64s[] = {
            {"fwac_cbf_input_hz", &FW::cbfInputHz, 240, FWA::MAX_CBF_HZ},
        };

        constexpr Field<double> kDoubles[] = {
            {"fwac_lstar_respawn", &FW::lstarRespawn, 0.0, 60.0},
            {"fwac_lstar_target", &FW::lstarTarget, 1.0, 1e9},
            {"fwac_lstar_nerve", &FW::lstarNerve, 0.0, 1.0},
            {"fwac_lstar_fatigue", &FW::lstarFatigue, 0.0, 1.0},
            {"fwac_lstar_cps", &FW::lstarCps, 0.0, 4.0},
        };

        constexpr Field<float> kFloats[] = {
            {"fwac_sound_volume", &FW::soundVolume, 0.f, 1.f},
            {"fwac_marker_radius", &FW::markerRadius, 4.f, 60.f},
            {"fwac_marker_scale", &FW::markerScale, 0.1f, 2.f},
            {"fwac_circle_dot", &FW::circleSkinDotRadius, 1.f, 30.f},
            {"fwac_circle_per_frame", &FW::circleSkinRadiusPerFrame, 0.f, 10.f},
            {"fwac_circle_max", &FW::circleSkinMaxRadius, 1.f, 120.f},
            {"fwac_hud_scale", &FW::hudScale, 0.1f, 2.f},
            {"fwac_lstar_hud_scale", &FW::lstarHudScale, 0.1f, 2.f},
            {"fwac_label_cbf", &FW::labelCbf, 0.f, static_cast<float>(kBandWindowMax)},
        };

        constexpr char const* kTiersKey = "fwac_tiers";

        template <typename T, size_t N>
        Limits<T> find(Field<T> const (&table)[N], T FW::* member) {
            for (auto const& f : table)
                if (f.member == member)
                    return {f.lo, f.hi};
            // Only reachable if the menu asks about a field that is not in a
            // table, which is a programming error; the field's own type range
            // keeps the control usable rather than crashing the menu.
            log::error("[GucciBot] fwstore: no limits for a frame-window field");
            return {T{}, T{}};
        }

        // ---- bands ----------------------------------------------------------

        // The bands as one JSON array in a string, in the format the pre-bz
        // build wrote, so saved bands load unchanged.
        std::string dumpTiers(std::vector<FrameWindowTier> const& tiers) {
            auto arr = matjson::Value::array();
            for (auto const& t : tiers) {
                auto o = matjson::Value::object();
                o["id"] = t.id;
                o["min"] = t.minWindow;
                o["max"] = t.maxWindow;
                o["text"] = t.text;
                o["audio"] = t.audioPath;
                o["hud"] = t.showInHud;
                o["shape"] = static_cast<int>(t.style.shape);
                o["fill"] = static_cast<int>(t.style.fill);
                o["sides"] = t.style.polygonSides;
                o["corner"] = t.style.polygonCornerRadius;
                o["noborder"] = t.style.noBorder;
                o["stroke"] = t.style.strokeSize;
                o["sizescale"] = t.style.sizeScale;
                o["r"] = t.color[0];
                o["g"] = t.color[1];
                o["b"] = t.color[2];
                o["a"] = t.color[3];
                arr.push(o);
            }
            return arr.dump();
        }

        int64_t jint(matjson::Value const& v, char const* k, int64_t def) {
            auto r = v[k].as<int64_t>();
            return r.isOk() ? r.unwrap() : def;
        }
        double jreal(matjson::Value const& v, char const* k, double def) {
            auto r = v[k].as<double>();
            return r.isOk() ? r.unwrap() : def;
        }
        bool jbool(matjson::Value const& v, char const* k, bool def) {
            auto r = v[k].as<bool>();
            return r.isOk() ? r.unwrap() : def;
        }
        std::string jstr(matjson::Value const& v, char const* k, std::string def) {
            auto r = v[k].asString();
            return r.isOk() ? r.unwrap() : def;
        }

        // Keeps a band inside what the analyzer and the marker drawing cope
        // with. Shared by load and save, so a typed-in value can't be saved
        // out of range either.
        void clampTier(FrameWindowTier& t) {
            t.minWindow = std::clamp(t.minWindow, 0, kBandWindowMax);
            t.maxWindow = std::clamp(t.maxWindow, t.minWindow, kBandWindowMax);
            for (auto& c : t.color)
                c = std::clamp(c, 0.f, 1.f);
            int const shape = std::clamp(static_cast<int>(t.style.shape), 0, 3);
            int const fill = std::clamp(static_cast<int>(t.style.fill), 0, 2);
            t.style.shape = static_cast<gbshape::Shape>(shape);
            t.style.fill = static_cast<gbshape::Fill>(fill);
            t.style.polygonSides = std::clamp(t.style.polygonSides, 3, 12);
            t.style.polygonCornerRadius = std::clamp(t.style.polygonCornerRadius, 0.f, 1.f);
            t.style.strokeSize = std::clamp(t.style.strokeSize, 0.f, 10.f);
            t.style.sizeScale = std::clamp(t.style.sizeScale, 0.1f, 4.f);
        }

        // The HUD keys its count labels by band id, so two bands sharing one
        // would share a count. Renumber any repeat.
        void uniqueIds(std::vector<FrameWindowTier>& tiers) {
            std::set<int> seen;
            int next = 1;
            for (auto const& t : tiers)
                next = std::max(next, t.id + 1);
            for (auto& t : tiers) {
                if (t.id < 1 || !seen.insert(t.id).second) {
                    t.id = next++;
                    seen.insert(t.id);
                }
            }
        }

        bool loadTiers(std::string const& text, std::vector<FrameWindowTier>& out) {
            auto parsed = matjson::parse(text);
            if (!parsed.isOk())
                return false;
            auto const root = parsed.unwrap();
            if (!root.isArray() || root.size() == 0)
                return false;

            FrameWindowTier const def;
            std::vector<FrameWindowTier> tiers;
            tiers.reserve(root.size());
            for (size_t i = 0; i < root.size(); i++) {
                auto const& j = root[i];
                if (!j.isObject())
                    continue;
                FrameWindowTier t;
                t.id = static_cast<int>(jint(j, "id", static_cast<int64_t>(i) + 1));
                t.minWindow = static_cast<int>(jint(j, "min", def.minWindow));
                t.maxWindow = static_cast<int>(jint(j, "max", def.maxWindow));
                t.text = jstr(j, "text", "");
                t.audioPath = jstr(j, "audio", "");
                t.showInHud = jbool(j, "hud", true);
                t.style.shape = static_cast<gbshape::Shape>(jint(j, "shape", static_cast<int>(def.style.shape)));
                t.style.fill = static_cast<gbshape::Fill>(jint(j, "fill", static_cast<int>(def.style.fill)));
                t.style.polygonSides = static_cast<int>(jint(j, "sides", def.style.polygonSides));
                t.style.polygonCornerRadius = static_cast<float>(jreal(j, "corner", def.style.polygonCornerRadius));
                t.style.noBorder = jbool(j, "noborder", def.style.noBorder);
                t.style.strokeSize = static_cast<float>(jreal(j, "stroke", def.style.strokeSize));
                t.style.sizeScale = static_cast<float>(jreal(j, "sizescale", def.style.sizeScale));
                t.color[0] = static_cast<float>(jreal(j, "r", 1.0));
                t.color[1] = static_cast<float>(jreal(j, "g", 1.0));
                t.color[2] = static_cast<float>(jreal(j, "b", 1.0));
                t.color[3] = static_cast<float>(jreal(j, "a", 1.0));
                clampTier(t);
                tiers.push_back(std::move(t));
            }
            if (tiers.empty())
                return false;
            uniqueIds(tiers);
            out = std::move(tiers);
            return true;
        }

        void clampAll(FW& fw) {
            for (auto const& f : kInts)
                fw.*f.member = std::clamp(fw.*f.member, f.lo, f.hi);
            for (auto const& f : kInt64s)
                fw.*f.member = std::clamp(fw.*f.member, f.lo, f.hi);
            for (auto const& f : kDoubles)
                fw.*f.member = std::clamp(fw.*f.member, f.lo, f.hi);
            for (auto const& f : kFloats)
                fw.*f.member = std::clamp(fw.*f.member, f.lo, f.hi);
            for (auto& t : fw.tiers)
                clampTier(t);
            uniqueIds(fw.tiers);
        }

    } // namespace

    Limits<int> limits(int FW::* member) { return find(kInts, member); }
    Limits<int64_t> limits(int64_t FW::* member) { return find(kInt64s, member); }
    Limits<float> limits(float FW::* member) { return find(kFloats, member); }
    Limits<double> limits(double FW::* member) { return find(kDoubles, member); }

    void load() {
        auto* mod = Mod::get();
        auto& fw = SLSettings::get()->frameWindow;

        for (auto const& f : kFlags)
            fw.*f.member = mod->getSavedValue<bool>(f.key, fw.*f.member);
        for (auto const& f : kInts)
            fw.*f.member = static_cast<int>(
                std::clamp<int64_t>(mod->getSavedValue<int64_t>(f.key, fw.*f.member), f.lo, f.hi));
        for (auto const& f : kInt64s)
            fw.*f.member = std::clamp(mod->getSavedValue<int64_t>(f.key, fw.*f.member), f.lo, f.hi);
        for (auto const& f : kDoubles)
            fw.*f.member = std::clamp(mod->getSavedValue<double>(f.key, fw.*f.member), f.lo, f.hi);
        for (auto const& f : kFloats)
            fw.*f.member = static_cast<float>(std::clamp(
                mod->getSavedValue<double>(f.key, static_cast<double>(fw.*f.member)),
                static_cast<double>(f.lo), static_cast<double>(f.hi)));

        if (mod->hasSavedValue(kTiersKey)) {
            auto const text = mod->getSavedValue<std::string>(kTiersKey, "");
            if (!loadTiers(text, fw.tiers))
                log::warn("[GucciBot] fwstore: saved bands could not be read, using the defaults");
        }
    }

    void save() {
        auto* mod = Mod::get();
        auto& fw = SLSettings::get()->frameWindow;
        clampAll(fw);

        for (auto const& f : kFlags)
            mod->setSavedValue<bool>(f.key, fw.*f.member);
        for (auto const& f : kInts)
            mod->setSavedValue<int64_t>(f.key, fw.*f.member);
        for (auto const& f : kInt64s)
            mod->setSavedValue<int64_t>(f.key, fw.*f.member);
        for (auto const& f : kDoubles)
            mod->setSavedValue<double>(f.key, fw.*f.member);
        for (auto const& f : kFloats)
            mod->setSavedValue<double>(f.key, fw.*f.member);
        mod->setSavedValue<std::string>(kTiersKey, dumpTiers(fw.tiers));
    }

    void resetAll() {
        SLSettings::get()->frameWindow = FrameWindowSettings{};
        // The L* number on screen was solved with the old inputs.
        lstar::Solver::get()->markDirty();
        save();
    }

    void resetBands() {
        SLSettings::get()->frameWindow.tiers = FrameWindowSettings{}.tiers;
        save();
    }

} // namespace gucci::fwstore

$on_mod(Loaded) {
    gucci::fwstore::load();
}

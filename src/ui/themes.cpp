#include "ui/themes.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <fstream>
#include <sstream>

using namespace geode::prelude;

namespace gucci::ui::themes {

    namespace {

        Theme make(int id, const char* name, const char* ext, ImVec4 accent, bool pulse = false) {
            Theme t;
            t.id = id;
            t.name = name;
            t.extension = ext;
            t.subtitle = "Frame perfect. Ice cold.";
            t.accent = accent;
            t.pulse = pulse;
            return t;
        }

        int s_activeId = 0;
        std::string s_activeCustom;

    } // namespace

    const std::vector<Theme>& builtins() {
        // Numbers are the old menu's, so a saved "active_theme" still matches.
        static const std::vector<Theme> s_themes = {
            make(0, "GucciBot", ".brrr", {0.79f, 0.66f, 0.30f, 1.f}),
            make(1, "TooSiiBot", ".toosii", {0.86f, 0.20f, 0.24f, 1.f}),
            make(2, "TooSiiBot Syracuse", ".toosii", {0.96f, 0.45f, 0.12f, 1.f}),
            make(3, "TooSiiBot Sac State", ".toosii", {0.11f, 0.55f, 0.32f, 1.f}),
            make(4, "JaBot", ".ja", {0.36f, 0.32f, 0.85f, 1.f}),
            make(5, "GiddeyBot", ".giddey", {0.15f, 0.47f, 0.92f, 1.f}),
            make(6, "BamBot", ".bam", {0.95f, 0.75f, 0.15f, 1.f}),
            make(7, "SexyyBot", ".sexyy", {0.93f, 0.35f, 0.66f, 1.f}),
            make(8, "JuiceBot", ".juice", {0.62f, 0.35f, 0.90f, 1.f}),
            make(9, "ButlerBot", ".butler", {0.82f, 0.10f, 0.20f, 1.f}),
            make(10, "SaweetieBot", ".saweetie", {0.98f, 0.62f, 0.78f, 1.f}),
            make(11, "MaybachBot", ".maybach", {0.72f, 0.72f, 0.76f, 1.f}),
            make(12, "RomoBot", ".romo", {0.20f, 0.42f, 0.78f, 1.f}),
            make(13, "GrizzleyBot", ".grizzley", {0.55f, 0.40f, 0.25f, 1.f}),
            make(14, "RedKingdomBot", ".redkingdom", {0.85f, 0.08f, 0.10f, 1.f}, true),
            make(15, "LemonadeBot", ".lemonade", {0.97f, 0.88f, 0.30f, 1.f}),
            make(16, "BrrrBot", ".icebrrr", {0.55f, 0.85f, 0.98f, 1.f}),
            make(17, "WakaBot", ".waka", {0.25f, 0.70f, 0.40f, 1.f}),
            make(18, "YoungstaBot", ".youngsta", {0.95f, 0.55f, 0.20f, 1.f}),
            make(19, "KnockerzBot", ".knockerz", {0.60f, 0.60f, 0.65f, 1.f}),
        };
        return s_themes;
    }

    const Theme* builtin(int id) {
        for (auto const& t : builtins())
            if (t.id == id)
                return &t;
        return nullptr;
    }

    std::filesystem::path customThemesDir() {
        return Mod::get()->getSaveDir() / "customthemes";
    }

    std::vector<Theme>& customs() {
        static std::vector<Theme> s_customs;
        return s_customs;
    }

    void loadCustoms() {
        auto& list = customs();
        list.clear();
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(customThemesDir(), ec)) {
            if (entry.path().extension() != ".json")
                continue;
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            auto parsed = matjson::parse(text.str());
            if (!parsed)
                continue;
            auto const& v = parsed.unwrap();
            Theme t;
            t.custom = true;
            t.id = kCustomThemeId;
            t.name = v["name"].asString().unwrapOr(entry.path().stem().string());
            std::string ext = v["extension"].asString().unwrapOr("");
            if (ext.empty())
                continue;
            t.extension = ext.front() == '.' ? ext : "." + ext;
            t.subtitle = v["subtitle"].asString().unwrapOr("Custom theme.");
            list.push_back(std::move(t));
        }
    }

    const Theme& active() {
        if (s_activeId == kCustomThemeId)
            for (auto const& t : customs())
                if (t.name == s_activeCustom)
                    return t;
        if (auto* t = builtin(s_activeId))
            return *t;
        return builtins().front();
    }

    int activeId() {
        return s_activeId;
    }

    const std::string& activeCustomName() {
        return s_activeCustom;
    }

    void setActive(int id) {
        s_activeId = id;
    }

    void setActiveCustom(const std::string& name) {
        s_activeId = kCustomThemeId;
        s_activeCustom = name;
    }

} // namespace gucci::ui::themes

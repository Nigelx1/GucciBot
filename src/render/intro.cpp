// Frame Window intro card -- ported from anticroom's Silicate fork ("slc
// count", 2026-09-26). Layout, text substitution and band list are his; the
// changes are listed at the top of intro.hpp.

#include "intro.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>

#include "analysis/ac/shim.hpp"

using namespace cocos2d;

namespace gucci {

    namespace {

        constexpr float LEFT_X = 0.0364f;
        constexpr float RIGHT_X = 0.9600f;
        constexpr float CREDIT_X = 0.0120f;
        constexpr float CREDIT_Y = 0.0100f;

        constexpr float LEFT_MAIN_H = 0.0984f;
        constexpr float LEFT_SMALL_H = 0.0610f;
        constexpr float RIGHT_H = 0.0569f;
        constexpr float CREDIT_H = 0.0220f;

        constexpr float TTF_SUPERSAMPLE = 2.0f;

        constexpr char const* CREDIT_TEXT = "Fully inspired from NaN GD.";

        constexpr float RIGHT_TOP = 0.0354f;
        constexpr float RIGHT_STEP = 0.0588f;

        constexpr float Y_NAME = 0.2100f;
        constexpr float Y_NOTE = 0.3000f;
        constexpr float Y_HEAD1 = 0.5026f;
        constexpr float Y_HEAD2 = 0.5946f;
        constexpr float Y_HEAD3 = 0.6906f;
        constexpr float Y_GREY = 0.8979f;
        constexpr float Y_FOOTER = 0.9656f;

        constexpr char const* kFallbackFont = "bigFont.fnt";

        std::string withCommas(int64_t value) {
            bool const negative = value < 0;
            std::string const digits = fmt::format("{}", negative ? -value : value);

            std::string out;
            int count = 0;
            for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
                if (count != 0 && count % 3 == 0)
                    out.push_back(',');
                out.push_back(*it);
                count++;
            }
            if (negative)
                out.push_back('-');

            std::reverse(out.begin(), out.end());
            return out;
        }

        std::string trimNumber(double value) {
            std::string out = fmt::format("{:.1f}", value);
            if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0)
                out.erase(out.size() - 2);
            return out;
        }

        bool isTtf(std::string const& name) {
            if (name.size() < 4)
                return false;

            std::string tail = name.substr(name.size() - 4);
            for (auto& c : tail)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return tail == ".ttf";
        }

        // A bare .ttf name is looked for in the mod's resources, then in its
        // save folder (where a user can drop one); a path is used as given.
        // Returns empty when the file is not there -- CCLabelTTF would
        // otherwise quietly render in the system default font, which reads as
        // "my font setting is ignored".
        std::string resolveTtf(std::string const& name) {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (name.find_first_of("/\\") != std::string::npos)
                return fs::exists(fs::path(name), ec) ? name : std::string();

            auto* mod = geode::Mod::get();
            if (fs::exists(mod->getResourcesDir() / name, ec))
                return mod->getID() + "/" + name;
            auto const saved = mod->getSaveDir() / name;
            if (fs::exists(saved, ec))
                return saved.string();
            return {};
        }

        std::string sanitize(std::string const& text) {
            std::string out;
            out.reserve(text.size());

            for (unsigned char const c : text) {
                if (c >= 0x20 && c <= 0x7E)
                    out.push_back(static_cast<char>(c));
            }

            return out;
        }

        std::string replaceAll(std::string text, std::string const& from, std::string const& to) {
            if (from.empty())
                return text;
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
            return text;
        }

        std::string fill(std::string text,
                         std::vector<std::pair<std::string, std::string>> const& subs) {
            for (auto const& sub : subs)
                text = replaceAll(std::move(text), sub.first, sub.second);
            return text;
        }

        struct Band {
            std::string m_line;
            std::string m_time;
            ccColor3B m_color;
            GLubyte m_opacity;
        };

        std::vector<Band> collectBands(RenderIntroSettings const& settings, double tps) {
            auto tiers = SLSettings::get()->frameWindow.tiers;

            tiers.erase(std::remove_if(tiers.begin(),
                                       tiers.end(),
                                       [](FrameWindowTier const& t) { return !t.showInHud; }),
                        tiers.end());

            std::sort(tiers.begin(), tiers.end(), [](FrameWindowTier const& a, FrameWindowTier const& b) {
                if (a.maxWindow != b.maxWindow)
                    return a.maxWindow > b.maxWindow;
                return a.minWindow > b.minWindow;
            });

            double const safeTps = tps > 0.0 ? tps : 240.0;

            std::vector<Band> bands;
            bands.reserve(tiers.size());

            for (auto const& tier : tiers) {
                int const lo = tier.minWindow;
                int const hi = tier.maxWindow;

                std::string const range =
                    tier.text.empty()
                        ? (lo == hi ? fmt::format("{}", lo) : fmt::format("{}-{}", lo, hi))
                        : tier.text;

                std::string const hz = lo <= 0 ? fmt::format(">{}Hz", trimNumber(safeTps))
                                               : fmt::format("<{}Hz", trimNumber(safeTps / lo));

                std::string const low = fmt::format("{:.1f}", lo / safeTps * 1000.0);
                std::string const high = fmt::format("{:.1f}", hi / safeTps * 1000.0);
                std::string const ms = lo == hi ? low : fmt::format("{}-{}", low, high);

                std::vector<std::pair<std::string, std::string>> const subs = {
                    {"{range}", range},
                    {"{hz}", hz},
                    {"{ms}", ms},
                    {"{low}", low},
                    {"{high}", high},
                    {"{min}", fmt::format("{}", lo)},
                    {"{max}", fmt::format("{}", hi)},
                };

                auto const byte = [](float v) {
                    return (GLubyte)(std::clamp(v, 0.f, 1.f) * 255.f);
                };

                Band band;
                band.m_line = fill(settings.m_bandLine, subs);
                band.m_time = fill(settings.m_bandTime, subs);
                band.m_color = {byte(tier.color[0]), byte(tier.color[1]), byte(tier.color[2])};
                band.m_opacity = byte(tier.color[3]);

                bands.push_back(std::move(band));
            }

            return bands;
        }

    } // namespace

    void RenderIntroSettings::load() {
        auto* mod = geode::Mod::get();
        RenderIntroSettings const d;
        auto str = [&](char const* key, std::string const& def) {
            return mod->getSavedValue<std::string>(key, def);
        };
        m_enabled = mod->getSavedValue<bool>("render_intro_enabled", d.m_enabled);
        m_fadeInTime = std::max(0.0, mod->getSavedValue<double>("render_intro_fade_in", d.m_fadeInTime));
        m_holdTime = std::max(0.0, mod->getSavedValue<double>("render_intro_hold", d.m_holdTime));
        m_fadeOutTime = std::max(0.0, mod->getSavedValue<double>("render_intro_fade_out", d.m_fadeOutTime));
        m_autoLevelName = mod->getSavedValue<bool>("render_intro_auto_name", d.m_autoLevelName);
        m_font = str("render_intro_font", d.m_font);
        m_levelName = str("render_intro_name", d.m_levelName);
        m_levelNote = str("render_intro_note", d.m_levelNote);
        m_heading1 = str("render_intro_heading_1", d.m_heading1);
        m_heading2 = str("render_intro_heading_2", d.m_heading2);
        m_heading3 = str("render_intro_heading_3", d.m_heading3);
        m_greyLine = str("render_intro_grey", d.m_greyLine);
        m_footerLine = str("render_intro_footer", d.m_footerLine);
        m_bandHeader = str("render_intro_band_header", d.m_bandHeader);
        m_bandLine = str("render_intro_band_line", d.m_bandLine);
        m_bandTime = str("render_intro_band_time", d.m_bandTime);
    }

    void RenderIntroSettings::save() const {
        auto* mod = geode::Mod::get();
        mod->setSavedValue("render_intro_enabled", m_enabled);
        mod->setSavedValue("render_intro_fade_in", m_fadeInTime);
        mod->setSavedValue("render_intro_hold", m_holdTime);
        mod->setSavedValue("render_intro_fade_out", m_fadeOutTime);
        mod->setSavedValue("render_intro_auto_name", m_autoLevelName);
        mod->setSavedValue("render_intro_font", m_font);
        mod->setSavedValue("render_intro_name", m_levelName);
        mod->setSavedValue("render_intro_note", m_levelNote);
        mod->setSavedValue("render_intro_heading_1", m_heading1);
        mod->setSavedValue("render_intro_heading_2", m_heading2);
        mod->setSavedValue("render_intro_heading_3", m_heading3);
        mod->setSavedValue("render_intro_grey", m_greyLine);
        mod->setSavedValue("render_intro_footer", m_footerLine);
        mod->setSavedValue("render_intro_band_header", m_bandHeader);
        mod->setSavedValue("render_intro_band_line", m_bandLine);
        mod->setSavedValue("render_intro_band_time", m_bandTime);
    }

    void RenderIntro::build(PlayLayer* pl, RenderIntroSettings const& settings, float pixelHeight) {
        this->destroy();

        auto* director = CCDirector::get();
        auto* scene = director->getRunningScene();
        if (!scene)
            return;

        CCSize const win = director->getWinSize();

        m_root = CCNode::create();
        m_root->retain();
        m_root->setID("render-intro"_spr);
        m_root->setContentSize(win);
        m_root->setPosition({0.f, 0.f});
        scene->addChild(m_root, std::numeric_limits<int>::max() - 1);

        auto* bg = CCLayerColor::create({0, 0, 0, 255});
        bg->setContentSize({win.width * 3.f, win.height * 3.f});
        bg->setPosition({-win.width, -win.height});
        m_root->addChild(bg, -1);

        std::string const requested = settings.m_font.empty() ? std::string(kFallbackFont) : settings.m_font;
        bool useTtf = isTtf(requested);
        std::string font = requested;
        if (useTtf) {
            font = resolveTtf(requested);
            if (font.empty()) {
                geode::log::warn("[GucciBot] intro font {} not found, using {}", requested, kFallbackFont);
                useTtf = false;
                font = kFallbackFont;
            }
        }

        float const rasterHeight = pixelHeight > 0.f ? pixelHeight : win.height;

        auto makeLabel = [&](std::string const& text, float heightFrac) -> CCNode* {
            if (useTtf) {
                float const points =
                    std::clamp(heightFrac * rasterHeight * TTF_SUPERSAMPLE, 8.f, 1024.f);

                auto* label = CCLabelTTF::create(text.c_str(), font.c_str(), points);
                if (label && label->getContentSize().height > 0.f) {
                    if (auto* tex = label->getTexture())
                        tex->setAntiAliasTexParameters();
                    return label;
                }

                geode::log::error("[GucciBot] intro font {} failed, using {}", font, kFallbackFont);
                useTtf = false;
                font = kFallbackFont;
            }

            if (auto* label = CCLabelBMFont::create(text.c_str(), font.c_str()))
                return label;
            if (font != kFallbackFont) {
                geode::log::warn("[GucciBot] intro font {} failed, using {}", font, kFallbackFont);
                font = kFallbackFont;
                return CCLabelBMFont::create(text.c_str(), kFallbackFont);
            }
            return nullptr;
        };

        auto add = [&](std::string const& text,
                       float xFrac,
                       float yFracFromTop,
                       float heightFrac,
                       float anchorX,
                       ccColor3B color,
                       GLubyte opacity) -> CCNode* {
            std::string const clean = sanitize(text);
            if (clean.empty())
                return nullptr;

            auto* label = makeLabel(clean, heightFrac);
            if (!label)
                return nullptr;

            float const natural = label->getContentSize().height;
            if (natural > 0.f)
                label->setScale(heightFrac * win.height / natural);

            label->setAnchorPoint({anchorX, 0.5f});
            label->setPosition({win.width * xFrac, win.height * (1.f - yFracFromTop)});

            if (auto* rgba = dynamic_cast<CCRGBAProtocol*>(label)) {
                rgba->setColor(color);
                rgba->setOpacity(opacity);
            }

            m_root->addChild(label);
            m_labels.push_back(Line{label, opacity});
            return label;
        };

        double const tps = Bot::get()->updater().m_tps;
        int64_t const cbfHz = SLSettings::get()->frameWindow.cbfInputHz;

        std::string levelName = settings.m_levelName;
        if (settings.m_autoLevelName && pl && pl->m_level)
            levelName = pl->m_level->m_levelName;

        std::vector<std::pair<std::string, std::string>> const subs = {
            {"{tps}", trimNumber(tps)},
            {"{subfps}", withCommas(cbfHz)},
            {"{cbfhz}", fmt::format("{}", cbfHz)},
            {"{name}", levelName},
            {"{note}", settings.m_levelNote},
        };

        ccColor3B const white = {255, 255, 255};
        ccColor3B const grey = {0x7f, 0x7f, 0x7f};

        add(fill(levelName, subs), LEFT_X, Y_NAME, LEFT_MAIN_H, 0.f, white, 255);
        add(fill(settings.m_levelNote, subs), LEFT_X, Y_NOTE, LEFT_MAIN_H, 0.f, white, 255);
        add(fill(settings.m_heading1, subs), LEFT_X, Y_HEAD1, LEFT_MAIN_H, 0.f, white, 255);
        add(fill(settings.m_heading2, subs), LEFT_X, Y_HEAD2, LEFT_MAIN_H, 0.f, white, 255);
        add(fill(settings.m_heading3, subs), LEFT_X, Y_HEAD3, LEFT_MAIN_H, 0.f, white, 255);
        add(fill(settings.m_greyLine, subs), LEFT_X, Y_GREY, LEFT_SMALL_H, 0.f, grey, 255);
        add(fill(settings.m_footerLine, subs), LEFT_X, Y_FOOTER, LEFT_SMALL_H, 0.f, white, 255);

        int row = 0;
        add(fill(settings.m_bandHeader, subs), RIGHT_X, RIGHT_TOP, RIGHT_H, 1.f, white, 255);
        row++;

        for (auto const& band : collectBands(settings, tps)) {
            add(band.m_line, RIGHT_X, RIGHT_TOP + RIGHT_STEP * row, RIGHT_H, 1.f, band.m_color, band.m_opacity);
            row++;
            add(band.m_time, RIGHT_X, RIGHT_TOP + RIGHT_STEP * row, RIGHT_H, 1.f, band.m_color, band.m_opacity);
            row++;
        }

        if (auto* credit = makeLabel(CREDIT_TEXT, CREDIT_H)) {
            float const natural = credit->getContentSize().height;
            if (natural > 0.f)
                credit->setScale(CREDIT_H * win.height / natural);
            credit->setAnchorPoint({0.f, 1.f});
            credit->setPosition({win.width * CREDIT_X, win.height * (1.f - CREDIT_Y)});

            if (auto* rgba = dynamic_cast<CCRGBAProtocol*>(credit))
                rgba->setColor(ccColor3B{255, 255, 255});

            m_root->addChild(credit);
            m_labels.push_back(Line{credit, 255});
        }

        this->setAlpha(0.f);
    }

    void RenderIntro::setAlpha(float alpha) {
        float const clamped = std::clamp(alpha, 0.f, 1.f);

        for (auto const& line : m_labels) {
            if (!line.m_label)
                continue;
            auto* rgba = dynamic_cast<CCRGBAProtocol*>(line.m_label);
            if (!rgba)
                continue;

            rgba->setOpacity(static_cast<GLubyte>(
                std::lround(static_cast<float>(line.m_baseOpacity) * clamped)));
        }
    }

    void RenderIntro::destroy() {
        m_labels.clear();

        if (m_root) {
            m_root->removeFromParentAndCleanup(true);
            m_root->release();
            m_root = nullptr;
        }
    }

} // namespace gucci

#pragma once

// Frame Window intro card for renders, ported from anticroom's Silicate fork
// ("slc count", 2026-09-26). A title card played before the level starts:
// the level name, a subtitle, three heading lines and the frame-window bands
// in their colours, fading in and out over black. The game is held still
// while it plays, and the video and every audio track get the same length of
// lead-in, so the level lines up after it.
//
// Changes from his:
//  - GucciBot's settings (saved values, loaded with the other render settings)
//    instead of glaze.
//  - GD's own bigFont.fnt is the default font. His ships PUSAB.ttf, whose
//    embedded notice reads "All rights reserved" with no license, so it is
//    not bundled here. bigFont is the same Pusab lettering and is in every GD
//    install; a .ttf can still be named in the settings.
//  - The root node is retained while the card exists, so leaving the level
//    mid-intro cannot leave a dangling pointer behind.

#include <cocos2d.h>

#include <string>
#include <vector>

class PlayLayer;

namespace gucci {

    struct RenderIntroSettings {
        bool m_enabled = false;
        double m_fadeInTime = 1.0;
        double m_holdTime = 3.0;
        double m_fadeOutTime = 1.0;

        bool m_autoLevelName = true;
        std::string m_font = "bigFont.fnt";

        std::string m_levelName = "";
        std::string m_levelNote = "MindCap's Top 1";
        std::string m_heading1 = "CBF Window";
        std::string m_heading2 = "Counter";
        std::string m_heading3 = "@{subfps}SubFPS";
        std::string m_greyLine = "(Precision is not Difficulty)";
        std::string m_footerLine = "Raw Precision (24h) in o/s";

        std::string m_bandHeader = "{tps}Hz \"Frames\" w/ Syzzi's CBF";
        std::string m_bandLine = "{range} Frames ({hz} FP)";
        std::string m_bandTime = "({ms}ms)";

        double duration() const {
            return m_fadeInTime + m_holdTime + m_fadeOutTime;
        }

        void load();
        void save() const;
    };

    class RenderIntro {
    public:
        static RenderIntro* get() {
            static RenderIntro instance;
            return &instance;
        }

        void build(PlayLayer* pl, RenderIntroSettings const& settings, float pixelHeight);
        void setAlpha(float alpha);
        void destroy();

        bool active() const {
            return m_root != nullptr;
        }

    private:
        struct Line {
            cocos2d::CCNode* m_label = nullptr;
            GLubyte m_baseOpacity = 255;
        };

        cocos2d::CCNode* m_root = nullptr;
        std::vector<Line> m_labels;
    };

} // namespace gucci

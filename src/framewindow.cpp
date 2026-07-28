#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "GucciBot.hpp"

using namespace geode::prelude;

static bool parentChainFlipped(cocos2d::CCNode* n) {
    float sx = 1.f;
    for (auto* p = n; p; p = p->getParent()) sx *= p->getScaleX();
    return sx < 0.f;
}

class FrameWindowOverlay {
public:
    static FrameWindowOverlay* get() {
        static FrameWindowOverlay inst;
        return &inst;
    }

    void attach(PlayLayer* pl) {
        if (m_node || !pl) return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor) return;

                auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1403);
        m_node = node;

                auto* labels = CCNode::create();
        anchor->addChild(labels, 1404);
        m_labelLayer = labels;
    }

    void detach() {
        if (m_node) { m_node->removeFromParent(); m_node = nullptr; }
        if (m_labelLayer) { m_labelLayer->removeFromParent(); m_labelLayer = nullptr; }
        m_builtForCount = -1;
    }

        void render(PlayLayer* pl, bool isRendering) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl) return;
                                if (!m_node) {
            attach(pl);
            if (!m_node) return;
        }

        bool show = isRendering ? gb->fwEnabledRender : gb->fwEnabledLive;
        if (!show || !gb->fwHasData) {
            if (m_builtForCount != 0) { clear(); m_builtForCount = 0; }
            return;
        }

                                        uint32_t curFrame = gb->updater.getFrame();
                int visibleCount = 0;
        for (auto const& mk : gb->fwMarks)
            if (mk.frame <= curFrame && mk.window <= gb->fwMaxWindow) ++visibleCount;

                                        bool mirrored = m_labelLayer && parentChainFlipped(m_labelLayer);

        int sig = visibleCount * 100000
                + static_cast<int>(gb->fwMarks.size()) * 100
                + gb->fwMaxWindow
                + (mirrored ? 1 : 0);
        if (sig == m_builtForCount) return;
        m_builtForCount = sig;

                                        {
            int loosest = 0;
            for (auto const& mk : gb->fwMarks) if (mk.window > loosest) loosest = mk.window;
            log::info("[GucciBot] Frame-window overlay: node={}, {} marks total, "
                      "{} visible at threshold <= {} (loosest measured = {})",
                      m_node ? "attached" : "NULL",
                      gb->fwMarks.size(), visibleCount, gb->fwMaxWindow, loosest);
        }

        clear();

        for (auto const& mk : gb->fwMarks) {
            if (mk.window > gb->fwMaxWindow) continue;
            if (mk.frame > curFrame) continue;

            CCPoint at{ mk.x, mk.y };

                        auto* tier = gb->fwTierFor(mk.window);
            ccColor4F col = tier
                ? ccColor4F{ tier->r, tier->g, tier->b, 1.f }
                : gradeColor(mk.window, gb->fwMaxWindow);

            bool drewSprite = false;
            if (tier && tier->imageFile[0]) {
                auto path = Mod::get()->getSaveDir() / "fw_assets" / tier->imageFile;
                std::error_code ec;
                if (std::filesystem::exists(path, ec)) {
                    if (auto* spr = CCSprite::create(path.string().c_str())) {
                        spr->setPosition(at);
                        float maxDim = std::max(spr->getContentSize().width,
                                                spr->getContentSize().height);
                        if (maxDim > 0.f) spr->setScale((kRadius * 2.2f) / maxDim);
                        spr->setColor({ (GLubyte)(col.r*255),(GLubyte)(col.g*255),(GLubyte)(col.b*255) });
                        if (mirrored) spr->setScaleX(-spr->getScaleX());
                        m_labelLayer->addChild(spr);
                        drewSprite = true;
                    }
                }
            }
            if (!drewSprite) drawRing(at, kRadius, col);

                        auto* lbl = CCLabelBMFont::create(
                std::to_string(mk.window).c_str(), "bigFont.fnt");
            lbl->setScale(0.45f);
            if (mirrored) lbl->setScaleX(-0.45f);
            lbl->setPosition({ at.x, at.y + kRadius + 11.f });
            lbl->setColor({ (GLubyte)(col.r * 255), (GLubyte)(col.g * 255), (GLubyte)(col.b * 255) });
            lbl->setOpacity(255);
            m_labelLayer->addChild(lbl);
        }
    }

private:
    static constexpr float kRadius = 11.f;

                ccColor4F gradeColor(int window, int maxWindow) const {
        float span = std::max(1, maxWindow - 1);
        float t = std::clamp(static_cast<float>(window - 1) / span, 0.f, 1.f);
                return ccColor4F{ 1.f - t, t, 0.15f, 1.f };
    }

    void drawRing(CCPoint center, float radius, ccColor4F color) {
        const int segs = 28;
        const float thickness = 2.2f;
        ccColor4F clear4{ 0, 0, 0, 0 };
                m_node->drawCircle(center, radius, clear4, thickness, color, segs);
    }

    void clear() {
        if (m_node) m_node->clear();
        if (m_labelLayer) m_labelLayer->removeAllChildren();
    }

    CCDrawNode* m_node = nullptr;
    CCNode*     m_labelLayer = nullptr;
    int         m_builtForCount = -1;
};

class $modify(FrameWindowPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        FrameWindowOverlay::get()->attach(this);
        return true;
    }

    void onQuit() {
        FrameWindowOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};

namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRendering) {
        FrameWindowOverlay::get()->render(pl, isRendering);
    }

                                        void playTierSound(int window) {
        static std::unordered_map<std::string, FMOD::Sound*> s_soundCache;
        static FMOD::Channel* s_channel = nullptr;
        static FMOD::Sound*   s_currentSound = nullptr;

        auto* gb = GucciEngine::get();
        auto* tier = gb->fwTierFor(window);

        std::filesystem::path path;
        if (tier && std::string(tier->soundFile) == "none") {
            return;
        } else if (tier && tier->soundFile[0]) {
            path = Mod::get()->getSaveDir() / "fw_assets" / tier->soundFile;
        } else {
            path = Mod::get()->getResourcesDir() / "fw_default.mp3";
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        auto* system = FMODAudioEngine::sharedEngine()->m_system;
        if (!system) return;

                std::string key = path.string();
        FMOD::Sound* sound = nullptr;
        auto it = s_soundCache.find(key);
        if (it != s_soundCache.end()) {
            sound = it->second;
        } else {
            if (system->createSound(key.c_str(), FMOD_CREATESAMPLE, nullptr, &sound) != FMOD_OK || !sound)
                return;
            s_soundCache[key] = sound;
        }

                                                                        bool playing = false;
        if (s_channel) s_channel->isPlaying(&playing);
        if (s_channel && playing && s_currentSound == sound) {
            s_channel->setPosition(0, FMOD_TIMEUNIT_MS);
            return;
        }
        if (s_channel) s_channel->stop();
        system->playSound(sound, nullptr, false, &s_channel);
        s_currentSound = sound;
    }
}

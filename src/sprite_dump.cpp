#include "GucciBot.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <filesystem>
#include <fstream>

using namespace geode::prelude;

// One-time diagnostic, same spirit as objectid_dump.cpp but for actual pixels
// instead of just the object-type enum: renders each not-yet-captured object's
// real sprite (unrotated, unscaled -- as authored in the texture atlas) to a
// small transparent PNG, so shape assumptions for procedural decoration don't
// have to be guessed from rotation/scale statistics anymore.
//
// First attempt produced an empty output folder with no way to tell why --
// geode::log::info doesn't persist anywhere readable on this machine (see
// guccibot_objectids.log's own history). This version logs every stage's
// outcome to a plain file instead, so a failure is diagnosable without a
// live console.
class $modify(SpriteDumpPL, PlayLayer) {
    void createObjectsFromSetupFinished() {
        PlayLayer::createObjectsFromSetupFinished();
        if (!m_objects) return;

        auto dir = Mod::get()->getSaveDir() / "sprite_dump";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::ofstream dbg(Mod::get()->getSaveDir() / "sprite_dump_debug.log", std::ios::app);
        dbg << "--- createObjectsFromSetupFinished, m_objects count="
            << m_objects->count() << " ---\n";

        int captured = 0;
        int attempted = 0;
        for (auto* go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            int id = go->m_objectID;
            auto path = dir / (std::to_string(id) + ".png");
            if (std::filesystem::exists(path)) continue;
            attempted++;

            auto* texture = go->getTexture();
            if (!texture) {
                dbg << "id=" << id << " SKIP no texture\n";
                continue;
            }
            CCRect rect = go->getObjectTextureRect();
            dbg << "id=" << id << " rect=(" << rect.origin.x << "," << rect.origin.y
                << "," << rect.size.width << "," << rect.size.height << ")";
            if (rect.size.width <= 0.f || rect.size.height <= 0.f) {
                dbg << " SKIP degenerate rect\n";
                continue;
            }

            float w = std::min(rect.size.width + 24.f, 320.f);
            float h = std::min(rect.size.height + 24.f, 320.f);

            auto* snap = CCSprite::createWithTexture(texture, rect);
            if (!snap) {
                dbg << " SKIP createWithTexture failed\n";
                continue;
            }
            snap->setRotation(0.f);
            snap->setScaleX(1.f);
            snap->setScaleY(1.f);
            snap->setPosition({w / 2.f, h / 2.f});

            auto* rt = CCRenderTexture::create((int)w, (int)h);
            if (!rt) {
                dbg << " SKIP CCRenderTexture::create failed\n";
                continue;
            }
            rt->beginWithClear(0.f, 0.f, 0.f, 0.f);
            snap->visit();
            rt->end();
            bool saved = rt->saveToFile(path.string().c_str(), kCCImageFormatPNG);
            bool existsAfter = std::filesystem::exists(path);
            dbg << " saveToFile=" << (saved ? "true" : "false")
                << " existsOnDisk=" << (existsAfter ? "true" : "false") << "\n";

            if (existsAfter) captured++;
        }
        dbg << "attempted=" << attempted << " captured=" << captured << "\n";
    }
};

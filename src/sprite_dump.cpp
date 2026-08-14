#include "GucciBot.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <filesystem>

using namespace geode::prelude;

// One-time diagnostic, same spirit as objectid_dump.cpp but for actual pixels
// instead of just the object-type enum: renders each not-yet-captured object's
// real sprite (unrotated, unscaled -- as authored in the texture atlas) to a
// small transparent PNG, so shape assumptions for procedural decoration don't
// have to be guessed from rotation/scale statistics anymore.
class $modify(SpriteDumpPL, PlayLayer) {
    void createObjectsFromSetupFinished() {
        PlayLayer::createObjectsFromSetupFinished();
        if (!m_objects) return;

        auto dir = Mod::get()->getSaveDir() / "sprite_dump";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        int captured = 0;
        for (auto* go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            int id = go->m_objectID;
            auto path = dir / (std::to_string(id) + ".png");
            if (std::filesystem::exists(path)) continue;

            auto* texture = go->getTexture();
            if (!texture) continue;
            CCRect rect = go->getObjectTextureRect();
            if (rect.size.width <= 0.f || rect.size.height <= 0.f) continue;

            float w = std::min(rect.size.width + 24.f, 320.f);
            float h = std::min(rect.size.height + 24.f, 320.f);

            auto* snap = CCSprite::createWithTexture(texture, rect);
            if (!snap) continue;
            snap->setRotation(0.f);
            snap->setScaleX(1.f);
            snap->setScaleY(1.f);
            snap->setPosition({w / 2.f, h / 2.f});

            auto* rt = CCRenderTexture::create((int)w, (int)h);
            if (!rt) continue;
            rt->beginWithClear(0.f, 0.f, 0.f, 0.f);
            snap->visit();
            rt->end();
            rt->saveToFile(path.string().c_str(), kCCImageFormatPNG);

            captured++;
        }
        if (captured > 0) {
            geode::log::info("[GucciBot] sprite_dump captured {} new object sprites", captured);
        }
    }
};

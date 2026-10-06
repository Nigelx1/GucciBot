// The menu's key on a phone: a GucciBot button in the pause menu. On a Mac too,
// as a way in that doesn't depend on a key (Nigel's Option key didn't open the
// menu there, 2026-10-06; hacks/keybinds.cpp). Windows keeps its keybind and
// gets no button.

#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>

#if defined(GEODE_IS_MOBILE) || defined(GEODE_IS_MACOS)

using namespace geode::prelude;

class $modify(GBPauseButton, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto const win = CCDirector::sharedDirector()->getWinSize();
        auto* sprite = ButtonSprite::create("GucciBot");
        sprite->setScale(0.7f);
        auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(GBPauseButton::onGucciBot));
        button->setID("open-menu"_spr);
        auto* menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->setPosition({win.width - 60.f, 28.f});
        menu->addChild(button);
        this->addChild(menu, 10);
    }

    void onGucciBot(CCObject*) {
        gucci::ui::toggleOpen();
    }
};

#endif

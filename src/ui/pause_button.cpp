// The menu's key on a phone: a GucciBot button in the pause menu. On a Mac too,
// as a way in that doesn't depend on a key (Nigel's Option key didn't open the
// menu there, 2026-10-06; hacks/keybinds.cpp). Windows keeps its keybind and
// gets no button. It shows GucciBot's logo (ui/brand.hpp, Nigel 2026-11-01:
// the mark instead of the word GucciBot); while disguised (GBot) it stays the
// plain button with GBot's name.

#include "ui/brand.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>

#if defined(GEODE_IS_MOBILE) || defined(GEODE_IS_MACOS)

using namespace geode::prelude;

namespace {
    // The logo's size: a comfortable finger's width, about half again as tall
    // as the word's button was.
    constexpr float kLogoPoints = 36.f;
}

class $modify(GBPauseButton, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto const win = CCDirector::sharedDirector()->getWinSize();
        // Made again for every pause, so it follows the theme from the next
        // one. The logo sits in the corner the word's button took, 25 points
        // in from the right edge and 16 up from the bottom. Under GBot, or if
        // the logo's picture couldn't load, the word as before (GBot's name
        // while disguised).
        CCNode* sprite = gucci::ui::disguised() ? nullptr : gucci::ui::brand::pauseSprite(kLogoPoints);
        CCPoint at{win.width - 43.f, 34.f};
        if (!sprite) {
            auto* named = ButtonSprite::create(gucci::ui::brandName());
            named->setScale(0.7f);
            sprite = named;
            at = CCPoint{win.width - 60.f, 28.f};
        }
        auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(GBPauseButton::onGucciBot));
        button->setID("open-menu"_spr);
        auto* menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->setPosition(at);
        menu->addChild(button);
        this->addChild(menu, 10);
    }

    void onGucciBot(CCObject*) {
        gucci::ui::toggleOpen();
    }
};

#endif

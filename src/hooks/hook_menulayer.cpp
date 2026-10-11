#include "core/setup_check.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

// The setup check's once-a-launch popup (core/setup_check.hpp), on the first
// main menu: by then every mod has loaded and placed its hooks.
class $modify(GB7MenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init())
            return false;
        gucci::setupcheck::showLaunchPopupOnce();
        return true;
    }
};

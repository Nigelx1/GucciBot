#pragma once

// Stands in for Absense's ui/core/modal.hpp. Absense's own UI has modal
// dialogs; the pathfinder uses two things from it -- whether one is open (it
// waits while one is) and an info box when it stops. GucciBot's menu is not
// modal, so nothing is ever "open", and the info box is a notification.

#include <Geode/Geode.hpp>

#include <string>

namespace absense::modal {

    struct Info {
        std::string title;
        std::string message;
    };

    inline bool isOpen() { return false; }

    inline void info(Info const& i) {
        geode::Notification::create(i.title + ": " + i.message, geode::NotificationIcon::Info)
            ->show();
    }

}  // namespace absense::modal

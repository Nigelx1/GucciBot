// Seeded random teleports on macOS, iOS and Android.
//
// A teleport portal with no linked exit sends the player to one member of its
// target group, picked at random (GJBaseGameLayer::teleportPlayer). On Windows
// the teleportRandomOverride midhook (hook_gjbasegamelayer.cpp) hands GD the
// next value of the replay system's teleport state for that pick. Every
// attempt reseeds that state from the macro's seed and every checkpoint keeps
// it (hook_playlayer.cpp, engine_core.cpp), so a macro through a random
// teleport replays the same. The midhook sits inside teleportPlayer at a
// Windows address, and GD draws the value there from a generator of its own
// that the bindings have no address for, so neither carries over.
//
// So here GucciBot makes the pick itself, from the same state with the same
// formula (core/teleport_pick.hpp), and has GD take it: while teleportPlayer
// runs, the group's slot in m_groups holds a one-object array with the picked
// member, so GD finds a single member and takes it without drawing. The slot
// gets its own array back at the first of three points: GD's getGroup call
// for it (where teleportPlayer calls getGroup rather than having it inlined),
// the teleport's game event (gameEventTriggered, before any event trigger
// runs), or teleportPlayer returning. Swapping the slot, rather than only
// answering getGroup, works whether or not getGroup was inlined.
//
// What this rests on, read from the 2.2081 Windows binary (teleportPlayer
// 0x20fdb0-0x2107be, getGroup 0x224280): GD reads the group once, as
// getGroup(target group ID), which is m_groups[clamp(id, 0, 9999)], made and
// filed in m_groupDict the first time it's asked for; a group of one is taken
// as it is, with no draw; an empty group, or a target group of 0, teleports
// nowhere. After the pick, teleportPlayer moves the player and calls the
// portal's activatedByPlayer (which only sets its activated flags),
// flipGravity, and effect, streak and force helpers, none of which read a
// group, and last gameEventTriggered. GD's own generator isn't moved on here
// as it is on Windows, where GD draws before the midhook replaces the value;
// its only other use is GJBaseGameLayer::init filling the level's variance
// table (m_varianceValues), which GucciBot refills from the macro's seed
// (hook_playlayer.cpp). The other platforms' GD is built from the same source
// but wasn't read. Untested on a device: the log says once that it engaged,
// and how GD read the group.

#include "core/platform.hpp"

#if !GB_NATIVE_ENGINE

#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "core/teleport_pick.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace {

    // The slot teleportPlayer is reading its pick from, while it runs.
    struct GroupSwap {
        GJBaseGameLayer* layer = nullptr;  // null: nothing swapped
        size_t slot = 0;
        CCArray* group = nullptr;  // the group's own array, which goes back in the slot
        CCArray* pick = nullptr;   // the one-object array standing in for it
    };
    GroupSwap s_swap;

    // GD read the group through getGroup (a call of its own on this platform),
    // for the log.
    bool s_readThroughGetGroup = false;

    // getGroup's slot for a group ID.
    size_t groupSlot(int id) {
        return static_cast<size_t>(std::clamp(id, 0, 9999));
    }

    // The group's own array back in its slot. Safe to call at any time.
    void putGroupBack() {
        if (!s_swap.layer)
            return;
        auto& groups = s_swap.layer->m_groups;
        if (s_swap.slot < groups.size() && groups[s_swap.slot] == s_swap.pick)
            groups[s_swap.slot] = s_swap.group;
        s_swap = GroupSwap{};
    }

} // namespace

class $modify(GBPortableTeleport, GJBaseGameLayer) {
    void teleportPlayer(TeleportPortalObject* portal, PlayerObject* player) {
        // A teleport from inside another (an event trigger the first one's
        // event fired): the first one has taken its pick by now.
        putGroupBack();

        auto* gb = gucci::GucciEngine::get();
        // The teleports GD picks nothing for, and the look-ahead copies:
        // Absense's teleportPlayer moves those from the run's own state
        // (absense/hooks.cpp, the same test), and GD's never runs for them.
        if (!gb->enabled || !portal || portal->m_orangePortal || portal->m_targetGroupID <= 0 ||
            Bot::get()->trajectory().isFakePlayer(player))
            return GJBaseGameLayer::teleportPlayer(portal, player);

        int const id = portal->m_targetGroupID;
        // GD's own read, with its side effect: the first time a group is
        // asked for, an empty one is made and filed, as GD would.
        CCArray* group = this->getGroup(id);
        int const count = group ? static_cast<int>(group->count()) : 0;
        if (count <= 1)
            return GJBaseGameLayer::teleportPlayer(portal, player);

        size_t const slot = groupSlot(id);
        if (slot >= m_groups.size() || m_groups[slot] != group) {
            // getGroup doesn't keep its groups where it does on Windows: GD
            // draws for itself, unseeded, as it did before this port.
            static bool warned = false;
            if (!warned) {
                warned = true;
                log::error("[GucciBot] Random teleports can't be seeded on this platform: group {} isn't in "
                           "slot {} of {}. A macro through a random teleport may not replay. Please report this.",
                           id, slot, m_groups.size());
                std::string msg = gucci::ui::branded("GucciBot: random teleports aren't seeded here, please report");
                geode::queueInMainThread([msg] {
                    geode::Notification::create(msg, geode::NotificationIcon::Error)->show();
                });
            }
            return GJBaseGameLayer::teleportPlayer(portal, player);
        }

        // teleportRandomOverride's draw (hook_gjbasegamelayer.cpp), then GD's
        // pick from it.
        uint64_t& state = gb->replay.m_teleportRandomState;
        state = gucci::teleportpick::nextState(state);
        int const index = gucci::teleportpick::memberIndex(count, static_cast<int>(state));
        CCObject* target = group->objectAtIndex(static_cast<unsigned>(index));

        CCArray* pick = CCArray::create();
        pick->addObject(target);
        pick->retain();
        s_swap = GroupSwap{this, slot, group, pick};
        s_readThroughGetGroup = false;
        m_groups[slot] = pick;
        GJBaseGameLayer::teleportPlayer(portal, player);
        putGroupBack();
        pick->release();

        static bool logged = false;
        if (!logged) {
            logged = true;
            log::info("[GucciBot] Random teleport: member {} of {} in group {}, picked from the seeded state; GD {}. "
                      "Logged once.",
                      index, count, id,
                      s_readThroughGetGroup ? "read the group through getGroup"
                                            : "didn't call getGroup (inlined here), so it read the swapped slot");
        }
    }

    // GD's read of the group inside teleportPlayer, where getGroup is a call
    // of its own: the pick goes straight back, and the group to its slot.
    cocos2d::CCArray* getGroup(int id) {
        if (s_swap.layer == this && groupSlot(id) == s_swap.slot) {
            CCArray* pick = s_swap.pick;
            putGroupBack();
            s_readThroughGetGroup = true;
            return pick;
        }
        return GJBaseGameLayer::getGroup(id);
    }

    // The teleport's event, before any event trigger listening for it runs.
    void gameEventTriggered(GJGameEvent event, int p1, int p2) {
        putGroupBack();
        GJBaseGameLayer::gameEventTriggered(event, p1, p2);
    }
};

#endif // !GB_NATIVE_ENGINE

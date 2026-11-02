#pragma once

// GD's own screens as data, for Assistant Access (2026-11-01): gucci_scene,
// gucci_overlays, the event recorder (mcp/events.hpp) and gucci_level_info's
// gd_labels read the cocos node tree through this instead of a screenshot.
//
// Only reads, on the main thread, from the tools' pump (the scheduler's
// update): never in the middle of a visit, and nothing is kept between calls.
// Boxes are in cocos units (the window is win[0] x win[1], y up from the
// bottom), the way the game places its nodes; px_per_unit turns them into
// the window's pixels.

#include <matjson.hpp>

#include <string>
#include <vector>

namespace cocos2d {
    class CCObject;
    class CCNode;
}
class PlayLayer;

namespace gucci::mcp::scene {

    struct Options {
        // "all" (the running scene, then Geode's overlay: notifications), "scene",
        // "overlay", "ui" (the level's UI layer) or "id:<node id>".
        std::string root = "all";
        // Any that are set must match, ignoring case: the node's ID (a part of
        // it; "a|b" for either), its class (a part) and its text (a part).
        // With find, only the nodes that match are listed, each with every
        // text under it.
        std::string findId;
        std::string findClass;
        std::string findText;
        int depth = 14;
        int max = 1500;
        bool visibleOnly = true;
        // The level's objects: off, the batch nodes under the object layer
        // are counted, not walked (LIMBO has 392k); nodes there with an ID
        // (GucciBot's route and marker nodes) are walked anyway.
        bool objects = false;
    };

    // {scene, transition, win, px_per_unit, gd, popups, notifications, nodes,
    // truncated}, or with `text` a "lines" string, one node a line.
    matjson::Value walk(Options const& options, bool text);

    // GD's own labels in a level, from PlayLayer's members (not node IDs):
    // percentage and attempts {shown, text}, progress_bar {shown}.
    matjson::Value gdLabels(PlayLayer* pl);

    // On screen now: alerts (FLAlertLayer and Geode's popups: class, title,
    // body, buttons) and Geode notifications (text, box, offscreen).
    matjson::Value alerts();
    matjson::Value notifications();
    // One alert's {class, id, title, body, buttons, shown}: the text highest
    // on the screen is its title, the rest (top to bottom) its body; the
    // buttons' texts apart.
    matjson::Value alertOf(cocos2d::CCNode* alert);

    // Every text under a node, in the order it's drawn: labels and text boxes.
    std::vector<std::string> textsUnder(cocos2d::CCNode* node, int maxDepth = 16);

    // A node's box on screen (cocos units, y up), its descendants' too when
    // `deep`; and whether that box leaves the screen.
    struct Box {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    };
    Box boxOf(cocos2d::CCNode* node, bool deep);
    bool offscreen(Box const& box);
    matjson::Value boxJson(Box const& box);

    // Drawn: the node and every parent visible, and (where it has one) its
    // displayed opacity above 0.
    bool drawn(cocos2d::CCNode* node);

    // A node's class as Geode names it ("cocos2d::CCLabelBMFont").
    std::string className(cocos2d::CCObject const* object);

} // namespace gucci::mcp::scene

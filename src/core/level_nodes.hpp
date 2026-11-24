#pragma once

// GucciBot (2026-11-23, 2.0.1): nodes an overlay puts into a level and keeps a
// pointer to between frames (the macro path, the JMF Trainer's and the
// Trainer's ghosts, Practice Range, the agency map).
//
// GD's Edit exits (the pause menu's Edit and the end screen's Edit, on your own
// levels; Eclipse's Level Edit puts the first one on every level) destroy the
// PlayLayer without PlayLayer::onQuit, and onQuit was where these overlays let
// go of their nodes. With a plain pointer the level took the nodes with it, the
// next level refused to make new ones (the pointer was still set), and that
// level's quit called removeFromParent through the freed node. iOS 16.1+ wipes
// freed memory, so there it crashed (an iPhone tester's crash log, 2026-11-23:
// SIGSEGV at 0x1f8 in MacroPathPlayLayer::onQuit); macOS 13+ likely too. On
// Windows it wrote into freed memory every frame of that level instead.
//
// So an overlay holds its own reference to each node (hold, drop) and uses one
// only while it hangs in the current level (inLevel). Asking is safe after the
// level has gone: our reference keeps the node alive, and a dying cocos node
// clears each child's parent pointer before it lets the child go (~CCNode,
// also in GD's libcocos2d), so the walk up never reaches a freed node, and a
// new level allocated where the old one was isn't mistaken for it. The walk
// goes up to the PlayLayer itself, not only to the layer the node was added
// to: in a level with a shader GD moves children out of the object layer into
// its other layers (GJBaseGameLayer::updateShaderLayer; framewindow.cpp's leg
// trail found it on KOCMOC, 2026-11-18), and a node moved there is still in
// the level. hitborders.cpp and hitboxes.cpp hold their nodes the same way.

#include <Geode/Geode.hpp>

namespace gucci::levelnodes {

    // `node` hangs somewhere under `level`.
    inline bool inLevel(cocos2d::CCNode* node, cocos2d::CCNode* level) {
        if (!node || !level)
            return false;
        for (auto* n = node->getParent(); n; n = n->getParent())
            if (n == level)
                return true;
        return false;
    }

    // Adds `node` to `parent` with a reference of our own on it.
    template <class T>
    T* hold(T* node, cocos2d::CCNode* parent, int z) {
        node->retain();
        parent->addChild(node, z);
        return node;
    }

    // Takes the node out of whatever holds it (nothing, once its level has
    // gone) and lets our reference go. Safe to call again.
    template <class T>
    void drop(T*& node) {
        if (!node)
            return;
        node->removeFromParent();
        node->release();
        node = nullptr;
    }

    // A log line when an overlay finds that the nodes it holds aren't in this
    // level: the last level closed without its quit (an Edit exit), or
    // something took them out. The first 3 times per overlay in a launch, so
    // reporters' logs show it happened and a level that keeps losing them
    // can't fill the log.
    inline void noteStale(char const* overlay, int& logged) {
        if (logged >= 3)
            return;
        ++logged;
        geode::log::info("[GucciBot] {}: its nodes weren't in this level (the last level closed without "
                         "its quit, as an Edit exit does, or something took them out); made new ones{}",
                         overlay,
                         logged == 3 ? " (not logged again this launch)" : "");
    }

} // namespace gucci::levelnodes

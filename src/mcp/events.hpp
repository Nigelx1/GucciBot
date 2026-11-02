#pragma once

// The event recorder (Assistant Access, 2026-11-01): what happened on screen
// and in the game since a session last looked, kept as it happens, so a
// notification that lasts two seconds, a popup, a death or a run ending isn't
// missed between two calls. gucci_events reads it.
//
// tick() runs once a frame at the start of the tools' pump (the scheduler's
// update, main thread), only while Assistant Access is on. It compares with
// the frame before: the direct children of the running scene and of Geode's
// overlay (where notifications go), the level, player 1, GucciBot's mode and
// run owner, ImGui's open popups, Calculate's last message and the last
// render. Only a new node is walked for its text.

#include <matjson.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace gucci::mcp::events {

    void tick();

    // One more event, from anywhere on the main thread.
    void add(std::string kind, std::string text, matjson::Value data = matjson::Value::object());

    // gucci_events: the events after `since` (a seq), of these kinds (all
    // when empty), at most `limit`, oldest first. {seq: the newest one,
    // dropped: events after `since` the ring no longer holds, events}.
    matjson::Value read(uint64_t since, std::vector<std::string> const& kinds, size_t limit);

    // The newest event's seq (0: none yet).
    uint64_t lastSeq();

} // namespace gucci::mcp::events

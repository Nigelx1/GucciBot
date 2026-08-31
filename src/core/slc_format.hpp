#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace gucci {

    // A press/release input read from an imported Silicate (.slc) macro.
    // Button numbering matches BRRInput/GdrJsonInput's own convention, which
    // happens to already line up with slc's own ActionType (Jump=1, Left=2,
    // Right=3), so no translation table is needed downstream.
    struct SlcInput {
        uint64_t frame = 0;
        uint8_t button = 1;
        bool player2 = false;
        bool holding = false;
    };

    // Tries to parse `bytes` as a Silicate replay, v3 first then v2 -- matches
    // the probe order Silicate's own ReplaySystem::load() uses (v3's 8-byte
    // "SLC3RPLY" magic is unambiguous; v2's is a 4-byte "SILL"). Returns
    // std::nullopt if neither magic matches or the stream is malformed.
    //
    // Both formats' binary layout were read directly from the real slc
    // library source (git.silicate.dev/silicate/slc -- see
    // project_guccibot_status.md for exact paths this was verified against),
    // not guessed. Restart/RestartFull/Death/TPS-change/Bugpoint markers are
    // parsed just enough to stay correctly synced with the byte stream (they
    // carry their own frame deltas that everything after them depends on) but
    // are not themselves emitted -- only Jump/Left/Right press/release events
    // are returned, since that's what the Click Indicator needs to score
    // against. `outTps` receives the replay's base TPS if the caller wants it.
    std::optional<std::vector<SlcInput>> parseSlcReplay(const std::vector<uint8_t>& bytes,
                                                         double* outTps = nullptr);

} // namespace gucci

#include "core/slc_format.hpp"

#include <cstring>

namespace gucci {

    namespace {

        // Internal decode-time representation. Frame/type/holding/player2 only
        // -- covers every slc action type (Jump/Left/Right plus the
        // Restart/RestartFull/Death/TPS/Bugpoint markers), since the markers'
        // own frame deltas have to be walked correctly to keep the byte stream
        // in sync even though only player inputs (type 1-3) get returned.
        struct SlcAction {
            uint64_t frame = 0;
            int type = 0;
            bool holding = false;
            bool player2 = false;
        };

        bool readBytes(const std::vector<uint8_t>& b, size_t& pos, void* out, size_t n) {
            if (pos + n > b.size())
                return false;
            std::memcpy(out, b.data() + pos, n);
            pos += n;
            return true;
        }

        template <typename T>
        bool readLE(const std::vector<uint8_t>& b, size_t& pos, T& out) {
            return readBytes(b, pos, &out, sizeof(T));
        }

        // Reads `byteSize` little-endian bytes (byteSize in {1,2,4,8}),
        // zero-extended into a uint64_t -- matches slc's own packed-state
        // encoding, which always stores the smallest byte width that fits.
        bool readPacked(const std::vector<uint8_t>& b, size_t& pos, uint64_t byteSize, uint64_t& out) {
            out = 0;
            if (pos + byteSize > b.size())
                return false;
            std::memcpy(&out, b.data() + pos, byteSize);
            pos += byteSize;
            return true;
        }

        // ---- slc v2 ("SILL") ----
        // Layout: double tps, uint64 metaSize, <metaSize bytes>, uint64
        // inputCount, uint64 blobCount, blobCount x {uint64 byteSize, uint64
        // start, uint64 length} blob table, then each blob's packed inputs in
        // table order. Per-input state = (delta<<5)|(type<<2)|(p2<<1)|holding,
        // type 7 (TPS) carries an extra trailing 8-byte double. Frame deltas
        // thread continuously across all blobs in table order.
        bool parseSlcV2(const std::vector<uint8_t>& b,
                        size_t pos,
                        std::vector<SlcAction>& out,
                        double& tps) {
            if (!readLE(b, pos, tps))
                return false;
            uint64_t metaSize = 0;
            if (!readLE(b, pos, metaSize))
                return false;
            pos += metaSize;
            if (pos > b.size())
                return false;

            uint64_t inputCount = 0, blobCount = 0;
            if (!readLE(b, pos, inputCount) || !readLE(b, pos, blobCount))
                return false;

            struct BlobMeta {
                uint64_t byteSize, start, length;
            };
            std::vector<BlobMeta> blobs(blobCount);
            for (auto& bl : blobs) {
                if (!readLE(b, pos, bl.byteSize) || !readLE(b, pos, bl.start) ||
                    !readLE(b, pos, bl.length))
                    return false;
            }

            out.assign(inputCount, SlcAction{});
            uint64_t frame = 0;
            for (auto& bl : blobs) {
                for (uint64_t i = bl.start; i < bl.start + bl.length && i < inputCount; i++) {
                    uint64_t state = 0;
                    if (!readPacked(b, pos, bl.byteSize, state))
                        return false;
                    uint64_t delta = state >> 5;
                    int type = (int)((state >> 2) & 0b111);
                    bool player2 = (state & 2) != 0;
                    bool holding = (state & 1) != 0;
                    frame += delta;
                    if (type == 7) { // TPS -- extra trailing double, not returned
                        double newTps = 0.0;
                        if (!readLE(b, pos, newTps))
                            return false;
                    }
                    out[i] = SlcAction{frame, type, holding, player2};
                }
            }
            // 3-byte "EOM" footer isn't validated -- a malformed footer doesn't
            // change anything we've already decoded correctly.
            return true;
        }

        // ---- slc v3 ("SLC3RPLY") ----
        // Mirrors Section::read()'s exact bit layout and previous-frame
        // threading, including the swift (same-frame press+release) case and
        // the Repeat section's cluster-relative-delta re-application on each
        // repetition.
        bool parseV3Section(const std::vector<uint8_t>& b, size_t& pos, std::vector<SlcAction>& actions) {
            uint16_t header = 0;
            if (!readLE(b, pos, header))
                return false;
            int id = header >> 14;

            auto lastFrame = [&]() -> uint64_t { return actions.empty() ? 0 : actions.back().frame; };

            if (id == 0) { // Input
                int deltaSize = (header >> 12) & 0b11;
                int countExp = (header >> 8) & 0b1111;
                uint64_t byteSize = 1ull << deltaSize;
                uint64_t length = 1ull << countExp;
                for (uint64_t i = 0; i < length; i++) {
                    uint64_t state = 0;
                    if (!readPacked(b, pos, byteSize, state))
                        return false;
                    uint64_t previousFrame = lastFrame();
                    uint64_t delta = state >> 4;
                    int button = (int)((state >> 2) & 0b11);
                    bool holding = (state & 1) != 0;
                    bool player2 = (state & 2) != 0;
                    uint64_t frame = previousFrame + delta;
                    if (button == 0) { // Swift: same-frame jump press+release
                        actions.push_back({frame, 1, true, player2});
                        actions.push_back({frame, 1, false, player2});
                    } else {
                        actions.push_back({frame, button, holding, player2});
                    }
                }
                return true;
            } else if (id == 1) { // Repeat
                int deltaSize = (header >> 12) & 0b11;
                int countExp = (header >> 8) & 0b1111;
                int repeatsExp = (header >> 3) & 0b11111;
                uint64_t byteSize = 1ull << deltaSize;
                uint64_t length = 1ull << countExp;
                uint64_t repeats = 1ull << repeatsExp;

                struct Cluster {
                    uint64_t delta;
                    int button;
                    bool holding, player2;
                };
                std::vector<Cluster> cluster;
                cluster.reserve(length);
                for (uint64_t i = 0; i < length; i++) {
                    uint64_t state = 0;
                    if (!readPacked(b, pos, byteSize, state))
                        return false;
                    cluster.push_back({state >> 4,
                                       (int)((state >> 2) & 0b11),
                                       (state & 1) != 0,
                                       (state & 2) != 0});
                }
                // The cluster is stored once but replayed `repeats` times, each
                // repetition re-applying the same relative deltas from
                // wherever the real action sequence currently sits -- NOT from
                // the cluster's own first read.
                for (uint64_t r = 0; r < repeats; r++) {
                    for (auto& c : cluster) {
                        uint64_t frame = lastFrame() + c.delta;
                        if (c.button == 0) {
                            actions.push_back({frame, 1, true, c.player2});
                            actions.push_back({frame, 1, false, c.player2});
                        } else {
                            actions.push_back({frame, c.button, c.holding, c.player2});
                        }
                    }
                }
                return true;
            } else if (id == 2) { // Special
                int deltaSize = (header >> 8) & 0b11;
                int specialType = (header >> 10) & 0b1111;
                uint64_t byteSize = 1ull << deltaSize;
                uint64_t frameDelta = 0;
                if (!readPacked(b, pos, byteSize, frameDelta))
                    return false;
                uint64_t currentFrame = lastFrame() + frameDelta;
                switch (specialType) {
                case 3: { // TPS
                    double tps = 0.0;
                    if (!readLE(b, pos, tps))
                        return false;
                    actions.push_back({currentFrame, 7, false, false});
                    break;
                }
                case 0:
                case 1:
                case 2: { // Restart, RestartFull, Death -- carries a seed
                    uint64_t seed = 0;
                    if (!readLE(b, pos, seed))
                        return false;
                    actions.push_back({currentFrame, 4 + specialType, false, false});
                    break;
                }
                case 4: // Bugpoint -- no payload
                    actions.push_back({currentFrame, 8, false, false});
                    break;
                default:
                    return false;
                }
                return true;
            }
            return false; // unreachable -- id is only 2 bits
        }

        bool parseSlcV3(const std::vector<uint8_t>& b,
                        size_t pos,
                        std::vector<SlcAction>& out,
                        double& tps) {
            uint16_t metaSize = 0;
            if (!readLE(b, pos, metaSize) || metaSize != 64)
                return false;
            if (pos + 64 > b.size())
                return false;
            std::memcpy(&tps, b.data() + pos, sizeof(double)); // Metadata's first field
            pos += 64;

            size_t streamEnd = b.size() >= 1 ? b.size() - 1 : 0; // last byte is the footer

            while (pos < streamEnd) {
                uint32_t atomId = 0;
                uint64_t sizeField = 0;
                if (!readLE(b, pos, atomId) || !readLE(b, pos, sizeField))
                    return false;
                uint64_t bodySize = sizeField & 0x00FFFFFFFFFFFFFFull;
                size_t bodyStart = pos;
                if (bodyStart + bodySize > b.size())
                    return false;

                if (atomId == 1) { // Action
                    uint64_t count = 0;
                    if (!readLE(b, pos, count))
                        return false;
                    while (out.size() < count) {
                        if (pos >= bodyStart + bodySize)
                            return false;
                        if (!parseV3Section(b, pos, out))
                            return false;
                    }
                }
                pos = bodyStart + bodySize; // trust the atom's own size field either way
            }
            return true;
        }

    } // namespace

    std::optional<std::vector<SlcInput>> parseSlcReplay(const std::vector<uint8_t>& bytes,
                                                         double* outTps) {
        std::vector<SlcAction> actions;
        double tps = 240.0;
        bool ok = false;

        if (bytes.size() >= 8 && std::memcmp(bytes.data(), "SLC3RPLY", 8) == 0)
            ok = parseSlcV3(bytes, 8, actions, tps);
        else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "SILL", 4) == 0)
            ok = parseSlcV2(bytes, 4, actions, tps);
        else
            return std::nullopt;

        if (!ok)
            return std::nullopt;

        if (outTps)
            *outTps = tps;
        std::vector<SlcInput> result;
        for (auto& a : actions)
            if (a.type >= 1 && a.type <= 3)
                result.push_back({a.frame, (uint8_t)a.type, a.player2, a.holding});
        return result;
    }

} // namespace gucci

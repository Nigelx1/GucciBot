// gbr6_format.cpp — GucciBot 10.0
// Hypercompressed replay format implementation.

#include "gbr6_format.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
static void writeLE(std::vector<uint8_t>& buf, T v) {
    size_t pos = buf.size();
    buf.resize(pos + sizeof(T));
    std::memcpy(buf.data() + pos, &v, sizeof(T));
}

template<typename T>
static T readLE(const uint8_t* data, size_t& pos, size_t size) {
    if (pos + sizeof(T) > size)
        throw std::runtime_error("GBR6: unexpected end of data");
    T v;
    std::memcpy(&v, data + pos, sizeof(T));
    pos += sizeof(T);
    return v;
}

static void writeStr(std::vector<uint8_t>& buf, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(std::min(s.size(), size_t(65535)));
    writeLE<uint16_t>(buf, len);
    buf.insert(buf.end(), s.begin(), s.begin() + len);
}

static std::string readStr(const uint8_t* data, size_t& pos, size_t size) {
    uint16_t len = readLE<uint16_t>(data, pos, size);
    if (pos + len > size)
        throw std::runtime_error("GBR6: unexpected end of string");
    std::string s(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6Encoder::encode
// ─────────────────────────────────────────────────────────────────────────────
//
// Input list must be sorted by frame, alternating press/release per button.
// The encoder:
// 1. Detects autoclicker runs (repeating hold+release patterns)
// 2. Packs short hold durations as tap entries (Type B)
// 3. Falls back to normal entries (Type A) for everything else
// 4. Chains 0xFF delta bytes for gaps > 254 frames

std::vector<uint8_t> GBR6Encoder::encode(
    const std::vector<GBR6Input>& inputs,
    uint8_t tap_threshold)
{
    std::vector<uint8_t> out;
    out.reserve(inputs.size() * 2 + 32);

    if (inputs.empty()) return out;

    // ── Pass 1: detect autoclicker runs ──────────────────────────────────────
    // An autoclicker run is a contiguous sequence where:
    // - alternating press/release on the same button
    // - hold durations are all the same (acHoldTicks)
    // - release durations are all the same (acRelTicks)
    // - at least MIN_AC_CYCLES consecutive cycles
    static constexpr int MIN_AC_CYCLES = 8;

    struct Span { size_t start, end; uint8_t hold, rel; uint8_t btn; bool p2; };
    std::vector<Span> acSpans;

    {
        size_t i = 0;
        while (i + 1 < inputs.size()) {
            // Try to detect an autoclicker run starting at i
            const auto& p = inputs[i];
            if (!p.pressed) { i++; continue; }

            // Measure first cycle
            const auto& r = inputs[i + 1];
            if (r.pressed || r.button != p.button || r.player2 != p.player2) { i++; continue; }

            uint32_t hold = r.frame - p.frame;
            if (hold == 0 || hold > 15) { i++; continue; }

            // Check if there's a next cycle to measure release gap
            if (i + 2 >= inputs.size()) { i++; continue; }
            const auto& p2 = inputs[i + 2];
            if (!p2.pressed || p2.button != p.button || p2.player2 != p.player2) { i++; continue; }

            uint32_t rel = p2.frame - r.frame;
            if (rel == 0 || rel > 15) { i++; continue; }

            // Count how many cycles match
            size_t cycles = 1;
            size_t j = i + 2;
            while (j + 1 < inputs.size()) {
                const auto& cp = inputs[j];
                const auto& cr = inputs[j + 1];
                if (!cp.pressed || cr.pressed) break;
                if (cp.button != p.button || cp.player2 != p.player2) break;
                if (cr.button != p.button || cr.player2 != p.player2) break;
                if ((cr.frame - cp.frame) != hold) break;
                // Check next gap if available
                if (j + 2 < inputs.size()) {
                    const auto& cn = inputs[j + 2];
                    if (cn.pressed && cn.button == p.button && cn.player2 == p.player2) {
                        if ((cn.frame - cr.frame) != rel) break;
                    }
                }
                cycles++;
                j += 2;
            }

            if (cycles >= MIN_AC_CYCLES) {
                acSpans.push_back({i, i + cycles * 2, (uint8_t)hold, (uint8_t)rel, p.button, p.player2});
                i += cycles * 2;
            } else {
                i++;
            }
        }
    }

    // Build a set of AC-covered input indices
    std::vector<bool> inAC(inputs.size(), false);
    for (auto& sp : acSpans)
        for (size_t k = sp.start; k < sp.end; k++)
            inAC[k] = true;

    // ── Pass 2: encode ────────────────────────────────────────────────────────

    auto writeWithDelta = [&](uint8_t b0, uint32_t delta) {
        // Chain 0xFF bytes for delta > 254
        while (delta > 254) {
            out.push_back(b0);
            out.push_back(0xFF);
            delta -= 255;
        }
        out.push_back(b0);
        out.push_back(static_cast<uint8_t>(delta));
    };

    // Track which AC span we're currently in
    size_t acSpanIdx = 0;
    uint32_t prevFrame = 0;

    size_t i = 0;
    while (i < inputs.size()) {
        // Check if this is the start of an AC span
        bool foundAC = false;
        if (acSpanIdx < acSpans.size() && acSpans[acSpanIdx].start == i) {
            auto& sp = acSpans[acSpanIdx];
            uint32_t cycles = static_cast<uint32_t>((sp.end - sp.start) / 2);

            // Delta to the start of this AC run
            uint32_t delta = inputs[i].frame - prevFrame;

            // Type C entry: b0=0x00, b1=pattern
            uint8_t pattern = (sp.hold << 4) | sp.rel;

            // Write delta leading up to AC run
            // We encode the delta as part of a dummy Type A entry before the AC block
            // if delta > 0, otherwise just write the AC block directly.
            // Use a special Type A with pressed=0 as a "delta advance" marker:
            // b0 = 0x80 (delta-only, no actual button), b1 = delta
            if (delta > 0) {
                // Write delta advance: Type A, button=0 means "just advance frame"
                // We encode button=0 pressed=0 player2=0 as 0x80
                writeWithDelta(0x80, delta);
            }

            // Write Type C: b0=0, b1=pattern
            out.push_back(0x00);
            out.push_back(pattern);

            // Write cycle count as 2 bytes (little-endian uint16)
            uint16_t cycleCount = static_cast<uint16_t>(std::min(cycles, uint32_t(65535)));
            out.push_back(static_cast<uint8_t>(cycleCount & 0xFF));
            out.push_back(static_cast<uint8_t>((cycleCount >> 8) & 0xFF));

            // Advance frame by the total AC block duration
            prevFrame = inputs[i].frame + cycles * (sp.hold + sp.rel);

            i = sp.end;
            acSpanIdx++;
            foundAC = true;
        }
        if (foundAC) continue;

        if (inAC[i]) { i++; continue; }  // shouldn't happen, but guard

        const auto& inp = inputs[i];

        // Check for tap encoding: press followed immediately by release of same button
        bool tryTap = false;
        if (inp.pressed && tap_threshold > 0 && i + 1 < inputs.size() && !inAC[i + 1]) {
            const auto& nxt = inputs[i + 1];
            if (!nxt.pressed && nxt.button == inp.button && nxt.player2 == inp.player2) {
                uint32_t holdDur = nxt.frame - inp.frame;
                uint32_t gapToPress = inp.frame - prevFrame;
                // Tap encoding: hold <= 127 frames AND (gap + hold) fits in a byte
                // gap = gapToPress, total = gap + holdDur (the full slot)
                // b0 = holdDur (1-127), b1 = gapToPress (0-255)
                // But b0 must be < 0x80, and we need gapToPress to fit in a byte
                if (holdDur >= 1 && holdDur <= 127 && gapToPress <= 255 && holdDur <= tap_threshold) {
                    // Check if next input after release is far enough that
                    // the release isn't immediately followed by another press
                    // (if it is, the "gap" encoding gets confusing)
                    // Safe to encode: b0=holdDur, b1=gapToPress
                    out.push_back(static_cast<uint8_t>(holdDur));
                    out.push_back(static_cast<uint8_t>(gapToPress));
                    prevFrame = nxt.frame;  // frame after the release
                    i += 2;
                    tryTap = true;
                }
            }
        }
        if (tryTap) continue;

        // Normal Type A input
        uint32_t delta = inp.frame - prevFrame;
        uint8_t b0 = 0x80
            | ((inp.player2 ? 1 : 0) << 4)
            | ((inp.button & 0x07) << 1)
            | (inp.pressed ? 1 : 0);

        writeWithDelta(b0, delta);
        prevFrame = inp.frame;
        i++;
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6Decoder::decode
// ─────────────────────────────────────────────────────────────────────────────

std::vector<GBR6Input> GBR6Decoder::decode(
    const uint8_t* data, size_t size,
    bool is_player2)
{
    std::vector<GBR6Input> out;
    if (!data || size < 2) return out;

    uint32_t frame = 0;
    size_t i = 0;
    uint32_t pendingDelta = 0;  // accumulated from 0xFF chain

    while (i + 1 < size) {
        uint8_t b0 = data[i];
        uint8_t b1 = data[i + 1];
        i += 2;

        // ── Type C: Autoclicker run ───────────────────────────────────────────
        if (b0 == 0x00) {
            if (i + 1 >= size) break;  // need 2 more bytes for cycle count
            uint8_t countLo = data[i];
            uint8_t countHi = data[i + 1];
            i += 2;

            uint8_t hold = (b1 >> 4) & 0x0F;
            uint8_t rel  = b1 & 0x0F;
            uint32_t cycles = static_cast<uint32_t>(countLo) | (static_cast<uint32_t>(countHi) << 8);

            if (hold == 0 || rel == 0 || cycles == 0) continue;

            frame += pendingDelta;
            pendingDelta = 0;

            // Expand the AC run into individual press/release pairs
            for (uint32_t c = 0; c < cycles; c++) {
                GBR6Input press;
                press.frame    = frame;
                press.button   = 1;
                press.pressed  = true;
                press.player2  = is_player2;
                press.isAutoclicker = true;
                press.acHoldTicks   = hold;
                press.acRelTicks    = rel;
                press.acCycles      = cycles;
                out.push_back(press);

                frame += hold;

                GBR6Input release = press;
                release.frame   = frame;
                release.pressed = false;
                out.push_back(release);

                frame += rel;
            }
            continue;
        }

        // ── Extended delta (b1 == 0xFF) ───────────────────────────────────────
        if (b1 == 0xFF) {
            pendingDelta += 255;
            // b0 still carries the input info — but if b0 == 0x80 it's a pure delta advance
            if (b0 == 0x80) continue;  // pure delta, no input
            // Otherwise fall through — but we DON'T emit the input yet,
            // we need the next entry's delta first.
            // Actually for Type A with 0xFF we emit the input at frame + pendingDelta
            // on the NEXT non-0xFF entry. But that's complex.
            // Simpler: we already consumed b0 as Type A with delta=255 contribution.
            // Just add to pendingDelta and record the input.
            // For Type A with 0xFF: input fires at (frame + pendingDelta) when resolved.
            // We resolve immediately since each 0xFF entry is paired with the same b0.
            uint8_t topBit = b0 & 0x80;
            if (!topBit) {
                // Type B tap with 0xFF gap - unusual but handle it
                uint8_t holdDur = b0 & 0x7F;
                frame += pendingDelta;
                pendingDelta = 0;
                GBR6Input press;
                press.frame   = frame;
                press.button  = 1;
                press.pressed = true;
                press.player2 = is_player2;
                out.push_back(press);
                frame += holdDur;
                GBR6Input rel = press;
                rel.frame   = frame;
                rel.pressed = false;
                // gap is still 255 (b1=0xFF), frame advances naturally
                out.push_back(rel);
            }
            // For Type A (topBit set) with b1=0xFF: just accumulate delta
            continue;
        }

        uint8_t topBit = b0 & 0x80;

        // ── Type B: Tap entry ────────────────────────────────────────────────
        if (!topBit) {
            uint8_t holdDur  = b0 & 0x7F;  // 1-127 frames to hold
            uint8_t gapToPress = b1;        // frames from prevFrame to this press

            frame += pendingDelta + gapToPress;
            pendingDelta = 0;

            // Press
            GBR6Input press;
            press.frame   = frame;
            press.button  = 1;  // tap encoding is always button 1 (jump)
            press.pressed = true;
            press.player2 = is_player2;
            out.push_back(press);

            // Release
            frame += holdDur;
            GBR6Input rel = press;
            rel.frame   = frame;
            rel.pressed = false;
            out.push_back(rel);
            continue;
        }

        // ── Type A: Normal input ─────────────────────────────────────────────
        // b0 = 0x80 | (p2<<4) | (btn<<1) | pressed
        // Special: b0 == 0x80 exactly means "delta advance only" (no actual input)
        bool   p2      = (b0 >> 4) & 0x01;
        uint8_t btn    = (b0 >> 1) & 0x07;
        bool   pressed = b0 & 0x01;

        frame += pendingDelta + b1;
        pendingDelta = 0;

        if (b0 == 0x80) continue;  // delta-only marker, no input

        GBR6Input inp;
        inp.frame   = frame;
        inp.button  = btn == 0 ? 1 : btn;  // button 0 treated as 1 (jump)
        inp.pressed = pressed;
        inp.player2 = p2 || is_player2;
        out.push_back(inp);
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File::decode
// ─────────────────────────────────────────────────────────────────────────────

void GBR6File::decode() {
    p1Inputs = GBR6Decoder::decode(p1Raw.data(), p1Raw.size(), false);
    p2Inputs = GBR6Decoder::decode(p2Raw.data(), p2Raw.size(), true);
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File::serialize
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> GBR6File::serialize() const {
    std::vector<uint8_t> buf;

    // Magic
    buf.insert(buf.end(), GBR6_MAGIC, GBR6_MAGIC + 4);

    // Version + flags
    buf.push_back(header.version);
    buf.push_back(header.flags);

    // TPS
    writeLE<float>(buf, header.tps);

    // Timestamp
    writeLE<int64_t>(buf, header.timestamp);

    // RNG seed
    writeLE<uint64_t>(buf, header.rngSeed);

    // Counts
    writeLE<uint32_t>(buf, static_cast<uint32_t>(p1Raw.size()));
    writeLE<uint32_t>(buf, static_cast<uint32_t>(p2Raw.size()));

    // Name
    writeStr(buf, header.name);

    // P1 data
    buf.insert(buf.end(), p1Raw.begin(), p1Raw.end());

    // P2 data
    buf.insert(buf.end(), p2Raw.begin(), p2Raw.end());

    // v8.5: optional trailing level-name block (only when the flag is set, so
    // pre-8.5 files — flag clear — are byte-identical and load unchanged).
    if (header.flags & GBR6_HAS_LEVELNAME)
        writeStr(buf, header.levelName);

    // v10.3 (P1 fix): optional trailing death/restart-marker block. Same
    // flag-gated pattern as level-name: absent (and byte-identical to old files)
    // when GBR6_HAS_DEATHS is clear. Layout: uint32 count, then count ×
    // (uint32 frame, uint8 type).
    if (header.flags & GBR6_HAS_DEATHS) {
        writeLE<uint32_t>(buf, static_cast<uint32_t>(deaths.size()));
        for (auto const& d : deaths) {
            writeLE<uint32_t>(buf, d.frame);
            buf.push_back(d.type);
        }
    }

    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File::deserialize
// ─────────────────────────────────────────────────────────────────────────────

std::optional<GBR6File> GBR6File::deserialize(const uint8_t* data, size_t size) {
    if (size < 4) return std::nullopt;

    // Check magic
    if (std::memcmp(data, GBR6_MAGIC, 4) != 0) return std::nullopt;

    GBR6File f;
    size_t pos = 4;

    try {
        f.header.version   = readLE<uint8_t>(data, pos, size);
        f.header.flags     = readLE<uint8_t>(data, pos, size);
        f.header.tps       = readLE<float>(data, pos, size);
        f.header.timestamp = readLE<int64_t>(data, pos, size);
        f.header.rngSeed   = readLE<uint64_t>(data, pos, size);

        uint32_t p1size = readLE<uint32_t>(data, pos, size);
        uint32_t p2size = readLE<uint32_t>(data, pos, size);

        f.header.name = readStr(data, pos, size);

        if (pos + p1size + p2size > size)
            return std::nullopt;

        f.p1Raw.assign(data + pos, data + pos + p1size);
        pos += p1size;

        f.p2Raw.assign(data + pos, data + pos + p2size);
        pos += p2size;

        // v8.5: read the optional level-name block if this file declares one
        if ((f.header.flags & GBR6_HAS_LEVELNAME) && pos < size)
            f.header.levelName = readStr(data, pos, size);

        // v10.3 (P1 fix): read the optional death/restart-marker block.
        if ((f.header.flags & GBR6_HAS_DEATHS) && pos < size) {
            uint32_t dcount = readLE<uint32_t>(data, pos, size);
            for (uint32_t i = 0; i < dcount; ++i) {
                GBR6Death d;
                d.frame = readLE<uint32_t>(data, pos, size);
                d.type  = readLE<uint8_t>(data, pos, size);
                f.deaths.push_back(d);
            }
        }

        f.decode();
        return f;
    } catch (...) {
        return std::nullopt;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File I/O
// ─────────────────────────────────────────────────────────────────────────────

bool GBR6File::saveToPath(const std::filesystem::path& path) const {
    auto bytes = serialize();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return f.good();
}

std::optional<GBR6File> GBR6File::loadFromPath(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    return deserialize(bytes.data(), bytes.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File::fromInputs
// ─────────────────────────────────────────────────────────────────────────────

GBR6File GBR6File::fromInputs(
    GBR6Header hdr,
    std::vector<GBR6Input> p1,
    std::vector<GBR6Input> p2)
{
    GBR6File f;
    f.header = hdr;

    // Sort inputs by frame
    std::sort(p1.begin(), p1.end(), [](auto& a, auto& b){ return a.frame < b.frame; });
    std::sort(p2.begin(), p2.end(), [](auto& a, auto& b){ return a.frame < b.frame; });

    f.p1Inputs = p1;
    f.p2Inputs = p2;
    f.p1Raw = GBR6Encoder::encode(p1);
    f.p2Raw = GBR6Encoder::encode(p2);

    f.header.p1Count = static_cast<uint32_t>(f.p1Raw.size());
    f.header.p2Count = static_cast<uint32_t>(f.p2Raw.size());

    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File::fromLegacy — convert old frame/pressed pairs
// ─────────────────────────────────────────────────────────────────────────────

GBR6File GBR6File::fromLegacy(
    const std::string& name,
    float tps,
    const std::vector<std::pair<uint32_t, bool>>& p1FramePressed,
    const std::vector<std::pair<uint32_t, bool>>& p2FramePressed)
{
    auto toInputs = [](const std::vector<std::pair<uint32_t, bool>>& src, bool p2) {
        std::vector<GBR6Input> out;
        for (auto& [frame, pressed] : src) {
            GBR6Input inp;
            inp.frame   = frame;
            inp.button  = 1;
            inp.pressed = pressed;
            inp.player2 = p2;
            out.push_back(inp);
        }
        return out;
    };

    GBR6Header hdr;
    hdr.name      = name;
    hdr.tps       = tps;
    hdr.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    return fromInputs(hdr,
        toInputs(p1FramePressed, false),
        toInputs(p2FramePressed, true));
}

#include "core/gbr6_format.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace gucci {

    template <typename T> static void writeLE(std::vector<uint8_t>& buf, T v) {
        size_t pos = buf.size();
        buf.resize(pos + sizeof(T));
        std::memcpy(buf.data() + pos, &v, sizeof(T));
    }

    template <typename T> static T readLE(const uint8_t* data, size_t& pos, size_t size) {
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

    std::vector<uint8_t> GBR6Encoder::encode(const std::vector<GBR6Input>& inputs,
                                             uint8_t tap_threshold) {
        std::vector<uint8_t> out;
        out.reserve(inputs.size() * 2 + 32);

        if (inputs.empty())
            return out;

        static constexpr int MIN_AC_CYCLES = 8;

        struct Span {
            size_t start, end;
            uint8_t hold, rel;
            uint8_t btn;
            bool p2;
        };
        std::vector<Span> acSpans;

        {
            size_t i = 0;
            while (i + 1 < inputs.size()) {
                const auto& p = inputs[i];
                if (!p.pressed) {
                    i++;
                    continue;
                }

                const auto& r = inputs[i + 1];
                if (r.pressed || r.button != p.button || r.player2 != p.player2) {
                    i++;
                    continue;
                }

                uint32_t hold = r.frame - p.frame;
                if (hold == 0 || hold > 15) {
                    i++;
                    continue;
                }

                if (i + 2 >= inputs.size()) {
                    i++;
                    continue;
                }
                const auto& p2 = inputs[i + 2];
                if (!p2.pressed || p2.button != p.button || p2.player2 != p.player2) {
                    i++;
                    continue;
                }

                uint32_t rel = p2.frame - r.frame;
                if (rel == 0 || rel > 15) {
                    i++;
                    continue;
                }

                size_t cycles = 1;
                size_t j = i + 2;
                while (j + 1 < inputs.size()) {
                    const auto& cp = inputs[j];
                    const auto& cr = inputs[j + 1];
                    if (!cp.pressed || cr.pressed)
                        break;
                    if (cp.button != p.button || cp.player2 != p.player2)
                        break;
                    if (cr.button != p.button || cr.player2 != p.player2)
                        break;
                    if ((cr.frame - cp.frame) != hold)
                        break;
                    if (j + 2 < inputs.size()) {
                        const auto& cn = inputs[j + 2];
                        if (cn.pressed && cn.button == p.button && cn.player2 == p.player2) {
                            if ((cn.frame - cr.frame) != rel)
                                break;
                        }
                    }
                    cycles++;
                    j += 2;
                }

                if (cycles >= MIN_AC_CYCLES) {
                    acSpans.push_back(
                        {i, i + cycles * 2, (uint8_t)hold, (uint8_t)rel, p.button, p.player2});
                    i += cycles * 2;
                } else {
                    i++;
                }
            }
        }

        std::vector<bool> inAC(inputs.size(), false);
        for (auto& sp : acSpans)
            for (size_t k = sp.start; k < sp.end; k++)
                inAC[k] = true;

        auto writeWithDelta = [&](uint8_t b0, uint32_t delta) {
            while (delta > 254) {
                out.push_back(b0);
                out.push_back(0xFF);
                delta -= 255;
            }
            out.push_back(b0);
            out.push_back(static_cast<uint8_t>(delta));
        };

        size_t acSpanIdx = 0;
        uint32_t prevFrame = 0;

        size_t i = 0;
        while (i < inputs.size()) {
            bool foundAC = false;
            if (acSpanIdx < acSpans.size() && acSpans[acSpanIdx].start == i) {
                auto& sp = acSpans[acSpanIdx];
                uint32_t cycles = static_cast<uint32_t>((sp.end - sp.start) / 2);

                uint32_t delta = inputs[i].frame - prevFrame;

                uint8_t pattern = (sp.hold << 4) | sp.rel;

                if (delta > 0) {
                    writeWithDelta(0x80, delta);
                }

                out.push_back(0x00);
                out.push_back(pattern);

                uint16_t cycleCount = static_cast<uint16_t>(std::min(cycles, uint32_t(65535)));
                out.push_back(static_cast<uint8_t>(cycleCount & 0xFF));
                out.push_back(static_cast<uint8_t>((cycleCount >> 8) & 0xFF));

                prevFrame = inputs[i].frame + cycles * (sp.hold + sp.rel);

                i = sp.end;
                acSpanIdx++;
                foundAC = true;
            }
            if (foundAC)
                continue;

            if (inAC[i]) {
                i++;
                continue;
            }

            const auto& inp = inputs[i];

            bool tryTap = false;
            if (inp.pressed && tap_threshold > 0 && i + 1 < inputs.size() && !inAC[i + 1]) {
                const auto& nxt = inputs[i + 1];
                if (!nxt.pressed && nxt.button == inp.button && nxt.player2 == inp.player2) {
                    uint32_t holdDur = nxt.frame - inp.frame;
                    uint32_t gapToPress = inp.frame - prevFrame;
                    if (holdDur >= 1 && holdDur <= 127 && gapToPress <= 255 &&
                        holdDur <= tap_threshold) {
                        out.push_back(static_cast<uint8_t>(holdDur));
                        out.push_back(static_cast<uint8_t>(gapToPress));
                        prevFrame = nxt.frame;
                        i += 2;
                        tryTap = true;
                    }
                }
            }
            if (tryTap)
                continue;

            uint32_t delta = inp.frame - prevFrame;
            uint8_t b0 = 0x80 | ((inp.player2 ? 1 : 0) << 4) | ((inp.button & 0x07) << 1) |
                         (inp.pressed ? 1 : 0);

            writeWithDelta(b0, delta);
            prevFrame = inp.frame;
            i++;
        }

        return out;
    }

    std::vector<GBR6Input> GBR6Decoder::decode(const uint8_t* data, size_t size, bool is_player2) {
        std::vector<GBR6Input> out;
        if (!data || size < 2)
            return out;

        uint32_t frame = 0;
        size_t i = 0;
        uint32_t pendingDelta = 0;

        while (i + 1 < size) {
            uint8_t b0 = data[i];
            uint8_t b1 = data[i + 1];
            i += 2;

            if (b0 == 0x00) {
                if (i + 1 >= size)
                    break;
                uint8_t countLo = data[i];
                uint8_t countHi = data[i + 1];
                i += 2;

                uint8_t hold = (b1 >> 4) & 0x0F;
                uint8_t rel = b1 & 0x0F;
                uint32_t cycles =
                    static_cast<uint32_t>(countLo) | (static_cast<uint32_t>(countHi) << 8);

                if (hold == 0 || rel == 0 || cycles == 0)
                    continue;

                frame += pendingDelta;
                pendingDelta = 0;

                for (uint32_t c = 0; c < cycles; c++) {
                    GBR6Input press;
                    press.frame = frame;
                    press.button = 1;
                    press.pressed = true;
                    press.player2 = is_player2;
                    press.isAutoclicker = true;
                    press.acHoldTicks = hold;
                    press.acRelTicks = rel;
                    press.acCycles = cycles;
                    out.push_back(press);

                    frame += hold;

                    GBR6Input release = press;
                    release.frame = frame;
                    release.pressed = false;
                    out.push_back(release);

                    frame += rel;
                }
                continue;
            }

            if (b1 == 0xFF) {
                pendingDelta += 255;
                if (b0 == 0x80)
                    continue;
                uint8_t topBit = b0 & 0x80;
                if (!topBit) {
                    uint8_t holdDur = b0 & 0x7F;
                    frame += pendingDelta;
                    pendingDelta = 0;
                    GBR6Input press;
                    press.frame = frame;
                    press.button = 1;
                    press.pressed = true;
                    press.player2 = is_player2;
                    out.push_back(press);
                    frame += holdDur;
                    GBR6Input rel = press;
                    rel.frame = frame;
                    rel.pressed = false;
                    out.push_back(rel);
                }
                continue;
            }

            uint8_t topBit = b0 & 0x80;

            if (!topBit) {
                uint8_t holdDur = b0 & 0x7F;
                uint8_t gapToPress = b1;

                frame += pendingDelta + gapToPress;
                pendingDelta = 0;

                GBR6Input press;
                press.frame = frame;
                press.button = 1;
                press.pressed = true;
                press.player2 = is_player2;
                out.push_back(press);

                frame += holdDur;
                GBR6Input rel = press;
                rel.frame = frame;
                rel.pressed = false;
                out.push_back(rel);
                continue;
            }

            bool p2 = (b0 >> 4) & 0x01;
            uint8_t btn = (b0 >> 1) & 0x07;
            bool pressed = b0 & 0x01;

            frame += pendingDelta + b1;
            pendingDelta = 0;

            if (b0 == 0x80)
                continue;

            GBR6Input inp;
            inp.frame = frame;
            inp.button = btn == 0 ? 1 : btn;
            inp.pressed = pressed;
            inp.player2 = p2 || is_player2;
            out.push_back(inp);
        }

        return out;
    }

    void GBR6File::decode() {
        p1Inputs = GBR6Decoder::decode(p1Raw.data(), p1Raw.size(), false);
        p2Inputs = GBR6Decoder::decode(p2Raw.data(), p2Raw.size(), true);
    }

    std::vector<uint8_t> GBR6File::serialize() const {
        std::vector<uint8_t> buf;

        buf.insert(buf.end(), GBR6_MAGIC, GBR6_MAGIC + 4);

        buf.push_back(header.version);
        buf.push_back(header.flags);

        writeLE<float>(buf, header.tps);

        writeLE<int64_t>(buf, header.timestamp);

        writeLE<uint64_t>(buf, header.rngSeed);

        writeLE<uint32_t>(buf, static_cast<uint32_t>(p1Raw.size()));
        writeLE<uint32_t>(buf, static_cast<uint32_t>(p2Raw.size()));

        writeStr(buf, header.name);

        buf.insert(buf.end(), p1Raw.begin(), p1Raw.end());

        buf.insert(buf.end(), p2Raw.begin(), p2Raw.end());

        if (header.flags & GBR6_HAS_LEVELNAME)
            writeStr(buf, header.levelName);

        if (header.flags & GBR6_HAS_DEATHS) {
            writeLE<uint32_t>(buf, static_cast<uint32_t>(deaths.size()));
            for (auto const& d : deaths) {
                writeLE<uint32_t>(buf, d.frame);
                buf.push_back(d.type);
            }
        }

        return buf;
    }

    std::optional<GBR6File> GBR6File::deserialize(const uint8_t* data, size_t size) {
        if (size < 4)
            return std::nullopt;

        if (std::memcmp(data, GBR6_MAGIC, 4) != 0)
            return std::nullopt;

        GBR6File f;
        size_t pos = 4;

        try {
            f.header.version = readLE<uint8_t>(data, pos, size);
            f.header.flags = readLE<uint8_t>(data, pos, size);
            f.header.tps = readLE<float>(data, pos, size);
            f.header.timestamp = readLE<int64_t>(data, pos, size);
            f.header.rngSeed = readLE<uint64_t>(data, pos, size);

            uint32_t p1size = readLE<uint32_t>(data, pos, size);
            uint32_t p2size = readLE<uint32_t>(data, pos, size);

            f.header.name = readStr(data, pos, size);

            if (pos + p1size + p2size > size)
                return std::nullopt;

            f.p1Raw.assign(data + pos, data + pos + p1size);
            pos += p1size;

            f.p2Raw.assign(data + pos, data + pos + p2size);
            pos += p2size;

            if ((f.header.flags & GBR6_HAS_LEVELNAME) && pos < size)
                f.header.levelName = readStr(data, pos, size);

            if ((f.header.flags & GBR6_HAS_DEATHS) && pos < size) {
                uint32_t dcount = readLE<uint32_t>(data, pos, size);
                for (uint32_t i = 0; i < dcount; ++i) {
                    GBR6Death d;
                    d.frame = readLE<uint32_t>(data, pos, size);
                    d.type = readLE<uint8_t>(data, pos, size);
                    f.deaths.push_back(d);
                }
            }

            f.decode();
            return f;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool GBR6File::saveToPath(const std::filesystem::path& path) const {
        auto bytes = serialize();
        std::ofstream f(path, std::ios::binary);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return f.good();
    }

    std::optional<GBR6File> GBR6File::loadFromPath(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return std::nullopt;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        return deserialize(bytes.data(), bytes.size());
    }

    GBR6File
    GBR6File::fromInputs(GBR6Header hdr, std::vector<GBR6Input> p1, std::vector<GBR6Input> p2) {
        GBR6File f;
        f.header = hdr;

        std::sort(p1.begin(), p1.end(), [](auto& a, auto& b) {
            return a.frame < b.frame;
        });
        std::sort(p2.begin(), p2.end(), [](auto& a, auto& b) {
            return a.frame < b.frame;
        });

        f.p1Inputs = p1;
        f.p2Inputs = p2;
        f.p1Raw = GBR6Encoder::encode(p1);
        f.p2Raw = GBR6Encoder::encode(p2);

        f.header.p1Count = static_cast<uint32_t>(f.p1Raw.size());
        f.header.p2Count = static_cast<uint32_t>(f.p2Raw.size());

        return f;
    }

    GBR6File GBR6File::fromLegacy(const std::string& name,
                                  float tps,
                                  const std::vector<std::pair<uint32_t, bool>>& p1FramePressed,
                                  const std::vector<std::pair<uint32_t, bool>>& p2FramePressed) {
        auto toInputs = [](const std::vector<std::pair<uint32_t, bool>>& src, bool p2) {
            std::vector<GBR6Input> out;
            for (auto& [frame, pressed] : src) {
                GBR6Input inp;
                inp.frame = frame;
                inp.button = 1;
                inp.pressed = pressed;
                inp.player2 = p2;
                out.push_back(inp);
            }
            return out;
        };

        GBR6Header hdr;
        hdr.name = name;
        hdr.tps = tps;
        hdr.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

        return fromInputs(hdr, toInputs(p1FramePressed, false), toInputs(p2FramePressed, true));
    }

} // namespace gucci

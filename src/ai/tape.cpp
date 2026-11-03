#include "ai/tape.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace gucci::ai::tape {

    namespace {
        uint64_t fnv1a(std::string const& s) {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }

        matjson::Value channelLayout() {
            auto list = matjson::Value::array();
            auto add = [&](char const* name, int offset, int size, char const* what) {
                auto e = matjson::Value::object();
                e["name"] = name;
                e["offset"] = offset;
                e["size"] = size;
                e["what"] = what;
                list.push(e);
            };
            add("deadly", 0, kBands,
                "one byte per height band (0/1): a hazard or hazard slope's hitbox overlaps it; band b covers "
                "y0 + b*band_height .. + band_height");
            add("solid", kBands, kBands, "one byte per height band (0/1): a block or slope overlaps it");
            add("tokens", 2 * kBands, kTokenChannels,
                "per token type 1..31 (gucci_ai_layout's token table): how many have their center in the column "
                "(saturates at 255)");
            return list;
        }

        std::string layoutText() {
            return fmt::format("gbai-tape v1 2026-11-02 stageB; x0 {} col {} y0 {} band {} bands {} channels {}; "
                               "deadly bytes, solid bytes, token counts 1..31; static objects only; raw hitboxes",
                               kX0, kColumnWidth, kY0, kBandHeight, kBands, kChannels);
        }
    }  // namespace

    std::string layoutHash() {
        static std::string const h = fmt::format("{:016x}", fnv1a(layoutText()));
        return h;
    }

    matjson::Value layoutJson() {
        auto out = matjson::Value::object();
        out["tape_channels"] = kChannels;
        out["tape_layout_hash"] = layoutHash();
        auto l = matjson::Value::object();
        l["column_width"] = (double)kColumnWidth;
        l["x0"] = (double)kX0;
        l["y0"] = (double)kY0;
        l["band_height"] = (double)kBandHeight;
        l["bands"] = kBands;
        l["channel_layout"] = channelLayout();
        out["tape_layout"] = l;
        return out;
    }

    Tape build(obs::LevelIndex const& index) {
        Tape t;
        float maxX = index.levelLength;
        for (auto const& it : index.items)
            if (!it.mover) maxX = std::max(maxX, it.x1);
        t.columns = std::max(1, (int)std::ceil((maxX - kX0) / kColumnWidth) + 1);
        t.data.assign((size_t)t.columns * kChannels, 0);
        auto col = [&](float x) { return (int)std::floor((x - kX0) / kColumnWidth); };
        auto band = [&](float y) { return (int)std::floor((y - kY0) / kBandHeight); };
        for (auto const& it : index.items) {
            if (it.mover) continue;
            if (it.type == obs::Item::Special) {
                if (it.token < 1 || it.token > kTokenChannels) continue;
                int const c = col(it.cx);
                if (c < 0 || c >= t.columns) continue;
                uint8_t& v = t.data[(size_t)c * kChannels + 2 * kBands + (it.token - 1)];
                if (v < 255) v++;
                continue;
            }
            bool const deadly = it.type == obs::Item::Hazard || it.hazardSlope;
            // A block kills from its inner core too: for the tape both count
            // as solid, and a block is not deadly (the player's size decides
            // how much of it kills, and the tape has no player).
            int const base = deadly ? 0 : kBands;
            int const c0 = std::max(0, col(it.x0)), c1 = std::min(t.columns - 1, col(it.x1));
            int const b0 = std::max(0, band(it.y0)), b1 = std::min(kBands - 1, band(it.y1));
            if (b1 < 0 || b0 >= kBands) continue;
            for (int c = c0; c <= c1; c++)
                for (int b = b0; b <= b1; b++) t.data[(size_t)c * kChannels + base + b] = 1;
        }
        return t;
    }

    bool write(std::filesystem::path const& dir, obs::LevelIndex const& index, std::string const& levelName,
               Tape const& tape, std::string& why) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto const stem = std::to_string(index.levelId);
        {
            std::ofstream f(dir / (stem + ".tape.u8"), std::ios::binary | std::ios::trunc);
            if (!f) {
                why = "can't write the tape";
                return false;
            }
            f.write(reinterpret_cast<char const*>(tape.data.data()), (std::streamsize)tape.data.size());
            if (!f) {
                why = "can't write the tape";
                return false;
            }
        }
        auto h = matjson::Value::object();
        h["format"] = "gbai-tape";
        h["version"] = 1;
        h["level_id"] = index.levelId;
        h["level_name"] = levelName;
        h["column_width"] = (double)kColumnWidth;
        h["x0"] = (double)kX0;
        h["columns"] = tape.columns;
        h["bands"] = kBands;
        h["band_height"] = (double)kBandHeight;
        h["y0"] = (double)kY0;
        h["channels"] = kChannels;
        h["channel_layout"] = channelLayout();
        h["layout_hash"] = layoutHash();
        h["level_length_x"] = (double)index.levelLength;
        // The header last, written whole then renamed, so a reader never sees
        // a header without its data.
        auto const tmp = dir / (stem + ".json.tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << h.dump(2);
            if (!f) {
                why = "can't write the tape's header";
                return false;
            }
        }
        std::filesystem::rename(tmp, dir / (stem + ".json"), ec);
        if (ec) {
            why = "can't write the tape's header: " + ec.message();
            return false;
        }
        return true;
    }

} // namespace gucci::ai::tape

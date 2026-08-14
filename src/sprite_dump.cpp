#include "GucciBot.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <filesystem>
#include <fstream>
#include <vector>
#include <zlib.h>

using namespace geode::prelude;

namespace {

void writeBE32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)((v >> 24) & 0xFF));
    buf.push_back((uint8_t)((v >> 16) & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    buf.push_back((uint8_t)(v & 0xFF));
}

void writeChunk(std::ofstream& out, const char* type, std::vector<uint8_t> const& data) {
    std::vector<uint8_t> lenBuf;
    writeBE32(lenBuf, (uint32_t)data.size());
    out.write((const char*)lenBuf.data(), 4);

    std::vector<uint8_t> typeAndData(type, type + 4);
    typeAndData.insert(typeAndData.end(), data.begin(), data.end());
    out.write((const char*)typeAndData.data(), (std::streamsize)typeAndData.size());

    uLong crc = crc32(0L, typeAndData.data(), (uInt)typeAndData.size());
    std::vector<uint8_t> crcBuf;
    writeBE32(crcBuf, (uint32_t)crc);
    out.write((const char*)crcBuf.data(), 4);
}

// Hand-rolled PNG encoder -- bypasses CCRenderTexture::saveToFile entirely
// (which returned false for every single object in the first real test,
// most likely cocos2d's own file-path handling not liking a Windows
// absolute path). Raw RGBA pixels -> zlib-compressed scanlines (proven zlib
// usage already exists in this codebase, see brr_format.cpp) -> written via
// plain std::ofstream, the file-write pattern already proven to work
// everywhere else in this mod.
bool writePng(std::filesystem::path const& path, const unsigned char* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return false;

    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * (1 + (size_t)width * 4));
    for (int y = 0; y < height; y++) {
        raw.push_back(0); // filter type: None
        const unsigned char* row = rgba + (size_t)y * (size_t)width * 4;
        raw.insert(raw.end(), row, row + (size_t)width * 4);
    }

    uLongf boundLen = compressBound((uLong)raw.size());
    std::vector<uint8_t> compressed(boundLen);
    int rc = compress2(compressed.data(), &boundLen, raw.data(), (uLong)raw.size(), Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return false;
    compressed.resize(boundLen);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    out.write((const char*)sig, 8);

    std::vector<uint8_t> ihdr;
    writeBE32(ihdr, (uint32_t)width);
    writeBE32(ihdr, (uint32_t)height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // color type: RGBA
    ihdr.push_back(0); // compression method
    ihdr.push_back(0); // filter method
    ihdr.push_back(0); // interlace method
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", compressed);
    writeChunk(out, "IEND", {});

    return out.good();
}

}

// One-time diagnostic, same spirit as objectid_dump.cpp but for actual pixels
// instead of just the object-type enum: renders each not-yet-captured object's
// real sprite (unrotated, unscaled -- as authored in the texture atlas) to a
// small transparent PNG, so shape assumptions for procedural decoration don't
// have to be guessed from rotation/scale statistics anymore.
//
// Take 3: the first two attempts confirmed the render pipeline itself works
// (real non-degenerate texture rects were being read for every object) but
// CCRenderTexture::saveToFile returned false for all of them -- so this
// bypasses it and writes the PNG manually instead of trusting cocos2d's own
// file writer.
class $modify(SpriteDumpPL, PlayLayer) {
    void createObjectsFromSetupFinished() {
        PlayLayer::createObjectsFromSetupFinished();
        if (!m_objects) return;

        auto dir = Mod::get()->getSaveDir() / "sprite_dump";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::ofstream dbg(Mod::get()->getSaveDir() / "sprite_dump_debug.log", std::ios::app);
        dbg << "--- createObjectsFromSetupFinished, m_objects count="
            << m_objects->count() << " ---\n";

        int captured = 0;
        int attempted = 0;
        for (auto* go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            int id = go->m_objectID;
            auto path = dir / (std::to_string(id) + ".png");
            if (std::filesystem::exists(path)) continue;
            attempted++;

            auto* frame = go->displayFrame();
            if (!frame) {
                dbg << "id=" << id << " SKIP no displayFrame\n";
                continue;
            }

            // Build a brand-new standalone sprite from the frame object
            // itself -- this carries the atlas's own correct crop/rotated/
            // offset metadata, instead of me reconstructing that by hand
            // (which is what produced neighboring-sprite bleed and blanks
            // last attempt). Also sidesteps any CCSpriteBatchNode concerns
            // since this is a fresh, unparented node.
            auto* snap = CCSprite::createWithSpriteFrame(frame);
            if (!snap) {
                dbg << "id=" << id << " SKIP createWithSpriteFrame failed\n";
                continue;
            }
            CCSize contentSize = snap->getContentSize();
            dbg << "id=" << id << " contentSize=(" << contentSize.width << ","
                << contentSize.height << ")";
            if (contentSize.width <= 0.f || contentSize.height <= 0.f) {
                dbg << " SKIP degenerate contentSize\n";
                continue;
            }

            float w = std::min(contentSize.width + 24.f, 320.f);
            float h = std::min(contentSize.height + 24.f, 320.f);
            snap->setRotation(0.f);
            snap->setScaleX(1.f);
            snap->setScaleY(1.f);
            snap->setPosition({w / 2.f, h / 2.f});

            auto* rt = CCRenderTexture::create((int)w, (int)h);
            if (!rt) {
                dbg << "id=" << id << " SKIP CCRenderTexture::create failed\n";
                continue;
            }
            rt->beginWithClear(0.f, 0.f, 0.f, 0.f);
            snap->visit();
            rt->end();

            auto* img = rt->newCCImage();
            if (!img) {
                dbg << "id=" << id << " SKIP newCCImage failed\n";
                continue;
            }
            int imgW = img->getWidth();
            int imgH = img->getHeight();
            bool ok = writePng(path, img->getData(), imgW, imgH);
            img->release();

            bool existsAfter = std::filesystem::exists(path);
            dbg << "id=" << id << " imgSize=" << imgW << "x" << imgH
                << " writePng=" << (ok ? "true" : "false")
                << " existsOnDisk=" << (existsAfter ? "true" : "false") << "\n";

            if (existsAfter) captured++;
        }
        dbg << "attempted=" << attempted << " captured=" << captured << "\n";
    }
};

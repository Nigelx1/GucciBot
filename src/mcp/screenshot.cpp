// gucci_screenshot's capture and PNG writer (mcp/screenshot.hpp).

#include "mcp/screenshot.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/cocos/platform/win32/CCGL.h>

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace gucci::mcp::shot {

    namespace {

        using Clock = std::chrono::steady_clock;

        // The shot waiting for its frame. Main thread only: the tool asks and
        // the swap hook takes it.
        struct Ask {
            bool armed = false;
            int framesLeft = 0;
            uint64_t id = 0;  // counts asks, so a late result can't land on a newer one
            Clock::time_point at;
        };
        Ask g_ask;

        // The last ask's outcome, which the writer thread finishes.
        std::mutex g_statusMutex;
        Status g_status;
        uint64_t g_statusId = 0;

        // One file written at a time: writeBinarySafe goes through <path>.tmp,
        // which two shots with the same name would share.
        std::mutex g_writeMutex;

        // The window's framebuffer in pixels, asked of the OS as the renderer
        // does (renderer.cpp's getWindowFramebufferSize): while a render runs,
        // cocos's view size is the render's, and DPI scaling changes it too.
        bool windowSize(int& width, int& height) {
            HDC const dc = wglGetCurrentDC();
            HWND const window = dc ? WindowFromDC(dc) : nullptr;
            RECT rect{};
            if (!window || !GetClientRect(window, &rect))
                return false;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            return width > 0 && height > 0;
        }

        void put32(std::vector<uint8_t>& out, uint32_t v) {
            uint8_t const be[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
            out.insert(out.end(), be, be + 4);
        }

        // A PNG chunk: length, type, data, and the CRC of type and data.
        void chunk(std::vector<uint8_t>& out, char const* type, uint8_t const* data, size_t size) {
            put32(out, static_cast<uint32_t>(size));
            size_t const start = out.size();
            out.insert(out.end(), type, type + 4);
            if (size > 0)
                out.insert(out.end(), data, data + size);
            uLong const crc = crc32(0L, out.data() + start, static_cast<uInt>(out.size() - start));
            put32(out, static_cast<uint32_t>(crc));
        }

        // GL's RGBA rows (bottom row first) as an RGB PNG. Alpha is dropped:
        // the window's own alpha is whatever was last drawn into it.
        std::vector<uint8_t> encodePng(std::vector<uint8_t> const& rgba, int width, int height) {
            // Top row first, each led by filter type 0 (none).
            size_t const w = static_cast<size_t>(width);
            size_t const h = static_cast<size_t>(height);
            size_t const stride = 1 + w * 3;
            std::vector<uint8_t> raw(stride * h);
            for (size_t y = 0; y < h; ++y) {
                uint8_t* dst = raw.data() + y * stride;
                uint8_t const* src = rgba.data() + (h - 1 - y) * w * 4;
                *dst++ = 0;
                for (size_t x = 0; x < w; ++x, dst += 3, src += 4) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }

            uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
            std::vector<uint8_t> packed(packedSize);
            if (compress2(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()),
                          Z_DEFAULT_COMPRESSION) != Z_OK)
                throw std::runtime_error("zlib couldn't compress the image");

            // IHDR: width, height, 8 bits a channel, color type 2 (RGB), then
            // deflate, filtering method 0 and no interlace.
            uint8_t ihdr[13] = {};
            for (int i = 0; i < 4; ++i) {
                ihdr[i] = uint8_t(uint32_t(width) >> (24 - 8 * i));
                ihdr[4 + i] = uint8_t(uint32_t(height) >> (24 - 8 * i));
            }
            ihdr[8] = 8;
            ihdr[9] = 2;

            static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            std::vector<uint8_t> png(kSignature, kSignature + 8);
            png.reserve(8 + 25 + 12 + packedSize + 12);
            chunk(png, "IHDR", ihdr, sizeof(ihdr));
            chunk(png, "IDAT", packed.data(), packedSize);
            chunk(png, "IEND", nullptr, 0);
            return png;
        }

        // The ask's outcome, unless a newer ask has taken its place (its file
        // is still written).
        void finish(uint64_t id, std::string error, Clock::time_point at) {
            std::lock_guard lock(g_statusMutex);
            if (g_statusId != id)
                return;
            g_status.pending = false;
            g_status.done = true;
            g_status.ok = error.empty();
            g_status.error = std::move(error);
            g_status.ms = std::chrono::duration<double, std::milli>(Clock::now() - at).count();
        }

    } // namespace

    void request(std::filesystem::path path, int frames) {
        g_ask.armed = true;
        g_ask.framesLeft = frames;
        g_ask.id++;
        g_ask.at = Clock::now();
        std::lock_guard lock(g_statusMutex);
        g_statusId = g_ask.id;
        g_status = Status{};
        g_status.asked = true;
        g_status.pending = true;
        g_status.framesLeft = frames;
        g_status.path = std::move(path);
    }

    Status status() {
        Status out;
        {
            std::lock_guard lock(g_statusMutex);
            out = g_status;
        }
        out.framesLeft = g_ask.armed ? g_ask.framesLeft : 0;
        return out;
    }

    void onSwap() {
        if (!g_ask.armed)
            return;
        // Nothing to read while the window has no size (minimized): the shot,
        // and its count of frames, wait.
        int width = 0;
        int height = 0;
        if (!windowSize(width, height))
            return;
        if (--g_ask.framesLeft > 0)
            return;
        g_ask.armed = false;
        uint64_t const id = g_ask.id;
        auto const at = g_ask.at;

        std::filesystem::path path;
        {
            std::lock_guard lock(g_statusMutex);
            path = g_status.path;
        }
        // This is inside the swap: nothing may throw out of it.
        std::vector<uint8_t> pixels;
        try {
            pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        } catch (std::exception const&) {
            finish(id, fmt::format("no memory for a {}x{} image", width, height), at);
            return;
        }

        // The back buffer, as it's about to be shown. Everything changed to
        // read it is put back: the renderer leaves its own framebuffer bound
        // for reading during a render (texture.cpp's displayPreview), and its
        // pixel buffers on the pack binding while it reads back.
        GLint readFramebuffer = 0, packBuffer = 0, packAlignment = 4, rowLength = 0, skipRows = 0,
              skipPixels = 0, readBuffer = GL_BACK;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        // An error someone else left behind would be taken for this read's.
        for (int i = 0; i < 16; ++i)
            if (glGetError() == GL_NO_ERROR)
                break;

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &readBuffer);  // the window's own, which glReadBuffer changes
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadBuffer(GL_BACK);

        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        GLenum const error = glGetError();

        glReadBuffer(static_cast<GLenum>(readBuffer));
        glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));

        if (error != GL_NO_ERROR) {
            finish(id, fmt::format("reading the window failed (GL error 0x{:X})", error), at);
            return;
        }
        {
            std::lock_guard lock(g_statusMutex);
            if (g_statusId == id) {
                g_status.width = width;
                g_status.height = height;
            }
        }

        // The PNG off the main thread: nothing GL or cocos in there.
        try {
            std::thread([id, width, height, at, path = std::move(path), pixels = std::move(pixels)] {
                std::string why;
                try {
                    auto const png = encodePng(pixels, width, height);
                    std::lock_guard lock(g_writeMutex);
                    if (auto written = geode::utils::file::writeBinarySafe(path, png); !written)
                        why = "couldn't write the file: " + written.unwrapErr();
                } catch (std::exception const& e) {
                    why = std::string("couldn't make the PNG: ") + e.what();
                } catch (...) {
                    why = "couldn't make the PNG";
                }
                finish(id, std::move(why), at);
            }).detach();
        } catch (std::exception const& e) {
            finish(id, std::string("couldn't start writing it: ") + e.what(), at);
        }
    }

} // namespace gucci::mcp::shot

#endif

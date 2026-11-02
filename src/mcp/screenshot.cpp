// gucci_screenshot's capture and PNG writer (mcp/screenshot.hpp).

#include "mcp/screenshot.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/cocos/platform/win32/CCGL.h>
#include <imgui.h>

#include <zlib.h>

#include "core/GucciBot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace gucci::mcp::shot {

    namespace {

        using Clock = std::chrono::steady_clock;

        // The ask waiting for its trigger or its next shot. Main thread only:
        // the tool asks and the swap hook takes it.
        struct Ask {
            bool armed = false;      // shots still to take
            bool triggered = false;  // its trigger has fired (Now: at once)
            Burst burst;
            int framesLeft = 0;
            int taken = 0;
            uint64_t id = 0;         // counts asks, so a late result can't land on a newer one
            Clock::time_point at;
            Clock::time_point firedAt;
            // What the trigger compares the game with.
            bool lastDead = false;
            bool lastComplete = false;
            bool lastLevel = false;
            int64_t lastFrame = 0;
            matjson::Value entries = matjson::Value::array();  // the manifest's
        };
        Ask g_ask;

        // The last ask's outcome, which the writer thread finishes.
        std::mutex g_statusMutex;
        Status g_status;
        uint64_t g_statusId = 0;
        int g_expected = 0;  // files the last ask writes (its shots, and its manifest)
        Clock::time_point g_statusAt;

        // One writer thread with a queue, for every shot: PNG encoding and the
        // file, nothing GL or cocos.
        struct Job {
            uint64_t id = 0;
            std::filesystem::path path;
            std::vector<uint8_t> pixels;  // RGBA, bottom row first
            int width = 0;
            int height = 0;
            int scale = 1;
            std::string text;  // a manifest, written as it is
        };
        std::mutex g_jobMutex;
        std::condition_variable g_jobCv;
        std::deque<Job> g_jobs;
        size_t g_queuedBytes = 0;
        bool g_writerStarted = false;
        constexpr size_t kMaxQueuedBytes = size_t(256) << 20;

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

        // GL's RGBA rows (bottom row first) as an RGB PNG, each side divided
        // by `scale` (each pixel the average of its scale x scale block).
        // Alpha is dropped: the window's own alpha is whatever was last drawn
        // into it.
        std::vector<uint8_t> encodePng(std::vector<uint8_t> const& rgba, int width, int height, int scale) {
            scale = std::max(1, scale);
            size_t const w = static_cast<size_t>(std::max(1, width / scale));
            size_t const h = static_cast<size_t>(std::max(1, height / scale));
            size_t const srcW = static_cast<size_t>(width);
            size_t const srcH = static_cast<size_t>(height);
            size_t const s = static_cast<size_t>(scale);
            // Top row first, each led by filter type 0 (none).
            size_t const stride = 1 + w * 3;
            std::vector<uint8_t> raw(stride * h);
            for (size_t y = 0; y < h; ++y) {
                uint8_t* dst = raw.data() + y * stride;
                *dst++ = 0;
                for (size_t x = 0; x < w; ++x, dst += 3) {
                    unsigned sum[3] = {0, 0, 0};
                    unsigned n = 0;
                    for (size_t dy = 0; dy < s; ++dy) {
                        size_t const sy = y * s + dy;
                        if (sy >= srcH)
                            break;
                        uint8_t const* row = rgba.data() + (srcH - 1 - sy) * srcW * 4;
                        for (size_t dx = 0; dx < s; ++dx) {
                            size_t const sx = x * s + dx;
                            if (sx >= srcW)
                                break;
                            sum[0] += row[sx * 4];
                            sum[1] += row[sx * 4 + 1];
                            sum[2] += row[sx * 4 + 2];
                            n++;
                        }
                    }
                    n = std::max(1u, n);
                    dst[0] = uint8_t(sum[0] / n);
                    dst[1] = uint8_t(sum[1] / n);
                    dst[2] = uint8_t(sum[2] / n);
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
                ihdr[i] = uint8_t(uint32_t(w) >> (24 - 8 * i));
                ihdr[4 + i] = uint8_t(uint32_t(h) >> (24 - 8 * i));
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

        // One of the ask's files is done (or failed: `error`). The ask is
        // done when every file it writes is.
        void noteWritten(uint64_t id, std::string error) {
            std::lock_guard lock(g_statusMutex);
            if (g_statusId != id)
                return;
            g_status.written++;
            if (!error.empty() && g_status.error.empty())
                g_status.error = std::move(error);
            if (g_status.written >= g_expected) {
                g_status.pending = false;
                g_status.done = true;
                g_status.ok = g_status.error.empty();
                g_status.ms = std::chrono::duration<double, std::milli>(Clock::now() - g_statusAt).count();
            }
        }

        void writerLoop() {
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(g_jobMutex);
                    g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
                    job = std::move(g_jobs.front());
                    g_jobs.pop_front();
                }
                std::string why;
                try {
                    if (!job.text.empty()) {
                        if (auto written = geode::utils::file::writeString(job.path, job.text); !written)
                            why = "couldn't write the manifest: " + written.unwrapErr();
                    } else {
                        auto const png = encodePng(job.pixels, job.width, job.height, job.scale);
                        if (auto written = geode::utils::file::writeBinarySafe(job.path, png); !written)
                            why = "couldn't write the file: " + written.unwrapErr();
                    }
                } catch (std::exception const& e) {
                    why = std::string("couldn't make the PNG: ") + e.what();
                } catch (...) {
                    why = "couldn't make the PNG";
                }
                size_t const bytes = job.pixels.size() + job.text.size();
                {
                    std::lock_guard lock(g_jobMutex);
                    g_queuedBytes -= std::min(g_queuedBytes, bytes);
                }
                noteWritten(job.id, std::move(why));
            }
        }

        // To the writer, unless too much is waiting for it already.
        void queue(Job job) {
            size_t const bytes = job.pixels.size() + job.text.size();
            uint64_t const id = job.id;
            std::string refused;
            {
                std::lock_guard lock(g_jobMutex);
                if (g_queuedBytes + bytes > kMaxQueuedBytes) {
                    refused = "too many shots waiting to be written (256 MB): fewer, cropped or scaled down";
                } else {
                    try {
                        if (!g_writerStarted) {
                            std::thread(writerLoop).detach();
                            g_writerStarted = true;
                        }
                        g_queuedBytes += bytes;
                        g_jobs.push_back(std::move(job));
                    } catch (std::exception const& e) {
                        refused = std::string("couldn't start writing it: ") + e.what();
                    }
                }
            }
            if (!refused.empty())
                noteWritten(id, std::move(refused));
            else
                g_jobCv.notify_one();
        }

        std::filesystem::path fileFor(Burst const& b, int index) {
            if (b.count <= 1)
                return b.dir / (b.name + ".png");
            return b.dir / fmt::format("{}_{:03}.png", b.name, index);
        }

        // Whether the ask's trigger fires on this swap. The game's state is
        // read here, in the swap, on the main thread.
        bool fired() {
            auto* pl = PlayLayer::get();
            auto const& b = g_ask.burst;
            bool const dead = pl && pl->m_player1 && pl->m_player1->m_isDead;
            bool const complete = pl && pl->m_hasCompletedLevel;
            int64_t const frame = static_cast<int64_t>(GucciEngine::get()->updater.getFrame());
            bool fire = false;
            switch (b.trigger) {
                case Trigger::Now: fire = true; break;
                case Trigger::Death: fire = dead && !g_ask.lastDead; break;
                // Back alive, or the frame went back (a restart without a death).
                case Trigger::Respawn:
                    fire = (pl && !dead && g_ask.lastDead) || (pl && g_ask.lastLevel && frame < g_ask.lastFrame);
                    break;
                case Trigger::Complete: fire = complete && !g_ask.lastComplete; break;
                case Trigger::Frame: fire = pl && frame >= b.frame; break;
            }
            g_ask.lastDead = dead;
            g_ask.lastComplete = complete;
            g_ask.lastLevel = pl != nullptr;
            g_ask.lastFrame = frame;
            return fire;
        }

        // Reads one shot (the back buffer, as it's about to be shown) and
        // hands it to the writer.
        void takeOne(int width, int height) {
            auto const& b = g_ask.burst;
            uint64_t const id = g_ask.id;
            int const index = g_ask.taken;
            // The part to read, in the window's pixels from its top left.
            float x0 = 0.f, y0 = 0.f, x1 = static_cast<float>(width), y1 = static_cast<float>(height);
            if (b.crop.kind == Crop::Kind::Rect) {
                // ImGui's pixels to the window's: the same on this PC normally.
                float sx = 1.f, sy = 1.f;
                if (ImGui::GetCurrentContext()) {
                    auto const display = ImGui::GetIO().DisplaySize;
                    if (display.x > 0.f && display.y > 0.f) {
                        sx = static_cast<float>(width) / display.x;
                        sy = static_cast<float>(height) / display.y;
                    }
                }
                x0 = b.crop.x * sx;
                y0 = b.crop.y * sy;
                x1 = (b.crop.x + b.crop.w) * sx;
                y1 = (b.crop.y + b.crop.h) * sy;
            } else if (b.crop.kind == Crop::Kind::Player) {
                // Player 1 where it's drawn now; without one, the whole window.
                auto* pl = PlayLayer::get();
                if (pl && pl->m_player1 && pl->m_player1->getParent()) {
                    auto const world = pl->m_player1->getParent()->convertToWorldSpace(pl->m_player1->getPosition());
                    auto const win = CCDirector::get()->getWinSize();
                    float const px = world.x / win.width * static_cast<float>(width);
                    float const py = (1.f - world.y / win.height) * static_cast<float>(height);
                    x0 = px - b.crop.half;
                    y0 = py - b.crop.half;
                    x1 = px + b.crop.half;
                    y1 = py + b.crop.half;
                }
            }
            int const left = std::clamp(static_cast<int>(std::floor(x0)), 0, width);
            int const top = std::clamp(static_cast<int>(std::floor(y0)), 0, height);
            int const right = std::clamp(static_cast<int>(std::ceil(x1)), 0, width);
            int const bottom = std::clamp(static_cast<int>(std::ceil(y1)), 0, height);
            int const w = right - left;
            int const h = bottom - top;
            if (w < 1 || h < 1) {
                noteWritten(id, "the crop is outside the window");
                return;
            }

            std::vector<uint8_t> pixels;
            try {
                pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            } catch (std::exception const&) {
                noteWritten(id, fmt::format("no memory for a {}x{} image", w, h));
                return;
            }

            // Everything changed to read it is put back: the renderer leaves its
            // own framebuffer bound for reading during a render (texture.cpp's
            // displayPreview), and its pixel buffers on the pack binding while
            // it reads back.
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

            // GL counts rows from the bottom.
            glReadPixels(left, height - bottom, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            GLenum const error = glGetError();

            glReadBuffer(static_cast<GLenum>(readBuffer));
            glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
            glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
            glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
            glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));

            if (error != GL_NO_ERROR) {
                noteWritten(id, fmt::format("reading the window failed (GL error 0x{:X})", error));
                return;
            }

            auto const path = fileFor(b, index);
            int const scale = std::max(1, b.scale);
            {
                std::lock_guard lock(g_statusMutex);
                if (g_statusId == id) {
                    g_status.taken++;
                    g_status.files.push_back(path);
                    g_status.width = std::max(1, w / scale);
                    g_status.height = std::max(1, h / scale);
                }
            }
            if (b.manifest) {
                auto e = matjson::Value::object();
                e["i"] = (int64_t)index;
                e["path"] = geode::utils::string::pathToString(path);
                e["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                if (ImGui::GetCurrentContext())
                    e["imgui_frame"] = (int64_t)ImGui::GetFrameCount();
                e["ms"] = std::round(std::chrono::duration<double, std::milli>(Clock::now() - g_ask.firedAt).count() * 10.0) /
                          10.0;
                if (auto* pl = PlayLayer::get()) {
                    e["tick"] = (int64_t)pl->m_gameState.m_currentProgress;
                    if (pl->m_player1) {
                        auto p = matjson::Value::object();
                        p["x"] = (double)pl->m_player1->getPositionX();
                        p["y"] = (double)pl->m_player1->getPositionY();
                        p["dead"] = pl->m_player1->m_isDead;
                        e["player"] = p;
                    }
                }
                e["rect"] = std::vector<int64_t>{left, top, w, h};
                g_ask.entries.push(e);
            }
            Job job;
            job.id = id;
            job.path = path;
            job.pixels = std::move(pixels);
            job.width = w;
            job.height = h;
            job.scale = scale;
            queue(std::move(job));
        }

        void queueManifest() {
            auto const& b = g_ask.burst;
            auto m = matjson::Value::object();
            m["name"] = b.name;
            m["count"] = (int64_t)b.count;
            m["every"] = (int64_t)b.every;
            static char const* const kTriggers[] = {"now", "death", "respawn", "complete", "frame"};
            m["trigger"] = kTriggers[static_cast<int>(b.trigger)];
            m["scale"] = (int64_t)b.scale;
            m["files"] = g_ask.entries;
            Job job;
            job.id = g_ask.id;
            job.path = b.dir / (b.name + ".json");
            job.text = m.dump();
            queue(std::move(job));
        }

    } // namespace

    bool windowPixels(int& width, int& height) {
        // The window's framebuffer in pixels, asked of the OS as the renderer
        // does (renderer.cpp's getWindowFramebufferSize): while a render runs,
        // cocos's view size is the render's, and DPI scaling changes it too.
        HDC const dc = wglGetCurrentDC();
        HWND const window = dc ? WindowFromDC(dc) : nullptr;
        RECT rect{};
        if (!window || !GetClientRect(window, &rect))
            return false;
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        return width > 0 && height > 0;
    }

    void request(Burst burst) {
        static uint64_t s_lastId = 0;  // main thread only
        burst.count = std::max(1, burst.count);
        burst.every = std::max(1, burst.every);
        burst.frames = std::max(1, burst.frames);
        g_ask = Ask{};
        g_ask.armed = true;
        g_ask.burst = burst;
        g_ask.id = ++s_lastId;
        g_ask.at = Clock::now();
        g_ask.firedAt = g_ask.at;
        g_ask.framesLeft = burst.frames;
        // The trigger fires on a change from what the game shows now.
        auto* pl = PlayLayer::get();
        g_ask.lastDead = pl && pl->m_player1 && pl->m_player1->m_isDead;
        g_ask.lastComplete = pl && pl->m_hasCompletedLevel;
        g_ask.lastLevel = pl != nullptr;
        g_ask.lastFrame = static_cast<int64_t>(GucciEngine::get()->updater.getFrame());
        std::lock_guard lock(g_statusMutex);
        g_statusId = g_ask.id;
        g_status = Status{};
        g_status.asked = true;
        g_status.pending = true;
        g_status.count = burst.count;
        g_status.framesLeft = burst.frames;
        g_status.path = fileFor(burst, 0);
        if (burst.manifest)
            g_status.manifest = burst.dir / (burst.name + ".json");
        g_expected = burst.count + (burst.manifest ? 1 : 0);
        g_statusAt = g_ask.at;
    }

    void request(std::filesystem::path path, int frames, Crop crop) {
        Burst b;
        b.dir = path.parent_path();
        b.name = geode::utils::string::pathToString(path.stem());
        b.frames = frames;
        b.crop = crop;
        request(std::move(b));
    }

    Status status() {
        Status out;
        {
            std::lock_guard lock(g_statusMutex);
            out = g_status;
        }
        out.armed = g_ask.armed && !g_ask.triggered && g_ask.burst.trigger != Trigger::Now;
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
        if (!windowPixels(width, height))
            return;
        // This is inside the swap: nothing may throw out of it.
        try {
            if (!g_ask.triggered) {
                if (!fired())
                    return;
                g_ask.triggered = true;
                g_ask.firedAt = Clock::now();
                if (g_ask.burst.trigger != Trigger::Now)
                    g_ask.framesLeft = g_ask.burst.frames;
            }
            if (--g_ask.framesLeft > 0)
                return;
            takeOne(width, height);
            g_ask.taken++;
            if (g_ask.taken >= g_ask.burst.count) {
                g_ask.armed = false;
                if (g_ask.burst.manifest)
                    queueManifest();
            } else {
                g_ask.framesLeft = g_ask.burst.every;
            }
        } catch (...) {
            g_ask.armed = false;
            noteWritten(g_ask.id, "the shot failed");
        }
    }

} // namespace gucci::mcp::shot

#endif

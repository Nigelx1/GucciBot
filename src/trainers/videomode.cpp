// Video Mode's engine: the decode thread, the texture and the overlay. See
// videomode.hpp for the shape of it.

#include "render/gl_compat.hpp"
#include "trainers/videomode.hpp"

#include "core/GucciBot.hpp"
#include "render/renderer.hpp"
#include "render/video_decoder.hpp"

#include <Geode/Geode.hpp>
#include <Geode/cocos/platform/win32/CCGL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

using namespace geode::prelude;

namespace gucci::videomode {

    namespace fs = std::filesystem;

    namespace {

        // The keys older builds saved these under, kept so a saved offset
        // carries over.
        constexpr const char* kKeyOffset = "jupiter_video_offset_sec";
        constexpr const char* kKeyOpacity = "jupiter_video_opacity";

        constexpr const char* kVideoFile = "jmf_showcase.mp4";

        bool g_loaded = false;

        // ---- guccibot_videomode.log: what opened, what failed, when it closed.
        // Started over once a launch so it never grows without end.

        std::mutex g_logMutex;
        bool g_logStarted = false;

        void logLine(std::string const& text) {
            std::lock_guard lock(g_logMutex);
            auto const mode = g_logStarted ? std::ios::app : std::ios::trunc;
            g_logStarted = true;
            std::ofstream f(Mod::get()->getSaveDir() / "guccibot_videomode.log", std::ios::out | mode);
            if (f)
                f << text << '\n';
        }

        // ---- the decode thread
        //
        // It owns the decoder outright: it opens it, serves each request the
        // main thread posts (only the newest; requests it never got to are
        // dropped), and closes it when told to stop. A finished frame is
        // swapped into `shared` under the lock; the main thread swaps it out
        // again, so the two buffers trade places instead of being copied.

        struct Worker {
            std::thread thread;
            std::mutex m;
            std::condition_variable cv;
            bool stop = false;
            bool hasRequest = false;
            double request = 0.0;

            Status phase = Status::Opening;
            std::string failure;
            int width = 0, height = 0;
            double duration = 0.0;
            double frameSec = 1.0 / 30.0;

            bool fresh = false;
            std::vector<uint8_t> shared;
            double sharedPts = -1.0;
        };

        void run(Worker* w, fs::path path) {
            VideoDecoder decoder;
            std::string why;
            std::error_code ec;
            if (!SLRenderer::get()->isFFmpegLoaded())
                why = "FFmpeg didn't load (the same libraries rendering uses), so nothing can decode the video.";
            else if (!fs::exists(path, ec))
                why = fmt::format("{} isn't in the mod's resources folder.", kVideoFile);
            else if (!decoder.open(path))
                why = "FFmpeg couldn't open the video. The Geode log has its reason.";

            {
                std::lock_guard lock(w->m);
                if (why.empty()) {
                    w->phase = Status::Ready;
                    w->width = decoder.width();
                    w->height = decoder.height();
                    w->duration = decoder.durationSec();
                    w->frameSec = decoder.frameDurationSec();
                } else {
                    w->phase = Status::Failed;
                    w->failure = why;
                }
            }
            if (!why.empty()) {
                logLine("open failed: " + why + " (" + path.string() + ")");
                return;
            }
            logLine(fmt::format("opened {} at {}x{}, {:.2f}s, {:.3f}s a frame", path.string(), decoder.width(),
                                decoder.height(), decoder.durationSec(), decoder.frameDurationSec()));

            std::vector<uint8_t> local;
            for (;;) {
                double want = 0.0;
                {
                    std::unique_lock lock(w->m);
                    w->cv.wait(lock, [w] { return w->stop || w->hasRequest; });
                    if (w->stop)
                        break;
                    want = w->request;
                    w->hasRequest = false;
                }
                if (!decoder.getFrameAt(want, local))
                    continue;
                double const pts = decoder.framePtsSec();
                std::lock_guard lock(w->m);
                w->shared.swap(local);
                w->sharedPts = pts;
                w->fresh = true;
            }
            decoder.close();
        }

        // Heap-held and never destroyed at exit: a std::thread still joinable
        // when static destructors run would end the process with an abort.
        Worker* g_worker = nullptr;

        // ---- main-thread copies

        Status g_status = Status::Off;
        std::string g_failure;
        int g_width = 0, g_height = 0;
        double g_duration = 0.0;
        double g_frameSec = 1.0 / 30.0;

        std::vector<uint8_t> g_pixels;
        double g_shownPts = -1.0;
        double g_asked = std::numeric_limits<double>::quiet_NaN();

        GLuint g_tex = 0;
        int g_texW = 0, g_texH = 0;

        // GL state the upload touches, put back afterwards. cocos keeps its
        // own record of the bound texture and skips a bind it thinks is
        // already done, so leaving a different texture bound would draw the
        // next sprite with the video. A pixel-unpack buffer left bound by
        // anyone would make the upload read from it instead of our pixels.
        struct SavedGl {
            GLint activeTexture = GL_TEXTURE0;
            GLint texture = 0;
            GLint unpackBuffer = 0;
            GLint alignment = 4;
            GLint rowLength = 0;

            SavedGl() {
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
                glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
                if (unpackBuffer)
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            ~SavedGl() {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
                glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
                if (unpackBuffer)
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
                glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
                glActiveTexture((GLenum)activeTexture);
            }
        };

        void freeTexture() {
            if (g_tex) {
                // cocos's helper, so its record of the bound texture forgets
                // the name too (GL hands deleted names out again).
                ccGLDeleteTexture(g_tex);
                g_tex = 0;
            }
            g_texW = g_texH = 0;
        }

        // Created at the video's size the first time, then updated in place.
        void upload(std::vector<uint8_t> const& px, int w, int h) {
            if (w <= 0 || h <= 0 || px.size() < (size_t)w * (size_t)h * 4)
                return;
            SavedGl saved;
            if (!g_tex || w != g_texW || h != g_texH) {
                freeTexture();
                glGenTextures(1, &g_tex);
                glBindTexture(GL_TEXTURE_2D, g_tex);
                // Smooth when scaled to the screen, and no wrapped-round
                // pixels bleeding in at the edges.
                GLenum const params[][2] = {{GL_TEXTURE_MIN_FILTER, GL_LINEAR},
                                            {GL_TEXTURE_MAG_FILTER, GL_LINEAR},
                                            {GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE},
                                            {GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE}};
                for (auto const& p : params)
                    glTexParameteri(GL_TEXTURE_2D, p[0], (GLint)p[1]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                g_texW = w;
                g_texH = h;
            } else {
                glBindTexture(GL_TEXTURE_2D, g_tex);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            }
        }

        void start() {
            g_worker = new Worker();
            g_status = Status::Opening;
            g_failure.clear();
            g_shownPts = -1.0;
            g_asked = std::numeric_limits<double>::quiet_NaN();
            fs::path path = Mod::get()->getResourcesDir() / kVideoFile;
            g_worker->thread = std::thread(run, g_worker, std::move(path));
        }

        // Everything the mode holds goes: the thread, the decoder (closed by
        // the thread on its way out), both frame buffers and the texture.
        void shutdown() {
            if (g_worker) {
                {
                    std::lock_guard lock(g_worker->m);
                    g_worker->stop = true;
                }
                g_worker->cv.notify_all();
                if (g_worker->thread.joinable())
                    g_worker->thread.join();
                bool const wasOpen = g_worker->phase == Status::Ready;
                delete g_worker;
                g_worker = nullptr;
                if (wasOpen)
                    logLine("closed");
            }
            std::vector<uint8_t>().swap(g_pixels);
            freeTexture();
            g_status = Status::Off;
            g_failure.clear();
            g_width = g_height = 0;
            g_duration = 0.0;
            g_shownPts = -1.0;
            g_asked = std::numeric_limits<double>::quiet_NaN();
        }

        // Takes the thread's news: whether the video opened, and the newest
        // finished frame (uploaded here, on the thread GL belongs to).
        void poll() {
            if (!g_worker)
                return;
            bool got = false;
            {
                std::lock_guard lock(g_worker->m);
                g_status = g_worker->phase;
                g_failure = g_worker->failure;
                g_width = g_worker->width;
                g_height = g_worker->height;
                g_duration = g_worker->duration;
                g_frameSec = g_worker->frameSec > 0.0 ? g_worker->frameSec : 1.0 / 30.0;
                if (g_worker->fresh) {
                    g_pixels.swap(g_worker->shared);
                    g_shownPts = g_worker->sharedPts;
                    g_worker->fresh = false;
                    got = true;
                }
            }
            if (got)
                upload(g_pixels, g_width, g_height);
        }

        void post(double sec) {
            if (!g_worker || sec == g_asked)
                return;
            g_asked = sec;
            {
                std::lock_guard lock(g_worker->m);
                g_worker->request = sec;
                g_worker->hasRequest = true;
            }
            g_worker->cv.notify_one();
        }

        // Kept a frame short of the end: the last frame starts there, and a
        // time past it would only find the end of the file.
        double clampToVideo(double sec) {
            double const hi = std::max(0.0, g_duration - g_frameSec);
            return std::clamp(sec, 0.0, hi);
        }

    } // namespace

    // ------------------------------------------------------------ settings

    void ensureLoaded() {
        if (g_loaded)
            return;
        g_loaded = true;
        auto* gb = GucciEngine::get();
        auto* mod = Mod::get();
        gb->jupiterVideoOffsetSec =
            std::clamp((float)mod->getSavedValue<double>(kKeyOffset, gb->jupiterVideoOffsetSec), -600.f, 600.f);
        gb->jupiterVideoOpacity =
            std::clamp((float)mod->getSavedValue<double>(kKeyOpacity, gb->jupiterVideoOpacity), 0.05f, 1.f);
    }

    void saveSettings() {
        auto* gb = GucciEngine::get();
        Mod::get()->setSavedValue<double>(kKeyOffset, gb->jupiterVideoOffsetSec);
        Mod::get()->setSavedValue<double>(kKeyOpacity, gb->jupiterVideoOpacity);
    }

    // ------------------------------------------------------------ status

    Status status() {
        return g_status;
    }

    std::string failure() {
        return g_failure;
    }

    int width() {
        return g_width;
    }

    int height() {
        return g_height;
    }

    double durationSec() {
        return g_duration;
    }

    double frameSec() {
        return g_frameSec;
    }

    bool shownFrame(double& ptsSec) {
        if (!g_tex || g_shownPts < 0.0)
            return false;
        ptsSec = g_shownPts;
        return true;
    }

    double targetSec() {
        auto* gb = GucciEngine::get();
        double const sec = gb->jupiterVideoAlignToolActive
                               ? (double)gb->jupiterVideoAlignScrubSec
                               : gb->jupiterClickBarPosSec + (double)gb->jupiterVideoOffsetSec;
        return clampToVideo(sec);
    }

    bool firstClickSec(double& sec) {
        auto const& iv = GucciEngine::get()->jupiterMacro.clickIntervalsSec;
        if (iv.empty())
            return false;
        sec = iv.front().first;
        for (auto const& p : iv)
            sec = std::min(sec, p.first);
        return true;
    }

    // ------------------------------------------------------------ the overlay

    void drawOverlay() {
        ensureLoaded();
        auto* gb = GucciEngine::get();
        if (!gb->jupiterVideoModeEnabled) {
            gb->jupiterVideoAlignToolActive = false;
            if (g_worker || g_tex)
                shutdown();
            return;
        }
        if (!g_worker)
            start();
        poll();
        if (g_status != Status::Ready)
            return;
        post(targetSec());
        if (!g_tex || g_texW <= 0 || g_texH <= 0)
            return;

        // Fitted to the screen with its shape kept; any bars left over show
        // the game.
        ImVec2 const screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0.f || screen.y <= 0.f)
            return;
        float const scale = std::min(screen.x / (float)g_texW, screen.y / (float)g_texH);
        float const w = (float)g_texW * scale;
        float const h = (float)g_texH * scale;
        ImVec2 const a((screen.x - w) * 0.5f, (screen.y - h) * 0.5f);
        ImVec2 const b(a.x + w, a.y + h);
        int const alpha = std::clamp((int)std::lround(gb->jupiterVideoOpacity * 255.f), 0, 255);
        // The background list sits under every ImGui window, so the menu and
        // the click bar stay readable over the video.
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)g_tex, a, b, ImVec2(0.f, 0.f),
                                                 ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, alpha));
    }

    // ------------------------------------------------------------ the Alignment Tool

    void setAlignTool(bool on) {
        auto* gb = GucciEngine::get();
        if (on && !gb->jupiterVideoAlignToolActive) {
            double shown = 0.0;
            // Start on the frame on screen; half a frame before its
            // timestamp, so the decoder's "first frame at or after" lands on
            // that same frame.
            double const from = shownFrame(shown) ? shown - g_frameSec * 0.5
                                                  : gb->jupiterClickBarPosSec + gb->jupiterVideoOffsetSec;
            gb->jupiterVideoAlignScrubSec = (float)clampToVideo(from);
        }
        gb->jupiterVideoAlignToolActive = on;
    }

    void stepFrames(int frames) {
        auto* gb = GucciEngine::get();
        double shown = 0.0;
        // Frame n from the one on screen starts at shown + n frames; aiming
        // half a frame before that start makes it the first frame at or after
        // the target, whatever rounding the timestamps carry.
        double const to = shownFrame(shown) ? shown + (frames - 0.5) * g_frameSec
                                            : gb->jupiterVideoAlignScrubSec + frames * g_frameSec;
        gb->jupiterVideoAlignScrubSec = (float)clampToVideo(to);
    }

    bool setOffsetFromShownFrame() {
        auto* gb = GucciEngine::get();
        double shown = 0.0, first = 0.0;
        if (!shownFrame(shown) || !firstClickSec(first))
            return false;
        // The frame shows while the video time is in the frame before its
        // timestamp. Half a frame back puts the first press in the middle of
        // that stretch, so the frame picked here is the one on screen at the
        // press, not its neighbour, however the clock's time rounds.
        gb->jupiterVideoOffsetSec = (float)(shown - first - g_frameSec * 0.5);
        gb->jupiterVideoAlignToolActive = false;
        saveSettings();
        logLine(fmt::format("offset set to {:.4f}s (frame at {:.4f}s, first press at {:.4f}s)",
                            gb->jupiterVideoOffsetSec, shown, first));
        return true;
    }

} // namespace gucci::videomode

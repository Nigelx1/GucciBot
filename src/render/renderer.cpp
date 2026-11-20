#include "renderer.hpp"
#include "core/geode_compat.hpp" // gucci::stdPath
#include "core/GucciBot.hpp"
#include "analysis/ac/sound.hpp"
#include "analysis/ac/shim.hpp"  // SLSettings (the intro reads the frame-window switch)
#include "hacks/hitborders.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/ShaderLayer.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "colorspace/nv12.hpp"
#include "colorspace/rgb0.hpp"
#include "colorspace/rgb24.hpp"
#include "colorspace/yuv420p.hpp"
#include "colorspace/yuv444p.hpp"

using namespace geode::prelude;

namespace gucci {

    namespace fs = std::filesystem;

    constexpr int ALIGNMENT = 1;

    static void replaceString(std::string& s, const std::string& from, const std::string& to) {
        if (from.empty())
            return;
        size_t start_pos = 0;
        while ((start_pos = s.find(from, start_pos)) != std::string::npos) {
            s.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }

    struct RenderOpt {
        std::string m_name;
        std::string m_value;
    };

    static std::vector<RenderOpt> parseArgs(std::stringstream& in) {
        std::vector<RenderOpt> opts;
        while (!in.eof()) {
            std::string name, value;
            in >> name >> value;
            if (!name.starts_with("-"))
                return opts;
            name.erase(0, 1);
            if (value.starts_with("\"") && value.ends_with("\"")) {
                value.erase(0, 1);
                value.erase(value.size() - 1, value.size());
            }
            opts.push_back({name, value});
        }
        return opts;
    }

    // The ffmpeg command line's filter options (video and audio). They belong
    // to its filter graph, not to an encoder, and this renderer has no graph.
    static bool isFilterOption(std::string const& name) {
        return name == "vf" || name == "af" || name == "filter" || name == "filter_complex" ||
               name == "lavfi" || name.starts_with("filter:");
    }

    // For the log: the formats this renderer writes (av_get_pix_fmt_name
    // isn't in the loaded FFmpeg table).
    static const char* pixFmtName(AVPixelFormat f) {
        switch (f) {
        case AV_PIX_FMT_YUV420P:
            return "yuv420p";
        case AV_PIX_FMT_YUV444P:
            return "yuv444p";
        case AV_PIX_FMT_NV12:
            return "nv12";
        case AV_PIX_FMT_RGB0:
            return "rgb0";
        case AV_PIX_FMT_RGB24:
            return "rgb24";
        default:
            return "other";
        }
    }

    enum class GPUVendor { NVIDIA, AMD, INTEL, OTHER };

    static GPUVendor getGPUVendor() {
#ifdef _WIN32
        HKEY hKey;
        if (RegOpenKeyExA(
                HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, &hKey) !=
            ERROR_SUCCESS) {
            return GPUVendor::OTHER;
        }
        char subKeyName[256];
        DWORD idx = 0;
        while (idx < 8192) {
            DWORD size = sizeof(subKeyName);
            LONG result =
                RegEnumKeyExA(hKey, idx++, subKeyName, &size, nullptr, nullptr, nullptr, nullptr);
            if (result != ERROR_SUCCESS)
                break;
            std::string keyName = subKeyName;
            if (keyName.find("VEN_10DE") != std::string::npos) {
                RegCloseKey(hKey);
                return GPUVendor::NVIDIA;
            }
            // Radeon graphics only. VEN_1022 is AMD's CPU and chipset ID, and
            // it sorts before NVIDIA's VEN_10DE, so every Ryzen PC with a
            // GeForce card was handed AMF, which can't open there.
            if (keyName.find("VEN_1002") != std::string::npos) {
                RegCloseKey(hKey);
                return GPUVendor::AMD;
            }
            if (keyName.find("VEN_8086") != std::string::npos) {
                RegCloseKey(hKey);
                return GPUVendor::INTEL;
            }
        }
        RegCloseKey(hKey);
        return GPUVendor::OTHER;
#else
        return GPUVendor::OTHER;
#endif
    }

    static std::string getDefaultCodec() {
#if defined(GEODE_IS_MACOS)
        // macOS: Apple's own hardware encoder, VideoToolbox, on every Mac
        // (Apple silicon and Intel alike); no graphics-card lookup. A Mac
        // whose VideoToolbox won't open it (an 8K H.264, an old Intel Mac)
        // falls back to libx264 in start(), as a GPU encoder does on Windows.
        return "h264_videotoolbox";
#endif
        switch (getGPUVendor()) {
        case GPUVendor::NVIDIA:
            return "h264_nvenc";
        case GPUVendor::AMD:
            return "h264_amf";
        case GPUVendor::INTEL:
            return "h264_qsv";
        default:
            return "libx264";
        }
    }

    static void silentChangeSize(CCSize size) {
        auto director = CCDirector::sharedDirector();
        auto view = CCEGLView::sharedOpenGLView();
        view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
        director->updateScreenScale(size);
        director->setViewport();
        director->setProjection(kCCDirectorProjection2D);
        glViewport(0, 0, size.width, size.height);
    }

    static CCSize alignedRenderSize(SLRendererSettings const& settings) {
        return CCSize((settings.m_width + ALIGNMENT - 1) & ~(ALIGNMENT - 1),
                      (settings.m_height + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    }

    // The window's framebuffer in pixels, which is not the same as the view
    // size once DPI scaling is involved. Asked of the OS rather than cocos,
    // because cocos is the thing being lied to while a render owns the view.
    static std::optional<CCSize> getWindowFramebufferSize() {
#ifdef _WIN32
        HDC deviceContext = wglGetCurrentDC();
        if (!deviceContext)
            return std::nullopt;

        HWND window = WindowFromDC(deviceContext);
        RECT clientRect{};
        if (!window || !GetClientRect(window, &clientRect))
            return std::nullopt;

        int const width = clientRect.right - clientRect.left;
        int const height = clientRect.bottom - clientRect.top;
        if (width <= 0 || height <= 0)
            return std::nullopt;

        return CCSize(static_cast<float>(width), static_cast<float>(height));
#elif defined(GEODE_IS_MACOS)
        // macOS has no WGL window to ask. cocos's own window viewport is the
        // framebuffer in pixels (setViewport scales the frame size by the
        // view's zoom factor, which is where a Retina screen's 2x would be),
        // so it's set, read back, and the viewport put back as it was. Called
        // before a render resizes the view, so the frame size is the window's.
        GLint saved[4] = {};
        glGetIntegerv(GL_VIEWPORT, saved);
        CCDirector::get()->setViewport();
        GLint vp[4] = {};
        glGetIntegerv(GL_VIEWPORT, vp);
        glViewport(saved[0], saved[1], saved[2], saved[3]);

        // A letterboxed or cropped viewport sits centered in the framebuffer.
        int const width = vp[2] + 2 * vp[0];
        int const height = vp[3] + 2 * vp[1];
        if (width <= 0 || height <= 0)
            return std::nullopt;

        return CCSize(static_cast<float>(width), static_cast<float>(height));
#else
        return std::nullopt;
#endif
    }

    // Silicate's resizeShaderLayer (render/renderer.cpp), ported 2026-09-27.
    // GD's shader layer renders into a texture of exactly the render size,
    // with no aspect "extra". GucciBot's version, from before the Silicate
    // port, sized the texture to the WINDOW's aspect and offset it -- and then
    // read the height offset from the width (`m_heightOffset =
    // ...Extra.width`), while the other branch would have produced a negative
    // offset for a uint32. For a render at the window's aspect (the usual
    // case) both versions did the same thing; they differed only when the
    // render's aspect ratio isn't the window's.
    static void resizeShaderLayer(CCSize size) {
        auto* gjbgl = GJBaseGameLayer::get();
        if (!gjbgl || !gjbgl->m_shaderLayer) {
            geode::log::warn("[GucciBot] resizeShaderLayer: ShaderLayer missing, skipping");
            return;
        }
        ShaderLayer* sh = gjbgl->m_shaderLayer;
        auto winSize = CCDirector::get()->getWinSize();

        sh->m_screenSize = size;
        sh->m_scaleFactor = size.height / winSize.height;
        sh->m_aspectRatio = size.width / size.height;

        float csf = CCDirector::get()->getContentScaleFactor();
        CCDirector::get()->setContentScaleFactor(1.0f);
        sh->m_renderTexture->release();
        sh->m_renderTexture =
            CCRenderTexture::create(size.width, size.height, kCCTexture2DPixelFormat_RGBA8888);
        sh->m_renderTexture->retain();
        CCDirector::get()->setContentScaleFactor(csf);
        sh->m_sprite->setTexture(sh->m_renderTexture->getSprite()->getTexture());
        sh->m_textureContentSize = sh->m_sprite->getTexture()->getContentSize();
        sh->m_targetTextureSize = size;
        sh->m_targetTextureSizeExtra = CCSize(0.0f, 0.0f);
        sh->m_sprite->setTextureRect({-1.0f, -1.0f, size.width + 2.0f, size.height + 2.0f});
        sh->m_state.m_textureScaleX = size.width / winSize.width;
        sh->m_state.m_textureScaleY = size.height / winSize.height;
        geode::log::info(
            "[GucciBot] resizeShaderLayer -> {}x{}", (int)size.width, (int)size.height);
    }

    void SLRenderer::acquireView() {
        CCSize const renderSize = alignedRenderSize(m_settings);

        if (m_viewResized) {
            silentChangeSize(renderSize);
            auto* gjbgl = GJBaseGameLayer::get();
            if (m_recording && gjbgl && gjbgl->m_shaderLayer) {
                auto* sh = gjbgl->m_shaderLayer;
                if (!sh->m_screenSize.equals(renderSize) ||
                    !sh->m_targetTextureSize.equals(renderSize))
                    resizeShaderLayer(renderSize);
            }
            return;
        }

        auto* view = CCDirector::get()->getOpenGLView();
        m_windowSize = view->getFrameSize();
        m_windowFramebufferSize = getWindowFramebufferSize().value_or(m_windowSize);
        m_viewToFramebufferScale =
            CCSize(m_windowSize.width / m_windowFramebufferSize.width,
                   m_windowSize.height / m_windowFramebufferSize.height);
        m_texture.m_windowWidth = (uint32_t)m_windowFramebufferSize.width;
        m_texture.m_windowHeight = (uint32_t)m_windowFramebufferSize.height;
#if defined(GEODE_IS_MACOS)
        // Untested on a Mac: whether cocos's viewport there is the frame size
        // (points) or twice it (Retina pixels). If it's twice, a viewport set
        // inside the captured scene (GD's ShaderLayer) would zoom the video,
        // so the first Mac render logs both to settle it.
        geode::log::info("[RENDERER] mac view: frame size {}x{}, framebuffer {}x{}, render size {}x{}",
                         (int)m_windowSize.width, (int)m_windowSize.height, (int)m_windowFramebufferSize.width,
                         (int)m_windowFramebufferSize.height, (int)renderSize.width, (int)renderSize.height);
#endif
        m_viewResized = true;
    }

    void SLRenderer::restoreView() {
        if (!m_viewResized)
            return;
        m_viewResized = false;
        silentChangeSize(m_windowSize);
#if defined(GEODE_IS_MACOS)
        // silentChangeSize ends on a viewport of the frame size itself. On a
        // Mac whose view scales that (Retina), the window's viewport is
        // cocos's own, so it's put back from cocos.
        CCDirector::get()->setViewport();
#endif
        resizeShaderLayer(m_windowSize);
    }

    void SLRenderer::withOriginalView(std::function<void()> const& callback) {
        if (!m_viewResized) {
            callback();
            return;
        }
        silentChangeSize(m_windowSize);
#if defined(GEODE_IS_MACOS)
        CCDirector::get()->setViewport();  // as in restoreView
#endif
        callback();
        silentChangeSize(alignedRenderSize(m_settings));
    }

    void SLRenderer::updateWindowFramebufferSize(CCSize size) {
        if (size.width <= 0.0f || size.height <= 0.0f ||
            size.equals(m_windowFramebufferSize))
            return;

        m_windowFramebufferSize = size;
        m_windowSize = CCSize(size.width * m_viewToFramebufferScale.width,
                              size.height * m_viewToFramebufferScale.height);
        m_texture.m_windowWidth = (uint32_t)size.width;
        m_texture.m_windowHeight = (uint32_t)size.height;

        geode::log::info(
            "[GucciBot] window framebuffer changed mid-render: {}x{} (will restore to {}x{})",
            (int)m_windowFramebufferSize.width, (int)m_windowFramebufferSize.height,
            (int)m_windowSize.width, (int)m_windowSize.height);
    }

    // The three below answer "did a render swallow this resize event?". They
    // return true to tell the CCEGLView hook not to pass it on, because cocos
    // acting on it would resize the view out from under the render.
    bool SLRenderer::handleFrameSizeChange(float, float) {
        if (!m_viewResized)
            return false;
        if (auto fb = getWindowFramebufferSize())
            this->updateWindowFramebufferSize(*fb);
        return true;
    }

    bool SLRenderer::handleFramebufferSizeChange(int width, int height) {
        if (!m_viewResized)
            return false;
        this->updateWindowFramebufferSize(
            CCSize(static_cast<float>(width), static_cast<float>(height)));
        return true;
    }

    bool SLRenderer::handleWindowSizeChange() {
        if (!m_viewResized)
            return false;
        if (auto fb = getWindowFramebufferSize())
            this->updateWindowFramebufferSize(*fb);
        return true;
    }

    void SLRenderer::loadSettingsFromGeode() {
        auto* mod = Mod::get();
        m_settings.m_width = (int)mod->getSavedValue<int64_t>("render_width", 1920);
        m_settings.m_height = (int)mod->getSavedValue<int64_t>("render_height", 1080);
        m_settings.m_fps = (int)mod->getSavedValue<int64_t>("render_fps", 60);
        m_settings.m_codec = mod->getSavedValue<std::string>("render_codec", "");
        m_settings.m_codecAuto = m_settings.m_codec.empty();
        if (m_settings.m_codecAuto)
            m_settings.m_codec = getDefaultCodec();

        auto br = geode::utils::numFromString<int>(
                      mod->getSavedValue<std::string>("render_bitrate", "30"))
                      .unwrapOr(30);
        m_settings.m_bitrate = (uint32_t)br * 1'000'000;

        m_settings.m_renderArgs = mod->getSavedValue<std::string>("render_video_args", "");
        m_settings.m_afterEndTime =
            geode::utils::numFromString<float>(
                mod->getSavedValue<std::string>("render_seconds_after", "3"))
                .unwrapOr(3.f);
        m_settings.m_fadeInTime = mod->getSavedValue<double>("render_fade_in", 1.5);
        m_settings.m_fadeOutTime = mod->getSavedValue<double>("render_fade_out", 1.5);
        m_settings.m_audioCodec = mod->getSavedValue<std::string>("render_audio_codec", "aac");
        m_settings.m_musicVolume = mod->getSavedValue<double>("render_music_volume", 1.0);
        m_settings.m_sfxVolume = mod->getSavedValue<double>("render_sfx_volume", 1.0);
        m_settings.m_triggerSfxVolume =
            mod->getSavedValue<double>("render_trigger_sfx_volume", 1.0);

        m_collectAudio = mod->getSavedValue<bool>("render_include_audio", true);
        m_settings.m_splitAudioTracks =
            mod->getSavedValue<bool>("render_split_audio_tracks", false);
        m_settings.m_autoExitLevel = mod->getSavedValue<bool>("render_auto_exit", false);
        m_settings.m_previewAudio = mod->getSavedValue<bool>("render_preview_audio", false);
        m_settings.m_intro.load();

        std::string ext = mod->getSavedValue<std::string>("render_file_extension", ".mp4");
        if (!ext.empty() && ext[0] == '.')
            ext.erase(0, 1);
        m_settings.m_extension = ext;
    }

    geode::Result<> SLRenderer::start() {
        geode::log::info("[GucciBot] SLRenderer starting");
        this->finishStop();
        if (m_recording || m_recordThread.joinable())
            return geode::Err("A render is already in progress");
        if (!ff || !m_ffmpegLoaded)
            return geode::Err("FFmpeg not loaded");
        // No render is running (checked above): free what a start that failed
        // halfway left behind, before this one allocates its own.
        this->releaseEncoder();
        auto* pl = PlayLayer::get();
        if (!pl)
            return geode::Err("Not in a level");
        if (pl == m_quittingLayer)
            return geode::Err("The level is closing");

        loadSettingsFromGeode();

        FMODAudioEngine::get()->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);

        std::vector<RenderOpt> args;
        {
            std::stringstream s(m_settings.m_renderArgs);
            args = parseArgs(s);
        }

        auto* mod = Mod::get();

        std::string renderFolderStr = mod->getSavedValue<std::string>("render_output_folder", "");
        fs::path outDir;
        if (!renderFolderStr.empty()) {
            // Typed in the menu, so UTF-8 (a std::string path is the ANSI
            // code page on Windows).
            outDir = fs::path(std::u8string(renderFolderStr.begin(), renderFolderStr.end()));
        } else {
            outDir = gucci::stdPath(mod->getSaveDir()) / "renders";
        }
        std::error_code mkec;
        fs::create_directories(outDir, mkec);

        std::string customName = mod->getSavedValue<std::string>("render_name", "");
        std::string fileName;
        if (!customName.empty()) {
            fileName = customName + "." + m_settings.m_extension;
        } else if (m_autoVideoName && pl->m_level) {
            std::string formatted(m_videoNameTemplate);
            replaceString(formatted, "%name%", pl->m_level->m_levelName);
            replaceString(formatted, "%id%", std::to_string(pl->m_level->m_levelID.value()));
            replaceString(formatted, "%creator%", pl->m_level->m_creatorName);
            replaceString(formatted, "%rand%", std::to_string(rand()));
            fileName = formatted + "." + m_settings.m_extension;
        } else {
            fileName = m_settings.m_outputPath + "." + m_settings.m_extension;
        }

        fs::path out = outDir / fs::path(std::u8string(fileName.begin(), fileName.end()));
        auto outU8 = out.u8string();
        std::string outPath(outU8.begin(), outU8.end());
        geode::log::info("[GucciBot] SLRenderer output: {}", outPath);
        m_lastRender.path = outPath;

        ff->avformat_alloc_output_context2(&m_formatCtx, nullptr, nullptr, outPath.c_str());
        if (!m_formatCtx)
            return geode::Err("Failed to allocate output context");

        auto const makeColorspace = [](AVPixelFormat f) -> std::unique_ptr<Colorspace> {
            switch (f) {
            case AV_PIX_FMT_YUV420P:
                return std::make_unique<YUV420PColorspace>();
            case AV_PIX_FMT_YUV444P:
                return std::make_unique<YUV444PColorspace>();
            case AV_PIX_FMT_NV12:
                return std::make_unique<NV12Colorspace>();
            case AV_PIX_FMT_RGB0:
                return std::make_unique<RGB0Colorspace>();
            case AV_PIX_FMT_RGB24:
                return std::make_unique<RGB24Colorspace>();
            default:
                return nullptr;
            }
        };
        // The pixel format picked on the Render page comes first, when the
        // codec takes it; otherwise the first of these five the codec lists.
        std::string const wanted = Mod::get()->getSavedValue<std::string>("render_pix_fmt", "auto");
        AVPixelFormat const wantedFmt = wanted == "yuv420p" ? AV_PIX_FMT_YUV420P
                                        : wanted == "yuv444p" ? AV_PIX_FMT_YUV444P
                                        : wanted == "nv12"  ? AV_PIX_FMT_NV12
                                        : wanted == "rgb0"  ? AV_PIX_FMT_RGB0
                                        : wanted == "rgb24" ? AV_PIX_FMT_RGB24
                                                            : AV_PIX_FMT_NONE;
        std::unique_ptr<Colorspace> colorspace;

        // Finds the video encoder, picks its pixel format and opens it. One
        // lambda, so an encoder picked for the graphics card that won't open
        // (no driver for it, a card that has since been taken out) gets a
        // second try as libx264 instead of a render that never starts.
        auto const openVideoCodec = [&](std::string const& name) -> geode::Result<> {
            m_settings.m_codec = name;
            m_videoCodec = ff->avcodec_find_encoder_by_name(name.c_str());
            if (!m_videoCodec)
                return geode::Err("Failed to find codec '{}'", name);

            m_videoCodecCtx.reset(ff->avcodec_alloc_context3(m_videoCodec));
            if (!m_videoCodecCtx)
                return geode::Err("Failed to allocate codec context");

            AVPixelFormat* outPixFmts = nullptr;
            int outPixFmtCount = 0;
            m_settings.m_pixFmt = AV_PIX_FMT_NONE;
            colorspace.reset();
            if (ff->avcodec_get_supported_config(m_videoCodecCtx.get(),
                                                 m_videoCodec,
                                                 AV_CODEC_CONFIG_PIX_FORMAT,
                                                 0,
                                                 (const void**)&outPixFmts,
                                                 &outPixFmtCount) < 0) {
                return geode::Err("Failed to query codec pixel-format capabilities");
            }
            for (int i = 0; i < outPixFmtCount && wantedFmt != AV_PIX_FMT_NONE; i++) {
                if (outPixFmts[i] == wantedFmt) {
                    m_settings.m_pixFmt = wantedFmt;
                    colorspace = makeColorspace(wantedFmt);
                    break;
                }
            }
#if defined(GEODE_IS_MACOS)
            // VideoToolbox lists nv12 before yuv420p. nv12's color plane is
            // read back as two channels (GL_RG), which macOS's old OpenGL 2.1
            // may not take; yuv420p reads one channel per plane, as libx264's
            // renders do everywhere. So on a Mac, Auto takes yuv420p when the
            // codec has it.
            for (int i = 0; i < outPixFmtCount && m_settings.m_pixFmt == AV_PIX_FMT_NONE; i++) {
                if (outPixFmts[i] == AV_PIX_FMT_YUV420P) {
                    m_settings.m_pixFmt = AV_PIX_FMT_YUV420P;
                    colorspace = makeColorspace(AV_PIX_FMT_YUV420P);
                }
            }
#endif
            for (int i = 0; i < outPixFmtCount && m_settings.m_pixFmt == AV_PIX_FMT_NONE; i++) {
                if (auto cs = makeColorspace(outPixFmts[i])) {
                    m_settings.m_pixFmt = outPixFmts[i];
                    colorspace = std::move(cs);
                }
            }
            if (m_settings.m_pixFmt == AV_PIX_FMT_NONE)
                return geode::Err("Couldn't find a usable pixel format for codec '{}'", name);

            m_videoCodecCtx->codec_id = m_videoCodec->id;
            m_videoCodecCtx->pix_fmt = m_settings.m_pixFmt;
            m_videoCodecCtx->width = m_settings.m_width;
            m_videoCodecCtx->height = m_settings.m_height;
            m_videoCodecCtx->bit_rate = m_settings.m_bitrate;
            m_videoCodecCtx->time_base = {1, m_settings.m_fps};
            m_videoCodecCtx->framerate = {m_settings.m_fps, 1};
            m_videoCodecCtx->max_b_frames = 0;
            // An RGB stream (libx264rgb with rgb24, the old Showcase) is tagged as
            // RGB, full range. Tagged BT.709 limited-range YUV like the others,
            // libx264rgb wrote that matrix into the stream, and players turned
            // its RGB planes into colors as if they were YUV: a teal background
            // and a green ground (test pass 2026-11-10, part 2 bug 2). The
            // primaries and transfer stay BT.709, as for YUV.
            // What decides it is the stream, not the input: a GPU encoder
            // (NVENC, AMF, QSV, Media Foundation) that takes rgb0 converts it
            // to YUV itself, so its stream keeps the YUV tags. Only a software
            // encoder that takes RGB input writes RGB.
            bool const rgbInput =
                m_settings.m_pixFmt == AV_PIX_FMT_RGB0 || m_settings.m_pixFmt == AV_PIX_FMT_RGB24;
            bool const gpuEncoder =
                (m_videoCodec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) != 0;
            bool const rgbStream = rgbInput && !gpuEncoder;
            m_videoCodecCtx->color_primaries = AVCOL_PRI_BT709;
            m_videoCodecCtx->colorspace = rgbStream ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
            m_videoCodecCtx->color_trc = AVCOL_TRC_BT709;
            m_videoCodecCtx->color_range = rgbStream ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
                m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            AVDictionary* video_opts = nullptr;
            for (const auto& arg : args) {
                // Video args are the encoder's own options (-preset, -crf,
                // -qp, -threads...): this renderer runs no FFmpeg filter
                // graph. A filter pair like TCBot's "-vf colorspace=..." used
                // to go in as a codec option no encoder has, and was dropped
                // without a word. It isn't needed: the colorspace shaders
                // already convert to BT.709 and the stream is tagged so.
                if (isFilterOption(arg.m_name)) {
                    geode::log::info("[GucciBot] Video args: skipped -{} {} (GucciBot runs no FFmpeg filters; "
                                     "it already writes BT.709 color itself)",
                                     arg.m_name, arg.m_value);
                    continue;
                }
                ff->av_dict_set(&video_opts, arg.m_name.c_str(), arg.m_value.c_str(), 0);
            }
            int const opened = ff->avcodec_open2(m_videoCodecCtx.get(), m_videoCodec, &video_opts);
            // avcodec_open2 leaves behind the options nothing took. Said once
            // in the log, so a typo (or an option of another encoder) doesn't
            // look like it was used.
            if (opened >= 0 && video_opts) {
                std::string unused;
                for (AVDictionaryEntry* e = nullptr;
                     (e = ff->av_dict_get(video_opts, "", e, AV_DICT_IGNORE_SUFFIX));) {
                    if (!unused.empty())
                        unused += ", ";
                    unused += fmt::format("-{} {}", e->key, e->value);
                }
                if (!unused.empty())
                    geode::log::warn("[GucciBot] Video args not used ('{}' has no such option): {}", name,
                                     unused);
            }
            ff->av_dict_free(&video_opts);
            if (opened < 0)
                return geode::Err("Failed to open codec '{}'", name);
            // What the render runs with, for anyone reading a log. Threads 0
            // is "auto" (the encoder picks; this FFmpeg's default, checked
            // 2026-11-15), unless the Video args gave -threads.
            geode::log::info("[GucciBot] Video encoder '{}' opened: {}x{} at {} FPS, pixel format {}, threads {}",
                             name, m_settings.m_width, m_settings.m_height, m_settings.m_fps,
                             pixFmtName(m_settings.m_pixFmt),
                             m_videoCodecCtx->thread_count == 0 ? std::string("auto")
                                                                : std::to_string(m_videoCodecCtx->thread_count));
            return geode::Ok();
        };
        if (auto r = openVideoCodec(m_settings.m_codec); r.isErr()) {
            // Only an encoder GucciBot picked falls back: one typed into
            // Codec is what the player asked for, so its error is shown.
            if (!m_settings.m_codecAuto || m_settings.m_codec == "libx264")
                return r;
            geode::log::warn("[GucciBot] SLRenderer: {} (picked for this PC's graphics card), trying libx264",
                             r.unwrapErr());
            if (AVCodecContext* failed = m_videoCodecCtx.release())
                ff->avcodec_free_context(&failed);
            if (auto r2 = openVideoCodec("libx264"); r2.isErr())
                return r2;
        }

        m_videoStream = ff->avformat_new_stream(m_formatCtx, m_videoCodec);
        if (!m_videoStream)
            return geode::Err("Failed to create video stream");
        if (ff->avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx.get()) < 0)
            return geode::Err("Failed to copy codec parameters");
        // The stream's frame rate, as FFmpeg's own command line sets it. Left
        // unset, the MKV muxer wrote no default frame duration, so players and
        // editors had to guess the rate from timestamps MKV rounds to the
        // millisecond (16 and 17 ms apart at 60 FPS): ffprobe read a 60 FPS
        // render as 120 FPS, averaging 62.5 (acceptance test 2026-11-15).
        // The time base is only a hint; each muxer picks its own.
        m_videoStream->time_base = m_videoCodecCtx->time_base;
        m_videoStream->avg_frame_rate = m_videoCodecCtx->framerate;
        m_videoStream->r_frame_rate = m_videoCodecCtx->framerate;

        m_audioTracks.clear();
        if (m_collectAudio) {
            int trackCount = m_settings.m_splitAudioTracks ? 4 : 1;
            static const char* kTrackNames[4] = {"combined", "music", "sfx", "frame-window"};
            for (int i = 0; i < trackCount; i++) {
                SLAudioTrack track;

                track.codec = ff->avcodec_find_encoder_by_name(m_settings.m_audioCodec.c_str());
                if (!track.codec)
                    return geode::Err("Failed to find audio codec '{}'", m_settings.m_audioCodec);

                // The codec's own sample format. Float planes were set for
                // every codec, and FLAC only takes 16- or 32-bit integers, so
                // it never opened: "Failed to open audio codec 'flac'" and a
                // Start that did nothing (Nigel, 2026-10-17). writeAudio's
                // resampler converts to whichever format opens. Float planes
                // first (AAC, Opus), then the integer kinds, then the rest.
                static constexpr AVSampleFormat kSampleFormats[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16,
                                                                    AV_SAMPLE_FMT_S32,  AV_SAMPLE_FMT_FLT,
                                                                    AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32P};
                bool opened = false;
                for (const AVSampleFormat sampleFormat : kSampleFormats) {
                    AVCodecContext* ctx = ff->avcodec_alloc_context3(track.codec);
                    if (!ctx)
                        return geode::Err("Failed to allocate audio codec context");
                    ctx->codec_tag = 0;
                    ctx->codec_type = AVMEDIA_TYPE_AUDIO;
                    ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                    ctx->bit_rate = 320000;
                    ctx->sample_fmt = sampleFormat;
                    ctx->sample_rate = m_sampleRate;
                    ctx->time_base = {1, m_sampleRate};
                    ctx->flags &= ~AV_CODEC_FLAG_QSCALE;
                    if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
                        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                    AVDictionary* audio_opts = nullptr;
                    ff->av_dict_set(&audio_opts, "compression_level", "0", 0);
                    opened = ff->avcodec_open2(ctx, track.codec, &audio_opts) >= 0;
                    ff->av_dict_free(&audio_opts);
                    if (opened) {
                        track.codecCtx.reset(ctx);
                        break;
                    }
                    ff->avcodec_free_context(&ctx);  // this format refused: the next one gets a fresh context
                }
                if (!opened)
                    return geode::Err("Failed to open audio codec '{}' (no sample format it takes)",
                                      m_settings.m_audioCodec);

                track.stream = ff->avformat_new_stream(m_formatCtx, track.codec);
                if (!track.stream)
                    return geode::Err("Failed to create audio stream");
                if (ff->avcodec_parameters_from_context(track.stream->codecpar,
                                                        track.codecCtx.get()) < 0)
                    return geode::Err("Failed to copy audio codec parameters");
                track.stream->time_base = track.codecCtx->time_base;
                if (m_settings.m_splitAudioTracks) {
                    ff->av_dict_set(&track.stream->metadata, "title", kTrackNames[i], 0);
                }

                geode::log::info("[GucciBot] Audio stream {} ({}): {} @ {}Hz, frame_size={}",
                                 i,
                                 m_settings.m_splitAudioTracks ? kTrackNames[i] : "combined",
                                 m_settings.m_audioCodec,
                                 m_sampleRate,
                                 track.codecCtx->frame_size);

                m_audioTracks.push_back(std::move(track));
            }
        }

        if (ff->avio_open(&m_formatCtx->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
            return geode::Err("Failed to open output file");
        if (ff->avformat_write_header(m_formatCtx, nullptr) < 0)
            return geode::Err("Failed to write header");

        m_frame.reset(ff->av_frame_alloc());
        if (!m_frame)
            return geode::Err("Failed to allocate frame");
        m_frame->format = m_settings.m_pixFmt;
        m_frame->width = m_settings.m_width;
        m_frame->height = m_settings.m_height;

        m_pkt.reset(ff->av_packet_alloc());
        if (!m_pkt)
            return geode::Err("Failed to allocate packet");

        for (auto& track : m_audioTracks) {
            track.frame.reset(ff->av_frame_alloc());
            if (!track.frame)
                return geode::Err("Failed to allocate audio frame");
            // A codec that takes any frame size (frame_size 0) gets the
            // mixer's block size.
            track.frameSamples =
                track.codecCtx->frame_size > 0 ? track.codecCtx->frame_size : kAudioBlockSamples;
            track.pending.clear();
            track.nextPts = -1;
            track.frame->nb_samples = track.frameSamples;
            track.frame->sample_rate = track.codecCtx->sample_rate;
            track.frame->format = track.codecCtx->sample_fmt;
            track.frame->ch_layout = track.codecCtx->ch_layout;
            if (ff->av_frame_get_buffer(track.frame.get(), 0) < 0)
                return geode::Err("Failed to get audio frame buffer");

            track.pkt.reset(ff->av_packet_alloc());
            if (!track.pkt)
                return geode::Err("Failed to allocate packet (audio)");
        }

        m_time = 0.0;
        m_endTime = 0.0f;
        m_completionCaptured = false;
        m_macroTailTime = 0.0;
        m_macroTailDone = false;
        m_updateIndex = 0;
        // The encode thread's count, read by the Render card's HUD and by the
        // ring's room check (tryBeginFrame). It has to start each render at
        // 0: the ring's issued count does (SLRenderTexture::init), and a count
        // left over from the last render made the check always pass, so a
        // slow encoder could have its slot reclaimed before it read it.
        m_encoded.store(0, std::memory_order_release);
        m_lostFrames = 0;
        // Nothing from the last render: its frames' timestamps (a stop that
        // couldn't collect every readback left some, see flushPending).
        m_ptsQueue.clear();
        {
            std::lock_guard<std::mutex> lock(m_recordMutex);
            m_frameQueue.clear();
        }
        m_introStarted = false;
        m_introFinished = false;
        m_introIndex = 0;
        m_introTotal = 0;
        m_levelStartDecided = false;
        m_levelStartFrames = 0;
        RenderIntro::get()->destroy();
        m_layer = pl;
        m_layerGen = m_levelGen;
        m_recording = true;

        m_texture.m_width = m_settings.m_width;
        m_texture.m_height = m_settings.m_height;
        m_alignedWidth = (m_settings.m_width + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        m_alignedHeight = (m_settings.m_height + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        m_texture.m_alignedWidth = m_alignedWidth;
        m_texture.m_alignedHeight = m_alignedHeight;
        m_texture.m_widthOffset = 0;
        m_texture.m_heightOffset = 0;

        this->acquireView();
        auto frameSize = m_windowSize;
        silentChangeSize(CCSize(m_alignedWidth, m_alignedHeight));
        // No aspect "extra" any more (see resizeShaderLayer), so the preview
        // blit's source offsets stay at the 0 set above -- as in Silicate.
        resizeShaderLayer(CCSize(m_alignedWidth, m_alignedHeight));
        silentChangeSize(frameSize);

        m_texture.init(std::move(colorspace));
        m_bufferSize = m_texture.m_colorspace->getBufferSize();
        geode::log::info("[GucciBot] SLRenderer capture ready — buffer {}", m_bufferSize);

        if (m_collectAudio) {
            // The volume the player normally hears music at, read before the
            // render takes the engine over -- the preview plays at it.
            float const listenVolume = FMODAudioEngine::get()->getBackgroundMusicVolume();
            FrameWindowSound::channelGroup();
            AudioEngineRenderState::enter(m_settings.m_musicVolume, m_settings.m_sfxVolume);
            FrameWindowSound::setRenderMode(true);

            AudioRecorder::get()->init();
            AudioRecorder::get()->attach();
            if (m_settings.m_previewAudio) {
                AudioRecorder::get()->startMonitor();
                AudioRecorder::get()->m_monVolume.store(listenVolume, std::memory_order_relaxed);
            }
            if (m_settings.m_splitAudioTracks) {
                auto* engine = FMODAudioEngine::get();
                AudioRecorder::getMusic()->init(engine->m_backgroundMusicChannel);
                AudioRecorder::getMusic()->attach();
                AudioRecorder::getSfx()->init(engine->m_globalChannel);
                AudioRecorder::getSfx()->attach();
                AudioRecorder::getFrameWindow()->init(FrameWindowSound::channelGroup());
                AudioRecorder::getFrameWindow()->attach();
            }
        }

        m_readyToKill.store(false, std::memory_order_relaxed);
        m_recordThread = std::thread(&SLRenderer::recordLoop, this);
        return geode::Ok();
    }

    geode::Result<> SLRenderer::encode(uint8_t* data, size_t size) {
        ff->av_frame_unref(m_frame.get());
        m_frame->format = m_videoCodecCtx->pix_fmt;
        m_frame->width = m_videoCodecCtx->width;
        m_frame->height = m_videoCodecCtx->height;

        int ret = ff->av_frame_get_buffer(m_frame.get(), 1);
        if (ret < 0)
            return geode::Err("Failed to allocate frame buffer: {}", ret);
        ret = ff->av_frame_make_writable(m_frame.get());
        if (ret < 0)
            return geode::Err("Failed to make frame writable");

        ff->av_image_fill_linesizes(m_frame->linesize, m_settings.m_pixFmt, m_alignedWidth);

        auto result = m_texture.m_colorspace->prepareFrame(m_frame.get(), data, size);
        if (result.isErr())
            return result;

        // The frame's own time, taken when it was captured (m_bufferPts, from
        // the queue). m_updateIndex is the game's, which has moved on by the
        // time this thread gets to the frame: with the encoder behind, frames
        // queued together all came out with the same time, the muxer refused
        // the repeat, and the render stopped ("Failed to write frame", Nigel
        // 2026-10-21, about two seconds into every render).
        m_frame->pts = m_bufferPts;
        ret = ff->avcodec_send_frame(m_videoCodecCtx.get(), m_frame.get());
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                return geode::Err("Codec requires more input data");
            if (ret == AVERROR_EOF)
                return geode::Err("End of file reached");
            return geode::Err("Failed to send frame: {}", ret);
        }
        return geode::Ok();
    }

    geode::Result<> SLRenderer::write() {
        int ret = 0;
        while (ret >= 0) {
            ret = ff->avcodec_receive_packet(m_videoCodecCtx.get(), m_pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                return geode::Err("Failed to receive packet");
            if (m_pkt->duration <= 0)
                m_pkt->duration = 1;  // one frame, in the codec's 1/fps time base
            ff->av_packet_rescale_ts(
                m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_pkt->stream_index = m_videoStream->index;
            {
                std::lock_guard<std::mutex> lock(m_muxMutex);
                ret = ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
            }
            if (ret < 0)
                return geode::Err("Failed to write frame (FFmpeg error {}, pts {}, dts {})", ret, m_pkt->pts,
                                  m_pkt->dts);
            ff->av_packet_unref(m_pkt.get());
        }
        return geode::Ok();
    }

    // The mixer hands over 1024 samples at a time; an encoder takes its own
    // frame size (FLAC 1152, Opus 960, MP3 1152, AAC 1024). Blocks are queued
    // and cut into whole encoder frames, timed by the samples written. Each
    // block used to go out as one encoder frame of frame_size samples read
    // from it: with FLAC every 1024 samples became 1152 (128 read from the
    // next block, or past the end of the buffer) at a pts 1024 apart, so the
    // music came out 12.5% long and garbled, 11 s behind the picture by the
    // end of Stereo Madness (acceptance test 2026-11-15).
    geode::Result<> SLRenderer::writeAudio(std::vector<float>& data, uint64_t pts, int trackIndex) {
        if (trackIndex < 0 || trackIndex >= (int)m_audioTracks.size())
            return geode::Err("Invalid audio track index {}", trackIndex);
        auto& track = m_audioTracks[trackIndex];
        if (m_channels <= 0 || track.frameSamples <= 0)
            return geode::Err("Audio track {} isn't set up", trackIndex);

        if (track.nextPts < 0)
            track.nextPts = (int64_t)pts;

        size_t const block = std::min(data.size(), (size_t)kAudioBlockSamples * (size_t)m_channels);
        track.pending.insert(track.pending.end(), data.begin(), data.begin() + block);

        size_t const perFrame = (size_t)track.frameSamples * (size_t)m_channels;
        size_t used = 0;
        while (track.pending.size() - used >= perFrame) {
            auto ret = this->encodeAudioFrame(track, track.pending.data() + used, track.frameSamples);
            used += perFrame;
            if (ret.isErr()) {
                track.pending.erase(track.pending.begin(), track.pending.begin() + used);
                return ret;
            }
        }
        track.pending.erase(track.pending.begin(), track.pending.begin() + used);
        return geode::Ok();
    }

    geode::Result<> SLRenderer::encodeAudioFrame(SLAudioTrack& track, float const* samples, int count) {
        int ret = ff->av_frame_make_writable(track.frame.get());
        if (ret < 0)
            return geode::Err("Failed to make the audio frame writable: {}", ret);

        track.frame->pts = track.nextPts;
        track.frame->nb_samples = count;
        track.frame->ch_layout = track.codecCtx->ch_layout;

        AVChannelLayout inputChLayout;
        ff->av_channel_layout_default(&inputChLayout, m_channels);

        if (!track.swrCtx) {
            ret = ff->swr_alloc_set_opts2(&track.swrCtx,
                                          &track.codecCtx->ch_layout,
                                          track.codecCtx->sample_fmt,
                                          track.codecCtx->sample_rate,
                                          &inputChLayout,
                                          AV_SAMPLE_FMT_FLT,
                                          m_sampleRate,
                                          0,
                                          nullptr);
            if (ret < 0)
                return geode::Err("Failed to set swr options: {}", ret);
            if (!track.swrCtx)
                return geode::Err("Failed to allocate swr context");
            if (ff->swr_init(track.swrCtx) < 0)
                return geode::Err("Failed to initialize swr context");
        }

        const uint8_t* inData[1] = {reinterpret_cast<const uint8_t*>(samples)};
        ff->swr_convert(track.swrCtx, track.frame->data, count, inData, count);
        track.nextPts += count;

        ret = ff->avcodec_send_frame(track.codecCtx.get(), track.frame.get());
        if (ret < 0)
            return geode::Err("Failed to send audio frame: {}", ret);

        while (ret >= 0) {
            ret = ff->avcodec_receive_packet(track.codecCtx.get(), track.pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                return geode::Err("Failed to receive audio packet");

            ff->av_packet_rescale_ts(
                track.pkt.get(), track.codecCtx->time_base, track.stream->time_base);
            track.pkt->stream_index = track.stream->index;
            {
                std::lock_guard<std::mutex> lock(m_muxMutex);
                ret = ff->av_interleaved_write_frame(m_formatCtx, track.pkt.get());
            }
            if (ret < 0)
                return geode::Err("Failed to write audio frame");
            ff->av_packet_unref(track.pkt.get());
        }

        return geode::Ok();
    }

    // The end of a render: the samples short of a whole encoder frame go out
    // as a shorter last frame where the codec takes one (FLAC does), padded
    // with silence to a whole frame otherwise.
    void SLRenderer::flushPendingAudio(SLAudioTrack& track) {
        if (track.pending.empty() || m_channels <= 0 || track.frameSamples <= 0 || !track.codec)
            return;
        int const have = (int)(track.pending.size() / (size_t)m_channels);
        int count = have;
        bool const shortOk = (track.codec->capabilities &
                              (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) != 0;
        if (!shortOk)
            count = track.frameSamples;
        track.pending.resize((size_t)count * (size_t)m_channels, 0.f);
        if (count > 0 && count <= track.frameSamples) {
            auto ret = this->encodeAudioFrame(track, track.pending.data(), count);
            if (ret.isErr())
                geode::log::warn("[GucciBot] The audio's last {} samples weren't written: {}", have,
                                 ret.unwrapErr());
        }
        track.pending.clear();
    }

    geode::Result<> SLRenderer::stop() {
        // Runs on the game thread (finishStop), after the encode thread has
        // exited. No flushPending() here: the drain happened just before
        // signalStop(), and the encode thread has written everything it was
        // handed -- anything still in the ring was never going to be encoded.
        m_recording = false;
        m_recordCv.notify_all();

        // Cancelled mid-intro: take the card down. Game thread, so this is
        // safe (finishStop).
        RenderIntro::get()->destroy();
        // The hitborders' framebuffer at the render's size (133 MB at 8K)
        // isn't kept until the level closes; the live game makes its own.
        HitborderOverlay::get()->releaseGpu();

        AudioRecorder::get()->stopMonitor();
        AudioRecorder::get()->detach();
        AudioRecorder::get()->uninit();
        if (m_settings.m_splitAudioTracks) {
            AudioRecorder::getMusic()->detach();
            AudioRecorder::getMusic()->uninit();
            AudioRecorder::getSfx()->detach();
            AudioRecorder::getSfx()->uninit();
            AudioRecorder::getFrameWindow()->detach();
            AudioRecorder::getFrameWindow()->uninit();
        }
        FrameWindowSound::setRenderMode(false);
        AudioEngineRenderState::exit();

        if (m_pkt && m_videoCodecCtx && m_formatCtx && m_videoStream) {
            ff->avcodec_send_frame(m_videoCodecCtx.get(), nullptr);
            while (ff->avcodec_receive_packet(m_videoCodecCtx.get(), m_pkt.get()) == 0) {
                if (m_pkt->duration <= 0)
                    m_pkt->duration = 1;
                ff->av_packet_rescale_ts(
                    m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
                m_pkt->stream_index = m_videoStream->index;
                {
                    std::lock_guard<std::mutex> lock(m_muxMutex);
                    ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
                }
                ff->av_packet_unref(m_pkt.get());
            }
        }

        for (auto& track : m_audioTracks) {
            if (!(track.pkt && track.codecCtx && m_formatCtx && track.stream))
                continue;
            if (track.frame)
                this->flushPendingAudio(track);
            ff->avcodec_send_frame(track.codecCtx.get(), nullptr);
            while (ff->avcodec_receive_packet(track.codecCtx.get(), track.pkt.get()) == 0) {
                ff->av_packet_rescale_ts(
                    track.pkt.get(), track.codecCtx->time_base, track.stream->time_base);
                track.pkt->stream_index = track.stream->index;
                {
                    std::lock_guard<std::mutex> lock(m_muxMutex);
                    ff->av_interleaved_write_frame(m_formatCtx, track.pkt.get());
                }
                ff->av_packet_unref(track.pkt.get());
            }
        }

        if (m_formatCtx) {
            {
                std::lock_guard<std::mutex> lock(m_muxMutex);
                ff->av_write_trailer(m_formatCtx);
            }
            if (m_formatCtx->pb) {
                ff->avio_close(m_formatCtx->pb);
                m_formatCtx->pb = nullptr;
            }
        }
        this->releaseEncoder();
        m_texture.destroy();
        this->restoreView();
        this->publishRenderResult(true);
        if (m_settings.m_autoExitLevel)
            m_shouldQuit = true;
        geode::log::info("[GucciBot] SLRenderer stopped");
        return geode::Ok();
    }

    // Every render used to keep its encoder until the game closed: about
    // 120-165 MB and libx264's 13 worker threads per render (soak test,
    // 2026-11-16), because the pointers' deleters do nothing (SL_AV_LEAK) and
    // nothing freed them. Silicate's stop() frees the codec contexts and the
    // format context after the trailer; this does the same, and the frames and
    // packets too. Only from the game thread with the encode thread joined
    // (finishStop) and the audio recorders detached (stop), so nothing else
    // can be using any of it; start() also calls it, for what a start that
    // failed halfway left behind.
    void SLRenderer::releaseEncoder() {
        if (!ff)
            return;
        // avcodec_free_context closes the encoder, which joins its threads.
        if (AVCodecContext* ctx = m_videoCodecCtx.release())
            ff->avcodec_free_context(&ctx);
        if (AVFrame* frame = m_frame.release()) {
            // The planes point into the capture's buffer (prepareFrame), not
            // memory this frame owns.
            for (auto& plane : frame->data)
                plane = nullptr;
            ff->av_frame_free(&frame);
        }
        if (AVPacket* pkt = m_pkt.release())
            ff->av_packet_free(&pkt);
        for (auto& track : m_audioTracks) {
            if (AVCodecContext* ctx = track.codecCtx.release())
                ff->avcodec_free_context(&ctx);
            if (AVFrame* frame = track.frame.release())
                ff->av_frame_free(&frame);
            if (AVPacket* pkt = track.pkt.release())
                ff->av_packet_free(&pkt);
            ff->swr_free(&track.swrCtx);
        }
        m_audioTracks.clear();
        if (m_formatCtx) {
            if (m_formatCtx->pb) {
                ff->avio_close(m_formatCtx->pb);
                m_formatCtx->pb = nullptr;
            }
            // The streams belong to the format context and go with it.
            ff->avformat_free_context(m_formatCtx);
            m_formatCtx = nullptr;
        }
        m_videoStream = nullptr;
    }

    void SLRenderer::publishRenderResult(bool success) {
        m_lastRender.success = success;
        m_lastRender.width = (unsigned)m_settings.m_width;
        m_lastRender.height = (unsigned)m_settings.m_height;
        m_lastRender.fps = (unsigned)m_settings.m_fps;
        // m_time counts the level's stepped frames; the video also has the
        // intro card in front of them, and the level's starting frame when
        // the render began at the level's start (levelStartDue).
        m_lastRender.duration = m_time + (m_introTotal + m_levelStartFrames) * (double)getDt();
        m_lastRender.fileSize = 0;
        std::error_code ec;
        // m_lastRender.path is UTF-8 (start() takes it from u8string()).
        fs::path const file(std::u8string(m_lastRender.path.begin(), m_lastRender.path.end()));
        if (auto const sz = fs::file_size(file, ec); !ec)
            m_lastRender.fileSize = sz;
        m_lastRender.pending = true;
    }

    void SLRenderer::recordLoop() {
        geode::log::info("[GucciBot] encode thread started");

        while (true) {
            QueuedFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_recordMutex);
                m_recordCv.wait(lock, [this] { return !m_frameQueue.empty() || !m_recording; });

                if (m_frameQueue.empty()) {
                    if (!m_recording)
                        break;  // nothing queued and the render is done
                    continue;
                }
                frame = m_frameQueue.front();
                m_frameQueue.pop_front();
            }

            m_bufferPts = frame.m_pts;

            if (auto e = this->encode(frame.m_data, m_bufferSize); e.isErr()) {
                geode::log::error("[GucciBot] Render stopping: encoding a frame failed: {}", e.unwrapErr());
                break;
            }
            if (auto w = this->write(); w.isErr()) {
                geode::log::error("[GucciBot] Render stopping: writing the video file failed: {}",
                                  w.unwrapErr());
                break;
            }

            m_encoded.fetch_add(1, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(m_recordMutex);
            m_recording = false;
        }

        // stop() is NOT called here -- see finishStop().
        m_readyToKill.store(true, std::memory_order_release);
        geode::log::info("[GucciBot] encode thread finished");
    }

    void SLRenderer::finishStop() {
        if (!m_readyToKill.exchange(false, std::memory_order_acquire))
            return;
        if (m_recordThread.joinable())
            m_recordThread.join();
        auto st = this->stop();
        if (st.isErr())
            geode::log::error("[GucciBot] stop failed: {}", st.unwrapErr());
    }

    // True when there is room in the ring to start another readback. Harvests
    // first, so a full ring usually frees a slot right here.
    bool SLRenderer::tryBeginFrame() {
        this->harvestCompleted();
        // A lost readback's slot is free again but never gets encoded: count
        // it, or each one would shrink the ring by a slot for the rest of the
        // render.
        return m_texture.issuedCount() - m_lostFrames - m_encoded.load(std::memory_order_acquire) <
               SLRenderTexture::RING_SIZE;
    }

    void SLRenderer::enqueueFrame(uint8_t* data, int64_t pts) {
        {
            std::lock_guard<std::mutex> lock(m_recordMutex);
            m_frameQueue.push_back(QueuedFrame{data, pts});
        }
        m_recordCv.notify_one();
    }

    // Non-blocking: take whatever the GPU has finished, in issue order, and
    // hand each to the encoder with the timestamp it was issued under.
    void SLRenderer::harvestCompleted() {
        uint8_t* data = nullptr;
        while (!m_ptsQueue.empty()) {
            auto const got = m_texture.tryHarvest(&data, false);
            if (got == SLRenderTexture::Harvest::Pending)
                break;
            this->takeHarvested(got, data);
        }
    }

    // One readback came back: hand it to the encoder with its own timestamp,
    // or, if it was lost, drop that timestamp so the next frame keeps its own.
    void SLRenderer::takeHarvested(SLRenderTexture::Harvest got, uint8_t* data) {
        int64_t const pts = m_ptsQueue.front();
        m_ptsQueue.pop_front();
        if (got == SLRenderTexture::Harvest::Ready) {
            this->enqueueFrame(data, pts);
            return;
        }
        // Logged once here and counted at the stop, not once a frame.
        if (m_lostFrames++ == 0)
            geode::log::warn("[GucciBot] Video frame {} is missing: the graphics card's copy of it "
                             "couldn't be read",
                             pts);
    }

    // End of render: wait for everything still in flight, so the tail of the
    // video is not silently dropped.
    void SLRenderer::flushPending() {
        uint8_t* data = nullptr;
        while (!m_ptsQueue.empty() && m_texture.mappedCount() < m_texture.issuedCount()) {
            auto const got = m_texture.tryHarvest(&data, true);
            if (got == SLRenderTexture::Harvest::Pending)
                break;
            this->takeHarvested(got, data);
        }
        // A readback the graphics card didn't finish within the wait (a
        // second each) is lost. Its timestamp goes with it: left in the
        // queue, the next render's first frames took the old render's
        // timestamps, the encoder got them out of order and that render
        // failed at once ("Failed to write frame", FFmpeg error -22).
        if (!m_ptsQueue.empty()) {
            geode::log::warn("[GucciBot] The video's last {} frames weren't written: the graphics card "
                             "didn't hand them over in time",
                             m_ptsQueue.size());
            m_ptsQueue.clear();
        }
        if (m_lostFrames > 0)
            geode::log::warn("[GucciBot] {} video frames were missing from this render (their copies "
                             "couldn't be read); the frames after each gap kept their own times",
                             m_lostFrames);
    }

    // No slot check here: drawScene has already waited for one (tryBeginFrame)
    // before letting the game advance to this frame. Checking here instead is
    // what the async port first did, and it meant a busy encoder DROPPED the
    // frame after the game had already moved on -- a gap in the video.
    void SLRenderer::capture() {
        m_ptsQueue.push_back(m_updateIndex - 1);
        m_texture.issue(m_fadeThreshold);
    }

    bool SLRenderer::introEnabled() const {
        auto const& intro = m_settings.m_intro;
        return intro.m_enabled && SLSettings::get()->frameWindow.enabled &&
               intro.duration() > 0.0;
    }

    // The game is held still for the intro, so nothing feeds the audio in that
    // time. Every active track gets the same length of silence up front and
    // its recorder's position moves past it, so the level's audio starts
    // exactly where the level's first video frame does.
    //
    // anticroom's writes one track. GucciBot can split the audio into four
    // (combined, music, SFX, frame-window cues), each with its own recorder
    // and its own position, so all four get the silence -- otherwise the split
    // tracks would start the intro's length ahead of the picture.
    void SLRenderer::writeIntroSilence(double seconds) {
        if (!m_collectAudio || m_audioTracks.empty())
            return;
        if (m_sampleRate <= 0 || m_channels <= 0)
            return;

        constexpr int frameSize = 1024;  // same block drainRecorderIntoTrack writes
        int const frames = (int)(seconds * m_sampleRate / frameSize);
        if (frames <= 0)
            return;

        AudioRecorder* const recorders[] = {
            AudioRecorder::get(),
            AudioRecorder::getMusic(),
            AudioRecorder::getSfx(),
            AudioRecorder::getFrameWindow(),
        };
        size_t const tracks =
            std::min(m_audioTracks.size(), (size_t)(m_settings.m_splitAudioTracks ? 4 : 1));

        std::vector<float> silence;
        for (size_t t = 0; t < tracks; t++) {
            AudioRecorder* rec = recorders[t];
            uint32_t const base = rec->m_index;
            for (int i = 0; i < frames; i++) {
                silence.assign((size_t)frameSize * m_channels, 0.f);
                auto ret = this->writeAudio(silence, (uint64_t)(base + i) * frameSize, (int)t);
                if (ret.isErr()) {
                    geode::log::error("[GucciBot] Failed to write intro silence (track {}): {}",
                                      t, ret.unwrapErr());
                    return;
                }
            }
            rec->m_index = base + frames;
        }

        // Only the combined recorder keeps a clock (the FMOD pump measures
        // how much to mix from it); both halves move together, so the amount
        // it asks for next is unchanged.
        double const written = (double)frames * frameSize / m_sampleRate;
        auto* main = AudioRecorder::get();
        main->m_time += written;
        main->m_fmodTime += written;
    }

    bool SLRenderer::tickIntro(PlayLayer* pl) {
        if (m_introFinished)
            return false;

        auto const& intro = m_settings.m_intro;

        if (!m_introStarted) {
            m_introTotal =
                this->introEnabled() ? (int)std::round(intro.duration() * m_settings.m_fps) : 0;
            if (m_introTotal <= 0) {
                m_introTotal = 0;
                m_introFinished = true;
                return false;
            }

            m_introStarted = true;
            m_introIndex = 0;

            RenderIntro::get()->build(pl, intro, (float)m_settings.m_height);
            this->writeIntroSilence(m_introTotal * (double)this->getDt());
        }

        double const t = m_introIndex * (double)this->getDt();
        double const fadeOutAt = intro.m_fadeInTime + intro.m_holdTime;

        float alpha = 1.f;
        if (t < intro.m_fadeInTime) {
            alpha = (float)(t / intro.m_fadeInTime);
        } else if (t >= fadeOutAt) {
            alpha = intro.m_fadeOutTime > 0.0
                        ? (float)(1.0 - (t - fadeOutAt) / intro.m_fadeOutTime)
                        : 0.f;
        }

        RenderIntro::get()->setAlpha(alpha);

        m_fadeThreshold = 1.f;
        m_updateIndex++;
        this->capture();

        if (++m_introIndex >= m_introTotal) {
            RenderIntro::get()->destroy();
            m_introFinished = true;
        }

        return true;
    }

    // Every level frame of a render is captured after the game's step for it
    // (drawScene: the scheduler update, then update() -> capture()), as in
    // Silicate. So the video's first level frame was the level one step in
    // (0.01%), and the level as it stands before anything moves was never in
    // it (Nigel, 2026-11-20: "my render starts at 0.01%, it should start at
    // 0.00%"). This captures that frame once, at level time 0, without
    // stepping the game; drawScene carries on as before from the next frame,
    // whose level time is dt as it always was.
    //
    // Only when the render begins at the level's start: GucciBot's frame
    // counter is still 0 (no tick has run since the level's start or a
    // restart from it). A render begun mid-level, or at a checkpoint, starts
    // where it starts, with no extra frame. drawScene draws anticroom's
    // frame-window HUD between the two calls: it is drawn on each tick, and
    // none has run yet at frame 0.
    bool SLRenderer::levelStartDue(PlayLayer* pl) {
        if (m_levelStartDecided || !this->isRecording())
            return false;
        m_levelStartDecided = true;

        uint32_t const frame = GucciEngine::get()->updater.getFrame();
        if (frame != 0 || pl->m_hasCompletedLevel || pl->m_playerDied) {
            geode::log::info("[GucciBot] Render: began mid-level (frame {}), so the video starts there", frame);
            return false;
        }
        return true;
    }

    void SLRenderer::captureLevelStart() {
        m_levelStartFrames = 1;
        m_time = 0.0;
        // The fade-in at level time 0 (update()'s formula with m_time 0 and
        // no fade-out yet).
        m_fadeThreshold = m_settings.m_fadeInTime == 0.0 ? 1.f : 0.f;
        this->writeLevelStartSilence();
        m_updateIndex++;
        this->capture();
        geode::log::info("[GucciBot] Render: the video starts on the level's first frame (frame 0)");
    }

    // The starting frame's dt of sound: silence, written straight to each
    // track (as the intro's is), with FMOD not pumped. The recorders' clocks
    // pace FMOD's mixing (audio_hook.cpp), and they never see this, so the
    // level's audio comes out sample for sample as it did without the
    // starting frame, just dt later -- as its picture is. Every sound keeps
    // the frame it had relative to the action. Pumping FMOD for this frame
    // instead would move a song that is already playing at frame 0 a frame
    // earlier against the action, while the clicks stayed where they were.
    // Silence fed through a recorder wouldn't do either: its clock only
    // counts whole blocks, so the part-block left over would push every
    // sound after it a frame later against the picture.
    void SLRenderer::writeLevelStartSilence() {
        if (!m_collectAudio || m_audioTracks.empty())
            return;
        if (m_sampleRate <= 0 || m_channels <= 0)
            return;

        int const samples = (int)std::round((double)this->getDt() * m_sampleRate);
        if (samples <= 0)
            return;

        AudioRecorder* const recorders[] = {
            AudioRecorder::get(),
            AudioRecorder::getMusic(),
            AudioRecorder::getSfx(),
            AudioRecorder::getFrameWindow(),
        };
        size_t const tracks =
            std::min(m_audioTracks.size(), (size_t)(m_settings.m_splitAudioTracks ? 4 : 1));

        std::vector<float> silence;
        for (size_t t = 0; t < tracks; t++) {
            // A track's first write sets where it starts; this is where the
            // recorder would have started it (the intro's silence, if any,
            // came first and set it already).
            uint64_t const pts = (uint64_t)recorders[t]->m_index * kAudioBlockSamples;
            // writeAudio takes at most a block at a time.
            for (int left = samples; left > 0;) {
                int const n = std::min(left, kAudioBlockSamples);
                silence.assign((size_t)n * m_channels, 0.f);
                auto ret = this->writeAudio(silence, pts, (int)t);
                if (ret.isErr()) {
                    geode::log::error("[GucciBot] Failed to write the starting frame's silence (track {}): {}",
                                      t, ret.unwrapErr());
                    return;
                }
                left -= n;
            }
        }
    }

    void SLRenderer::update(PlayLayer* pl) {
        if (!this->isRecording())
            return;

        if (pl->m_isPaused || !pl->m_started)
            return;

        // Intro frames used update indices too (they are the start of the
        // video), but the level's clock starts after them. The level's
        // starting frame, when there is one, is level time 0, so the first
        // frame after it is dt, as the first frame of a render without it.
        m_time = (++m_updateIndex - m_introTotal - m_levelStartFrames) * this->getDt();

        // When the level is done. A macro that is playing back may hold this
        // for a moment, as Silicate's renderer does: inputs it still has at or
        // after the level-complete frame (often the last release) play into
        // the video, click sounds included, before the time after the end
        // starts. Only Play mode can hold it, and for at most
        // kMacroTailWait. In Off nothing moves the macro's cursor, so a
        // loaded macro held the stop forever: the end screen was rendered
        // until Stop was pressed (test pass 2026-11-18, X1). A playing macro
        // longer than the level held it until its last input.
        bool levelEnded = pl->m_hasCompletedLevel;
        if (levelEnded && !m_macroTailDone) {
            auto* gb = GucciEngine::get();
            bool const inputsLeft = gb->isPlaying() && gb->replay.getCurrentQueuedInput().has_value();
            if (inputsLeft && m_macroTailTime < kMacroTailWait) {
                m_macroTailTime += this->getDt();
                levelEnded = false;
            } else {
                if (inputsLeft)
                    geode::log::info("[GucciBot] Render: the macro still has inputs {:.1f} s after the "
                                     "level end; the render doesn't wait for them",
                                     m_macroTailTime);
                m_macroTailDone = true;
            }
        }

        if (levelEnded) {
            if (this->m_settings.m_afterEndTime <= 0.f) {
                // 0 seconds after the end: the level-complete frame is the
                // video's last. It is captured below (m_endTime stays 0, so
                // no fade-out on it) and the next update stops. Stopping on
                // this update, as the general case below would, ended the
                // video one frame before the level was complete.
                if (m_completionCaptured) {
                    this->flushAndStop("the level ended (0 seconds after the end)");
                    return;
                }
                m_completionCaptured = true;
            } else {
                if (this->m_endTime >= this->m_settings.m_afterEndTime) {
                    this->flushAndStop(fmt::format("the level ended and the {} {} after it are in",
                                                   this->m_settings.m_afterEndTime,
                                                   this->m_settings.m_afterEndTime == 1.f ? "second" : "seconds"));
                    return;
                }
                this->m_endTime += this->getDt();
            }
        }

        {
            double const fadeIn =
                m_settings.m_fadeInTime == 0.0
                    ? 1.0
                    : std::clamp(m_time / m_settings.m_fadeInTime, 0.0, 1.0);
            double const fadeOut =
                (m_settings.m_fadeOutTime == 0.0 || m_endTime <= 0.0f)
                    ? 1.0
                    : std::clamp((m_settings.m_afterEndTime - m_endTime) /
                                     m_settings.m_fadeOutTime,
                                 0.0, 1.0);
            m_fadeThreshold = static_cast<float>(std::min(fadeIn, fadeOut));
        }

        if (m_collectAudio)
            AudioEngineRenderState::pump(getDt(), m_settings.m_splitAudioTracks);

        this->capture();
    }

} // namespace gucci

// Closing the game mid-render: nothing finishes the video (the encode thread is
// left as it is, as before), so the log says so. One line, like every other
// way a render ends.
$on_game(Exiting) {
    if (gucci::SLRenderer::get()->isRecording())
        geode::log::warn("[GucciBot] Render stopping: the game is closing. The video was never finished, "
                         "so it may not play (an .mp4 won't)");
}

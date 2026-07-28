// SLRenderer (Silicate FFmpeg-library renderer) — port into GucciBot.
// Stage 2 ported Silicate's resizeShaderLayer (high-res shader effects stay
// crisp). Stage 3 (this build) wires the audio encode path: start() opens the
// audio codec + stream and attaches the FMOD DSP recorder; writeAudio() encodes
// captured PCM (lazy SwrContext FLT->FLTP) and muxes it; stop() flushes +
// detaches; update() pumps FMOD per frame. Bot->GucciEngine, settings from
// Geode saved values, class renamed SLRenderer/SLRenderTexture to coexist with
// the legacy TTR renderer until Stage 4 swaps it out.
#include "renderer.hpp"
#include "GucciBot.hpp"  // GucciEngine (for the end-of-macro stop check)

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/ShaderLayer.hpp>  // resizeShaderLayer (Stage 2)

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include "colorspace/nv12.hpp"
#include "colorspace/rgb0.hpp"
#include "colorspace/rgb24.hpp"
#include "colorspace/yuv420p.hpp"

using namespace geode::prelude;
namespace fs = std::filesystem;

constexpr int ALIGNMENT = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (ported from Silicate)
// ─────────────────────────────────────────────────────────────────────────────

static void replaceString(std::string& s, const std::string& from,
                          const std::string& to) {
    if (from.empty()) return;
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
        if (!name.starts_with("-")) return opts;
        name.erase(0, 1);
        if (value.starts_with("\"") && value.ends_with("\"")) {
            value.erase(0, 1);
            value.erase(value.size() - 1, value.size());
        }
        opts.push_back({name, value});
    }
    return opts;
}

enum class GPUVendor { NVIDIA, AMD, INTEL, OTHER };

static GPUVendor getGPUVendor() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ,
                      &hKey) != ERROR_SUCCESS) {
        return GPUVendor::OTHER;
    }
    char subKeyName[256];
    DWORD idx = 0;
    while (idx < 8192) {
        DWORD size = sizeof(subKeyName);
        LONG result = RegEnumKeyExA(hKey, idx++, subKeyName, &size, nullptr,
                                    nullptr, nullptr, nullptr);
        if (result != ERROR_SUCCESS) break;
        std::string keyName = subKeyName;
        if (keyName.find("VEN_10DE") != std::string::npos) { RegCloseKey(hKey); return GPUVendor::NVIDIA; }
        if (keyName.find("VEN_1002") != std::string::npos || keyName.find("VEN_1022") != std::string::npos) { RegCloseKey(hKey); return GPUVendor::AMD; }
        if (keyName.find("VEN_8086") != std::string::npos) { RegCloseKey(hKey); return GPUVendor::INTEL; }
    }
    RegCloseKey(hKey);
    return GPUVendor::OTHER;
}

static std::string getDefaultCodec() {
    switch (getGPUVendor()) {
        case GPUVendor::NVIDIA: return "h264_nvenc";
        case GPUVendor::AMD:    return "h264_amf";
        case GPUVendor::INTEL:  return "h264_qsv";
        default:                return "libx264";
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

// Ported from Silicate (renderer.cpp resizeShaderLayer): re-size the game's
// ShaderLayer render texture to the render resolution so the full-screen shader
// effects (glow, color shifts, pulses, blur) render crisp instead of being
// stretched up from the window resolution. The width/height offsets this
// computes feed only the live preview blit — the encoded frame is always the
// full aligned capture (capture()'s silentChangeSize ignores the offsets, same
// as upstream Silicate).
static void resizeShaderLayer(CCSize size, CCSize original) {
    ShaderLayer* sh = GJBaseGameLayer::get()->m_shaderLayer;
    if (!sh) {
        geode::log::warn("[GucciBot] resizeShaderLayer: ShaderLayer missing, skipping");
        return;
    }
    sh->m_screenSize = size;
    sh->m_scaleFactor = size.height / CCDirector::get()->getWinSize().height;
    sh->m_aspectRatio = size.width / size.height;

    auto winSize = CCDirector::get()->getWinSize();
    float baseAspectRatio = original.aspect();

    float csf = CCDirector::get()->getContentScaleFactor();
    CCDirector::get()->setContentScaleFactor(1.0f);
    sh->m_renderTexture->release();
    sh->m_renderTexture = nullptr;
    sh->m_renderTexture = CCRenderTexture::create(
        size.width / sh->m_aspectRatio * baseAspectRatio, size.height,
        kCCTexture2DPixelFormat_RGBA8888);
    sh->m_renderTexture->retain();
    CCDirector::get()->setContentScaleFactor(csf);
    sh->m_sprite->setTexture(sh->m_renderTexture->getSprite()->getTexture());
    sh->m_textureContentSize = sh->m_sprite->getTexture()->getContentSize();
    sh->m_targetTextureSize = size;
    sh->m_targetTextureSizeExtra = CCSize(0.0f, 0.0f);
    if (baseAspectRatio > sh->m_aspectRatio) {
        float calculatedWidth = size.width / sh->m_aspectRatio * baseAspectRatio;
        sh->m_targetTextureSizeExtra = cocos2d::CCSize{calculatedWidth - size.width, 0.0f};
    } else if (baseAspectRatio < sh->m_aspectRatio) {
        float calculatedHeight = size.height / sh->m_aspectRatio * baseAspectRatio;
        sh->m_targetTextureSizeExtra = cocos2d::CCSize{0.0f, calculatedHeight - size.height};
    }
    sh->m_sprite->setTextureRect({-1.0f + sh->m_targetTextureSizeExtra.width,
                                  -1.0f + sh->m_targetTextureSizeExtra.height,
                                  size.width + 2.0f, size.height + 2.0f});
    sh->m_state.m_textureScaleX = size.width / winSize.width;
    sh->m_state.m_textureScaleY = size.height / winSize.height;
    geode::log::info("[GucciBot] resizeShaderLayer -> {}x{}", (int)size.width, (int)size.height);
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings — pull from GucciBot's existing Geode saved values (same keys the
// legacy render GUI already uses, so Stage 4's TCBot UI just reuses them).
// ─────────────────────────────────────────────────────────────────────────────

void SLRenderer::loadSettingsFromGeode() {
    auto* mod = Mod::get();
    m_settings.m_width  = (int)mod->getSavedValue<int64_t>("render_width", 1920);
    m_settings.m_height = (int)mod->getSavedValue<int64_t>("render_height", 1080);
    m_settings.m_fps    = (int)mod->getSavedValue<int64_t>("render_fps", 60);
    m_settings.m_codec  = mod->getSavedValue<std::string>("render_codec", "");
    if (m_settings.m_codec.empty()) m_settings.m_codec = getDefaultCodec();

    auto br = geode::utils::numFromString<int>(
        mod->getSavedValue<std::string>("render_bitrate", "30")).unwrapOr(30);
    m_settings.m_bitrate = (uint32_t)br * 1'000'000;

    m_settings.m_renderArgs = mod->getSavedValue<std::string>("render_video_args", "");
    m_settings.m_afterEndTime = geode::utils::numFromString<float>(
        mod->getSavedValue<std::string>("render_seconds_after", "3")).unwrapOr(3.f);
    m_settings.m_colorFix  = mod->getSavedValue<bool>("render_color_fix", true);
    m_settings.m_audioCodec = mod->getSavedValue<std::string>("render_audio_codec", "aac");
    m_settings.m_musicVolume = mod->getSavedValue<double>("render_music_volume", 1.0);
    m_settings.m_sfxVolume   = mod->getSavedValue<double>("render_sfx_volume", 1.0);

    // Stage 3 (audio): the GUI "Include Audio" toggle gates the whole audio
    // path. Default on; when off, start()/update()/stop() skip audio entirely
    // and the render is video-only (same as builds before this one).
    m_collectAudio = mod->getSavedValue<bool>("render_include_audio", true);

    std::string ext = mod->getSavedValue<std::string>("render_file_extension", ".mp4");
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    m_settings.m_extension = ext;
}

// ─────────────────────────────────────────────────────────────────────────────
// start — video-only setup (Stage 1)
// ─────────────────────────────────────────────────────────────────────────────

geode::Result<> SLRenderer::start() {
    geode::log::info("[GucciBot] SLRenderer starting");
    if (!ff || !m_ffmpegLoaded) return geode::Err("FFmpeg not loaded");
    auto* pl = PlayLayer::get();
    if (!pl) return geode::Err("Not in a level");

    loadSettingsFromGeode();
    m_seenFrames = 0;

    // Query FMOD's software sample rate + channel count (Silicate renderer.cpp:233).
    // Feeds the audio encoder + the SwrContext input layout. Harmless when audio
    // is disabled; matches upstream which queries this unconditionally.
    FMODAudioEngine::get()->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);

    std::vector<RenderOpt> args;
    { std::stringstream s(m_settings.m_renderArgs); args = parseArgs(s); }

    auto* mod = Mod::get();

    // Output directory: honor the GUI "Output Folder" field when set; else fall
    // back to the mod save dir's renders/ folder. (Mirrors the legacy renderer,
    // which read render_output_folder / render_name.)
    std::string renderFolderStr = mod->getSavedValue<std::string>("render_output_folder", "");
    fs::path outDir;
    if (!renderFolderStr.empty()) {
        outDir = fs::path(renderFolderStr);
    } else {
        outDir = mod->getSaveDir() / "renders";
    }
    std::error_code mkec;
    fs::create_directories(outDir, mkec);

    // Filename: honor the GUI "Output Name" field when set; else the auto
    // template (level name + random); else the configured output path.
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

    ff->avformat_alloc_output_context2(&m_formatCtx, nullptr, nullptr, outPath.c_str());
    if (!m_formatCtx) return geode::Err("Failed to allocate output context");

    m_videoCodec = ff->avcodec_find_encoder_by_name(m_settings.m_codec.c_str());
    if (!m_videoCodec) return geode::Err("Failed to find codec '{}'", m_settings.m_codec);

    m_videoCodecCtx.reset(ff->avcodec_alloc_context3(m_videoCodec));
    if (!m_videoCodecCtx) return geode::Err("Failed to allocate codec context");

    AVPixelFormat* outPixFmts = nullptr;
    int outPixFmtCount = 0;
    m_settings.m_pixFmt = AV_PIX_FMT_NONE;
    if (ff->avcodec_get_supported_config(m_videoCodecCtx.get(), m_videoCodec,
            AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&outPixFmts,
            &outPixFmtCount) < 0) {
        return geode::Err("Failed to query codec pixel-format capabilities");
    }

    std::unique_ptr<Colorspace> colorspace;
    for (int i = 0; i < outPixFmtCount; i++) {
        switch (outPixFmts[i]) {
            case AV_PIX_FMT_YUV420P: m_settings.m_pixFmt = AV_PIX_FMT_YUV420P; colorspace = std::make_unique<YUV420PColorspace>(); break;
            case AV_PIX_FMT_NV12:    m_settings.m_pixFmt = AV_PIX_FMT_NV12;    colorspace = std::make_unique<NV12Colorspace>();    break;
            case AV_PIX_FMT_RGB0:    m_settings.m_pixFmt = AV_PIX_FMT_RGB0;    colorspace = std::make_unique<RGB0Colorspace>();    break;
            case AV_PIX_FMT_RGB24:   m_settings.m_pixFmt = AV_PIX_FMT_RGB24;   colorspace = std::make_unique<RGB24Colorspace>();   break;
            default: break;
        }
        if (m_settings.m_pixFmt != AV_PIX_FMT_NONE) break;
    }
    if (m_settings.m_pixFmt == AV_PIX_FMT_NONE)
        return geode::Err("Couldn't find a usable pixel format for codec '{}'", m_settings.m_codec);

    m_videoCodecCtx->codec_id    = m_videoCodec->id;
    m_videoCodecCtx->pix_fmt     = m_settings.m_pixFmt;
    m_videoCodecCtx->width       = m_settings.m_width;
    m_videoCodecCtx->height      = m_settings.m_height;
    m_videoCodecCtx->bit_rate    = m_settings.m_bitrate;
    m_videoCodecCtx->time_base   = {1, m_settings.m_fps};
    m_videoCodecCtx->framerate   = {m_settings.m_fps, 1};
    m_videoCodecCtx->max_b_frames = 0;
    m_videoCodecCtx->color_primaries = AVCOL_PRI_BT709;
    m_videoCodecCtx->colorspace      = AVCOL_SPC_BT709;
    m_videoCodecCtx->color_trc       = AVCOL_TRC_BT709;
    m_videoCodecCtx->color_range     = AVCOL_RANGE_MPEG;
    if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* video_opts = nullptr;
    for (const auto& arg : args)
        ff->av_dict_set(&video_opts, arg.m_name.c_str(), arg.m_value.c_str(), 0);
    if (ff->avcodec_open2(m_videoCodecCtx.get(), m_videoCodec, &video_opts) < 0)
        return geode::Err("Failed to open codec '{}'", m_settings.m_codec);
    ff->av_dict_free(&video_opts);

    m_videoStream = ff->avformat_new_stream(m_formatCtx, m_videoCodec);
    if (!m_videoStream) return geode::Err("Failed to create video stream");
    if (ff->avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx.get()) < 0)
        return geode::Err("Failed to copy codec parameters");

    // ── Audio setup (Stage 3), gated on m_collectAudio (GUI "Include Audio").
    // Ported from Silicate renderer.cpp:284-442. MUST run before
    // avformat_write_header so the muxer sees both streams when writing the
    // header. Sub-ordering mirrors upstream exactly: find encoder -> alloc ctx
    // -> configure -> open -> new stream -> copy params -> set time_base.
    if (m_collectAudio) {
        m_audioCodec = ff->avcodec_find_encoder_by_name(m_settings.m_audioCodec.c_str());
        if (!m_audioCodec) return geode::Err("Failed to find audio codec '{}'", m_settings.m_audioCodec);

        m_audioCodecCtx.reset(ff->avcodec_alloc_context3(m_audioCodec));
        if (!m_audioCodecCtx) return geode::Err("Failed to allocate audio codec context");

        m_audioCodecCtx->codec_tag   = 0;
        m_audioCodecCtx->codec_type  = AVMEDIA_TYPE_AUDIO;
        m_audioCodecCtx->ch_layout   = AV_CHANNEL_LAYOUT_STEREO;
        m_audioCodecCtx->bit_rate    = 320000;
        m_audioCodecCtx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
        m_audioCodecCtx->sample_rate = m_sampleRate;
        m_audioCodecCtx->time_base   = {1, m_sampleRate};
        m_audioCodecCtx->flags &= ~AV_CODEC_FLAG_QSCALE;
        if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
            m_audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* audio_opts = nullptr;
        ff->av_dict_set(&audio_opts, "compression_level", "0", 0);
        if (ff->avcodec_open2(m_audioCodecCtx.get(), m_audioCodec, &audio_opts) < 0)
            return geode::Err("Failed to open audio codec '{}'", m_settings.m_audioCodec);
        ff->av_dict_free(&audio_opts);

        m_audioStream = ff->avformat_new_stream(m_formatCtx, m_audioCodec);
        if (!m_audioStream) return geode::Err("Failed to create audio stream");
        if (ff->avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx.get()) < 0)
            return geode::Err("Failed to copy audio codec parameters");
        m_audioStream->time_base = m_audioCodecCtx->time_base;

        geode::log::info("[GucciBot] Audio stream: {} @ {}Hz, frame_size={}",
                         m_settings.m_audioCodec, m_sampleRate, m_audioCodecCtx->frame_size);
    }

    if (ff->avio_open(&m_formatCtx->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
        return geode::Err("Failed to open output file");
    if (ff->avformat_write_header(m_formatCtx, nullptr) < 0)
        return geode::Err("Failed to write header");

    m_frame.reset(ff->av_frame_alloc());
    if (!m_frame) return geode::Err("Failed to allocate frame");
    m_frame->format = m_settings.m_pixFmt;
    m_frame->width  = m_settings.m_width;
    m_frame->height = m_settings.m_height;

    m_pkt.reset(ff->av_packet_alloc());
    if (!m_pkt) return geode::Err("Failed to allocate packet");

    // Audio frame + packet (Silicate renderer.cpp:461-482).
    if (m_collectAudio) {
        m_audioFrame.reset(ff->av_frame_alloc());
        if (!m_audioFrame) return geode::Err("Failed to allocate audio frame");
        m_audioFrame->nb_samples  = m_audioCodecCtx->frame_size;
        m_audioFrame->sample_rate = m_audioCodecCtx->sample_rate;
        m_audioFrame->format      = m_audioCodecCtx->sample_fmt;
        m_audioFrame->ch_layout   = m_audioCodecCtx->ch_layout;
        if (ff->av_frame_get_buffer(m_audioFrame.get(), 0) < 0)
            return geode::Err("Failed to get audio frame buffer");

        m_audioPkt.reset(ff->av_packet_alloc());
        if (!m_audioPkt) return geode::Err("Failed to allocate packet (audio)");

        m_audioIndex = 0;
        m_audioBuffer.clear();
    }

    m_time = 0.0; m_endTime = 0.0f; m_updateIndex = 0; m_frameCount = 0;
    m_recording = true;

    m_texture.m_width  = m_settings.m_width;
    m_texture.m_height = m_settings.m_height;
    m_alignedWidth  = (m_settings.m_width  + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    m_alignedHeight = (m_settings.m_height + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    m_texture.m_alignedWidth  = m_alignedWidth;
    m_texture.m_alignedHeight = m_alignedHeight;
    m_texture.m_widthOffset  = 0;  // defaults; overridden from ShaderLayer below
    m_texture.m_heightOffset = 0;

    auto frameSize = CCDirector::get()->getOpenGLView()->getFrameSize();
    silentChangeSize(CCSize(m_alignedWidth, m_alignedHeight));
    resizeShaderLayer(CCSize(m_alignedWidth, m_alignedHeight), frameSize);
    if (ShaderLayer* sh = GJBaseGameLayer::get()->m_shaderLayer) {
        // Offsets feed only the live preview blit. Faithful to Silicate, which
        // reads .width for both (latent upstream quirk; 0 for matching aspects).
        m_texture.m_widthOffset  = static_cast<uint32_t>(sh->m_targetTextureSizeExtra.width);
        m_texture.m_heightOffset = static_cast<uint32_t>(sh->m_targetTextureSizeExtra.width);
    }
    silentChangeSize(frameSize);

    m_texture.init(std::move(colorspace));
    m_bufferSize = m_texture.m_colorspace->getBufferSize();
    geode::log::info("[GucciBot] SLRenderer capture ready — buffer {}", m_bufferSize);

    // Attach the FMOD DSP capture (Silicate renderer.cpp:527-529). Flips FMOD to
    // NOSOUND_NRT and routes the master mix into AudioRecorder::m_buffer, which
    // the FMOD::System::update hook drains into writeAudio each render tick.
    if (m_collectAudio) {
        AudioRecorder::get()->init();
        AudioRecorder::get()->attach(m_settings.m_musicVolume, m_settings.m_sfxVolume);
    }

    std::thread(&SLRenderer::recordLoop, this).detach();
    return geode::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// encode / write / stop / loop / capture / update (video only)
// ─────────────────────────────────────────────────────────────────────────────

geode::Result<> SLRenderer::encode(uint8_t* data, size_t size) {
    ff->av_frame_unref(m_frame.get());
    m_frame->format = m_videoCodecCtx->pix_fmt;
    m_frame->width  = m_videoCodecCtx->width;
    m_frame->height = m_videoCodecCtx->height;

    int ret = ff->av_frame_get_buffer(m_frame.get(), 1);
    if (ret < 0) return geode::Err("Failed to allocate frame buffer: {}", ret);
    ret = ff->av_frame_make_writable(m_frame.get());
    if (ret < 0) return geode::Err("Failed to make frame writable");

    ff->av_image_fill_linesizes(m_frame->linesize, m_settings.m_pixFmt, m_alignedWidth);

    auto result = m_texture.m_colorspace->prepareFrame(m_frame.get(), data, size);
    if (result.isErr()) return result;

    m_frame->pts = m_updateIndex - 1;
    ret = ff->avcodec_send_frame(m_videoCodecCtx.get(), m_frame.get());
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) return geode::Err("Codec requires more input data");
        if (ret == AVERROR_EOF)     return geode::Err("End of file reached");
        return geode::Err("Failed to send frame: {}", ret);
    }
    return geode::Ok();
}

geode::Result<> SLRenderer::write() {
    int ret = 0;
    while (ret >= 0) {
        ret = ff->avcodec_receive_packet(m_videoCodecCtx.get(), m_pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return geode::Err("Failed to receive packet");
        ff->av_packet_rescale_ts(m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_pkt->stream_index = m_videoStream->index;
        ret = ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
        if (ret < 0) return geode::Err("Failed to write frame");
        ff->av_packet_unref(m_pkt.get());
    }
    return geode::Ok();
}

geode::Result<> SLRenderer::writeAudio(std::vector<float>& data, uint64_t pts) {
    // Ported from Silicate renderer.cpp:607-668. Converts the captured float PCM
    // (interleaved, AV_SAMPLE_FMT_FLT @ m_sampleRate / m_channels) into the
    // encoder's planar float format via a lazily-allocated SwrContext, then
    // encodes + muxes the packet. Runs on the MAIN thread (called from the
    // FMOD::System::update hook), same as upstream.
    m_audioFrame->pts        = pts;
    m_audioFrame->nb_samples = m_audioCodecCtx->frame_size;
    m_audioFrame->ch_layout  = m_audioCodecCtx->ch_layout;

    int ret = 0;

    AVChannelLayout inputChLayout;
    ff->av_channel_layout_default(&inputChLayout, m_channels);

    if (!m_swrCtx) {
        ret = ff->swr_alloc_set_opts2(
            &m_swrCtx, &m_audioCodecCtx->ch_layout, m_audioCodecCtx->sample_fmt,
            m_audioCodecCtx->sample_rate, &inputChLayout, AV_SAMPLE_FMT_FLT,
            m_sampleRate, 0, nullptr);
        if (ret < 0) return geode::Err("Failed to set swr options: {}", ret);
        if (!m_swrCtx) return geode::Err("Failed to allocate swr context");
        if (ff->swr_init(m_swrCtx) < 0) return geode::Err("Failed to initialize swr context");
    }

    const uint8_t* inData[1] = {reinterpret_cast<const uint8_t*>(data.data())};
    ff->swr_convert(m_swrCtx, m_audioFrame->data, m_audioCodecCtx->frame_size,
                    inData, m_audioCodecCtx->frame_size);

    ret = ff->avcodec_send_frame(m_audioCodecCtx.get(), m_audioFrame.get());
    if (ret < 0) return geode::Err("Failed to send audio frame: {}", ret);

    // Drain encoded audio packets. m_audioPkt (not m_pkt) — video packets are
    // drained separately on the recordLoop thread.
    while (ret >= 0) {
        ret = ff->avcodec_receive_packet(m_audioCodecCtx.get(), m_audioPkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return geode::Err("Failed to receive audio packet");

        ff->av_packet_rescale_ts(m_audioPkt.get(), m_audioCodecCtx->time_base,
                                 m_audioStream->time_base);
        m_audioPkt->stream_index = m_audioStream->index;
        ret = ff->av_interleaved_write_frame(m_formatCtx, m_audioPkt.get());
        if (ret < 0) return geode::Err("Failed to write audio frame");
        ff->av_packet_unref(m_audioPkt.get());
    }

    return geode::Ok();
}

geode::Result<> SLRenderer::stop() {
    m_recording = false;

    // Tear down FMOD capture first (Silicate renderer.cpp:672-674). Both calls
    // are internally guarded, so safe even when audio was never attached
    // (detach: if (!m_attached) return; uninit: if (m_dsp)).
    AudioRecorder::get()->detach();
    AudioRecorder::get()->uninit();

    if (m_pkt && m_videoCodecCtx && m_formatCtx && m_videoStream) {
        ff->avcodec_send_frame(m_videoCodecCtx.get(), nullptr);
        while (ff->avcodec_receive_packet(m_videoCodecCtx.get(), m_pkt.get()) == 0) {
            ff->av_packet_rescale_ts(m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_pkt->stream_index = m_videoStream->index;
            ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
            ff->av_packet_unref(m_pkt.get());
        }
    }

    // Flush the audio encoder (Silicate renderer.cpp:693-710). Guarded so it's
    // a no-op when audio was disabled (m_audioStream/m_audioPkt stay null).
    if (m_audioPkt && m_audioCodecCtx && m_formatCtx && m_audioStream) {
        ff->avcodec_send_frame(m_audioCodecCtx.get(), nullptr);
        while (ff->avcodec_receive_packet(m_audioCodecCtx.get(), m_audioPkt.get()) == 0) {
            ff->av_packet_rescale_ts(m_audioPkt.get(), m_audioCodecCtx->time_base, m_audioStream->time_base);
            m_audioPkt->stream_index = m_audioStream->index;
            ff->av_interleaved_write_frame(m_formatCtx, m_audioPkt.get());
            ff->av_packet_unref(m_audioPkt.get());
        }
    }

    if (m_formatCtx) {
        ff->av_write_trailer(m_formatCtx);
        if (m_formatCtx->pb) ff->avio_close(m_formatCtx->pb);
    }
    ff->swr_free(&m_swrCtx);  // safe on nullptr (audio disabled / first render)

    m_halting = false;
    m_collected = false;
    if (m_needsCleanup) m_texture.postCapture();
    if (m_frame) { m_frame->data[0] = nullptr; }
    m_texture.destroy();
    geode::log::info("[GucciBot] SLRenderer stopped");
    return geode::Ok();
}

void SLRenderer::recordLoop() {
    while (m_recording || m_collected) {
        if (m_collected) {
            auto e = this->encode(m_buffer, m_bufferSize);
            if (e.isErr()) { geode::log::error("[GucciBot] encode failed: {}", e.unwrapErr()); break; }
            auto w = this->write();
            if (w.isErr()) { geode::log::error("[GucciBot] write failed: {}", w.unwrapErr()); break; }
            m_needsCleanup = true;
            m_collected = false;
            m_halting = false;
        }
    }
    auto s = this->stop();
    if (s.isErr()) geode::log::error("[GucciBot] stop failed: {}", s.unwrapErr());
}

void SLRenderer::capture() { m_texture.capture(&m_buffer, m_collected); }

void SLRenderer::update(PlayLayer* pl) {
    if (!this->isRecording()) return;

    bool started = pl->m_started || (m_settings.m_firstAttemptPause && m_seenFrames >= 2);
    m_seenFrames++;  // GD adds two "fade" frames
    if (pl->m_isPaused || !started) return;
    if (m_halting || m_collected) return;
    m_halting = true;

    m_time = ++m_updateIndex * this->getDt();

    // Stop once the level is complete and the macro has no more queued inputs.
    if (pl->m_hasCompletedLevel &&
        !GucciEngine::get()->replay.getCurrentQueuedInput().has_value()) {
        if (this->m_endTime >= this->m_settings.m_afterEndTime) {
            this->signalStop();
            return;
        }
        this->m_endTime += this->getDt();
    }

    // Per-frame audio pump (Silicate renderer.cpp:798-800). Advances FMOD by one
    // frame's dt, firing the DSP read callback that fills AudioRecorder::m_buffer;
    // the hooked FMOD::System::update then drains it in 1024-sample chunks into
    // writeAudio. Without this call the capture side never runs, so even a fully
    // wired encode path produces a silent file.
    if (m_collectAudio) AudioRecorder::get()->unpause();

    this->capture();
}

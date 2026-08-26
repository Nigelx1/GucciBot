#include "renderer.hpp"
#include "GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/ShaderLayer.hpp>

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

enum class GPUVendor { NVIDIA, AMD, INTEL, OTHER };

static GPUVendor getGPUVendor() {
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
        if (keyName.find("VEN_1002") != std::string::npos ||
            keyName.find("VEN_1022") != std::string::npos) {
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
}

static std::string getDefaultCodec() {
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
    sh->m_renderTexture = CCRenderTexture::create(size.width / sh->m_aspectRatio * baseAspectRatio,
                                                  size.height,
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
                                  size.width + 2.0f,
                                  size.height + 2.0f});
    sh->m_state.m_textureScaleX = size.width / winSize.width;
    sh->m_state.m_textureScaleY = size.height / winSize.height;
    geode::log::info("[GucciBot] resizeShaderLayer -> {}x{}", (int)size.width, (int)size.height);
}

void SLRenderer::loadSettingsFromGeode() {
    auto* mod = Mod::get();
    m_settings.m_width = (int)mod->getSavedValue<int64_t>("render_width", 1920);
    m_settings.m_height = (int)mod->getSavedValue<int64_t>("render_height", 1080);
    m_settings.m_fps = (int)mod->getSavedValue<int64_t>("render_fps", 60);
    m_settings.m_codec = mod->getSavedValue<std::string>("render_codec", "");
    if (m_settings.m_codec.empty())
        m_settings.m_codec = getDefaultCodec();

    auto br =
        geode::utils::numFromString<int>(mod->getSavedValue<std::string>("render_bitrate", "30"))
            .unwrapOr(30);
    m_settings.m_bitrate = (uint32_t)br * 1'000'000;

    m_settings.m_renderArgs = mod->getSavedValue<std::string>("render_video_args", "");
    m_settings.m_afterEndTime = geode::utils::numFromString<float>(
                                    mod->getSavedValue<std::string>("render_seconds_after", "3"))
                                    .unwrapOr(3.f);
    m_settings.m_colorFix = mod->getSavedValue<bool>("render_color_fix", true);
    m_settings.m_audioCodec = mod->getSavedValue<std::string>("render_audio_codec", "aac");
    m_settings.m_musicVolume = mod->getSavedValue<double>("render_music_volume", 1.0);
    m_settings.m_sfxVolume = mod->getSavedValue<double>("render_sfx_volume", 1.0);

    m_collectAudio = mod->getSavedValue<bool>("render_include_audio", true);
    m_settings.m_splitAudioTracks = mod->getSavedValue<bool>("render_split_audio_tracks", false);

    std::string ext = mod->getSavedValue<std::string>("render_file_extension", ".mp4");
    if (!ext.empty() && ext[0] == '.')
        ext.erase(0, 1);
    m_settings.m_extension = ext;
}

geode::Result<> SLRenderer::start() {
    geode::log::info("[GucciBot] SLRenderer starting");
    if (!ff || !m_ffmpegLoaded)
        return geode::Err("FFmpeg not loaded");
    auto* pl = PlayLayer::get();
    if (!pl)
        return geode::Err("Not in a level");

    loadSettingsFromGeode();
    m_seenFrames = 0;

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
        outDir = fs::path(renderFolderStr);
    } else {
        outDir = mod->getSaveDir() / "renders";
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

    ff->avformat_alloc_output_context2(&m_formatCtx, nullptr, nullptr, outPath.c_str());
    if (!m_formatCtx)
        return geode::Err("Failed to allocate output context");

    m_videoCodec = ff->avcodec_find_encoder_by_name(m_settings.m_codec.c_str());
    if (!m_videoCodec)
        return geode::Err("Failed to find codec '{}'", m_settings.m_codec);

    m_videoCodecCtx.reset(ff->avcodec_alloc_context3(m_videoCodec));
    if (!m_videoCodecCtx)
        return geode::Err("Failed to allocate codec context");

    AVPixelFormat* outPixFmts = nullptr;
    int outPixFmtCount = 0;
    m_settings.m_pixFmt = AV_PIX_FMT_NONE;
    if (ff->avcodec_get_supported_config(m_videoCodecCtx.get(),
                                         m_videoCodec,
                                         AV_CODEC_CONFIG_PIX_FORMAT,
                                         0,
                                         (const void**)&outPixFmts,
                                         &outPixFmtCount) < 0) {
        return geode::Err("Failed to query codec pixel-format capabilities");
    }

    std::unique_ptr<Colorspace> colorspace;
    for (int i = 0; i < outPixFmtCount; i++) {
        switch (outPixFmts[i]) {
        case AV_PIX_FMT_YUV420P:
            m_settings.m_pixFmt = AV_PIX_FMT_YUV420P;
            colorspace = std::make_unique<YUV420PColorspace>();
            break;
        case AV_PIX_FMT_NV12:
            m_settings.m_pixFmt = AV_PIX_FMT_NV12;
            colorspace = std::make_unique<NV12Colorspace>();
            break;
        case AV_PIX_FMT_RGB0:
            m_settings.m_pixFmt = AV_PIX_FMT_RGB0;
            colorspace = std::make_unique<RGB0Colorspace>();
            break;
        case AV_PIX_FMT_RGB24:
            m_settings.m_pixFmt = AV_PIX_FMT_RGB24;
            colorspace = std::make_unique<RGB24Colorspace>();
            break;
        default:
            break;
        }
        if (m_settings.m_pixFmt != AV_PIX_FMT_NONE)
            break;
    }
    if (m_settings.m_pixFmt == AV_PIX_FMT_NONE)
        return geode::Err("Couldn't find a usable pixel format for codec '{}'", m_settings.m_codec);

    m_videoCodecCtx->codec_id = m_videoCodec->id;
    m_videoCodecCtx->pix_fmt = m_settings.m_pixFmt;
    m_videoCodecCtx->width = m_settings.m_width;
    m_videoCodecCtx->height = m_settings.m_height;
    m_videoCodecCtx->bit_rate = m_settings.m_bitrate;
    m_videoCodecCtx->time_base = {1, m_settings.m_fps};
    m_videoCodecCtx->framerate = {m_settings.m_fps, 1};
    m_videoCodecCtx->max_b_frames = 0;
    m_videoCodecCtx->color_primaries = AVCOL_PRI_BT709;
    m_videoCodecCtx->colorspace = AVCOL_SPC_BT709;
    m_videoCodecCtx->color_trc = AVCOL_TRC_BT709;
    m_videoCodecCtx->color_range = AVCOL_RANGE_MPEG;
    if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* video_opts = nullptr;
    for (const auto& arg : args)
        ff->av_dict_set(&video_opts, arg.m_name.c_str(), arg.m_value.c_str(), 0);
    if (ff->avcodec_open2(m_videoCodecCtx.get(), m_videoCodec, &video_opts) < 0)
        return geode::Err("Failed to open codec '{}'", m_settings.m_codec);
    ff->av_dict_free(&video_opts);

    m_videoStream = ff->avformat_new_stream(m_formatCtx, m_videoCodec);
    if (!m_videoStream)
        return geode::Err("Failed to create video stream");
    if (ff->avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx.get()) < 0)
        return geode::Err("Failed to copy codec parameters");

    m_audioTracks.clear();
    if (m_collectAudio) {
        int trackCount = m_settings.m_splitAudioTracks ? 4 : 1;
        static const char* kTrackNames[4] = {"combined", "music", "sfx", "frame-window"};
        for (int i = 0; i < trackCount; i++) {
            SLAudioTrack track;

            track.codec = ff->avcodec_find_encoder_by_name(m_settings.m_audioCodec.c_str());
            if (!track.codec)
                return geode::Err("Failed to find audio codec '{}'", m_settings.m_audioCodec);

            track.codecCtx.reset(ff->avcodec_alloc_context3(track.codec));
            if (!track.codecCtx)
                return geode::Err("Failed to allocate audio codec context");

            track.codecCtx->codec_tag = 0;
            track.codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
            track.codecCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
            track.codecCtx->bit_rate = 320000;
            track.codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            track.codecCtx->sample_rate = m_sampleRate;
            track.codecCtx->time_base = {1, m_sampleRate};
            track.codecCtx->flags &= ~AV_CODEC_FLAG_QSCALE;
            if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
                track.codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            AVDictionary* audio_opts = nullptr;
            ff->av_dict_set(&audio_opts, "compression_level", "0", 0);
            if (ff->avcodec_open2(track.codecCtx.get(), track.codec, &audio_opts) < 0)
                return geode::Err("Failed to open audio codec '{}'", m_settings.m_audioCodec);
            ff->av_dict_free(&audio_opts);

            track.stream = ff->avformat_new_stream(m_formatCtx, track.codec);
            if (!track.stream)
                return geode::Err("Failed to create audio stream");
            if (ff->avcodec_parameters_from_context(track.stream->codecpar, track.codecCtx.get()) <
                0)
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
        track.frame->nb_samples = track.codecCtx->frame_size;
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
    m_updateIndex = 0;
    m_frameCount = 0;
    m_recording = true;

    m_texture.m_width = m_settings.m_width;
    m_texture.m_height = m_settings.m_height;
    m_alignedWidth = (m_settings.m_width + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    m_alignedHeight = (m_settings.m_height + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    m_texture.m_alignedWidth = m_alignedWidth;
    m_texture.m_alignedHeight = m_alignedHeight;
    m_texture.m_widthOffset = 0;
    m_texture.m_heightOffset = 0;

    auto frameSize = CCDirector::get()->getOpenGLView()->getFrameSize();
    silentChangeSize(CCSize(m_alignedWidth, m_alignedHeight));
    resizeShaderLayer(CCSize(m_alignedWidth, m_alignedHeight), frameSize);
    if (ShaderLayer* sh = GJBaseGameLayer::get()->m_shaderLayer) {
        m_texture.m_widthOffset = static_cast<uint32_t>(sh->m_targetTextureSizeExtra.width);
        m_texture.m_heightOffset = static_cast<uint32_t>(sh->m_targetTextureSizeExtra.width);
    }
    silentChangeSize(frameSize);

    m_texture.init(std::move(colorspace));
    m_bufferSize = m_texture.m_colorspace->getBufferSize();
    geode::log::info("[GucciBot] SLRenderer capture ready — buffer {}", m_bufferSize);

    if (m_collectAudio) {
        gbfw::frameWindowChannelGroup();
        AudioEngineRenderState::enter(m_settings.m_musicVolume, m_settings.m_sfxVolume);

        AudioRecorder::get()->init();
        AudioRecorder::get()->attach();
        if (m_settings.m_splitAudioTracks) {
            auto* engine = FMODAudioEngine::get();
            AudioRecorder::getMusic()->init(engine->m_backgroundMusicChannel);
            AudioRecorder::getMusic()->attach();
            AudioRecorder::getSfx()->init(engine->m_globalChannel);
            AudioRecorder::getSfx()->attach();
            AudioRecorder::getFrameWindow()->init(gbfw::frameWindowChannelGroup());
            AudioRecorder::getFrameWindow()->attach();
        }
    }

    std::thread(&SLRenderer::recordLoop, this).detach();
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

    m_frame->pts = m_updateIndex - 1;
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
        ff->av_packet_rescale_ts(m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_pkt->stream_index = m_videoStream->index;
        ret = ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
        if (ret < 0)
            return geode::Err("Failed to write frame");
        ff->av_packet_unref(m_pkt.get());
    }
    return geode::Ok();
}

geode::Result<> SLRenderer::writeAudio(std::vector<float>& data, uint64_t pts, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)m_audioTracks.size())
        return geode::Err("Invalid audio track index {}", trackIndex);
    auto& track = m_audioTracks[trackIndex];

    track.frame->pts = pts;
    track.frame->nb_samples = track.codecCtx->frame_size;
    track.frame->ch_layout = track.codecCtx->ch_layout;

    int ret = 0;

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

    const uint8_t* inData[1] = {reinterpret_cast<const uint8_t*>(data.data())};
    ff->swr_convert(track.swrCtx,
                    track.frame->data,
                    track.codecCtx->frame_size,
                    inData,
                    track.codecCtx->frame_size);

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
        ret = ff->av_interleaved_write_frame(m_formatCtx, track.pkt.get());
        if (ret < 0)
            return geode::Err("Failed to write audio frame");
        ff->av_packet_unref(track.pkt.get());
    }

    return geode::Ok();
}

geode::Result<> SLRenderer::stop() {
    m_recording = false;

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
    AudioEngineRenderState::exit();

    if (m_pkt && m_videoCodecCtx && m_formatCtx && m_videoStream) {
        ff->avcodec_send_frame(m_videoCodecCtx.get(), nullptr);
        while (ff->avcodec_receive_packet(m_videoCodecCtx.get(), m_pkt.get()) == 0) {
            ff->av_packet_rescale_ts(
                m_pkt.get(), m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_pkt->stream_index = m_videoStream->index;
            ff->av_interleaved_write_frame(m_formatCtx, m_pkt.get());
            ff->av_packet_unref(m_pkt.get());
        }
    }

    for (auto& track : m_audioTracks) {
        if (!(track.pkt && track.codecCtx && m_formatCtx && track.stream))
            continue;
        ff->avcodec_send_frame(track.codecCtx.get(), nullptr);
        while (ff->avcodec_receive_packet(track.codecCtx.get(), track.pkt.get()) == 0) {
            ff->av_packet_rescale_ts(
                track.pkt.get(), track.codecCtx->time_base, track.stream->time_base);
            track.pkt->stream_index = track.stream->index;
            ff->av_interleaved_write_frame(m_formatCtx, track.pkt.get());
            ff->av_packet_unref(track.pkt.get());
        }
    }

    if (m_formatCtx) {
        ff->av_write_trailer(m_formatCtx);
        if (m_formatCtx->pb)
            ff->avio_close(m_formatCtx->pb);
    }
    for (auto& track : m_audioTracks)
        ff->swr_free(&track.swrCtx);
    m_audioTracks.clear();

    m_halting = false;
    m_collected = false;
    if (m_needsCleanup)
        m_texture.postCapture();
    if (m_frame) {
        m_frame->data[0] = nullptr;
    }
    m_texture.destroy();
    geode::log::info("[GucciBot] SLRenderer stopped");
    return geode::Ok();
}

void SLRenderer::recordLoop() {
    while (m_recording || m_collected) {
        if (m_collected) {
            auto e = this->encode(m_buffer, m_bufferSize);
            if (e.isErr()) {
                geode::log::error("[GucciBot] encode failed: {}", e.unwrapErr());
                break;
            }
            auto w = this->write();
            if (w.isErr()) {
                geode::log::error("[GucciBot] write failed: {}", w.unwrapErr());
                break;
            }
            m_needsCleanup = true;
            m_collected = false;
            m_halting = false;
        }
    }
    auto s = this->stop();
    if (s.isErr())
        geode::log::error("[GucciBot] stop failed: {}", s.unwrapErr());
}

void SLRenderer::capture() {
    m_texture.capture(&m_buffer, m_collected);
}

void SLRenderer::update(PlayLayer* pl) {
    if (!this->isRecording())
        return;

    bool started = pl->m_started || (m_settings.m_firstAttemptPause && m_seenFrames >= 2);
    m_seenFrames++;
    if (pl->m_isPaused || !started)
        return;
    if (m_halting || m_collected)
        return;
    m_halting = true;

    m_time = ++m_updateIndex * this->getDt();

    if (pl->m_hasCompletedLevel &&
        !GucciEngine::get()->replay.getCurrentQueuedInput().has_value()) {
        if (this->m_endTime >= this->m_settings.m_afterEndTime) {
            this->signalStop();
            return;
        }
        this->m_endTime += this->getDt();
    }

    if (m_collectAudio)
        AudioEngineRenderState::pump(getDt(), m_settings.m_splitAudioTracks);

    this->capture();
}

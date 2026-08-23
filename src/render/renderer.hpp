#ifndef SL_RENDERER_HPP
#define SL_RENDERER_HPP

#include "ffmpeg.hpp"
#include "texture.hpp"
#include "dsp.hpp"

#include <Geode/Geode.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

struct SLRendererSettings {
    int m_width = 1920;
    int m_height = 1080;
    uint32_t m_bitrate = 30'000'000;
    std::string m_codec = "libx264";
    AVPixelFormat m_pixFmt = AV_PIX_FMT_YUV420P;

    int m_fps = 60;

    float m_afterEndTime = 3.0f;
    bool m_colorFix = true;
    bool m_firstAttemptPause = false;

    std::string m_outputPath = "output";
    std::string m_extension = "mp4";
    std::string m_audioCodec = "aac";

    std::string m_renderArgs = "";

    double m_musicVolume = 1.0;
    double m_sfxVolume = 1.0;

    // Nigel's ask, 2026-08-23: off (default) = one combined audio track,
    // exactly as before. On = FOUR tracks -- the original combined mix PLUS
    // music, level/UI SFX, and frame-window cues isolated -- muxed into the
    // same output file (Juice's correction: adds the 3 isolated tracks
    // alongside the combined one, doesn't replace it). See render/dsp.hpp/
    // .cpp for the capture side (AudioRecorder::get()/getMusic()/getSfx()/
    // getFrameWindow(), each hooked to its own FMOD channel group) and
    // SLRenderer::start()/writeAudio() below for the FFmpeg multi-stream side.
    bool m_splitAudioTracks = false;
};

#define SL_AV_PTR(type) std::unique_ptr<type, void (*)(type*)>
#define SL_AV_LEAK(type) [](type*) {}

// One encoded audio stream in the output file. m_audioStream/m_audioCodecCtx/
// etc. used to be single SLRenderer members (one combined track only) --
// now there's a vector of these, sized 1 (combined only) or 4 (split: index
// 0 = combined, 1 = music, 2 = SFX, 3 = frame-window cues). swrCtx is
// per-track (not shared) because FMOD::Swr::Context carries internal
// resampling state tied to one specific stream of samples -- sharing it
// across concurrently-fed tracks would corrupt that state.
struct SLAudioTrack {
    AVStream* stream = nullptr;
    SL_AV_PTR(AVCodecContext) codecCtx = {nullptr, SL_AV_LEAK(AVCodecContext)};
    const AVCodec* codec = nullptr;
    SL_AV_PTR(AVFrame) frame = {nullptr, SL_AV_LEAK(AVFrame)};
    SL_AV_PTR(AVPacket) pkt = {nullptr, SL_AV_LEAK(AVPacket)};
    SwrContext* swrCtx = nullptr;
};

class SLRenderer {
   public:
    void queueStart() { m_shouldStart = true; }
    void startIfQueued() {
        if (m_shouldStart) {
            m_shouldStart = false;
            auto ret = start();
            if (ret.isErr()) {
                geode::log::error("[GucciBot] Failed to start SLRenderer: {}",
                                  ret.unwrapErr());
            }
        }
    }
    geode::Result<> start();
    geode::Result<> encode(uint8_t* data, size_t size);
    geode::Result<> write();
    geode::Result<> writeAudio(std::vector<float>& data, uint64_t pts, int trackIndex);
    geode::Result<> stop();

    void signalStop() { m_recording = false; }

    void recordLoop();

    void capture();
    void update(PlayLayer* pl);

    void displayPreview() { m_texture.displayPreview(); }

    SLRendererSettings m_settings;

    static SLRenderer* get() {
        static SLRenderer instance;
        return &instance;
    }

    float getDt() const { return 1.f / m_settings.m_fps; }
    bool isRecording() const { return m_recording; }
    float getTime() const { return m_time; }
    inline bool isFFmpegLoaded() const { return m_ffmpegLoaded; }

    void loadFFmpeg() {
        if (ff) {
            free(ff);
        }
        ff = (ff_t*)malloc(sizeof(ff_t));
        if (!ff) return;
        m_ffmpegLoaded = loadFFmpegFunctions(ff);
    }

                void loadSettingsFromGeode();

    bool m_shouldStart = false;
    bool m_collectAudio = true;

    std::atomic<bool> m_halting = false;
    std::atomic<bool> m_collected = false;

    bool m_autoVideoName = true;
    std::string m_videoNameTemplate = "%name%_%rand%";

    double m_time = 0;
    bool m_needsCleanup = false;
    SLRenderTexture m_texture;
    ff_t* ff = 0;

   private:
    SL_AV_PTR(AVCodecContext) m_videoCodecCtx = {nullptr, SL_AV_LEAK(AVCodecContext)};
    AVFormatContext* m_formatCtx = nullptr;
    AVStream* m_videoStream = nullptr;

    const AVCodec* m_videoCodec = nullptr;

    SL_AV_PTR(AVFrame) m_frame = {nullptr, SL_AV_LEAK(AVFrame)};
    SL_AV_PTR(AVPacket) m_pkt = {nullptr, SL_AV_LEAK(AVPacket)};

    // Size 0 (audio off), 1 (combined track), or 3 (split: music/SFX/fw).
    std::vector<SLAudioTrack> m_audioTracks;

    float m_visualFps = 60.0f;
    bool m_recording = false;

    int m_frameCount = 0;
    int m_seenFrames = 0;
    int m_updateIndex = 0;
    float m_endTime = 0;

    int m_sampleRate = 44100;
    int m_channels = 2;

    uint8_t* m_buffer = nullptr;
    size_t m_bufferSize = 0;

    int m_alignedWidth = 0;
    int m_alignedHeight = 0;

    bool m_ffmpegLoaded = false;

    std::mutex m_lock;

    friend class AudioRecorder;
};

#endif

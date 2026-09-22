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

namespace gucci {

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

        // GD plays level sound through two different calls, so a render can
        // treat them separately:
        //
        //   m_sfxVolume         gameplay sound -- death, orbs, pads, portals,
        //                       checkpoints, level complete, UI. These go
        //                       through the plain playEffect overloads.
        //   m_triggerSfxVolume  the level's OWN sound, placed by its creator
        //                       with SFX triggers, which call
        //                       playEffectAdvanced with an effect id and SFX
        //                       group. On many modern levels this is part of
        //                       the song rather than decoration, so muting it
        //                       with the death sound loses real audio.
        double m_sfxVolume = 1.0;
        double m_triggerSfxVolume = 1.0;

        bool m_splitAudioTracks = false;
    };

#define SL_AV_PTR(type) std::unique_ptr<type, void (*)(type*)>
#define SL_AV_LEAK(type) [](type*) {}

    struct SLAudioTrack {
        AVStream* stream = nullptr;
        SL_AV_PTR(AVCodecContext) codecCtx = { nullptr, SL_AV_LEAK(AVCodecContext) };
        const AVCodec* codec = nullptr;
        SL_AV_PTR(AVFrame) frame = { nullptr, SL_AV_LEAK(AVFrame) };
        SL_AV_PTR(AVPacket) pkt = { nullptr, SL_AV_LEAK(AVPacket) };
        SwrContext* swrCtx = nullptr;
    };

    class SLRenderer {
    public:
        void queueStart() {
            m_shouldStart = true;
        }
        void startIfQueued() {
            if (m_shouldStart) {
                m_shouldStart = false;
                auto ret = start();
                if (ret.isErr()) {
                    geode::log::error("[GucciBot] Failed to start SLRenderer: {}", ret.unwrapErr());
                }
            }
        }
        geode::Result<> start();
        geode::Result<> encode(uint8_t* data, size_t size);
        geode::Result<> write();
        geode::Result<> writeAudio(std::vector<float>& data, uint64_t pts, int trackIndex);
        geode::Result<> stop();

        void signalStop() {
            m_recording = false;
        }

        void recordLoop();

        void capture();
        void update(PlayLayer* pl);

        void displayPreview() {
            m_texture.displayPreview();
        }

        SLRendererSettings m_settings;

        static SLRenderer* get() {
            static SLRenderer instance;
            return &instance;
        }

        float getDt() const {
            return 1.f / m_settings.m_fps;
        }
        bool isRecording() const {
            return m_recording;
        }

        // Moved off the legacy TTR renderer in 2.0. The render-complete popup
        // and the render HUD were both reading that renderer's fields, and it
        // has been unreachable since SLRenderer took over -- so neither ever
        // appeared. They read these now.
        struct LastRender {
            bool pending = false;
            bool success = false;
            std::string path;
            unsigned width = 0, height = 0, fps = 0;
            double duration = 0.0;
            uintmax_t fileSize = 0;
        } m_lastRender;

        int renderedFrames() const {
            return m_frameCount;
        }
        void publishRenderResult(bool success);
        float getTime() const {
            return m_time;
        }
        inline bool isFFmpegLoaded() const {
            return m_ffmpegLoaded;
        }

        void loadFFmpeg() {
            if (ff) {
                free(ff);
            }
            ff = (ff_t*)malloc(sizeof(ff_t));
            if (!ff)
                return;
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
        SL_AV_PTR(AVCodecContext) m_videoCodecCtx = { nullptr, SL_AV_LEAK(AVCodecContext) };
        AVFormatContext* m_formatCtx = nullptr;
        AVStream* m_videoStream = nullptr;

        const AVCodec* m_videoCodec = nullptr;

        SL_AV_PTR(AVFrame) m_frame = { nullptr, SL_AV_LEAK(AVFrame) };
        SL_AV_PTR(AVPacket) m_pkt = { nullptr, SL_AV_LEAK(AVPacket) };

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

} // namespace gucci

#endif

#ifndef SL_DSP_HPP
#define SL_DSP_HPP
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace gucci {

    // Silicate's single-producer/single-consumer ring for the audio preview.
    // The FMOD mixer (game thread, inside the render's pump) pushes; the
    // preview system's stream thread pops. A full ring drops the overflow so
    // the preview never falls further behind than its size.
    class AudioMonitorRing {
    public:
        void init(size_t capacity) {
            m_data.assign(capacity, 0.0f);
            m_capacity = capacity;
            clear();
        }
        void clear() {
            m_read.store(0, std::memory_order_relaxed);
            m_write.store(0, std::memory_order_relaxed);
        }
        void push(const float* src, size_t n) {
            if (m_capacity == 0)
                return;
            size_t const w = m_write.load(std::memory_order_relaxed);
            size_t const r = m_read.load(std::memory_order_acquire);
            size_t const space = m_capacity - (w - r);
            if (n > space)
                n = space;
            for (size_t i = 0; i < n; ++i)
                m_data[(w + i) % m_capacity] = src[i];
            m_write.store(w + n, std::memory_order_release);
        }
        size_t pop(float* dst, size_t n) {
            if (m_capacity == 0)
                return 0;
            size_t const r = m_read.load(std::memory_order_relaxed);
            size_t const w = m_write.load(std::memory_order_acquire);
            size_t const take = std::min(n, w - r);
            for (size_t i = 0; i < take; ++i)
                dst[i] = m_data[(r + i) % m_capacity];
            m_read.store(r + take, std::memory_order_release);
            return take;
        }

    private:
        std::vector<float> m_data;
        size_t m_capacity = 0;
        std::atomic<size_t> m_write{0};
        std::atomic<size_t> m_read{0};
    };

    class AudioRecorder {
    public:
        static AudioRecorder* get() {
            static AudioRecorder i;
            return &i;
        }
        static AudioRecorder* getMusic() {
            static AudioRecorder i;
            return &i;
        }
        static AudioRecorder* getSfx() {
            static AudioRecorder i;
            return &i;
        }
        static AudioRecorder* getFrameWindow() {
            static AudioRecorder i;
            return &i;
        }

        void init(FMOD::ChannelGroup* group = nullptr);
        void attach();
        void detach();
        void uninit();

        void haltWithData(float* data, unsigned int length);

        static FMOD_RESULT F_CALLBACK
        writeCallbackMain(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
        static FMOD_RESULT F_CALLBACK
        writeCallbackMusic(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
        static FMOD_RESULT F_CALLBACK
        writeCallbackSfx(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
        static FMOD_RESULT F_CALLBACK
        writeCallbackFrameWindow(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);

        bool m_attached = false;
        std::atomic_bool m_shouldUpdateFmod = false;

        FMOD::ChannelGroup* m_master = nullptr;

        double m_fmodTime = 0.0;
        double m_time = 0.0;
        uint32_t m_index = 0;
        int m_sampleRate = 0;
        int m_channels = 0;

        // What FMOD is ACTUALLY mixing, recorded from the DSP callback.
        // m_channels above is read once from getSoftwareFormat at init, which
        // happens BEFORE the output switches to non-realtime -- so the two can
        // disagree, and encoding against the wrong count corrupts the audio.
        // Ported from Silicate 2026-09-22; we had no equivalent.
        std::atomic<int> m_mixedChannels{0};
        void syncMixFormat();
        size_t m_lastCollectedLength = 0;

        std::vector<float> m_buffer;

        // Audio preview (Silicate's monitor). Only the combined recorder,
        // AudioRecorder::get(), runs one.
        void startMonitor();
        void stopMonitor();
        static FMOD_RESULT F_CALLBACK monitorReadCallback(FMOD_SOUND*, void* data,
                                                         unsigned int datalen);
        FMOD::System* m_monSystem = nullptr;
        FMOD::Sound* m_monSound = nullptr;
        FMOD::Channel* m_monChannel = nullptr;
        AudioMonitorRing m_monRing;
        std::atomic<float> m_monVolume{1.0f};

    private:
        FMOD::DSP* m_dsp = nullptr;
    };

    namespace AudioEngineRenderState {
        void enter(double musicVolume, double sfxVolume);
        void exit();

        void pump(float dt, bool split);
    } // namespace AudioEngineRenderState

} // namespace gucci

#endif

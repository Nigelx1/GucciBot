#ifndef SL_DSP_HPP
#define SL_DSP_HPP
// AudioRecorder — ported from Silicate (src/render/dsp.cpp). Taps GD's master
// audio via a FMOD DSP unit and accumulates samples into m_buffer. During a
// render FMOD is flipped to NOSOUND_NRT (silent non-realtime), so the DSP
// capture callback and the encode-drain both run on the MAIN thread — no audio-
// thread locking needed (matches upstream). Encode is driven per render tick
// from SLRenderer::update(), which drains m_buffer in 1024-sample chunks and
// calls SLRenderer::writeAudio.
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

class AudioRecorder {
   public:
    static AudioRecorder* get() {
        static AudioRecorder instance;
        return &instance;
    }

    void init();
    void attach(double musicVolume, double sfxVolume);
    void detach();
    void uninit();
    void unpause();

    void haltWithData(float* data, unsigned int length);

    static FMOD_RESULT F_CALLBACK writeCallback(FMOD_DSP_STATE* dspState,
                                                float* inBuffer,
                                                float* outBuffer,
                                                unsigned int length,
                                                int inChannels,
                                                int* outChannels);

    bool m_attached = false;
    std::atomic_bool m_shouldUpdateFmod = false;

    FMOD::ChannelGroup* m_master = nullptr;

    float m_previousMusicVolume = 0.0f;
    float m_previousSFXVolume = 0.0f;

    double m_fmodTime = 0.0;
    double m_time = 0.0;
    uint32_t m_index = 0;
    int m_sampleRate = 0;
    int m_channels = 0;
    size_t m_lastCollectedLength = 0;

    std::vector<float> m_buffer;

   private:
    FMOD::DSP* m_dsp = nullptr;
};

#endif  // SL_DSP_HPP

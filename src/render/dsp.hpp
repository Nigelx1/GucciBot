#ifndef SL_DSP_HPP
#define SL_DSP_HPP
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

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
    size_t m_lastCollectedLength = 0;

    std::vector<float> m_buffer;

private:
    FMOD::DSP* m_dsp = nullptr;
};

namespace AudioEngineRenderState {
    void enter(double musicVolume, double sfxVolume);
    void exit();

    void pump(float dt, bool split);
} // namespace AudioEngineRenderState

#endif

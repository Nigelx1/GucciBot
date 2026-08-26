#include "dsp.hpp"

#include "renderer.hpp"

#include <algorithm>
#include <cstring>

namespace gucci {

    static FMOD_RESULT captureInto(AudioRecorder* recorder,
                                   float* inBuffer,
                                   float* outBuffer,
                                   unsigned int length,
                                   int inChannels) {
        if (!recorder->m_shouldUpdateFmod) {
            return FMOD_OK;
        }

        recorder->m_lastCollectedLength = length;
        recorder->haltWithData(inBuffer, length * inChannels);

        std::memset(outBuffer, 0, length * inChannels * sizeof(float));

        return FMOD_OK;
    }

    FMOD_RESULT F_CALLBACK AudioRecorder::writeCallbackMain(FMOD_DSP_STATE*,
                                                            float* inBuffer,
                                                            float* outBuffer,
                                                            unsigned int length,
                                                            int inChannels,
                                                            int*) {
        return captureInto(AudioRecorder::get(), inBuffer, outBuffer, length, inChannels);
    }
    FMOD_RESULT F_CALLBACK AudioRecorder::writeCallbackMusic(FMOD_DSP_STATE*,
                                                             float* inBuffer,
                                                             float* outBuffer,
                                                             unsigned int length,
                                                             int inChannels,
                                                             int*) {
        return captureInto(AudioRecorder::getMusic(), inBuffer, outBuffer, length, inChannels);
    }
    FMOD_RESULT F_CALLBACK AudioRecorder::writeCallbackSfx(FMOD_DSP_STATE*,
                                                           float* inBuffer,
                                                           float* outBuffer,
                                                           unsigned int length,
                                                           int inChannels,
                                                           int*) {
        return captureInto(AudioRecorder::getSfx(), inBuffer, outBuffer, length, inChannels);
    }
    FMOD_RESULT F_CALLBACK AudioRecorder::writeCallbackFrameWindow(FMOD_DSP_STATE*,
                                                                   float* inBuffer,
                                                                   float* outBuffer,
                                                                   unsigned int length,
                                                                   int inChannels,
                                                                   int*) {
        return captureInto(
            AudioRecorder::getFrameWindow(), inBuffer, outBuffer, length, inChannels);
    }

    void AudioRecorder::haltWithData(float* data, unsigned int length) {
        m_buffer.insert(m_buffer.end(), data, data + length);

        for (size_t i = m_buffer.size() - length; i < m_buffer.size(); i++) {
            m_buffer[i] = std::clamp(m_buffer[i], -1.0f, 1.0f);
        }
    }

    void AudioRecorder::init(FMOD::ChannelGroup* group) {
        FMOD_DSP_DESCRIPTION desc = {};
        if (this == getMusic()) {
            strcpy_s(desc.name, "guccibot dsp (music)");
            desc.read = AudioRecorder::writeCallbackMusic;
        } else if (this == getSfx()) {
            strcpy_s(desc.name, "guccibot dsp (sfx)");
            desc.read = AudioRecorder::writeCallbackSfx;
        } else if (this == getFrameWindow()) {
            strcpy_s(desc.name, "guccibot dsp (fw)");
            desc.read = AudioRecorder::writeCallbackFrameWindow;
        } else {
            strcpy_s(desc.name, "guccibot dsp");
            desc.read = AudioRecorder::writeCallbackMain;
        }
        desc.version = 0x00020000;
        desc.numinputbuffers = 1;
        desc.numoutputbuffers = 1;
        desc.numparameters = 0;

        auto engine = FMODAudioEngine::get();
        FMOD::System* system = engine->m_system;

        if (group) {
            m_master = group;
        } else {
            system->getMasterChannelGroup(&m_master);
        }
        system->createDSP(&desc, &m_dsp);
        system->setDSPBufferSize(1024, 2);

        m_time = 0.0;
        m_fmodTime = 0.0;
        m_index = 0;
        m_buffer.clear();
    }

    void AudioRecorder::attach() {
        int numDsps;
        m_master->getNumDSPs(&numDsps);
        m_master->addDSP(numDsps, m_dsp);
        m_dsp->setMeteringEnabled(true, false);
        m_master->setPaused(false);

        auto engine = FMODAudioEngine::get();
        engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);

        m_attached = true;
    }

    void AudioRecorder::detach() {
        if (!m_attached)
            return;

        m_master->removeDSP(m_dsp);
        m_master->setPaused(false);

        m_attached = false;
    }

    void AudioRecorder::uninit() {
        if (m_dsp) {
            m_dsp->release();
            m_dsp = nullptr;
        }
    }

    namespace AudioEngineRenderState {
        static float s_previousMusicVolume = 0.0f;
        static float s_previousSFXVolume = 0.0f;
        static bool s_active = false;

        void enter(double musicVolume, double sfxVolume) {
            auto engine = FMODAudioEngine::get();
            s_previousMusicVolume = engine->getBackgroundMusicVolume();
            s_previousSFXVolume = engine->getEffectsVolume();
            engine->setEffectsVolume((float)sfxVolume);
            engine->setBackgroundMusicVolume((float)musicVolume);
            engine->m_system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
            s_active = true;
        }

        void exit() {
            if (!s_active)
                return;
            auto engine = FMODAudioEngine::get();
            engine->setEffectsVolume(s_previousSFXVolume);
            engine->setBackgroundMusicVolume(s_previousMusicVolume);
            engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
            s_active = false;
        }

        void pump(float dt, bool split) {
            auto engine = FMODAudioEngine::get();
            AudioRecorder::get()->m_shouldUpdateFmod = true;
            if (split) {
                AudioRecorder::getMusic()->m_shouldUpdateFmod = true;
                AudioRecorder::getSfx()->m_shouldUpdateFmod = true;
                AudioRecorder::getFrameWindow()->m_shouldUpdateFmod = true;
            }
            engine->update(dt);
            AudioRecorder::get()->m_shouldUpdateFmod = false;
            AudioRecorder::getMusic()->m_shouldUpdateFmod = false;
            AudioRecorder::getSfx()->m_shouldUpdateFmod = false;
            AudioRecorder::getFrameWindow()->m_shouldUpdateFmod = false;
        }
    } // namespace AudioEngineRenderState

} // namespace gucci

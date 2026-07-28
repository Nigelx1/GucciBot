#include "dsp.hpp"

#include "renderer.hpp"  // SLRenderer (unpause() reads getDt())

#include <algorithm>
#include <cstring>

// Ported from Silicate src/render/dsp.cpp. Captures GD's master mix into
// m_buffer via a FMOD DSP read callback; runs on the main thread under
// NOSOUND_NRT, so no locking (matches upstream).

FMOD_RESULT F_CALLBACK AudioRecorder::writeCallback(FMOD_DSP_STATE*,
                                                    float* inBuffer,
                                                    float* outBuffer,
                                                    unsigned int length,
                                                    int inChannels, int*) {
    AudioRecorder* recorder = AudioRecorder::get();

    if (!recorder->m_shouldUpdateFmod) {
        return FMOD_OK;
    }

    recorder->m_lastCollectedLength = length;
    recorder->haltWithData(inBuffer, length * inChannels);

    // NOSOUND_NRT output goes nowhere; zero the buffer for cleanliness.
    std::memset(outBuffer, 0, length * inChannels * sizeof(float));

    return FMOD_OK;
}

void AudioRecorder::haltWithData(float* data, unsigned int length) {
    m_buffer.insert(m_buffer.end(), data, data + length);

    // FMOD samples can overshoot [-1, 1] slightly; clamp to avoid clipping
    // artifacts after resampling.
    for (size_t i = m_buffer.size() - length; i < m_buffer.size(); i++) {
        m_buffer[i] = std::clamp(m_buffer[i], -1.0f, 1.0f);
    }
}

void AudioRecorder::init() {
    FMOD_DSP_DESCRIPTION desc = {};
    strcpy_s(desc.name, "guccibot dsp");
    desc.version = 0x00020000;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = AudioRecorder::writeCallback;
    desc.numparameters = 0;

    auto engine = FMODAudioEngine::get();
    FMOD::System* system = engine->m_system;
    system->getMasterChannelGroup(&m_master);
    system->createDSP(&desc, &m_dsp);
    system->setDSPBufferSize(1024, 2);

    m_time = 0.0;
    m_fmodTime = 0.0;
    m_index = 0;
    m_buffer.clear();
}

void AudioRecorder::attach(double musicVolume, double sfxVolume) {
    int numDsps;
    m_master->getNumDSPs(&numDsps);
    m_master->addDSP(numDsps, m_dsp);
    m_dsp->setMeteringEnabled(true, false);

    auto engine = FMODAudioEngine::get();

    m_previousMusicVolume = engine->getBackgroundMusicVolume();
    m_previousSFXVolume = engine->getEffectsVolume();
    m_master->setPaused(false);
    engine->m_system->getSoftwareFormat(&m_sampleRate, nullptr, &m_channels);

    engine->setEffectsVolume(sfxVolume);
    engine->setBackgroundMusicVolume(musicVolume);
    engine->m_system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);

    m_attached = true;
}

void AudioRecorder::detach() {
    if (!m_attached) return;

    m_master->removeDSP(m_dsp);
    m_master->setPaused(false);

    auto engine = FMODAudioEngine::get();
    engine->setEffectsVolume(m_previousSFXVolume);
    engine->setBackgroundMusicVolume(m_previousMusicVolume);
    engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);

    m_attached = false;
}

void AudioRecorder::uninit() {
    if (m_dsp) {
        m_dsp->release();
        m_dsp = nullptr;
    }
}

void AudioRecorder::unpause() {
    auto renderer = SLRenderer::get();
    auto engine = FMODAudioEngine::get();

    m_shouldUpdateFmod = true;
    engine->update(renderer->getDt());
    m_shouldUpdateFmod = false;
}

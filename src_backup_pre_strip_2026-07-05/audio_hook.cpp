// audio_hook.cpp — ported from Silicate (src/hooks/FMODAudioEngine.cpp).
// Two pieces:
//  (1) GB7AudioEngine Modify: gates FMODAudioEngine::update so during a render
//      FMOD only advances on our explicit per-frame tick (m_shouldUpdateFmod),
//      keeping audio locked to video frames; advances AudioRecorder::m_fmodTime.
//  (2) FMOD::System::update function hook: the audio encode driver. Each tick it
//      runs the original System::update enough times to produce this frame's
//      samples (the DSP callback captures them into AudioRecorder::m_buffer),
//      then drains that buffer in 1024-sample chunks and encodes each via
//      SLRenderer::writeAudio. A/V sync = m_fmodTime vs m_time.
// The original-call pattern mirrors the project's proven actionMgrHook
// (engine_updater.cpp): a static fn pointer to the address, installed via
// Mod::get()->hook.
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include "render/dsp.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <string>

using namespace geode::prelude;

static bool shouldUpdateAudio() {
    auto audio = AudioRecorder::get();
    if (!audio->m_attached) return true;
    if (!audio->m_shouldUpdateFmod) return false;
    return true;
}

// (1) Gate the game's normal per-frame FMOD update during a render.
struct GB7AudioEngine : Modify<GB7AudioEngine, FMODAudioEngine> {
    void update(float dt) {
        if (!shouldUpdateAudio()) return;
        AudioRecorder::get()->m_fmodTime += dt;
        FMODAudioEngine::update(dt);
    }
};

// (2) FMOD::System::update hook — drives capture + encode.
static void (*fmodSystemUpdateOrig)(FMOD::System*) = nullptr;

static void fmodSystemUpdateHook(FMOD::System* self) {
    if (!shouldUpdateAudio()) {
        return;  // suppress the game's stray update during a render
    }

    auto audio = AudioRecorder::get();
    if (!audio->m_attached) {
        if (fmodSystemUpdateOrig) fmodSystemUpdateOrig(self);
        return;
    }

    auto renderer = SLRenderer::get();

    unsigned int bufferLength;
    int bufferCount;
    self->getDSPBufferSize(&bufferLength, &bufferCount);

    const double requiredDt = std::max(audio->m_fmodTime - audio->m_time, 0.0);
    const int requiredSamples =
        static_cast<int>(requiredDt * static_cast<double>(audio->m_sampleRate));

    int processedSamples = 0;
    while (requiredSamples > processedSamples) {
        if (fmodSystemUpdateOrig) fmodSystemUpdateOrig(self);
        processedSamples += static_cast<int>(bufferLength);
    }

    const unsigned int frameSize = 1024;
    const size_t totalFrameSize =
        frameSize * static_cast<size_t>(audio->m_channels);

    while (audio->m_buffer.size() >= totalFrameSize) {
        uint64_t pts = static_cast<uint64_t>(audio->m_index) * frameSize;

        auto result = renderer->writeAudio(audio->m_buffer, pts);
        if (result.isErr()) {
            geode::log::error("[GucciBot] Failed to write audio, stopping render");
            renderer->signalStop();
            return;
        }

        audio->m_buffer.erase(audio->m_buffer.begin(),
                              audio->m_buffer.begin() + totalFrameSize);
        audio->m_time =
            static_cast<double>(audio->m_index++) *
            (static_cast<double>(frameSize) /
             static_cast<double>(audio->m_sampleRate));
    }
}

$execute {
    auto addr = reinterpret_cast<void*>(
        geode::addresser::getNonVirtual(&FMOD::System::update));
    fmodSystemUpdateOrig = reinterpret_cast<void (*)(FMOD::System*)>(addr);
    auto res = Mod::get()->hook(addr, &fmodSystemUpdateHook,
                                "FMOD::System::update",
                                tulip::hook::TulipConvention::Stdcall);
    if (res.isErr()) {
        geode::log::error("[GucciBot] Failed to hook FMOD::System::update: {}",
                          res.unwrapErr());
    }
}

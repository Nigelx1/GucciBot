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

struct GB7AudioEngine : Modify<GB7AudioEngine, FMODAudioEngine> {
    void update(float dt) {
        if (!shouldUpdateAudio()) return;
        AudioRecorder::get()->m_fmodTime += dt;
        FMODAudioEngine::update(dt);
    }
};

static void (*fmodSystemUpdateOrig)(FMOD::System*) = nullptr;

// Drains one AudioRecorder's accumulated buffer into the given SLRenderer
// audio track, exactly like the pre-split single-track logic did -- each
// recorder keeps its own m_index/m_time (independent pts bookkeeping), so
// this is safe to call for however many recorders are active without them
// interfering with each other.
static bool drainRecorderIntoTrack(SLRenderer* renderer, AudioRecorder* audio, int trackIndex) {
    const unsigned int frameSize = 1024;
    const size_t totalFrameSize = frameSize * static_cast<size_t>(audio->m_channels);

    while (audio->m_buffer.size() >= totalFrameSize) {
        uint64_t pts = static_cast<uint64_t>(audio->m_index) * frameSize;

        auto result = renderer->writeAudio(audio->m_buffer, pts, trackIndex);
        if (result.isErr()) {
            geode::log::error("[GucciBot] Failed to write audio (track {}), stopping render", trackIndex);
            renderer->signalStop();
            return false;
        }

        audio->m_buffer.erase(audio->m_buffer.begin(),
                              audio->m_buffer.begin() + totalFrameSize);
        audio->m_time =
            static_cast<double>(audio->m_index++) *
            (static_cast<double>(frameSize) /
             static_cast<double>(audio->m_sampleRate));
    }
    return true;
}

static void fmodSystemUpdateHook(FMOD::System* self) {
    if (!shouldUpdateAudio()) {
        return;
    }

    // get() (combined/master) is always attached now, in both modes -- see
    // renderer.cpp's start(), which always calls get()->init()/attach()
    // regardless of split, and additionally attaches getMusic()/getSfx()/
    // getFrameWindow() when split. So get() is both the timing/gating
    // reference AND a real capturing recorder (track 0) in every case,
    // same as before any split-mode work existed.
    auto audio = AudioRecorder::get();
    if (!audio->m_attached) {
        if (fmodSystemUpdateOrig) fmodSystemUpdateOrig(self);
        return;
    }

    auto renderer = SLRenderer::get();
    bool split = renderer->m_settings.m_splitAudioTracks;

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

    // Unchanged from before any split-mode work: drainRecorderIntoTrack
    // advances audio's (get()'s) own m_time/m_index internally as it
    // drains, which is what keeps requiredDt correct next call -- true in
    // both modes now, since get() always has a real buffer to drain.
    if (!drainRecorderIntoTrack(renderer, audio, 0)) return;
    if (!split) return;

    if (!drainRecorderIntoTrack(renderer, AudioRecorder::getMusic(), 1)) return;
    if (!drainRecorderIntoTrack(renderer, AudioRecorder::getSfx(), 2)) return;
    if (!drainRecorderIntoTrack(renderer, AudioRecorder::getFrameWindow(), 3)) return;
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

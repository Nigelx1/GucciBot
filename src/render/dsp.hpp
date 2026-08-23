#ifndef SL_DSP_HPP
#define SL_DSP_HPP
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

// Juice/Nigel, 2026-08-23: render audio can now be captured as either one
// combined track (the original behavior -- get(), hooked to FMOD's master
// channel group) or three separate ones when SLRendererSettings::m_splitAudioTracks
// is on: getMusic()/getSfx()/getFrameWindow(), each hooked to its OWN FMOD
// channel group instead of master, so each only ever sees its own slice of
// the mix. Deliberately NOT one generic multi-instance class using FMOD's
// DSP userdata/plugindata to tell instances apart in a shared callback --
// that behavior isn't nailed down with confidence, so each named instance
// gets its own small, separate static callback instead, exactly mirroring
// the original single-instance pattern that's already proven to work. More
// repetitive, but nothing here depends on unverified FMOD behavior.
class AudioRecorder {
   public:
    static AudioRecorder* get()           { static AudioRecorder i; return &i; } // combined-track instance (master group)
    static AudioRecorder* getMusic()      { static AudioRecorder i; return &i; } // split-mode: music only
    static AudioRecorder* getSfx()        { static AudioRecorder i; return &i; } // split-mode: level/UI SFX only
    static AudioRecorder* getFrameWindow(){ static AudioRecorder i; return &i; } // split-mode: frame-window cues only

    // group == nullptr means "fetch FMOD's master channel group internally"
    // (get()'s original behavior, unchanged). The three split-mode
    // instances are always initialized with an explicit group instead.
    void init(FMOD::ChannelGroup* group = nullptr);
    // Hooks this instance's DSP onto its own group and captures the
    // current sample rate/channel count. Does NOT touch global engine
    // state (music/SFX volume, output mode) -- callers that need that
    // (SLRenderer::start(), for the ONE time it's actually needed) handle
    // it separately so attaching 1 or 3 instances doesn't fight over the
    // same global settings.
    void attach();
    void detach();
    void uninit();

    void haltWithData(float* data, unsigned int length);

    static FMOD_RESULT F_CALLBACK writeCallbackMain(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
    static FMOD_RESULT F_CALLBACK writeCallbackMusic(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
    static FMOD_RESULT F_CALLBACK writeCallbackSfx(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);
    static FMOD_RESULT F_CALLBACK writeCallbackFrameWindow(FMOD_DSP_STATE*, float*, float*, unsigned int, int, int*);

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

// Global engine-level setup shared across however many AudioRecorder
// instances are attached at once (1 combined-only, or 4 when split: get()
// PLUS getMusic()/getSfx()/getFrameWindow()) -- volume save/restore and the
// NRT output-mode switch are process-global FMOD state, not per-channel-
// group, so they're handled exactly once regardless of split mode instead
// of living inside AudioRecorder::attach() itself.
namespace AudioEngineRenderState {
    void enter(double musicVolume, double sfxVolume);
    void exit();

    // Replaces the old per-instance AudioRecorder::unpause(). get() is
    // always attached and always the timing reference for FMOD's update-
    // pump throttle (m_fmodTime/m_time bookkeeping) -- in split mode it's
    // ALSO track 0 (the original combined mix), not merely a bookkeeping
    // stand-in, since Juice wanted split mode to add 3 isolated tracks
    // alongside the combined one rather than replace it. split=true
    // additionally arms getMusic()/getSfx()/getFrameWindow()'s capture gate
    // for this same pumped update, so all four see the same underlying mix
    // tick instead of drifting apart.
    void pump(float dt, bool split);
}

#endif

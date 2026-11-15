#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include "core/standdown.hpp"
#include "render/dsp.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace gucci {

    static bool shouldUpdateAudio() {
        auto audio = AudioRecorder::get();
        if (!audio->m_attached)
            return true;
        if (!audio->m_shouldUpdateFmod)
            return false;
        return true;
    }

    // Splits the level's own SFX-trigger audio from GD's gameplay sound while
    // rendering, so a showcase can keep the sound a creator built into the
    // level and drop the death/orb/pad noise the macro makes.
    //
    // The split is by call path, which is how GD itself separates them:
    // gameplay sound goes through the plain playEffect overloads, while an SFX
    // trigger calls playEffectAdvanced -- it is the only one carrying an
    // effect id, channel and SFX group, which is exactly what a trigger
    // configures. No filename matching, which would break the moment a creator
    // used a built-in sound in a trigger.
    //
    // Only active while rendering. Outside a render both return true and the
    // game sounds exactly as it always does.
    static bool renderAudioActive() {
        auto* r = SLRenderer::get();
        return r && r->isRecording();
    }

    static float gameplaySfxScale() {
        if (!renderAudioActive())
            return 1.f;
        return (float)SLRenderer::get()->m_settings.m_sfxVolume;
    }

    static float triggerSfxScale() {
        if (!renderAudioActive())
            return 1.f;
        return (float)SLRenderer::get()->m_settings.m_triggerSfxVolume;
    }

    // GD's playEffect overloads hand the sound on to playEffectAdvanced (the
    // death sound, explode_11.ogg, goes playEffect -> playEffectAdvanced), so
    // the hook below sees a gameplay sound twice (seen in a render's log,
    // 2026-11-15). Before these counters the second hook scaled it again by
    // the TRIGGER volume: a render with sound effects at 100% and trigger
    // sounds at 0% had no death sound, and 50%/50% would play it at 25%.
    // A gameplay sound is scaled once, by the first overload that carries a
    // volume. Game thread only, like every FMODAudioEngine call.
    static int s_inGameplayEffect = 0;  // inside a gameplay playEffect
    static int s_gameplayScaled = 0;    // ... one that applied the SFX volume
    // A sound that isn't loaded yet is queued (queuePlayEffect, from inside
    // playEffectAdvanced) with the volume it was given -- already scaled --
    // and played once it has loaded by a second playEffectAdvanced call from
    // GD's queue. Scaling that one again played the first use of every SFX
    // trigger's sound at the trigger volume squared (measured: 0.25 instead
    // of 0.5 at 50%, its later uses 0.50).
    static int s_inQueuedReplay = 0;

    struct GameplayEffectScope {
        bool scaled;
        explicit GameplayEffectScope(bool scaledHere) : scaled(scaledHere) {
            ++s_inGameplayEffect;
            if (scaled)
                ++s_gameplayScaled;
        }
        ~GameplayEffectScope() {
            --s_inGameplayEffect;
            if (scaled)
                --s_gameplayScaled;
        }
    };

    struct GB7AudioEngine : Modify<GB7AudioEngine, FMODAudioEngine> {
        // Gameplay sound. The one-argument overload carries no volume to
        // scale, so at zero the call is dropped rather than played silently.
        int playEffect(gd::string path) {
            if (gameplaySfxScale() <= 0.f)
                return 0;
            GameplayEffectScope scope(false);
            return FMODAudioEngine::playEffect(path);
        }

        int playEffect(gd::string path, float speed, float unknown, float volume) {
            float const scale = gameplaySfxScale();
            if (scale <= 0.f)
                return 0;
            bool const scaleHere = s_gameplayScaled == 0;
            GameplayEffectScope scope(scaleHere);
            return FMODAudioEngine::playEffect(path, speed, unknown,
                                               scaleHere ? volume * scale : volume);
        }

        // SFX triggers -- and, called from inside the playEffect overloads
        // above, gameplay sound on its way through (see s_inGameplayEffect).
        int playEffectAdvanced(gd::string path, float speed, float unknown, float volume,
                               float pitch, bool fastFourierTransform, bool reverb,
                               int startMillis, int endMillis, int fadeIn, int fadeOut,
                               bool loopEnabled, int effectID, bool override, bool noPreload,
                               int channelID, int uniqueID, float minInterval, int sfxGroup) {
            float scale = triggerSfxScale();
            if (s_inQueuedReplay > 0)
                scale = 1.f;
            else if (s_inGameplayEffect > 0)
                scale = s_gameplayScaled > 0 ? 1.f : gameplaySfxScale();
            if (scale <= 0.f)
                return 0;
            return FMODAudioEngine::playEffectAdvanced(
                path, speed, unknown, volume * scale, pitch, fastFourierTransform, reverb,
                startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID, override,
                noPreload, channelID, uniqueID, minInterval, sfxGroup);
        }

        // GD's queue playing sounds that have finished loading (see
        // s_inQueuedReplay). Both entry points, as either may be the one
        // that plays them.
        void updateQueuedEffects() {
            ++s_inQueuedReplay;
            FMODAudioEngine::updateQueuedEffects();
            --s_inQueuedReplay;
        }
        void queuedEffectFinishedLoading(gd::string path) {
            ++s_inQueuedReplay;
            FMODAudioEngine::queuedEffectFinishedLoading(path);
            --s_inQueuedReplay;
        }

        void update(float dt) {
            if (!shouldUpdateAudio())
                return;
            AudioRecorder::get()->m_fmodTime += dt;
            FMODAudioEngine::update(dt);
        }
    };

    static void (*fmodSystemUpdateOrig)(FMOD::System*) = nullptr;

    static bool drainRecorderIntoTrack(SLRenderer* renderer, AudioRecorder* audio, int trackIndex) {
        audio->syncMixFormat();

        const unsigned int frameSize = 1024;
        const size_t totalFrameSize = frameSize * static_cast<size_t>(audio->m_channels);

        while (audio->m_buffer.size() >= totalFrameSize) {
            uint64_t pts = static_cast<uint64_t>(audio->m_index) * frameSize;

            auto result = renderer->writeAudio(audio->m_buffer, pts, trackIndex);
            if (result.isErr()) {
                renderer->signalStop(
                    fmt::format("writing the audio failed (track {}): {}", trackIndex, result.unwrapErr()),
                    true);
                return false;
            }

            audio->m_buffer.erase(audio->m_buffer.begin(),
                                  audio->m_buffer.begin() + totalFrameSize);
            audio->m_time =
                static_cast<double>(audio->m_index++) *
                (static_cast<double>(frameSize) / static_cast<double>(audio->m_sampleRate));
        }
        return true;
    }

    static void fmodSystemUpdateHook(FMOD::System* self) {
        if (!shouldUpdateAudio()) {
            return;
        }

        auto audio = AudioRecorder::get();
        if (!audio->m_attached) {
            if (fmodSystemUpdateOrig)
                fmodSystemUpdateOrig(self);
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
            if (fmodSystemUpdateOrig)
                fmodSystemUpdateOrig(self);
            processedSamples += static_cast<int>(bufferLength);
        }

        if (!drainRecorderIntoTrack(renderer, audio, 0))
            return;
        if (!split)
            return;

        if (!drainRecorderIntoTrack(renderer, AudioRecorder::getMusic(), 1))
            return;
        if (!drainRecorderIntoTrack(renderer, AudioRecorder::getSfx(), 2))
            return;
        if (!drainRecorderIntoTrack(renderer, AudioRecorder::getFrameWindow(), 3))
            return;
    }

    $execute {
        // Standing down for another bot: no address hooks (core/standdown.hpp).
        if (standDownBot())
            return;
        auto addr = reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::update));
        fmodSystemUpdateOrig = reinterpret_cast<void (*)(FMOD::System*)>(addr);
        auto res = Mod::get()->hook(addr,
                                    &fmodSystemUpdateHook,
                                    "FMOD::System::update",
                                    tulip::hook::TulipConvention::Stdcall);
        if (res.isErr()) {
            geode::log::error("[GucciBot] Failed to hook FMOD::System::update: {}",
                              res.unwrapErr());
        }
    }

} // namespace gucci

// Detailed audio settings (INTERFACES.md §7, owner wave4/audio): converter quality, pitch range, input low cut,
// automatic level, limiter, preset crossfade, soundboard voices / fades / ducking times, monitor soundboard switch.
#include "App/AppController.h"

#include "Dsp/PitchDetector.h"

namespace koe
{
void AppController::applyAudioSettings (const Settings* before)
{
    // every setter is a cheap atomic store that takes effect smoothly on the audio thread, so all of them are applied
    // every time. Only the converter quality does real work (a new converter set, built here and crossfaded in by the
    // audio thread), and setConverterQuality() returns at once when the quality is unchanged.
    juce::ignoreUnused (before);
    const auto& s = settings;
    processor.setConverterQuality (s.converterQuality);
    dsp::setVoicePitchRange (s.pitchMinHz, s.pitchMaxHz);
    processor.setHighPass (s.highPassOn, s.highPassHz);
    processor.setAgc (s.agcOn, s.agcTargetDb, s.agcMaxGainDb);
    processor.setLimiter (s.limiterCeilingDb, s.limiterReleaseMs);
    processor.setChainCrossfadeMs (s.presetCrossfadeMs);
    soundboard->setMaxVoices (s.soundboardMaxVoices);
    soundboard->setFadeMs (s.soundFadeMs);
    soundboard->setDuckTimes (s.duckAttackMs, s.duckReleaseMs);
    soundboard->setMonitorIncludesSounds (s.monitorIncludeSoundboard);
}
} // namespace koe

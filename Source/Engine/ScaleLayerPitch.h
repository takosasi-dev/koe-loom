#pragma once

#include "Dsp/PitchDetector.h"
#include "Engine/VoiceProcessor.h"

namespace koe
{
/**
    Pitch provider for the scale-locked layers (F-02-11, spec §5.7): one detector for all layers, fed with
    the voice before the shifters. layerSemitones() snaps the detected note to the nearest note of the
    key's major / natural minor scale, moves `degree` scale steps (+2 = a third up, +7 = an octave up)
    and returns the distance from the detected pitch, so the layer lands exactly on the scale note.
    NaN while unvoiced (E-28: the processor fades the layer out).
    prepare() allocates (message thread, before setScaleLayerPitch()); the rest runs on the audio thread.
*/
class ScaleLayerPitch final : public IScaleLayerPitch
{
public:
    void prepare (double sampleRate, int maxBlock);
    void reset() { detector.reset(); }

    void analyse (const float* voice, int numSamples) override { detector.process (voice, numSamples); }
    float layerSemitones (int key, bool minor, int degree) const override;

    /** The detected fundamental (Hz, 0 = unvoiced), for tests and diagnostics. */
    float getFrequencyHz() const noexcept { return detector.getFrequencyHz(); }

private:
    dsp::PitchDetector detector;
};
} // namespace koe

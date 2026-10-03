#include "Engine/ScaleLayerPitch.h"

#include <cmath>

namespace koe
{
void ScaleLayerPitch::prepare (double sampleRate, int maxBlock)
{
    (void) maxBlock; // the detector takes any block length
    detector.prepare (sampleRate);
}

float ScaleLayerPitch::layerSemitones (int key, bool minor, int degree) const
{
    const float f = detector.getFrequencyHz();
    if (f <= 0.0f) return std::nanf ("");
    return dsp::scaleShiftSemitones (dsp::hzToMidi (f), key, minor ? dsp::Scale::minor : dsp::Scale::major, degree);
}
} // namespace koe

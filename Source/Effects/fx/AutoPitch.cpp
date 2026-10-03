#include "Dsp/Building.h"
#include "Dsp/IVoiceShifter.h"
#include "Dsp/PitchDetector.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// autopitch (koeloom_effects.md §5.2, F-04-20). Its own MPM detector (spec §5.7) listens to the slot input.
// Every 128 samples the detected note is snapped to the nearest note of key / scale (minor = natural minor),
// the correction (semitones, times strength) moves towards that with the time constant retuneMs (0 = at
// once), and the phase-vocoder shifter (the voice converter, F-04-12) applies it. Unvoiced (silence, noise,
// consonants): the correction heads back to 0, so the voice passes uncorrected (E-28).
// An octave error of the detector does not matter here: the snap distance is the same one octave away.
// Output = the shifter output (no mix parameter); latency = the shifter's, 1535 samples (32 ms).

namespace koe
{
namespace
{
class AutoPitchEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int maxBlockSize) override
    {
        sr = sampleRate;
        shifter = createPhaseVocoderShifter();
        shifter->prepare (sr, maxBlockSize);
        detector.prepare (sr);
        hop = detector.getHopSamples();
        in.assign (size_t (std::max (1, maxBlockSize)), 0.0f);
    }

    void reset() override
    {
        detector.reset();
        correction = 0.0f;
        shifter->setPitchSemitones (0.0f);
        shifter->setFormantSemitones (0.0f);
        shifter->reset();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: key = int (v); break;
            case 1: scale = static_cast<dsp::Scale> (int (v)); break;
            case 2: retuneMs = v; break;
            case 3: strength = v; break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        std::copy (x, x + n, in.begin());
        detector.setRange (dsp::gVoicePitchMinHz.load (std::memory_order_relaxed), dsp::gVoicePitchMaxHz.load (std::memory_order_relaxed)); // S-03 詳細
        for (int pos = 0; pos < n; pos += hop)
        {
            const int len = std::min (hop, n - pos);
            detector.process (in.data() + pos, len);
            float want = 0.0f;
            if (detector.isVoiced())
                want = strength * dsp::scaleShiftSemitones (dsp::hzToMidi (detector.getFrequencyHz()), key, scale);
            const float a = retuneMs <= 0.0f ? 0.0f : std::exp (-float (len) / (float (sr) * retuneMs * 0.001f));
            correction = want + a * (correction - want);
            shifter->setPitchSemitones (correction);
            shifter->process (in.data() + pos, x + pos, len);
        }
    }

    int getLatencySamples() const override { return shifter != nullptr ? shifter->getLatencySamples() : 0; }

private:
    double sr = 48000.0;
    std::unique_ptr<IVoiceShifter> shifter;
    dsp::PitchDetector detector;
    std::vector<float> in;
    int hop = 128, key = 0;
    dsp::Scale scale = dsp::Scale::chromatic;
    float retuneMs = 50.0f, strength = 1.0f, correction = 0.0f;
};
} // namespace

KOE_REGISTER_EFFECT ("autopitch", AutoPitchEffect)
} // namespace koe

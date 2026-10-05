#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// octaver (wave10/fx, INTERFACES.md §12.3): adds the voice one and two octaves down without a pitch shifter, the way
// an analog octave pedal does it. A 4th-order low-pass leaves mostly the fundamental; a zero-crossing detector with
// hysteresis (armed below -25 % of that signal's envelope, fires at the next rising zero crossing) finds one edge per
// period; a flip-flop toggles on each edge (a square at f/2) and a second one on the first's rising edges (f/4).
// The low-pass follows the pitch: at every edge its cutoff moves 30 % of the way to 1.25 x the measured frequency
// (70-450 Hz, starts at 260 Hz), so a strong second harmonic (vowels with a low first formant) cannot add crossings:
// on the synthetic voice, which hardly has a fundamental, that took the octave tracking from 53 % (a fixed 260 Hz) to
// 97 % of the voiced 60 ms windows (NewFxTests). The sub voices are the low-passed voice times those squares: their
// period is 2 (4) times the voice's, they are as loud as its fundamental and silent when it is (no envelope follower,
// no gate), and the flips happen at zero crossings, so they do not click. They go through a 30 Hz high-pass (the
// product can carry DC) and the tone low-pass, and are added to the dry voice. Latency 0.

namespace koe
{
namespace
{
constexpr float kTrackStartHz = 260.0f;
constexpr float kSubGain = 2.0f; // the fundamental is quieter than the whole voice; brings level 1 near the voice's loudness

class OctaverEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        minPeriod = int (sr / 600.0);
        maxPeriod = int (sr / 50.0);
        dcCut.setHighpass (sr, 30.0f);
        env.setTimes (sr, 1.0f, 30.0f);
        sub1.prepare (sr, 30.0f);
        sub2.prepare (sr, 30.0f);
        dry.prepare (sr, 30.0f);
        toneCoeff32 = std::pow (dsp::onePoleCoeff (30.0f, sr), 32.0f);
    }

    void reset() override
    {
        sub1.snap (sub1.target);
        sub2.snap (sub2.target);
        dry.snap (dry.target);
        trackHz = kTrackStartHz;
        track1.setLowpass (sr, trackHz);
        track2.setLowpass (sr, trackHz);
        track1.reset();
        track2.reset();
        sinceEdge = 0;
        dcCut.reset();
        env.reset();
        toneS = toneTarget;
        tone.reset();
        tone.setLowpass (sr, toneS);
        sinceTone = 0;
        armed = false;
        sq1 = sq2 = 1.0f;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: sub1.setTarget (v); break;
            case 1: sub2.setTarget (v); break;
            case 2: dry.setTarget (v); break;
            case 3: toneTarget = v; break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            if (sinceTone-- <= 0)
            {
                toneS = toneTarget + toneCoeff32 * (toneS - toneTarget);
                tone.setLowpass (sr, toneS);
                sinceTone = 31;
            }
            const float in = x[i];
            const float f = track2.process (track1.process (in));
            const float threshold = 0.25f * env.process (f);
            ++sinceEdge;
            if (f < -threshold) armed = true;
            else if (armed && f >= 0.0f)
            {
                armed = false;
                sq1 = -sq1;
                if (sq1 > 0.0f) sq2 = -sq2;
                if (sinceEdge >= minPeriod && sinceEdge <= maxPeriod)
                {
                    trackHz += 0.3f * (std::clamp (1.25f * float (sr) / float (sinceEdge), 70.0f, 450.0f) - trackHz);
                    track1.setLowpass (sr, trackHz);
                    track2.setLowpass (sr, trackHz);
                }
                sinceEdge = 0;
            }
            const float subs = f * (sub1.next() * sq1 + sub2.next() * sq2);
            x[i] = dry.next() * in + kSubGain * tone.process (dcCut.process (subs));
        }
    }

private:
    double sr = 48000.0;
    dsp::Biquad track1, track2, dcCut, tone;
    dsp::EnvelopeFollower env;
    dsp::Ramp sub1, sub2, dry;
    float toneTarget = 700.0f, toneS = 700.0f, toneCoeff32 = 0.0f, trackHz = kTrackStartHz;
    int sinceTone = 0, sinceEdge = 0, minPeriod = 80, maxPeriod = 960;
    bool armed = false;
    float sq1 = 1.0f, sq2 = 1.0f;
};
} // namespace

KOE_REGISTER_EFFECT ("octaver", OctaverEffect)
} // namespace koe

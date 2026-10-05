#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>

// freqshift (wave10/fx, INTERFACES.md §12.3): every component of the voice moves by the same number of Hz, so the
// harmonics stop being a harmonic series (metallic, inharmonic). Single-sideband modulation: an analytic signal from
// two chains of 4 all-passes in z^-2 whose outputs stay 90 degrees apart (O. Niemitalo's coefficients; within
// 0.7 degrees = the mirror image below -44 dB from 30 Hz to 22 kHz at 48 kHz), times a complex oscillator.
// The dry of the mix is the in-phase all-pass output, so mixing never comb-filters against the shifted voice
// (shift 0 Hz = exactly the dry). The shift glides 30 ms and the oscillator phase is continuous; the optional
// wobble swings the shift by depth Hz at rate Hz. Latency 0 (all-passes only).

namespace koe
{
namespace
{
/** 4 sections H(z) = (c - z^-2) / (1 - c z^-2), c = a^2. */
struct AllpassChain
{
    std::array<float, 4> c {}, x1 {}, x2 {}, y1 {}, y2 {};

    void setCoefficients (const std::array<double, 4>& a) noexcept
    {
        for (size_t k = 0; k < 4; ++k) c[k] = float (a[k] * a[k]);
    }
    void reset() noexcept { x1.fill (0.0f); x2.fill (0.0f); y1.fill (0.0f); y2.fill (0.0f); }
    float process (float x) noexcept
    {
        for (size_t k = 0; k < 4; ++k)
        {
            const float y = c[k] * (x + y2[k]) - x2[k];
            x2[k] = x1[k]; x1[k] = x;
            y2[k] = y1[k]; y1[k] = y;
            x = y;
        }
        return x;
    }
};

class FreqShifterEffect final : public IEffect
{
public:
    FreqShifterEffect()
    {
        // the second chain lags the first by 90 degrees (plus one sample of delay, below)
        inPhase.setCoefficients ({ 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 });
        quadrature.setCoefficients ({ 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 });
    }

    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        shift.prepare (sr, 30.0f);
        depth.prepare (sr, 30.0f);
        mix.prepare (sr, 30.0f);
    }

    void reset() override
    {
        shift.snap (shift.target);
        depth.snap (depth.target);
        mix.snap (mix.target);
        inPhase.reset();
        quadrature.reset();
        prevIn = 0.0f;
        phase = 0.0;
        lfo.reset();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: shift.setTarget (v); break;
            case 1: depth.setTarget (v); break;
            case 2: lfo.setRate (sr, v); break;
            case 3: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            const float re = inPhase.process (in);
            const float im = quadrature.process (prevIn);
            prevIn = in;

            const float a = dsp::kTwoPi * float (phase);
            const float shifted = re * std::cos (a) - im * std::sin (a); // Re{(re + j im) e^(j a)}: up by the shift
            const float m = mix.next();
            x[i] = re + m * (shifted - re);

            const float hz = shift.next() + depth.next() * lfo.nextSine();
            phase += double (hz) / sr;
            phase -= std::floor (phase);
        }
    }

private:
    double sr = 48000.0, phase = 0.0;
    AllpassChain inPhase, quadrature;
    float prevIn = 0.0f;
    dsp::Ramp shift, depth, mix;
    dsp::Lfo lfo;
};
} // namespace

KOE_REGISTER_EFFECT ("freqshift", FreqShifterEffect)
} // namespace koe

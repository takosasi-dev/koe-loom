#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// noise (koeloom_effects.md §3, §5.1). Adds one of five noises. Every generator is scaled to unit RMS
// (measured once in prepare() from a fixed seed), so levelDb is the noise RMS in dBFS before the tone
// low-pass. followVoice blends a constant level (0) with a level proportional to the input RMS (1;
// -20 dBFS input RMS = levelDb, capped at +6 dB). Changing `kind` crossfades the two generators (30 ms).
//   white   flat
//   pink    -3 dB/oct (Kellet's 7-pole filter)
//   hiss    tape hiss: white, high-passed at 1.2 kHz with a +4 dB shelf at 7 kHz
//   crackle vinyl: sparse heavy-tailed clicks with fast decays over a faint surface noise
//   static  radio: band-limited hiss whose level wanders, plus frequent sharp clicks

namespace koe
{
namespace
{
constexpr int kKinds = 5;

struct NoiseGen
{
    void prepare (double sr)
    {
        hissHp.setHighpass (sr, 1200.0f, 0.5f);
        hissShelf.setHighShelf (sr, 7000.0f, 4.0f);
        crackleHp.setHighpass (sr, 300.0f);
        staticBp1.setHighpass (sr, 700.0f);
        staticBp2.setLowpass (sr, 6500.0f);
        clickDecay = std::exp (-1.0f / float (sr * 0.0004));
        popDecay = std::exp (-1.0f / float (sr * 0.0012));
        wanderCoeff = std::exp (-1.0f / float (sr * 0.06));
        crackleRate = float (14.0 / sr);
        staticRate = float (30.0 / sr);
    }

    void reset()
    {
        for (auto& b : pink) b = 0.0f;
        hissHp.reset(); hissShelf.reset(); crackleHp.reset(); staticBp1.reset(); staticBp2.reset();
        click = pop = wander = 0.0f;
        wanderTarget = 1.0f;
    }

    float next (int kind, dsp::Rng& rng)
    {
        const float w = rng.nextBipolar() * 1.7320508f; // unit RMS
        switch (kind)
        {
            case 0: return w;
            case 1: return pinkOf (w);
            case 2: return hissShelf.process (hissHp.process (w));
            case 3:
            {
                if (rng.nextFloat01() < crackleRate)
                {
                    const float u = rng.nextFloat01();
                    const float a = 0.15f + 0.85f * u * u * u * u; // mostly small, occasionally loud
                    if (rng.nextFloat01() < 0.15f) pop += (rng.nextBipolar() > 0 ? a : -a) * 0.8f;
                    else click += a;
                }
                click *= clickDecay;
                pop *= popDecay;
                const float surface = pinkOf (w) * 0.04f;
                return crackleHp.process (click * w + pop + surface);
            }
            default:
            {
                if (rng.nextFloat01() < 0.0005f) wanderTarget = 0.45f + 0.55f * rng.nextFloat01();
                wander = wanderTarget + wanderCoeff * (wander - wanderTarget);
                if (rng.nextFloat01() < staticRate) click += 0.4f + 1.6f * rng.nextFloat01();
                click *= clickDecay;
                const float hiss = staticBp2.process (staticBp1.process (w));
                return hiss * wander + click * rng.nextBipolar() * 2.0f;
            }
        }
    }

    float pinkOf (float w) noexcept
    {
        pink[0] = 0.99886f * pink[0] + w * 0.0555179f;
        pink[1] = 0.99332f * pink[1] + w * 0.0750759f;
        pink[2] = 0.96900f * pink[2] + w * 0.1538520f;
        pink[3] = 0.86650f * pink[3] + w * 0.3104856f;
        pink[4] = 0.55000f * pink[4] + w * 0.5329522f;
        pink[5] = -0.7616f * pink[5] - w * 0.0168980f;
        const float out = pink[0] + pink[1] + pink[2] + pink[3] + pink[4] + pink[5] + pink[6] + w * 0.5362f;
        pink[6] = w * 0.115926f;
        return out;
    }

    float pink[7] {};
    dsp::Biquad hissHp, hissShelf, crackleHp, staticBp1, staticBp2;
    float click = 0, pop = 0, wander = 0, wanderTarget = 1, clickDecay = 0, popDecay = 0, wanderCoeff = 0;
    float crackleRate = 0, staticRate = 0;
};

class NoiseEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        gens[0].prepare (sr);
        gens[1].prepare (sr);
        level.prepare (sr, 30.0f);
        follow.prepare (sr, 30.0f);
        kindFade.prepare (sr, 30.0f);
        envCoeff = dsp::onePoleCoeff (60.0f, sr);
        toneCoeff = dsp::onePoleCoeff (30.0f, sr);

        // unit-RMS calibration per kind (deterministic, off the audio thread)
        for (int k = 0; k < kKinds; ++k)
        {
            NoiseGen g;
            g.prepare (sr);
            g.reset();
            dsp::Rng r;
            r.seed (0xC0FFEEu + uint32_t (k));
            const int len = int (sr * (k >= 3 ? 5.0 : 0.5)); // ~70 clicks for the sparse kinds
            double sum = 0.0;
            for (int i = 0; i < len; ++i) { const double v = g.next (k, r); sum += v * v; }
            norm[k] = float (1.0 / std::sqrt (std::max (1.0e-12, sum / len)));
        }
    }

    void reset() override
    {
        gens[0].reset();
        gens[1].reset();
        cur = 0;
        kindOf[0] = kindOf[1] = kind;
        rng.seed (0x0A15E5u);
        level.snap (level.target);
        follow.snap (follow.target);
        kindFade.snap (1.0f);
        env2 = 0.0f;
        toneS = toneTarget;
        updateTone();
        lp1.reset();
        lp2.reset();
        sinceTone = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
            {
                const int k = std::clamp (int (v), 0, kKinds - 1);
                if (k != kind)
                {
                    // the running generator fades out while a fresh one for the new kind fades in
                    kind = k;
                    cur ^= 1;
                    gens[cur].reset();
                    kindOf[cur] = k;
                    kindFade.snap (0.0f);
                    kindFade.setTarget (1.0f);
                }
                break;
            }
            case 1: level.setTarget (dsp::dbToGain (v)); break;
            case 2: toneTarget = v; break;
            case 3: follow.setTarget (v); break;
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
                toneS = toneTarget + std::pow (toneCoeff, 32.0f) * (toneS - toneTarget);
                updateTone();
                sinceTone = 31;
            }
            const float in = x[i];
            env2 = in * in + envCoeff * (env2 - in * in);

            float nz = gens[cur].next (kindOf[cur], rng) * norm[kindOf[cur]];
            if (kindFade.isRamping())
            {
                const int o = cur ^ 1;
                const float f = kindFade.next();
                nz = f * nz + (1.0f - f) * gens[o].next (kindOf[o], rng) * norm[kindOf[o]];
            }
            const float fv = follow.next();
            const float voice = std::min (2.0f, std::sqrt (env2) * 10.0f); // 1 at -20 dBFS RMS
            const float g = level.next() * ((1.0f - fv) + fv * voice);
            x[i] = in + lp2.process (lp1.process (nz * g));
        }
    }

private:
    void updateTone() noexcept
    {
        lp1.setLowpass (sr, toneS, 0.5412f); // 4th-order Butterworth pair
        lp2.setLowpass (sr, toneS, 1.3066f);
    }

    double sr = 48000.0;
    NoiseGen gens[2];
    dsp::Rng rng;
    dsp::Ramp level, follow, kindFade;
    dsp::Biquad lp1, lp2;
    float norm[kKinds] { 1, 1, 1, 1, 1 };
    float env2 = 0.0f, envCoeff = 0.0f, toneTarget = 8000.0f, toneS = 8000.0f, toneCoeff = 0.0f;
    int kind = 2, cur = 0, kindOf[2] { 2, 2 }, sinceTone = 0;
};
} // namespace

KOE_REGISTER_EFFECT ("noise", NoiseEffect)
} // namespace koe

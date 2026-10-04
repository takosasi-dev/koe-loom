#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// echo (koeloom_effects.md §3, §5.1). One feedback delay line (4 s, allocated in prepare()).
//   digital: the tap goes through the `toneHz` low-pass (12 dB/oct) and back in -> every repeat a bit
//            darker; wet = the tap.
//   tape   : additionally a 120 Hz high-pass, a 4.5 kHz one-pole and soft saturation inside the loop
//            (repeats get rounder and thinner), and wow (0.8 Hz) + flutter (6.5 Hz) + slow random drift
//            on the delay time. Fades in over 200 ms when the mode is chosen.
//   reverse: the input is cut into timeMs grains, each played backwards right after it was recorded
//            (two read heads, sin^2/cos^2 windows that sum to 1, grain length latched at grain start);
//            repeats come from the same feedback line.
// timeMs changes crossfade (40 ms) to a second tap instead of moving the read head, so there is no
// pitch sweep or click. Mode changes duck the source and the wet for 15 ms. The dry is never delayed.
// mix is equal-power (dry cos, wet sin), since the wet is uncorrelated with the dry.
// reset() does not wipe the 3 MB of history (that took up to 0.5 ms on the audio thread); reads that
// would reach samples from before the last reset() return silence instead.
//
// Types added in wave 7 (INTERFACES.md §9; digital / tape / reverse produce exactly the same output as v0.2.0).
// They run in processExtra() and share the line, the time crossfade, the tone low-pass and the ducking:
//   slapback: nothing is fed back. One bounce at timeMs (through the tone low-pass and a 100 Hz high-pass),
//             plus a second bounce at 2 x timeMs whose level is `feedback` x 0.6. Never a train of repeats.
//   analog  : bucket-brigade style. Inside the loop a 4-pole low-pass whose cutoff falls as the time grows
//             (3.4 kHz at 300 ms, 0.9-6 kHz), a 150 Hz high-pass, two all-passes that smear the transients
//             and a soft clip; the delay time wobbles slowly (0.5 Hz, +-0.25 %).
//   multitap: taps at 0.27, 0.46, 0.73 and 1.0 x timeMs; only the last one goes back into the loop.

namespace koe
{
namespace
{
class EchoEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        loop.prepare (int (sr * 4.02) + 64);
        reverseIn.prepare (int (sr * 8.0) + 64);
        fb.prepare (sr, 40.0f);
        mix.prepare (sr, 40.0f);
        duck.prepare (sr, 15.0f);
        tapeAmount.prepare (sr, 200.0f);
        xfadeLen = int (sr * 0.04);
        toneCoeff32 = std::pow (dsp::onePoleCoeff (30.0f, sr), 32.0f);
        dcBlock.setHighpass (sr, 20.0f);
        tapeHp.setHighpass (sr, 120.0f, 0.6f);
        tapeLp.setCutoff (sr, 4500.0f);
        wowAmp = float (0.0025 * sr / (2.0 * 3.14159265358979323846 * 0.8));
        flutterAmp = float (0.0012 * sr / (2.0 * 3.14159265358979323846 * 6.5));
        driftCoeff = dsp::onePoleCoeff (150.0f, sr);
        slapHp.setHighpass (sr, 100.0f);
        bbdHp.setHighpass (sr, 150.0f);
        smear[0].setAllpass (sr, 700.0f, 0.5f);
        smear[1].setAllpass (sr, 1900.0f, 0.5f);
    }

    void reset() override
    {
        mode = pendingMode;
        fb.snap (fb.target);
        mix.snap (mix.target);
        duck.snap (1.0f);
        tapeAmount.snap (mode == 1 ? 1.0f : 0.0f);
        curDelay = nextDelay = targetDelay();
        xfadePos = xfadeLen;
        toneS = toneTarget;
        tone.reset();
        tone.setLowpass (sr, toneS);
        dcBlock.reset();
        tapeHp.reset();
        tapeLp.reset();
        sinceTone = 0;
        wowPhase = flutterPhase = 0.0;
        drift = driftTarget = 0.0f;
        rng.seed (0xEC40u);
        t = 0;
        resetReverse();
        resetExtra();
        updateMixGains();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                pendingMode = std::clamp (int (v), 0, 5);
                duck.setTarget (pendingMode == mode ? 1.0f : 0.0f);
                break;
            case 1: timeMs = v; break;
            case 2: fb.setTarget (v); break;
            case 3: toneTarget = v; break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const double twoPi = 2.0 * 3.14159265358979323846;
        const float wantDelay = targetDelay();
        for (int i = 0; i < n; ++i)
        {
            if (pendingMode != mode && duck.value <= 0.0f)
            {
                mode = pendingMode;
                tapeAmount.setTarget (mode == 1 ? 1.0f : 0.0f);
                resetReverse();
                resetExtra();
                duck.setTarget (1.0f);
            }
            if (sinceTone-- <= 0)
            {
                toneS = toneTarget + toneCoeff32 * (toneS - toneTarget);
                tone.setLowpass (sr, toneS);
                if (mode >= 3) updateExtra();
                sinceTone = 31;
                if (rng.nextFloat01() < 0.01f) driftTarget = rng.nextBipolar() * 8.0f; // ~every 0.07 s
            }
            if (xfadePos >= xfadeLen && std::abs (wantDelay - curDelay) > 0.5f)
            {
                nextDelay = wantDelay;
                xfadePos = 0;
            }
            if (mix.isRamping()) updateMixGains (mix.next());

            const float in = x[i];
            reverseIn.push (in);

            // tap (with tape wow/flutter, crossfading to a new time if one is pending)
            const float ta = tapeAmount.next();
            drift = driftTarget + driftCoeff * (drift - driftTarget);
            wowPhase += 0.8 / sr;
            flutterPhase += 6.5 / sr;
            if (wowPhase >= 1.0) wowPhase -= 1.0;
            if (flutterPhase >= 1.0) flutterPhase -= 1.0;
            if (mode >= 3)
            {
                x[i] = processExtra (in);
                ++t;
                continue;
            }
            const float wobble = ta * (wowAmp * float (std::sin (twoPi * wowPhase)) + flutterAmp * float (std::sin (twoPi * flutterPhase)) + drift);
            float tap = readSinceReset (loop, curDelay - 1.0f + wobble, t);
            if (xfadePos < xfadeLen)
            {
                const float b = readSinceReset (loop, nextDelay - 1.0f + wobble, t);
                const float w = 0.5f - 0.5f * std::cos (dsp::kPi * (float (xfadePos) + 0.5f) / float (xfadeLen));
                tap += w * (b - tap);
                if (++xfadePos == xfadeLen) curDelay = nextDelay;
            }

            float rep = tone.process (tap);
            if (ta > 0.0f)
            {
                const float taped = std::tanh (1.6f * tapeLp.process (tapeHp.process (rep))) * 0.625f;
                rep += ta * (taped - rep);
            }
            rep = dcBlock.process (rep);

            const float d = duck.next();
            const float f = fb.next();
            float wet;
            if (mode == 2)
            {
                const float e = d * reverseGrains() + f * rep;
                loop.push (e);
                wet = e;
            }
            else
            {
                loop.push (d * in + f * rep);
                wet = rep;
            }
            x[i] = dryGain * in + wetGain * d * wet;
            ++t;
        }
    }

private:
    float targetDelay() const noexcept { return float (timeMs * 0.001 * sr); }

    /** DelayLine::readCubic, but samples older than the last reset() (index >= pushed) read as 0. */
    static float readSinceReset (const dsp::DelayLine& l, float d, int64_t pushed) noexcept
    {
        const int i = int (d);
        if (i + 2 < pushed) return l.readCubic (d);
        auto at = [&] (int k) { return k >= 0 && k < pushed ? l.readInt (k) : 0.0f; };
        const float f = d - float (i);
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }

    void updateMixGains (float m) noexcept
    {
        dryGain = std::sin (0.5f * dsp::kPi * (1.0f - m)); // exactly 0 at mix 1, exactly 1 at mix 0
        wetGain = std::sin (0.5f * dsp::kPi * m);
    }
    void updateMixGains() noexcept { updateMixGains (mix.value); }

    void resetReverse() noexcept
    {
        halfLen = std::max (1, int (targetDelay() * 0.5f));
        halfPos = 0;
        asc = 0;
        headStart[0] = t;
        headStart[1] = t - halfLen;
    }

    float reverseGrains() noexcept
    {
        const float s = std::sin (0.5f * dsp::kPi * (float (halfPos) + 0.5f) / float (halfLen));
        const float wAsc = s * s;
        const auto dAsc = 2 * (t - headStart[asc]), dDesc = 2 * (t - headStart[asc ^ 1]);
        const float rAsc = dAsc <= t ? reverseIn.readInt (int (dAsc)) : 0.0f; // t + 1 samples pushed since reset()
        const float rDesc = dDesc <= t ? reverseIn.readInt (int (dDesc)) : 0.0f;
        if (++halfPos >= halfLen)
        {
            halfPos = 0;
            halfLen = std::max (1, int (targetDelay() * 0.5f)); // grain length latched here, under a zero window
            asc ^= 1;
            headStart[asc] = t + 1;
        }
        return wAsc * rAsc + (1.0f - wAsc) * rDesc;
    }

    // ---- wave 7 types (slapback = 3, analog = 4, multitap = 5) ----
    float bbdTargetHz() const noexcept { return std::clamp (3400.0f * std::sqrt (300.0f / timeMs), 900.0f, 6000.0f); }

    void resetExtra() noexcept
    {
        tone2.reset();
        tone2.setLowpass (sr, toneS);
        slapHp.reset();
        bbdHp.reset();
        for (auto& f : bbdLp) f.reset();
        for (auto& f : smear) f.reset();
        bbdPhase = 0.0;
        bbdHz = bbdTargetHz();
        bbdLp[0].setLowpass (sr, bbdHz, 0.5412f);
        bbdLp[1].setLowpass (sr, bbdHz, 1.3066f);
    }

    /** Every 32 samples, right after the tone low-pass moved. */
    void updateExtra() noexcept
    {
        tone2.setLowpass (sr, toneS);
        if (mode == 4)
        {
            const float target = bbdTargetHz();
            bbdHz = target + toneCoeff32 * (bbdHz - target);
            bbdLp[0].setLowpass (sr, bbdHz, 0.5412f); // 4-pole Butterworth
            bbdLp[1].setLowpass (sr, bbdHz, 1.3066f);
        }
    }

    float processExtra (float in) noexcept
    {
        const bool fading = xfadePos < xfadeLen;
        const float xw = fading ? 0.5f - 0.5f * std::cos (dsp::kPi * (float (xfadePos) + 0.5f) / float (xfadeLen)) : 0.0f;
        auto rd = [&] (float dCur, float dNext)
        {
            float a = readSinceReset (loop, dCur - 1.0f, t);
            if (fading) a += xw * (readSinceReset (loop, dNext - 1.0f, t) - a);
            return a;
        };
        const float d = duck.next();
        const float f = fb.next();
        float wet = 0.0f;
        if (mode == 3)
        {
            const float maxD = float (loop.capacity() - 8);
            const float first = tone.process (rd (curDelay, nextDelay));
            const float second = tone2.process (rd (std::min (2.0f * curDelay, maxD), std::min (2.0f * nextDelay, maxD)));
            loop.push (d * in);
            wet = slapHp.process (first + 0.6f * f * second);
        }
        else if (mode == 4)
        {
            bbdPhase += 0.5 / sr;
            if (bbdPhase >= 1.0) bbdPhase -= 1.0;
            const float wob = 0.0025f * curDelay * float (std::sin (2.0 * 3.14159265358979323846 * bbdPhase));
            float rep = tone.process (rd (curDelay + wob, nextDelay + wob));
            rep = bbdHp.process (bbdLp[1].process (bbdLp[0].process (rep)));
            rep = smear[1].process (smear[0].process (rep));
            rep = dcBlock.process (std::tanh (1.5f * rep) * (1.0f / 1.5f));
            loop.push (d * in + f * rep);
            wet = rep;
        }
        else
        {
            static constexpr float frac[3] = { 0.27f, 0.46f, 0.73f }, gain[3] = { 0.32f, 0.42f, 0.35f };
            for (int k = 0; k < 3; ++k) wet += gain[k] * rd (curDelay * frac[k], nextDelay * frac[k]);
            const float rep = dcBlock.process (tone.process (rd (curDelay, nextDelay)));
            loop.push (d * in + f * rep);
            wet += 0.56f * rep; // tap levels: the four together sit near the dry level, like one digital repeat
        }
        if (fading && ++xfadePos == xfadeLen) curDelay = nextDelay;
        return dryGain * in + wetGain * d * wet;
    }

    dsp::Biquad tone2, slapHp, bbdHp, bbdLp[2], smear[2];
    double bbdPhase = 0.0;
    float bbdHz = 3400.0f;

    double sr = 48000.0, wowPhase = 0.0, flutterPhase = 0.0;
    int mode = 0, pendingMode = 0;
    float timeMs = 300.0f, toneTarget = 8000.0f, toneS = 8000.0f, toneCoeff32 = 0.0f;
    float curDelay = 0.0f, nextDelay = 0.0f, dryGain = 1.0f, wetGain = 0.0f;
    float wowAmp = 0.0f, flutterAmp = 0.0f, drift = 0.0f, driftTarget = 0.0f, driftCoeff = 0.0f;
    int xfadePos = 0, xfadeLen = 1, sinceTone = 0;
    int64_t t = 0, headStart[2] {};
    int halfLen = 1, halfPos = 0, asc = 0;
    dsp::DelayLine loop, reverseIn;
    dsp::Biquad tone, dcBlock, tapeHp;
    dsp::OnePoleLowpass tapeLp;
    dsp::Rng rng;
    dsp::Ramp fb, mix, duck, tapeAmount;
};
} // namespace

KOE_REGISTER_EFFECT ("echo", EchoEffect)
} // namespace koe

#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// reverb (koeloom_effects.md §3, §5.1). Pre-delay line, then one engine per type:
//   room, hall, ambience: early reflections (tapped pre-delay line) + 4 series all-pass diffusers +
//       an 8-line feedback delay network (Hadamard mixing, per-line one-pole absorption filters set from
//       RT60 at DC and at high frequencies, so `damp` shortens only the top end). Lengths, RT60 range,
//       reflection pattern and modulation differ per type: room ~0.35-1.3 s, hall ~1.6-5 s with
//       slowly modulated lines, ambience ~0.12-0.45 s with dense short reflections and little tail.
//   plate: Dattorro's plate tank (J. Dattorro, "Effect Design Part 1", JAES 1997): input diffusers into
//       two cross-coupled modulated all-pass / delay / damping branches, mono sum of his output taps.
//       Dense from the first milliseconds and bright.
//   spring: two "springs", each a feedback delay loop through 40 stretched all-pass sections
//       (a + z^-5)/(1 + a z^-5): high frequencies travel slower, so every round trip smears into a chirp
//       (the "boing"); band-limited loop, slow wobble on the loop delay.
// `room` = size (line lengths glide at most 2.0/s, so the tail bends instead of clicking) and RT60,
// `damp` = high-frequency decay, `preDelayMs` glides (read speed limited to +-0.5). Type changes duck
// the wet for 20 ms and start the new engine empty. The wet is high-passed at 90 Hz (keeps voices
// clear). mix is equal-power.

namespace koe
{
namespace
{
constexpr int kLines = 8;
constexpr int kCtrl = 32;
enum Type { room = 0, hall, plate, spring, ambience };
// Wet trim per type so the default settings sit about 1 dB under the dry level on speech (measured
// with the synthetic test voice).
constexpr float kTypeGain[5] = { 0.78f, 0.68f, 0.89f, 0.68f, 0.5f };

struct Allpass
{
    void prepare (int maxDelay) { line.prepare (maxDelay + 4); }
    void reset() { line.reset(); }
    /** Schroeder all-pass, delay >= 1 sample (fractional ok). */
    float process (float x, float delay, float g) noexcept
    {
        const float v = line.readLinear (delay - 1.0f);
        const float w = x + g * v;
        line.push (w);
        return v - g * w;
    }
    dsp::DelayLine line;
};

struct FdnSpec
{
    float lineMs[kLines];
    float scaleLo, scaleHi;   // line/reflection length scale at room 0 / 1
    float rtLo, rtHi;         // RT60 (s) at room 0 / 1 (exponential in between)
    float hfLo, hfHi;         // high-frequency RT60 ratio at damp 0 / 1
    float diffMs[4], diffG;
    float modSamples;
    int erCount;
    float erMs[12], erGain[12];
    float erLevel, lateLevel;
};

constexpr FdnSpec kRoom {
    { 9.7f, 11.3f, 13.1f, 14.9f, 16.7f, 18.7f, 20.9f, 23.3f }, 0.6f, 1.4f, 0.35f, 1.3f, 0.8f, 0.2f,
    { 2.4f, 1.8f, 6.3f, 4.6f }, 0.65f, 1.5f,
    10, { 3.1f, 5.3f, 7.9f, 10.3f, 12.7f, 15.1f, 18.3f, 21.7f, 25.9f, 30.1f },
    { 0.82f, -0.68f, 0.61f, -0.53f, 0.47f, -0.41f, 0.36f, -0.31f, 0.27f, -0.22f }, 0.55f, 1.0f
};
constexpr FdnSpec kHall {
    { 31.1f, 35.9f, 40.3f, 45.7f, 50.9f, 57.1f, 63.7f, 71.3f }, 0.75f, 1.25f, 1.6f, 5.0f, 0.7f, 0.15f,
    { 4.8f, 3.6f, 12.7f, 9.3f }, 0.7f, 7.0f,
    10, { 9.3f, 14.1f, 21.7f, 27.3f, 34.9f, 41.3f, 48.7f, 57.1f, 66.3f, 77.9f },
    { 0.6f, -0.55f, 0.5f, -0.47f, 0.43f, -0.4f, 0.36f, -0.33f, 0.3f, -0.27f }, 0.4f, 1.0f
};
constexpr FdnSpec kAmbience {
    { 2.9f, 3.7f, 4.6f, 5.5f, 6.6f, 7.7f, 8.9f, 10.3f }, 0.7f, 1.4f, 0.12f, 0.45f, 1.0f, 0.35f,
    { 1.2f, 0.9f, 3.1f, 2.3f }, 0.6f, 0.5f,
    12, { 1.3f, 2.1f, 2.9f, 3.8f, 4.9f, 6.1f, 7.4f, 8.9f, 10.6f, 12.5f, 14.7f, 17.2f },
    { 0.8f, -0.72f, 0.66f, -0.6f, 0.55f, -0.5f, 0.45f, -0.4f, 0.36f, -0.32f, 0.28f, -0.24f }, 0.8f, 0.7f
};

inline void hadamard8 (float* v) noexcept
{
    for (int h = 1; h < kLines; h <<= 1)
        for (int i = 0; i < kLines; i += 2 * h)
            for (int j = i; j < i + h; ++j)
            {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (int i = 0; i < kLines; ++i) v[i] *= 0.35355339f;
}

struct Fdn
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const float ms = float (sr * 0.001);
        for (auto& l : lines) l.prepare (int (75.0f * 1.25f * ms + 16.0f));
        for (int k = 0; k < 4; ++k) diff[k].prepare (int (13.0f * ms));
    }

    void reset()
    {
        for (auto& l : lines) l.reset();
        for (auto& d : diff) d.reset();
        for (auto& s : z) s = 0.0f;
        for (int i = 0; i < kLines; ++i) modPhase[i] = double (i) * 0.125;
        fresh = true;
    }

    /** Control rate: length targets (reached linearly over the next kCtrl samples), modulation,
        absorption filters. */
    void update (const FdnSpec& s, float sizeNow, float roomNow, float dampNow)
    {
        const float ms = float (sr * 0.001);
        const float scale = s.scaleLo + (s.scaleHi - s.scaleLo) * sizeNow;
        const float rt = s.rtLo * std::pow (s.rtHi / s.rtLo, roomNow);
        const float rtHf = rt * (s.hfLo + (s.hfHi - s.hfLo) * dampNow);
        for (int i = 0; i < kLines; ++i)
        {
            modPhase[i] += (0.41 + 0.093 * i) * kCtrl / sr;
            modPhase[i] -= std::floor (modPhase[i]);
            const float target = s.lineMs[i] * scale * ms + s.modSamples * float (std::sin (6.283185307179586 * modPhase[i]));
            if (fresh) len[i] = target;
            lenStep[i] = (target - len[i]) / float (kCtrl);
            const float gDc = std::pow (10.0f, -3.0f * target / (rt * float (sr)));
            const float gHf = std::pow (10.0f, -3.0f * target / (rtHf * float (sr)));
            pole[i] = (gDc - gHf) / (gDc + gHf);
            gain[i] = gDc * (1.0f - pole[i]);
        }
        for (int k = 0; k < 4; ++k) diffLen[k] = std::max (1.0f, std::round (s.diffMs[k] * ms));
        diffG = s.diffG;
        fresh = false;
    }

    float process (float in) noexcept
    {
        float d = in;
        for (int k = 0; k < 4; ++k) d = diff[k].process (d, diffLen[k], diffG);
        float o[kLines];
        for (int i = 0; i < kLines; ++i)
        {
            len[i] += lenStep[i];
            z[i] = gain[i] * lines[i].readLinear (len[i] - 1.0f) + pole[i] * z[i];
            o[i] = z[i];
        }
        const float late = (o[0] - o[1] + o[2] - o[3] + o[4] - o[5] + o[6] - o[7]) * 0.35355339f;
        hadamard8 (o);
        static constexpr float inSign[kLines] = { 0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f };
        for (int i = 0; i < kLines; ++i) lines[i].push (o[i] + inSign[i] * d);
        return late;
    }

    double sr = 48000.0;
    dsp::DelayLine lines[kLines];
    Allpass diff[4];
    float len[kLines] {}, lenStep[kLines] {}, gain[kLines] {}, pole[kLines] {}, z[kLines] {}, diffLen[4] { 1, 1, 1, 1 }, diffG = 0.6f;
    double modPhase[kLines] {};
    bool fresh = true;
};

struct Plate
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        r = float (sr / 29761.0);
        static constexpr float inLen[4] = { 142, 107, 379, 277 };
        for (int k = 0; k < 4; ++k) { inputLen[k] = std::round (inLen[k] * r); input[k].prepare (int (inputLen[k]) + 2); }
        static constexpr float ap1[2] = { 672, 908 }, del1[2] = { 4453, 4217 }, ap2[2] = { 1800, 2656 }, del2[2] = { 3720, 3163 };
        excursion = 16.0f * r;
        for (int s = 0; s < 2; ++s)
        {
            ap1Len[s] = std::round (ap1[s] * r);
            tankAp1[s].prepare (int (ap1Len[s] + excursion) + 4);
            d1Len[s] = int (std::round (del1[s] * r));
            d1[s].prepare (d1Len[s] + 2);
            ap2Len[s] = std::round (ap2[s] * r);
            tankAp2[s].prepare (int (ap2Len[s]) + 2);
            d2Len[s] = int (std::round (del2[s] * r));
            d2[s].prepare (d2Len[s] + 2);
        }
    }

    void reset()
    {
        for (auto& a : input) a.reset();
        for (int s = 0; s < 2; ++s) { tankAp1[s].reset(); d1[s].reset(); tankAp2[s].reset(); d2[s].reset(); damp[s] = 0.0f; }
        bwState = 0.0f;
        modPhase = 0.0;
        mod[0] = mod[1] = modStep[0] = modStep[1] = 0.0f;
    }

    void update (float roomNow, float dampNow)
    {
        decay = 0.2f + 0.65f * roomNow;
        decayDiff2 = std::clamp (decay + 0.15f, 0.25f, 0.5f);
        damping = 0.0005f + 0.5f * dampNow * dampNow;
        bandwidth = 0.9995f - 0.25f * dampNow;
        modPhase += 1.0 * kCtrl / sr;
        modPhase -= std::floor (modPhase);
        modStep[0] = (excursion * float (std::sin (6.283185307179586 * modPhase)) - mod[0]) / float (kCtrl);
        modStep[1] = (excursion * float (std::cos (6.283185307179586 * modPhase)) - mod[1]) / float (kCtrl);
    }

    float process (float in) noexcept
    {
        bwState += bandwidth * (in - bwState);
        float v = bwState;
        v = input[0].process (v, inputLen[0], 0.75f);
        v = input[1].process (v, inputLen[1], 0.75f);
        v = input[2].process (v, inputLen[2], 0.625f);
        v = input[3].process (v, inputLen[3], 0.625f);

        const float crossToLeft = decay * d2[1].readInt (d2Len[1] - 1);
        const float crossToRight = decay * d2[0].readInt (d2Len[0] - 1);
        const float sideIn[2] = { v + crossToLeft, v + crossToRight };
        for (int s = 0; s < 2; ++s)
        {
            mod[s] += modStep[s];
            const float a = tankAp1[s].process (sideIn[s], ap1Len[s] + mod[s], -0.7f);
            d1[s].push (a);
            const float b = d1[s].readInt (d1Len[s] - 1);
            damp[s] = b + damping * (damp[s] - b);
            d2[s].push (tankAp2[s].process (damp[s] * decay, ap2Len[s], decayDiff2));
        }

        auto tap = [this] (const dsp::DelayLine& l, float idx) { return l.readInt (int (idx * r)); };
        const float left = tap (d1[1], 266) + tap (d1[1], 2974) - tap (tankAp2[1].line, 1913) + tap (d2[1], 1996)
                         - tap (d1[0], 1990) - tap (tankAp2[0].line, 187) - tap (d2[0], 1066);
        const float right = tap (d1[0], 353) + tap (d1[0], 3627) - tap (tankAp2[0].line, 1228) + tap (d2[0], 2673)
                          - tap (d1[1], 2111) - tap (tankAp2[1].line, 335) - tap (d2[1], 121);
        return 0.3f * (left + right);
    }

    double sr = 48000.0, modPhase = 0.0;
    float r = 1.0f, excursion = 0.0f, decay = 0.5f, decayDiff2 = 0.5f, damping = 0.0005f, bandwidth = 0.9995f, bwState = 0.0f;
    Allpass input[4], tankAp1[2], tankAp2[2];
    dsp::DelayLine d1[2], d2[2];
    float inputLen[4] {}, ap1Len[2] {}, ap2Len[2] {}, damp[2] {}, mod[2] {}, modStep[2] {};
    int d1Len[2] {}, d2Len[2] {};
};

struct Spring
{
    static constexpr int kStages = 40, kStretch = 5;
    static constexpr float kAllpassCoeff = 0.6f;

    void prepare (double sr, float maxMs) { line.prepare (int (maxMs * 0.001 * sr) + 64); }
    void reset()
    {
        line.reset();
        lp.reset();
        hp.reset();
        for (auto& s : xh) for (auto& v : s) v = 0.0f;
        for (auto& s : yh) for (auto& v : s) v = 0.0f;
        idx = 0;
    }

    float process (float in) noexcept
    {
        len += lenStep;
        const float tap = line.readLinear (len - 1.0f);
        float v = in + loopGain * hp.process (lp.process (tap));
        for (int s = 0; s < kStages; ++s)
        {
            const float xk = xh[s][idx], yk = yh[s][idx];
            const float y = kAllpassCoeff * (v - yk) + xk;
            xh[s][idx] = v;
            yh[s][idx] = y;
            v = y;
        }
        idx = idx + 1 == kStretch ? 0 : idx + 1;
        line.push (v);
        return tap;
    }

    dsp::DelayLine line;
    dsp::Biquad lp, hp;
    float xh[kStages][kStretch] {}, yh[kStages][kStretch] {};
    float len = 100.0f, lenStep = 0.0f, loopGain = 0.0f;
    int idx = 0;
};

struct SpringTank
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& s : springs) s.prepare (sr, 41.0f * 1.3f + 2.0f);
        inHp.setHighpass (sr, 120.0f);
        inLp.setLowpass (sr, 4500.0f);
    }

    void reset()
    {
        for (auto& s : springs) s.reset();
        inHp.reset();
        inLp.reset();
        wobble = 0.0;
        fresh = true;
    }

    void update (float roomNow, float dampNow)
    {
        static constexpr float baseMs[2] = { 33.0f, 41.0f };
        const float ms = float (sr * 0.001);
        const float rt = 1.4f + 2.2f * roomNow;
        wobble += kCtrl / sr;
        const float w = float (0.35 * std::sin (6.283185307179586 * 0.53 * wobble) + 0.12 * std::sin (6.283185307179586 * 1.71 * wobble));
        for (int k = 0; k < 2; ++k)
        {
            auto& s = springs[k];
            const float target = (baseMs[k] * (0.8f + 0.5f * roomNow) + (k == 0 ? w : -w)) * ms;
            if (fresh) s.len = target;
            s.lenStep = (target - s.len) / float (kCtrl);
            const float trip = target + 200.0f; // + mean group delay of the all-pass chain
            s.loopGain = std::pow (10.0f, -3.0f * trip / (rt * float (sr)));
            s.lp.setLowpass (sr, 6500.0f - 4000.0f * dampNow);
            s.hp.setHighpass (sr, 90.0f);
        }
        fresh = false;
    }

    float process (float in) noexcept
    {
        const float x = inLp.process (inHp.process (in));
        return springs[0].process (x) - 0.8f * springs[1].process (x);
    }

    double sr = 48000.0, wobble = 0.0;
    bool fresh = true;
    Spring springs[2];
    dsp::Biquad inHp, inLp;
};

class ReverbEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        preLine.prepare (int (sr * 0.32) + 16); // 200 ms pre-delay + longest reflection (hall 78 ms x 1.25)
        fdn.prepare (sr);
        plateTank.prepare (sr);
        springTank.prepare (sr);
        mix.prepare (sr, 40.0f);
        duck.prepare (sr, 20.0f);
        lowCut.setHighpass (sr, 90.0f);
        dampCoeff32 = std::pow (dsp::onePoleCoeff (40.0f, sr), float (kCtrl));
    }

    void reset() override
    {
        type = pendingType;
        preLine.reset();
        clearEngine();
        mix.snap (mix.target);
        duck.snap (1.0f);
        sizeNow = roomTarget;
        dampNow = dampTarget;
        preDelay = preDelayTarget();
        ctrlLeft = 0;
        updateMixGains (mix.value);
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                pendingType = std::clamp (int (v), 0, 4);
                duck.setTarget (pendingType == type ? 1.0f : 0.0f);
                break;
            case 1: roomTarget = v; break;
            case 2: dampTarget = v; break;
            case 3: preDelayMs = v; break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float preTarget = preDelayTarget();
        const float preRate = float (1.0 / (0.05 * sr));
        const float sizeStep = float (2.0 / sr) * kCtrl;
        for (int i = 0; i < n; ++i)
        {
            if (pendingType != type && duck.value <= 0.0f)
            {
                type = pendingType;
                clearEngine();
                duck.setTarget (1.0f);
                ctrlLeft = 0;
            }
            if (ctrlLeft-- <= 0)
            {
                sizeNow += std::clamp (roomTarget - sizeNow, -sizeStep, sizeStep);
                dampNow = dampTarget + dampCoeff32 * (dampNow - dampTarget);
                updateEngine();
                ctrlLeft = kCtrl - 1;
            }
            if (mix.isRamping()) updateMixGains (mix.next());
            preDelay += std::clamp ((preTarget - preDelay) * preRate * 8.0f, -0.5f, 0.5f);

            const float in = x[i];
            preLine.push (in);
            const float p = preLine.readLinear (preDelay);
            float wet;
            switch (type)
            {
                case plate: wet = plateTank.process (p); break;
                case spring: wet = springTank.process (p) * 0.5f; break;
                default:
                {
                    const auto& s = spec();
                    float er = 0.0f;
                    for (int k = 0; k < s.erCount; ++k)
                    {
                        erDelay[k] += erStep[k];
                        er += s.erGain[k] * preLine.readLinear (preDelay + erDelay[k]);
                    }
                    wet = s.erLevel * er + s.lateLevel * fdn.process (p);
                    break;
                }
            }
            wet = lowCut.process (wet) * kTypeGain[type] * duck.next();
            x[i] = dryGain * in + wetGain * wet;
        }
    }

private:
    const FdnSpec& spec() const noexcept { return type == hall ? kHall : (type == ambience ? kAmbience : kRoom); }
    float preDelayTarget() const noexcept { return float (preDelayMs * 0.001 * sr); }

    void updateMixGains (float m) noexcept
    {
        dryGain = std::sin (0.5f * dsp::kPi * (1.0f - m)); // exactly 0 at mix 1, exactly 1 at mix 0
        wetGain = std::sin (0.5f * dsp::kPi * m);
    }

    /** Only the engine about to run is wiped; the others are wiped when they get selected. */
    void clearEngine() noexcept
    {
        erFresh = true;
        if (type == plate) plateTank.reset();
        else if (type == spring) springTank.reset();
        else fdn.reset();
        lowCut.reset();
    }

    void updateEngine() noexcept
    {
        switch (type)
        {
            case plate: plateTank.update (sizeNow, dampNow); break;
            case spring: springTank.update (sizeNow, dampNow); break;
            default:
            {
                const auto& s = spec();
                fdn.update (s, sizeNow, sizeNow, dampNow);
                const float scale = (s.scaleLo + (s.scaleHi - s.scaleLo) * sizeNow) * float (sr * 0.001);
                for (int k = 0; k < s.erCount; ++k)
                {
                    if (erFresh) erDelay[k] = s.erMs[k] * scale;
                    erStep[k] = (s.erMs[k] * scale - erDelay[k]) / float (kCtrl);
                }
                erFresh = false;
                break;
            }
        }
    }

    double sr = 48000.0;
    int type = room, pendingType = room, ctrlLeft = 0;
    float roomTarget = 0.5f, dampTarget = 0.5f, preDelayMs = 0.0f;
    float sizeNow = 0.5f, dampNow = 0.5f, preDelay = 0.0f, dampCoeff32 = 0.0f, dryGain = 1.0f, wetGain = 0.0f;
    float erDelay[12] {}, erStep[12] {};
    bool erFresh = true;
    dsp::DelayLine preLine;
    Fdn fdn;
    Plate plateTank;
    SpringTank springTank;
    dsp::Biquad lowCut;
    dsp::Ramp mix, duck;
};
} // namespace

KOE_REGISTER_EFFECT ("reverb", ReverbEffect)
} // namespace koe

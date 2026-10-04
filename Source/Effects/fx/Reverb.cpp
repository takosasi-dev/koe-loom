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
//
// Types added in wave 7 (INTERFACES.md §9; the first five above produce exactly the same output as v0.2.0):
//   cathedral: long sparse early reflections, and the input goes through 4 long (23-71 ms) all-passes before
//       the FDN, so the tail "blooms" (builds up over ~100 ms) instead of starting dense. RT60 2.8-6.5 s, dark.
//   gated: a dense 2.2 s room whose whole wet is gated by the input level (80s gated reverb): the gate opens
//       while the voice is above -36 dBFS, holds for 80-500 ms (`room`) after it drops, then cuts in 15 ms.
//   reverse: no feedback at all. 48 taps of a 1 s line spread over 150-700 ms (`room`), quieter at the start
//       and loudest at the end (-30 dB -> 0 dB), then 3 short all-passes: the "reverb" swells towards you
//       and stops dead.
//   shimmer: a medium FDN whose output is pitch-shifted one octave up (two crossfaded read heads over a
//       50 ms window), band-limited (250 Hz - 7 kHz) and fed back into its input: every round trip climbs
//       an octave until the low-pass eats it, so it always dies out.
//   cave: 6 discrete distant echoes (56-514 ms, each darker than the last) feeding a dark, barely diffused
//       FDN (RT60 1.8-5.5 s): separate rock-wall slaps over a grainy tail.
//   bathroom: a tiny bright FDN plus a flutter echo between parallel tiles (a 5-12 ms comb) and three
//       +6 dB room-mode resonances (118/167/241 Hz scaled by `room`): short, boxy and ringing.

namespace koe
{
namespace
{
constexpr int kLines = 8;
constexpr int kCtrl = 32;
enum Type { room = 0, hall, plate, spring, ambience, cathedral, gated, reverse, shimmer, cave, bathroom };
constexpr int kNumTypes = 11;
// Wet trim per type so the default settings sit about 1 dB under the dry level on speech (measured
// with the synthetic test voice).
constexpr float kTypeGain[kNumTypes] = { 0.78f, 0.68f, 0.89f, 0.68f, 0.5f, 0.75f, 0.49f, 0.94f, 0.52f, 0.7f, 0.27f };

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
constexpr FdnSpec kCathedral {
    { 41.3f, 47.9f, 53.1f, 59.7f, 66.1f, 71.9f, 79.3f, 86.9f }, 0.8f, 1.1f, 2.8f, 6.5f, 0.55f, 0.12f,
    { 4.8f, 3.6f, 12.7f, 9.3f }, 0.7f, 9.0f,
    8, { 19.3f, 27.1f, 36.7f, 45.9f, 58.3f, 69.1f, 81.7f, 97.3f },
    { 0.5f, -0.45f, 0.42f, -0.38f, 0.33f, -0.3f, 0.27f, -0.24f }, 0.15f, 1.0f // far walls: weak reflections
};
constexpr FdnSpec kGated {
    { 13.1f, 15.7f, 17.9f, 20.3f, 22.9f, 25.1f, 28.3f, 31.1f }, 1.0f, 1.0f, 2.2f, 2.2f, 0.75f, 0.25f,
    { 2.4f, 1.8f, 6.3f, 4.6f }, 0.72f, 1.0f,
    10, { 2.3f, 4.1f, 5.9f, 8.3f, 10.7f, 13.9f, 16.1f, 19.7f, 23.3f, 27.1f },
    { 0.7f, -0.64f, 0.6f, -0.55f, 0.5f, -0.46f, 0.42f, -0.38f, 0.34f, -0.3f }, 0.6f, 1.1f
};
constexpr FdnSpec kShimmer {
    { 23.3f, 27.7f, 31.9f, 36.1f, 41.3f, 45.7f, 50.3f, 55.9f }, 0.8f, 1.2f, 1.5f, 4.0f, 0.7f, 0.2f,
    { 3.1f, 2.3f, 8.9f, 6.7f }, 0.7f, 4.0f,
    4, { 7.1f, 13.3f, 19.7f, 29.3f },
    { 0.4f, -0.35f, 0.3f, -0.25f }, 0.25f, 1.0f
};
constexpr FdnSpec kCave {
    { 37.1f, 43.9f, 51.7f, 58.3f, 66.7f, 73.1f, 81.9f, 89.3f }, 0.75f, 1.05f, 1.8f, 5.5f, 0.45f, 0.1f,
    { 6.1f, 4.3f, 11.9f, 8.7f }, 0.35f, 3.0f,
    6, { 11.3f, 23.9f, 31.7f, 47.3f, 62.9f, 79.1f },
    { 0.45f, -0.4f, 0.36f, -0.3f, 0.26f, -0.22f }, 0.4f, 0.9f
};
constexpr FdnSpec kBathroom {
    { 3.7f, 4.3f, 5.1f, 5.9f, 6.7f, 7.3f, 8.3f, 9.1f }, 0.7f, 1.4f, 0.45f, 1.4f, 1.0f, 0.55f,
    { 0.9f, 0.7f, 2.3f, 1.7f }, 0.7f, 0.2f,
    12, { 1.1f, 1.9f, 2.6f, 3.4f, 4.3f, 5.2f, 6.1f, 7.3f, 8.4f, 9.8f, 11.2f, 12.9f },
    { 0.85f, -0.78f, 0.72f, -0.66f, 0.6f, -0.55f, 0.5f, -0.46f, 0.42f, -0.38f, 0.34f, -0.3f }, 0.9f, 0.8f
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
        for (auto& l : lines) l.prepare (int (100.0f * ms + 16.0f)); // cathedral 86.9 ms x 1.1 + modulation
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

/** Reverse reverb: a feed-forward swell (no feedback, so it ends exactly `length` after the input stops). */
struct ReverseTank
{
    static constexpr int kTaps = 48;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        line.prepare (int (sr * 0.72) + 16);
        static constexpr float apMs[3] = { 4.1f, 6.7f, 11.3f };
        for (int k = 0; k < 3; ++k) { apLen[k] = std::round (apMs[k] * float (sr * 0.001)); ap[k].prepare (int (apLen[k]) + 2); }
        dsp::Rng rng;
        rng.seed (0x5EBu);
        double energy = 0.0;
        for (int k = 0; k < kTaps; ++k)
        {
            frac[k] = (float (k) + 0.5f + 0.35f * rng.nextBipolar()) / float (kTaps);
            gain[k] = std::pow (10.0f, (frac[k] - 1.0f) * 1.5f) * (rng.nextU32() & 1u ? 1.0f : -1.0f); // -30 dB -> 0 dB
            energy += double (gain[k]) * gain[k];
        }
        const float norm = float (1.0 / std::sqrt (energy));
        for (auto& g : gain) g *= norm;
    }

    void reset()
    {
        line.reset();
        for (auto& a : ap) a.reset();
        lp.reset();
        fresh = true;
    }

    void update (float sizeNow, float dampNow)
    {
        const float len = (150.0f + 550.0f * sizeNow) * float (sr * 0.001);
        for (int k = 0; k < kTaps; ++k)
        {
            const float target = frac[k] * len;
            if (fresh) delay[k] = target;
            step[k] = (target - delay[k]) / float (kCtrl);
        }
        lp.setCutoff (sr, 12000.0f * (1.0f - 0.75f * dampNow));
        fresh = false;
    }

    float process (float in) noexcept
    {
        line.push (in);
        float s = 0.0f;
        for (int k = 0; k < kTaps; ++k)
        {
            delay[k] += step[k];
            s += gain[k] * line.readLinear (delay[k]);
        }
        s = ap[0].process (s, apLen[0], 0.6f);
        s = ap[1].process (s, apLen[1], 0.6f);
        s = ap[2].process (s, apLen[2], 0.6f);
        return lp.process (s);
    }

    double sr = 48000.0;
    bool fresh = true;
    dsp::DelayLine line;
    Allpass ap[3];
    dsp::OnePoleLowpass lp;
    float apLen[3] {}, frac[kTaps] {}, gain[kTaps] {}, delay[kTaps] {}, step[kTaps] {};
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
        prepareExtras();
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
                pendingType = std::clamp (int (v), 0, kNumTypes - 1);
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
                case reverse: wet = reverseTank.process (p); break;
                case cathedral: case gated: case shimmer: case cave: case bathroom: wet = processExtra (p); break;
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
    const FdnSpec& spec() const noexcept
    {
        switch (type)
        {
            case cathedral: return kCathedral;
            case gated: return kGated;
            case shimmer: return kShimmer;
            case cave: return kCave;
            case bathroom: return kBathroom;
            default: return type == hall ? kHall : (type == ambience ? kAmbience : kRoom);
        }
    }
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
        else if (type == reverse) reverseTank.reset();
        else
        {
            fdn.reset();
            if (type >= cathedral) clearExtras();
        }
        lowCut.reset();
    }

    void updateEngine() noexcept
    {
        switch (type)
        {
            case plate: plateTank.update (sizeNow, dampNow); break;
            case spring: springTank.update (sizeNow, dampNow); break;
            case reverse: reverseTank.update (sizeNow, dampNow); break;
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
                if (type >= cathedral) updateExtras();
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

    // ---- wave 7 types ----
    static constexpr float kBloomMs[4] = { 23.1f, 37.3f, 53.9f, 71.3f };
    static constexpr float kCaveMs[6] = { 93.0f, 151.0f, 227.0f, 301.0f, 389.0f, 467.0f };
    static constexpr float kCaveGain[6] = { 0.42f, -0.36f, 0.31f, -0.26f, 0.21f, -0.17f };
    static constexpr float kCaveHz[6] = { 4200.0f, 3300.0f, 2600.0f, 2000.0f, 1600.0f, 1250.0f };
    static constexpr float kModeHz[3] = { 118.0f, 167.0f, 241.0f };

    void prepareExtras()
    {
        const float ms = float (sr * 0.001);
        reverseTank.prepare (sr);
        for (auto& b : bloom) b.prepare (int (72.0f * 1.2f * ms) + 4);
        shiftWindow = std::round (50.0f * ms);
        shiftLine.prepare (int (shiftWindow) + 8);
        shimHp.setHighpass (sr, 250.0f);
        shimLp.setLowpass (sr, 7000.0f);
        caveLine.prepare (int (467.0f * 1.1f * ms) + 16);
        flutterLine.prepare (int (13.0f * ms) + 4);
        gateEnv.setTimes (sr, 1.0f, 40.0f);
        gateOpenStep = float (1.0 / (0.002 * sr));
        gateCloseStep = float (1.0 / (0.015 * sr));
    }

    void clearExtras() noexcept
    {
        for (auto& b : bloom) b.reset();
        shiftLine.reset();
        shimHp.reset();
        shimLp.reset();
        shiftPhase = 0.0f;
        shimOut = 0.0f;
        caveLine.reset();
        for (auto& l : caveLp) l.reset();
        flutterLine.reset();
        flutterLp.reset();
        for (auto& m : modes) m.reset();
        gateEnv.reset();
        gateGain = 0.0f;
        holdLeft = 0;
    }

    /** Control rate, after the FDN and early reflections (erFresh still tells a fresh start). */
    void updateExtras() noexcept
    {
        const float ms = float (sr * 0.001);
        switch (type)
        {
            case cathedral:
                for (int k = 0; k < 4; ++k)
                {
                    const float target = kBloomMs[k] * (0.8f + 0.4f * sizeNow) * ms;
                    if (erFresh) bloomLen[k] = target;
                    bloomStep[k] = (target - bloomLen[k]) / float (kCtrl);
                }
                break;
            case gated: holdSamples = int ((80.0f + 420.0f * sizeNow) * ms); break;
            case shimmer: shimFb = 0.3f + 0.25f * sizeNow; break;
            case cave:
                for (int k = 0; k < 6; ++k)
                {
                    const float target = kCaveMs[k] * (0.6f + 0.5f * sizeNow) * ms;
                    if (erFresh) caveDelay[k] = target;
                    caveStep[k] = (target - caveDelay[k]) / float (kCtrl);
                    caveLp[k].setCutoff (sr, kCaveHz[k] * (1.0f - 0.5f * dampNow));
                }
                break;
            case bathroom:
            {
                const float target = (5.0f + 7.0f * sizeNow) * ms;
                if (erFresh) flutterLen = target;
                flutterStep = (target - flutterLen) / float (kCtrl);
                flutterLp.setCutoff (sr, 9000.0f * (1.0f - 0.5f * dampNow));
                for (int k = 0; k < 3; ++k) modes[k].setPeak (sr, kModeHz[k] * (1.35f - 0.7f * sizeNow), 4.0f, 6.0f);
                break;
            }
            default: break;
        }
    }

    /** cathedral / gated / shimmer / cave / bathroom: early reflections + FDN + the type's own stage. */
    float processExtra (float p) noexcept
    {
        const auto& s = spec();
        float er = 0.0f;
        for (int k = 0; k < s.erCount; ++k)
        {
            erDelay[k] += erStep[k];
            er += s.erGain[k] * preLine.readLinear (preDelay + erDelay[k]);
        }
        switch (type)
        {
            case cathedral:
            {
                float b = p;
                for (int k = 0; k < 4; ++k)
                {
                    bloomLen[k] += bloomStep[k];
                    b = bloom[k].process (b, bloomLen[k], 0.62f);
                }
                return s.erLevel * er + s.lateLevel * fdn.process (b);
            }
            case gated:
            {
                if (gateEnv.process (p) > 0.0158f) holdLeft = holdSamples; // -36 dBFS
                else if (holdLeft > 0) --holdLeft;
                gateGain += std::clamp ((holdLeft > 0 ? 1.0f : 0.0f) - gateGain, -gateCloseStep, gateOpenStep);
                return (s.erLevel * er + s.lateLevel * fdn.process (p)) * gateGain;
            }
            case shimmer:
            {
                const float late = fdn.process (p + shimFb * shimOut);
                shiftLine.push (shimHp.process (late));
                shiftPhase += 1.0f / shiftWindow; // the delay shrinks 1 sample per sample: read speed 2 = +12 st
                if (shiftPhase >= 1.0f) shiftPhase -= 1.0f;
                const float ph2 = shiftPhase < 0.5f ? shiftPhase + 0.5f : shiftPhase - 0.5f;
                const float w1 = std::sin (dsp::kPi * shiftPhase);
                const float r1 = shiftLine.readLinear (shiftWindow * (1.0f - shiftPhase) + 1.0f);
                const float r2 = shiftLine.readLinear (shiftWindow * (1.0f - ph2) + 1.0f);
                shimOut = shimLp.process (w1 * w1 * r1 + (1.0f - w1 * w1) * r2);
                return s.erLevel * er + s.lateLevel * late + 0.6f * shimOut;
            }
            case cave:
            {
                caveLine.push (p);
                float echoes = 0.0f;
                for (int k = 0; k < 6; ++k)
                {
                    caveDelay[k] += caveStep[k];
                    echoes += caveLp[k].process (kCaveGain[k] * caveLine.readLinear (caveDelay[k]));
                }
                return s.erLevel * er + s.lateLevel * fdn.process (p + 0.5f * echoes) + 0.8f * echoes;
            }
            case bathroom:
            {
                flutterLen += flutterStep;
                const float fl = flutterLine.readLinear (flutterLen);
                flutterLine.push (0.5f * p + 0.55f * flutterLp.process (fl));
                float w = s.erLevel * er + s.lateLevel * fdn.process (p) + 0.3f * fl;
                for (auto& m : modes) w = m.process (w);
                return w;
            }
            default: return 0.0f;
        }
    }

    ReverseTank reverseTank;
    Allpass bloom[4];
    float bloomLen[4] {}, bloomStep[4] {};
    dsp::EnvelopeFollower gateEnv;
    float gateGain = 0.0f, gateOpenStep = 0.0f, gateCloseStep = 0.0f;
    int holdLeft = 0, holdSamples = 0;
    dsp::DelayLine shiftLine;
    dsp::Biquad shimHp, shimLp;
    float shiftWindow = 2400.0f, shiftPhase = 0.0f, shimOut = 0.0f, shimFb = 0.4f;
    dsp::DelayLine caveLine;
    dsp::OnePoleLowpass caveLp[6];
    float caveDelay[6] {}, caveStep[6] {};
    dsp::DelayLine flutterLine;
    dsp::OnePoleLowpass flutterLp;
    float flutterLen = 240.0f, flutterStep = 0.0f;
    dsp::Biquad modes[3];
};
} // namespace

KOE_REGISTER_EFFECT ("reverb", ReverbEffect)
} // namespace koe

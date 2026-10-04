#pragma once

// Small allocation-free DSP building blocks shared by the effects and the engine.
// Everything here is safe to call on the audio thread (no allocation, no locks) except
// DelayLine::prepare(), which allocates and must be called from prepare().

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace koe::dsp
{
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain (float db) noexcept { return db <= -120.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept { return g <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (g); }
inline bool isFiniteSample (float x) noexcept { return std::isfinite (x); }
inline float msToSamples (float ms, double sr) noexcept { return float (ms * 0.001 * sr); }

/** Bit-identical to std::fmod (x, y) for y > 0 and |x / y| < 2^20, several times faster (MSVC's fmodf is slow; the phase
    vocoder calls it per bin, wave9/stream bench). x - n*y is exact in double (n has <= 20 bits, y 24) and the true remainder
    is representable as a float, so the one rounding at the end is exact; the n from the division is corrected by one. */
inline float fmodExact (float x, float y) noexcept
{
    const double dx = x, dy = y;
    double r = dx - std::trunc (dx / dy) * dy;
    if (dx >= 0.0) { if (r < 0.0) r += dy; else if (r >= dy) r -= dy; }
    else if (r > 0.0) r -= dy;
    else if (r <= -dy) r += dy;
    return float (r == 0.0 ? std::copysign (0.0, dx) : r); // fmod keeps the sign of x on a zero remainder
}

/** One-pole smoothing coefficient so that a step settles to ~63 % in timeMs. */
inline float onePoleCoeff (float timeMs, double sr) noexcept
{
    if (timeMs <= 0.0f) return 0.0f;
    return std::exp (-1.0f / (float (sr) * timeMs * 0.001f));
}

/** Linear ramp towards a target over a fixed time; snap() jumps. Use for gains/params. */
struct Ramp
{
    void prepare (double sr, float rampMs) noexcept { steps = std::max (1, int (sr * rampMs * 0.001)); }
    void setTarget (float t) noexcept { setTarget (t, steps); }
    /** Ramp over a one-off length instead of the prepared one (e.g. a faster cut). */
    void setTarget (float t, int overSteps) noexcept
    {
        if (t == target) return;
        target = t;
        remaining = std::max (1, overSteps);
        inc = (target - value) / float (remaining);
    }
    void snap (float t) noexcept { target = value = t; remaining = 0; inc = 0.0f; }
    float next() noexcept
    {
        if (remaining > 0) { value += inc; if (--remaining == 0) value = target; }
        return value;
    }
    bool isRamping() const noexcept { return remaining > 0; }
    float value = 0.0f, target = 0.0f, inc = 0.0f;
    int steps = 1, remaining = 0;
};

/** RBJ cookbook biquad, transposed direct form II. Coefficient setters are allocation-free. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0.0f; }
    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setLowpass (double sr, float f, float q = 0.70710678f) noexcept { design (sr, f, q, 0, Type::lp); }
    void setHighpass (double sr, float f, float q = 0.70710678f) noexcept { design (sr, f, q, 0, Type::hp); }
    void setBandpass (double sr, float f, float q) noexcept { design (sr, f, q, 0, Type::bp); }   // 0 dB peak
    void setNotch (double sr, float f, float q) noexcept { design (sr, f, q, 0, Type::notch); }
    void setAllpass (double sr, float f, float q) noexcept { design (sr, f, q, 0, Type::ap); }
    void setPeak (double sr, float f, float q, float gainDb) noexcept { design (sr, f, q, gainDb, Type::peak); }
    void setLowShelf (double sr, float f, float gainDb, float q = 0.70710678f) noexcept { design (sr, f, q, gainDb, Type::lowShelf); }
    void setHighShelf (double sr, float f, float gainDb, float q = 0.70710678f) noexcept { design (sr, f, q, gainDb, Type::highShelf); }

private:
    enum class Type { lp, hp, bp, notch, ap, peak, lowShelf, highShelf };
    void design (double sr, float f, float q, float gainDb, Type t) noexcept
    {
        f = std::clamp (f, 1.0f, float (sr * 0.49));
        q = std::max (q, 0.01f);
        const double w0 = 2.0 * 3.14159265358979323846 * f / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        const double A = std::pow (10.0, gainDb / 40.0);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (t)
        {
            case Type::lp:    B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = (1 - cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::hp:    B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::bp:    B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::notch: B0 = 1; B1 = -2 * cw; B2 = 1; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::ap:    B0 = 1 - alpha; B1 = -2 * cw; B2 = 1 + alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::peak:  B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A; break;
            case Type::lowShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) - (A - 1) * cw + sq); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - sq);
                A0 = (A + 1) + (A - 1) * cw + sq; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - sq;
                break;
            }
            case Type::highShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) + (A - 1) * cw + sq); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - sq);
                A0 = (A + 1) - (A - 1) * cw + sq; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - sq;
                break;
            }
        }
        b0 = float (B0 / A0); b1 = float (B1 / A0); b2 = float (B2 / A0);
        a1 = float (A1 / A0); a2 = float (A2 / A0);
    }
};

/** One-pole low-pass (6 dB/oct). */
struct OnePoleLowpass
{
    float a = 0.0f, z = 0.0f;
    void setCutoff (double sr, float f) noexcept { a = std::exp (-2.0f * kPi * std::clamp (f, 1.0f, float (sr * 0.49)) / float (sr)); }
    float process (float x) noexcept { z = x + a * (z - x); return z; }
    void reset() noexcept { z = 0.0f; }
};

/** Peak/RMS-ish envelope follower with separate attack and release. */
struct EnvelopeFollower
{
    float att = 0.0f, rel = 0.0f, env = 0.0f;
    void setTimes (double sr, float attackMs, float releaseMs) noexcept { att = onePoleCoeff (attackMs, sr); rel = onePoleCoeff (releaseMs, sr); }
    float process (float x) noexcept
    {
        const float r = std::abs (x);
        env = r > env ? r + att * (env - r) : r + rel * (env - r);
        return env;
    }
    void reset() noexcept { env = 0.0f; }
};

/** Sine/triangle LFO, phase in [0,1). */
struct Lfo
{
    float phase = 0.0f, inc = 0.0f;
    void setRate (double sr, float hz) noexcept { inc = float (hz / sr); }
    void reset (float startPhase = 0.0f) noexcept { phase = startPhase; }
    float nextSine() noexcept { const float v = std::sin (kTwoPi * phase); advance(); return v; }
    float nextTriangle() noexcept { const float v = 1.0f - 4.0f * std::abs (phase - 0.5f); advance(); return v; }
    void advance() noexcept { phase += inc; if (phase >= 1.0f) phase -= 1.0f; }
};

/** Circular delay line with fractional (linear or cubic) reads. prepare() allocates. */
class DelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        int size = 1;
        while (size < maxDelaySamples + 4) size <<= 1;
        buf.assign (size_t (size), 0.0f);
        mask = size - 1;
        w = 0;
    }
    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    void push (float x) noexcept { buf[size_t (w)] = x; w = (w + 1) & mask; }
    /** delay in samples, 0 = the sample just pushed. */
    float readInt (int d) const noexcept { return buf[size_t ((w - 1 - d) & mask)]; }
    float readLinear (float d) const noexcept
    {
        const int i = int (d);
        const float f = d - float (i);
        return readInt (i) + f * (readInt (i + 1) - readInt (i));
    }
    float readCubic (float d) const noexcept
    {
        const int i = int (d);
        const float f = d - float (i);
        const float y0 = readInt (i - 1), y1 = readInt (i), y2 = readInt (i + 1), y3 = readInt (i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    int capacity() const noexcept { return mask - 3; }

private:
    std::vector<float> buf;
    int mask = 0, w = 0;
};

/** Deterministic xorshift RNG (stutter seeds, noise). */
struct Rng
{
    uint32_t s = 0x12345678u;
    void seed (uint32_t v) noexcept { s = v ? v : 0x12345678u; }
    uint32_t nextU32() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float nextFloat01() noexcept { return float (nextU32() >> 8) * (1.0f / 16777216.0f); }
    float nextBipolar() noexcept { return nextFloat01() * 2.0f - 1.0f; }
};

/** Keeps a filtered/processed signal at the loudness of its input (100 ms RMS ratio, capped boost),
    so effects that remove a lot of the spectrum (band-pass, wah, vowel filter) "do not change the level"
    (spec §5.8). Holds the gain while the input is near silence. */
struct RmsMatch
{
    float coef = 0.0f, pIn = 0.0f, pOut = 0.0f, gain = 1.0f, maxGain = 1.0f;
    void prepare (double sr, float maxGainDb) noexcept { coef = onePoleCoeff (100.0f, sr); maxGain = dbToGain (maxGainDb); }
    void reset() noexcept { pIn = pOut = 0.0f; gain = 1.0f; }
    float process (float in, float processed) noexcept
    {
        pIn = in * in + coef * (pIn - in * in);
        pOut = processed * processed + coef * (pOut - processed * processed);
        if (pIn > 1.0e-8f) gain = std::min (maxGain, std::sqrt (pIn / (pOut + 1.0e-12f)));
        return processed * gain;
    }
};

inline float softClip (float x) noexcept { return std::tanh (x); }
} // namespace koe::dsp

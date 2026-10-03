#include "Dsp/PitchDetector.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace koe::dsp
{
namespace
{
constexpr double kAnalysisRate = 12000.0;
constexpr float kMpmK = 0.93f;         // key-maximum ratio
constexpr float kMpmClarity = 0.6f;    // NSDF peak needed to call a frame voiced
constexpr float kYinThreshold = 0.15f; // CMNDF absolute threshold
constexpr float kGateRms = 1.0e-3f;    // -60 dBFS over the newest 5 ms
constexpr double kPeriodsInWindow = 2.3;
constexpr double kTauMaxFraction = 0.6; // longest lag searched, as a fraction of the window
constexpr int kIntegration = 256;       // samples beyond the longest period in the full window
constexpr float kOnsetFraction = 1.0f / 32.0f; // -30 dB re the window peak: "not started yet"
} // namespace

PitchDetector::PitchDetector() = default;
PitchDetector::~PitchDetector() = default;

void PitchDetector::prepare (double sampleRate, const Settings& s)
{
    set = s;
    decim = std::max (1, int (std::lround (sampleRate / kAnalysisRate)));
    rate = sampleRate / decim;
    hop = std::max (1, s.hopSamples / decim);
    minLag = std::max (2, int (std::floor (rate / s.fmaxHz)));
    maxLag = int (std::ceil (rate / s.fminHz)) + 1;
    longN = kIntegration * int (rate / kAnalysisRate + 0.5) + maxLag;
    shortN = std::min (longN, std::max (int (rate * 0.0107), int (minLag * kPeriodsInWindow) + 2));
    gateN = std::max (8, int (rate * 0.005));

    int size = 1;
    while (size < longN + 8) size <<= 1;
    ring.assign (size_t (size), 0.0f);
    raw.assign (size_t (size), 0.0f);
    ringMask = size - 1;

    minOrder = 1;
    while ((1 << minOrder) < 2 * shortN) ++minOrder;
    int maxOrder = minOrder;
    while ((1 << maxOrder) < 2 * longN) ++maxOrder;
    ffts.clear();
    for (int o = minOrder; o <= maxOrder; ++o) ffts.push_back (std::make_unique<juce::dsp::FFT> (o));
    const int maxFft = 1 << maxOrder;
    win.assign (size_t (longN), 0.0f);
    bufA.assign (size_t (2 * maxFft), 0.0f);
    bufB.assign (size_t (2 * maxFft), 0.0f);
    curve.assign (size_t (maxLag + 3), 0.0f);
    prefix.assign (size_t (longN + 1), 0.0);

    const float aaHz = float (std::min (3000.0, rate * 0.25));
    aa1.setLowpass (sampleRate, aaHz, 0.5412f); // 4th-order Butterworth
    aa2.setLowpass (sampleRate, aaHz, 1.3066f);
    reset();
}

void PitchDetector::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (raw.begin(), raw.end(), 0.0f);
    ringW = 0;
    decimPhase = 0;
    hopCount = 0;
    aa1.reset();
    aa2.reset();
    curN = longN;
    f0 = 0.0f;
    clarity = 0.0f;
}

void PitchDetector::process (const float* x, int numSamples)
{
    juce::ScopedNoDenormals noDenormals;
    for (int i = 0; i < numSamples; ++i)
    {
        const float y = aa2.process (aa1.process (x[i]));
        if (++decimPhase < decim) continue;
        decimPhase = 0;
        ring[size_t (ringW)] = y;
        raw[size_t (ringW)] = x[i];
        ringW = (ringW + 1) & ringMask;
        if (++hopCount >= hop)
        {
            hopCount = 0;
            detect();
        }
    }
}

juce::dsp::FFT& PitchDetector::fftFor (int n) const noexcept
{
    int o = minOrder;
    while ((1 << o) < 2 * n && o - minOrder + 1 < int (ffts.size())) ++o;
    return *ffts[size_t (o - minOrder)];
}

void PitchDetector::detect()
{
    // gate on the raw input (the anti-alias filters must not hold a stopped note open)
    double e = 0.0;
    for (int k = 1; k <= gateN; ++k)
    {
        const double v = raw[size_t ((ringW - k) & ringMask)];
        e += v * v;
    }
    float lag = 0.0f;
    clarity = 0.0f;
    if (std::sqrt (e / gateN) >= kGateRms)
    {
        // Leave out a quiet start of the window (an onset): with part of the window empty, MPM / YIN read
        // the period short and only get within 10 cents 1-2 periods later.
        int n = curN;
        float peak = 0.0f;
        for (int k = 0; k < n; ++k) peak = std::max (peak, std::abs (ring[size_t ((ringW - n + k) & ringMask)]));
        while (n > 0 && std::abs (ring[size_t ((ringW - n) & ringMask)]) < peak * kOnsetFraction) --n;
        double mean = 0.0;
        for (int k = 0; k < n; ++k) mean += win[size_t (k)] = ring[size_t ((ringW - n + k) & ringMask)];
        mean /= std::max (1, n);
        for (int k = 0; k < n; ++k) win[size_t (k)] -= float (mean); // DC (no high-pass: its onset transient biases the first periods)
        const int tauMax = std::min (maxLag, int (n * kTauMaxFraction));
        if (tauMax >= minLag + 2)
            lag = set.method == Method::mpm ? detectMpm (n, tauMax) : detectYin (n, tauMax);
    }

    if (lag > 0.0f)
    {
        f0 = float (rate / lag);
        curN = std::clamp (int (kPeriodsInWindow * lag) + 1, shortN, longN);
    }
    else
    {
        f0 = 0.0f;
        curN = longN;
    }
}

float PitchDetector::vertex (int tau) const noexcept
{
    const float y0 = curve[size_t (tau - 1)], y1 = curve[size_t (tau)], y2 = curve[size_t (tau + 1)];
    const float den = y0 - 2.0f * y1 + y2;
    const float shift = std::abs (den) > 1.0e-12f ? 0.5f * (y0 - y2) / den : 0.0f;
    return float (tau) + std::clamp (shift, -1.0f, 1.0f);
}

float PitchDetector::detectMpm (int n, int tauMax)
{
    auto& fft = fftFor (n);
    const int size = fft.getSize();
    // type-II ACF r'(tau) = sum_{j<n-tau} x[j] x[j+tau] = IFFT (|FFT (x)|^2), zero padded to >= 2n
    std::fill (bufA.begin(), bufA.begin() + 2 * size, 0.0f);
    std::copy (win.begin(), win.begin() + n, bufA.begin());
    fft.performRealOnlyForwardTransform (bufA.data(), true);
    for (int k = 0; k <= size / 2; ++k)
    {
        const float re = bufA[size_t (2 * k)], im = bufA[size_t (2 * k + 1)];
        bufA[size_t (2 * k)] = re * re + im * im;
        bufA[size_t (2 * k + 1)] = 0.0f;
    }
    fft.performRealOnlyInverseTransform (bufA.data());

    prefix[0] = 0.0;
    for (int j = 0; j < n; ++j) prefix[size_t (j + 1)] = prefix[size_t (j)] + double (win[size_t (j)]) * win[size_t (j)];
    const double total = prefix[size_t (n)];
    for (int tau = 0; tau <= tauMax + 1; ++tau)
    {
        const double m = prefix[size_t (n - tau)] + (total - prefix[size_t (tau)]);
        curve[size_t (tau)] = m > 1.0e-20 ? float (2.0 * double (bufA[size_t (tau)]) / m) : 0.0f;
    }

    // key maxima: the highest point of each positive region after the lobe around lag 0
    int keys[64];
    int numKeys = 0;
    float highest = 0.0f;
    int tau = 1;
    while (tau <= tauMax && curve[size_t (tau)] > 0.0f) ++tau;
    while (tau <= tauMax && numKeys < 64)
    {
        while (tau <= tauMax && curve[size_t (tau)] <= 0.0f) ++tau;
        int peak = -1;
        while (tau <= tauMax && curve[size_t (tau)] > 0.0f)
        {
            if (tau >= minLag && (peak < 0 || curve[size_t (tau)] > curve[size_t (peak)])) peak = tau;
            ++tau;
        }
        if (peak > 0 && peak < tauMax) // a region cut off by tauMax may peak beyond it
        {
            keys[numKeys++] = peak;
            highest = std::max (highest, curve[size_t (peak)]);
        }
    }
    clarity = highest;
    if (numKeys == 0 || highest < kMpmClarity) return 0.0f;
    for (int i = 0; i < numKeys; ++i)
        if (curve[size_t (keys[i])] >= kMpmK * highest) return vertex (keys[i]);
    return 0.0f;
}

float PitchDetector::detectYin (int n, int tauMax)
{
    auto& fft = fftFor (n);
    const int size = fft.getSize();
    const int w = n - tauMax; // integration window: the newest w samples, compared with tau samples earlier
    // b = the window newest first, a = b[0..w); r(tau) = sum_{j<w} a[j] b[j+tau] = IFFT (conj (A) B)
    std::fill (bufA.begin(), bufA.begin() + 2 * size, 0.0f);
    std::fill (bufB.begin(), bufB.begin() + 2 * size, 0.0f);
    for (int j = 0; j < n; ++j) bufB[size_t (j)] = win[size_t (n - 1 - j)];
    std::copy (bufB.begin(), bufB.begin() + w, bufA.begin());
    fft.performRealOnlyForwardTransform (bufA.data(), true);
    fft.performRealOnlyForwardTransform (bufB.data(), true);
    for (int k = 0; k <= size / 2; ++k)
    {
        const float ar = bufA[size_t (2 * k)], ai = bufA[size_t (2 * k + 1)];
        const float br = bufB[size_t (2 * k)], bi = bufB[size_t (2 * k + 1)];
        bufA[size_t (2 * k)] = ar * br + ai * bi;
        bufA[size_t (2 * k + 1)] = ar * bi - ai * br;
    }
    fft.performRealOnlyInverseTransform (bufA.data());

    // prefix over b (newest first)
    prefix[0] = 0.0;
    for (int j = 0; j < n; ++j)
    {
        const double v = win[size_t (n - 1 - j)];
        prefix[size_t (j + 1)] = prefix[size_t (j)] + v * v;
    }
    const double e0 = prefix[size_t (w)];
    double running = 0.0;
    curve[0] = 1.0f;
    for (int tau = 1; tau <= tauMax; ++tau) // tauMax + 1 would read prefix[n + 1], one past the end when n == longN
    {
        const double et = prefix[size_t (tau + w)] - prefix[size_t (tau)];
        const double d = std::max (0.0, e0 + et - 2.0 * double (bufA[size_t (tau)]));
        running += d;
        curve[size_t (tau)] = running > 0.0 ? float (d * tau / running) : 1.0f;
    }
    int best = -1;
    for (int tau = minLag; tau <= tauMax; ++tau)
    {
        if (curve[size_t (tau)] < kYinThreshold)
        {
            while (tau + 1 <= tauMax && curve[size_t (tau + 1)] < curve[size_t (tau)]) ++tau;
            best = tau < tauMax ? tau : -1; // still falling at tauMax: the dip lies beyond the range
            break;
        }
    }
    if (best < 0)
    {
        float m = 1.0f;
        for (int tau = minLag; tau <= tauMax; ++tau) m = std::min (m, curve[size_t (tau)]);
        clarity = std::max (0.0f, 1.0f - m);
        return 0.0f;
    }
    clarity = std::max (0.0f, 1.0f - curve[size_t (best)]);
    return vertex (best);
}

float scaleShiftSemitones (float midiNote, int key, Scale scale, int degree) noexcept
{
    static constexpr int kChromatic[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    static constexpr int kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinor[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* steps = scale == Scale::major ? kMajor : (scale == Scale::minor ? kMinor : kChromatic);
    const int count = scale == Scale::chromatic ? 12 : 7;
    auto noteOf = [&] (int index) // scale step index (any integer) -> MIDI note
    {
        const int oct = index >= 0 ? index / count : -((-index + count - 1) / count);
        return key + 12 * oct + steps[index - count * oct];
    };
    // nearest scale step among the steps of the octave around the note (and one either side)
    const int base = count * int (std::floor ((midiNote - float (key)) / 12.0f));
    int best = base;
    float bestDist = 1.0e9f;
    for (int i = base - 1; i <= base + count; ++i)
    {
        const float d = std::abs (float (noteOf (i)) - midiNote);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return float (noteOf (best + degree)) - midiNote;
}
} // namespace koe::dsp

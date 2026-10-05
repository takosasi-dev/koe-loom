#include "Engine/Spectrum.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
const double kLogSpan = std::log (double (kSpectrumHighHz) / double (kSpectrumLowHz));

double bandEdgeHz (int k) noexcept { return kSpectrumLowHz * std::exp (kLogSpan * double (k) / double (kSpectrumBands)); }
} // namespace

float SpectrumAnalyser::bandCentreHz (int band) noexcept
{
    return float (kSpectrumLowHz * std::exp (kLogSpan * (double (band) + 0.5) / double (kSpectrumBands)));
}

int SpectrumAnalyser::bandFor (float hz) noexcept
{
    if (! (hz >= kSpectrumLowHz && hz < kSpectrumHighHz)) return -1;
    const int b = int (std::floor (double (kSpectrumBands) * std::log (double (hz) / double (kSpectrumLowHz)) / kLogSpan));
    return juce::jlimit (0, kSpectrumBands - 1, b);
}

int SpectrumAnalyser::windowSize (double sampleRate) noexcept
{
    return 1 << juce::jlimit (10, 14, juce::roundToInt (std::log2 (std::max (1.0, sampleRate) * 0.085)));
}

void SpectrumAnalyser::prepare (double sampleRate)
{
    preparedRate = sampleRate;
    n = windowSize (sampleRate);
    const int fftSize = 2 * n; // zero-padded: smoother bands below ~200 Hz, where a band is narrower than a bin
    fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (double (fftSize))));
    window.resize (size_t (n));
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        window[size_t (i)] = 0.5f - 0.5f * std::cos (2.0f * dsp::kPi * float (i) / float (n));
        sum += window[size_t (i)];
    }
    magToAmp = float (2.0 / sum);
    work.assign (size_t (fftSize) * 2, 0.0f);
    frame.assign (size_t (n), 0.0f);

    const double binHz = sampleRate / double (fftSize), nyquist = sampleRate * 0.5;
    for (int b = 0; b < kSpectrumBands; ++b)
    {
        const double centre = bandCentreHz (b);
        if (centre >= nyquist)
        {
            binLo[size_t (b)] = 1;
            binHi[size_t (b)] = 0;
            binAt[size_t (b)] = -1.0f;
            continue;
        }
        binLo[size_t (b)] = int (std::ceil (bandEdgeHz (b) / binHz));                   // bins whose frequency is in
        binHi[size_t (b)] = std::min (n, int (std::ceil (bandEdgeHz (b + 1) / binHz)) - 1); // [low edge, high edge)
        binAt[size_t (b)] = float (centre / binHz);
    }
}

bool SpectrumAnalyser::measure (const float* x, int numSamples, double sampleRate, SpectrumBands& dbOut)
{
    if (! (sampleRate > 0.0) || x == nullptr) return false;
    if (sampleRate != preparedRate) prepare (sampleRate);
    if (numSamples < n) return false;

    const float* newest = x + (numSamples - n);
    for (int i = 0; i < n; ++i)
    {
        const float v = newest[i];
        work[size_t (i)] = (std::isfinite (v) ? v : 0.0f) * window[size_t (i)];
    }
    std::fill (work.begin() + n, work.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (work.data(), true); // magnitudes of bins 0..n

    for (int b = 0; b < kSpectrumBands; ++b)
    {
        const float at = binAt[size_t (b)];
        if (at < 0.0f)
        {
            dbOut[size_t (b)] = kFloorDb;
            continue;
        }
        float m = 0.0f;
        if (binLo[size_t (b)] <= binHi[size_t (b)]) // the loudest bin in the band: a sine reads its level in any band
        {
            for (int k = binLo[size_t (b)]; k <= binHi[size_t (b)]; ++k) m = std::max (m, work[size_t (k)]);
        }
        else // narrower than a bin: between the two bins around the centre
        {
            const int k0 = std::min (n, int (at));
            const float t = juce::jlimit (0.0f, 1.0f, at - float (k0));
            m = work[size_t (k0)] * (1.0f - t) + work[size_t (std::min (n, k0 + 1))] * t;
        }
        const float amp = m * magToAmp;
        const float db = amp > 0.0f ? 20.0f * std::log10 (amp) : kFloorDb;
        dbOut[size_t (b)] = std::isfinite (db) ? std::max (kFloorDb, db) : kFloorDb;
    }
    return true;
}

bool SpectrumAnalyser::poll (const TapRing& ring, double sampleRate, SpectrumBands& out)
{
    if (sampleRate > 0.0 && sampleRate != preparedRate)
    {
        prepare (sampleRate);
        reset();
    }
    const long long written = ring.getWritten();
    if (written < lastWritten) reset(); // the ring was cleared
    if (n > 0 && written != lastWritten && written >= n && ring.readLatest (frame.data(), n))
    {
        lastWritten = written;
        stale = 0;
        SpectrumBands now;
        measure (frame.data(), n, sampleRate, now);
        if (! hasHeld)
            held = now;
        else
            for (size_t b = 0; b < held.size(); ++b)
                held[b] += (now[b] - held[b]) * (now[b] > held[b] ? kAttack : kRelease);
        hasHeld = true;
    }
    else if (hasHeld && written == lastWritten && ++stale > kStalePolls)
    {
        hasHeld = false;
    }

    if (! hasHeld)
    {
        out.fill (kFloorDb);
        return false;
    }
    out = held;
    return true;
}

void SpectrumAnalyser::reset() noexcept
{
    hasHeld = false;
    lastWritten = -1;
    stale = 0;
}

void SpectrumAnalyser::demoSpectra (SpectrumBands& in, SpectrumBands& out)
{
    // harmonics of f0 falling ~9 dB / octave, an "a" vowel's formants, breath noise at the top
    auto voice = [] (SpectrumBands& bands, double f0, double formantScale, double brightDb)
    {
        static constexpr double formantHz[] = { 750.0, 1200.0, 2600.0, 3500.0 };
        static constexpr double formantDb[] = { 16.0, 12.0, 8.0, 5.0 };
        for (int i = 0; i < kSpectrumBands; ++i)
        {
            const double f = bandCentreHz (i);
            double db = f >= f0 * 0.94 ? -24.0 - 9.0 * std::log2 (f / f0) : -24.0 - 30.0 * std::log2 (f0 * 0.94 / f);
            for (size_t k = 0; k < std::size (formantHz); ++k)
            {
                const double d = std::log2 (f / (formantHz[k] * formantScale)) / 0.22;
                db += formantDb[k] * std::exp (-d * d);
            }
            if (f > 3000.0) db += brightDb;
            // separate harmonics where they are further apart than a band, a smooth envelope above
            const double h = std::max (1.0, f / f0);
            const double resolved = juce::jlimit (0.0, 1.0, (12.0 * std::log2 ((h + 1.0) / h) - 1.2) / 3.0);
            db -= 26.0 * resolved * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * f / f0));
            const double noiseDb = -80.0 + 3.0 * std::sin (double (i) * 2.7) * std::cos (double (i) * 1.3) + brightDb;
            db = 10.0 * std::log10 (std::pow (10.0, db / 10.0) + std::pow (10.0, noiseDb / 10.0));
            bands[size_t (i)] = float (std::max (double (kFloorDb), db));
        }
    };
    voice (in, 135.0, 1.0, 0.0);
    voice (out, 180.0, 1.15, 4.0); // +5 semitones, formants up, brighter: a typical "higher voice" preset
}
} // namespace koe

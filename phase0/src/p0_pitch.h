#pragma once

// Own implementations (no external code) of the two pitch detectors compared in T6 (spec §5.7):
//   YIN  (de Cheveigne & Kawahara 2002): difference function -> cumulative mean normalised
//        difference (CMNDF) -> absolute threshold -> parabolic interpolation.
//   MPM  (McLeod & Wyvill 2005): normalised square difference function (NSDF, type-II ACF) ->
//        key maxima between zero crossings -> first key maximum >= k * highest -> parabolic interpolation.
// Both use the same analysis buffer (N newest samples) and FFT-based correlation, so latency and
// CPU differences come from the algorithms only. prepare() allocates; detect() does not.

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace p0
{
struct PitchSettings
{
    double sampleRate = 48000.0;
    double fminHz = 60.0;      // spec §5.7: 60..1000 Hz [暫定]
    double fmaxHz = 1000.0;
    int integrationSamples = 1024;  // YIN sum length; buffer N = integration + maxLag
    float yinThreshold = 0.15f;     // CMNDF absolute threshold
    float mpmK = 0.93f;             // key-maximum ratio
    float mpmClarity = 0.6f;        // NSDF peak needed to call the frame voiced
};

class PitchDetectorBase
{
public:
    virtual ~PitchDetectorBase() = default;

    void prepare (const PitchSettings& s)
    {
        set = s;
        maxLag = int (std::ceil (s.sampleRate / s.fminHz)) + 1;
        minLag = std::max (2, int (std::floor (s.sampleRate / s.fmaxHz)));
        n = s.integrationSamples + maxLag;
        int order = 1;
        while ((1 << order) < 2 * n) ++order;
        fftSize = 1 << order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        bufA.assign (size_t (2 * fftSize), 0.0f);
        bufB.assign (size_t (2 * fftSize), 0.0f);
        prefix.assign (size_t (n + 1), 0.0);
        curve.assign (size_t (maxLag + 2), 0.0f);
    }

    /** Samples the detector looks at (the newest `bufferSamples()` input samples). */
    int bufferSamples() const { return n; }

    /** x = the newest bufferSamples() samples, oldest first. Returns f0 in Hz, or 0 when unvoiced. */
    virtual double detect (const float* x) = 0;

    /** 0..1 confidence of the last detect(). */
    double lastClarity = 0.0;

protected:
    void prefixSums (const float* x)
    {
        prefix[0] = 0.0;
        for (int i = 0; i < n; ++i) prefix[size_t (i + 1)] = prefix[size_t (i)] + double (x[i]) * x[i];
    }

    double parabolicLag (int tau) const
    {
        const double y0 = curve[size_t (tau - 1)], y1 = curve[size_t (tau)], y2 = curve[size_t (tau + 1)];
        const double den = y0 - 2.0 * y1 + y2;
        const double shift = std::abs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        return tau + std::clamp (shift, -1.0, 1.0);
    }

    PitchSettings set;
    int maxLag = 0, minLag = 0, n = 0, fftSize = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> bufA, bufB, curve;
    std::vector<double> prefix;
};

class YinDetector final : public PitchDetectorBase
{
public:
    double detect (const float* x) override
    {
        const int w = set.integrationSamples;
        prefixSums (x);
        // r(tau) = sum_{j<w} x[j] x[j+tau]  via  IFFT(conj(FFT(a)) * FFT(b)), a = x[0..w), b = x[0..n)
        std::fill (bufA.begin(), bufA.end(), 0.0f);
        std::fill (bufB.begin(), bufB.end(), 0.0f);
        std::copy (x, x + w, bufA.begin());
        std::copy (x, x + n, bufB.begin());
        fft->performRealOnlyForwardTransform (bufA.data(), true);
        fft->performRealOnlyForwardTransform (bufB.data(), true);
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float ar = bufA[size_t (2 * k)], ai = bufA[size_t (2 * k + 1)];
            const float br = bufB[size_t (2 * k)], bi = bufB[size_t (2 * k + 1)];
            bufA[size_t (2 * k)] = ar * br + ai * bi;       // conj(A) * B
            bufA[size_t (2 * k + 1)] = ar * bi - ai * br;
        }
        fft->performRealOnlyInverseTransform (bufA.data());

        const double e0 = prefix[size_t (w)];
        double running = 0.0;
        curve[0] = 1.0f;
        for (int tau = 1; tau <= maxLag + 1 && tau + w <= n; ++tau)
        {
            const double etau = prefix[size_t (tau + w)] - prefix[size_t (tau)];
            const double d = std::max (0.0, e0 + etau - 2.0 * double (bufA[size_t (tau)]));
            running += d;
            curve[size_t (tau)] = running > 0.0 ? float (d * tau / running) : 1.0f;
        }
        const int last = std::min (maxLag, n - w - 1);
        int best = -1;
        for (int tau = minLag; tau < last; ++tau)
        {
            if (curve[size_t (tau)] < set.yinThreshold)
            {
                while (tau + 1 < last && curve[size_t (tau + 1)] < curve[size_t (tau)]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
        {
            float m = 1e9f;
            for (int tau = minLag; tau < last; ++tau) m = std::min (m, curve[size_t (tau)]);
            lastClarity = std::max (0.0, 1.0 - double (m));
            return 0.0;
        }
        lastClarity = std::max (0.0, 1.0 - double (curve[size_t (best)]));
        return set.sampleRate / parabolicLag (best);
    }
};

class MpmDetector final : public PitchDetectorBase
{
public:
    double detect (const float* x) override
    {
        prefixSums (x);
        // type-II ACF r'(tau) = sum_{j<n-tau} x[j] x[j+tau] = IFFT(|FFT(x)|^2)
        std::fill (bufB.begin(), bufB.end(), 0.0f);
        std::copy (x, x + n, bufB.begin());
        fft->performRealOnlyForwardTransform (bufB.data(), true);
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float re = bufB[size_t (2 * k)], im = bufB[size_t (2 * k + 1)];
            bufB[size_t (2 * k)] = re * re + im * im;
            bufB[size_t (2 * k + 1)] = 0.0f;
        }
        fft->performRealOnlyInverseTransform (bufB.data());

        const double total = prefix[size_t (n)];
        for (int tau = 0; tau <= maxLag + 1; ++tau)
        {
            const double m = prefix[size_t (n - tau)] + (total - prefix[size_t (tau)]);
            curve[size_t (tau)] = m > 1e-20 ? float (2.0 * double (bufB[size_t (tau)]) / m) : 0.0f;
        }
        // key maxima: highest point between each positive-going zero crossing and the next negative-going one
        int keys[64];
        int numKeys = 0;
        int tau = 1;
        while (tau <= maxLag && curve[size_t (tau)] > 0.0f) ++tau;      // leave the lobe around lag 0
        float highest = 0.0f;
        while (tau <= maxLag && numKeys < 64)
        {
            while (tau <= maxLag && curve[size_t (tau)] <= 0.0f) ++tau; // to the next positive region
            int peak = -1;
            while (tau <= maxLag && curve[size_t (tau)] > 0.0f)
            {
                if (tau >= minLag && (peak < 0 || curve[size_t (tau)] > curve[size_t (peak)])) peak = tau;
                ++tau;
            }
            if (peak > 0 && peak < maxLag + 1)
            {
                keys[numKeys++] = peak;
                highest = std::max (highest, curve[size_t (peak)]);
            }
        }
        lastClarity = highest;
        if (numKeys == 0 || highest < set.mpmClarity) return 0.0;
        for (int i = 0; i < numKeys; ++i)
            if (curve[size_t (keys[i])] >= set.mpmK * highest)
                return set.sampleRate / parabolicLag (keys[i]);
        return 0.0;
    }
};
} // namespace p0

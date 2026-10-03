#include "Tests/TestUtil.h"

#include "Dsp/Building.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace koe::test
{
std::vector<float> sine (double hz, double seconds, float amplitude, double sr)
{
    std::vector<float> v (size_t (seconds * sr));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = amplitude * float (std::sin (2.0 * juce::MathConstants<double>::pi * hz * double (i) / sr));
    return v;
}

std::vector<float> silence (double seconds, double sr) { return std::vector<float> (size_t (seconds * sr), 0.0f); }

std::vector<float> whiteNoise (double seconds, float amplitude, uint32_t seed, double sr)
{
    dsp::Rng rng;
    rng.seed (seed);
    std::vector<float> v (size_t (seconds * sr));
    for (auto& s : v) s = amplitude * rng.nextBipolar();
    return v;
}

std::vector<float> synthVoice (double seconds, uint32_t seed, double sr, bool consonants)
{
    const size_t n = size_t (seconds * sr);
    std::vector<float> v (n);
    dsp::Rng rng;
    rng.seed (seed);

    // vowel formant targets (Hz): a i u e o (rough Japanese male averages)
    const float F[5][3] = { { 800, 1200, 2500 }, { 300, 2300, 3000 }, { 350, 1300, 2400 }, { 500, 1900, 2600 }, { 500, 900, 2400 } };
    dsp::Biquad f1, f2, f3, cons;
    double phase = 0.0;
    float cur[3] = { F[0][0], F[0][1], F[0][2] };
    int vowel = 0;
    const int segment = int (sr * 0.18);

    for (size_t i = 0; i < n; ++i)
    {
        const double t = double (i) / sr;
        if (int (i) % segment == 0) vowel = int (rng.nextU32() % 5);
        for (int k = 0; k < 3; ++k) cur[k] += 0.002f * (F[vowel][k] - cur[k]);
        if (i % 32 == 0)
        {
            f1.setBandpass (sr, cur[0], 6.0f);
            f2.setBandpass (sr, cur[1], 8.0f);
            f3.setBandpass (sr, cur[2], 10.0f);
            cons.setBandpass (sr, 5000.0f, 1.5f);
        }
        // f0: slow glide 105..175 Hz plus 5 Hz vibrato
        const double f0 = 140.0 + 35.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.23 * t) + 2.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.0 * t);
        phase += f0 / sr;
        if (phase >= 1.0) phase -= 1.0;
        // band-limited-ish glottal pulse: sum of harmonics with 1/k^1.2 roll-off up to 4 kHz
        float src = 0.0f;
        const int harmonics = std::min (40, int (4000.0 / f0));
        for (int h = 1; h <= harmonics; ++h)
            src += float (std::sin (2.0 * juce::MathConstants<double>::pi * h * phase) / std::pow (double (h), 1.2));
        const float voiced = f1.process (src) * 1.0f + f2.process (src) * 0.6f + f3.process (src) * 0.3f;
        // syllable envelope ~4 Hz, floor -26 dB so there is never a silent gap
        const float env = 0.05f + 0.95f * float (0.5 + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 3.7 * t));
        const float consonant = (consonants && std::fmod (t, 0.6) < 0.04) ? cons.process (rng.nextBipolar()) * 0.5f : cons.process (0.0f);
        v[i] = voiced * env + consonant;
    }
    float peak = 0.0f;
    for (float s : v) peak = std::max (peak, std::abs (s));
    if (peak > 0.0f)
        for (auto& s : v) s *= 0.5f / peak; // -6 dBFS
    return v;
}

std::vector<float> referenceSpeechOrSynth (bool* isReal)
{
    auto path = juce::SystemStats::getEnvironmentVariable ("KOELOOM_REFERENCE_WAV", {});
    if (path.isNotEmpty())
    {
        juce::File f (path);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
        {
            juce::AudioBuffer<float> b (int (r->numChannels), int (r->lengthInSamples));
            r->read (&b, 0, int (r->lengthInSamples), 0, true, true);
            std::vector<float> v (size_t (b.getNumSamples()));
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float s = 0.0f;
                for (int c = 0; c < b.getNumChannels(); ++c) s += b.getSample (c, i);
                v[size_t (i)] = s / float (b.getNumChannels());
            }
            if (std::abs (r->sampleRate - kSr) < 1.0)
            {
                if (isReal) *isReal = true;
                return v;
            }
        }
    }
    if (isReal) *isReal = false;
    return synthVoice (10.0);
}

std::vector<float> concat (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> v;
    for (auto& p : parts) v.insert (v.end(), p.begin(), p.end());
    return v;
}

float rmsDb (const float* x, int n)
{
    if (n <= 0) return -120.0f;
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += double (x[i]) * x[i];
    const double r = std::sqrt (s / n);
    return r <= 1e-9 ? -120.0f : float (20.0 * std::log10 (r));
}

float peakDb (const float* x, int n)
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max (p, std::abs (x[i]));
    return dsp::gainToDb (p);
}

bool allFinite (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (x[i])) return false;
    return true;
}

double estimateF0 (const float* x, int n, double sr, double fmin, double fmax)
{
    const int maxLag = std::min (n / 2, int (sr / fmin));
    const int minLag = std::max (2, int (sr / fmax));
    const int w = n - maxLag;
    if (w <= minLag) return 0.0;
    std::vector<double> d (size_t (maxLag + 2), 0.0);
    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        double s = 0.0;
        for (int j = 0; j < w; ++j)
        {
            const double diff = double (x[j]) - double (x[j + tau]);
            s += diff * diff;
        }
        d[size_t (tau)] = s;
    }
    // cumulative mean normalised difference
    std::vector<double> c (d.size(), 1.0);
    double run = 0.0;
    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        run += d[size_t (tau)];
        c[size_t (tau)] = run > 0.0 ? d[size_t (tau)] * tau / run : 1.0;
    }
    int best = -1;
    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (c[size_t (tau)] < 0.15)
        {
            while (tau + 1 <= maxLag && c[size_t (tau + 1)] < c[size_t (tau)]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        double m = 1e9;
        for (int tau = minLag; tau <= maxLag; ++tau)
            if (c[size_t (tau)] < m) { m = c[size_t (tau)]; best = tau; }
        if (m > 0.5) return 0.0;
    }
    const double y0 = c[size_t (best - 1)], y1 = c[size_t (best)], y2 = c[size_t (best + 1)];
    const double den = y0 - 2 * y1 + y2;
    const double shift = std::abs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
    return sr / (best + std::clamp (shift, -1.0, 1.0));
}

double centsBetween (double f, double reference)
{
    if (f <= 0.0 || reference <= 0.0) return 1.0e9;
    return 1200.0 * std::log2 (f / reference);
}

static float maxAdjacentDiff (const std::vector<float>& v, int a, int b)
{
    a = std::max (a, 1);
    b = std::min (b, int (v.size()));
    float m = 0.0f;
    for (int i = a; i < b; ++i) m = std::max (m, std::abs (v[size_t (i)] - v[size_t (i - 1)]));
    return m;
}

double clickRatio (const std::vector<float>& out, int opStart, int opEnd, double sr)
{
    const int margin = int (0.020 * sr);
    const int win = int (0.100 * sr);
    const float during = maxAdjacentDiff (out, opStart - margin, opEnd + margin);
    const float before = maxAdjacentDiff (out, opStart - margin - win, opStart - margin);
    const float after = maxAdjacentDiff (out, opEnd + margin, opEnd + margin + win);
    const float ref = std::max (before, after);
    if (ref <= 1.0e-9f) return during <= 1.0e-9f ? 0.0 : 1.0e9;
    return double (during / ref);
}

int findLag (const std::vector<float>& ref, const std::vector<float>& sig, int maxLag)
{
    int best = 0;
    double bestScore = -1e30;
    const int n = int (std::min (ref.size(), sig.size())) - maxLag;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double s = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = ref[size_t (i)], b = sig[size_t (i + lag)];
            s += a * b; e1 += a * a; e2 += b * b;
        }
        const double score = s / std::sqrt (std::max (1e-20, e1 * e2));
        if (score > bestScore) { bestScore = score; best = lag; }
    }
    return best;
}

bool writeWav (const juce::File& file, const std::vector<float>& samples, double sr)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
    if (os == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
    if (w == nullptr) return false;
    os.release();
    const float* ch[] = { samples.data() };
    return w->writeFromFloatArrays (ch, 1, int (samples.size()));
}
} // namespace koe::test

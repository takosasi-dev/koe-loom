// マイクの癖の補正 (INTERFACES.md §11.3). Owner: wave9/voice.

#include "Engine/MicEq.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace koe
{
namespace
{
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kQ = 2.0f;               // about 0.7 octave wide: neighbours overlap, so a smooth curve has no ripple
constexpr float kMaxFilterDb = 15.0f;    // a single filter after solving (the summed response stays within kMicEqMaxDb)
constexpr double kTopFraction = 0.45;    // bands above this x rate are left out

// ANSI S3.5-1997 (R2017) "Methods for Calculation of the Speech Intelligibility Index", Table 3: one-third octave band
// centre frequencies and the standard speech spectrum level (dB) for normal vocal effort. Checked on 2026-10-04 in two
// independent copies that agree to the last digit: the MATLAB SII.m of the Speech-enhancement repository
// (github.com/jtkim-kaist/Speech-enhancement, ...objective_measures/intelligibility/SII.m, `EV = SpV(:,1)'`) and the
// "normal" column of data/onethird.rda in the CRAN package SII 1.0.3 (github.com/cran/SII).
constexpr float kAnsiHz[] = { 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000 };
constexpr float kAnsiNormalDb[] = { 32.41f, 34.48f, 34.75f, 33.98f, 34.59f, 34.27f, 32.06f, 28.30f, 25.01f, 23.00f,
                                    20.15f, 17.32f, 13.18f, 11.55f, 9.33f, 5.31f, 2.59f, 1.13f };
constexpr int kAnsiN = int (std::size (kAnsiHz));

bool usable (float hz, double rate) { return double (hz) < kTopFraction * rate; }

float biquadDbAt (const dsp::Biquad& q, double w)
{
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    const auto h = (double (q.b0) + double (q.b1) * z1 + double (q.b2) * z2) / (1.0 + double (q.a1) * z1 + double (q.a2) * z2);
    return float (20.0 * std::log10 (std::max (1.0e-12, std::abs (h))));
}

/** Filters for the bands below 0.45 x rate; returns how many. */
int designBands (std::array<dsp::Biquad, kMicEqBands>& bands, const MicEqBands& filterDb, double rate)
{
    int count = 0;
    for (int b = 0; b < kMicEqBands && usable (kMicEqBandHz[b], rate); ++b, ++count)
        bands[size_t (b)].setPeak (rate, kMicEqBandHz[b], kQ, filterDb[size_t (b)]);
    return count;
}

float cascadeDbAt (const std::array<dsp::Biquad, kMicEqBands>& bands, int count, double rate, double hz)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * hz / rate;
    float db = 0.0f;
    for (int b = 0; b < count; ++b) db += biquadDbAt (bands[size_t (b)], w);
    return db;
}

/** Solves A x = y in place (Gaussian elimination with partial pivoting); n <= kMicEqBands. */
void solve (std::array<std::array<double, kMicEqBands>, kMicEqBands> a, std::array<double, kMicEqBands>& y, int n)
{
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs (a[size_t (r)][size_t (col)]) > std::abs (a[size_t (pivot)][size_t (col)])) pivot = r;
        std::swap (a[size_t (col)], a[size_t (pivot)]);
        std::swap (y[size_t (col)], y[size_t (pivot)]);
        const double d = a[size_t (col)][size_t (col)];
        if (std::abs (d) < 1.0e-12) continue;
        for (int r = 0; r < n; ++r)
        {
            if (r == col) continue;
            const double f = a[size_t (r)][size_t (col)] / d;
            if (f == 0.0) continue;
            for (int k = col; k < n; ++k) a[size_t (r)][size_t (k)] -= f * a[size_t (col)][size_t (k)];
            y[size_t (r)] -= f * y[size_t (col)];
        }
    }
    for (int r = 0; r < n; ++r)
    {
        const double d = a[size_t (r)][size_t (r)];
        y[size_t (r)] = std::abs (d) < 1.0e-12 ? 0.0 : y[size_t (r)] / d;
    }
}
} // namespace

// ============================================================================ measuring
MicEqBands micEqTargetDb()
{
    MicEqBands t;
    for (int b = 0; b < kMicEqBands; ++b)
    {
        const float hz = kMicEqBandHz[b];
        t[size_t (b)] = kNaN;
        if (hz < kAnsiHz[0] || hz > kAnsiHz[kAnsiN - 1]) continue; // outside the table: no guessing
        for (int k = 0; k + 1 < kAnsiN; ++k)
            if (hz >= kAnsiHz[k] && hz <= kAnsiHz[k + 1])
            {
                const float f = std::log (hz / kAnsiHz[k]) / std::log (kAnsiHz[k + 1] / kAnsiHz[k]);
                t[size_t (b)] = kAnsiNormalDb[k] + f * (kAnsiNormalDb[k + 1] - kAnsiNormalDb[k]);
                break;
            }
    }
    return t;
}

MicEqBands micEqBandLevelsDb (const std::vector<float>& x, double rate)
{
    MicEqBands out;
    out.fill (kNaN);
    if (rate <= 0.0) return out;
    const int order = juce::jlimit (10, 14, juce::roundToInt (std::log2 (rate * 0.085))); // ~85 ms: 4096 at 48 kHz
    const int n = 1 << order, hop = n / 2;
    if (int (x.size()) < n) return out;

    // which frames are voice: within 30 dB of the loudest and above -50 dBFS (checkCalibrationTake's rule)
    const int frames = 1 + (int (x.size()) - n) / hop;
    std::vector<float> frameDb (size_t (frames), -300.0f);
    float loudest = -300.0f;
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0.0;
        const float* p = x.data() + size_t (f) * size_t (hop);
        for (int i = 0; i < n; ++i) sum += double (p[i]) * double (p[i]);
        frameDb[size_t (f)] = float (10.0 * std::log10 (std::max (1.0e-30, sum / n)));
        loudest = std::max (loudest, frameDb[size_t (f)]);
    }
    const float floorDb = std::max (-50.0f, loudest - 30.0f);

    juce::dsp::FFT fft (order);
    std::vector<float> window (static_cast<size_t> (n)), work (static_cast<size_t> (n) * 2), power (static_cast<size_t> (n / 2 + 1), 0.0f);
    for (int i = 0; i < n; ++i) window[size_t (i)] = 0.5f - 0.5f * std::cos (2.0f * dsp::kPi * float (i) / float (n));
    int used = 0;
    for (int f = 0; f < frames; ++f)
    {
        if (frameDb[size_t (f)] < floorDb) continue;
        const float* p = x.data() + size_t (f) * size_t (hop);
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < n; ++i) work[size_t (i)] = p[i] * window[size_t (i)];
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        for (int k = 0; k <= n / 2; ++k) power[size_t (k)] += work[size_t (k)] * work[size_t (k)];
        ++used;
    }
    if (used == 0) return out;

    const double binHz = rate / n;
    for (int b = 0; b < kMicEqBands; ++b)
    {
        const double fc = kMicEqBandHz[b];
        if (! usable (kMicEqBandHz[b], rate)) continue;
        const int lo = int (std::ceil (fc * std::pow (2.0, -0.25) / binHz)), hi = int (std::floor (fc * std::pow (2.0, 0.25) / binHz));
        double sum = 0.0;
        int bins = 0;
        for (int k = std::max (1, lo); k <= std::min (n / 2, hi); ++k, ++bins) sum += power[size_t (k)];
        if (bins > 0) out[size_t (b)] = float (10.0 * std::log10 (std::max (1.0e-30, sum / bins / used)));
    }
    return out;
}

std::vector<float> micEqGainsFromLevels (const MicEqBands& levels)
{
    const auto target = micEqTargetDb();
    std::vector<float> measured (size_t (kMicEqBands), kNaN);
    for (size_t b = 0; b < measured.size(); ++b)
        if (std::isfinite (target[b]) && std::isfinite (levels[b])) measured[b] = target[b] - levels[b];
    // bands with no target or no measurement continue the nearest band that has both (no extrapolated guess)
    auto d = measured;
    for (int b = 0; b < kMicEqBands; ++b)
    {
        if (std::isfinite (d[size_t (b)])) continue;
        d[size_t (b)] = 0.0f; // nothing measured at all
        for (int off = 1; off < kMicEqBands; ++off)
        {
            if (b - off >= 0 && std::isfinite (measured[size_t (b - off)])) { d[size_t (b)] = measured[size_t (b - off)]; break; }
            if (b + off < kMicEqBands && std::isfinite (measured[size_t (b + off)])) { d[size_t (b)] = measured[size_t (b + off)]; break; }
        }
    }

    auto takeMean = [&d]
    {
        float m = 0.0f;
        for (float v : d) m += v;
        m /= float (d.size());
        for (auto& v : d) v -= m;
    };
    takeMean();
    std::vector<float> s (d.size());
    for (int b = 0; b < kMicEqBands; ++b)
    {
        const float l = d[size_t (std::max (0, b - 1))], r = d[size_t (std::min (kMicEqBands - 1, b + 1))];
        s[size_t (b)] = 0.25f * l + 0.5f * d[size_t (b)] + 0.25f * r;
    }
    d = s;
    for (int k = 0; k < 8; ++k) // mean 0 and within the limit (clamping moves the mean, so a few rounds)
    {
        takeMean();
        for (auto& v : d) v = std::clamp (v, -kMicEqMaxDb, kMicEqMaxDb);
    }
    return d;
}

// ============================================================================ the filter design
MicEqBands micEqFilterGains (const std::vector<float>& gainsDb, double rate)
{
    MicEqBands g {};
    if (int (gainsDb.size()) != kMicEqBands || rate <= 0.0) return g;
    int n = 0;
    while (n < kMicEqBands && usable (kMicEqBandHz[n], rate)) ++n;
    if (n == 0) return g;

    // how much 1 dB on band j moves the response at centre i
    std::array<std::array<double, kMicEqBands>, kMicEqBands> m {};
    for (int j = 0; j < n; ++j)
    {
        dsp::Biquad q;
        q.setPeak (rate, kMicEqBandHz[j], kQ, 1.0f);
        for (int i = 0; i < n; ++i)
            m[size_t (i)][size_t (j)] = biquadDbAt (q, 2.0 * juce::MathConstants<double>::pi * kMicEqBandHz[i] / rate);
    }
    std::array<dsp::Biquad, kMicEqBands> bands;
    for (int round = 0; round < 8; ++round) // a peak's dB is only nearly linear in its gain: refine a few times
    {
        designBands (bands, g, rate);
        std::array<double, kMicEqBands> err {};
        for (int i = 0; i < n; ++i) err[size_t (i)] = double (gainsDb[size_t (i)]) - cascadeDbAt (bands, n, rate, kMicEqBandHz[i]);
        solve (m, err, n);
        for (int j = 0; j < n; ++j) g[size_t (j)] = std::clamp (g[size_t (j)] + float (err[size_t (j)]), -kMaxFilterDb, kMaxFilterDb);
    }
    return g;
}

float micEqResponseDb (const std::vector<float>& gainsDb, double rate, double hz)
{
    std::array<dsp::Biquad, kMicEqBands> bands;
    const int n = designBands (bands, micEqFilterGains (gainsDb, rate), rate);
    return cascadeDbAt (bands, n, rate, hz);
}

// ============================================================================ MicEqFilter
MicEqFilter::~MicEqFilter()
{
    delete current;
    delete previous;
    delete toRetire;
    delete pending.exchange (nullptr);
    for (auto& r : retired) delete r.exchange (nullptr);
}

void MicEqFilter::setGains (const std::vector<float>& gainsDb, double rate)
{
    auto* p = new Program();
    p->count = designBands (p->bands, micEqFilterGains (gainsDb, rate), rate);
    p->fadeSamples = std::max (1, int (std::lround (rate * 0.03)));
    delete pending.exchange (p, std::memory_order_acq_rel); // one the audio thread never took
}

void MicEqFilter::collectGarbage()
{
    for (auto& r : retired) delete r.exchange (nullptr, std::memory_order_acq_rel);
}

void MicEqFilter::process (float* x, int n)
{
    // hand-overs: only this thread fills a retire slot, so a free one stays free until it is used
    auto retire = [this] (Program*& p)
    {
        if (p == nullptr) return true;
        for (auto& r : retired)
            if (r.load (std::memory_order_acquire) == nullptr)
            {
                r.store (p, std::memory_order_release);
                p = nullptr;
                return true;
            }
        return false;
    };
    retire (toRetire);
    if (crossLeft == 0) retire (previous);
    if (crossLeft == 0 && previous == nullptr && toRetire == nullptr)
        if (auto* p = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (current != nullptr && running)
            {
                for (size_t b = 0; b < p->bands.size(); ++b) // start from the old state: no restart transient
                {
                    p->bands[b].z1 = current->bands[b].z1;
                    p->bands[b].z2 = current->bands[b].z2;
                }
                previous = current;
                crossLeft = p->fadeSamples;
            }
            else
                toRetire = current;
            current = p;
        }

    const bool want = enabled.load (std::memory_order_acquire);
    if (current == nullptr || (! running && ! want)) return; // off: untouched
    if (! running)
    {
        current->reset();
        running = true;
        mix = 0.0f;
        idle.store (false, std::memory_order_release);
    }

    const float step = 1.0f / float (current->fadeSamples);
    for (int i = 0; i < n; ++i)
    {
        const float in = x[i];
        float y = current->run (in);
        if (crossLeft > 0)
        {
            const float yo = previous->run (in);
            const float t = 1.0f - float (crossLeft) / float (current->fadeSamples);
            y = yo + t * (y - yo);
            --crossLeft;
        }
        if (! std::isfinite (y)) // a non-finite input would stay in the state forever
        {
            current->reset();
            if (previous != nullptr) previous->reset();
            y = std::isfinite (in) ? in : 0.0f;
        }
        mix = want ? std::min (1.0f, mix + step) : std::max (0.0f, mix - step);
        x[i] = in + mix * (y - in);
    }
    if (! want && mix <= 0.0f)
    {
        running = false;
        idle.store (true, std::memory_order_release);
    }
}

// ============================================================================ MicEqJob
MicEqJob::MicEqJob (std::vector<float> t, double sampleRate) : juce::Thread ("KoeLoom mic eq"), take (std::move (t)), rate (sampleRate) {}

MicEqJob::~MicEqJob() { stopThread (-1); }

void MicEqJob::start() { startThread (juce::Thread::Priority::low); } // the owner may be playing a game on the same PC

void MicEqJob::run()
{
    const auto levels = micEqBandLevelsDb (take, rate);
    if (threadShouldExit())
    {
        cancelled.store (true);
        finished.store (true, std::memory_order_release);
        return;
    }
    int measured = 0;
    for (float v : levels) measured += std::isfinite (v) ? 1 : 0;
    if (measured < kMicEqBands / 2)
        error = juce::String::fromUTF8 ("声の区間が見つからず、測れませんでした。10 秒のあいだに 3 秒以上話してください。");
    else
        gains = micEqGainsFromLevels (levels);
    finished.store (true, std::memory_order_release);
}
} // namespace koe

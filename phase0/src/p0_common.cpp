#include "p0_common.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <intrin.h>

namespace p0
{
double nowUs()
{
    static const double toUs = []
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency (&f);
        return 1.0e6 / double (f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter (&c);
    return double (c.QuadPart) * toUs;
}

Stats stats (std::vector<double> v)
{
    Stats s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort (v.begin(), v.end());
    auto rank = [&] (double p) { return v[std::min (v.size() - 1, size_t (std::ceil (p * double (v.size()))) - (p > 0 ? 1 : 0))]; };
    s.p50 = rank (0.50);
    s.p99 = rank (0.99);
    s.max = v.back();
    s.mean = std::accumulate (v.begin(), v.end(), 0.0) / double (v.size());
    return s;
}

//==============================================================================
Args::Args (int argc, char** argv)
{
    SetConsoleOutputCP (CP_UTF8);   // printf writes UTF-8

    for (int i = 1; i < argc; ++i)
        tokens.add (juce::String::fromUTF8 (argv[i]));
    commandLine = tokens.joinIntoString (" ");
}

bool Args::has (const char* name) const { return tokens.contains (name); }

juce::String Args::get (const char* name, const juce::String& def) const
{
    const int i = tokens.indexOf (name);
    return (i >= 0 && i + 1 < tokens.size()) ? tokens[i + 1] : def;
}

double Args::getDouble (const char* name, double def) const
{
    auto s = get (name);
    return s.isEmpty() ? def : s.getDoubleValue();
}

int Args::getInt (const char* name, int def) const
{
    auto s = get (name);
    return s.isEmpty() ? def : s.getIntValue();
}

//==============================================================================
juce::File phase0Dir() { return juce::File (P0_DIR); }
juce::File resultsDir() { return phase0Dir().getChildFile ("results"); }
juce::File rawDir()
{
    auto d = resultsDir().getChildFile ("raw");
    d.createDirectory();
    return d;
}

std::vector<float> readWavMono (const juce::File& f, double* sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return {};
    juce::AudioBuffer<float> b (int (r->numChannels), int (r->lengthInSamples));
    r->read (&b, 0, int (r->lengthInSamples), 0, true, true);
    std::vector<float> v (size_t (b.getNumSamples()), 0.0f);
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            v[size_t (i)] += b.getSample (c, i) / float (b.getNumChannels());
    if (sampleRate != nullptr) *sampleRate = r->sampleRate;
    return v;
}

bool writeWav (const juce::File& f, const std::vector<float>& x, double sr)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
    if (w == nullptr) return false;
    os.release();
    const float* ch[] = { x.data() };
    return w->writeFromFloatArrays (ch, 1, int (x.size()));
}

bool writeText (const juce::File& f, const juce::String& s)
{
    f.getParentDirectory().createDirectory();
    return f.replaceWithText (s.replace ("\r\n", "\n"), false, false, "\n");
}

//==============================================================================
namespace
{
struct Rng
{
    uint32_t s = 0x12345678u;
    void seed (uint32_t v) { s = v ? v : 0x12345678u; }
    uint32_t nextU32() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float nextBipolar() { return float (nextU32() >> 8) * (2.0f / 16777216.0f) - 1.0f; }
};

struct Bandpass // RBJ band-pass, 0 dB peak
{
    float b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (double sr, float f, float q)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * std::clamp (double (f), 1.0, sr * 0.49) / sr;
        const double alpha = std::sin (w0) / (2.0 * q), a0 = 1 + alpha;
        b0 = float (alpha / a0); b2 = float (-alpha / a0);
        a1 = float (-2 * std::cos (w0) / a0); a2 = float ((1 - alpha) / a0);
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
} // namespace

double synthVoiceF0 (double t)
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;
    return 140.0 + 35.0 * std::sin (twoPi * 0.23 * t) + 2.0 * std::sin (twoPi * 5.0 * t);
}

std::vector<float> synthVoice (double seconds, uint32_t seed, double sr)
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;
    const size_t n = size_t (seconds * sr);
    std::vector<float> v (n);
    Rng rng;
    rng.seed (seed);
    const float F[5][3] = { { 800, 1200, 2500 }, { 300, 2300, 3000 }, { 350, 1300, 2400 }, { 500, 1900, 2600 }, { 500, 900, 2400 } };
    Bandpass f1, f2, f3, cons;
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
            f1.set (sr, cur[0], 6.0f);
            f2.set (sr, cur[1], 8.0f);
            f3.set (sr, cur[2], 10.0f);
            cons.set (sr, 5000.0f, 1.5f);
        }
        const double f0 = synthVoiceF0 (t);
        phase += f0 / sr;
        if (phase >= 1.0) phase -= 1.0;
        float src = 0.0f;
        const int harmonics = std::min (40, int (4000.0 / f0));
        for (int h = 1; h <= harmonics; ++h)
            src += float (std::sin (twoPi * h * phase) / std::pow (double (h), 1.2));
        const float voiced = f1.process (src) * 1.0f + f2.process (src) * 0.6f + f3.process (src) * 0.3f;
        const float env = 0.05f + 0.95f * float (0.5 + 0.5 * std::sin (twoPi * 3.7 * t));
        const float consonant = (std::fmod (t, 0.6) < 0.04) ? cons.process (rng.nextBipolar()) * 0.5f : cons.process (0.0f);
        v[i] = voiced * env + consonant;
    }
    float peak = 0.0f;
    for (float s : v) peak = std::max (peak, std::abs (s));
    if (peak > 0.0f)
        for (auto& s : v) s *= 0.5f / peak;
    return v;
}

Speech loadSpeech (const Args& args)
{
    Speech s;
    juce::File f = args.has ("--input") ? juce::File (args.get ("--input"))
                                        : phase0Dir().getChildFile ("testdata/reference-speech.wav");
    if (f.existsAsFile())
    {
        double sr = 0;
        auto x = readWavMono (f, &sr);
        if (! x.empty() && std::abs (sr - kSr) < 1.0)
        {
            s.x = std::move (x);
            s.real = true;
            s.source = (args.has ("--input") ? "speech file (--input): " : "reference speech: ") + f.getFullPathName();
            return s;
        }
        std::fprintf (stderr, "warning: %s is not a 48 kHz WAV, using synthetic speech\n", f.getFullPathName().toRawUTF8());
    }
    s.x = synthVoice (10.0);
    s.source = juce::String::fromUTF8 ("synthetic speech (synthVoice 10 s, seed 7) - 合成音声で代用");
    return s;
}

std::vector<float> sine (double hz, double seconds, float amplitude, double sr)
{
    std::vector<float> v (size_t (seconds * sr));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = amplitude * float (std::sin (2.0 * juce::MathConstants<double>::pi * hz * double (i) / sr));
    return v;
}

std::vector<float> whiteNoiseRms (double seconds, double rmsDbfs, uint32_t seed, double sr)
{
    Rng rng;
    rng.seed (seed);
    const float a = float (std::pow (10.0, rmsDbfs / 20.0) * std::sqrt (3.0)); // uniform: rms = a / sqrt(3)
    std::vector<float> v (size_t (seconds * sr));
    for (auto& x : v) x = a * rng.nextBipolar();
    return v;
}

//==============================================================================
double rmsDb (const float* x, size_t n)
{
    if (n == 0) return -200.0;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += double (x[i]) * x[i];
    const double r = std::sqrt (s / double (n));
    return r <= 1e-10 ? -200.0 : 20.0 * std::log10 (r);
}

static std::vector<double> envelope1ms (const std::vector<float>& x)
{
    const size_t hop = size_t (kSr / 1000.0);
    std::vector<double> e (x.size() / hop);
    for (size_t k = 0; k < e.size(); ++k)
    {
        double s = 0.0;
        for (size_t i = 0; i < hop; ++i) s += double (x[k * hop + i]) * x[k * hop + i];
        e[k] = std::sqrt (s / double (hop));
    }
    return e;
}

int envelopeLagMs (const std::vector<float>& in, const std::vector<float>& out, int maxLagMs, double* score)
{
    auto a = envelope1ms (in), b = envelope1ms (out);
    const int n = int (std::min (a.size(), b.size())) - maxLagMs;
    if (n <= 10) return 0;
    auto meanOf = [] (const std::vector<double>& v, int from, int count)
    { return std::accumulate (v.begin() + from, v.begin() + from + count, 0.0) / count; };
    const double ma = meanOf (a, 0, n);
    int best = 0;
    double bestScore = -2.0;
    for (int lag = 0; lag <= maxLagMs; ++lag)
    {
        const double mb = meanOf (b, lag, n);
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < n; ++i)
        {
            const double da = a[size_t (i)] - ma, db = b[size_t (i + lag)] - mb;
            sab += da * db; saa += da * da; sbb += db * db;
        }
        const double sc = sab / std::sqrt (std::max (1e-30, saa * sbb));
        if (sc > bestScore) { bestScore = sc; best = lag; }
    }
    if (score != nullptr) *score = bestScore;
    return best;
}

int waveformLag (const std::vector<float>& in, const std::vector<float>& out, int centre, int radius, double* score)
{
    const int lo = std::max (0, centre - radius), hi = centre + radius;
    const int start = int (kSr);                      // skip the first second
    const int n = std::min (int (2 * kSr), int (std::min (in.size(), out.size())) - start - hi);
    if (n <= 0) return centre;
    int best = lo;
    double bestScore = -2.0;
    for (int lag = lo; lag <= hi; ++lag)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < n; ++i)
        {
            const double a = in[size_t (start + i)], b = out[size_t (start + i + lag)];
            sab += a * b; saa += a * a; sbb += b * b;
        }
        const double sc = sab / std::sqrt (std::max (1e-30, saa * sbb));
        if (sc > bestScore) { bestScore = sc; best = lag; }
    }
    if (score != nullptr) *score = bestScore;
    return best;
}

//==============================================================================
static uint64_t ft (const FILETIME& f) { return (uint64_t (f.dwHighDateTime) << 32) | f.dwLowDateTime; }

void SystemLoad::start()
{
    FILETIME i, k, u;
    GetSystemTimes (&i, &k, &u);
    idle0 = ft (i); kernel0 = ft (k); user0 = ft (u);
}

double SystemLoad::busyPercent() const
{
    FILETIME i, k, u;
    GetSystemTimes (&i, &k, &u);
    const double idle = double (ft (i) - idle0), total = double (ft (k) - kernel0 + ft (u) - user0); // kernel includes idle
    return total > 0 ? 100.0 * (total - idle) / total : 0.0;
}

double processCpuSeconds()
{
    FILETIME c, e, k, u;
    GetProcessTimes (GetCurrentProcess(), &c, &e, &k, &u);
    return double (ft (k) + ft (u)) * 1.0e-7;
}

double processCycleSeconds()
{
    static const double tscHz = []
    {
        const double t0 = nowUs();
        const auto c0 = __rdtsc();
        while (nowUs() - t0 < 200000.0) {}
        return double (__rdtsc() - c0) / ((nowUs() - t0) * 1.0e-6);
    }();
    ULONG64 cycles = 0;
    QueryProcessCycleTime (GetCurrentProcess(), &cycles);
    return double (cycles) / tscHz;
}

ScopedProAudio::ScopedProAudio()
{
    DWORD idx = 0;
    handle = AvSetMmThreadCharacteristicsW (L"Pro Audio", &idx);
    error = handle ? 0 : GetLastError();
}

ScopedProAudio::~ScopedProAudio()
{
    if (handle != nullptr) AvRevertMmThreadCharacteristics (handle);
}

juce::String num (double v, int decimals) { return juce::String (v, decimals); }

juce::String timestamp() { return juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S"); }
} // namespace p0

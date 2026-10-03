// effects-b: noise, voicechar, ringmod, modulation, phaser, rotary, ensemble, tremolo, slicer, stutter,
// echo, reverb. AC-14 for each, AC-43 for slicer / stutter, and one "does it do its job" check per type.
// Signals are synthetic (synthVoice / sines / impulses), so measured values are "合成音声で代用".

#include "Dsp/Building.h"
#include "Effects/EffectRegistry.h"
#include "Tests/EffectTestHarness.h"
#include "Tests/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace koe
{
namespace
{
using test::kSr;
constexpr int kBlock = 480;
constexpr double kPiD = 3.14159265358979323846;

const char* const kTypes[] = { "noise", "voicechar", "ringmod", "modulation", "phaser", "rotary",
                               "ensemble", "tremolo", "slicer", "stutter", "echo", "reverb" };

std::vector<float> paramsOf (const char* type, std::initializer_list<std::pair<const char*, double>> overrides = {})
{
    auto* info = findEffectInfo (type);
    std::vector<float> p;
    for (auto& s : info->params) p.push_back (s.def);
    for (auto& [id, v] : overrides)
    {
        const int i = info->paramIndex (id);
        jassert (i >= 0);
        p[size_t (i)] = info->params[size_t (i)].clamp (float (v));
    }
    return p;
}

std::unique_ptr<IEffect> makeFx (const char* type, const std::vector<float>& params)
{
    auto fx = createEffect (type);
    fx->prepare (kSr, kBlock);
    for (size_t i = 0; i < params.size(); ++i) fx->setParam (int (i), params[i]);
    fx->reset();
    return fx;
}

void run (IEffect& fx, std::vector<float>& buf)
{
    for (size_t pos = 0; pos < buf.size(); pos += kBlock)
        fx.process (buf.data() + pos, int (std::min<size_t> (kBlock, buf.size() - pos)));
}

/** Amplitude (dBFS of a sine) of the component at hz in x[start, start+len). */
double toneDb (const std::vector<float>& x, int start, int len, double hz)
{
    const double coeff = 2.0 * std::cos (2.0 * kPiD * hz / kSr);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i)
    {
        const double s = x[size_t (start + i)] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = std::max (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2);
    return 20.0 * std::log10 (std::max (1.0e-12, 2.0 * std::sqrt (power) / len));
}

float rmsDbOf (const std::vector<float>& x, int start, int len) { return test::rmsDb (x.data() + start, len); }

/** RMS envelope in windows of `win` samples (linear). */
std::vector<float> envelope (const std::vector<float>& x, int start, int end, int win)
{
    std::vector<float> e;
    for (int p = start; p + win <= end; p += win)
    {
        double s = 0.0;
        for (int i = 0; i < win; ++i) s += double (x[size_t (p + i)]) * x[size_t (p + i)];
        e.push_back (float (std::sqrt (s / win)));
    }
    return e;
}

/** Lag in [minLag, maxLag] with the highest autocorrelation of the mean-removed sequence. */
int periodOf (std::vector<float> e, int minLag, int maxLag)
{
    double mean = 0.0;
    for (float v : e) mean += v;
    mean /= double (e.size());
    for (auto& v : e) v -= float (mean);
    int best = minLag;
    double bestScore = -1e30;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double s = 0.0;
        for (size_t i = 0; i + size_t (lag) < e.size(); ++i) s += double (e[i]) * e[i + size_t (lag)];
        s /= double (e.size() - size_t (lag));
        if (s > bestScore) { bestScore = s; best = lag; }
    }
    return best;
}

/** RT60 estimate from the Schroeder backward integral (T20: -5 dB .. -25 dB, x3). */
double rt60Of (const std::vector<float>& ir)
{
    std::vector<double> back (ir.size() + 1, 0.0);
    for (size_t i = ir.size(); i-- > 0;) back[i] = back[i + 1] + double (ir[i]) * ir[i];
    const double total = back[0];
    if (total <= 0.0) return 0.0;
    auto timeAt = [&] (double db)
    {
        const double thr = total * std::pow (10.0, db / 10.0);
        for (size_t i = 0; i < ir.size(); ++i)
            if (back[i] <= thr) return double (i) / kSr;
        return double (ir.size()) / kSr;
    };
    return 3.0 * (timeAt (-25.0) - timeAt (-5.0));
}

class EffectsBTests : public juce::UnitTest
{
public:
    EffectsBTests() : juce::UnitTest ("Effects B", "EffectsB") {}

    void runTest() override
    {
        beginTest ("AC-14: every effects-b type (finite, limiter, 50 ms sweeps)");
        for (auto* type : kTypes)
            expect (test::runAc14Checks (*this, type), juce::String ("AC-14 failed for ") + type);

        beginTest ("no allocation in process / setParam / reset");
        for (auto* type : kTypes)
        {
            auto fx = makeFx (type, paramsOf (type));
            auto buf = test::synthVoice (0.5);
            auto* info = findEffectInfo (type);
            test::AllocationCounter allocs;
            run (*fx, buf);
            for (size_t p = 0; p < info->params.size(); ++p) fx->setParam (int (p), info->params[p].max);
            run (*fx, buf);
            fx->reset();
            run (*fx, buf);
            const auto count = allocs.count();
            expectEquals (int (count), 0, juce::String (type) + " allocated on the audio thread");
        }

        testNoise();
        testVoiceChar();
        testRingMod();
        testModulation();
        testPhaser();
        testRotary();
        testEnsemble();
        testTremolo();
        testSlicer();
        testStutter();
        testEcho();
        testReverb();
        measureCpu();
    }

private:
    void testNoise()
    {
        beginTest ("noise: levelDb is the noise RMS; followVoice 1 is silent on silence");
        const int skip = int (kSr * 0.5);
        auto pink = test::renderEffect ("noise", test::silence (4.0), paramsOf ("noise", { { "kind", 1 }, { "levelDb", -20 }, { "toneHz", 16000 } }));
        const float pinkDb = rmsDbOf (pink, skip, int (pink.size()) - skip);
        logMessage ("  noise pink -20 dB, tone 16 kHz: " + juce::String (pinkDb, 2) + " dB");
        expectWithinAbsoluteError (pinkDb, -20.0f, 1.0f);
        for (int kind : { 0, 2 })
        {
            auto out = test::renderEffect ("noise", test::silence (4.0), paramsOf ("noise", { { "kind", float (kind) }, { "levelDb", -30 }, { "toneHz", 16000 } }));
            const float db = rmsDbOf (out, skip, int (out.size()) - skip);
            logMessage ("  noise kind " + juce::String (kind) + " -30 dB: " + juce::String (db, 2) + " dB");
            expectWithinAbsoluteError (db, -30.0f, 3.0f);
        }
        auto followed = test::renderEffect ("noise", test::silence (2.0), paramsOf ("noise", { { "levelDb", -6 }, { "followVoice", 1 } }));
        expectLessThan (test::rmsDb (followed), -100.0f);
        auto voice = test::synthVoice (3.0);
        auto withVoice = test::renderEffect ("noise", voice, paramsOf ("noise", { { "kind", 0 }, { "levelDb", -30 }, { "followVoice", 1 }, { "toneHz", 16000 } }));
        for (size_t i = 0; i < voice.size(); ++i) withVoice[i] -= voice[i];
        const float added = rmsDbOf (withVoice, skip, int (voice.size()) - skip);
        logMessage ("  noise following the synthetic voice: " + juce::String (added, 2) + " dB");
        expect (added > -45.0f && added < -20.0f, "followVoice noise level out of range: " + juce::String (added));
    }

    void testVoiceChar()
    {
        beginTest ("voicechar: intensity 0 is the original; band limits; level kept");
        auto voice = test::synthVoice (3.0);
        auto untouched = test::renderEffect ("voicechar", voice, paramsOf ("voicechar", { { "intensity", 0 } }));
        float maxDiff = 0.0f;
        for (size_t i = 0; i < voice.size(); ++i) maxDiff = std::max (maxDiff, std::abs (untouched[i] - voice[i]));
        expectLessOrEqual (maxDiff, 1.0e-7f);

        const auto lowMidHigh = [] {
            auto a = test::sine (100.0, 2.0, 0.2f), b = test::sine (1000.0, 2.0, 0.2f), c = test::sine (7000.0, 2.0, 0.2f);
            for (size_t i = 0; i < a.size(); ++i) a[i] += b[i] + c[i];
            return a;
        }();
        const char* names[] = { "telephone", "radio", "megaphone", "walkie" };
        const int start = int (kSr), len = int (kSr * 0.5);
        for (int k = 0; k < 4; ++k)
        {
            auto out = test::renderEffect ("voicechar", lowMidHigh, paramsOf ("voicechar", { { "kind", float (k) }, { "intensity", 1 }, { "mix", 1 } }));
            const double lo = toneDb (out, start, len, 100.0), mid = toneDb (out, start, len, 1000.0), hi = toneDb (out, start, len, 7000.0);
            expectGreaterThan (mid - lo, 20.0, juce::String (names[k]) + ": 100 Hz not cut");
            expectGreaterThan (mid - hi, 20.0, juce::String (names[k]) + ": 7 kHz not cut");

            auto v = test::renderEffect ("voicechar", voice, paramsOf ("voicechar", { { "kind", float (k) }, { "intensity", 1 }, { "mix", 1 } }));
            const float diff = rmsDbOf (v, int (kSr * 0.5), int (kSr * 2.5)) - rmsDbOf (voice, int (kSr * 0.5), int (kSr * 2.5));
            logMessage ("  voicechar " + juce::String (names[k]) + ": level change " + juce::String (diff, 2) + " dB, 100 Hz "
                        + juce::String (lo - mid, 1) + " dB, 7 kHz " + juce::String (hi - mid, 1) + " dB re 1 kHz");
            expectWithinAbsoluteError (diff, 0.0f, 2.0f, juce::String (names[k]) + " changes the level");
        }
    }

    void testRingMod()
    {
        beginTest ("ringmod: freqHz side bands, carrier gone");
        auto out = test::renderEffect ("ringmod", test::sine (1000.0, 1.0, 0.5f), paramsOf ("ringmod", { { "freqHz", 200 }, { "mix", 1 } }));
        const int start = 4800, len = 24000;
        const double lower = toneDb (out, start, len, 800.0), upper = toneDb (out, start, len, 1200.0), centre = toneDb (out, start, len, 1000.0);
        expectWithinAbsoluteError (lower, -12.04, 0.5);
        expectWithinAbsoluteError (upper, -12.04, 0.5);
        expectLessThan (centre, lower - 40.0);
    }

    void testModulation()
    {
        beginTest ("modulation: vibrato bends pitch and ignores mix; chorus mix 0 is dry; flanger stays bounded");
        auto in = test::sine (440.0, 3.0, 0.5f);
        auto vib = test::renderEffect ("modulation", in, paramsOf ("modulation", { { "mode", 2 }, { "rateHz", 2 }, { "depth", 1 }, { "mix", 0 } }));
        double lo = 1e9, hi = 0.0;
        for (int p = int (kSr); p + 1920 < int (kSr * 2.0); p += 480)
        {
            const double f = test::estimateF0 (vib.data() + p, 1920);
            lo = std::min (lo, f);
            hi = std::max (hi, f);
        }
        logMessage ("  vibrato depth 1 at 2 Hz: " + juce::String (test::centsBetween (hi, lo), 1) + " cents peak to peak");
        expectGreaterThan (test::centsBetween (hi, lo), 40.0);

        auto dry = test::renderEffect ("modulation", in, paramsOf ("modulation", { { "mix", 0 } }));
        float maxDiff = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) maxDiff = std::max (maxDiff, std::abs (dry[i] - in[i]));
        expectLessOrEqual (maxDiff, 1.0e-7f);

        auto noise = test::whiteNoise (3.0, 0.1f, 5);
        auto fl = test::renderEffect ("modulation", noise, paramsOf ("modulation", { { "mode", 1 }, { "feedback", 0.9f }, { "mix", 1 } }));
        expect (test::allFinite (fl));
        expectLessThan (test::rmsDb (fl) - test::rmsDb (noise), 6.0f);
    }

    void testPhaser()
    {
        beginTest ("phaser: static notch at depth 0, mix 0.5");
        float lo = 0.0f, hi = -200.0f;
        for (int k = 0; k <= 40; ++k)
        {
            const double hz = 100.0 * std::pow (80.0, k / 40.0);
            auto out = test::renderEffect ("phaser", test::sine (hz, 0.5, 0.5f), paramsOf ("phaser", { { "depth", 0 }, { "feedback", 0 }, { "mix", 0.5f } }));
            const float g = rmsDbOf (out, 12000, 12000) - test::rmsDb (test::sine (hz, 0.25, 0.5f));
            lo = std::min (lo, g);
            hi = std::max (hi, g);
        }
        logMessage ("  phaser response: min " + juce::String (lo, 1) + " dB, max " + juce::String (hi, 1) + " dB");
        expectLessThan (lo, -20.0f);
        expectWithinAbsoluteError (hi, 0.0f, 0.5f);
    }

    void testRotary()
    {
        beginTest ("rotary: level wobbles at the rotor rate");
        // 5 kHz: well inside the horn band, so the drum's own (slower) rotation does not beat with it
        auto out = test::renderEffect ("rotary", test::sine (5000.0, 4.0, 0.5f), paramsOf ("rotary", { { "rateHz", 6 }, { "depth", 1 }, { "mix", 1 } }));
        auto env = envelope (out, int (kSr), int (kSr * 4.0), 48); // 1 ms windows
        const float mx = *std::max_element (env.begin(), env.end()), mn = *std::min_element (env.begin(), env.end());
        const int period = periodOf (env, 100, 400);
        logMessage ("  rotary 6 Hz: AM " + juce::String (dsp::gainToDb (mx / mn), 1) + " dB, period " + juce::String (period) + " ms");
        expectGreaterThan (dsp::gainToDb (mx / mn), 2.0f);
        expectWithinAbsoluteError (period, 167, 8);
    }

    void testEnsemble()
    {
        beginTest ("ensemble: many decorrelated voices at the same level");
        auto voice = test::synthVoice (3.0);
        auto out = test::renderEffect ("ensemble", voice, paramsOf ("ensemble", { { "voices", 8 }, { "depth", 1 }, { "mix", 1 } }));
        const float diff = rmsDbOf (out, int (kSr), int (kSr)) - rmsDbOf (voice, int (kSr), int (kSr));
        // best match of the wet against the dry at any lag up to 40 ms
        std::vector<float> ref (voice.begin() + 48000, voice.begin() + 96000), sig (out.begin() + 48000, out.begin() + 96000 + 1920);
        const int lag = test::findLag (ref, sig, 1920);
        double s = 0, e1 = 0, e2 = 0;
        for (size_t i = 0; i < ref.size(); ++i) { s += double (ref[i]) * sig[i + size_t (lag)]; e1 += double (ref[i]) * ref[i]; e2 += double (sig[i + size_t (lag)]) * sig[i + size_t (lag)]; }
        const double corr = s / std::sqrt (e1 * e2);
        logMessage ("  ensemble 8 voices: level " + juce::String (diff, 2) + " dB, best correlation " + juce::String (corr, 3));
        expectWithinAbsoluteError (diff, 0.0f, 3.0f);
        expectLessThan (corr, 0.85);
    }

    void testTremolo()
    {
        beginTest ("tremolo: level swings at rateHz by depth");
        auto out = test::renderEffect ("tremolo", test::sine (1000.0, 3.0, 0.5f), paramsOf ("tremolo", { { "rateHz", 5 }, { "depth", 1 } }));
        auto env = envelope (out, 0, int (out.size()), 48);
        const float mx = *std::max_element (env.begin(), env.end()), mn = *std::min_element (env.begin(), env.end());
        expectGreaterThan (dsp::gainToDb (mx / std::max (mn, 1.0e-6f)), 30.0f);
        expectWithinAbsoluteError (periodOf (env, 100, 400), 200, 3);
        auto half = test::renderEffect ("tremolo", test::sine (1000.0, 3.0, 0.5f), paramsOf ("tremolo", { { "rateHz", 5 }, { "depth", 0.5f } }));
        auto env2 = envelope (half, 0, int (half.size()), 48);
        const float mx2 = *std::max_element (env2.begin(), env2.end()), mn2 = *std::min_element (env2.begin(), env2.end());
        expectWithinAbsoluteError (mn2 / mx2, 0.5f, 0.03f);
    }

    /** Crossing positions (fractional samples) of `level`, both directions. */
    static std::vector<double> crossings (const std::vector<float>& x, float level)
    {
        std::vector<double> c;
        for (size_t i = 1; i < x.size(); ++i)
            if ((x[i - 1] - level) * (x[i] - level) < 0.0f || (x[i] == level && x[i - 1] != level))
                c.push_back (double (i - 1) + double (level - x[i - 1]) / double (x[i] - x[i - 1]));
        return c;
    }

    void testSlicer()
    {
        beginTest ("AC-43 slicer: cuts every 125 ms (+-1 sample) at 120 BPM 1/16, pattern 0, smoothMs 0");
        const std::vector<float> dc (size_t (kSr * 3.0), 0.5f);
        for (float smooth : { 0.0f, 5.0f })
        {
            auto out = test::renderEffect ("slicer", dc, paramsOf ("slicer", { { "bpm", 120 }, { "pattern", 0 }, { "division", 1 }, { "depth", 1 }, { "smoothMs", smooth } }));
            auto c = crossings (out, 0.25f); // 50 % of the 0.5 amplitude
            expectGreaterOrEqual (int (c.size()), 22);
            double worst = 0.0;
            for (size_t i = 1; i < c.size(); ++i) worst = std::max (worst, std::abs (c[i] - c[i - 1] - 6000.0));
            logMessage ("  slicer smoothMs " + juce::String (smooth, 0) + ": " + juce::String (int (c.size())) + " cuts, worst interval error "
                        + juce::String (worst, 3) + " samples");
            expectLessOrEqual (worst, 1.0);
            if (smooth == 0.0f) expectWithinAbsoluteError (c.front(), 5999.5, 1.0); // the first step starts at reset()
        }
        auto half = test::renderEffect ("slicer", dc, paramsOf ("slicer", { { "depth", 0.5f } }));
        expectWithinAbsoluteError (half[size_t (6000 + 3000)], 0.25f, 1.0e-4f); // cut step = 1 - depth
        expectWithinAbsoluteError (half[size_t (3000)], 0.5f, 1.0e-4f);
    }

    void testStutter()
    {
        beginTest ("AC-43 stutter: same input -> identical output, also from the first sample after reset()");
        auto voice = test::synthVoice (6.0, 11);
        const auto params = paramsOf ("stutter", { { "bpm", 120 }, { "division", 2 }, { "chance", 0.5f } });
        auto a = test::renderEffect ("stutter", voice, params);
        auto b = test::renderEffect ("stutter", voice, params);
        expect (a == b, "two renders differ");

        auto fx = makeFx ("stutter", params);
        auto c1 = voice, c2 = voice;
        run (*fx, c1);
        fx->reset(); // slot OFF -> ON
        run (*fx, c2);
        expect (c1 == a, "first pass differs from a fresh instance");
        expect (c2 == a, "after reset() the output is not identical from the first sample");
        int changed = 0;
        for (int s = 0; s + 6000 <= int (voice.size()); s += 6000)
        {
            bool diff = false; // look past the first piece and the previous section's tail fade
            for (int i = 1700; i < 6000 && ! diff; ++i) diff = a[size_t (s + i)] != voice[size_t (s + i)];
            changed += diff ? 1 : 0;
        }
        logMessage ("  stutter chance 0.5: " + juce::String (changed) + " of 48 sections repeated");
        expect (changed > 12 && changed < 36);

        beginTest ("stutter: chance 1 repeats the first piece exactly, chance 0 passes through");
        auto all = test::renderEffect ("stutter", voice, paramsOf ("stutter", { { "chance", 1 }, { "repeatCount", 4 } }));
        float worst = 0.0f;
        for (int s = 0; s + 6000 <= int (voice.size()); s += 6000)
            for (int k = 1; k < 4; ++k)
                for (int j = 144; j < 1500; ++j) // after the 3 ms crossfade
                    worst = std::max (worst, std::abs (all[size_t (s + k * 1500 + j)] - voice[size_t (s + j)]));
        expectLessOrEqual (worst, 1.0e-6f, "repeated pieces are not copies of the first piece");
        auto none = test::renderEffect ("stutter", voice, paramsOf ("stutter", { { "chance", 0 } }));
        expect (none == voice);
    }

    void testEcho()
    {
        beginTest ("echo: impulse response at timeMs, dry not delayed, feedback repeats");
        std::vector<float> imp (size_t (kSr * 2.0), 0.0f);
        imp[0] = 1.0f;
        auto wet = test::renderEffect ("echo", imp, paramsOf ("echo", { { "timeMs", 300 }, { "feedback", 0.5f }, { "toneHz", 12000 }, { "mix", 1 } }));
        auto peakIn = [&] (const std::vector<float>& v, int a, int b)
        {
            int best = a;
            for (int i = a; i < b; ++i) if (std::abs (v[size_t (i)]) > std::abs (v[size_t (best)])) best = i;
            return best;
        };
        const int first = peakIn (wet, 1, 20000), second = peakIn (wet, 20000, 35000);
        expectWithinAbsoluteError (first, 14400, 2);
        expectWithinAbsoluteError (second, 28800, 3);
        expectWithinAbsoluteError (std::abs (wet[size_t (second)] / wet[size_t (first)]), 0.5f, 0.15f);
        auto half = test::renderEffect ("echo", imp, paramsOf ("echo", { { "mix", 0.5f } }));
        expectWithinAbsoluteError (half[0], std::sin (0.25f * dsp::kPi), 1.0e-5f);

        beginTest ("echo: tape repeats lose highs faster than digital ones");
        auto burst = test::whiteNoise (0.05, 0.5f, 3);
        burst.resize (size_t (kSr * 1.5), 0.0f);
        auto brightness = [&] (int mode)
        {
            auto out = test::renderEffect ("echo", burst, paramsOf ("echo", { { "mode", float (mode) }, { "timeMs", 300 }, { "feedback", 0.8f }, { "toneHz", 12000 }, { "mix", 1 } }));
            const int a = 3 * 14400 - 2000, b = 3 * 14400 + 4400; // third repeat
            double e = 0, d = 0;
            for (int i = a + 1; i < b; ++i) { e += double (out[size_t (i)]) * out[size_t (i)]; d += double (out[size_t (i)] - out[size_t (i - 1)]) * (out[size_t (i)] - out[size_t (i - 1)]); }
            return d / std::max (1e-20, e);
        };
        const double digital = brightness (0), tape = brightness (1);
        logMessage ("  echo 3rd repeat brightness: digital " + juce::String (digital, 4) + ", tape " + juce::String (tape, 4));
        expectLessThan (tape, digital * 0.7);

        beginTest ("echo: reverse plays the last timeMs backwards");
        std::vector<float> rising (size_t (kSr * 2.0), 0.0f);
        for (int i = 0; i < 4800; ++i) rising[size_t (i)] = float (i) / 4800.0f * float (std::sin (2.0 * kPiD * 700.0 * i / kSr));
        // 1 s grains: the 100 ms burst comes back reversed near the top of a grain window (almost flat)
        auto rev = test::renderEffect ("echo", rising, paramsOf ("echo", { { "mode", 2 }, { "timeMs", 1000 }, { "feedback", 0 }, { "mix", 1 } }));
        // the input envelope rises, so its energy centroid sits at 75 % of the burst; reversed it must sit early
        rev.resize (70000); // first grain pair only (a faint second copy follows under a closing window)
        float peak = 0.0f;
        for (float v : rev) peak = std::max (peak, std::abs (v));
        int a = 0, b = int (rev.size()) - 1;
        while (a < b && std::abs (rev[size_t (a)]) < 0.05f * peak) ++a;
        while (b > a && std::abs (rev[size_t (b)]) < 0.05f * peak) --b;
        double num = 0.0, den = 0.0;
        for (int i = a; i <= b; ++i) { num += double (i - a) * rev[size_t (i)] * rev[size_t (i)]; den += double (rev[size_t (i)]) * rev[size_t (i)]; }
        const double centroid = num / den / double (b - a);
        logMessage ("  echo reverse: burst back at " + juce::String (a) + ".." + juce::String (b) + ", energy centroid at "
                    + juce::String (centroid * 100.0, 1) + " % (forward would be 75 %)");
        expectLessThan (centroid, 0.35);
        expect (b - a > 4000 && b - a < 4900, "reversed burst length " + juce::String (b - a));
        expectGreaterThan (a, 24000); // comes after the original
    }

    void testReverb()
    {
        beginTest ("reverb: tail length differs per type (hall > room > ambience), pre-delay");
        std::vector<float> imp (size_t (kSr * 8.0), 0.0f);
        imp[0] = 1.0f;
        const char* names[] = { "room", "hall", "plate", "spring", "ambience" };
        double rt[5];
        for (int t = 0; t < 5; ++t)
        {
            auto ir = test::renderEffect ("reverb", imp, paramsOf ("reverb", { { "type", float (t) }, { "mix", 1 } }));
            expect (test::allFinite (ir));
            rt[t] = rt60Of (ir);
            auto v = test::synthVoice (4.0);
            auto wet = test::renderEffect ("reverb", v, paramsOf ("reverb", { { "type", float (t) }, { "mix", 1 } }));
            logMessage ("  reverb " + juce::String (names[t]) + ": RT60 " + juce::String (rt[t], 2) + " s, wet level "
                        + juce::String (rmsDbOf (wet, int (kSr), int (kSr * 3)) - rmsDbOf (v, int (kSr), int (kSr * 3)), 1) + " dB re dry");
        }
        expectGreaterThan (rt[1], rt[0] * 1.5);
        expectGreaterThan (rt[0], rt[4] * 1.5);
        expectGreaterThan (rt[2], rt[4]);
        expectGreaterThan (rt[3], rt[4]);

        auto pre = test::renderEffect ("reverb", imp, paramsOf ("reverb", { { "type", 4 }, { "preDelayMs", 100 }, { "mix", 1 } }));
        int onset = 0;
        while (onset < int (pre.size()) && std::abs (pre[size_t (onset)]) < 1.0e-4f) ++onset;
        expectGreaterOrEqual (onset, 4800 - 2);
        expectLessThan (onset, 4800 + 200);
    }

    void measureCpu()
    {
        beginTest ("CPU per 480-sample block (logged; loose sanity bound)");
        const auto voice = test::synthVoice (10.0);
        std::vector<std::pair<juce::String, std::vector<float>>> cases;
        for (auto* type : kTypes) cases.push_back ({ type, paramsOf (type) });
        for (int t : { 1, 2, 3, 4 }) cases.push_back ({ "reverb/" + juce::String (t), paramsOf ("reverb", { { "type", t } }) });
        cases.push_back ({ "echo/tape", paramsOf ("echo", { { "mode", 1 } }) });
        cases.push_back ({ "echo/reverse", paramsOf ("echo", { { "mode", 2 } }) });
        cases.push_back ({ "ensemble/8", paramsOf ("ensemble", { { "voices", 8 } }) });
        for (auto& [label, params] : cases)
        {
            const auto type = label.upToFirstOccurrenceOf ("/", false, false);
            auto fx = makeFx (type.toRawUTF8(), params);
            auto buf = voice;
            const auto t0 = juce::Time::getHighResolutionTicks();
            run (*fx, buf);
            const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const double perBlockUs = sec * 1.0e6 / (double (buf.size()) / kBlock);
            const auto r0 = juce::Time::getHighResolutionTicks();
            fx->reset(); // slot OFF -> ON clears the history on the audio thread
            const double resetUs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - r0) * 1.0e6;
            logMessage ("  " + label.paddedRight (' ', 14) + juce::String (perBlockUs, 1) + " us/block = "
                        + juce::String (perBlockUs / 100.0, 2) + " % of 10 ms, reset() " + juce::String (resetUs, 0) + " us");
            expectLessThan (perBlockUs, 1000.0, label + " is far too slow");
        }
    }
};

static EffectsBTests effectsBTests;
} // namespace
} // namespace koe

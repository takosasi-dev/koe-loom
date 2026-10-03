#include "Dsp/Building.h"
#include "Effects/EffectRegistry.h"
#include "Tests/EffectTestHarness.h"
#include "Tests/TestUtil.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

// effects-a: AC-14 for the 12 types plus one "does it do its job" check per type, measured numerically.

namespace koe
{
namespace
{
using namespace test;

const char* const kTypes[] = { "compressor", "deesser", "enhancer", "eq", "peq", "autowah",
                               "filtersweep", "isolator", "formantfilter", "saturator", "distortion", "bitcrusher" };

std::vector<float> paramsWith (const char* type, std::initializer_list<std::pair<const char*, float>> overrides)
{
    std::vector<float> p;
    auto* info = findEffectInfo (type);
    if (info == nullptr) return p;
    for (auto& s : info->params) p.push_back (s.def);
    for (auto& [id, v] : overrides)
    {
        const int i = info->paramIndex (id);
        jassert (i >= 0);
        if (i >= 0) p[size_t (i)] = v;
    }
    return p;
}

/** Peak amplitude (dB) of the component at hz, Hann-windowed single-bin DFT over [start, start + len). */
float toneDb (const std::vector<float>& x, double hz, int start, int len)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (twoPi * i / (len - 1));
        const double v = w * x[size_t (start + i)];
        const double ph = twoPi * hz * (start + i) / kSr;
        re += v * std::cos (ph);
        im += v * std::sin (ph);
        wsum += w;
    }
    return dsp::gainToDb (float (2.0 * std::sqrt (re * re + im * im) / wsum));
}

constexpr int kSettle = 24000, kWindow = 24000; // measure 0.5..1.0 s

float toneDb (const std::vector<float>& x, double hz) { return toneDb (x, hz, kSettle, kWindow); }

/** Gain (dB) of the effect for a sine at hz. */
float gainAtDb (const char* type, double hz, const std::vector<float>& params, float amplitude = 0.25f)
{
    const auto in = sine (hz, 1.0, amplitude);
    return toneDb (renderEffect (type, in, params), hz) - toneDb (in, hz);
}

std::vector<float> add (std::vector<float> a, const std::vector<float>& b)
{
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i];
    return a;
}

/** Harmonic-rich test tone: sum of harmonics 1/h of f0 up to 8 kHz, peak about 0.5. */
std::vector<float> harmonicTone (double f0, double seconds)
{
    std::vector<float> v (size_t (seconds * kSr), 0.0f);
    for (int h = 1; h * f0 <= 8000.0; ++h)
    {
        const auto s = sine (h * f0, seconds, 0.15f / float (h));
        for (size_t i = 0; i < v.size(); ++i) v[i] += s[i];
    }
    return v;
}

float rmsDbAt (const std::vector<float>& x, int start, int len) { return rmsDb (x.data() + start, len); }

class EffectsATests : public juce::UnitTest
{
public:
    EffectsATests() : juce::UnitTest ("Effects A (12 types)", "EffectsA") {}

    void runTest() override
    {
        beginTest ("all 12 registered, latency 0, mix 0 = dry, no allocation while processing");
        for (auto* type : kTypes) checkBasics (type);

        beginTest ("AC-14 standard checks");
        for (auto* type : kTypes) runAc14Checks (*this, type);

        beginTest ("compressor: loud sine is reduced, quiet sine untouched");
        {
            const auto loud = sine (1000.0, 1.0, 0.5f);
            const float drop = rmsDbAt (renderEffect ("compressor", loud), kSettle, kWindow) - rmsDbAt (loud, kSettle, kWindow);
            // peak -6 dBFS, threshold -24, ratio 3: -12 dB at the peaks, a little less on average
            logMessage ("compressor: loud sine " + juce::String (drop, 1) + " dB");
            expectLessThan (drop, -7.0f, "compressor gain reduction");
            expectGreaterThan (drop, -14.0f, "compressor gain reduction not overdone");
            const auto quiet = sine (1000.0, 1.0, 0.01f);
            const float d2 = rmsDbAt (renderEffect ("compressor", quiet), kSettle, kWindow) - rmsDbAt (quiet, kSettle, kWindow);
            expectWithinAbsoluteError (d2, 0.0f, 0.1f, "compressor below threshold");
            const auto mk = renderEffect ("compressor", quiet, paramsWith ("compressor", { { "makeupDb", 12.0f } }));
            expectWithinAbsoluteError (rmsDbAt (mk, kSettle, kWindow) - rmsDbAt (quiet, kSettle, kWindow), 12.0f, 0.1f, "makeup");
        }

        beginTest ("deesser: strong 7 kHz is reduced, 500 Hz and quiet sibilance untouched");
        {
            const auto in = add (sine (500.0, 1.0, 0.1f), sine (7000.0, 1.0, 0.3f));
            const auto out = renderEffect ("deesser", in, paramsWith ("deesser", { { "freqHz", 7000.0f }, { "reductionDb", 12.0f } }));
            const float d7k = toneDb (out, 7000.0) - toneDb (in, 7000.0);
            const float d500 = toneDb (out, 500.0) - toneDb (in, 500.0);
            expectLessOrEqual (d7k, -9.0f, "7 kHz reduction");
            expectWithinAbsoluteError (d500, 0.0f, 0.3f, "500 Hz unchanged");
            const auto soft = sine (7000.0, 1.0, 0.003f);
            const auto softOut = renderEffect ("deesser", soft, paramsWith ("deesser", { { "freqHz", 7000.0f } }));
            expectWithinAbsoluteError (toneDb (softOut, 7000.0) - toneDb (soft, 7000.0), 0.0f, 0.1f, "quiet 7 kHz unchanged");
        }

        beginTest ("enhancer: adds harmonics above freqHz, leaves the low band alone");
        {
            const auto in = sine (4000.0, 1.0, 0.3f);
            const auto out = renderEffect ("enhancer", in, paramsWith ("enhancer", { { "amount", 0.3f }, { "mix", 1.0f } }));
            const float fund = toneDb (out, 4000.0);
            expectGreaterThan (toneDb (out, 8000.0) - fund, -40.0f, "2nd harmonic");
            expectGreaterThan (toneDb (out, 12000.0) - fund, -40.0f, "3rd harmonic");
            expectLessThan (toneDb (in, 8000.0) - toneDb (in, 4000.0), -80.0f, "input is clean");
            expectWithinAbsoluteError (gainAtDb ("enhancer", 300.0, {}), 0.0f, 0.5f, "300 Hz unchanged");
        }

        beginTest ("eq: shelves, peak, high-pass and low-pass hit their targets");
        {
            expectWithinAbsoluteError (gainAtDb ("eq", 50.0, paramsWith ("eq", { { "lowDb", 12.0f } })), 12.0f, 1.0f, "low shelf at 50 Hz");
            expectGreaterThan (gainAtDb ("eq", 100.0, paramsWith ("eq", { { "lowDb", 12.0f } })), 9.0f, "low shelf at 100 Hz");
            expectWithinAbsoluteError (gainAtDb ("eq", 6000.0, paramsWith ("eq", { { "lowDb", 12.0f } })), 0.0f, 0.3f, "low shelf leaves 6 kHz");
            expectWithinAbsoluteError (gainAtDb ("eq", 1000.0, paramsWith ("eq", { { "midDb", 12.0f } })), 12.0f, 0.3f, "mid peak at 1 kHz");
            expectWithinAbsoluteError (gainAtDb ("eq", 2000.0, paramsWith ("eq", { { "midDb", -12.0f }, { "midHz", 2000.0f } })), -12.0f, 0.3f, "mid cut at 2 kHz");
            expectWithinAbsoluteError (gainAtDb ("eq", 15000.0, paramsWith ("eq", { { "highDb", 12.0f } })), 12.0f, 1.0f, "high shelf at 15 kHz");
            expectWithinAbsoluteError (gainAtDb ("eq", 200.0, paramsWith ("eq", { { "highDb", 12.0f } })), 0.0f, 0.3f, "high shelf leaves 200 Hz");
            expectLessThan (gainAtDb ("eq", 100.0, paramsWith ("eq", { { "hpfHz", 1000.0f } })), -35.0f, "high-pass 1 kHz drops 100 Hz");
            expectWithinAbsoluteError (gainAtDb ("eq", 4000.0, paramsWith ("eq", { { "hpfHz", 1000.0f } })), 0.0f, 0.5f, "high-pass passes 4 kHz");
            expectLessThan (gainAtDb ("eq", 8000.0, paramsWith ("eq", { { "lpfHz", 1000.0f } })), -30.0f, "low-pass 1 kHz drops 8 kHz");
            for (double hz : { 100.0, 1000.0, 10000.0 })
                expectWithinAbsoluteError (gainAtDb ("eq", hz, {}), 0.0f, 0.2f, "defaults are flat at " + juce::String (hz));
        }

        beginTest ("peq: peaks with their own Q, shelves at their own frequency");
        {
            expectWithinAbsoluteError (gainAtDb ("peq", 500.0, paramsWith ("peq", { { "mid1Db", 12.0f } })), 12.0f, 0.3f, "mid1 +12 at 500 Hz");
            expectWithinAbsoluteError (gainAtDb ("peq", 4000.0, paramsWith ("peq", { { "mid1Db", 12.0f } })), 0.0f, 1.0f, "mid1 leaves 4 kHz");
            const auto narrow = paramsWith ("peq", { { "mid2Db", -12.0f }, { "mid2Q", 8.0f } });
            expectWithinAbsoluteError (gainAtDb ("peq", 3000.0, narrow), -12.0f, 0.3f, "mid2 Q8 -12 at 3 kHz");
            expectWithinAbsoluteError (gainAtDb ("peq", 2000.0, narrow), 0.0f, 1.0f, "mid2 Q8 leaves 2 kHz");
            expectWithinAbsoluteError (gainAtDb ("peq", 30.0, paramsWith ("peq", { { "lowDb", -12.0f }, { "lowHz", 200.0f } })), -12.0f, 1.0f, "low shelf");
            expectWithinAbsoluteError (gainAtDb ("peq", 20000.0, paramsWith ("peq", { { "highDb", 12.0f }, { "highHz", 4000.0f } })), 12.0f, 1.0f, "high shelf");
        }

        beginTest ("autowah: a louder input opens the band-pass higher");
        {
            // The wet path is loudness-matched to the input (spec §5.8), so single-tone gains all read ~0 dB.
            // Feed 400 Hz + 1600 Hz together and compare their balance: quiet input -> centre near baseHz
            // 400 Hz; loud input -> centre at 400 * 2^2 = 1600 Hz.
            const auto p = paramsWith ("autowah", { { "mix", 1.0f } });
            auto balance = [&] (float amp)
            {
                auto in = sine (400.0, 1.0, amp);
                const auto hi = sine (1600.0, 1.0, amp);
                for (size_t i = 0; i < in.size(); ++i) in[i] += hi[i];
                const auto out = renderEffect ("autowah", in, p);
                return toneDb (out, 400.0) - toneDb (out, 1600.0);
            };
            const float quiet = balance (0.005f), loud = balance (0.25f);
            logMessage ("autowah: 400 Hz minus 1600 Hz, quiet " + juce::String (quiet, 1) + " dB, loud " + juce::String (loud, 1) + " dB");
            expectGreaterThan (quiet, 12.0f, "quiet: centre near baseHz");
            expectLessThan (loud, -12.0f, "loud: centre two octaves up");
            const auto voice = synthVoice (2.0, 4);
            expectWithinAbsoluteError (rmsDbAt (renderEffect ("autowah", voice, p), int (kSr), int (kSr)), rmsDbAt (voice, int (kSr), int (kSr)), 2.0f,
                                       "keeps the voice level");
        }

        beginTest ("filtersweep: the cutoff moves periodically");
        {
            const auto in = whiteNoise (2.5, 0.3f, 5);
            const auto out = renderEffect ("filtersweep", in, paramsWith ("filtersweep", { { "rateHz", 1.0f }, { "baseHz", 300.0f }, { "depthOct", 4.0f }, { "q", 0.7f } }));
            float lo = 1000.0f, hi = -1000.0f;
            const int win = int (0.02 * kSr);
            for (int s = int (0.5 * kSr); s + win <= int (out.size()); s += win)
            {
                lo = std::min (lo, rmsDbAt (out, s, win));
                hi = std::max (hi, rmsDbAt (out, s, win));
            }
            expectGreaterThan (hi - lo, 8.0f, "RMS swing of the swept low-pass");
            const auto bp = paramsWith ("filtersweep", { { "mode", 1.0f }, { "baseHz", 1000.0f }, { "depthOct", 0.0f }, { "q", 8.0f } });
            expectWithinAbsoluteError (gainAtDb ("filtersweep", 1000.0, bp), 9.03f, 0.3f, "band-pass peak +sqrt(Q) at baseHz");
            expectLessThan (gainAtDb ("filtersweep", 2000.0, bp), -10.0f, "band-pass rejects an octave up");
            const auto hp = paramsWith ("filtersweep", { { "mode", 2.0f }, { "baseHz", 2000.0f }, { "depthOct", 0.0f }, { "q", 0.7f } });
            expectLessThan (gainAtDb ("filtersweep", 250.0, hp), -30.0f, "high-pass rejects 3 octaves down");
        }

        beginTest ("isolator: flat at 0 dB, a band at -60 dB disappears");
        {
            for (double hz : { 100.0, 300.0, 1000.0, 2500.0, 8000.0 })
                expectWithinAbsoluteError (gainAtDb ("isolator", hz, {}), 0.0f, 0.1f, "flat sum at " + juce::String (hz));
            expectLessThan (gainAtDb ("isolator", 60.0, paramsWith ("isolator", { { "lowDb", -60.0f } })), -40.0f, "low band gone");
            expectWithinAbsoluteError (gainAtDb ("isolator", 900.0, paramsWith ("isolator", { { "lowDb", -60.0f } })), 0.0f, 1.0f, "mid kept");
            expectLessThan (gainAtDb ("isolator", 900.0, paramsWith ("isolator", { { "midDb", -60.0f } })), -20.0f, "mid band gone");
            expectLessThan (gainAtDb ("isolator", 12000.0, paramsWith ("isolator", { { "highDb", -60.0f } })), -40.0f, "high band gone");
            expectWithinAbsoluteError (gainAtDb ("isolator", 100.0, paramsWith ("isolator", { { "lowDb", 6.0f } })), 6.0f, 0.5f, "low +6");
        }

        beginTest ("formantfilter: vowel a puts F2 near 1200 Hz, vowel i near 2300 Hz, loudness kept");
        {
            const auto in = harmonicTone (100.0, 1.0);
            const auto a = renderEffect ("formantfilter", in, paramsWith ("formantfilter", { { "vowel", 0.0f } }));
            const auto i = renderEffect ("formantfilter", in, paramsWith ("formantfilter", { { "vowel", 1.0f } }));
            const float aDiff = toneDb (a, 1200.0) - toneDb (a, 2300.0), iDiff = toneDb (i, 2300.0) - toneDb (i, 1200.0);
            logMessage ("formant: a 1200-2300 = " + juce::String (aDiff, 1) + " dB, i 2300-1200 = " + juce::String (iDiff, 1) + " dB");
            expectGreaterThan (aDiff, 10.0f, "vowel a: 1200 Hz over 2300 Hz");
            expectGreaterThan (iDiff, 10.0f, "vowel i: 2300 Hz over 1200 Hz");
            const auto voice = synthVoice (3.0, 9);
            const auto out = renderEffect ("formantfilter", voice);
            expectWithinAbsoluteError (rmsDbAt (out, 24000, 120000) - rmsDbAt (voice, 24000, 120000), 0.0f, 2.0f, "level match (synthetic voice)");
        }

        beginTest ("saturator: tube adds even harmonics, tape odd only, loudness within 2 dB");
        {
            const auto in = sine (1000.0, 1.0, 0.5f);
            const auto tape = renderEffect ("saturator", in, paramsWith ("saturator", { { "mode", 0.0f }, { "driveDb", 12.0f }, { "toneHz", 16000.0f } }));
            const auto tube = renderEffect ("saturator", in, paramsWith ("saturator", { { "mode", 1.0f }, { "driveDb", 12.0f }, { "toneHz", 16000.0f } }));
            expectGreaterThan (toneDb (tape, 3000.0) - toneDb (tape, 1000.0), -30.0f, "tape 3rd harmonic");
            expectLessThan (toneDb (tape, 2000.0) - toneDb (tape, 1000.0), -60.0f, "tape has no 2nd harmonic");
            expectGreaterThan (toneDb (tube, 2000.0) - toneDb (tube, 1000.0), -30.0f, "tube 2nd harmonic");
            const auto voice = synthVoice (3.0, 9);
            for (float drive : { 0.0f, 6.0f, 24.0f })
                for (float mode : { 0.0f, 1.0f })
                {
                    const auto out = renderEffect ("saturator", voice, paramsWith ("saturator", { { "mode", mode }, { "driveDb", drive } }));
                    expectWithinAbsoluteError (rmsDbAt (out, 24000, 120000) - rmsDbAt (voice, 24000, 120000), 0.0f, 2.0f,
                                               "saturator loudness, mode " + juce::String (mode) + " drive " + juce::String (drive));
                }
        }

        beginTest ("distortion: harmonics grow with drive, fuzz is asymmetric, loudness kept");
        {
            const auto in = sine (1000.0, 1.0, 0.3f);
            const auto dist = renderEffect ("distortion", in, paramsWith ("distortion", { { "driveDb", 24.0f }, { "toneHz", 12000.0f }, { "mix", 1.0f } }));
            const auto fuzz = renderEffect ("distortion", in, paramsWith ("distortion", { { "shape", 2.0f }, { "driveDb", 12.0f }, { "toneHz", 12000.0f }, { "mix", 1.0f } }));
            const auto clean = renderEffect ("distortion", in, paramsWith ("distortion", { { "shape", 0.0f }, { "driveDb", 0.0f }, { "toneHz", 12000.0f }, { "mix", 1.0f } }));
            expectGreaterThan (toneDb (dist, 3000.0) - toneDb (dist, 1000.0), -15.0f, "hard clip 3rd harmonic");
            expectLessThan (toneDb (dist, 2000.0) - toneDb (dist, 1000.0), -60.0f, "hard clip is symmetric");
            expectGreaterThan (toneDb (fuzz, 2000.0) - toneDb (fuzz, 1000.0), -25.0f, "fuzz 2nd harmonic");
            expectLessThan (toneDb (clean, 3000.0) - toneDb (clean, 1000.0), -30.0f, "overdrive at 0 dB is nearly clean");
            const auto voice = synthVoice (3.0, 9);
            for (float drive : { 0.0f, 12.0f, 36.0f })
            {
                const auto out = renderEffect ("distortion", voice, paramsWith ("distortion", { { "driveDb", drive }, { "mix", 1.0f } }));
                expectWithinAbsoluteError (rmsDbAt (out, 24000, 120000) - rmsDbAt (voice, 24000, 120000), 0.0f, 2.0f,
                                           "distortion loudness, drive " + juce::String (drive));
            }
        }

        beginTest ("bitcrusher: 4 bits raise the quantisation error, rateHz holds samples");
        {
            const auto in = sine (440.0, 1.0, 0.5f);
            auto errDb = [&] (float bits)
            {
                auto out = renderEffect ("bitcrusher", in, paramsWith ("bitcrusher", { { "bits", bits } }));
                for (size_t k = 0; k < out.size(); ++k) out[k] -= in[k];
                return rmsDb (out);
            };
            const float e4 = errDb (4.0f), e16 = errDb (16.0f);
            logMessage ("bitcrusher error: 4 bit " + juce::String (e4, 1) + " dB, 16 bit " + juce::String (e16, 1) + " dB");
            expectWithinAbsoluteError (e4, -28.8f, 3.0f, "4-bit error near step/sqrt(12)");
            expectLessThan (e16, -95.0f, "16-bit error");
            const auto held = renderEffect ("bitcrusher", in, paramsWith ("bitcrusher", { { "rateHz", 4000.0f } }));
            int changes = 0;
            for (size_t k = 1; k < held.size(); ++k) changes += held[k] != held[k - 1] ? 1 : 0;
            expectWithinAbsoluteError (double (changes), 4000.0, 100.0, "sample-and-hold updates per second");
        }

        beginTest ("CPU per 480-sample block (informational)");
        {
            // voice, then voice followed by silence (IIR tails decaying into denormals if FTZ is off)
            const auto voice = synthVoice (10.0, 4);
            const auto tail = concat ({ synthVoice (1.0, 4), silence (9.0) });
            auto usPerBlock = [] (const char* type, const std::vector<float>& input, bool ftz)
            {
                double best = 1.0e9;
                for (int run = 0; run < 3; ++run) // min of 3: other processes share the machine
                {
                    auto fx = createEffect (type);
                    fx->prepare (kSr, 480);
                    const auto p = paramsWith (type, {});
                    for (size_t k = 0; k < p.size(); ++k) fx->setParam (int (k), p[k]);
                    fx->reset();
                    auto buf = input;
                    std::optional<juce::ScopedNoDenormals> noDenormals;
                    if (ftz) noDenormals.emplace();
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    for (size_t pos = 0; pos + 480 <= buf.size(); pos += 480) fx->process (buf.data() + pos, 480);
                    const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                    best = std::min (best, sec * 1.0e6 / double (buf.size() / 480));
                }
                return best;
            };
            for (auto* type : kTypes)
            {
                const double v = usPerBlock (type, voice, false), t = usPerBlock (type, tail, false), tf = usPerBlock (type, tail, true);
                logMessage (juce::String (type) + ": voice " + juce::String (v, 2) + " us/block (" + juce::String (v / 100.0, 3)
                            + " % of 10 ms); voice+silence " + juce::String (t, 2) + " us/block, with FTZ/DAZ " + juce::String (tf, 2));
            }
        }
    }

private:
    void checkBasics (const char* type)
    {
        auto fx = createEffect (type);
        expect (fx != nullptr, juce::String ("not registered: ") + type);
        if (fx == nullptr) return;
        auto* info = findEffectInfo (type);
        fx->prepare (kSr, 480);
        expectEquals (fx->getLatencySamples(), 0, type);

        // no allocation in setParam / reset / process (AC-39 style)
        const auto p = paramsWith (type, {});
        auto buf = synthVoice (0.5, 2);
        long long allocations = 0;
        {
            AllocationCounter counter;
            for (size_t k = 0; k < p.size(); ++k) fx->setParam (int (k), p[k]);
            fx->reset();
            for (size_t pos = 0; pos + 480 <= buf.size(); pos += 480)
            {
                for (size_t k = 0; k < p.size(); ++k) fx->setParam (int (k), info->params[k].clamp (p[k] + 0.1f * (info->params[k].max - info->params[k].min)));
                fx->process (buf.data() + pos, 480);
            }
            allocations = counter.count();
        }
        expectEquals (int (allocations), 0, juce::String ("allocations in ") + type);

        if (info->paramIndex ("mix") >= 0)
        {
            const auto in = synthVoice (1.0, 6);
            const auto out = renderEffect (type, in, paramsWith (type, { { "mix", 0.0f } }));
            float maxDiff = 0.0f;
            for (size_t k = 0; k < in.size(); ++k) maxDiff = std::max (maxDiff, std::abs (out[k] - in[k]));
            expectLessOrEqual (maxDiff, 1.0e-6f, juce::String (type) + ": mix 0 must be the dry signal");
        }
    }
};

EffectsATests effectsATests;
} // namespace
} // namespace koe

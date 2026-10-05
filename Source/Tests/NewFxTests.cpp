// wave10/fx (INTERFACES.md §12.3): the four effects added in wave 10 — freqshift 周波数シフター, octaver オクターバー,
// resonator レゾネーター, tapestop テープストップ. Category "NewFx". No window, no audio device (AppController (false)).
// Signals are synthetic, so measured values (CPU, loudness) are "合成音声で代用".

#include "App/AppController.h"
#include "BinaryData.h" // KoeLoomPresetData
#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Dsp/Building.h"
#include "Dsp/IVoiceShifter.h"
#include "Effects/EffectRegistry.h"
#include "Engine/EffectChain.h"
#include "Model/Preset.h"
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
constexpr double kTwoPiD = 6.283185307179586;
const char* const kNewTypes[] = { "freqshift", "octaver", "resonator", "tapestop" };

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

/** Renders in 480-sample blocks; `atBlock (b)` runs before block b is processed. */
template <typename Fn>
std::vector<float> renderWith (IEffect& fx, std::vector<float> buf, Fn&& atBlock)
{
    for (int pos = 0, b = 0; pos < int (buf.size()); pos += kBlock, ++b)
    {
        atBlock (b);
        fx.process (buf.data() + pos, std::min (kBlock, int (buf.size()) - pos));
    }
    return buf;
}

std::vector<float> render (IEffect& fx, std::vector<float> buf) { return renderWith (fx, std::move (buf), [] (int) {}); }

float rmsDbOf (const std::vector<float>& x, double fromSec, double lenSec)
{
    const int a = int (fromSec * kSr), n = std::min (int (lenSec * kSr), int (x.size()) - a);
    return test::rmsDb (x.data() + a, n);
}

float peakDbOf (const std::vector<float>& x, double fromSec, double lenSec)
{
    const int a = int (fromSec * kSr), n = std::min (int (lenSec * kSr), int (x.size()) - a);
    return test::peakDb (x.data() + a, n);
}

/** Amplitude (dBFS of a sine) of the component at hz, Hann-windowed Goertzel over [fromSec, fromSec + lenSec). */
double toneDb (const std::vector<float>& x, double hz, double fromSec, double lenSec)
{
    const int a = int (fromSec * kSr), n = int (lenSec * kSr);
    static std::vector<double> window; // the scans call this thousands of times with one length
    if (int (window.size()) != n)
    {
        window.resize (size_t (n));
        for (int i = 0; i < n; ++i) window[size_t (i)] = 0.5 - 0.5 * std::cos (kTwoPiD * i / (n - 1));
    }
    const double coef = 2.0 * std::cos (kTwoPiD * hz / kSr);
    double s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = window[size_t (i)];
        const double s = w * x[size_t (a + i)] + coef * s1 - s2;
        s2 = s1;
        s1 = s;
        wsum += w;
    }
    const double power = s1 * s1 + s2 * s2 - coef * s1 * s2;
    return 20.0 * std::log10 (2.0 * std::sqrt (std::max (0.0, power)) / wsum + 1.0e-12);
}

/** A steady voice-like tone: harmonics of f0 at 1/k, peak about 0.5. */
std::vector<float> harmonicTone (double f0, double seconds, int harmonics = 20)
{
    std::vector<float> v (size_t (seconds * kSr), 0.0f);
    for (int k = 1; k <= harmonics && k * f0 < kSr * 0.45; ++k)
        for (size_t i = 0; i < v.size(); ++i) v[i] += float (0.25 / k * std::sin (kTwoPiD * k * f0 * double (i) / kSr));
    return v;
}

float maxAdjacentDiff (const std::vector<float>& x, size_t from, size_t to)
{
    float d = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i) d = std::max (d, std::abs (x[i] - x[i - 1]));
    return d;
}

float maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    float d = 0.0f;
    for (size_t i = from; i < std::min ({ to, a.size(), b.size() }); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

std::vector<float> renderProcessor (VoiceProcessor& vp, const std::vector<float>& in)
{
    std::vector<float> out (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
    return out;
}
} // namespace

class NewFxTests : public juce::UnitTest
{
public:
    NewFxTests() : juce::UnitTest ("Wave 10 effects (freqshift, octaver, resonator, tapestop)", "NewFx") {}

    void runTest() override
    {
        testRegistry();
        testAc14();
        testNoAllocation();
        testDecayToSilence();
        testDefaultsAudible();
        testFreqShifter();
        testOctaver();
        testResonator();
        testTapeStop();
        testTapeStopHotkey();
        testPresets();
        measureCpu();
    }

private:
    void testRegistry()
    {
        beginTest ("registry: the 4 types at the end in order, categories of INTERFACES.md 12.3, Japanese names, factories");
        const auto& all = allEffectInfos();
        expect (all.size() >= 4);
        if (all.size() < 4) return;
        const EffectCategory cats[] = { EffectCategory::pitchVocoder, EffectCategory::pitchVocoder, EffectCategory::eqFilter, EffectCategory::special };
        for (size_t k = 0; k < 4; ++k)
        {
            const auto& info = all[all.size() - 4 + k];
            expectEquals (juce::String (info.type), juce::String (kNewTypes[k]));
            expect (info.category == cats[k], info.type);
            expect (juce::String::fromUTF8 (info.nameJa).isNotEmpty() && juce::String::fromUTF8 (info.descJa).isNotEmpty(), info.type);
            expect (hasEffectFactory (info.type), info.type);
            for (auto& p : info.params) expect (juce::String::fromUTF8 (p.nameJa).isNotEmpty(), juce::String (info.type) + "." + p.id);
            auto fx = makeFx (info.type, paramsOf (info.type));
            expectEquals (fx->getLatencySamples(), 0, juce::String (info.type) + ": latency 0");
        }
    }

    void testAc14()
    {
        beginTest ("AC-14: defaults / min / max / every choice finite and limiter peak <= -1 dBFS; 50 ms sweeps do not click");
        for (auto* type : kNewTypes) test::runAc14Checks (*this, type);

        beginTest ("AC-14 for tapestop while stopping and stopped (every curve, times at min and max)");
        const auto input = test::concat ({ test::referenceSpeechOrSynth(), test::whiteNoise (2.0, 0.1f, 5) });
        for (int curve = 0; curve < 3; ++curve)
            for (const float ms : { 100.0f, 5000.0f })
            {
                auto fx = makeFx ("tapestop", paramsOf ("tapestop", { { "stopMs", ms }, { "startMs", ms }, { "curve", curve } }));
                auto out = renderWith (*fx, input, [&] (int b)
                {
                    if (b == 20 || b == 400 || b == 420) fx->trigger (EffectTrigger::tapeStopToggle); // stop, start, stop again mid-way
                });
                const juce::String label = "curve " + juce::String (curve) + ", " + juce::String (ms, 0) + " ms";
                expect (test::allFinite (out), label);
                expectLessOrEqual (test::peakDb (out), test::peakDb (input) + 1.0f, label + ": never louder than the input");
            }
    }

    void testNoAllocation()
    {
        beginTest ("audio thread: no allocation in process / setParam / trigger / reset, every knob at min and max");
        const auto voice = test::synthVoice (1.0, 3);
        for (auto* type : kNewTypes)
        {
            auto* info = findEffectInfo (type);
            auto fx = makeFx (type, paramsOf (type));
            std::vector<float> buf (voice);
            auto run = [&]
            {
                std::copy (voice.begin(), voice.end(), buf.begin());
                for (size_t pos = 0; pos < buf.size(); pos += kBlock)
                    fx->process (buf.data() + pos, int (std::min<size_t> (kBlock, buf.size() - pos)));
            };
            test::AllocationCounter allocs;
            run();
            fx->trigger (EffectTrigger::tapeStopToggle);
            for (size_t p = 0; p < info->params.size(); ++p) fx->setParam (int (p), info->params[p].max);
            run();
            fx->trigger (EffectTrigger::tapeStopToggle);
            for (size_t p = 0; p < info->params.size(); ++p) fx->setParam (int (p), info->params[p].min);
            run();
            fx->reset();
            run();
            const int count = int (allocs.count());
            expectEquals (count, 0, juce::String (type) + " allocated on the audio thread");
            expect (test::allFinite (buf), type);
        }
    }

    void testDecayToSilence()
    {
        beginTest ("silence after the voice is silent again within 10 s, every numeric knob at its max (no oscillation)");
        const auto input = test::concat ({ test::synthVoice (2.0, 4), test::silence (10.0) });
        for (auto* type : kNewTypes)
        {
            auto* info = findEffectInfo (type);
            auto params = paramsOf (type);
            for (size_t p = 0; p < info->params.size(); ++p)
                if (! info->params[p].isChoice()) params[p] = info->params[p].max;
            auto fx = makeFx (type, params);
            const bool tape = juce::String (type) == "tapestop";
            auto out = renderWith (*fx, input, [&] (int b)
            {
                if (tape && (b == 100 || b == 300)) fx->trigger (EffectTrigger::tapeStopToggle); // stop in the voice, start in the silence
            });
            expect (test::allFinite (out), type);
            const float first = rmsDbOf (out, 2.0, 1.0), ninth = rmsDbOf (out, 10.0, 1.0), last = rmsDbOf (out, 11.5, 0.5);
            logMessage ("  " + juce::String (type).paddedRight (' ', 10) + "max knobs: 0-1 s after " + juce::String (first, 1) + " dBFS, 8-9 s "
                        + juce::String (ninth, 1) + ", 9.5-10 s " + juce::String (last, 1));
            expectLessOrEqual (last, -80.0f, juce::String (type) + " not silent 10 s after");
            expectLessOrEqual (last, ninth + 0.5f, juce::String (type) + " grows instead of decaying");
        }
    }

    void testDefaultsAudible()
    {
        beginTest ("defaults: freqshift / octaver / resonator change the voice clearly (tapestop waits for its button, like freeze)");
        const auto voice = test::synthVoice (3.0, 6);
        for (auto* type : { "freqshift", "octaver", "resonator" })
        {
            auto fx = makeFx (type, paramsOf (type));
            auto out = render (*fx, voice);
            std::vector<float> diff (voice.size());
            for (size_t i = 0; i < diff.size(); ++i) diff[i] = out[i] - voice[i];
            const float rel = rmsDbOf (diff, 0.5, 2.5) - rmsDbOf (voice, 0.5, 2.5);
            const float level = rmsDbOf (out, 0.5, 2.5) - rmsDbOf (voice, 0.5, 2.5);
            logMessage ("  " + juce::String (type).paddedRight (' ', 10) + "defaults: change " + juce::String (rel, 1) + " dB, loudness "
                        + juce::String (level, 1) + " dB vs the input" + juce::String::fromUTF8 (" (合成音声で代用)"));
            expectGreaterThan (rel, -10.0f, juce::String (type) + ": the default barely changes the voice");
            expect (std::abs (level) <= 6.0f, juce::String (type) + ": default loudness far from the input");
        }
    }

    void testFreqShifter()
    {
        beginTest ("freqshift: a sine at f moves to f + shift; f and the mirror f - shift stay below -30 dB");
        for (const auto& [f, shift] : { std::pair<double, double> { 440.0, 300.0 }, { 440.0, -200.0 }, { 1000.0, 700.0 }, { 2500.0, -1000.0 }, { 220.0, 37.0 } })
        {
            auto fx = makeFx ("freqshift", paramsOf ("freqshift", { { "shiftHz", shift }, { "mix", 1 } }));
            auto out = render (*fx, test::sine (f, 1.5, 0.5f));
            const double want = toneDb (out, f + shift, 0.5, 1.0);
            const double carrier = toneDb (out, f, 0.5, 1.0) - want;
            const double mirror = toneDb (out, std::abs (f - shift), 0.5, 1.0) - want;
            logMessage ("  " + juce::String (f, 0) + " Hz shifted " + juce::String (shift, 0) + " Hz: wanted " + juce::String (want, 2) + " dBFS, at f "
                        + juce::String (carrier, 1) + " dB, mirror " + juce::String (mirror, 1) + " dB");
            expectWithinAbsoluteError (want, double (dsp::gainToDb (0.5f)), 0.5);
            expectLessThan (carrier, -30.0);
            expectLessThan (mirror, -30.0);
        }

        beginTest ("freqshift: at 0 Hz every mix gives the same sound (the dry is the same all-pass: no comb filtering)");
        {
            const auto noise = test::whiteNoise (1.0, 0.2f, 9);
            auto at = [&] (double mix) { auto fx = makeFx ("freqshift", paramsOf ("freqshift", { { "shiftHz", 0 }, { "mix", mix } })); return render (*fx, noise); };
            const auto m0 = at (0.0), m5 = at (0.5), m1 = at (1.0);
            expectLessThan (maxAbsDiff (m0, m5, 0, m0.size()), 1.0e-5f);
            expectLessThan (maxAbsDiff (m0, m1, 0, m0.size()), 1.0e-5f);
            expectWithinAbsoluteError (rmsDbOf (m5, 0.1, 0.9), rmsDbOf (noise, 0.1, 0.9), 0.2f);
        }

        beginTest ("freqshift: the wobble swings the shift (depth 200 Hz at 1 Hz moves the tone)");
        {
            auto fx = makeFx ("freqshift", paramsOf ("freqshift", { { "shiftHz", 300 }, { "wobbleHz", 200 }, { "wobbleRateHz", 1 }, { "mix", 1 } }));
            auto out = render (*fx, test::sine (500.0, 1.0, 0.5f));
            // the LFO starts at phase 0: a quarter period in (0.25 s) the shift is 300 + 200 = 500 Hz, at 0.75 s it is 100 Hz
            const double hi = test::estimateF0 (out.data() + int (0.22 * kSr), int (0.06 * kSr), kSr, 100.0, 1500.0);
            const double lo = test::estimateF0 (out.data() + int (0.72 * kSr), int (0.06 * kSr), kSr, 100.0, 1500.0);
            logMessage ("  wobble: f0 near 0.25 s " + juce::String (hi, 1) + " Hz (1000), near 0.75 s " + juce::String (lo, 1) + " Hz (600)");
            expectWithinAbsoluteError (hi, 1000.0, 30.0);
            expectWithinAbsoluteError (lo, 600.0, 30.0);
        }
    }

    void testOctaver()
    {
        beginTest ("octaver: a 220 Hz voice gets 110 Hz (1 octave down) and 55 Hz (2 octaves down)");
        const auto voice = harmonicTone (220.0, 2.0);
        const double in220 = toneDb (voice, 220.0, 0.5, 1.0);
        const double in110 = toneDb (voice, 110.0, 0.5, 1.0);
        {
            auto fx = makeFx ("octaver", paramsOf ("octaver", { { "sub1", 1 }, { "sub2", 0 }, { "dry", 1 }, { "toneHz", 700 } }));
            auto out = render (*fx, voice);
            const double sub = toneDb (out, 110.0, 0.5, 1.0);
            logMessage ("  220 Hz voice: input at 110 Hz " + juce::String (in110, 1) + " dBFS, output " + juce::String (sub, 1) + " dBFS (the voice's 220 Hz: "
                        + juce::String (in220, 1) + ")");
            expectGreaterThan (sub, in110 + 40.0, "no 110 Hz component");
            expectGreaterThan (sub, in220 - 10.0, "the octave down is too quiet");
        }
        {
            auto fx = makeFx ("octaver", paramsOf ("octaver", { { "sub1", 0 }, { "sub2", 1 }, { "dry", 1 }, { "toneHz", 700 } }));
            auto out = render (*fx, voice);
            const double sub2 = toneDb (out, 55.0, 0.5, 1.0), sub1 = toneDb (out, 110.0, 0.5, 1.0);
            logMessage ("  220 Hz voice, 2 octaves down only: 55 Hz " + juce::String (sub2, 1) + " dBFS, 110 Hz " + juce::String (sub1, 1) + " dBFS");
            expectGreaterThan (sub2, in110 + 30.0, "no 55 Hz component");
        }

        beginTest ("octaver: on the gliding synthetic voice the sub follows half the voice's pitch in most voiced windows");
        {
            const auto in = test::synthVoice (4.0, 11, kSr, false);
            auto fx = makeFx ("octaver", paramsOf ("octaver", { { "sub1", 1 }, { "sub2", 0 }, { "dry", 0 } }));
            const auto out = render (*fx, in);
            const int win = int (0.06 * kSr);
            int voiced = 0, tracked = 0;
            for (int a = int (0.2 * kSr); a + win <= int (in.size()); a += win)
            {
                const double fIn = test::estimateF0 (in.data() + a, win, kSr, 70.0, 600.0);
                if (fIn <= 0.0) continue;
                ++voiced;
                const double fOut = test::estimateF0 (out.data() + a, win, kSr, 30.0, 400.0);
                tracked += std::abs (fOut / (fIn * 0.5) - 1.0) < 0.06 ? 1 : 0;
            }
            const double share = voiced > 0 ? double (tracked) / voiced : 0.0;
            logMessage ("  octave down tracked in " + juce::String (tracked) + " of " + juce::String (voiced) + " voiced 60 ms windows ("
                        + juce::String (share * 100.0, 0) + " %)");
            expectGreaterThan (share, 0.7, "the octave down does not follow the voice");
        }

        beginTest ("octaver: silent input stays silent, dry 1 with subs 0 is the input exactly");
        {
            auto fx = makeFx ("octaver", paramsOf ("octaver"));
            expectLessOrEqual (test::rmsDb (render (*fx, test::silence (1.0))), -120.0f);
            auto dryOnly = makeFx ("octaver", paramsOf ("octaver", { { "sub1", 0 }, { "sub2", 0 }, { "dry", 1 } }));
            const auto in = test::synthVoice (1.0, 2);
            expectEquals (maxAbsDiff (render (*dryOnly, in), in, 0, in.size()), 0.0f);
        }
    }

    void testResonator()
    {
        beginTest ("resonator: peaks on the note's harmonic series (A2 = 110 Hz, C4, B6) at any brightness");
        struct Case { int note, octave; double brightness; };
        for (const auto& c : { Case { 9, 2, 0.5 }, Case { 9, 2, 0.0 }, Case { 9, 2, 1.0 }, Case { 0, 4, 0.5 }, Case { 11, 6, 0.5 } })
        {
            const double f0 = 440.0 * std::pow (2.0, (double (24 + 12 * (c.octave - 1) + c.note) - 69.0) / 12.0);
            auto fx = makeFx ("resonator", paramsOf ("resonator", { { "note", c.note }, { "octave", c.octave }, { "decayMs", 1500 }, { "brightness", c.brightness }, { "mix", 1 } }));
            std::vector<float> impulse (size_t (2.5 * kSr), 0.0f);
            impulse[size_t (0.1 * kSr)] = 0.5f;
            const auto out = render (*fx, impulse);
            juce::String line;
            for (int k = 1; k <= 4 && k * f0 < 4000.0; ++k)
            {
                // scan +-5 % around k * f0 for the strongest frequency
                double best = 0.0, bestDb = -1e9;
                for (double hz = k * f0 * 0.95; hz <= k * f0 * 1.05; hz += k * f0 * 0.0005)
                    if (const double db = toneDb (out, hz, 0.1, 2.0); db > bestDb) { bestDb = db; best = hz; }
                const double cents = test::centsBetween (best, k * f0);
                const double valley = bestDb - toneDb (out, (k + 0.5) * f0, 0.1, 2.0);
                line << " k" << k << " " << juce::String (cents, 1) << " ct / " << juce::String (valley, 1) << " dB";
                expectLessOrEqual (std::abs (cents), 15.0, "peak " + juce::String (k) + " off pitch at " + juce::String (f0, 1) + " Hz");
                expectGreaterThan (valley, 10.0, "no peak " + juce::String (k) + " at " + juce::String (f0, 1) + " Hz");
            }
            logMessage ("  " + juce::String (f0, 1) + " Hz, brightness " + juce::String (c.brightness, 1) + ":" + line);
        }

        beginTest ("resonator: the longest ring does not jump in level (noise, then a sine on the note) and keeps the voice's loudness");
        {
            const auto in = test::concat ({ test::whiteNoise (2.0, 0.1f, 4), test::sine (110.0, 2.0, 0.3f), test::synthVoice (3.0, 8) });
            auto fx = makeFx ("resonator", paramsOf ("resonator", { { "note", 9 }, { "octave", 2 }, { "decayMs", 3000 }, { "brightness", 1 }, { "mix", 1 } }));
            const auto out = render (*fx, in);
            const float noiseLevel = rmsDbOf (out, 1.0, 1.0) - rmsDbOf (in, 1.0, 1.0);
            const float sinePeak = peakDbOf (out, 2.0, 2.0), inPeak = peakDbOf (in, 2.0, 2.0);
            const float voiceLevel = rmsDbOf (out, 5.0, 2.0) - rmsDbOf (in, 5.0, 2.0);
            logMessage ("  max ring: noise " + juce::String (noiseLevel, 1) + " dB vs input, sine on the note peak " + juce::String (sinePeak, 1)
                        + " dBFS (input " + juce::String (inPeak, 1) + "), voice " + juce::String (voiceLevel, 1) + " dB vs input");
            expectLessOrEqual (sinePeak, inPeak + 6.0f, "jumps when the sine hits the resonance");
            expect (noiseLevel > -12.0f && noiseLevel < 3.0f, "noise loudness not kept");
            expect (std::abs (voiceLevel) <= 6.0f, "voice loudness not kept");

            // the voice starting after silence: the gain is high while the ring builds up; it must not overshoot
            const auto onset = test::concat ({ test::silence (0.5), test::synthVoice (2.0, 3) });
            for (const auto& [note, octave] : { std::pair<int, int> { 9, 2 }, { 0, 4 }, { 11, 6 } })
            {
                auto fx2 = makeFx ("resonator", paramsOf ("resonator", { { "note", note }, { "octave", octave }, { "decayMs", 3000 }, { "brightness", 1 }, { "mix", 1 } }));
                const auto out2 = render (*fx2, onset);
                const float onsetPeak = peakDbOf (out2, 0.5, 0.5), onsetIn = peakDbOf (onset, 0.5, 0.5);
                const juce::String label = "note " + juce::String (note) + " octave " + juce::String (octave);
                logMessage ("  max ring, " + label + ", voice after silence: first 0.5 s peak " + juce::String (onsetPeak, 1) + " dBFS (input "
                            + juce::String (onsetIn, 1) + "), 0.5-2 s loudness " + juce::String (rmsDbOf (out2, 1.0, 1.5) - rmsDbOf (onset, 1.0, 1.5), 1) + " dB vs input");
                expectLessOrEqual (onsetPeak, onsetIn + 6.0f, label + ": overshoots when the voice starts");
            }
        }

        beginTest ("resonator: changing the note on a steady voice does not click (glides)");
        {
            const auto voice = test::synthVoice (2.2, 3, kSr, false);
            auto fx = makeFx ("resonator", paramsOf ("resonator"));
            const int at = int (kSr * 1.0) / kBlock;
            auto out = renderWith (*fx, voice, [&] (int b) { if (b == at) fx->setParam (0, 7.0f); });
            const double r = test::clickRatio (out, at * kBlock, at * kBlock + int (0.05 * kSr));
            expectLessOrEqual (r, 2.0, "click ratio " + juce::String (r, 2));
        }
    }

    void testTapeStop()
    {
        beginTest ("tapestop: playing = the input exactly; stop -> pitch falls -> silence; start -> no delay left, the input exactly");
        const auto tone = test::sine (400.0, 4.0, 0.5f);
        auto fx = makeFx ("tapestop", paramsOf ("tapestop", { { "stopMs", 500 }, { "startMs", 300 }, { "curve", 0 } }));
        const int stopBlock = 100, startBlock = 250; // 1.0 s, 2.5 s
        std::vector<int> states;
        auto out = renderWith (*fx, tone, [&] (int b)
        {
            if (b == stopBlock || b == startBlock) fx->trigger (EffectTrigger::tapeStopToggle);
            states.push_back (fx->getUiState());
        });
        expectEquals (maxAbsDiff (out, tone, 0, size_t (1.0 * kSr)), 0.0f, "before the stop: the input untouched");
        // linear curve: a quarter of the way in (1.125 s) the speed is 0.75 -> 300 Hz; halfway (1.25 s) 0.5 -> 200 Hz
        const double f75 = test::estimateF0 (out.data() + int (1.11 * kSr), int (0.03 * kSr), kSr, 120.0, 1000.0);
        const double f50 = test::estimateF0 (out.data() + int (1.235 * kSr), int (0.03 * kSr), kSr, 120.0, 1000.0);
        logMessage ("  stopping: f0 " + juce::String (f75, 1) + " Hz at 1.125 s (300), " + juce::String (f50, 1) + " Hz at 1.25 s (200)");
        expectWithinAbsoluteError (f75, 300.0, 25.0);
        expectWithinAbsoluteError (f50, 200.0, 25.0);
        expectLessOrEqual (test::peakDb (out.data() + int (1.51 * kSr), int (0.98 * kSr)), -120.0f, "stopped: silence");
        expectEquals (states[size_t (stopBlock + 1)], 1);
        expectEquals (states[size_t (startBlock - 1)], 1);
        expectEquals (states[size_t (startBlock + 1)], 0);
        const size_t back = size_t (2.5 * kSr + 0.3 * kSr + 0.020 * kSr) + 2;
        expectEquals (maxAbsDiff (out, tone, back, out.size()), 0.0f, "after starting: no delay left (latency 0)");
        // pitch and level never jump: the steepest step stays near the 400 Hz input's (the 20 ms crossfade adds a little)
        expectLessOrEqual (maxAdjacentDiff (out, 0, out.size()), maxAdjacentDiff (tone, 0, tone.size()) * 1.2f, "a click somewhere");

        beginTest ("tapestop: toggling mid-way (stop, start halfway, stop again) stays continuous; reset() plays at once");
        {
            auto fx2 = makeFx ("tapestop", paramsOf ("tapestop", { { "stopMs", 1000 }, { "startMs", 1000 }, { "curve", 1 } }));
            auto out2 = renderWith (*fx2, tone, [&] (int b)
            {
                if (b == 50 || b == 100 || b == 130) fx2->trigger (EffectTrigger::tapeStopToggle);
            });
            expect (test::allFinite (out2));
            expectLessOrEqual (maxAdjacentDiff (out2, 0, out2.size()), maxAdjacentDiff (tone, 0, tone.size()) * 1.2f, "a click while toggling");
            expectEquals (fx2->getUiState(), 1);
            fx2->reset();
            expectEquals (fx2->getUiState(), 0);
            const auto again = render (*fx2, tone);
            expectEquals (maxAbsDiff (again, tone, 0, tone.size()), 0.0f, "reset(): playing, the input exactly");
        }

        beginTest ("tapestop: every curve reaches silence in stopMs and comes back in startMs");
        for (int curve = 0; curve < 3; ++curve)
        {
            auto fx3 = makeFx ("tapestop", paramsOf ("tapestop", { { "stopMs", 800 }, { "startMs", 600 }, { "curve", curve } }));
            auto out3 = renderWith (*fx3, tone, [&] (int b) { if (b == 50 || b == 250) fx3->trigger (EffectTrigger::tapeStopToggle); });
            expectLessOrEqual (test::peakDb (out3.data() + int (1.31 * kSr), int (1.18 * kSr)), -120.0f, "curve " + juce::String (curve) + ": silent after stopMs");
            expectEquals (maxAbsDiff (out3, tone, size_t (3.13 * kSr), out3.size()), 0.0f, "curve " + juce::String (curve) + ": back to the input");
        }
    }

    void testTapeStopHotkey()
    {
        beginTest ("tapestop: the tapeStopToggle hotkey stops the tape in the engine and starts it again");
        freshDataDir();
        auto c = std::make_unique<AppController> (false);
        c->startup();
        c->updateSettings ([] (Settings& s)
        {
            s.setupDone = true;
            s.tourStep = 7;
        });
        c->setVoiceChangerOn (true);
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        juce::String why;
        expect (c->addEffect ("tapestop", why), why);
        const int slot = int (c->getChain().size()) - 1;
        c->setSlotParam (slot, 0, 300.0f); // stopMs
        auto& vp = c->getProcessorForTests();
        const auto voice = test::synthVoice (1.0, 5);
        renderProcessor (vp, voice); // the new chain is in
        expect (rmsDbOf (renderProcessor (vp, voice), 0.2, 0.7) > -40.0f, "the voice comes out while playing");
        expectEquals (c->getSlotUiState (slot), 0);

        c->performAction ("tapeStopToggle");
        const auto stopped = renderProcessor (vp, voice);
        expectEquals (c->getSlotUiState (slot), 1);
        const float tail = rmsDbOf (stopped, 0.6, 0.39);
        logMessage ("  after the hotkey: the last 0.4 s at " + juce::String (tail, 1) + " dBFS");
        expectLessThan (tail, -90.0f, "the hotkey did not stop the tape");

        c->performAction ("tapeStopToggle");
        const auto back = renderProcessor (vp, voice);
        expectEquals (c->getSlotUiState (slot), 0);
        expect (rmsDbOf (back, 0.6, 0.39) > -40.0f, "the hotkey did not start the tape again");
        c->shutdown();
    }

    void testPresets()
    {
        beginTest ("built-in presets: one per effect, embedded, read with no corrections, trim within range; finite through the chain");
        const std::pair<const char*, const char*> presets[] = { { "character-metal-android", "freqshift" }, { "character-kaiju", "octaver" },
                                                                { "device-metal-pipe", "resonator" }, { "device-tape-stop", "tapestop" } };
        const auto voice = test::synthVoice (4.0, 7);
        for (auto& [id, type] : presets)
        {
            const juce::String file = juce::String (id) + ".json";
            const char* data = nullptr;
            int size = 0;
            for (int i = 0; i < KoeLoomPresetData::namedResourceListSize && data == nullptr; ++i)
                if (file == KoeLoomPresetData::getNamedResourceOriginalFilename (KoeLoomPresetData::namedResourceList[i]))
                    data = KoeLoomPresetData::getNamedResource (KoeLoomPresetData::namedResourceList[i], size);
            expect (data != nullptr, file + " is not embedded");
            if (data == nullptr) continue;
            PresetLoadReport r;
            auto p = parsePreset (juce::String::fromUTF8 (data, size), r);
            expect (p.has_value() && ! r.hasNotices(), file + ": " + r.toJapanese());
            if (! p) continue;
            expect (p->id == id && p->name.length() <= kPresetNameMaxChars && kTrimDb.clamp (p->outputTrimDb) == p->outputTrimDb, file);
            bool uses = false;
            for (auto& slot : p->chain) uses = uses || slot.type == type;
            expect (uses, file + " does not use " + type);
            auto chain = EffectChain::create (p->chain, kSr, kBlock);
            auto buf = voice;
            for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock) chain->process (buf.data() + pos, kBlock);
            expect (test::allFinite (buf), file);
            logMessage ("  " + juce::String (id).paddedRight (' ', 24) + "loudness " + juce::String (rmsDbOf (buf, 0.5, 3.0) - rmsDbOf (voice, 0.5, 3.0), 1)
                        + " dB vs the input (chain only" + juce::String::fromUTF8 ("、合成音声で代用)"));
        }
    }

    void measureCpu()
    {
        beginTest ("CPU per 480-sample block (decides W::; logged) and the octaver against a -12 st layer voice");
        const auto voice = test::synthVoice (10.0);
        auto time = [&] (auto&& processBlock)
        {
            std::vector<float> buf (voice), out (voice.size());
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock) processBlock (buf.data() + pos, out.data() + pos);
            const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            return sec * 1.0e6 / (double (buf.size()) / kBlock);
        };
        for (auto* type : kNewTypes)
        {
            auto fx = makeFx (type, paramsOf (type));
            if (juce::String (type) == "tapestop") fx->trigger (EffectTrigger::tapeStopToggle); // the moving tape costs more than playing
            const double us = time ([&] (float* x, float*) { fx->process (x, kBlock); });
            logMessage ("  " + juce::String (type).paddedRight (' ', 10) + juce::String (us, 1) + " us/block = " + juce::String (us / 100.0, 2) + " % of 10 ms");
            expectLessThan (us, 1000.0, juce::String (type) + " is far too slow"); // light = under 1 % of 10 ms (koeloom_effects.md §4), read from the log
        }
        auto layer = createConverterShifter (1);
        layer->prepare (kSr, kBlock);
        layer->setPitchSemitones (-12.0f);
        layer->reset();
        const double layerUs = time ([&] (float* x, float* y) { layer->process (x, y, kBlock); });
        logMessage (juce::String::fromUTF8 ("  layer -12 st (標準の変換器) ") + juce::String (layerUs, 1) + " us/block = " + juce::String (layerUs / 100.0, 2) + " % of 10 ms");
    }
};

static NewFxTests newFxTests;
} // namespace koe

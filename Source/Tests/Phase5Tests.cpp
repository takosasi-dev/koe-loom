// Phase 5: freeze, granular, looper, autopitch, vocoder, whisper, the pitch detector and the scale-layer
// pitch. AC-14 (all 6, plus the looper after record / play / overdub and the freeze held for 5 s),
// AC-46, AC-47, the automatic part of AC-48, freeze on / off / reset, vocoder and whisper behaviour,
// the YIN / MPM comparison (spec §5.7, Phase 0 T6 method) and CPU per block.
// Every signal is synthetic, so measured values are "合成音声で代用".

#include "Dsp/Building.h"
#include "Dsp/IVoiceShifter.h"
#include "Dsp/Limiter.h"
#include "Dsp/PitchDetector.h"
#include "Effects/EffectRegistry.h"
#include "Engine/ScaleLayerPitch.h"
#include "Engine/VoiceProcessor.h"
#include "Tests/EffectTestHarness.h"
#include "Tests/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <utility>

namespace koe
{
namespace
{
using test::kSr;
constexpr int kBlock = 480;
constexpr double kPiD = 3.14159265358979323846;

const char* const kTypes[] = { "freeze", "granular", "looper", "autopitch", "vocoder", "whisper" };

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

/** Sine, or (harmonics) the fundamental plus harmonics 2..5 at 1/k, about the same peak. */
std::vector<float> tone (double hz, double seconds, bool harmonics = false, float amp = 0.3f)
{
    std::vector<float> v (size_t (seconds * kSr));
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double ph = 2.0 * kPiD * hz * double (i) / kSr;
        double s = std::sin (ph);
        if (harmonics)
        {
            for (int k = 2; k <= 5; ++k) s += std::sin (k * ph) / k;
            s *= 0.5;
        }
        v[i] = amp * float (s);
    }
    return v;
}

/** Amplitude (dBFS of a sine) of the component at hz in x[start, start+len) (Goertzel). */
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

double f0Of (const std::vector<float>& v, double fromSec, double lenSec)
{
    return test::estimateF0 (v.data() + int (fromSec * kSr), int (lenSec * kSr), kSr, 50.0, 1100.0);
}

float rmsDbOf (const std::vector<float>& v, double fromSec, double lenSec)
{
    return test::rmsDb (v.data() + int (fromSec * kSr), int (lenSec * kSr));
}

/** RMS envelope in 10 ms windows over [fromSec, toSec). */
std::vector<double> envelope10ms (const std::vector<float>& x, double fromSec, double toSec)
{
    std::vector<double> e;
    const int w = int (kSr * 0.01);
    for (int p = int (fromSec * kSr); p + w <= int (toSec * kSr); p += w)
    {
        double s = 0.0;
        for (int i = 0; i < w; ++i) s += double (x[size_t (p + i)]) * x[size_t (p + i)];
        e.push_back (std::sqrt (s / w));
    }
    return e;
}

double correlation (const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min (a.size(), b.size());
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= double (n);
    mb /= double (n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return saa > 0.0 && sbb > 0.0 ? sab / std::sqrt (saa * sbb) : 0.0;
}

float maxAbsDiff (const std::vector<float>& a, size_t aFrom, const std::vector<float>& b, size_t bFrom, size_t n)
{
    float m = 0.0f;
    for (size_t i = 0; i < n; ++i) m = std::max (m, std::abs (a[aFrom + i] - b[bFrom + i]));
    return m;
}

// ---- pitch detector measurement (Phase 0 T6 method) ----
struct DetectorResult
{
    double onsetMs = 0, upMs = 0, downMs = 0, meanCents = 0, maxCents = 0;
    int unvoicedSteady = 0, steadyFrames = 0;
};

/** 0.5 s silence, 1.5 s at hz, +4 st for 1 s, back to hz for 1 s (phase continuous). Detection after
    every hop; latency = newest sample of the first frame that is within +-10 cents and stays there for
    2 more frames, minus the change; steady error over frames from 100 ms after a change. */
DetectorResult measureDetector (dsp::PitchDetector::Method method, double hz, bool harmonics)
{
    const double up = hz * std::exp2 (4.0 / 12.0);
    const int e0 = int (0.5 * kSr), e1 = int (2.0 * kSr), e2 = int (3.0 * kSr), end = int (4.0 * kSr);
    std::vector<float> x (size_t (end), 0.0f);
    double ph = 0.0;
    for (int i = e0; i < end; ++i)
    {
        const double f = (i >= e1 && i < e2) ? up : hz;
        ph += 2.0 * kPiD * f / kSr;
        double s = std::sin (ph);
        if (harmonics)
        {
            for (int k = 2; k <= 5; ++k) s += std::sin (k * ph) / k;
            s *= 0.5;
        }
        x[size_t (i)] = float (0.3 * s);
    }

    dsp::PitchDetector d;
    dsp::PitchDetector::Settings set;
    set.method = method;
    d.prepare (kSr, set);
    const int hop = d.getHopSamples();
    std::vector<float> fr;
    for (int pos = 0; pos + hop <= end; pos += hop)
    {
        d.process (x.data() + pos, hop);
        fr.push_back (d.getFrequencyHz());
    }
    auto frameEnd = [&] (size_t k) { return int ((k + 1) * size_t (hop)) - 1; };
    auto ok = [&] (size_t k, double truth) { return k < fr.size() && std::abs (test::centsBetween (fr[k], truth)) <= 10.0; };
    auto latency = [&] (int ev, double truth)
    {
        for (size_t k = 0; k < fr.size(); ++k)
            if (frameEnd (k) >= ev && ok (k, truth) && ok (k + 1, truth) && ok (k + 2, truth))
                return double (frameEnd (k) - ev) * 1000.0 / kSr;
        return 1.0e9;
    };
    DetectorResult r;
    r.onsetMs = latency (e0, hz);
    r.upMs = latency (e1, up);
    r.downMs = latency (e2, hz);
    double sum = 0.0;
    const int settle = int (0.1 * kSr);
    for (size_t k = 0; k < fr.size(); ++k)
    {
        const int t = frameEnd (k);
        double truth = 0.0;
        if (t >= e0 + settle && t < e1) truth = hz;
        else if (t >= e1 + settle && t < e2) truth = up;
        else if (t >= e2 + settle) truth = hz;
        if (truth == 0.0) continue;
        ++r.steadyFrames;
        if (fr[k] <= 0.0f) { ++r.unvoicedSteady; continue; }
        const double c = std::abs (test::centsBetween (fr[k], truth));
        sum += c;
        r.maxCents = std::max (r.maxCents, c);
    }
    r.meanCents = sum / std::max (1, r.steadyFrames - r.unvoicedSteady);
    return r;
}

class Phase5Tests : public juce::UnitTest
{
public:
    Phase5Tests() : juce::UnitTest ("Phase 5", "Phase5") {}

    void runTest() override
    {
        beginTest ("AC-14: every Phase 5 type (finite, limiter, 50 ms sweeps)");
        for (auto* type : kTypes)
            expect (test::runAc14Checks (*this, type), juce::String ("AC-14 failed for ") + type);

        beginTest ("AC-14: looper after 5 s record + play + overdub, freeze held on for 5 s");
        {
            const auto prime = test::synthVoice (5.0, 9, kSr, false);
            auto feed = [&] (IEffect& fx, double seconds)
            {
                auto b = std::vector<float> (prime.begin(), prime.begin() + long (seconds * kSr));
                run (fx, b);
            };
            auto looperPlayed = [&] (IEffect& fx, bool endInOverdub)
            {
                fx.trigger (EffectTrigger::looperRecordPlay);
                feed (fx, 5.0);
                if (fx.getUiState() == 1) fx.trigger (EffectTrigger::looperRecordPlay); // play (maxSec 5 already closed it)
                feed (fx, 1.0);
                fx.trigger (EffectTrigger::looperRecordPlay); // overdub
                feed (fx, 1.0);
                if (! endInOverdub) fx.trigger (EffectTrigger::looperRecordPlay); // play
                return fx.getUiState() == (endInOverdub ? 3 : 2);
            };
            expect (ac14InState ("looper", [&] (IEffect& fx) { return looperPlayed (fx, false); }), "looper (playing)");
            expect (ac14InState ("looper", [&] (IEffect& fx) { return looperPlayed (fx, true); }), "looper (overdubbing)");
            expect (ac14InState ("freeze", [&] (IEffect& fx)
                                 {
                                     feed (fx, 1.0);
                                     fx.trigger (EffectTrigger::freezeToggle);
                                     feed (fx, 5.0);
                                     return fx.getUiState() == 1;
                                 }),
                    "freeze (held on)");
        }

        beginTest ("no allocation in process / setParam / reset / trigger");
        for (auto* type : kTypes)
        {
            auto fx = makeFx (type, paramsOf (type));
            auto buf = test::synthVoice (0.5);
            auto* info = findEffectInfo (type);
            test::AllocationCounter allocs;
            fx->trigger (EffectTrigger::freezeToggle);
            fx->trigger (EffectTrigger::looperRecordPlay);
            run (*fx, buf);
            fx->trigger (EffectTrigger::looperRecordPlay);
            for (size_t p = 0; p < info->params.size(); ++p) fx->setParam (int (p), info->params[p].max);
            run (*fx, buf);
            fx->trigger (EffectTrigger::looperRecordPlay);
            fx->reset();
            run (*fx, buf);
            fx->trigger (EffectTrigger::looperClear);
            run (*fx, buf);
            const auto count = allocs.count(); // before the message string below is built
            expectEquals (int (count), 0, juce::String (type) + " allocated on the audio thread");
        }

        testAutoPitch();
        testScaleLayer();
        testLooper();
        testFreeze();
        testVocoderWhisper();
        testGranular();
        testDetectors();
        testCpu();
    }

private:
    /** AC-14 with the effect first brought into a state (looper recorded, freeze on): defaults, every
        numeric param at min / max and every choice must stay finite and <= -1 dBFS after the limiter;
        each numeric param swept over 50 ms must not click. Same checks as test::runAc14Checks. */
    bool ac14InState (const char* type, const std::function<bool (IEffect&)>& prime)
    {
        auto* info = findEffectInfo (type);
        const auto input = test::concat ({ test::referenceSpeechOrSynth(), test::silence (5.0), test::whiteNoise (5.0, 0.1f, 11) });
        bool good = true;
        auto check = [&] (const std::vector<float>& params, const juce::String& label)
        {
            auto fx = makeFx (type, params);
            const bool primed = prime (*fx);
            expect (primed, juce::String (type) + " did not reach the state: " + label);
            auto out = input;
            run (*fx, out);
            const bool finite = test::allFinite (out);
            expect (finite, juce::String (type) + " non-finite output: " + label);
            good = good && primed && finite;
            if (! finite) return;
            Limiter lim;
            lim.prepare (kSr);
            for (size_t pos = 0; pos < out.size(); pos += kBlock) lim.process (out.data() + pos, int (std::min<size_t> (kBlock, out.size() - pos)));
            const float pk = test::peakDb (out);
            expect (pk <= -0.99f, juce::String (type) + " limiter peak " + juce::String (pk, 2) + " dB: " + label);
            good = good && pk <= -0.99f;
        };
        const auto defaults = paramsOf (type);
        check (defaults, "defaults");
        for (size_t p = 0; p < info->params.size(); ++p)
        {
            const auto& spec = info->params[p];
            if (spec.isChoice())
            {
                for (int c = 0; c < int (spec.choices.size()); ++c)
                {
                    auto v = defaults;
                    v[p] = float (c);
                    check (v, juce::String (spec.id) + "=" + spec.choices[size_t (c)].first);
                }
                continue;
            }
            auto lo = defaults, hi = defaults;
            lo[p] = spec.min;
            hi[p] = spec.max;
            check (lo, juce::String (spec.id) + "=min");
            check (hi, juce::String (spec.id) + "=max");
        }

        const auto voice = test::synthVoice (2.2, 3);
        for (size_t p = 0; p < info->params.size(); ++p)
        {
            const auto& spec = info->params[p];
            if (spec.isChoice()) continue;
            auto v = defaults;
            v[p] = spec.min;
            auto fx = makeFx (type, v);
            prime (*fx);
            auto out = voice;
            const int opStart = int (kSr * 1.0), startBlock = opStart / kBlock, sweepBlocks = int (0.050 * kSr) / kBlock;
            for (int pos = 0, b = 0; pos < int (out.size()); pos += kBlock, ++b)
            {
                if (b >= startBlock && b <= startBlock + sweepBlocks)
                    fx->setParam (int (p), spec.clamp (spec.min + float (b - startBlock) / float (sweepBlocks) * (spec.max - spec.min)));
                fx->process (out.data() + pos, std::min (kBlock, int (out.size()) - pos));
            }
            const double r = test::clickRatio (out, opStart, opStart + int (0.050 * kSr));
            expect (r <= 2.0, juce::String (type) + " (held state) click on sweep of " + spec.id + ": ratio " + juce::String (r, 2));
            good = good && r <= 2.0;
        }
        return good;
    }

    void testAutoPitch()
    {
        beginTest ("AC-46: autopitch 215 Hz -> 220 Hz (A, chromatic), 360 Hz -> F 349.23 Hz (C major), strength 0 = input");
        auto worstCents = [&] (double inHz, int key, int scale, double strength, double expectHz)
        {
            const auto in = test::sine (inHz, 1.5, 0.3f);
            const auto out = test::renderEffect ("autopitch", in, paramsOf ("autopitch", { { "key", key }, { "scale", scale }, { "retuneMs", 0 }, { "strength", strength } }));
            double worst = 0.0;
            for (double t = 0.25; t + 0.1 <= 1.5; t += 0.1) // 200 ms after the onset + the 32 ms latency, onwards
                worst = std::max (worst, std::abs (test::centsBetween (f0Of (out, t, 0.1), expectHz)));
            return worst;
        };
        const double a = worstCents (215.0, 9, 0, 1.0, 220.0);
        const double b = worstCents (360.0, 0, 1, 1.0, 349.23);
        const double c = worstCents (215.0, 9, 0, 0.0, 215.0);
        logMessage ("  worst error from 200 ms on: 215->220 " + juce::String (a, 2) + " cents, 360->349.23 " + juce::String (b, 2)
                    + " cents, strength 0: " + juce::String (c, 2) + " cents from the input");
        expectLessOrEqual (a, 10.0);
        expectLessOrEqual (b, 10.0);
        expectLessOrEqual (c, 2.0);

        beginTest ("autopitch: minor = natural minor, unvoiced input passes uncorrected (E-28), latency reported");
        {
            // 300 Hz in A minor (A B C D E F G): D4 293.66 is nearest
            const double m = worstCents (300.0, 9, 2, 1.0, 293.66);
            expectLessOrEqual (m, 10.0);
            auto fx = makeFx ("autopitch", paramsOf ("autopitch", { { "key", 0 }, { "scale", 1 }, { "retuneMs", 0 } }));
            const int lat = fx->getLatencySamples();
            expect (lat > 0 && lat <= int (0.04 * kSr), "latency " + juce::String (lat));
            // noise has no pitch: the effect is the plain delayed signal (shifter at 0 st)
            auto noise = test::whiteNoise (1.0, 0.1f, 5);
            auto out = noise;
            run (*fx, out);
            float worst = 0.0f;
            double ref = 0.0;
            for (size_t i = size_t (0.3 * kSr); i < noise.size(); ++i)
            {
                worst = std::max (worst, std::abs (out[i] - noise[i - size_t (lat)]));
                ref = std::max (ref, double (std::abs (noise[i])));
            }
            logMessage ("  noise through autopitch: max |out - delayed in| = " + juce::String (worst, 4) + " (input peak " + juce::String (ref, 3) + ")");
            expect (test::findLag (noise, out, 4000) == lat, "measured delay differs from getLatencySamples");
        }
    }

    void testScaleLayer()
    {
        beginTest ("AC-47: scale-layer pitch, C major +2: C4 -> E4, D4 -> F4, silence -> NaN");
        {
            expectEquals (dsp::scaleShiftSemitones (60.0f, 0, dsp::Scale::major, 2), 4.0f);
            expectEquals (dsp::scaleShiftSemitones (62.0f, 0, dsp::Scale::major, 2), 3.0f);
            expectEquals (dsp::scaleShiftSemitones (57.0f, 9, dsp::Scale::minor, 2), 3.0f);   // A3 -> C4 in A minor
            expectEquals (dsp::scaleShiftSemitones (60.0f, 0, dsp::Scale::major, -2), -3.0f); // C4 -> A3
            expectEquals (dsp::scaleShiftSemitones (60.0f, 0, dsp::Scale::major, 7), 12.0f);  // an octave
            expectWithinAbsoluteError (dsp::scaleShiftSemitones (60.3f, 0, dsp::Scale::major, 2), 3.7f, 1.0e-4f);

            ScaleLayerPitch s;
            s.prepare (kSr, kBlock);
            auto layerHz = [&] (double hz)
            {
                s.reset();
                auto in = test::sine (hz, 0.5, 0.3f);
                for (size_t pos = 0; pos < in.size(); pos += kBlock) s.analyse (in.data() + pos, kBlock);
                return hz * std::exp2 (double (s.layerSemitones (0, false, 2)) / 12.0);
            };
            const double e4 = layerHz (261.63), f4 = layerHz (293.66);
            logMessage ("  C4 -> " + juce::String (e4, 2) + " Hz, D4 -> " + juce::String (f4, 2) + " Hz");
            expectLessOrEqual (std::abs (test::centsBetween (e4, 329.63)), 10.0);
            expectLessOrEqual (std::abs (test::centsBetween (f4, 349.23)), 10.0);

            // silence after the tone: NaN within 20 ms
            const auto quiet = test::silence (0.1);
            int nanAfter = -1;
            for (int b = 0; b * kBlock < int (quiet.size()); ++b)
            {
                s.analyse (quiet.data() + b * kBlock, kBlock);
                if (nanAfter < 0 && std::isnan (s.layerSemitones (0, false, 2))) nanAfter = (b + 1) * kBlock;
            }
            logMessage ("  NaN after " + juce::String (nanAfter * 1000.0 / kSr, 1) + " ms of silence (480-sample blocks)");
            expect (nanAfter > 0 && nanAfter <= int (0.02 * kSr));
        }

        beginTest ("AC-47: through VoiceProcessor::setScaleLayerPitch (phase vocoder), layer = with - without");
        {
            const auto in = test::concat ({ test::sine (261.63, 1.5, 0.2f), test::silence (0.5) });
            auto renderVp = [&] (bool layerOn)
            {
                ScaleLayerPitch slp;
                slp.prepare (kSr, kBlock);
                VoiceProcessor vp;
                vp.setNoiseSuppression (false, 1.0f);
                vp.setGate (false, -45, 5, 80, 120);
                vp.setInputGainDb (0);
                vp.setOutputGainDb (0);
                vp.setVoiceChangerOn (true);
                vp.setShifterFactory ([] { return createPhaseVocoderShifter(); });
                vp.prepare (kSr, kBlock);
                vp.setShifter (true, 0, 0);
                VoiceProcessor::LayerParams lp;
                lp.active = layerOn;
                lp.scale = true;
                lp.key = 0;
                lp.minor = false;
                lp.degree = 2;
                lp.levelDb = 0;
                vp.setLayer (0, lp);
                vp.setScaleLayerPitch (&slp);
                std::vector<float> out (in.size());
                for (size_t pos = 0; pos < in.size(); pos += kBlock)
                    vp.process (in.data() + pos, out.data() + pos, nullptr, int (std::min<size_t> (kBlock, in.size() - pos)));
                vp.setScaleLayerPitch (nullptr);
                return out;
            };
            const auto with = renderVp (true), without = renderVp (false);
            std::vector<float> layer (with.size());
            for (size_t i = 0; i < layer.size(); ++i) layer[i] = with[i] - without[i];
            const double f = f0Of (layer, 0.8, 0.5);
            logMessage ("  layer f0 " + juce::String (f, 2) + " Hz (E4 = 329.63), level " + juce::String (rmsDbOf (layer, 0.8, 0.5), 1) + " dBFS");
            expectLessOrEqual (std::abs (test::centsBetween (f, 329.63)), 10.0);
            // after the input stops, the layer is gone within detection (<= 1 block) + 20 ms + a block
            int silentFrom = -1;
            const int w = int (0.001 * kSr);
            for (int p = int (1.5 * kSr); p + w <= int (layer.size()); p += w)
            {
                if (test::rmsDb (layer.data() + p, w) > -60.0f) silentFrom = -1;
                else if (silentFrom < 0) silentFrom = p;
            }
            const double ms = (silentFrom - 1.5 * kSr) * 1000.0 / kSr;
            logMessage ("  layer below -60 dBFS " + juce::String (ms, 1) + " ms after the input went silent");
            expect (silentFrom > 0 && ms <= 50.0);
        }
    }

    void testLooper()
    {
        const int L = int (5.0 * kSr);
        const auto rec = test::whiteNoise (5.0, 0.2f, 21);

        beginTest ("AC-48: looper loop length = recorded length to the sample; state cycle 0-1-2-3-2-3");
        auto fx = makeFx ("looper", paramsOf ("looper", { { "levelDb", 0 }, { "maxSec", 30 } }));
        expectEquals (fx->getUiState(), 0);
        fx->trigger (EffectTrigger::looperRecordPlay);
        expectEquals (fx->getUiState(), 1);
        auto buf = rec;
        run (*fx, buf);
        expectEquals (maxAbsDiff (buf, 0, rec, 0, rec.size()), 0.0f, "recording passes the voice unchanged");
        fx->trigger (EffectTrigger::looperRecordPlay);
        expectEquals (fx->getUiState(), 2);
        std::vector<float> out (size_t (2 * L + 960), 0.0f);
        run (*fx, out);
        const size_t e = 300; // keep clear of the 5 ms fades at the loop ends
        expectLessOrEqual (maxAbsDiff (out, e, rec, e, size_t (L) - 2 * e), 1.0e-7f, "first pass = recording");
        expectLessOrEqual (maxAbsDiff (out, size_t (L) + e, rec, e, size_t (L) - 2 * e), 1.0e-7f, "second pass starts exactly L later");
        expect (maxAbsDiff (out, size_t (L) + e, rec, e + 1, 1000) > 0.01f, "a loop one sample longer would not match");

        beginTest ("AC-48: overdub adds the voice to the loop");
        {
            const int startPos = (2 * L + 960) % L; // loop position when the overdub starts
            const auto add = test::sine (440.0, 5.0, 0.1f);
            fx->trigger (EffectTrigger::looperRecordPlay);
            expectEquals (fx->getUiState(), 3);
            auto dub = add;
            run (*fx, dub); // exactly one loop
            fx->trigger (EffectTrigger::looperRecordPlay);
            expectEquals (fx->getUiState(), 2);
            std::vector<float> after (size_t (L), 0.0f);
            run (*fx, after);
            float worst = 0.0f;
            for (int i = 300; i < L - 300; ++i)
            {
                const int p = (startPos + i) % L;
                if (p < 300 || p > L - 300) continue; // loop-end fades
                worst = std::max (worst, std::abs (after[size_t (i)] - (rec[size_t (p)] + add[size_t (i)])));
            }
            expectLessOrEqual (worst, 1.0e-5f);
            fx->trigger (EffectTrigger::looperRecordPlay);
            expectEquals (fx->getUiState(), 3);
        }

        beginTest ("AC-48: recording stops at maxSec; a smaller maxSec waits for the next recording");
        {
            auto m = makeFx ("looper", paramsOf ("looper", { { "maxSec", 5 } }));
            m->trigger (EffectTrigger::looperRecordPlay);
            auto six = test::concat ({ rec, test::whiteNoise (1.0, 0.2f, 22) });
            run (*m, six);
            expectEquals (m->getUiState(), 2);
            std::vector<float> o (size_t (L), 0.0f);
            run (*m, o);
            const int pos0 = int (1.0 * kSr); // playback started at 5 s
            expectLessOrEqual (maxAbsDiff (o, 300, rec, size_t (pos0 + 300), size_t (L - pos0 - 600)), 1.0e-7f);

            auto big = makeFx ("looper", paramsOf ("looper", { { "maxSec", 30 } }));
            big->trigger (EffectTrigger::looperRecordPlay);
            auto r = six;
            run (*big, r);
            big->setParam (1, 5.0f); // already 6 s recorded
            big->trigger (EffectTrigger::looperRecordPlay);
            std::vector<float> o2 (six.size(), 0.0f);
            run (*big, o2);
            expectLessOrEqual (maxAbsDiff (o2, 300, six, 300, six.size() - 600), 1.0e-7f, "6 s loop kept");
        }

        beginTest ("looper: reset() keeps the recording (closes an open one), clear empties it");
        {
            auto r = makeFx ("looper", paramsOf ("looper"));
            r->trigger (EffectTrigger::looperRecordPlay);
            auto two = std::vector<float> (rec.begin(), rec.begin() + long (2.0 * kSr));
            run (*r, two);
            r->reset(); // slot OFF -> ON while recording
            expectEquals (r->getUiState(), 2);
            const int L2 = int (2.0 * kSr);
            std::vector<float> o (size_t (2 * L2), 0.0f);
            run (*r, o);
            expectLessOrEqual (maxAbsDiff (o, 300, rec, 300, size_t (L2 - 600)), 1.0e-7f);
            expectLessOrEqual (maxAbsDiff (o, size_t (L2 + 300), rec, 300, size_t (L2 - 600)), 1.0e-7f);
            r->reset(); // OFF -> ON while playing: still there, from the start
            expectEquals (r->getUiState(), 2);
            std::vector<float> o2 (size_t (L2), 0.0f);
            run (*r, o2);
            expectLessOrEqual (maxAbsDiff (o2, 300, rec, 300, size_t (L2 - 600)), 1.0e-7f);
            r->trigger (EffectTrigger::looperClear);
            expectEquals (r->getUiState(), 0);
            std::vector<float> o3 (size_t (0.1 * kSr), 0.0f);
            run (*r, o3);
            float tail = 0.0f;
            for (size_t i = size_t (0.025 * kSr); i < o3.size(); ++i) tail = std::max (tail, std::abs (o3[i]));
            expectEquals (tail, 0.0f, "silent 25 ms after clear");
            r->reset();
            expectEquals (r->getUiState(), 0);
        }
    }

    void testFreeze()
    {
        beginTest ("freeze: toggle on holds the last grainMs, off returns to the voice in 20 ms, reset() = off");
        auto fx = makeFx ("freeze", paramsOf ("freeze", { { "grainMs", 120 }, { "mix", 1 } }));
        expectEquals (fx->getUiState(), 0);
        const auto voice = test::synthVoice (3.0, 5, kSr, false);
        auto a = std::vector<float> (voice.begin(), voice.begin() + long (kSr));
        auto b = a;
        run (*fx, b);
        expectEquals (maxAbsDiff (a, 0, b, 0, a.size()), 0.0f, "off = untouched");

        fx->trigger (EffectTrigger::freezeToggle);
        expectEquals (fx->getUiState(), 1);
        std::vector<float> held (size_t (2.0 * kSr), 0.0f); // silence in, the frozen grain out
        run (*fx, held);
        const int L = int (0.120 * kSr), T = int (kSr);
        // first pass (after the 20 ms fade, before the crossfade half) = the input just before the toggle
        expectLessOrEqual (maxAbsDiff (held, size_t (0.02 * kSr) + 1, voice, size_t (T - L) + size_t (0.02 * kSr) + 1, size_t (L / 2) - size_t (0.02 * kSr) - 2), 1.0e-6f);
        expectLessOrEqual (maxAbsDiff (held, size_t (0.1 * kSr), held, size_t (0.1 * kSr) + size_t (L), size_t (kSr)), 1.0e-6f, "periodic, period grainMs");
        expectGreaterThan (rmsDbOf (held, 0.5, 1.0), -30.0f);

        fx->trigger (EffectTrigger::freezeToggle);
        expectEquals (fx->getUiState(), 0);
        auto c = std::vector<float> (voice.begin() + long (kSr), voice.begin() + long (2 * kSr));
        auto d = c;
        run (*fx, d);
        expectEquals (maxAbsDiff (c, size_t (0.02 * kSr) + 1, d, size_t (0.02 * kSr) + 1, c.size() - size_t (0.02 * kSr) - 1), 0.0f, "voice again after 20 ms");

        fx->trigger (EffectTrigger::freezeToggle);
        expectEquals (fx->getUiState(), 1);
        fx->reset();
        expectEquals (fx->getUiState(), 0);
        auto e = c;
        run (*fx, e);
        expectEquals (maxAbsDiff (c, 0, e, 0, c.size()), 0.0f, "reset() turns the freeze off at once");
    }

    void testVocoderWhisper()
    {
        // voice 0.5 s, silence 1 s, voice 0.5 s, silence 1.2 s
        const auto v = test::synthVoice (1.0, 13);
        const auto gated = test::concat ({ std::vector<float> (v.begin(), v.begin() + long (0.5 * kSr)), test::silence (1.0),
                                           std::vector<float> (v.begin() + long (0.5 * kSr), v.end()), test::silence (1.2) });
        auto followsEnvelope = [&] (const char* type, const std::vector<float>& params)
        {
            const auto out = test::renderEffect (type, gated, params);
            expect (test::allFinite (out));
            const double c1 = correlation (envelope10ms (gated, 0.1, 0.5), envelope10ms (out, 0.1, 0.5));
            const double c2 = correlation (envelope10ms (gated, 1.6, 2.0), envelope10ms (out, 1.6, 2.0));
            const float on = rmsDbOf (out, 1.6, 0.4), in = rmsDbOf (gated, 1.6, 0.4), off = rmsDbOf (out, 2.9, 0.3);
            logMessage (juce::String ("  ") + type + ": envelope correlation " + juce::String (c1, 2) + " / " + juce::String (c2, 2)
                        + ", voiced " + juce::String (on, 1) + " dBFS (input " + juce::String (in, 1) + "), 0.9 s after the voice "
                        + juce::String (off, 1) + " dBFS");
            expectGreaterThan (std::min (c1, c2), 0.6);
            expectLessOrEqual (std::abs (on - in), 3.0f, "level kept within 3 dB");
            expectLessThan (off, in - 45.0f, "decays after the voice stops");
            return out;
        };

        beginTest ("vocoder: finite, follows the voice envelope, fixed noteSt sets the pitch");
        {
            const auto out = followsEnvelope ("vocoder", paramsOf ("vocoder", { { "carrierPitch", 1 }, { "noteSt", 48 } }));
            const double f = f0Of (out, 1.65, 0.3);
            logMessage ("  fixed note 48: output f0 " + juce::String (f, 2) + " Hz (130.81)");
            expectLessOrEqual (std::abs (test::centsBetween (f, 130.81)), 10.0);
            for (int ch = 0; ch < 3; ++ch) // every character keeps the level and the envelope
                followsEnvelope ("vocoder", paramsOf ("vocoder", { { "character", ch }, { "carrierPitch", 1 }, { "chord", 1 } }));
            followsEnvelope ("vocoder", paramsOf ("vocoder", { { "carrier", 2 } })); // noise: no pitch, always runs
            followsEnvelope ("vocoder", paramsOf ("vocoder", { { "carrier", 1 }, { "carrierPitch", 1 } }));
        }

        beginTest ("vocoder: follow tracks the voice pitch; unvoiced -> holds 50 ms, then stops (E-28)");
        {
            const auto in = test::concat ({ tone (200.0, 0.6, true), test::whiteNoise (0.6, 0.1f, 3) });
            const auto out = test::renderEffect ("vocoder", in, paramsOf ("vocoder"));
            const double f = f0Of (out, 0.2, 0.3);
            logMessage ("  follow on a 200 Hz tone: f0 " + juce::String (f, 2) + " Hz; after the switch to noise: "
                        + juce::String (rmsDbOf (out, 0.61, 0.03), 1) + " dBFS (10-40 ms: held), "
                        + juce::String (rmsDbOf (out, 0.8, 0.4), 1) + " dBFS (200 ms on: stopped)");
            expectLessOrEqual (std::abs (test::centsBetween (f, 200.0)), 10.0);
            expectGreaterThan (rmsDbOf (out, 0.61, 0.03), -40.0f);
            expectLessThan (rmsDbOf (out, 0.8, 0.4), -60.0f);
        }

        beginTest ("whisper: finite, follows the voice envelope, no pitch left (noise between the harmonics)");
        {
            followsEnvelope ("whisper", paramsOf ("whisper"));
            for (double br : { -1.0, 1.0 })
                followsEnvelope ("whisper", paramsOf ("whisper", { { "brightness", br } }));
            // 200 Hz harmonic tone: the input has its energy at 400 / 600 / 800 / 1000 Hz and none in between;
            // the whisper fills the gaps like noise
            const auto in = tone (200.0, 1.0, true);
            const auto out = test::renderEffect ("whisper", in, paramsOf ("whisper"));
            auto harmMinusGap = [&] (const std::vector<float>& x)
            {
                double h = 0.0, g = 0.0;
                for (int k = 2; k <= 5; ++k)
                    for (double d = -6.0; d <= 6.0; d += 2.0)
                    {
                        h += std::pow (10.0, toneDb (x, int (0.4 * kSr), int (0.5 * kSr), 200.0 * k + d) / 10.0);
                        g += std::pow (10.0, toneDb (x, int (0.4 * kSr), int (0.5 * kSr), 200.0 * k + 100.0 + d) / 10.0);
                    }
                return 10.0 * std::log10 (h / g);
            };
            const double hin = harmMinusGap (in), hout = harmMinusGap (out);
            logMessage ("  harmonics vs gaps: input " + juce::String (hin, 1) + " dB, whisper " + juce::String (hout, 1) + " dB");
            expectGreaterThan (hin, 30.0);
            expectLessThan (std::abs (hout), 10.0);
        }
    }

    void testGranular()
    {
        beginTest ("granular: grains come from the last 200 ms, pitchSt shifts them, density sets the rate");
        {
            // pitch: +12 st grains of a 200 Hz tone, wet only -> 400 Hz
            const auto in = tone (200.0, 1.5);
            const auto out = test::renderEffect ("granular", in, paramsOf ("granular", { { "pitchSt", 12 }, { "density", 40 }, { "grainMs", 50 }, { "spray", 0 }, { "mix", 1 } }));
            const double f = f0Of (out, 0.5, 0.3);
            logMessage ("  +12 st grains of 200 Hz: f0 " + juce::String (f, 2) + " Hz");
            expectLessOrEqual (std::abs (test::centsBetween (f, 400.0)), 10.0);

            // an impulse: with spray 1 every copy of it lies within 200 ms + one grain (100 ms) after it
            std::vector<float> imp (size_t (kSr), 0.0f);
            imp[size_t (0.1 * kSr)] = 1.0f;
            const auto g = test::renderEffect ("granular", imp, paramsOf ("granular", { { "spray", 1 }, { "density", 40 }, { "grainMs", 100 }, { "mix", 1 } }));
            int last = 0;
            for (int i = 0; i < int (g.size()); ++i)
                if (std::abs (g[size_t (i)]) > 1.0e-6f) last = i;
            const double ms = (last - 0.1 * kSr) * 1000.0 / kSr;
            logMessage ("  impulse: last grain sample " + juce::String (ms, 1) + " ms after it");
            expect (last > int (0.1 * kSr) && ms <= 305.0);

            // density 2 / s, grains 20 ms, 2 s: 4 separate grains
            const auto d = test::renderEffect ("granular", tone (300.0, 2.0), paramsOf ("granular", { { "density", 2 }, { "grainMs", 20 }, { "spray", 0 }, { "mix", 1 } }));
            const auto env = envelope10ms (d, 0.0, 2.0);
            int bursts = 0;
            for (size_t i = 0; i < env.size(); ++i)
                if (env[i] > 0.005 && (i == 0 || env[i - 1] <= 0.005)) ++bursts;
            logMessage ("  density 2 over 2 s: " + juce::String (bursts) + " grains");
            expectEquals (bursts, 4);
        }
    }

    void testDetectors()
    {
        beginTest ("pitch detector: YIN vs MPM on 100-400 Hz sines and harmonic tones (Phase 0 T6 method)");
        using M = dsp::PitchDetector::Method;
        double worstLatency[2] = {}, worstDown[2] = {}, worstCents[2] = {};
        int unvoiced[2] = {};
        for (int mi = 0; mi < 2; ++mi)
        {
            const M m = mi == 0 ? M::mpm : M::yin;
            for (bool harm : { false, true })
                for (double hz : { 100.0, 150.0, 200.0, 300.0, 400.0 })
                {
                    const auto r = measureDetector (m, hz, harm);
                    logMessage (juce::String (mi == 0 ? "  MPM " : "  YIN ") + (harm ? "harmonic " : "sine     ") + juce::String (hz, 0)
                                + " Hz: onset " + juce::String (r.onsetMs, 2) + " ms, +4 st " + juce::String (r.upMs, 2) + " ms, -4 st "
                                + juce::String (r.downMs, 2) + " ms, steady error mean " + juce::String (r.meanCents, 3) + " / max "
                                + juce::String (r.maxCents, 3) + " cents, unvoiced " + juce::String (r.unvoicedSteady) + "/" + juce::String (r.steadyFrames));
                    worstLatency[mi] = std::max ({ worstLatency[mi], r.onsetMs, r.upMs }); // T6: onset and +4 st
                    worstDown[mi] = std::max (worstDown[mi], r.downMs);                       // extra, logged only
                    worstCents[mi] = std::max (worstCents[mi], r.maxCents);
                    unvoiced[mi] += r.unvoicedSteady;
                }
        }

        // synthetic speech: voiced ratio and error against the known f0 track (truth at the window centre)
        const auto speech = test::synthVoice (10.0, 7);
        double usPerBlock[2] = {}, p99[2] = {};
        for (int mi = 0; mi < 2; ++mi)
        {
            dsp::PitchDetector::Settings set;
            set.method = mi == 0 ? M::mpm : M::yin;
            dsp::PitchDetector d;
            d.prepare (kSr, set);
            const int hop = d.getHopSamples();
            int frames = 0, voiced = 0;
            std::vector<double> errs;
            for (int pos = 0; pos + hop <= int (speech.size()); pos += hop)
            {
                const int w = d.getWindowSamples();
                d.process (speech.data() + pos, hop);
                ++frames;
                if (! d.isVoiced()) continue;
                ++voiced;
                const double t = (pos + hop - 1 - w / 2) / kSr;
                const double truth = 140.0 + 35.0 * std::sin (2.0 * kPiD * 0.23 * t) + 2.0 * std::sin (2.0 * kPiD * 5.0 * t);
                errs.push_back (std::abs (test::centsBetween (d.getFrequencyHz(), truth)));
            }
            std::sort (errs.begin(), errs.end());
            const double med = errs.empty() ? 0.0 : errs[errs.size() / 2], e99 = errs.empty() ? 0.0 : errs[errs.size() * 99 / 100];
            logMessage (juce::String (mi == 0 ? "  MPM" : "  YIN") + " on synthetic speech: voiced " + juce::String (double (voiced) / frames, 4)
                        + ", error median " + juce::String (med, 2) + " / p99 " + juce::String (e99, 2) + " cents");

            // CPU: 480-sample blocks of the speech, min of 3 runs (mean), plus the p99 block of one run
            double best = 1.0e9;
            std::vector<double> per;
            for (int runIx = 0; runIx < 3; ++runIx)
            {
                d.reset();
                double total = 0.0;
                for (int pos = 0; pos + kBlock <= int (speech.size()); pos += kBlock)
                {
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    d.process (speech.data() + pos, kBlock);
                    const double us = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6;
                    total += us;
                    if (runIx == 0) per.push_back (us);
                }
                best = std::min (best, total / double (speech.size() / kBlock));
            }
            std::sort (per.begin(), per.end());
            usPerBlock[mi] = best;
            p99[mi] = per[per.size() * 99 / 100];
            logMessage (juce::String (mi == 0 ? "  MPM" : "  YIN") + " CPU: " + juce::String (best, 2) + " us per 480-sample block ("
                        + juce::String (best / 100.0, 3) + " % of 10 ms), p99 block " + juce::String (p99[mi], 2) + " us");
        }
        logMessage ("  worst latency (onset, +4 st) MPM " + juce::String (worstLatency[0], 2) + " ms / YIN " + juce::String (worstLatency[1], 2)
                    + " ms; -4 st MPM " + juce::String (worstDown[0], 2) + " / YIN " + juce::String (worstDown[1], 2)
                    + " ms; worst steady error MPM " + juce::String (worstCents[0], 3) + " / YIN " + juce::String (worstCents[1], 3) + " cents");

        // the default (MPM) must meet the §13.1 targets: 25 ms, +-10 cents, CPU 5 %
        expect (dsp::PitchDetector::Settings().method == M::mpm, "MPM is the default");
        expectLessOrEqual (worstLatency[0], 25.0, "MPM latency");
        expectLessOrEqual (worstCents[0], 10.0, "MPM steady error");
        expectEquals (unvoiced[0], 0, "MPM unvoiced frames on steady tones");
        expectLessOrEqual (usPerBlock[0] / 100.0, 5.0, "MPM CPU %");
    }

    void testCpu()
    {
        beginTest ("CPU per 480-sample block of the 6 types (informational, synthetic voice)");
        const auto voice = test::synthVoice (10.0, 4);
        struct Case { const char* type; const char* label; std::vector<float> params; };
        std::vector<Case> cases;
        for (auto* type : kTypes) cases.push_back ({ type, "defaults", paramsOf (type) });
        cases.push_back ({ "granular", "density 40, grainMs 200, pitch +12", paramsOf ("granular", { { "density", 40 }, { "grainMs", 200 }, { "pitchSt", 12 } }) });
        cases.push_back ({ "vocoder", "32 bands, major chord, follow", paramsOf ("vocoder", { { "bands", 32 }, { "chord", 1 } }) });
        cases.push_back ({ "whisper", "32 bands", paramsOf ("whisper", { { "bands", 32 } }) });
        for (auto& cs : cases)
        {
            const char* type = cs.type;
            double best = 1.0e9;
            for (int r = 0; r < 3; ++r) // min of 3: other processes share the machine
            {
                auto fx = makeFx (type, cs.params);
                fx->trigger (EffectTrigger::freezeToggle);    // freeze: frozen
                fx->trigger (EffectTrigger::looperRecordPlay); // looper: recording
                auto buf = voice;
                const auto t0 = juce::Time::getHighResolutionTicks();
                for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock)
                {
                    if (pos == size_t (5 * kSr)) fx->trigger (EffectTrigger::looperRecordPlay); // looper: playing
                    fx->process (buf.data() + pos, kBlock);
                }
                const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                best = std::min (best, sec * 1.0e6 / double (buf.size() / kBlock));
            }
            logMessage (juce::String ("  ") + type + " (" + cs.label + "): " + juce::String (best, 2) + " us/block (" + juce::String (best / 100.0, 3) + " % of 10 ms), weight "
                        + juce::String::fromUTF8 (weightNameJa (findEffectInfo (type)->weight)));
        }
    }
};

static Phase5Tests phase5Tests;
} // namespace
} // namespace koe

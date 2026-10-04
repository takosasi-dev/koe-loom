// wave7/tone: new kinds for voicechar (helmet, underwater, wall, vinyl, gasmask, stadium), distortion
// (rectifier, hardclip, wavefolder) and saturator (transistor, tapewear), plus their 11 built-in presets.
// runAc14Checks only exercises the default choice, so the per-kind checks (finite, limiter, no allocation,
// 50 ms sweeps, kind switches, latency, silence) are done here for every new kind.
// Signals are synthetic (synthVoice / sines), so measured values are "合成音声で代用".

#include "Dsp/Building.h"
#include "Dsp/Limiter.h"
#include "Effects/EffectRegistry.h"
#include "Model/PresetLibrary.h"
#include "Tests/EffectTestHarness.h"
#include "Tests/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <tuple>
#include <utility>

namespace koe
{
namespace
{
using test::kSr;
constexpr int kBlock = 480;
constexpr double kPiD = 3.14159265358979323846;

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

/** FNV-1a 64 over the float bit patterns. */
uint64_t hashOf (const std::vector<float>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (float f : v)
    {
        uint32_t b;
        std::memcpy (&b, &f, 4);
        for (int k = 0; k < 4; ++k) { h ^= (b >> (8 * k)) & 0xffu; h *= 1099511628211ull; }
    }
    return h;
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

std::vector<float> sum (std::vector<float> a, const std::vector<float>& b)
{
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i];
    return a;
}

/** Peak-to-peak pitch movement (cents) of a steady sine through the effect, 1 s .. 3.5 s. */
double pitchSwingCents (const std::vector<float>& out)
{
    double lo = 1e9, hi = 0.0;
    for (int p = int (kSr); p + 1920 < int (kSr * 3.5); p += 480)
    {
        const double f = test::estimateF0 (out.data() + p, 1920);
        if (f <= 0.0) continue;
        lo = std::min (lo, f);
        hi = std::max (hi, f);
    }
    return hi > 0.0 ? test::centsBetween (hi, lo) : 0.0;
}

struct NewKind
{
    const char* type;
    const char* param;  // the choice param
    int choice;
    const char* id;     // choice json id
    int latency;        // expected getLatencySamples() at 48 kHz
};

const NewKind kNew[] = {
    { "voicechar", "kind", 4, "helmet", 0 },        { "voicechar", "kind", 5, "underwater", 72 },
    { "voicechar", "kind", 6, "wall", 0 },          { "voicechar", "kind", 7, "vinyl", 72 },
    { "voicechar", "kind", 8, "gasmask", 0 },       { "voicechar", "kind", 9, "stadium", 0 },
    { "distortion", "shape", 3, "rectifier", 0 },   { "distortion", "shape", 4, "hardclip", 0 },
    { "distortion", "shape", 5, "wavefolder", 0 },
    { "saturator", "mode", 2, "transistor", 0 },    { "saturator", "mode", 3, "tapewear", 96 },
};

std::vector<float> kindParams (const NewKind& k, std::initializer_list<std::pair<const char*, double>> more = {})
{
    auto p = paramsOf (k.type, more);
    p[size_t (findEffectInfo (k.type)->paramIndex (k.param))] = float (k.choice);
    return p;
}

juce::String nameOf (const NewKind& k) { return juce::String (k.type) + "/" + k.id; }

struct Golden
{
    const char* type;
    const char* param; // the choice param
    int choice;
    float a, b;        // voicechar: intensity, mix; distortion/saturator: driveDb, mix
    uint64_t hash;     // recorded with the wave-6 code (f76dba0), before any new type existed
};

const Golden kGolden[] = {
    { "voicechar", "kind", 0, 0.5f, 1.0f, 0x45b8af74df7c7fe1ull },  { "voicechar", "kind", 0, 0.9f, 0.6f, 0xec4e0e61971bd26aull },
    { "voicechar", "kind", 1, 0.5f, 1.0f, 0x337d0bddd169b44dull },  { "voicechar", "kind", 1, 0.9f, 0.6f, 0x35427f8968bde230ull },
    { "voicechar", "kind", 2, 0.5f, 1.0f, 0xd60ec57a246e04e5ull },  { "voicechar", "kind", 2, 0.9f, 0.6f, 0xd901fdbe4e5cffceull },
    { "voicechar", "kind", 3, 0.5f, 1.0f, 0x1dfe794d367da212ull },  { "voicechar", "kind", 3, 0.9f, 0.6f, 0x7e43ed6cca2350e0ull },
    { "distortion", "shape", 0, 12.0f, 0.5f, 0xac3a0c2ca8d0c72eull }, { "distortion", "shape", 0, 30.0f, 1.0f, 0x42941d5f2e78d861ull },
    { "distortion", "shape", 1, 12.0f, 0.5f, 0x8ea9593ca5b66157ull }, { "distortion", "shape", 1, 30.0f, 1.0f, 0xd78cff8b9facc643ull },
    { "distortion", "shape", 2, 12.0f, 0.5f, 0x197a048df6a92418ull }, { "distortion", "shape", 2, 30.0f, 1.0f, 0xfd3e9884d9d7946cull },
    { "saturator", "mode", 0, 6.0f, 1.0f, 0x0d889674fca54361ull },    { "saturator", "mode", 0, 18.0f, 0.4f, 0x1c2ec62a6da448deull },
    { "saturator", "mode", 1, 6.0f, 1.0f, 0xbf82095b5c1a7be8ull },    { "saturator", "mode", 1, 18.0f, 0.4f, 0x127861c52412a520ull },
};

std::vector<float> goldenParams (const Golden& g)
{
    const bool vc = juce::String (g.type) == "voicechar";
    return paramsOf (g.type, { { g.param, g.choice }, { vc ? "intensity" : "driveDb", g.a }, { "mix", g.b } });
}

class EffectsToneTests : public juce::UnitTest
{
public:
    EffectsToneTests() : juce::UnitTest ("Effects tone (wave 7)", "EffectsTone") {}

    void runTest() override
    {
        testGolden();
        testRegistry();
        for (auto& k : kNew) testKind (k);
        testJobs();
        testPresets();
        measureCpu();
    }

private:
    void testGolden()
    {
        beginTest ("existing kinds: output bit-identical to the wave-6 code (FNV-1a 64 of the float bits)");
        const auto voice = test::synthVoice (3.0, 5);
        for (auto& g : kGolden)
        {
            const auto h = hashOf (test::renderEffect (g.type, voice, goldenParams (g)));
            const auto label = juce::String (g.type) + " " + g.param + "=" + juce::String (g.choice) + " (" + juce::String (g.a) + ", " + juce::String (g.b) + ")";
            expect (h == g.hash, label + " changed: 0x" + juce::String::toHexString ((juce::int64) h));
        }
    }

    void testRegistry()
    {
        beginTest ("registry: new choices appended after the existing ones");
        for (auto& k : kNew)
        {
            auto* info = findEffectInfo (k.type);
            const int p = info->paramIndex (k.param);
            expect (p >= 0 && int (info->params[size_t (p)].choices.size()) > k.choice, nameOf (k));
            if (p < 0 || int (info->params[size_t (p)].choices.size()) <= k.choice) continue;
            const auto& spec = info->params[size_t (p)];
            expect (juce::String (spec.choices[size_t (k.choice)].first) == k.id, nameOf (k));
            expectEquals (int (spec.max), int (spec.choices.size()) - 1, nameOf (k) + " max");
        }
        auto first = [] (const char* type) { return juce::String (findEffectInfo (type)->params[0].choices[0].first); };
        expect (first ("voicechar") == "telephone" && first ("distortion") == "overdrive" && first ("saturator") == "tape");
    }

    void testKind (const NewKind& k)
    {
        const auto who = nameOf (k);
        auto* info = findEffectInfo (k.type);
        beginTest (who + ": finite, limiter, no allocation, 50 ms sweeps, kind switch, latency, silence");

        // ---- finite + limiter peak: base, every numeric param at min / max ----
        const auto input = test::concat ({ test::synthVoice (4.0), test::silence (2.0), test::whiteNoise (2.0, 0.1f, 11) });
        auto check = [&] (const std::vector<float>& params, const juce::String& label)
        {
            auto out = input;
            auto fx = makeFx (k.type, params);
            run (*fx, out);
            const bool finite = test::allFinite (out);
            expect (finite, who + " non-finite: " + label);
            if (! finite) return;
            Limiter lim;
            lim.prepare (kSr);
            for (size_t pos = 0; pos < out.size(); pos += kBlock)
                lim.process (out.data() + pos, int (std::min<size_t> (kBlock, out.size() - pos)));
            expectLessOrEqual (test::peakDb (out), -0.99f, who + " limiter peak: " + label);
        };
        const auto base = kindParams (k);
        check (base, "defaults");
        for (size_t p = 0; p < info->params.size(); ++p)
        {
            if (info->params[p].isChoice()) continue;
            auto lo = base, hi = base;
            lo[p] = info->params[p].min;
            hi[p] = info->params[p].max;
            check (lo, juce::String (info->params[p].id) + "=min");
            check (hi, juce::String (info->params[p].id) + "=max");
        }

        // ---- no allocation on the audio thread ----
        {
            auto fx = makeFx (k.type, base);
            auto buf = test::synthVoice (0.5);
            const int choiceIndex = info->paramIndex (k.param);
            test::AllocationCounter allocs;
            run (*fx, buf);
            for (size_t p = 0; p < info->params.size(); ++p)
                if (! info->params[p].isChoice()) fx->setParam (int (p), info->params[p].max);
            run (*fx, buf);
            fx->setParam (choiceIndex, 0.0f);
            run (*fx, buf);
            fx->setParam (choiceIndex, float (k.choice));
            run (*fx, buf);
            fx->reset();
            run (*fx, buf);
            const auto count = allocs.count(); // before building the message string
            expectEquals (int (count), 0, who + " allocated on the audio thread");
        }

        // ---- 50 ms sweeps of every numeric param (setParam once per block) ----
        const auto voice = test::synthVoice (2.2, 3);
        const int opStart = int (kSr * 1.0), startBlock = opStart / kBlock;
        for (size_t p = 0; p < info->params.size(); ++p)
        {
            const auto& spec = info->params[p];
            if (spec.isChoice()) continue;
            auto v = base;
            v[p] = spec.min;
            auto fx = makeFx (k.type, v);
            auto out = voice;
            const int sweepBlocks = int (0.050 * kSr) / kBlock;
            for (int pos = 0, b = 0; pos < int (out.size()); pos += kBlock, ++b)
            {
                if (b >= startBlock && b <= startBlock + sweepBlocks)
                    fx->setParam (int (p), spec.clamp (spec.min + float (b - startBlock) / float (sweepBlocks) * (spec.max - spec.min)));
                fx->process (out.data() + pos, std::min (kBlock, int (out.size()) - pos));
            }
            const double r = test::clickRatio (out, opStart, opStart + int (0.050 * kSr));
            expectLessOrEqual (r, 2.0, who + " click on sweep of " + spec.id + ": ratio " + juce::String (r, 2));
        }

        // ---- kind switch both ways (default choice <-> new kind) ----
        const int choiceIndex = info->paramIndex (k.param);
        const int defChoice = int (info->params[size_t (choiceIndex)].def);
        for (auto [from, to] : { std::pair { defChoice, k.choice }, std::pair { k.choice, defChoice } })
        {
            auto v = base;
            v[size_t (choiceIndex)] = float (from);
            auto fx = makeFx (k.type, v);
            auto out = voice;
            for (int pos = 0, b = 0; pos < int (out.size()); pos += kBlock, ++b)
            {
                if (b == startBlock) fx->setParam (choiceIndex, float (to));
                fx->process (out.data() + pos, std::min (kBlock, int (out.size()) - pos));
            }
            expect (test::allFinite (out), who + " switch non-finite");
            const double r = test::clickRatio (out, opStart, opStart + int (0.030 * kSr));
            expectLessOrEqual (r, 2.0, who + " click on switch " + juce::String (from) + " -> " + juce::String (to) + ": ratio " + juce::String (r, 2));
        }

        // ---- latency: reported value, dry path delayed by it (mix 0), wet path centred on it ----
        {
            auto fx = makeFx (k.type, base);
            expectEquals (fx->getLatencySamples(), k.latency, who + " getLatencySamples");
            std::vector<float> imp (4800, 0.0f);
            imp[100] = 0.5f;
            auto dry = imp;
            auto fx0 = makeFx (k.type, kindParams (k, { { "mix", 0 } }));
            run (*fx0, dry);
            float worst = 0.0f;
            for (size_t i = 0; i < dry.size(); ++i)
                worst = std::max (worst, std::abs (dry[i] - (i >= size_t (k.latency) ? imp[i - size_t (k.latency)] : 0.0f)));
            expectLessOrEqual (worst, 1.0e-6f, who + ": mix 0 is not the input delayed by the latency");

            // wet path: mean best lag over 2 s of 50 ms windows (covers the wobble cycles); gentle settings
            const bool vc = juce::String (k.type) == "voicechar";
            auto gentle = vc ? kindParams (k, { { "intensity", 0.1 }, { "mix", 1 } })
                             : kindParams (k, { { "driveDb", 0 }, { "toneHz", 16000 }, { "mix", 1 } });
            auto in = test::whiteNoise (3.5, 0.2f, 21); // unambiguous correlation peak
            auto wet = in;
            auto fx1 = makeFx (k.type, gentle);
            run (*fx1, wet);
            double lagSum = 0.0;
            int windows = 0;
            for (int s = int (kSr); s + 2400 + 400 < int (kSr * 3.0); s += 2400, ++windows)
            {
                std::vector<float> ref (in.begin() + s, in.begin() + s + 2400), sig (wet.begin() + s, wet.begin() + s + 2400 + 400);
                lagSum += test::findLag (ref, sig, 400);
            }
            const double meanLag = lagSum / windows;
            logMessage ("  " + who + ": latency " + juce::String (fx->getLatencySamples()) + ", mean wet lag " + juce::String (meanLag, 1));
            expectWithinAbsoluteError (meanLag, double (k.latency), 6.0, who + ": wet path not centred on the latency");
        }

        // ---- silence in -> silence out within 10 s (defaults and everything at max) ----
        for (bool atMax : { false, true })
        {
            auto v = base;
            if (atMax)
                for (size_t p = 0; p < info->params.size(); ++p)
                    if (! info->params[p].isChoice()) v[p] = info->params[p].max;
            auto buf = test::concat ({ test::synthVoice (2.0), test::silence (10.0) });
            auto fx = makeFx (k.type, v);
            run (*fx, buf);
            const int n = int (kSr);
            expectLessThan (test::peakDb (buf.data() + buf.size() - size_t (n), n), -80.0f, who + (atMax ? " (max)" : "") + " does not return to silence");
        }
    }

    void testJobs()
    {
        beginTest ("voicechar new kinds: intensity 0 is the original (delayed by the latency), level kept within 2 dB");
        const auto voice = test::synthVoice (3.0);
        for (auto& k : kNew)
        {
            if (juce::String (k.type) != "voicechar") continue;
            auto out = test::renderEffect ("voicechar", voice, kindParams (k, { { "intensity", 0 } }));
            float maxDiff = 0.0f;
            for (size_t i = size_t (k.latency); i < voice.size(); ++i) maxDiff = std::max (maxDiff, std::abs (out[i] - voice[i - size_t (k.latency)]));
            expectLessOrEqual (maxDiff, 1.0e-6f, nameOf (k) + " intensity 0");

            auto full = test::renderEffect ("voicechar", voice, kindParams (k, { { "intensity", 1 }, { "mix", 1 } }));
            const float diff = test::rmsDb (full.data() + int (kSr * 0.5), int (kSr * 2)) - test::rmsDb (voice.data() + int (kSr * 0.5), int (kSr * 2));
            logMessage ("  voicechar " + juce::String (k.id) + ": level change " + juce::String (diff, 2) + " dB at intensity 1");
            expectWithinAbsoluteError (diff, 0.0f, 2.0f, nameOf (k) + " changes the level");
        }

        beginTest ("underwater / wall cut the highs hard; underwater and vinyl wobble the pitch");
        {
            const auto lowHigh = sum (test::sine (300.0, 2.0, 0.2f), test::sine (3000.0, 2.0, 0.2f));
            const int start = int (kSr), len = int (kSr * 0.5);
            for (int kind : { 5, 6 })
            {
                auto out = test::renderEffect ("voicechar", lowHigh, paramsOf ("voicechar", { { "kind", kind }, { "intensity", 1 }, { "mix", 1 } }));
                const double cut = toneDb (out, start, len, 300.0) - toneDb (out, start, len, 3000.0);
                logMessage ("  voicechar kind " + juce::String (kind) + ": 3 kHz " + juce::String (cut, 1) + " dB below 300 Hz");
                expectGreaterThan (cut, kind == 6 ? 35.0 : 25.0);
            }
            for (int kind : { 5, 7 })
            {
                const double hz = kind == 5 ? 300.0 : 440.0;
                auto out = test::renderEffect ("voicechar", test::sine (hz, 4.0, 0.3f), paramsOf ("voicechar", { { "kind", kind }, { "intensity", 1 }, { "mix", 1 } }));
                const double c = pitchSwingCents (out);
                logMessage ("  voicechar kind " + juce::String (kind) + ": pitch swing " + juce::String (c, 1) + " cents p-p");
                expectGreaterThan (c, 5.0);
            }
        }

        beginTest ("stadium: reflections come back 160 ms after the sound; helmet adds short reflections");
        {
            auto burst = test::whiteNoise (0.02, 0.3f, 4);
            burst.resize (size_t (kSr * 1.0), 0.0f);
            auto out = test::renderEffect ("voicechar", burst, paramsOf ("voicechar", { { "kind", 9 }, { "intensity", 1 }, { "mix", 1 } }));
            const float gap = test::rmsDb (out.data() + int (kSr * 0.07), int (kSr * 0.07));
            const float echo = test::rmsDb (out.data() + int (kSr * 0.16), int (kSr * 0.03));
            logMessage ("  stadium: 70-140 ms " + juce::String (gap, 1) + " dB, 160-190 ms " + juce::String (echo, 1) + " dB");
            expectGreaterThan (echo - gap, 15.0f);

            std::vector<float> click (size_t (kSr * 0.2), 0.0f);
            click[0] = 0.5f;
            auto helm = test::renderEffect ("voicechar", click, paramsOf ("voicechar", { { "kind", 4 }, { "intensity", 1 }, { "mix", 1 } }));
            auto tel = test::renderEffect ("voicechar", click, paramsOf ("voicechar", { { "kind", 0 }, { "intensity", 1 }, { "mix", 1 } }));
            const float tail = test::rmsDb (helm.data() + 240, 480), telTail = test::rmsDb (tel.data() + 240, 480);
            logMessage ("  helmet: 5-15 ms after a click " + juce::String (tail, 1) + " dB (telephone " + juce::String (telTail, 1) + " dB)");
            expectGreaterThan (tail - telTail, 3.0f);
        }

        beginTest ("vinyl: crackle while talking, nothing while silent");
        {
            auto in = test::concat ({ test::sine (300.0, 2.0, 0.3f), test::silence (2.0) });
            auto out = test::renderEffect ("voicechar", in, paramsOf ("voicechar", { { "kind", 7 }, { "intensity", 1 }, { "mix", 1 } }));
            dsp::Biquad hp;
            hp.setHighpass (kSr, 7000.0f, 0.7071f);
            for (auto& v : out) v = hp.process (v);
            const float talking = test::peakDb (out.data() + int (kSr * 0.5), int (kSr)), quiet = test::peakDb (out.data() + int (kSr * 3.0), int (kSr));
            logMessage ("  vinyl: peak above 7 kHz " + juce::String (talking, 1) + " dB while talking, " + juce::String (quiet, 1) + " dB in silence");
            expectGreaterThan (talking, -40.0f);
            expectLessThan (quiet, -80.0f);
        }

        beginTest ("distortion: rectifier doubles the pitch, hardclip drops the lows, wavefolder folds past the fundamental");
        {
            const int start = int (kSr * 0.5), len = int (kSr * 0.5);
            auto rect = test::renderEffect ("distortion", test::sine (300.0, 1.0, 0.25f), paramsOf ("distortion", { { "shape", 3 }, { "driveDb", 12 }, { "toneHz", 12000 }, { "mix", 1 } }));
            const double f1 = toneDb (rect, start, len, 300.0), f2 = toneDb (rect, start, len, 600.0);
            logMessage ("  rectifier: 600 Hz " + juce::String (f2 - f1, 1) + " dB re 300 Hz");
            expectGreaterThan (f2, f1);

            const auto lowMid = sum (test::sine (100.0, 1.0, 0.2f), test::sine (1000.0, 1.0, 0.2f));
            auto rel = [&] (int shape)
            {
                auto o = test::renderEffect ("distortion", lowMid, paramsOf ("distortion", { { "shape", shape }, { "driveDb", 12 }, { "toneHz", 12000 }, { "mix", 1 } }));
                return toneDb (o, start, len, 100.0) - toneDb (o, start, len, 1000.0);
            };
            const double hard = rel (4), dist = rel (1);
            logMessage ("  100 Hz re 1 kHz: hardclip " + juce::String (hard, 1) + " dB, distortion " + juce::String (dist, 1) + " dB");
            expectLessThan (hard, dist - 6.0);

            auto fold = test::renderEffect ("distortion", test::sine (200.0, 1.0, 0.5f), paramsOf ("distortion", { { "shape", 5 }, { "driveDb", 24 }, { "toneHz", 12000 }, { "mix", 1 } }));
            const double h1 = toneDb (fold, start, len, 200.0), h3 = toneDb (fold, start, len, 600.0), h5 = toneDb (fold, start, len, 1000.0);
            logMessage ("  wavefolder: h1 " + juce::String (h1, 1) + ", h3 " + juce::String (h3, 1) + ", h5 " + juce::String (h5, 1) + " dB");
            expectGreaterThan (std::max (h3, h5), h1);
        }

        beginTest ("saturator: transistor adds even harmonics; tapewear wobbles and dulls; level kept");
        {
            const int start = int (kSr * 0.5), len = int (kSr * 0.5);
            auto tr = test::renderEffect ("saturator", test::sine (1000.0, 1.0, 0.3f), paramsOf ("saturator", { { "mode", 2 }, { "driveDb", 12 }, { "toneHz", 16000 } }));
            const double even = toneDb (tr, start, len, 2000.0) - toneDb (tr, start, len, 1000.0);
            logMessage ("  transistor: 2 kHz " + juce::String (even, 1) + " dB re 1 kHz");
            expectGreaterThan (even, -35.0);

            auto wear = test::renderEffect ("saturator", test::sine (440.0, 4.0, 0.3f), paramsOf ("saturator", { { "mode", 3 }, { "driveDb", 0 }, { "toneHz", 16000 } }));
            const double c = pitchSwingCents (wear);
            logMessage ("  tapewear: pitch swing " + juce::String (c, 1) + " cents p-p");
            expectGreaterThan (c, 6.0);
            const auto hiLo = sum (test::sine (500.0, 1.0, 0.1f), test::sine (8000.0, 1.0, 0.1f));
            auto dull = test::renderEffect ("saturator", hiLo, paramsOf ("saturator", { { "mode", 3 }, { "driveDb", 0 }, { "toneHz", 16000 } }));
            auto clean = test::renderEffect ("saturator", hiLo, paramsOf ("saturator", { { "mode", 0 }, { "driveDb", 0 }, { "toneHz", 16000 } }));
            const double loss = (toneDb (clean, start, len, 8000.0) - toneDb (clean, start, len, 500.0)) - (toneDb (dull, start, len, 8000.0) - toneDb (dull, start, len, 500.0));
            logMessage ("  tapewear: 8 kHz down " + juce::String (loss, 1) + " dB more than tape");
            expectGreaterThan (loss, 6.0);

            const auto speech = test::synthVoice (3.0);
            for (auto [type, param, choice] : { std::tuple { "distortion", "shape", 3 }, std::tuple { "distortion", "shape", 4 }, std::tuple { "distortion", "shape", 5 },
                                                std::tuple { "saturator", "mode", 2 }, std::tuple { "saturator", "mode", 3 } })
            {
                auto out = test::renderEffect (type, speech, paramsOf (type, { { param, choice }, { "driveDb", 12 }, { "mix", 1 } }));
                const float diff = test::rmsDb (out.data() + int (kSr * 0.5), int (kSr * 2)) - test::rmsDb (speech.data() + int (kSr * 0.5), int (kSr * 2));
                expectWithinAbsoluteError (diff, 0.0f, 2.0f, juce::String (type) + " " + juce::String (choice) + " loudness");
            }
        }
    }

    void testPresets()
    {
        beginTest ("the 11 new built-ins load with no corrections and use their new kind");
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("KoeLoomToneTests", "", false);
        PresetLibrary lib (dir);
        lib.reload();
        expectEquals (lib.builtinErrors(), 0);
        const std::pair<const char*, int> ids[] = {
            { "device-helmet", 0 }, { "device-underwater", 1 }, { "device-next-room", 2 }, { "device-vinyl", 3 },
            { "device-gasmask", 4 }, { "device-stadium", 5 }, { "device-octave-fuzz", 6 }, { "device-hardclip", 7 },
            { "device-wavefolder", 8 }, { "device-transistor", 9 }, { "device-worn-tape", 10 },
        };
        for (auto [id, kindIndex] : ids)
        {
            const auto& k = kNew[kindIndex];
            const Preset* found = nullptr;
            for (auto& p : lib.all())
                if (p.id == id) found = &p;
            expect (found != nullptr, juce::String (id) + " missing (or needed corrections)");
            if (found == nullptr) continue;
            expect (found->name.length() <= 32 && kTrimDb.clamp (found->outputTrimDb) == found->outputTrimDb, id); // trim set by lead's calibration
            bool uses = false;
            for (auto& s : found->chain)
                if (s.type == k.type) uses = uses || int (s.params[size_t (findEffectInfo (k.type)->paramIndex (k.param))]) == k.choice;
            expect (uses, juce::String (id) + " does not use " + nameOf (k));
            for (auto& p : lib.all())
                expect (p.id == found->id || p.name != found->name, juce::String (id) + ": name already used by " + juce::String (p.id));
        }
        dir.deleteRecursively();
    }

    void measureCpu()
    {
        beginTest ("CPU per 480-sample block for the new kinds (logged; light = under 100 us = 1 % of 10 ms)");
        const auto voice = test::synthVoice (10.0);
        for (auto& k : kNew)
        {
            auto fx = makeFx (k.type, kindParams (k));
            auto buf = voice;
            const auto t0 = juce::Time::getHighResolutionTicks();
            run (*fx, buf);
            const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const double perBlockUs = sec * 1.0e6 / (double (buf.size()) / kBlock);
            const auto r0 = juce::Time::getHighResolutionTicks();
            fx->reset();
            const double resetUs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - r0) * 1.0e6;
            logMessage ("  " + nameOf (k).paddedRight (' ', 22) + juce::String (perBlockUs, 1) + " us/block = " + juce::String (perBlockUs / 100.0, 2)
                        + " % of 10 ms, reset() " + juce::String (resetUs, 0) + " us");
            expectLessThan (perBlockUs, 1000.0, nameOf (k) + " is far too slow");
        }
    }
};

static EffectsToneTests effectsToneTests;
} // namespace
} // namespace koe

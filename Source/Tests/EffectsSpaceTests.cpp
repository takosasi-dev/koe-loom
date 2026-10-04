// wave7/space: the reverb types added after ambience (cathedral, gated, reverse, shimmer, cave, bathroom), the
// echo types added after reverse (slapback, analog, multitap) and their built-in presets (INTERFACES.md §9).
// Signals are synthetic, so measured values are "合成音声で代用".

#include "BinaryData.h" // KoeLoomPresetData
#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Dsp/Limiter.h"
#include "Effects/EffectRegistry.h"
#include "Model/Preset.h"
#include "Tests/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace koe
{
namespace
{
using test::kSr;
constexpr int kBlock = 480;

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

/** Renders in 480-sample blocks; `atBlock` may change parameters before a block is processed. */
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

uint64_t fnv1a (const std::vector<float>& x)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (float v : x)
    {
        uint32_t bits;
        std::memcpy (&bits, &v, 4);
        for (int k = 0; k < 4; ++k)
        {
            h ^= (bits >> (8 * k)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

juce::String hex (uint64_t h) { return "0x" + juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16); }

// ---- golden hashes of the types that existed before wave 7 (recorded with the f76dba0 code) ----
struct Golden
{
    const char* label;
    uint64_t hash;
};

class EffectsSpaceGoldenTests : public juce::UnitTest
{
public:
    EffectsSpaceGoldenTests() : juce::UnitTest ("Effects space: existing types unchanged", "EffectsSpace") {}

    void runTest() override
    {
        beginTest ("reverb room/hall/plate/spring/ambience and echo digital/tape/reverse: output bit-identical to v0.2.0");
        const auto voice = test::concat ({ test::synthVoice (2.0, 5), test::silence (1.0) });
        int idx = 0;
        auto check = [&] (const juce::String& label, const std::vector<float>& out)
        {
            const uint64_t h = fnv1a (out);
            const uint64_t want = kGolden[idx].hash;
            logMessage ("  " + label + ": " + hex (h));
            expect (label == kGolden[idx].label, "golden table order: " + label);
            expect (h == want, label + " output changed: " + hex (h) + " (want " + hex (want) + ")");
            ++idx;
        };

        for (int t = 0; t < 5; ++t)
        {
            auto fx = makeFx ("reverb", paramsOf ("reverb", { { "type", t }, { "room", 0.7 }, { "damp", 0.3 }, { "preDelayMs", 20 }, { "mix", 0.6 } }));
            check ("reverb/" + juce::String (t), renderWith (*fx, voice, [] (int) {}));
        }
        {
            auto fx = makeFx ("reverb", paramsOf ("reverb", { { "mix", 0.5 } }));
            check ("reverb/switch", renderWith (*fx, voice, [&] (int b) {
                       if (b == 100) fx->setParam (0, 2.0f);  // room -> plate
                       if (b == 150) fx->setParam (1, 0.9f);
                       if (b == 160) fx->setParam (0, 3.0f);  // -> spring
                       if (b == 170) fx->setParam (3, 80.0f);
                   }));
        }
        for (int m = 0; m < 3; ++m)
        {
            auto fx = makeFx ("echo", paramsOf ("echo", { { "mode", m }, { "timeMs", 250 }, { "feedback", 0.6 }, { "toneHz", 5000 }, { "mix", 0.5 } }));
            check ("echo/" + juce::String (m), renderWith (*fx, voice, [] (int) {}));
        }
        {
            auto fx = makeFx ("echo", paramsOf ("echo", { { "mix", 0.5 } }));
            check ("echo/switch", renderWith (*fx, voice, [&] (int b) {
                       if (b == 60) fx->setParam (0, 1.0f);   // digital -> tape
                       if (b == 90) fx->setParam (1, 120.0f);
                       if (b == 120) fx->setParam (0, 2.0f);  // -> reverse
                       if (b == 140) fx->setParam (3, 3000.0f);
                   }));
        }
    }

    static constexpr Golden kGolden[] = {
        { "reverb/0", 0x237568d1ff346324ull }, { "reverb/1", 0x8dc6a209c328d564ull }, { "reverb/2", 0x078378e9016ac53cull },
        { "reverb/3", 0xd90ad2abfc40850full }, { "reverb/4", 0xad65f455cdb6b3afull }, { "reverb/switch", 0xe394e76c2cbd61e9ull },
        { "echo/0", 0x2c8099a116883887ull }, { "echo/1", 0x71e5b8ca95b3ecc7ull }, { "echo/2", 0x170cf0153b7a0412ull },
        { "echo/switch", 0xd0db3c9f22f588d9ull },
    };
};

// ---- the new types ----
struct NewType
{
    const char* fx;    // "reverb" / "echo"
    const char* param; // the choice parameter id
    int index;
    const char* name;
};

constexpr NewType kNewTypes[] = {
    { "reverb", "type", 5, "cathedral" }, { "reverb", "type", 6, "gated" },   { "reverb", "type", 7, "reverse" },
    { "reverb", "type", 8, "shimmer" },   { "reverb", "type", 9, "cave" },    { "reverb", "type", 10, "bathroom" },
    { "echo", "mode", 3, "slapback" },    { "echo", "mode", 4, "analog" },    { "echo", "mode", 5, "multitap" },
};

std::vector<float> paramsFor (const NewType& n, std::initializer_list<std::pair<const char*, double>> overrides = {})
{
    auto p = paramsOf (n.fx, overrides);
    p[size_t (findEffectInfo (n.fx)->paramIndex (n.param))] = float (n.index);
    return p;
}

juce::String labelOf (const NewType& n) { return juce::String (n.fx) + "/" + n.name; }

double toneDb (const std::vector<float>& x, int start, int len, double hz)
{
    const double coeff = 2.0 * std::cos (2.0 * 3.14159265358979323846 * hz / kSr);
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

float rmsDbOf (const std::vector<float>& x, double fromSec, double lenSec)
{
    return test::rmsDb (x.data() + int (fromSec * kSr), int (lenSec * kSr));
}

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

std::vector<float> impulse (double seconds)
{
    std::vector<float> v (size_t (seconds * kSr), 0.0f);
    v[0] = 1.0f;
    return v;
}

int peakIndex (const std::vector<float>& v, int a, int b)
{
    int best = a;
    for (int i = a; i < b; ++i)
        if (std::abs (v[size_t (i)]) > std::abs (v[size_t (best)])) best = i;
    return best;
}

class EffectsSpaceTests : public juce::UnitTest
{
public:
    EffectsSpaceTests() : juce::UnitTest ("Effects space: new reverb / echo types", "EffectsSpace") {}

    void runTest() override
    {
        testFiniteAndLimiter();
        testNoAllocation();
        testSweeps();
        testTypeSwitch();
        testDecayToSilence();
        testLatencyAndDry();
        testReverbCharacter();
        testEchoCharacter();
        testLevels();
        testPresets();
        measureCpu();
    }

private:
    void testFiniteAndLimiter()
    {
        beginTest ("new types: finite output and limiter peak <= -1 dBFS at defaults and every knob at min / max");
        const auto input = test::concat ({ test::synthVoice (4.0, 9), test::silence (1.0), test::whiteNoise (1.0, 0.1f, 11) });
        for (auto& n : kNewTypes)
        {
            auto* info = findEffectInfo (n.fx);
            auto check = [&] (const std::vector<float>& params, const juce::String& what)
            {
                auto fx = makeFx (n.fx, params);
                auto out = renderWith (*fx, input, [] (int) {});
                const bool finite = test::allFinite (out);
                expect (finite, labelOf (n) + " non-finite: " + what);
                if (! finite) return;
                Limiter lim;
                lim.prepare (kSr);
                for (size_t pos = 0; pos < out.size(); pos += kBlock)
                    lim.process (out.data() + pos, int (std::min<size_t> (kBlock, out.size() - pos)));
                expectLessOrEqual (test::peakDb (out), -0.99f, labelOf (n) + " limiter peak: " + what);
            };
            check (paramsFor (n), "defaults");
            for (auto& s : info->params)
            {
                if (s.isChoice()) continue;
                check (paramsFor (n, { { s.id, s.min } }), juce::String (s.id) + "=min");
                check (paramsFor (n, { { s.id, s.max } }), juce::String (s.id) + "=max");
            }
        }
    }

    void testNoAllocation()
    {
        beginTest ("new types: no allocation in process / setParam / reset (AllocHook)");
        for (auto& n : kNewTypes)
        {
            auto* info = findEffectInfo (n.fx);
            auto fx = makeFx (n.fx, paramsFor (n));
            auto buf = test::synthVoice (0.5);
            auto run = [&]
            {
                for (size_t pos = 0; pos < buf.size(); pos += kBlock)
                    fx->process (buf.data() + pos, int (std::min<size_t> (kBlock, buf.size() - pos)));
            };
            test::AllocationCounter allocs;
            run();
            for (size_t p = 0; p < info->params.size(); ++p)
                if (! info->params[p].isChoice()) fx->setParam (int (p), info->params[p].max);
            run();
            fx->setParam (0, float (n.index == 5 ? 10 : 5)); // switch to another new type on the audio thread
            run();
            fx->reset();
            run();
            const int count = int (allocs.count()); // before building the message string
            expectEquals (count, 0, labelOf (n) + " allocated on the audio thread");
        }
    }

    void testSweeps()
    {
        beginTest ("new types: every knob swept min -> max over 50 ms without a click (clickRatio <= 2)");
        const auto voice = test::synthVoice (2.2, 3);
        for (auto& n : kNewTypes)
        {
            auto* info = findEffectInfo (n.fx);
            for (size_t p = 0; p < info->params.size(); ++p)
            {
                const auto& spec = info->params[p];
                if (spec.isChoice()) continue;
                auto fx = makeFx (n.fx, paramsFor (n, { { spec.id, spec.min } }));
                const int opStart = int (kSr), startBlock = opStart / kBlock, sweepBlocks = int (0.050 * kSr) / kBlock;
                auto out = renderWith (*fx, voice, [&] (int b) {
                    if (b >= startBlock && b <= startBlock + sweepBlocks)
                        fx->setParam (int (p), spec.clamp (spec.min + float (b - startBlock) / float (sweepBlocks) * (spec.max - spec.min)));
                });
                const double r = test::clickRatio (out, opStart, opStart + int (0.050 * kSr));
                expectLessOrEqual (r, 2.0, labelOf (n) + " click on sweep of " + spec.id + ": ratio " + juce::String (r, 2));
            }
        }
    }

    void testTypeSwitch()
    {
        beginTest ("type changes to and from every new type do not click");
        const auto voice = test::synthVoice (2.2, 3);
        for (auto& n : kNewTypes)
        {
            for (int dir = 0; dir < 2; ++dir)
            {
                const float from = dir == 0 ? 0.0f : float (n.index), to = dir == 0 ? float (n.index) : 0.0f;
                auto params = paramsOf (n.fx, { { "mix", 0.5 } });
                params[0] = from;
                auto fx = makeFx (n.fx, params);
                const int opStart = int (kSr);
                auto out = renderWith (*fx, voice, [&] (int b) { if (b == opStart / kBlock) fx->setParam (0, to); });
                const double r = test::clickRatio (out, opStart, opStart + int (0.050 * kSr));
                expectLessOrEqual (r, 2.0, labelOf (n) + (dir == 0 ? " switch in" : " switch out") + ": ratio " + juce::String (r, 2));
            }
        }
    }

    void testDecayToSilence()
    {
        beginTest ("new types: silence after the voice comes back to silence within 10 s, also with the knobs at max");
        const auto input = test::concat ({ test::synthVoice (2.0, 4), test::silence (10.0) });
        for (auto& n : kNewTypes)
        {
            const bool echo = juce::String (n.fx) == "echo";
            // reverb: largest room, least damping (= longest tail); echo: feedback at its max
            auto longest = echo ? paramsFor (n, { { "feedback", 0.9 }, { "toneHz", 12000 }, { "mix", 1 } })
                                : paramsFor (n, { { "room", 1 }, { "damp", 0 }, { "preDelayMs", 200 }, { "mix", 1 } });
            auto fx = makeFx (n.fx, longest);
            auto out = renderWith (*fx, input, [] (int) {});
            expect (test::allFinite (out), labelOf (n));
            const float first = rmsDbOf (out, 2.0, 1.0), ninth = rmsDbOf (out, 10.0, 1.0), last = rmsDbOf (out, 11.0, 1.0);
            logMessage ("  " + labelOf (n).paddedRight (' ', 16) + "max knobs: 0-1 s after " + juce::String (first, 1) + " dBFS, 8-9 s "
                        + juce::String (ninth, 1) + ", 9-10 s " + juce::String (last, 1));
            expectLessOrEqual (last, ninth + 0.5f, labelOf (n) + " grows instead of decaying (oscillation)");
            if (echo)
            {
                // feedback 0.9 at 300 ms repeats is still -30 dB after 10 s (as for digital); it must keep falling
                expectLessOrEqual (last, first - 25.0f, labelOf (n) + " decays too slowly at max feedback");
                auto fx2 = makeFx (n.fx, paramsFor (n, { { "mix", 1 } }));
                auto out2 = renderWith (*fx2, input, [] (int) {});
                expectLessOrEqual (rmsDbOf (out2, 11.5, 0.5), -80.0f, labelOf (n) + " not silent 10 s after (default feedback)");
            }
            else
            {
                expectLessOrEqual (rmsDbOf (out, 11.5, 0.5), -80.0f, labelOf (n) + " not silent 10 s after");
            }
        }
    }

    void testLatencyAndDry()
    {
        beginTest ("new types: getLatencySamples() is 0 and the dry path is not delayed");
        for (auto& n : kNewTypes)
        {
            auto fx = makeFx (n.fx, paramsFor (n, { { "mix", 0.5 } }));
            expectEquals (fx->getLatencySamples(), 0, labelOf (n));
            auto out = renderWith (*fx, impulse (0.5), [] (int) {});
            expectWithinAbsoluteError (out[0], std::sin (0.25f * dsp::kPi), 1.0e-5f, labelOf (n) + " dry at sample 0");
            auto voice = test::synthVoice (1.0);
            auto dry = makeFx (n.fx, paramsFor (n, { { "mix", 0 } }));
            expect (renderWith (*dry, voice, [] (int) {}) == voice, labelOf (n) + " mix 0 is not the input");
        }
    }

    void testReverbCharacter()
    {
        auto ir = [] (int type, std::initializer_list<std::pair<const char*, double>> o = {}, double sec = 8.0)
        {
            auto p = paramsOf ("reverb", o);
            p[0] = float (type);
            p[4] = 1.0f;
            auto fx = makeFx ("reverb", p);
            return renderWith (*fx, impulse (sec), [] (int) {});
        };
        // level at 260-400 ms relative to 60-120 ms: positive = the tail builds up (blooms) instead of only decaying
        auto earlyVsBody = [] (const std::vector<float>& x)
        {
            return test::rmsDb (x.data() + 12480, 6720) - test::rmsDb (x.data() + 2880, 2880);
        };

        beginTest ("cathedral: the tail blooms (builds up) and is longer than the hall");
        {
            const auto cat = ir (5), hall = ir (1);
            const double catRt = rt60Of (cat), hallRt = rt60Of (hall);
            const float catEarly = earlyVsBody (cat), hallEarly = earlyVsBody (hall);
            logMessage ("  cathedral RT60 " + juce::String (catRt, 2) + " s, 260-400 ms re 60-120 ms " + juce::String (catEarly, 1) + " dB; hall "
                        + juce::String (hallRt, 2) + " s, " + juce::String (hallEarly, 1) + " dB");
            expectGreaterThan (catRt, hallRt);
            expectGreaterThan (catEarly, 3.0f, "the cathedral tail must rise after the first reflections");
            expectLessThan (hallEarly, 0.0f);
        }

        beginTest ("gated: the reverb is cut shortly after the voice stops (room = gate time)");
        {
            auto burst = test::concat ({ test::synthVoice (1.0, 2, kSr, false), test::silence (2.0) });
            for (double room : { 0.0, 1.0 })
            {
                auto p = paramsOf ("reverb", { { "type", 6 }, { "room", room }, { "mix", 1 } });
                auto fx = makeFx ("reverb", p);
                auto out = renderWith (*fx, burst, [] (int) {});
                const float during = rmsDbOf (out, 0.5, 0.5);
                const float after = rmsDbOf (out, 1.0 + 0.08 + 0.42 * room + 0.12, 0.3);
                logMessage ("  gated room " + juce::String (room, 1) + ": during " + juce::String (during, 1) + " dBFS, after the gate "
                            + juce::String (after, 1) + " dBFS");
                expectLessThan (after, during - 40.0f);
            }
            auto ungated = renderWith (*makeFx ("reverb", paramsOf ("reverb", { { "type", 6 }, { "room", 1 }, { "mix", 1 } })), burst, [] (int) {});
            expectGreaterThan (rmsDbOf (ungated, 1.2, 0.2), rmsDbOf (ungated, 0.5, 0.5) - 20.0f, "the gate must hold through room 1");
        }

        beginTest ("reverse: the response swells towards its end and then stops");
        {
            const auto r = ir (7, { { "room", 0.5 } }, 2.0);
            const int len = int ((150.0 + 550.0 * 0.5) * kSr / 1000.0);
            double num = 0.0, den = 0.0;
            for (int i = 0; i < len; ++i) { num += double (i) * r[size_t (i)] * r[size_t (i)]; den += double (r[size_t (i)]) * r[size_t (i)]; }
            const double centroid = num / den / len;
            // 0.25 s after the last tap only the short all-pass smear could be left
            const float body = test::rmsDb (r.data() + len / 2, len / 2), after = test::rmsDb (r.data() + len + 12000, 9600);
            logMessage ("  reverse: energy centroid at " + juce::String (centroid * 100.0, 1) + " % of the length, 0.25 s after the end "
                        + juce::String (after - body, 1) + " dB re the swell");
            expectGreaterThan (centroid, 0.6);
            expectLessThan (after - body, -60.0f);
        }

        beginTest ("shimmer: the tail climbs an octave (880 Hz from a 440 Hz tone)");
        {
            auto tone = test::concat ({ test::sine (440.0, 0.5, 0.3f), test::silence (2.0) });
            auto tail = [&] (int type)
            {
                auto out = renderWith (*makeFx ("reverb", paramsOf ("reverb", { { "type", type }, { "mix", 1 } })), tone, [] (int) {});
                return std::make_pair (toneDb (out, int (kSr * 1.0), int (kSr * 1.0), 880.0), toneDb (out, int (kSr * 1.0), int (kSr * 1.0), 440.0));
            };
            const auto sh = tail (8), hall = tail (1);
            logMessage ("  tail 880 / 440 Hz: shimmer " + juce::String (sh.first, 1) + " / " + juce::String (sh.second, 1) + " dB, hall "
                        + juce::String (hall.first, 1) + " / " + juce::String (hall.second, 1) + " dB");
            expectGreaterThan (sh.first - hall.first, 15.0);
        }

        beginTest ("cave: distinct distant echoes (first at 79 ms for room 0.5)");
        {
            const auto r = ir (9, { { "room", 0.5 } }, 1.0);
            const int at = int (93.0 * 0.85 * kSr / 1000.0);
            float pk = 0.0f;
            for (int i = at - 20; i < at + 60; ++i) pk = std::max (pk, std::abs (r[size_t (i)]));
            const float before = test::rmsDb (r.data() + at - 960, 720);
            logMessage ("  cave: echo peak " + juce::String (dsp::gainToDb (pk), 1) + " dB, 15-20 ms before it " + juce::String (before, 1) + " dB RMS");
            expectGreaterThan (dsp::gainToDb (pk), before + 10.0f);
        }

        beginTest ("bathroom: shorter than the cathedral, and the room modes ring (118 Hz louder than 300 Hz, unlike room)");
        {
            auto ratio = [&] (int type)
            {
                auto lo = renderWith (*makeFx ("reverb", paramsOf ("reverb", { { "type", type }, { "room", 0.5 }, { "mix", 1 } })), test::sine (118.0, 1.5, 0.3f), [] (int) {});
                auto hi = renderWith (*makeFx ("reverb", paramsOf ("reverb", { { "type", type }, { "room", 0.5 }, { "mix", 1 } })), test::sine (300.0, 1.5, 0.3f), [] (int) {});
                return rmsDbOf (lo, 0.8, 0.6) - rmsDbOf (hi, 0.8, 0.6);
            };
            const float bath = ratio (10), roomT = ratio (0);
            const double rt = rt60Of (ir (10));
            logMessage ("  bathroom RT60 " + juce::String (rt, 2) + " s; 118 vs 300 Hz: bathroom " + juce::String (bath, 1) + " dB, room "
                        + juce::String (roomT, 1) + " dB");
            expectLessThan (rt, rt60Of (ir (5)) * 0.5);
            expectGreaterThan (bath - roomT, 3.0f);
        }
    }

    void testEchoCharacter()
    {
        beginTest ("slapback: one bounce at timeMs, a second at 2 x timeMs (feedback x 0.6), nothing after");
        {
            auto out = renderWith (*makeFx ("echo", paramsOf ("echo", { { "mode", 3 }, { "timeMs", 300 }, { "feedback", 0.5 }, { "toneHz", 12000 }, { "mix", 1 } })),
                                   impulse (2.0), [] (int) {});
            const int first = peakIndex (out, 1, 20000), second = peakIndex (out, 20000, 35000);
            expectWithinAbsoluteError (first, 14400, 3);
            expectWithinAbsoluteError (second, 28800, 3);
            expectWithinAbsoluteError (std::abs (out[size_t (second)] / out[size_t (first)]), 0.3f, 0.08f);
            float later = 0.0f;
            for (int i = 36000; i < int (out.size()); ++i) later = std::max (later, std::abs (out[size_t (i)]));
            expectLessThan (later, 0.01f * std::abs (out[size_t (first)]), "slapback must not repeat further");
        }

        beginTest ("analog: repeats are darker than digital ones and the time wobbles");
        {
            auto burst = test::whiteNoise (0.05, 0.5f, 3);
            burst.resize (size_t (kSr * 1.5), 0.0f);
            auto brightness = [&] (int mode)
            {
                auto out = renderWith (*makeFx ("echo", paramsOf ("echo", { { "mode", mode }, { "timeMs", 300 }, { "feedback", 0.8 }, { "toneHz", 12000 }, { "mix", 1 } })),
                                       burst, [] (int) {});
                const int a = 14400 - 2000, b = 14400 + 4400; // first repeat
                double e = 0, d = 0;
                for (int i = a + 1; i < b; ++i) { e += double (out[size_t (i)]) * out[size_t (i)]; d += double (out[size_t (i)] - out[size_t (i - 1)]) * (out[size_t (i)] - out[size_t (i - 1)]); }
                return d / std::max (1e-20, e);
            };
            const double digital = brightness (0), analog = brightness (4);
            logMessage ("  echo 1st repeat brightness: digital " + juce::String (digital, 4) + ", analog " + juce::String (analog, 4));
            expectLessThan (analog, digital * 0.5);

            auto tone = test::concat ({ test::sine (1000.0, 0.4, 0.3f), test::silence (1.2) });
            auto out = renderWith (*makeFx ("echo", paramsOf ("echo", { { "mode", 4 }, { "timeMs", 1000 }, { "feedback", 0 }, { "mix", 1 } })), tone, [] (int) {});
            double lo = 1e9, hi = 0.0;
            for (int p = int (kSr * 1.05); p + 1920 < int (kSr * 1.35); p += 480)
            {
                const double f0 = test::estimateF0 (out.data() + p, 1920, kSr, 500.0, 1500.0);
                if (f0 > 0.0) { lo = std::min (lo, f0); hi = std::max (hi, f0); }
            }
            logMessage ("  analog repeat pitch: " + juce::String (lo, 2) + " .. " + juce::String (hi, 2) + " Hz");
            expectGreaterThan (hi - lo, 0.3);
            expectLessThan (hi - lo, 40.0);
        }

        beginTest ("multitap: returns at 0.27 / 0.46 / 0.73 / 1.0 x timeMs");
        {
            auto out = renderWith (*makeFx ("echo", paramsOf ("echo", { { "mode", 5 }, { "timeMs", 1000 }, { "feedback", 0 }, { "toneHz", 12000 }, { "mix", 1 } })),
                                   impulse (1.5), [] (int) {});
            for (double f : { 0.27, 0.46, 0.73, 1.0 })
            {
                const int at = int (f * kSr);
                const int pk = peakIndex (out, at - 400, at + 400);
                expectWithinAbsoluteError (pk, at, 3, "multitap tap at " + juce::String (f));
                expectGreaterThan (std::abs (out[size_t (pk)]), 0.1f);
            }
        }
    }

    void testLevels()
    {
        beginTest ("new types: wet level at the defaults is near the dry level (logged; same measure as EffectsB)");
        const auto v = test::synthVoice (4.0);
        for (auto& n : kNewTypes)
        {
            auto wet = renderWith (*makeFx (n.fx, paramsFor (n, { { "mix", 1 } })), v, [] (int) {});
            const float d = rmsDbOf (wet, 1.0, 2.0) - rmsDbOf (v, 1.0, 2.0);
            logMessage ("  " + labelOf (n).paddedRight (' ', 16) + "wet level " + juce::String (d, 1) + " dB re dry");
            expect (d > -9.0f && d < 3.0f, labelOf (n) + " wet level " + juce::String (d, 1) + " dB");
        }
    }

    void testPresets()
    {
        beginTest ("the 9 new built-in presets load with no corrections and use their new type");
        const struct { const char* id; const char* fx; const char* choice; } presets[] = {
            { "space-stone-sanctuary", "reverb", "cathedral" }, { "space-gated-80s", "reverb", "gated" },
            { "space-reverse-swell", "reverb", "reverse" },     { "space-shimmer-heaven", "reverb", "shimmer" },
            { "space-limestone-cave", "reverb", "cave" },       { "space-tiled-bathroom", "reverb", "bathroom" },
            { "space-slapback-50s", "echo", "slapback" },       { "space-analog-echo", "echo", "analog" },
            { "space-multitap-echo", "echo", "multitap" },
        };
        for (auto& pr : presets)
        {
            const juce::String file = juce::String (pr.id) + ".json";
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
            expect (p->id == pr.id && p->name.length() <= kPresetNameMaxChars && kTrimDb.clamp (p->outputTrimDb) == p->outputTrimDb, file); // trim set by lead's calibration
            auto* info = findEffectInfo (pr.fx);
            bool uses = false;
            for (auto& s : p->chain)
                if (s.type == pr.fx)
                    uses = uses || juce::String (info->params[0].choices[size_t (s.params[0])].first) == pr.choice;
            expect (uses, file + " does not use " + pr.choice);
        }
    }

    void measureCpu()
    {
        beginTest ("CPU per 480-sample block for every reverb / echo type (logged; loose sanity bound)");
        const auto voice = test::synthVoice (10.0);
        for (const char* fxType : { "reverb", "echo" })
        {
            auto* info = findEffectInfo (fxType);
            for (int c = 0; c < int (info->params[0].choices.size()); ++c)
            {
                auto params = paramsOf (fxType);
                params[0] = float (c);
                auto fx = makeFx (fxType, params);
                double best = 1e9;
                for (int rep = 0; rep < 3; ++rep) // best of 3 (the machine may be busy)
                {
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    renderWith (*fx, voice, [] (int) {});
                    const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                    best = std::min (best, sec * 1.0e6 / (double (voice.size()) / kBlock));
                }
                const juce::String label = juce::String (fxType) + "/" + info->params[0].choices[size_t (c)].first;
                logMessage ("  " + label.paddedRight (' ', 18) + juce::String (best, 1) + " us/block = " + juce::String (best / 100.0, 2) + " % of 10 ms");
                expectLessThan (best, 1000.0, label + " is far too slow");
            }
        }
    }
};

static EffectsSpaceGoldenTests effectsSpaceGoldenTests;
static EffectsSpaceTests effectsSpaceTests;
} // namespace
} // namespace koe

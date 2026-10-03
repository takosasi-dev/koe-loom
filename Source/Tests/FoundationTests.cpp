#include "Dsp/Limiter.h"
#include "Effects/EffectRegistry.h"
#include "Tests/TestUtil.h"

#include <limits>
#include <set>
#include <string>

namespace koe
{
class RegistryMetadataTests : public juce::UnitTest
{
public:
    RegistryMetadataTests() : juce::UnitTest ("Effect registry metadata", "Foundation") {}

    void runTest() override
    {
        beginTest ("30 types, unique, koeloom_effects.md §2 counts");
        const auto& all = allEffectInfos();
        expectEquals (int (all.size()), 30);
        std::set<std::string> types;
        int phase2 = 0, light = 0, medium = 0, heavy = 0;
        for (auto& i : all)
        {
            types.insert (i.type);
            phase2 += i.phase == 2 ? 1 : 0;
            light += i.weight == EffectWeight::light ? 1 : 0;
            medium += i.weight == EffectWeight::medium ? 1 : 0;
            heavy += i.weight == EffectWeight::heavy ? 1 : 0;
            expect (int (i.params.size()) <= kMaxEffectParams);
            for (auto& p : i.params)
            {
                expect (p.min <= p.def && p.def <= p.max, juce::String (i.type) + "." + p.id + " default out of range");
                if (p.isChoice()) expectEquals (int (p.max) + 1, int (p.choices.size()), juce::String (i.type) + "." + p.id);
            }
        }
        expectEquals (int (types.size()), 30);
        expectEquals (phase2, 24);
        expectEquals (light, 17);
        expectEquals (medium, 10);
        expectEquals (heavy, 3);
    }
};

class LimiterTests : public juce::UnitTest
{
public:
    LimiterTests() : juce::UnitTest ("Limiter", "Foundation") {}

    void runTest() override
    {
        beginTest ("peak <= -1 dBFS for full scale, +12 dB and NaN input (AC-06, AC-07)");
        auto x = test::sine (1000.0, 1.0, 4.0f); // +12 dB over full scale
        x[1000] = std::numeric_limits<float>::quiet_NaN();
        Limiter lim;
        lim.prepare (test::kSr);
        for (size_t pos = 0; pos < x.size(); pos += 480) lim.process (x.data() + pos, 480);
        expect (test::allFinite (x));
        expectLessOrEqual (test::peakDb (x), -1.0f + 1.0e-4f);
        expect (lim.fetchAndClearActive());

        beginTest ("quiet signal passes unchanged apart from the look-ahead delay");
        auto q = test::sine (440.0, 0.5, 0.1f);
        auto y = q;
        lim.reset();
        for (size_t pos = 0; pos < y.size(); pos += 480) lim.process (y.data() + pos, 480);
        const int L = lim.getLatencySamples();
        float maxErr = 0.0f;
        for (size_t i = size_t (L); i < y.size(); ++i) maxErr = std::max (maxErr, std::abs (y[i] - q[i - size_t (L)]));
        expectLessOrEqual (maxErr, 1.0e-6f);
    }
};

static RegistryMetadataTests registryMetadataTests;
static LimiterTests limiterTests;
} // namespace koe

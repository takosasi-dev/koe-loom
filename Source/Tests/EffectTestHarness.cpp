#include "Tests/EffectTestHarness.h"

#include "Dsp/Limiter.h"
#include "Effects/EffectRegistry.h"
#include "Tests/TestUtil.h"

#include <algorithm>

namespace koe::test
{
namespace
{
constexpr int kBlock = 480;

std::vector<float> defaultsOf (const EffectInfo& info)
{
    std::vector<float> p;
    for (auto& s : info.params) p.push_back (s.def);
    return p;
}

void applyAll (IEffect& fx, const std::vector<float>& params)
{
    for (size_t i = 0; i < params.size(); ++i) fx.setParam (int (i), params[i]);
}

const std::vector<float>& standardInput()
{
    static const std::vector<float> input = []
    {
        auto one = concat ({ referenceSpeechOrSynth(), silence (5.0), whiteNoise (5.0, 0.1f, 11) });
        const bool full = juce::SystemStats::getEnvironmentVariable ("KOELOOM_FULL_TESTS", {}).isNotEmpty();
        return full ? concat ({ one, one, one }) : one;
    }();
    return input;
}
} // namespace

std::vector<float> renderEffect (const char* type, const std::vector<float>& in, const std::vector<float>& params)
{
    auto fx = createEffect (type);
    auto* info = findEffectInfo (type);
    if (fx == nullptr || info == nullptr) return {};
    fx->prepare (kSr, kBlock);
    applyAll (*fx, params.empty() ? defaultsOf (*info) : params);
    fx->reset();
    std::vector<float> out (in);
    for (size_t pos = 0; pos < out.size(); pos += kBlock)
        fx->process (out.data() + pos, int (std::min<size_t> (kBlock, out.size() - pos)));
    return out;
}

bool runAc14Checks (juce::UnitTest& t, const char* type)
{
    auto* info = findEffectInfo (type);
    t.expect (info != nullptr, juce::String ("unknown effect type ") + type);
    if (info == nullptr) return false;
    t.expect (hasEffectFactory (type), juce::String ("no factory registered for ") + type);
    if (! hasEffectFactory (type)) return false;

    bool ok = true;
    const auto& input = standardInput();

    auto check = [&] (const std::vector<float>& params, const juce::String& label)
    {
        auto out = renderEffect (type, input, params);
        const bool finite = allFinite (out);
        t.expect (finite, juce::String (type) + " non-finite output: " + label);
        ok = ok && finite;
        if (! finite) return;
        Limiter lim;
        lim.prepare (kSr);
        for (size_t pos = 0; pos < out.size(); pos += kBlock)
            lim.process (out.data() + pos, int (std::min<size_t> (kBlock, out.size() - pos)));
        const float pk = peakDb (out);
        t.expect (pk <= -1.0f + 0.01f, juce::String (type) + " limiter peak " + juce::String (pk, 2) + " dB: " + label);
        ok = ok && pk <= -0.99f;
    };

    const auto defaults = defaultsOf (*info);
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
        }
        else
        {
            auto lo = defaults, hi = defaults;
            lo[p] = spec.min;
            hi[p] = spec.max;
            check (lo, juce::String (spec.id) + "=min");
            check (hi, juce::String (spec.id) + "=max");
        }
    }

    // ---- 50 ms sweeps on steady voice ----
    const auto voice = synthVoice (2.2, 3);
    for (size_t p = 0; p < info->params.size(); ++p)
    {
        const auto& spec = info->params[p];
        if (spec.isChoice()) continue;
        auto fx = createEffect (type);
        fx->prepare (kSr, kBlock);
        auto v = defaults;
        v[p] = spec.min;
        applyAll (*fx, v);
        fx->reset();
        std::vector<float> out (voice);
        const int opStart = int (kSr * 1.0);
        const int sweepBlocks = int (0.050 * kSr) / kBlock; // 5 blocks
        for (int pos = 0, b = 0; pos < int (out.size()); pos += kBlock, ++b)
        {
            const int startBlock = opStart / kBlock;
            if (b >= startBlock && b <= startBlock + sweepBlocks)
            {
                const float frac = float (b - startBlock) / float (sweepBlocks);
                fx->setParam (int (p), spec.clamp (spec.min + frac * (spec.max - spec.min)));
            }
            fx->process (out.data() + pos, std::min (kBlock, int (out.size()) - pos));
        }
        const double r = clickRatio (out, opStart, opStart + int (0.050 * kSr));
        t.expect (r <= 2.0, juce::String (type) + " click on sweep of " + spec.id + ": ratio " + juce::String (r, 2));
        ok = ok && r <= 2.0;
    }
    return ok;
}
} // namespace koe::test

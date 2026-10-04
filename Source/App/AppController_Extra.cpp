// Compare (hold to hear the voice-changer-OFF sound) and おまかせ生成 (INTERFACES.md §9.4 / §9.5, owner wave7/extra).
#include "App/AppController.h"

#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace koe
{
namespace
{
/** std::mt19937 (the same numbers on every standard library; juce::Random's first values barely move between nearby seeds),
    mapped by hand because the std distributions differ between libraries. */
struct Rng
{
    explicit Rng (uint32_t seed) : g (seed) {}
    float nextFloat() { return float (g() >> 8) / 16777216.0f; } // [0, 1)
    int nextInt (int n) { return int (g() % uint32_t (n)); }
    bool nextBool() { return (g() & 1u) != 0; }
    std::mt19937 g;
};

bool idHas (const char* id, const char* part) { return juce::String (id).containsIgnoreCase (part); }

/** A safe random value for one parameter (§9.5): choices uniform, mix 0.2..0.7, feedback up to 60 % of its maximum,
    drive / makeup / level / gain in the lower 40 % of the range, everything else default ± 30 % of the range. */
float randomParam (const ParamSpec& s, Rng& rng)
{
    const float span = s.max - s.min, u = rng.nextFloat();
    float v;
    if (s.isChoice()) v = float (rng.nextInt (int (s.choices.size())));
    else if (idHas (s.id, "mix")) v = s.min + span * (0.2f + 0.5f * u);
    else if (idHas (s.id, "feedback")) v = s.min + (s.max * 0.6f - s.min) * u;
    else if (idHas (s.id, "drive") || idHas (s.id, "makeup") || idHas (s.id, "level") || idHas (s.id, "gain")) v = s.min + span * 0.4f * u;
    else
    {
        const float lo = std::max (s.min, s.def - span * 0.3f), hi = std::min (s.max, s.def + span * 0.3f);
        v = lo + (hi - lo) * u;
    }
    return s.clamp (v);
}

float tenth (float v) { return std::round (v * 10.0f) / 10.0f; }
} // namespace

void AppController::setCompareHold (bool held)
{
    if (compareHeld == held) return;
    compareHeld = held;
    processor.setVoiceChangerOn (settings.voiceChangerOn && ! compareHeld); // same 30 ms crossfade as voice OFF
    sendChangeMessage(); // never saved, never marks the preset modified
}

bool AppController::randomizeCurrent (uint32_t seed, juce::String& whyNot)
{
    if (hasLooperRecording())
    {
        whyNot = juce::String::fromUTF8 ("ルーパーに録音があるため、おまかせ生成はできません。"); // E-27
        return false;
    }
    Rng rng (seed);

    std::vector<const EffectInfo*> pool;
    for (auto& info : allEffectInfos())
    {
        const std::string type (info.type);
        if (type == "freeze" || type == "looper" || type == "convolution" || info.weight == EffectWeight::heavy || ! hasEffectFactory (type)) continue;
        pool.push_back (&info);
    }
    // 2..4 distinct types: a partial Fisher-Yates shuffle with our own generator (std::shuffle differs between libraries)
    const int count = std::min (int (pool.size()), 2 + rng.nextInt (3));
    for (int i = 0; i < count; ++i) std::swap (pool[size_t (i)], pool[size_t (i + rng.nextInt (int (pool.size()) - i))]);
    pool.resize (size_t (count));
    // signal order: registry order, time / space effects last so echo and reverb tails stay clean
    auto rank = [] (const EffectInfo* e) { return (e->category == EffectCategory::timeSpace ? 1000 : 0) + int (e - allEffectInfos().data()); };
    std::sort (pool.begin(), pool.end(), [&] (auto* a, auto* b) { return rank (a) < rank (b); });

    std::vector<SlotDef> chain;
    for (auto* info : pool)
        if (auto slot = makeDefaultSlot (info->type))
        {
            for (size_t k = 0; k < info->params.size() && k < slot->params.size(); ++k) slot->params[k] = randomParam (info->params[k], rng);
            chain.push_back (*slot);
        }

    current.hasShifter = true; // "変換 ON" = card 02 (§9.5); settings.voiceChangerOn stays as it is
    current.pitchSt = tenth (-8.0f + 16.0f * rng.nextFloat());
    current.formantSt = tenth (-4.0f + 8.0f * rng.nextFloat());
    current.layers.clear();
    if (rng.nextBool())
    {
        LayerDef l;
        const float intervals[] = { -12.0f, -7.0f, -5.0f, 5.0f, 7.0f, 12.0f };
        l.pitchSt = intervals[rng.nextInt (6)];
        l.formantSt = tenth (-3.0f + 6.0f * rng.nextFloat());
        l.levelDb = tenth (-14.0f + 6.0f * rng.nextFloat());
        current.layers.push_back (l);
    }
    current.chain = std::move (chain);
    current.outputTrimDb = 0.0f;
    applyPresetToEngine (true);
    markModified();
    return true;
}
} // namespace koe

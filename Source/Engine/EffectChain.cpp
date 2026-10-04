#include "Engine/EffectChain.h"

#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
float followCoeff (double sampleRate, float ms) { return std::exp (-1.0f / std::max (1.0f, float (sampleRate * ms * 0.001))); }
} // namespace

int EffectChain::modIndexFor (const EffectInfo& info, const std::string& target) noexcept
{
    if (target == "wet") return kModWet;
    const int p = info.paramIndex (target);
    return p >= 0 && ! info.params[size_t (p)].isChoice() ? p : kModNone;
}

std::unique_ptr<EffectChain> EffectChain::create (const std::vector<SlotDef>& defs, double sampleRate, int maxBlockSize)
{
    std::unique_ptr<EffectChain> chain (new EffectChain());
    chain->fadeStep = 1.0f / std::max (1.0f, float (sampleRate * kSlotToggleFadeMs * 0.001));
    chain->modAttack = followCoeff (sampleRate, kModAttackMs);
    chain->modRelease = followCoeff (sampleRate, kModReleaseMs);
    chain->onsetSamples = std::max (1, int (sampleRate * 0.005));
    for (auto& d : defs)
    {
        if (int (chain->slots.size()) >= kMaxSlots) break;
        auto* info = findEffectInfo (d.type);
        auto fx = createEffect (d.type);
        if (info == nullptr || fx == nullptr) continue;

        auto s = std::make_unique<Slot>();
        s->type = d.type;
        s->info = info;
        fx->prepare (sampleRate, maxBlockSize);
        if (! d.file.empty())
            fx->setAssetPath (paths::irDir().getChildFile (juce::String::fromUTF8 (d.file.c_str())).getFullPathName().toStdString());
        for (size_t p = 0; p < info->params.size(); ++p)
        {
            const float v = info->params[p].clamp (p < d.params.size() ? d.params[p] : info->params[p].def);
            s->params[p].store (v);
            s->applied[p] = v;
            fx->setParam (int (p), v);
        }
        fx->reset();
        s->fx = std::move (fx);
        s->enabled.store (d.enabled);
        s->wet.store (std::clamp (d.wet, 0.0f, 1.0f));
        s->modIndex.store (modIndexFor (*info, d.modTarget));
        s->modDepth.store (std::clamp (d.modDepth, -1.0f, 1.0f));
        s->running = d.enabled;
        s->fade = d.enabled ? s->wet.load() : 0.0f;
        s->latency.store (d.enabled ? s->fx->getLatencySamples() : 0);
        s->dry.assign (size_t (maxBlockSize), 0.0f);
        s->onsetLeft = chain->onsetSamples;
        chain->slots.push_back (std::move (s));
    }
    return chain;
}

std::unique_ptr<EffectChain> EffectChain::createFromEffects (std::vector<TestSlot> effects, double sampleRate, int maxBlockSize)
{
    std::unique_ptr<EffectChain> chain (new EffectChain());
    chain->fadeStep = 1.0f / std::max (1.0f, float (sampleRate * kSlotToggleFadeMs * 0.001));
    chain->onsetSamples = std::max (1, int (sampleRate * 0.005));
    for (auto& t : effects)
    {
        auto s = std::make_unique<Slot>();
        s->type = t.info->type;
        s->info = t.info;
        t.fx->prepare (sampleRate, maxBlockSize);
        for (size_t p = 0; p < t.info->params.size(); ++p)
        {
            s->params[p].store (t.info->params[p].def);
            s->applied[p] = t.info->params[p].def;
            t.fx->setParam (int (p), t.info->params[p].def);
        }
        t.fx->reset();
        s->fx = std::move (t.fx);
        s->enabled.store (t.enabled);
        s->running = t.enabled;
        s->fade = t.enabled ? 1.0f : 0.0f;
        s->latency.store (t.enabled ? s->fx->getLatencySamples() : 0);
        s->dry.assign (size_t (maxBlockSize), 0.0f);
        s->onsetLeft = chain->onsetSamples;
        chain->slots.push_back (std::move (s));
    }
    return chain;
}

void EffectChain::process (float* x, int n)
{
    // After a device reopens with a larger buffer the old chain still runs (its 30 ms crossfade):
    // never hand the effects, or the dry copies, more than the block size they were prepared for.
    if (slots.empty()) return;
    const int maxN = std::max (1, int (slots[0]->dry.size()));
    for (int pos = 0; pos < n; pos += maxN) processChunk (x + pos, std::min (maxN, n - pos));
}

void EffectChain::processChunk (float* x, int n)
{
    // 声の大きさで変わる効果 (INTERFACES.md §11): follow the chain input only while some slot is modulated
    bool anyMod = false;
    for (auto& s : slots) anyMod = anyMod || s->modIndex.load (std::memory_order_relaxed) != kModNone;
    float level = 0.0f;
    if (anyMod)
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            const float k = a > modEnv ? modAttack : modRelease;
            modEnv = std::isfinite (a) ? a + k * (modEnv - a) : modEnv;
        }
        const float db = 20.0f * std::log10 (modEnv + 1.0e-9f);
        level = std::clamp ((db - kModLowDb) / (kModHighDb - kModLowDb), 0.0f, 1.0f);
    }
    else modEnv = 0.0f;
    modLevel.store (level, std::memory_order_relaxed);

    for (int si = 0; si < int (slots.size()); ++si)
    {
        auto& s = *slots[size_t (si)];
        const bool want = s.enabled.load (std::memory_order_relaxed) && ! s.autoStopped.load (std::memory_order_relaxed);
        const int mi = s.modIndex.load (std::memory_order_relaxed);
        const float mod = mi != kModNone ? s.modDepth.load (std::memory_order_relaxed) * level : 0.0f;
        auto paramValue = [&s, mi, mod] (size_t p)
        {
            const float v = s.params[p].load (std::memory_order_relaxed);
            if (int (p) != mi) return v;
            const auto& spec = s.info->params[p];
            return spec.clamp (v + mod * (spec.max - spec.min));
        };

        if (want && ! s.running)
        {
            // F-04-7: entering ON clears internal state, then fades in over 20 ms
            for (size_t p = 0; p < s.info->params.size(); ++p)
            {
                s.applied[p] = paramValue (p);
                s.fx->setParam (int (p), s.applied[p]);
            }
            s.fx->reset();
            s.running = true;
            s.fade = 0.0f;
            s.onsetLeft = onsetSamples;
        }
        if (! s.running)
        {
            s.pendingTrigger.store (0, std::memory_order_relaxed); // triggers only act while ON
            s.latency.store (0, std::memory_order_relaxed);
            continue;
        }

        for (size_t p = 0; p < s.info->params.size(); ++p)
        {
            const float v = paramValue (p);
            if (v != s.applied[p]) { s.applied[p] = v; s.fx->setParam (int (p), v); }
        }
        if (const int trig = s.pendingTrigger.exchange (0); trig != 0 && want)
            s.fx->trigger (static_cast<EffectTrigger> (trig));

        std::copy (x, x + n, s.dry.begin());
        if (s.onsetLeft > 0)
        {
            // A freshly reset effect has empty delay lines; whatever first arrives through them would
            // step in from silence (a click when a chorus/echo/reverb tap first reaches real audio).
            // Ramping its input in over 5 ms makes those first arrivals ramp in too. The slot / chain
            // crossfade still starts near 0 here, so the effect's own dry part dipping is inaudible.
            for (int i = 0; i < n && s.onsetLeft > 0; ++i, --s.onsetLeft)
                x[i] *= 1.0f - float (s.onsetLeft) / float (onsetSamples);
        }
        s.fx->process (x, n);

        bool finite = true;
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (x[i])) { finite = false; break; }
        if (! finite)
        {
            std::copy (s.dry.begin(), s.dry.begin() + n, x);
            s.autoStopped.store (true);
            s.enabled.store (false);
            s.running = false;
            s.fade = 0.0f;
            s.latency.store (0, std::memory_order_relaxed);
            pushEvent (si);
            continue;
        }

        const float wet = s.wet.load (std::memory_order_relaxed) + (mi == kModWet ? mod : 0.0f);
        const float target = want ? std::clamp (wet, 0.0f, 1.0f) : 0.0f;
        if (s.fade != 1.0f || target != 1.0f) // steady at wet 1 (the usual case): the effect's output as is
        {
            for (int i = 0; i < n; ++i)
            {
                s.fade = target > s.fade ? std::min (target, s.fade + fadeStep) : std::max (target, s.fade - fadeStep);
                x[i] = s.dry[size_t (i)] + s.fade * (x[i] - s.dry[size_t (i)]);
            }
            if (! want && s.fade <= 0.0f)
            {
                // OFF: let the effect settle now (looper commits its take, freeze turns off, F-04-7)
                s.running = false;
                s.fx->reset();
            }
        }
        s.latency.store (s.running ? s.fx->getLatencySamples() : 0, std::memory_order_relaxed);
    }
}

void EffectChain::resetAll()
{
    for (auto& s : slots)
        if (s->running) s->fx->reset();
}

int EffectChain::getLatencySamples() const noexcept
{
    int total = 0;
    for (auto& s : slots) total += s->latency.load (std::memory_order_relaxed);
    return total;
}

void EffectChain::autoStop (int i) noexcept
{
    if (i < 0 || i >= int (slots.size())) return;
    slots[size_t (i)]->autoStopped.store (true);
    pushEvent (i);
}

void EffectChain::pushEvent (int slotIndex) noexcept
{
    const int w = evWrite.load (std::memory_order_relaxed);
    const int next = (w + 1) % int (events.size());
    if (next == evRead.load (std::memory_order_acquire)) return; // full: drop (the flag is still set)
    events[size_t (w)].store (slotIndex, std::memory_order_relaxed);
    evWrite.store (next, std::memory_order_release);
}

bool EffectChain::popAutoStopEvent (int& slotIndex) noexcept
{
    const int r = evRead.load (std::memory_order_relaxed);
    if (r == evWrite.load (std::memory_order_acquire)) return false;
    slotIndex = events[size_t (r)].load (std::memory_order_relaxed);
    evRead.store ((r + 1) % int (events.size()), std::memory_order_release);
    return true;
}
} // namespace koe

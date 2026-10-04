#include "Engine/MomentaryFx.h"

#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace koe
{
const std::vector<MomentaryRecipeDef>& momentaryRecipeDefs()
{
    // light / medium effects without latency (INTERFACES.md §10.3). Tails: how long the release may still ring.
    static const std::vector<MomentaryRecipeDef> defs {
        { "yamabiko", "やまびこ", "echo", { { "mode", 0.0f }, { "timeMs", 380.0f }, { "feedback", 0.45f }, { "toneHz", 5000.0f }, { "mix", 0.45f } }, 4.0f },
        { "cathedral", "大聖堂の残響", "reverb", { { "type", 5.0f }, { "room", 0.85f }, { "damp", 0.4f }, { "preDelayMs", 20.0f }, { "mix", 0.45f } }, 8.0f },
        { "cave", "洞窟", "reverb", { { "type", 9.0f }, { "room", 0.8f }, { "damp", 0.5f }, { "mix", 0.4f } }, 6.0f },
        { "telephone", "電話", "voicechar", { { "kind", 0.0f }, { "intensity", 0.7f }, { "mix", 1.0f } }, 0.5f },
        { "radio", "ラジオ", "voicechar", { { "kind", 1.0f }, { "intensity", 0.7f }, { "mix", 1.0f } }, 0.5f },
        { "megaphone", "メガホン", "voicechar", { { "kind", 2.0f }, { "intensity", 0.7f }, { "mix", 1.0f } }, 0.5f },
        { "robot", "ロボット", "ringmod", { { "freqHz", 90.0f }, { "mix", 0.8f } }, 0.5f },
        { "walkie", "トランシーバー", "voicechar", { { "kind", 3.0f }, { "intensity", 0.7f }, { "mix", 1.0f } }, 0.5f },
    };
    return defs;
}

const MomentaryRecipeDef* findMomentaryRecipe (const juce::String& id)
{
    for (auto& r : momentaryRecipeDefs())
        if (id == r.id) return &r;
    return nullptr;
}

std::vector<SlotDef> momentaryRecipeChain (const MomentaryRecipeDef& r)
{
    const auto* info = findEffectInfo (r.type);
    if (info == nullptr) return {};
    SlotDef s;
    s.type = r.type;
    for (auto& p : info->params) s.params.push_back (p.def);
    for (auto& [id, v] : r.params)
        if (const int i = info->paramIndex (id); i >= 0) s.params[size_t (i)] = info->params[size_t (i)].clamp (v);
    return { s };
}

// ============================================================================ MomentaryFx
MomentaryFx::MomentaryFx()
{
    send.assign (size_t (kMaxBlockSize), 0.0f);
    gains.assign (size_t (kMaxBlockSize), 0.0f);
}

MomentaryFx::~MomentaryFx()
{
    for (int s = 0; s < kMomentarySlots; ++s)
    {
        delete state[size_t (s)].program;
        delete pending[size_t (s)].exchange (nullptr);
        for (auto& r : retired[size_t (s)]) delete r.exchange (nullptr);
    }
}

void MomentaryFx::setChain (int slot, std::unique_ptr<EffectChain> chain, double sampleRate, float tailSeconds)
{
    auto* p = new Program();
    p->chain = std::move (chain);
    p->rate = sampleRate;
    p->fadeSamples = std::max (1, int (std::lround (sampleRate * 0.03))); // 30 ms in and out
    p->tailSamples = int (sampleRate * double (tailSeconds));
    const auto i = size_t (slot);
    if (lastRate[i] != 0.0 && lastRate[i] != sampleRate) swapNow[i].store (true);
    lastRate[i] = sampleRate;
    delete pending[i].exchange (p, std::memory_order_acq_rel); // one the audio thread never took
}

void MomentaryFx::collectGarbage()
{
    for (auto& slot : retired)
        for (auto& r : slot) delete r.exchange (nullptr, std::memory_order_acq_rel);
}

void MomentaryFx::process (float* x, int n)
{
    for (int s = 0; s < kMomentarySlots; ++s)
    {
        const auto si = size_t (s);
        auto& st = state[si];

        // hand-over: only the audio thread fills a retire slot, so a free one stays free until we use it
        if (! st.running || swapNow[si].load (std::memory_order_relaxed))
        {
            std::atomic<Program*>* room = nullptr;
            for (auto& r : retired[si])
                if (r.load (std::memory_order_acquire) == nullptr) { room = &r; break; }
            if (room != nullptr || st.program == nullptr)
                if (auto* p = pending[si].exchange (nullptr, std::memory_order_acq_rel))
                {
                    if (st.program != nullptr) room->store (st.program, std::memory_order_release);
                    st.program = p;
                    swapNow[si].store (false, std::memory_order_relaxed);
                }
        }

        auto* p = st.program;
        if (p == nullptr || p->chain == nullptr)
        {
            st.running = false;
            active[si].store (false, std::memory_order_relaxed);
            continue;
        }
        const bool want = held[si].load (std::memory_order_relaxed);
        if (! st.running)
        {
            if (! want) continue; // idle: the voice passes untouched
            p->chain->resetAll();
            st.running = true;
            st.gain = 0.0f;
        }

        const float step = 1.0f / float (p->fadeSamples);
        for (int pos = 0; pos < n; pos += kMaxBlockSize)
        {
            const int m = std::min (kMaxBlockSize, n - pos);
            float* y = x + pos;
            for (int i = 0; i < m; ++i)
            {
                st.gain = want ? std::min (1.0f, st.gain + step) : std::max (0.0f, st.gain - step);
                gains[size_t (i)] = st.gain;
                send[size_t (i)] = y[i] * st.gain;
            }
            p->chain->process (send.data(), m);
            for (int i = 0; i < m; ++i) y[i] = y[i] * (1.0f - gains[size_t (i)]) + send[size_t (i)];
        }

        if (want || st.gain > 0.0f) st.tailLeft = p->tailSamples;
        else if ((st.tailLeft -= n) <= 0) st.running = false;
        active[si].store (st.running, std::memory_order_relaxed);
    }
}
} // namespace koe

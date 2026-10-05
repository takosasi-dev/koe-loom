#pragma once

// Common interface for the 30 chain effects (koeloom_effects.md, spec §9.4).
// The metadata (parameters, ranges, defaults, weight) for every type lives in EffectRegistry.cpp,
// which is the code copy of koeloom_effects.md §2–§3. Implementations register a factory with
// KOE_REGISTER_EFFECT so no central file has to name them.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace koe
{
enum class EffectCategory { dynamics, eqFilter, distortion, modulation, rhythm, timeSpace, special, pitchVocoder };
enum class EffectWeight { light, medium, heavy };

struct ParamSpec
{
    const char* id;      // JSON key (koeloom_effects.md §3), e.g. "thresholdDb"
    const char* nameJa;  // UI label
    float min, max, def; // spec units; choice params: 0..n-1 and def = index
    const char* unit;    // "dB", "ms", "Hz", ":1", "oct", "st", "bit", "%" ... or ""
    std::vector<std::pair<const char*, const char*>> choices = {}; // (json id, Japanese label)
    bool integer = false;                                        // values are rounded

    bool isChoice() const noexcept { return ! choices.empty(); }
    float clamp (float v) const noexcept
    {
        v = v < min ? min : (v > max ? max : v);
        if (integer || isChoice()) v = float (int (v + (v >= 0 ? 0.5f : -0.5f)));
        return v;
    }
};

struct EffectInfo
{
    const char* type;    // JSON "type"
    const char* nameJa;
    EffectCategory category;
    EffectWeight weight;
    int phase;           // 2 or 5
    const char* descJa;  // the "内容" column, also used for tooltips (F-13-6)
    std::vector<ParamSpec> params;

    int paramIndex (std::string_view id) const noexcept
    {
        for (size_t i = 0; i < params.size(); ++i)
            if (id == params[i].id) return int (i);
        return -1;
    }
};

/** Trigger actions for the Phase 5 momentary effects (freeze / looper) and wave 10's tapestop (INTERFACES.md §12). */
enum class EffectTrigger { freezeToggle = 1, looperRecordPlay = 2, looperClear = 3, tapeStopToggle = 4 };

/**
    Threading contract (enforced by the chain):
    - prepare() runs on a non-audio thread and may allocate.
    - After prepare(), the host calls setParam() once for every parameter and then reset();
      reset() must snap every internally smoothed value to its current target.
    - setParam(), process(), reset(), trigger() run on the audio thread: no allocation, no locks,
      no I/O. setParam() values are already clamped to the ParamSpec range (choices = index).
      A parameter change must not click: smooth internally over 20–50 ms where needed (F-04-9).
    - process() is in-place mono. Output must stay finite for finite input; the chain still checks
      and auto-bypasses on NaN/Inf (F-04-11).
*/
class IEffect
{
public:
    virtual ~IEffect() = default;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;
    virtual void setParam (int index, float value) = 0;
    virtual void process (float* samples, int numSamples) = 0;
    /** Latency of the wet path; effects with a dry/wet mix delay their dry path by the same amount (F-04-12). */
    virtual int getLatencySamples() const { return 0; }
    /** Phase 5 momentary controls (freeze / looper). Ignored by everything else. */
    virtual void trigger (EffectTrigger) {}
    /** Small UI-visible state, e.g. looper state 0..3, freeze on/off. Read from the UI thread (atomic inside). */
    virtual int getUiState() const { return 0; }
    /** Effects that use a file (convolution, INTERFACES.md §9). Message thread, after prepare() and before reset();
        may allocate and read the file. Empty path = no file. A missing or unreadable file must leave the effect
        passing its input through (and getUiState() may report it). */
    virtual void setAssetPath (const std::string& /*utf8Path*/) {}
};

using EffectFactory = std::unique_ptr<IEffect> (*)();
bool registerEffectFactory (const char* type, EffectFactory factory);

} // namespace koe

#define KOE_REGISTER_EFFECT(typeString, ClassName)                                                                \
    namespace                                                                                                     \
    {                                                                                                             \
    const bool koeRegistered_##ClassName = ::koe::registerEffectFactory (                                         \
        typeString, []() -> std::unique_ptr<::koe::IEffect> { return std::make_unique<ClassName>(); });           \
    }

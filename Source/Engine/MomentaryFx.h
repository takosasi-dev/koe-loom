#pragma once

// 押している間だけのエフェクト (INTERFACES.md §10.3, owner wave8/automation).
// The recipes (an existing effect with fixed knobs) and the IVoicePostProcessor that runs them while a hotkey is held.

#include "Core/Constants.h"
#include "Engine/EffectChain.h"
#include "Engine/VoiceProcessor.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace koe
{
struct MomentaryRecipeDef
{
    const char* id;
    const char* nameJa;
    const char* type;                                   // effect type (light / medium, no latency)
    std::vector<std::pair<const char*, float>> params;  // overrides of the registry defaults
    float tailSeconds;                                  // keeps running this long after the release (echo / reverb tails)
};

/** Every recipe, in UI order. */
const std::vector<MomentaryRecipeDef>& momentaryRecipeDefs();
/** nullptr for "" or an unknown id. */
const MomentaryRecipeDef* findMomentaryRecipe (const juce::String& id);
/** The one-slot chain of a recipe (registry defaults + its overrides). */
std::vector<SlotDef> momentaryRecipeChain (const MomentaryRecipeDef& r);

/**
    Up to kMomentarySlots chains, each faded in while its slot is held (30 ms) and faded out on release by closing the
    chain's input only, so echoes and reverbs ring out. out = (1 - g) * x + chain (g * x). A slot that is neither held
    nor ringing is not processed at all (the voice passes untouched, bit for bit).
    Chains are built on the message thread (setChain) and handed over without allocation, like VoiceProcessor's chain:
    a waiting chain is taken while its slot is idle, or at once when the sample rate changed (device reopen).
*/
class MomentaryFx final : public IVoicePostProcessor
{
public:
    MomentaryFx();
    ~MomentaryFx() override;

    /** Message thread. chain == nullptr: the slot does nothing. */
    void setChain (int slot, std::unique_ptr<EffectChain> chain, double sampleRate, float tailSeconds);
    /** Any thread. */
    void setHeld (int slot, bool on) noexcept { held[size_t (slot)].store (on); }
    /** True while the slot is being processed (held, fading or ringing out). Any thread. */
    bool isActive (int slot) const noexcept { return active[size_t (slot)].load(); }
    /** Message thread: frees chains the audio thread has let go of. */
    void collectGarbage();

    void process (float* samples, int numSamples) override;

private:
    struct Program
    {
        std::unique_ptr<EffectChain> chain;
        double rate = 0.0;
        int fadeSamples = 1;
        int tailSamples = 0;
    };
    struct SlotState                      // audio thread only
    {
        Program* program = nullptr;
        bool running = false;
        float gain = 0.0f;
        int tailLeft = 0;
    };

    std::array<std::atomic<bool>, kMomentarySlots> held {}, active {};
    std::array<std::atomic<bool>, kMomentarySlots> swapNow {};  // the waiting chain is for a new rate: take it even while running
    std::array<double, kMomentarySlots> lastRate {};             // message thread
    std::array<std::atomic<Program*>, kMomentarySlots> pending {};
    std::array<std::array<std::atomic<Program*>, 2>, kMomentarySlots> retired {};
    std::array<SlotState, kMomentarySlots> state {};
    std::vector<float> send, gains;       // kMaxBlockSize, allocated once
};
} // namespace koe

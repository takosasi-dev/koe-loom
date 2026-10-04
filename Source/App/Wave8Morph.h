#pragma once

// プリセットを混ぜる (INTERFACES.md §10). Owner: wave8/morph. AppController keeps one MorphData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.

#include "Model/Preset.h"

#include <utility>
#include <vector>

namespace koe
{
struct MorphData
{
    Preset a, b;                            // the two presets as they were when the blend began
    float trimA = 0.0f, trimB = 0.0f;       // their effective trims (a calibrated one for a built-in)
    std::vector<std::pair<int, int>> pairs; // blended slot order: (index in a.chain, index in b.chain), -1 = that side has none
    float amount = 0.0f;                    // 0 = A .. 1 = B
    int builds = -1;                        // AppController::chainBuilds right after beginMorph's own rebuild
    bool active = false;
};

// The blend itself, without the controller (AppController_Morph.cpp; the tests call these directly).
namespace morph
{
/** Slots paired by type, k-th with k-th ("convolution" only with the same file). The order is A's; a slot only B has goes
    right after the pair that came before it in B (at the front if none did). */
std::vector<std::pair<int, int>> pairSlots (const std::vector<SlotDef>& a, const std::vector<SlotDef>& b);
/** The working preset at amount t (id from A, name left to the caller, builtin false). Numbers glide, choices switch at 0.5,
    a slot one side lacks (or has OFF) fades by its wet, a voice one side lacks fades in linear gain (OFF below the level range). */
Preset blend (const MorphData& m, float t);
/** Japanese reason when the blend breaks the chain rules (10 slots, heavy effects ON, one freeze / looper), else empty. */
juce::String checkBlend (const Preset& p);
/** "A × B" within kPresetNameMaxChars (the longer name is shortened first). */
juce::String blendName (const juce::String& a, const juce::String& b);
} // namespace morph
} // namespace koe

#pragma once

#include "Core/Constants.h"
#include "Effects/IEffect.h"
#include "Model/Preset.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace koe
{
struct EffectInfo;

/**
    One immutable-structure chain of up to 10 slots (spec §5.6, F-04). Built on the message thread
    (create() prepares every effect), then handed to the audio thread; structure changes build a new
    chain and swap with a 30 ms crossfade (VoiceProcessor). Per-slot ON/OFF and parameters are atomics
    written by the message thread and applied by the audio thread before each block.
*/
class EffectChain
{
public:
    struct Slot
    {
        std::string type;
        const EffectInfo* info = nullptr;
        std::unique_ptr<IEffect> fx;

        // ---- written by the message thread, read by the audio thread ----
        std::atomic<bool> enabled { true };
        std::array<std::atomic<float>, kMaxEffectParams> params {};
        std::atomic<int> pendingTrigger { 0 };   // EffectTrigger value, 0 = none
        std::atomic<float> wet { 1.0f };         // 0..1, how much of the slot's output replaces its input while ON
                                                 // (プリセットを混ぜる, INTERFACES.md §10.1). Glides like the ON/OFF fade.

        // ---- written by the audio thread, read by the message thread ----
        std::atomic<bool> autoStopped { false }; // NaN/Inf auto-bypass (F-04-11) or watchdog (§5.6)
        std::atomic<int> latency { 0 };

        // ---- audio thread only ----
        std::array<float, kMaxEffectParams> applied {};
        bool running = false;
        float fade = 0.0f;
        int onsetLeft = 0;   // samples of input onset ramp left after a reset (see process())
        std::vector<float> dry;
    };

    /** Message thread. Unknown types / types without a factory are skipped (the caller validated). */
    static std::unique_ptr<EffectChain> create (const std::vector<SlotDef>& defs, double sampleRate, int maxBlockSize);

    /** Tests: build from ready-made effect objects (each paired with the metadata it pretends to be). */
    struct TestSlot { const EffectInfo* info; std::unique_ptr<IEffect> fx; bool enabled = true; };
    static std::unique_ptr<EffectChain> createFromEffects (std::vector<TestSlot> effects, double sampleRate, int maxBlockSize);

    int size() const noexcept { return int (slots.size()); }
    Slot& slot (int i) noexcept { return *slots[size_t (i)]; }
    const Slot& slot (int i) const noexcept { return *slots[size_t (i)]; }

    /** Audio thread. In-place mono. */
    void process (float* samples, int numSamples);
    /** Audio thread: clear every running effect's state (used when the voice changer is switched back ON). */
    void resetAll();

    /** Sum of the latencies of running slots (F-04-12). Any thread. */
    int getLatencySamples() const noexcept;

    /** Message thread: slot index whose output went non-finite (F-04-11), one per call. */
    bool popAutoStopEvent (int& slotIndex) noexcept;
    /** Audio thread (watchdog, §5.6): stop a slot as "自動停止". */
    void autoStop (int slotIndex) noexcept;

private:
    EffectChain() = default;
    void processChunk (float* samples, int numSamples);
    void pushEvent (int slotIndex) noexcept;

    std::vector<std::unique_ptr<Slot>> slots;
    float fadeStep = 1.0f / 960.0f;
    int onsetSamples = 240;
    // tiny SPSC ring for auto-stop events
    std::array<std::atomic<int>, 16> events {};
    std::atomic<int> evWrite { 0 }, evRead { 0 };
};
} // namespace koe

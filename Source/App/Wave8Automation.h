#pragma once

// 押している間だけのエフェクト and アプリごとの自動切り替え (INTERFACES.md §10). Owner: wave8/automation. AppController keeps one AutomationData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.

#include "Core/Constants.h"
#include "Engine/MomentaryFx.h"

#include <array>
#include <memory>
#include <string>

namespace koe
{
struct AutomationData
{
    // ---- 押している間だけのエフェクト ----
    std::unique_ptr<MomentaryFx> fx;                    // made on first need, attached to the processor until shutdown
    bool attached = false;
    std::array<juce::String, kMomentarySlots> built;    // the recipe each slot's chain was built for
    double builtRate = 0.0;
    int builtBlock = 0;
    std::array<bool, kMomentarySlots> held {};
    std::array<int, kMomentarySlots> heldKey {};        // virtual key of the hotkey that pressed it (0 = none)

    // ---- アプリごとの自動切り替え ----
    int switchTicks = 0;
    juce::String appliedRule;       // "exe|presetId" of the rule last acted on ("" = none matched)
    juce::String blockedRule;       // the rule we already told the user we could not apply
    std::string switchedTo;         // the preset an automatic switch loaded ("" = none in effect)
    std::string restoreId;          // the preset in use before the first automatic switch
};

/** The S-03 「押して試す」 button holds slot index while the mouse is down: tickAutomation does not release it for a
    key that is up (or unbound) meanwhile. Message thread. */
void setMomentaryUiHold (int index, bool held);
} // namespace koe

#pragma once

#include "Core/Constants.h"

namespace koe
{
/**
    CPU protection (spec §5.6, F-02-8). Fed once per audio callback with the callback's load
    (processing time / buffer time). When the load stays above 80 % for 1 s it asks for one action:
    stop the layers first, then (Phase 5) one heavy effect at a time from the highest slot, waiting
    another second after each; when nothing is left to stop it keeps asking to notify. Audio thread.
*/
class Watchdog
{
public:
    enum class Action { none, stopLayers, stopHeavySlot, notifyOnly };

    Action update (double load, double blockSeconds, bool layersRunning, bool heavySlotRunning) noexcept
    {
        if (load <= kWatchdogLoad)
        {
            overloadSeconds = 0.0;
            return Action::none;
        }
        overloadSeconds += blockSeconds;
        if (overloadSeconds < kWatchdogHoldSeconds) return Action::none;
        overloadSeconds = 0.0; // look again for a full second after each step
        if (layersRunning) return Action::stopLayers;
        if (heavySlotRunning) return Action::stopHeavySlot;
        return Action::notifyOnly;
    }
    void reset() noexcept { overloadSeconds = 0.0; }

private:
    double overloadSeconds = 0.0;
};
} // namespace koe

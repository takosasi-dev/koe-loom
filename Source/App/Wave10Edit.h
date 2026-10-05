#pragma once

// 元に戻す / やり直し and A/B 聞き比べ (INTERFACES.md §12). Owner: wave10/edit. AppController keeps one EditData as a private
// member; put this feature's state here (the history, the saved copy played during A/B ...) so AppController.h stays untouched.

#include "Model/Preset.h"

#include <deque>
#include <optional>

namespace koe
{
struct EditData
{
    EditData() = default;
    EditData (const EditData&) = delete;
    EditData& operator= (const EditData&) = delete;

    std::deque<Preset> undo, redo;   // states to go back / forward to, oldest first (at most kUndoSteps)
    std::optional<Preset> last;      // the working preset as of the last step: what the next edit goes back to (nullopt = unknown)
    double lastEditMs = -1.0e9;      // nowMs() of the last noteEdit (edits closer than kUndoMergeMs merge)
    bool fresh = true;               // no noteEdit / editReset yet (startup() sets the preset without a hook)
    bool abOn = false;               // A/B: the engine plays abSaved, the screen keeps the working preset
    Preset abSaved;                  // the library copy of the preset the working one came from

    /** Tests: added to the clock, so steps can be spaced past kUndoMergeMs without sleeping. */
    static inline double clockOffsetMsForTests = 0.0;
    static double nowMs() { return juce::Time::getMillisecondCounterHiRes() + clockOffsetMsForTests; }
};
} // namespace koe

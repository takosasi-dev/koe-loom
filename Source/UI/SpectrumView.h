#pragma once

// 声の見える化 (INTERFACES.md §12). Owner: wave10/viz. The strip in the slot detail panel (S-07); the big view is the
// ツール page's 7th tool (makeSpectrumTool in UI/main/ToolsView.h). Both are a SpectrumView.

#include "App/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace koe::ui
{
/** Height the slot detail panel gives the strip (it lays the strip out above 声の大きさで動かす). */
inline constexpr int kSpectrumStripH = 72;

/** The spectra before (加工前: a thin dashed line in a quiet colour) and after processing (加工後: the accent line with a
    light fill), on a log frequency axis (100 / 1k / 10k) with a dB scale and a legend. Polls AppController::pollSpectrum()
    at 30 fps while it is visible (the analysis stops by itself ~1 s after the last poll). compact = the strip: a title
    row and fewer scale marks. */
class SpectrumView : public juce::Component, public juce::Timer // public: snapshots and tests drive timerCallback()
{
public:
    SpectrumView (AppController& c, bool compact);

    void visibilityChanged() override;
    void timerCallback() override;
    void paint (juce::Graphics& g) override;

    bool hasData() const noexcept { return data; }
    const std::array<float, kSpectrumBands>& inputDb() const noexcept { return in; }
    const std::array<float, kSpectrumBands>& outputDb() const noexcept { return out; }
    /** What paint() writes over the graph when there is nothing to draw ("" while there is data). */
    juce::String statusText() const;

private:
    void poll();
    void drawLegend (juce::Graphics& g, juce::Rectangle<int> row) const;

    AppController& controller;
    const bool compact;
    const float minDb, maxDb; // the vertical axis
    std::array<float, kSpectrumBands> in {}, out {};
    bool data = false, running = false;
};

/** The input / output spectra strip for SlotDetailPanel; runs its own timer while showing. nullptr = no strip. */
std::unique_ptr<juce::Component> makeSpectrumStrip (AppController& c);
} // namespace koe::ui

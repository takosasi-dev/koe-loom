#pragma once

// S-01 ボイス page (docs/mockups/A-S01.dc.html, below 1000 px A-S01-min.dc.html = §8.2.1):
// preset bar with favourites (F-05-12), the cards 入力 / 声の変換 / 重ねる声 / 出力, the chain strip,
// and the bottom bar (monitor, latency, XRUN, CPU, 自動停止).

#include "UI/main/ChainStrip.h"
#include "UI/main/VoicePage.h"

namespace koe::ui::mainui
{
/** Bottom bar (monitor, latency, XRUN, CPU, 自動停止) shown under S-01 and S-02 (A-S02.dc.html has it too).
    Only the S-01 copy carries the tour IDs, so the tour never lands on the hidden twin. */
class BottomBar : public juce::Component
{
public:
    BottomBar (AppController& c, Navigator& nav, bool tourTarget);
    ~BottomBar() override;
    static int height (bool compact) { return compact ? Theme::touchMin : Theme::buttonH; }
    void setCompact (bool compact);
    void refresh();
    /** 30 fps (the status itself updates every 5th call). */
    void tick (const AppController::Status& s);
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class VoiceView : public VoicePage
{
public:
    VoiceView (AppController& c, Navigator& nav);
    ~VoiceView() override;

    /** §8.2.1 layout (MainComponent passes width < Theme::narrowWidth). */
    void setCompact (bool compact) override;
    bool isCompact() const override;
    /** Model changed (AppController change message). */
    void refresh() override;
    /** 30 fps: meters and status (F-08-1). */
    void tick() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    ChainStrip& chainStrip();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace koe::ui::mainui

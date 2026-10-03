#pragma once

// S-08 ガイドツアー (§8.5, F-13, docs/mockups/A-S08.dc.html): 7 steps over the real screen. Each step
// finds its parts by component ID, dims everything else, and shows a bubble with
// "n / 7", 次へ / 戻る / スキップ. Progress goes to Settings::tourStep (F-13-4). Parts that cannot be shown
// are skipped and logged (E-32).

#include "UI/main/Common.h"

namespace koe::ui::mainui
{
class GuideTour : public juce::Component, private juce::Timer
{
public:
    struct Step
    {
        juce::StringArray ids;          // highlighted together
        Navigator::Page page;
        Navigator::SettingsSection section;
        juce::StringArray fallbackIds;  // on the voice page, if the settings part is missing
        const char* title;
        const char* text;               // <= 60 characters (F-13-3)
    };
    static const std::vector<Step>& steps();

    /** root: the component that holds every page (MainComponent). onFinished runs after the last step or スキップ. */
    GuideTour (juce::Component& root, AppController& c, Navigator& nav, int startStep, std::function<void()> onFinished);
    ~GuideTour() override;

    int currentStep() const { return step; }
    void next();
    void back();
    void skip();
    /** Re-finds the current step's parts (window resized, banners appeared, E-31). */
    void relayout();

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool hitTest (int x, int y) override;
    bool keyPressed (const juce::KeyPress& k) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    class Bubble;
    void show (int index, int direction);
    void placeBubble();
    bool locate (const Step& s, juce::Rectangle<int>& out);
    void timerCallback() override;
    void finish();

    juce::Component& root;
    AppController& c;
    Navigator& nav;
    std::function<void()> onFinished;
    std::unique_ptr<Bubble> bubble;
    int step = 0;
    juce::Rectangle<float> hole, holeFrom, holeTo;
    double animStart = 0.0;
    bool finished = false;
};
} // namespace koe::ui::mainui

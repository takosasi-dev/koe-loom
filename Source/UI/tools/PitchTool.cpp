// 声の高さ tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/analysis.
// Hz and note name of the voice as it comes in and after processing, a 10 s scrolling graph of both, and the guide bands.
// Polls AppController::pollPitch() at 30 fps while shown (the analysis stops by itself ~1 s after the last poll).

#include "UI/main/ToolsView.h"

#include "Engine/VoiceAnalysis.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"
#include "UI/main/Common.h"

#include <array>
#include <cmath>

namespace koe::ui
{
namespace
{
constexpr int kHistory = 300;                      // 10 s at 30 fps
constexpr float kLowHz = 60.0f, kHighHz = 700.0f;  // graph range (log)

/** Visible up the tree and the window not minimised. Unlike isShowing() this also holds offscreen (snapshots, tests). */
bool shownInTree (const juce::Component& c)
{
    for (auto* p = &c; p->getParentComponent() != nullptr; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    auto* top = c.getTopLevelComponent();
    if (auto* peer = top->getPeer()) return top->isVisible() && ! peer->isMinimised();
    return true; // not on the desktop
}

class PitchTool : public juce::Component, public juce::Timer // public: snapshots and tests drive timerCallback()
{
public:
    explicit PitchTool (AppController& c) : controller (c) { in.fill (0.0f); out.fill (0.0f); }

    void visibilityChanged() override
    {
        if (isVisible()) startTimerHz (30);
        else stopTimer();
    }

    void timerCallback() override
    {
        if (! shownInTree (*this)) return; // the analysis stops after ~1 s without polls
        const auto r = controller.pollPitch();
        head = (head + 1) % kHistory;
        in[size_t (head)] = r.inputHz;
        out[size_t (head)] = r.outputHz;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        auto r = getLocalBounds();

        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontL, true));
        auto title = r.removeFromTop (Theme::space5);
        g.drawText (ja ("声の高さ"), title, juce::Justification::centredLeft);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawText (ja ("このページを開いているあいだだけ測ります"), title, juce::Justification::centredRight);
        r.removeFromTop (Theme::space2);

        auto readouts = r.removeFromTop (Theme::space5 * 2 + Theme::space2);
        const int half = readouts.getWidth() / 2;
        drawReadout (g, readouts.removeFromLeft (half).withTrimmedRight (Theme::space2), ja ("いまの声"), in[size_t (head)], p.textSub);
        drawReadout (g, readouts.withTrimmedLeft (Theme::space2), ja ("加工したあと"), out[size_t (head)], p.accent);
        r.removeFromTop (Theme::space3);

        auto bottom = r.removeFromBottom (Theme::space4);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawText (ja ("10 秒前"), bottom, juce::Justification::centredLeft);
        g.drawText (ja ("いま"), bottom, juce::Justification::centredRight);
        g.drawText (ja ("帯は目安です。声の高さは人それぞれです。"), bottom, juce::Justification::centred);
        drawGraph (g, r.withTrimmedBottom (Theme::space1));
    }

private:
    static float yFor (float hz, juce::Rectangle<float> a)
    {
        const float t = std::log (juce::jlimit (kLowHz, kHighHz, hz) / kLowHz) / std::log (kHighHz / kLowHz);
        return a.getBottom() - t * a.getHeight();
    }

    void drawReadout (juce::Graphics& g, juce::Rectangle<int> a, const juce::String& label, float hz, juce::Colour lineColour) const
    {
        const auto& p = Theme::colours();
        g.setColour (p.raised);
        g.fillRoundedRectangle (a.toFloat(), Theme::radiusM);
        a.reduce (Theme::space3, Theme::space2);
        auto top = a.removeFromTop (Theme::space4 - Theme::space1);
        g.setColour (lineColour);
        g.fillRoundedRectangle (top.removeFromLeft (Theme::space3).withSizeKeepingCentre (Theme::space3, 3).toFloat(), 1.5f);
        top.removeFromLeft (Theme::space2);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawText (label, top, juce::Justification::centredLeft);

        if (hz > 0.0f)
        {
            const auto value = juce::String (juce::roundToInt (hz)) + " Hz";
            g.setColour (p.text);
            g.setFont (Theme::mono (Theme::fontXL, true));
            g.drawText (value, a, juce::Justification::centredLeft);
            const int w = mainui::textWidth (Theme::mono (Theme::fontXL, true), value);
            g.setColour (p.textSub);
            g.setFont (Theme::ui (Theme::fontM, true));
            g.drawText (noteNameForHz (hz), a.withTrimmedLeft (w + Theme::space2), juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (p.textSub);
            g.setFont (Theme::ui (Theme::fontS));
            g.drawText (ja ("声が聞こえていません"), a, juce::Justification::centredLeft);
        }
    }

    void drawGraph (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const auto& p = Theme::colours();
        g.setColour (p.bg);
        g.fillRoundedRectangle (area.toFloat(), Theme::radiusM);
        g.setColour (p.border);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), Theme::radiusM, 1.0f);
        const auto plot = area.reduced (Theme::space2).withTrimmedLeft (Theme::space5).toFloat();

        // guide bands (目安): overlapping on 165..180 Hz
        auto band = [&] (float lo, float hi, juce::Colour c, const juce::String& text, bool labelAtTop)
        {
            const float y1 = yFor (hi, plot), y2 = yFor (lo, plot);
            g.setColour (c.withAlpha (0.14f));
            g.fillRect (juce::Rectangle<float> (plot.getX(), y1, plot.getWidth(), y2 - y1));
            g.setColour (p.textSub);
            g.setFont (Theme::ui (Theme::fontXS));
            const auto row = juce::Rectangle<float> (plot.getX(), labelAtTop ? y1 : y2 - 16.0f, plot.getWidth() - Theme::space1, 16.0f);
            g.drawText (text, row, juce::Justification::centredRight);
        };
        band (85.0f, 180.0f, p.accent, ja ("低めの声 85〜180 Hz（目安）"), false);
        band (165.0f, 255.0f, p.warn, ja ("高めの声 165〜255 Hz（目安）"), true);

        // grid
        g.setFont (Theme::ui (Theme::fontXS));
        for (float hz : { 100.0f, 200.0f, 300.0f, 500.0f })
        {
            const float y = yFor (hz, plot);
            g.setColour (p.divider);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (p.textSub);
            g.drawText (juce::String (int (hz)), juce::Rectangle<float> (float (area.getX()), y - 8.0f, Theme::space5 + 2.0f, 16.0f),
                        juce::Justification::centredRight);
        }

        // the two series, newest at the right; gaps where there was no pitch
        auto series = [&] (const std::array<float, kHistory>& v, juce::Colour c, float width)
        {
            juce::Path path;
            bool down = false;
            for (int i = 0; i < kHistory; ++i)
            {
                const float hz = v[size_t ((head + 1 + i) % kHistory)];
                const float x = plot.getX() + plot.getWidth() * float (i) / float (kHistory - 1);
                if (hz <= 0.0f) { down = false; continue; }
                const float y = yFor (hz, plot);
                if (down) path.lineTo (x, y);
                else path.startNewSubPath (x, y);
                down = true;
            }
            g.setColour (c);
            g.strokePath (path, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        series (in, p.textSub, 1.5f);
        series (out, p.accent, 2.0f);
        g.restoreState();
    }

    AppController& controller;
    std::array<float, kHistory> in {}, out {};
    int head = 0;
};
} // namespace

std::unique_ptr<juce::Component> makePitchTool (AppController& c, Navigator&) { return std::make_unique<PitchTool> (c); }
} // namespace koe::ui

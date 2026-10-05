// 声の見える化 (INTERFACES.md §12.3). Owner: wave10/viz.

#include "UI/SpectrumView.h"

#include "UI/Theme.h"
#include "UI/Widgets.h"
#include "UI/main/Common.h"

#include <cmath>

namespace koe::ui
{
namespace
{
/** Visible up the tree and the window not minimised. Unlike isShowing() this also holds offscreen (snapshots, tests). */
bool shownInTree (const juce::Component& c)
{
    for (auto* p = &c; p->getParentComponent() != nullptr; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    auto* top = c.getTopLevelComponent();
    if (auto* peer = top->getPeer()) return top->isVisible() && ! peer->isMinimised();
    return true; // not on the desktop
}

float xForHz (float hz, juce::Rectangle<float> plot)
{
    return plot.getX() + plot.getWidth() * std::log (hz / kSpectrumLowHz) / std::log (kSpectrumHighHz / kSpectrumLowHz);
}

float yForDb (float db, float minDb, float maxDb, juce::Rectangle<float> plot)
{
    const float t = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    return plot.getBottom() - t * plot.getHeight();
}

void strokeDashed (juce::Graphics& g, const juce::Path& path, float width)
{
    const float dashes[] = { 4.0f, 3.0f };
    juce::Path dashed;
    juce::PathStrokeType (width).createDashedStroke (dashed, path, dashes, 2);
    g.fillPath (dashed);
}
} // namespace

SpectrumView::SpectrumView (AppController& c, bool isCompact)
    : controller (c), compact (isCompact),
      minDb (isCompact ? -80.0f : -90.0f), maxDb (isCompact ? -10.0f : 0.0f) // the strip: a voice fills more of its low height
{
    in.fill (SpectrumAnalyser::kFloorDb);
    out.fill (SpectrumAnalyser::kFloorDb);
    setTitle (ja ("声の見える化"));
    setDescription (ja ("加工前と加工後の声の周波数のグラフ。点線が加工前、塗りのある線が加工後です。"));
    setInterceptsMouseClicks (false, false);
}

void SpectrumView::visibilityChanged()
{
    if (isVisible())
    {
        startTimerHz (30);
        poll(); // the first picture right away
    }
    else
    {
        stopTimer();
    }
}

void SpectrumView::timerCallback()
{
    if (! shownInTree (*this)) return; // the analysis stops after ~1 s without polls
    poll();
    repaint();
}

void SpectrumView::poll()
{
    data = controller.pollSpectrum (in, out);
    running = controller.getStatus().running;
}

juce::String SpectrumView::statusText() const
{
    if (data) return {};
    return running ? ja ("声を待っています") : ja ("入力デバイスが動いていません");
}

void SpectrumView::drawLegend (juce::Graphics& g, juce::Rectangle<int> row) const
{
    const auto& p = Theme::colours();
    const auto font = Theme::ui (Theme::fontXS);
    const auto before = ja ("加工前"), after = ja ("加工後");
    constexpr int swatchW = Theme::space3, gap = Theme::space1;
    const int afterW = swatchW + gap + mainui::textWidth (font, after);
    const int beforeW = swatchW + gap + mainui::textWidth (font, before);
    auto item = [&] (juce::Rectangle<int> a, const juce::String& text, bool processed)
    {
        const auto sw = a.removeFromLeft (swatchW).toFloat();
        const float y = sw.getCentreY();
        juce::Path line;
        line.startNewSubPath (sw.getX(), y);
        line.lineTo (sw.getRight(), y);
        if (processed)
        {
            g.setColour (p.accent.withAlpha (0.22f));
            g.fillRect (sw.withTop (y).withHeight (5.0f));
            g.setColour (p.accent);
            g.strokePath (line, juce::PathStrokeType (2.0f));
        }
        else
        {
            g.setColour (p.textSub);
            strokeDashed (g, line, 1.5f);
        }
        a.removeFromLeft (gap);
        g.setColour (p.textSub);
        g.setFont (font);
        g.drawText (text, a, juce::Justification::centredLeft, false);
    };
    item (row.removeFromRight (afterW), after, true);
    row.removeFromRight (Theme::space2);
    item (row.removeFromRight (beforeW), before, false);
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto& p = Theme::colours();
    const auto box = getLocalBounds();
    g.setColour (p.bg);
    g.fillRoundedRectangle (box.toFloat(), Theme::radiusM);
    g.setColour (p.border);
    g.drawRoundedRectangle (box.toFloat().reduced (0.5f), Theme::radiusM, 1.0f);

    constexpr int labelH = Theme::space3; // a row of fontXS
    auto inner = box.reduced (compact ? Theme::space2 : Theme::space3, compact ? Theme::space1 : Theme::space2);
    auto top = inner.removeFromTop (labelH);
    g.setFont (Theme::ui (Theme::fontXS));
    g.setColour (p.textSub);
    if (compact)
    {
        g.drawText (ja ("声の見える化"), top, juce::Justification::centredLeft, false);
    }
    else
    {
        inner.removeFromTop (Theme::space1);
        inner.removeFromBottom (labelH); // the frequency labels
        g.drawText ("dB", top.withWidth (Theme::space5), juce::Justification::centredRight, false);
        inner.removeFromLeft (Theme::space5 + Theme::space1); // the dB labels
    }
    drawLegend (g, top);
    const auto plot = inner.toFloat();

    // ---- the scale: 100 / 1k / 10k (finer lines between in the big view), dB lines ----
    struct Mark { float hz; const char* label; const char* labelCompact; };
    static constexpr Mark majors[] = { { 100.0f, "100 Hz", "100" }, { 1000.0f, "1 kHz", "1k" }, { 10000.0f, "10 kHz", "10k" } };
    if (! compact)
    {
        g.setColour (p.divider.withMultipliedAlpha (0.5f));
        for (float hz : { 200.0f, 500.0f, 2000.0f, 5000.0f })
            g.drawVerticalLine (juce::roundToInt (xForHz (hz, plot)), plot.getY(), plot.getBottom());
    }
    for (auto& m : majors)
    {
        const float x = xForHz (m.hz, plot);
        g.setColour (p.divider);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        if (! compact)
        {
            g.setColour (p.textSub);
            g.drawText (m.label, juce::Rectangle<float> (x - float (Theme::space5), plot.getBottom(), float (Theme::space5 * 2), float (labelH)),
                        juce::Justification::centred, false);
        }
    }
    const float step = compact ? 30.0f : 20.0f;
    for (float db = std::floor ((maxDb - 1.0f) / step) * step; db > minDb; db -= step) // multiples of step, none on the top edge
    {
        const float y = yForDb (db, minDb, maxDb, plot);
        g.setColour (p.divider);
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        if (! compact)
        {
            g.setColour (p.textSub);
            g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (float (box.getX() + Theme::space3), y - float (labelH) / 2.0f,
                                                                              float (Theme::space5), float (labelH)),
                        juce::Justification::centredRight, false);
        }
    }
    // the strip has no room under the graph: its frequency labels go inside, over the curves (drawn last)
    auto compactLabels = [&]
    {
        if (! compact) return;
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        for (auto& m : majors)
            g.drawText (m.labelCompact, juce::Rectangle<float> (xForHz (m.hz, plot) + 3.0f, plot.getBottom() - float (labelH), float (Theme::space5), float (labelH)),
                        juce::Justification::centredLeft, false);
    };

    if (! data)
    {
        compactLabels();
        g.setColour (p.textSub);
        g.setFont (Theme::ui (compact ? Theme::fontXS : Theme::fontS));
        g.drawText (statusText(), plot.toNearestInt(), juce::Justification::centred, false);
        return;
    }

    // ---- the two curves: 加工後 filled underneath, 加工前 dashed on top so it shows where they meet ----
    auto curve = [this, &plot] (const std::array<float, kSpectrumBands>& v)
    {
        juce::Path path;
        for (int b = 0; b < kSpectrumBands; ++b)
        {
            const float x = plot.getX() + plot.getWidth() * (float (b) + 0.5f) / float (kSpectrumBands);
            const float y = yForDb (v[size_t (b)], minDb, maxDb, plot);
            if (b == 0) path.startNewSubPath (x, y);
            else path.lineTo (x, y);
        }
        return path;
    };
    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    const auto processed = curve (out);
    auto fill = processed;
    fill.lineTo (plot.getX() + plot.getWidth() * (float (kSpectrumBands) - 0.5f) / float (kSpectrumBands), plot.getBottom());
    fill.lineTo (plot.getX() + plot.getWidth() * 0.5f / float (kSpectrumBands), plot.getBottom());
    fill.closeSubPath();
    g.setColour (p.accent.withAlpha (0.18f));
    g.fillPath (fill);
    g.setColour (p.accent);
    g.strokePath (processed, juce::PathStrokeType (compact ? 1.5f : 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (p.textSub);
    strokeDashed (g, curve (in), compact ? 1.0f : 1.25f);
    g.restoreState();
    compactLabels();
}

std::unique_ptr<juce::Component> makeSpectrumStrip (AppController& c)
{
    auto view = std::make_unique<SpectrumView> (c, true);
    view->setComponentID ("slotDetail.spectrum");
    return view;
}
} // namespace koe::ui

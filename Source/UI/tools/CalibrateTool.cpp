// 音量合わせ tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/analysis.
// Records 10 s of the user's voice, measures every built-in preset against そのまま on it, and offers 元に戻す.

#include "UI/main/ToolsView.h"

#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <cmath>

namespace koe::ui
{
namespace
{
using Phase = AppController::CalibrationState::Phase;

/** Visible up the tree and the window not minimised. Unlike isShowing() this also holds offscreen (snapshots, tests). */
bool shownInTree (const juce::Component& c)
{
    for (auto* p = &c; p->getParentComponent() != nullptr; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    auto* top = c.getTopLevelComponent();
    if (auto* peer = top->getPeer()) return top->isVisible() && ! peer->isMinimised();
    return true; // not on the desktop
}

class CalibrateTool : public juce::Component, public juce::Timer // public: snapshots and tests drive timerCallback()
{
public:
    explicit CalibrateTool (AppController& c) : controller (c)
    {
        for (auto* b : { &start, &cancel, &undo }) addChildComponent (*b);
        addChildComponent (meter);
        meter.setShowScale (false);
        start.setComponentID ("calibrate.start");
        cancel.setComponentID ("calibrate.cancel");
        undo.setComponentID ("calibrate.undo");
        start.onClick = [this]
        {
            refusal = {};
            if (! controller.startCalibration (refusal)) repaint();
            refresh (true);
        };
        cancel.onClick = [this] { controller.cancelCalibration(); refresh (true); };
        undo.onClick = [this] { controller.clearCalibration(); refresh (true); };
        refresh (true);
    }

    void visibilityChanged() override
    {
        if (isVisible()) { startTimerHz (30); refresh (true); }
        else stopTimer();
    }

    void timerCallback() override
    {
        if (! shownInTree (*this)) return;
        refresh (false);
        if (state.phase == Phase::recording)
        {
            const auto m = controller.pollMeters();
            meter.setLevel (m.inputDb, m.inputClip);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (Theme::space5 + Theme::space2 + descriptionH + Theme::space3);
        statusBox = r.removeFromTop (statusH);
        r.removeFromTop (Theme::space3);
        footer = r.removeFromTop (Theme::buttonH);

        auto inner = statusBox.reduced (Theme::space3);
        auto buttonArea = inner.removeFromRight (juce::jmax (start.preferredWidth(), cancel.preferredWidth()) + Theme::space2);
        for (auto* b : { &start, &cancel })
            b->setBounds (buttonArea.withSizeKeepingCentre (juce::jmax (start.preferredWidth(), cancel.preferredWidth()), Theme::buttonH)
                              .withRightX (buttonArea.getRight()));
        inner.removeFromRight (Theme::space3);
        statusText = inner;
        barArea = inner.removeFromBottom (Theme::space3).withSizeKeepingCentre (inner.getWidth(), 12);
        meter.setBounds (barArea.withHeight (juce::jmax (12, barArea.getHeight())));
        undo.setBounds (footer.removeFromRight (undo.preferredWidth()).withSizeKeepingCentre (undo.preferredWidth(), Theme::buttonH));
        footer.removeFromRight (Theme::space3);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        auto r = getLocalBounds();
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontL, true));
        g.drawText (ja ("音量合わせ"), r.removeFromTop (Theme::space5), juce::Justification::centredLeft);
        r.removeFromTop (Theme::space2);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (ja ("自分の声で、内蔵プリセットの音量を「そのまま」とそろえます。10 秒ほど、ふだんの声で話してください。"
                              "録った声は測るのに使うだけで、保存しません。"),
                          r.removeFromTop (descriptionH), juce::Justification::topLeft, 3, 1.0f);

        g.setColour (p.raised);
        g.fillRoundedRectangle (statusBox.toFloat(), Theme::radiusM);

        const int total = builtinCount();
        juce::String headline, detail;
        bool detailIsError = false;
        switch (state.phase)
        {
            case Phase::recording:
            {
                const int left = juce::jmax (0, int (std::ceil ((1.0f - state.progress) * kCalibrationSeconds)));
                headline = ja ("話してください… 残り ") + juce::String (left) + ja (" 秒");
                detail = ja ("ふだんの声の大きさで。メーターが動いていれば録れています。");
                break;
            }
            case Phase::analysing:
            {
                const int done = juce::jlimit (0, total, int (std::floor (state.progress * float (total + 1))) - 1);
                headline = ja ("測っています… ") + juce::String (juce::jmax (0, done)) + " / " + juce::String (total);
                detail = ja ("しばらくかかります。そのままほかの操作をしてかまいません。");
                break;
            }
            case Phase::done:
                headline = juce::String (total) + ja (" 本のうち ") + juce::String (state.presetsAdjusted) + ja (" 本の音量を合わせました。");
                detail = ja ("いまのプリセットにも、すぐ効いています。");
                break;
            case Phase::failed:
                headline = ja ("測れませんでした。");
                detail = state.error;
                detailIsError = true;
                break;
            case Phase::idle:
            default:
                headline = ja ("準備ができたら「始める」を押して、話し始めてください。");
                detail = ja ("測るのは内蔵プリセット ") + juce::String (total) + ja (" 本です。");
                break;
        }
        if (refusal.isNotEmpty())
        {
            detail = refusal;
            detailIsError = true;
        }
        auto text = statusText;
        const bool bar = state.phase == Phase::recording || state.phase == Phase::analysing;
        if (bar) text.removeFromBottom (Theme::space3 + Theme::space1);
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontM, true));
        g.drawFittedText (headline, text.removeFromTop (Theme::space4), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (detailIsError ? p.danger : p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (detail, text, juce::Justification::topLeft, 2, 0.9f);

        if (state.phase == Phase::analysing)
        {
            const auto b = barArea.toFloat();
            g.setColour (p.trackOff);
            g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
            g.setColour (p.accent);
            g.fillRoundedRectangle (b.withWidth (juce::jmax (b.getHeight(), b.getWidth() * state.progress)), b.getHeight() * 0.5f);
        }

        // 前回: the last measurement, kept in the settings
        const auto& s = controller.getSettings();
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        juce::String last;
        if (s.calibratedAt.isEmpty())
            last = ja ("まだ音量合わせをしていません（出荷時の音量です）。");
        else
            last = ja ("前回の音量合わせ: ") + juce::Time::fromISO8601 (s.calibratedAt).formatted ("%Y/%m/%d %H:%M") + ja ("（")
                   + juce::String (adjustedInSettings()) + ja (" 本を合わせています）");
        g.drawFittedText (last, footer, juce::Justification::centredLeft, 2, 0.9f);
    }

private:
    static constexpr int descriptionH = 60, statusH = 104;

    int builtinCount() const
    {
        int n = 0;
        for (auto& p : controller.getPresetLibrary().all()) n += p.builtin ? 1 : 0;
        return n;
    }

    int adjustedInSettings() const
    {
        int n = 0;
        for (auto& [id, trim] : controller.getSettings().calibratedTrimDb)
            if (auto* p = controller.getPresetLibrary().find (id.toStdString()); p != nullptr && std::abs (trim - p->outputTrimDb) >= 0.1f) ++n;
        return n;
    }

    void refresh (bool force)
    {
        const auto next = controller.getCalibrationState();
        const bool hasCalibration = controller.getSettings().calibratedAt.isNotEmpty();
        const bool changed = force || next.phase != state.phase || hasCalibration != shownCalibration;
        const bool moving = next.phase == Phase::recording || next.phase == Phase::analysing;
        if (! changed && ! moving) return;
        if (next.phase != state.phase && next.phase != Phase::idle) refusal = {};
        state = next;
        shownCalibration = hasCalibration;
        if (changed)
        {
            start.setButtonText (state.phase == Phase::done || state.phase == Phase::failed ? ja ("もう一度") : ja ("始める"));
            start.setVisible (! moving);
            cancel.setVisible (moving);
            meter.setVisible (state.phase == Phase::recording);
            undo.setVisible (hasCalibration && ! moving);
            resized();
        }
        repaint();
    }

    AppController& controller;
    PillButton start { ja ("始める"), PillButton::Style::primary };
    PillButton cancel { ja ("やめる"), PillButton::Style::outline };
    PillButton undo { ja ("元に戻す"), PillButton::Style::outline };
    LevelMeter meter { false };
    AppController::CalibrationState state;
    bool shownCalibration = false;
    juce::String refusal;
    juce::Rectangle<int> statusBox, statusText, barArea, footer;
};
} // namespace

std::unique_ptr<juce::Component> makeCalibrateTool (AppController& c, Navigator&) { return std::make_unique<CalibrateTool> (c); }
} // namespace koe::ui

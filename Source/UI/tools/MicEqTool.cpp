// マイク補正 tool (INTERFACES.md §11.3). Owner: wave9/voice.
// Records 10 s of the user's voice, shows the 14 band corrections it worked out, ON/OFF and 消す.

#include "UI/main/ToolsView.h"

#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <cmath>

namespace koe::ui
{
namespace
{
using Phase = AppController::MicEqState::Phase;

/** Visible up the tree and the window not minimised. Unlike isShowing() this also holds offscreen (snapshots, tests). */
bool shownInTree (const juce::Component& c)
{
    for (auto* p = &c; p->getParentComponent() != nullptr; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    auto* top = c.getTopLevelComponent();
    if (auto* peer = top->getPeer()) return top->isVisible() && ! peer->isMinimised();
    return true; // not on the desktop
}

juce::String bandLabel (float hz)
{
    if (hz < 1000.0f) return juce::String (juce::roundToInt (hz));
    const float k = hz / 1000.0f;
    return std::abs (k - std::round (k)) < 0.05f ? juce::String (juce::roundToInt (k)) + "k" : juce::String (k, 1) + "k";
}

class MicEqTool : public juce::Component, public juce::Timer // public: snapshots and tests drive timerCallback()
{
public:
    explicit MicEqTool (AppController& c) : controller (c)
    {
        for (auto* b : { &start, &cancel, &clear }) addChildComponent (*b);
        addChildComponent (meter);
        addChildComponent (toggle);
        meter.setShowScale (false);
        start.setComponentID ("miceq.start");
        cancel.setComponentID ("miceq.cancel");
        clear.setComponentID ("miceq.clear");
        toggle.setComponentID ("miceq.toggle");
        toggle.setTitle (ja ("マイク補正 ON/OFF"));
        clear.setTooltip (ja ("測った結果を消して、補正を外します。"));
        start.onClick = [this]
        {
            refusal = {};
            if (! controller.startMicEqMeasure (refusal)) repaint();
            refresh (true);
        };
        cancel.onClick = [this] { controller.cancelMicEqMeasure(); refresh (true); };
        clear.onClick = [this] { controller.clearMicEq(); refresh (true); };
        toggle.onClick = [this] { controller.setMicEqOn (toggle.getToggleState()); refresh (true); };
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
        r.removeFromTop (Theme::space5 + Theme::space2 + descriptionH + Theme::space2);
        statusBox = r.removeFromTop (statusH);
        r.removeFromTop (Theme::space2);
        footer = r.removeFromBottom (Theme::buttonH);
        r.removeFromBottom (Theme::space2);
        chart = r;

        auto inner = statusBox.reduced (Theme::space3, Theme::space2);
        const int bw = juce::jmax (start.preferredWidth(), cancel.preferredWidth());
        auto buttonArea = inner.removeFromRight (bw);
        for (auto* b : { &start, &cancel })
            b->setBounds (buttonArea.withSizeKeepingCentre (bw, Theme::buttonH));
        inner.removeFromRight (Theme::space3);
        statusText = inner;
        barArea = inner.removeFromBottom (12);
        meter.setBounds (barArea);

        auto f = footer;
        toggle.setBounds (f.withWidth (ToggleSwitch::preferredWidth()).withSizeKeepingCentre (ToggleSwitch::preferredWidth(), Theme::touchMin));
        if (toggle.isVisible()) f.removeFromLeft (ToggleSwitch::preferredWidth() + Theme::space3);
        clear.setBounds (f.removeFromRight (clear.preferredWidth()).withSizeKeepingCentre (clear.preferredWidth(), Theme::buttonH));
        f.removeFromRight (Theme::space3);
        footerText = f;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        auto r = getLocalBounds();
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontL, true));
        g.drawText (ja ("マイク補正"), r.removeFromTop (Theme::space5), juce::Justification::centredLeft);
        r.removeFromTop (Theme::space2);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (ja ("10 秒ほど、ふだんの声で話してください。マイクのこもりや刺さる高音を整えます。"
                              "録った声は測るのに使うだけで、保存しません。"),
                          r.removeFromTop (descriptionH), juce::Justification::topLeft, 3, 1.0f);

        g.setColour (p.raised);
        g.fillRoundedRectangle (statusBox.toFloat(), Theme::radiusM);
        juce::String headline, detail;
        bool detailIsError = false;
        switch (state.phase)
        {
            case Phase::recording:
            {
                const int left = juce::jmax (0, int (std::ceil ((1.0f - state.progress) * kMicEqSeconds)));
                headline = ja ("話してください… 残り ") + juce::String (left) + ja (" 秒");
                detail = ja ("ふだんの声の大きさで。");
                break;
            }
            case Phase::analysing:
                headline = ja ("声の特徴を調べています…");
                break;
            case Phase::done:
                headline = ja ("測り終わりました。補正をかけています。");
                detail = ja ("下の棒が、帯ごとの補正の量です。");
                break;
            case Phase::failed:
                headline = ja ("測れませんでした。");
                detail = state.error;
                detailIsError = true;
                break;
            case Phase::idle:
            default:
                headline = measured() ? ja ("もう一度測るときは「測り直す」を押してください。")
                                      : ja ("準備ができたら「測る」を押して、話し始めてください。");
                break;
        }
        if (refusal.isNotEmpty())
        {
            detail = refusal;
            detailIsError = true;
        }
        auto text = statusText;
        if (state.phase == Phase::recording) text.removeFromBottom (12 + Theme::space1);
        if (detail.isEmpty() && state.phase != Phase::recording) text = text.withSizeKeepingCentre (text.getWidth(), Theme::space4);
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontM, true));
        g.drawFittedText (headline, text.removeFromTop (Theme::space4), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (detailIsError ? p.danger : p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (detail, text, juce::Justification::topLeft, 2, 0.9f);

        paintChart (g);

        const auto& s = controller.getSettings();
        juce::String last = s.micEqAt.isEmpty() ? ja ("まだ測っていません。")
                                                : ja ("前回の測定: ") + juce::Time::fromISO8601 (s.micEqAt).formatted ("%Y/%m/%d %H:%M");
        drawText (g, last, footerText, Theme::fontS, p.textSub);
    }

    /** Bars of the stored gains (tests: the drawn values). */
    std::vector<float> shownGains() const { return controller.getSettings().micEqGainsDb; }

private:
    static constexpr int descriptionH = 44, statusH = 72;

    bool measured() const { return int (controller.getSettings().micEqGainsDb.size()) == kMicEqBands; }

    void paintChart (juce::Graphics& g)
    {
        const auto& p = Theme::colours();
        auto area = chart;
        const auto gains = shownGains();
        if (int (gains.size()) != kMicEqBands)
        {
            g.setColour (p.border);
            g.drawRoundedRectangle (area.toFloat().reduced (0.5f), Theme::radiusM, 1.0f);
            drawText (g, ja ("測ると、ここに帯ごとの補正（± ") + juce::String (int (kMicEqMaxDb)) + ja (" dB まで）が出ます。"), area,
                      Theme::fontS, p.textSub, juce::Justification::centred);
            return;
        }
        const bool on = controller.getSettings().micEqOn;
        auto labels = area.removeFromBottom (Theme::space3 + 2);
        auto scale = area.removeFromLeft (Theme::space5 + Theme::space2);
        const float top = float (area.getY() + Theme::space1), bottom = float (area.getBottom() - Theme::space1);
        const float mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
        // scale: +6 / 0 / -6 dB
        for (const float db : { kMicEqMaxDb, 0.0f, -kMicEqMaxDb })
        {
            const float y = mid - db / kMicEqMaxDb * half;
            g.setColour (db == 0.0f ? p.border : p.divider);
            g.drawHorizontalLine (int (y), float (area.getX()), float (area.getRight()));
            drawText (g, (db > 0 ? "+" : "") + juce::String (int (db)) + " dB", juce::Rectangle<int> (scale.getX(), int (y) - 8, scale.getWidth() - Theme::space1, 16),
                      Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
        }
        const float slot = float (area.getWidth()) / float (kMicEqBands);
        for (int b = 0; b < kMicEqBands; ++b)
        {
            const float v = juce::jlimit (-kMicEqMaxDb, kMicEqMaxDb, gains[size_t (b)]);
            const float x = float (area.getX()) + slot * float (b);
            const float y = mid - v / kMicEqMaxDb * half;
            auto bar = juce::Rectangle<float>::leftTopRightBottom (x + slot * 0.22f, std::min (y, mid), x + slot * 0.78f, std::max (y, mid));
            if (bar.getHeight() < 2.0f) bar = bar.withSizeKeepingCentre (bar.getWidth(), 2.0f);
            g.setColour (on ? p.accent : p.textSub);
            g.fillRoundedRectangle (bar, 2.0f);
            drawText (g, bandLabel (kMicEqBandHz[b]), juce::Rectangle<int> (int (x), labels.getY(), int (slot), labels.getHeight()), Theme::fontXS,
                      p.textSub, juce::Justification::centred, false, true);
        }
        if (! on)
            drawText (g, ja ("補正は OFF です（声はそのまま）"), area.removeFromTop (Theme::space4).withTrimmedLeft (Theme::space2), Theme::fontXS, p.textSub,
                      juce::Justification::topLeft);
    }

    void refresh (bool force)
    {
        const auto next = controller.getMicEqState();
        const auto& s = controller.getSettings();
        const bool hasResult = measured();
        const bool changed = force || next.phase != state.phase || hasResult != shownResult || s.micEqOn != shownOn;
        const bool moving = next.phase == Phase::recording || next.phase == Phase::analysing;
        if (! changed && ! moving) return;
        if (next.phase != state.phase && next.phase != Phase::idle) refusal = {};
        state = next;
        shownResult = hasResult;
        shownOn = s.micEqOn;
        if (changed)
        {
            start.setButtonText (hasResult || state.phase == Phase::failed ? ja ("測り直す") : ja ("測る"));
            start.setVisible (! moving);
            cancel.setVisible (moving);
            meter.setVisible (state.phase == Phase::recording);
            toggle.setVisible (hasResult);
            toggle.setToggleState (s.micEqOn, juce::dontSendNotification);
            clear.setVisible (hasResult && ! moving);
            resized();
        }
        repaint();
    }

    AppController& controller;
    PillButton start { ja ("測る"), PillButton::Style::primary };
    PillButton cancel { ja ("やめる"), PillButton::Style::outline };
    PillButton clear { ja ("消す"), PillButton::Style::outline };
    ToggleSwitch toggle;
    LevelMeter meter { false };
    AppController::MicEqState state;
    bool shownResult = false, shownOn = false;
    juce::String refusal;
    juce::Rectangle<int> statusBox, statusText, barArea, chart, footer, footerText;
};
} // namespace

std::unique_ptr<juce::Component> makeMicEqTool (AppController& c, Navigator&) { return std::make_unique<MicEqTool> (c); }
} // namespace koe::ui

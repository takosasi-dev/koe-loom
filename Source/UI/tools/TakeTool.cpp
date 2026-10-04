// 試し録り tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/capture.
// Record a few seconds of the raw microphone, then loop it through the whole voice path while trying presets and knobs.
// The virtual mic stays silent meanwhile; the take is heard on the monitor only.

#include "UI/main/ToolsView.h"

#include "Core/Constants.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

namespace koe::ui
{
namespace
{
juce::String seconds (float s) { return juce::String (s, 1) + ja (" 秒"); }

class TakeTool : public juce::Component, private juce::Timer
{
public:
    TakeTool (AppController& c, Navigator& n) : ctl (c), nav (n)
    {
        recordButton.setComponentID ("take.record");
        playButton.setComponentID ("take.play");
        clearButton.setComponentID ("take.clear");
        recordButton.onClick = [this]
        {
            if (ctl.getTestTakeState() == AppController::TakeState::recording) ctl.stopTestTake();
            else if (juce::String why; ! ctl.startTestTake (why)) nav.showToast (why);
            refresh();
        };
        playButton.onClick = [this]
        {
            if (ctl.getTestTakeState() == AppController::TakeState::playing) ctl.stopTestTake();
            else if (juce::String why; ! ctl.playTestTake (why)) nav.showToast (why);
            refresh();
        };
        clearButton.onClick = [this] { ctl.clearTestTake(); refresh(); };
        clearButton.setTooltip (ja ("試し録りを消します（保存はしていません）"));
        // wave9/share: the take through the working preset into a WAV (INTERFACES.md §11.3)
        renderButton.setComponentID ("take.render");
        renderButton.setTooltip (ja ("いまのプリセットで加工した試し録りを WAV に保存します（録音の道具からサウンドボードに入れられます）"));
        renderButton.onClick = [this]
        {
            juce::String why;
            const auto f = ctl.renderTestTakeToFile (why);
            nav.showToast (f != juce::File() ? ja ("「") + f.getFileName() + ja ("」に保存しました") : why);
            refresh();
        };
        for (auto* b : { &recordButton, &playButton, &clearButton, &renderButton }) addAndMakeVisible (*b);
        refresh();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        drawText (g, ja ("試し録り"), titleRow, Theme::fontL, p.text, juce::Justification::centredLeft, true);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (ja ("数秒録って、プリセットやつまみを変えながら、加工した声をくり返し聞けます。"), descRow, juce::Justification::topLeft, 2);
        drawIcon (g, Icon::headphones, noteRow.withWidth (noteRow.getHeight()).toFloat().reduced (1.0f), p.accent, 1.6f);
        drawText (g, ja ("相手には送りません。モニターで聞きます"), noteRow.withTrimmedLeft (noteRow.getHeight() + Theme::space2), Theme::fontS, p.text);

        // the take: length against the 15 s maximum, the playhead while it loops
        const auto state = ctl.getTestTakeState();
        const float len = ctl.getTestTakeSeconds();
        const auto track = barRow.toFloat();
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (track, Theme::radiusS);
        if (len > 0.0f)
        {
            const auto filled = track.withWidth (track.getWidth() * juce::jlimit (0.0f, 1.0f, len / kTestTakeMaxSeconds));
            g.setColour (state == AppController::TakeState::recording ? p.danger : p.accent);
            g.fillRoundedRectangle (filled, Theme::radiusS);
            if (state == AppController::TakeState::playing)
            {
                const float x = filled.getX() + filled.getWidth() * juce::jlimit (0.0f, 1.0f, ctl.getTestTakePosition() / len);
                g.setColour (p.text);
                g.fillRect (juce::Rectangle<float> (x - 1.5f, track.getY() - 3.0f, 3.0f, track.getHeight() + 6.0f));
            }
        }
        juce::String left;
        switch (state)
        {
            case AppController::TakeState::empty:     left = ja ("まだ録っていません"); break;
            case AppController::TakeState::recording: left = ja ("録音中  ") + seconds (len); break;
            case AppController::TakeState::ready:     left = ja ("録った長さ  ") + seconds (len); break;
            case AppController::TakeState::playing:   left = ja ("再生中  ") + juce::String (ctl.getTestTakePosition(), 1) + " / " + seconds (len); break;
        }
        drawText (g, left, barText, Theme::fontS, state == AppController::TakeState::recording ? p.danger : p.text);
        drawText (g, ja ("最大 ") + juce::String (int (kTestTakeMaxSeconds)) + ja (" 秒"), barText, Theme::fontXS, p.textSub, juce::Justification::centredRight);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawFittedText (ja ("録った声はこのアプリを閉じると消えます（保存はしません）。再生中はモニターを自動で ON にし、止めると元に戻します。"),
                          hintRow, juce::Justification::topLeft, 3);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleRow = r.removeFromTop (Theme::space5);
        descRow = r.removeFromTop (Theme::space5 + Theme::space2);
        r.removeFromTop (Theme::space1);
        noteRow = r.removeFromTop (Theme::space4);
        r.removeFromTop (Theme::space3);
        auto buttons = r.removeFromTop (Theme::buttonH);
        const int w = std::max ({ recordButton.preferredWidth(), playButton.preferredWidth(), 112 });
        recordButton.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (Theme::space2);
        playButton.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (Theme::space2);
        clearButton.setBounds (buttons.removeFromLeft (clearButton.preferredWidth()));
        buttons.removeFromLeft (Theme::space2);
        renderButton.setBounds (buttons.removeFromLeft (std::min (renderButton.preferredWidth(), buttons.getWidth())));
        r.removeFromTop (Theme::space4);
        barText = r.removeFromTop (Theme::space4);
        r.removeFromTop (Theme::space1);
        barRow = r.removeFromTop (Theme::space3 - Theme::space1);
        r.removeFromTop (Theme::space3);
        hintRow = r.removeFromTop (Theme::space5 + Theme::space4);
    }

private:
    void timerCallback() override { if (isShowing()) refresh(); } // the page itself may be hidden (another tab)
    void visibilityChanged() override
    {
        if (isVisible()) { refresh(); startTimerHz (15); }
        else stopTimer();
    }

    void refresh()
    {
        const auto state = ctl.getTestTakeState();
        const bool recording = state == AppController::TakeState::recording, playing = state == AppController::TakeState::playing;
        recordButton.setButtonText (recording ? ja ("止める") : ja ("録る"));
        recordButton.setIcon (recording ? Icon::stop : Icon::mic);
        recordButton.setStyle (recording ? PillButton::Style::danger : PillButton::Style::primary);
        recordButton.setEnabled (! playing);
        playButton.setButtonText (playing ? ja ("止める") : ja ("再生"));
        playButton.setIcon (playing ? Icon::stop : Icon::play);
        playButton.setStyle (playing ? PillButton::Style::danger : PillButton::Style::accentOutline);
        playButton.setEnabled (state != AppController::TakeState::empty); // while recording: stops it and plays
        clearButton.setEnabled (state != AppController::TakeState::empty);
        renderButton.setEnabled (state == AppController::TakeState::ready || playing);
        repaint();
    }

    AppController& ctl;
    Navigator& nav;
    PillButton recordButton { ja ("録る"), PillButton::Style::primary, Icon::mic };
    PillButton playButton { ja ("再生"), PillButton::Style::accentOutline, Icon::play };
    PillButton clearButton { ja ("消す"), PillButton::Style::outline, Icon::close };
    PillButton renderButton { ja ("加工して保存"), PillButton::Style::outline, Icon::folder };
    juce::Rectangle<int> titleRow, descRow, noteRow, barText, barRow, hintRow;
};
} // namespace

std::unique_ptr<juce::Component> makeTakeTool (AppController& c, Navigator& n) { return std::make_unique<TakeTool> (c, n); }
} // namespace koe::ui

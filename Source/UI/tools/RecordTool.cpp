// 録音 tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/capture.
// Records what goes to the virtual mic (processed voice, soundboard) into a WAV in paths::recordingsDir().

#include "UI/main/ToolsView.h"

#include "Core/Paths.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

namespace koe::ui
{
namespace
{
juce::String clock (double seconds)
{
    const int s = int (seconds);
    return juce::String::formatted ("%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

class RecordTool : public juce::Component, private juce::Timer
{
public:
    RecordTool (AppController& c, Navigator& n) : ctl (c), nav (n)
    {
        toggleButton.setComponentID ("record.toggle");
        folderButton.setComponentID ("record.folder");
        openButton.setComponentID ("record.open");
        toggleButton.onClick = [this]
        {
            if (ctl.isWavRecording()) ctl.stopWavRecording();
            else if (juce::String why; ! ctl.startWavRecording (why)) nav.showToast (why);
            refresh();
        };
        folderButton.onClick = [this]
        {
            if (const auto f = ctl.getLastWavFile(); f.existsAsFile()) f.revealToUser();
            else
            {
                const auto dir = paths::recordingsDir();
                dir.createDirectory();
                dir.startAsProcess();
            }
        };
        openButton.onClick = [this]
        {
            if (const auto f = ctl.getLastWavFile(); f.existsAsFile()) f.startAsProcess(); // the default player
            else nav.showToast (ja ("録音したファイルが見つかりません。"));
        };
        openButton.setTooltip (ja ("Windows の既定のアプリで開きます"));
        for (auto* b : { &toggleButton, &folderButton, &openButton }) addAndMakeVisible (*b);
        refresh();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        drawText (g, ja ("録音"), titleRow, Theme::fontL, p.text, juce::Justification::centredLeft, true);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (ja ("仮想マイクへ出している音（加工した声・サウンドボード）を WAV ファイルに保存します。"), descRow, juce::Justification::topLeft, 2);

        const bool rec = ctl.isWavRecording();
        if (rec)
        {
            const auto dot = timeRow.withWidth (timeRow.getHeight()).toFloat().reduced (timeRow.getHeight() * 0.3f);
            g.setColour (p.danger);
            g.fillEllipse (dot);
        }
        drawText (g, clock (ctl.getWavRecordingSeconds()), timeRow.withTrimmedLeft (rec ? timeRow.getHeight() : 0), Theme::fontXL,
                  rec ? p.text : p.textSub, juce::Justification::centredLeft, true, true);
        drawText (g, rec ? ja ("録音しています") : ja ("止まっています"), stateRow, Theme::fontS, rec ? p.danger : p.textSub);

        const auto hotkey = ctl.getHotkeyText ("recordToggle");
        drawText (g, ja ("ホットキー：") + (hotkey.isNotEmpty() ? hotkey : ja ("未割り当て（設定のホットキーで割り当てられます）")), hotkeyRow, Theme::fontS, p.textSub);

        const auto last = ctl.getLastWavFile();
        drawText (g, ja ("最後のファイル"), lastLabelRow, Theme::fontXS, p.textSub);
        drawText (g, last != juce::File() ? last.getFileName() : ja ("まだありません"), lastRow, Theme::fontS, last != juce::File() ? p.text : p.textSub);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawFittedText (ja ("KoeLoom のデータのフォルダの中の recordings に、モノラル・24 bit で保存します（「フォルダを開く」で開けます）。"), pathRow, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleRow = r.removeFromTop (Theme::space5);
        descRow = r.removeFromTop (Theme::space5 + Theme::space2);
        r.removeFromTop (Theme::space2);
        auto row = r.removeFromTop (Theme::pillH);
        toggleButton.setBounds (row.removeFromLeft (std::max (140, toggleButton.preferredWidth())));
        row.removeFromLeft (Theme::space4);
        timeRow = row.removeFromLeft (220);
        stateRow = row;
        r.removeFromTop (Theme::space2);
        hotkeyRow = r.removeFromTop (Theme::space4);
        r.removeFromTop (Theme::space3);
        lastLabelRow = r.removeFromTop (Theme::space3 + Theme::space1);
        lastRow = r.removeFromTop (Theme::space4);
        r.removeFromTop (Theme::space2);
        auto buttons = r.removeFromTop (Theme::buttonH);
        folderButton.setBounds (buttons.removeFromLeft (folderButton.preferredWidth()));
        buttons.removeFromLeft (Theme::space2);
        openButton.setBounds (buttons.removeFromLeft (openButton.preferredWidth()));
        r.removeFromTop (Theme::space3);
        pathRow = r.removeFromTop (Theme::space5 + Theme::space1);
    }

private:
    void timerCallback() override { if (isShowing()) refresh(); } // the page itself may be hidden (another tab)
    void visibilityChanged() override
    {
        if (isVisible()) { refresh(); startTimerHz (10); }
        else stopTimer();
    }

    void refresh()
    {
        const bool rec = ctl.isWavRecording();
        toggleButton.setButtonText (rec ? ja ("停止") : ja ("録音開始"));
        toggleButton.setIcon (rec ? Icon::stop : Icon::mic);
        toggleButton.setStyle (rec ? PillButton::Style::danger : PillButton::Style::primary);
        openButton.setEnabled (ctl.getLastWavFile() != juce::File());
        repaint();
    }

    AppController& ctl;
    Navigator& nav;
    PillButton toggleButton { ja ("録音開始"), PillButton::Style::primary, Icon::mic };
    PillButton folderButton { ja ("フォルダを開く"), PillButton::Style::outline, Icon::folder };
    PillButton openButton { ja ("再生"), PillButton::Style::outline, Icon::play };
    juce::Rectangle<int> titleRow, descRow, timeRow, stateRow, hotkeyRow, lastLabelRow, lastRow, pathRow;
};
} // namespace

std::unique_ptr<juce::Component> makeRecordTool (AppController& c, Navigator& n) { return std::make_unique<RecordTool> (c, n); }
} // namespace koe::ui

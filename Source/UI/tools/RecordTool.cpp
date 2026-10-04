// 録音 tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/capture.
// Records what goes to the virtual mic (processed voice, soundboard) into a WAV in paths::recordingsDir().

#include "UI/main/ToolsView.h"

#include "Core/Constants.h"
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
        // wave9/share: the last file into the soundboard (INTERFACES.md §11.3)
        soundboardButton.setComponentID ("record.soundboard");
        soundboardButton.setTooltip (ja ("最後のファイルを、サウンドボードの空いている枠に入れます（空きが無ければ枠を選びます）"));
        soundboardButton.onClick = [this] { addToSoundboard(); };
        for (auto* b : { &toggleButton, &folderButton, &openButton, &soundboardButton }) addAndMakeVisible (*b);
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
        buttons.removeFromLeft (Theme::space2);
        soundboardButton.setBounds (buttons.removeFromLeft (std::min (soundboardButton.preferredWidth(), buttons.getWidth())));
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
        soundboardButton.setEnabled (ctl.getLastWavFile() != juce::File() && ! rec);
        if (pendingSlot >= 0) // the soundboard loads on its own thread: tell how it went (60 s / 200 MB / broken file, E-12..E-14)
        {
            const auto st = ctl.getSoundboard().getSlotState (pendingSlot);
            if (st.status == SoundSlotState::Status::ready)
                nav.showToast (ja ("サウンドボードのスロット ") + juce::String (pendingSlot + 1) + ja (" に入れました"));
            else if (st.status == SoundSlotState::Status::error || st.status == SoundSlotState::Status::missing)
                nav.showToast (ja ("サウンドボードに入れられませんでした：") + st.error);
            if (st.status != SoundSlotState::Status::loading) pendingSlot = -1;
        }
        repaint();
    }

    void addToSoundboard()
    {
        const auto f = ctl.getLastWavFile();
        if (! f.existsAsFile())
        {
            nav.showToast (ja ("録音したファイルが見つかりません。"));
            return;
        }
        auto& sb = ctl.getSoundboard();
        for (int i = 0; i < kSoundboardSlots; ++i)
            if (sb.getSlotDef (i).file.isEmpty()) return assignTo (i);
        if (! isShowing()) // a menu needs a window on screen (tests and snapshots never get here with one)
        {
            nav.showToast (ja ("サウンドボードに空いている枠がありません。"));
            return;
        }
        juce::PopupMenu m;
        m.addSectionHeader (ja ("空いている枠がありません。置き換える枠を選んでください"));
        for (int i = 0; i < kSoundboardSlots; ++i)
            m.addItem (i + 1, ja ("スロット ") + juce::String (i + 1) + ja ("：") + juce::File (sb.getSlotDef (i).file).getFileName());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (soundboardButton),
                         [safe = juce::Component::SafePointer<RecordTool> (this)] (int r)
                         {
                             if (safe != nullptr && r > 0) safe->assignTo (r - 1);
                         });
    }

    void assignTo (int slot)
    {
        const auto f = ctl.getLastWavFile();
        if (! f.existsAsFile()) return;
        ctl.getSoundboard().assignFile (slot, f); // async: loading -> ready / error
        ctl.saveSoundboard();
        pendingSlot = slot;
        refresh();
    }

    AppController& ctl;
    Navigator& nav;
    PillButton toggleButton { ja ("録音開始"), PillButton::Style::primary, Icon::mic };
    PillButton folderButton { ja ("フォルダを開く"), PillButton::Style::outline, Icon::folder };
    PillButton openButton { ja ("再生"), PillButton::Style::outline, Icon::play };
    PillButton soundboardButton { ja ("サウンドボードに入れる"), PillButton::Style::outline, Icon::plus };
    int pendingSlot = -1; // wave9/share: waiting for this slot to load
    juce::Rectangle<int> titleRow, descRow, timeRow, stateRow, hotkeyRow, lastLabelRow, lastRow, pathRow;
};
} // namespace

std::unique_ptr<juce::Component> makeRecordTool (AppController& c, Navigator& n) { return std::make_unique<RecordTool> (c, n); }
} // namespace koe::ui

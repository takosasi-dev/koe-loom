// S-02 サウンドボード (docs/mockups/A-S02.dc.html): toolbar, 12 slots (4 x 3), one-time hint (F-13-7).
// The mock's slot card has no room for the per-slot options (F-06-4/5), so pressing a card opens
// "スロット NN の設定" as an overlay. The mock's bottom status bar is MainComponent's (shared with S-01).
#include "UI/Screens.h"
#include "UI/screens/Common.h"

#include "Core/Constants.h"

namespace koe::ui
{
using namespace screens;

namespace
{
constexpr int kCols = 4, kRows = 3;
constexpr int kGap = Theme::space3;                                               // mock 14
constexpr int kToolbarH = Theme::space5 * 2;                                      // mock 64
constexpr int kSlotMaxH = Theme::space5 * 4 + Theme::space4;                      // mock 150
constexpr int kSlotRegularH = Theme::space5 * 3 + Theme::space4;                 // 120: smaller cards use the compact layout
constexpr int kSlotCompactH = Theme::space5 * 2 + Theme::space3;                  // 80: below this the grid scrolls
constexpr int kBadgeRowH = Theme::space4 - Theme::space1;                         // 20
constexpr int kProgressH = Theme::space1;                                         // mock 4

juce::String slotNumber (int i) { return juce::String (i + 1).paddedLeft ('0', 2); }
juce::String soundAction (int i) { return "sound." + juce::String (i + 1); }

/** File name without folders (the path is not used with juce::File: it may come from another PC). */
juce::String baseName (const juce::String& path) { return path.fromLastOccurrenceOf ("\\", false, false).fromLastOccurrenceOf ("/", false, false); }

juce::String retriggerName (int i)
{
    const char* names[] = { "最初から", "無視", "重ねる" };
    return ja (names[juce::jlimit (0, 2, i)]);
}

// =============================================================================================== PlayButton
/** Round play button (mock 40 px). While the slot plays: filled with the accent, a stop square, and pressing stops it. */
class PlayButton : public juce::Button
{
public:
    explicit PlayButton (int s) : juce::Button ({}), slot (s)
    {
        setWantsKeyboardFocus (true);
        updateNames();
    }

    void setPlaying (bool p)
    {
        if (p == playing) return;
        playing = p;
        updateNames();
        repaint();
    }

    bool isPlaying() const { return playing; }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = Theme::colours();
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (playing)
        {
            g.setColour (p.accent);
            g.fillEllipse (r);
        }
        else
        {
            if ((highlighted || down) && isEnabled())
            {
                g.setColour (p.raised);
                g.fillEllipse (r);
            }
            g.setColour (p.border);
            g.drawEllipse (r.reduced (0.75f), Theme::borderWidth);
        }
        const float s = r.getWidth() * 0.4f;
        if (playing) drawIcon (g, Icon::stop, r.withSizeKeepingCentre (s, s), p.onAccent);
        else drawIcon (g, Icon::play, r.withSizeKeepingCentre (s, s).translated (s * 0.08f, 0.0f), isEnabled() ? p.text : p.textSub);
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, r.getHeight() * 0.5f);
    }

private:
    void updateNames()
    {
        const auto verb = playing ? ja ("停止") : ja ("再生");
        setButtonText (verb);
        setTitle (ja ("スロット ") + slotNumber (slot) + ja (" を") + verb);
        setTooltip (getTitle());
    }

    int slot;
    bool playing = false;
};

// =============================================================================================== SlotCard
/** One soundboard slot. Pressing the card opens its settings (or the file chooser when empty). */
class SlotCard : public juce::Button
{
public:
    explicit SlotCard (int i) : juce::Button ({}), index (i), play (i), choose (ja ("ファイルを選ぶ"), PillButton::Style::outline, Icon::folder)
    {
        setWantsKeyboardFocus (true);
        setComponentID ("soundboard.slot." + juce::String (i + 1));
        play.setComponentID ("soundboard.play." + juce::String (i + 1));
        choose.setComponentID ("soundboard.choose." + juce::String (i + 1));
        choose.setFontSize (Theme::fontXS);
        addChildComponent (play);
        addChildComponent (choose);
        play.onClick = [this] { if (onPlay) onPlay(); };
        choose.onClick = [this] { if (onChoose) onChoose(); };
        onClick = [this]
        {
            if (isEmpty()) { if (onChoose) onChoose(); }
            else if (onSettings) onSettings();
        };
    }

    std::function<void()> onPlay, onChoose, onSettings;

    void update (const SoundboardSlotDef& d, const SoundSlotState& s, const juce::String& hotkey)
    {
        def = d;
        state = s;
        hotkeyText = hotkey;
        const bool empty = isEmpty();
        play.setVisible (! empty);
        choose.setVisible (empty);
        play.setPlaying (s.playing);
        play.setEnabled (s.status == SoundSlotState::Status::ready);
        const auto title = ja ("スロット ") + slotNumber (index)
                           + (empty ? ja ("（ファイル未割り当て。押すとファイルを選びます）") : ja ("「") + displayName() + ja ("」（押すと設定を開きます）"));
        setTitle (title);
        setTooltip (title);
        resized();
        repaint();
    }

    /** Timer path while playing: only the progress bar moves. */
    void setPosition (double seconds)
    {
        if (! state.playing || seconds == state.positionSeconds) return;
        state.positionSeconds = seconds;
        repaint();
    }

    bool isPlaying() const { return state.playing; }
    bool isEmpty() const { return def.file.isEmpty(); }
    bool isCompact() const { return getHeight() < kSlotRegularH; }
    juce::String getStatusText() const { return status().text; }

    juce::String displayName() const
    {
        const auto n = baseName (def.file);
        return n.containsChar ('.') ? n.upToLastOccurrenceOf (".", false, false) : n;
    }

    void resized() override
    {
        if (isEmpty())
        {
            const auto e = emptyLayout();
            const int bw = choose.preferredWidth();
            choose.setBounds (e.button.withSizeKeepingCentre (juce::jmin (bw, getWidth() - Theme::space3), Theme::touchMin));
            return;
        }
        if (isCompact())
        {
            auto r = getLocalBounds().reduced (Theme::space2);
            play.setBounds (r.removeFromTop (Theme::touchMin).removeFromRight (Theme::touchMin));
        }
        else
        {
            auto r = getLocalBounds().reduced (Theme::space3, Theme::space3 - Theme::space1);
            r.removeFromTop (kBadgeRowH + Theme::space2);
            play.setBounds (r.removeFromTop (Theme::controlH).removeFromRight (Theme::controlH));
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& p = Theme::colours();
        const auto bounds = getLocalBounds().toFloat();
        if (isEmpty())
        {
            if (highlighted) { g.setColour (p.surface); g.fillRoundedRectangle (bounds, Theme::radiusL); }
            drawDashedRoundedRect (g, bounds, Theme::radiusL, Theme::borderWidth, p.border);
            const auto e = emptyLayout();
            drawText (g, slotNumber (index), e.number, Theme::fontXS, p.textSub, juce::Justification::centred, false, true);
            if (! e.text.isEmpty()) drawText (g, ja ("ファイル未割り当て"), e.text, Theme::fontS, p.textSub, juce::Justification::centred);
            if (hasKeyboardFocus (false)) drawFocusRing (g, bounds.reduced (Theme::space1), Theme::radiusL);
            return;
        }

        const auto st = status();
        const bool problem = st.tone == Tone::danger;
        g.setColour (highlighted ? p.raised : p.surface);
        g.fillRoundedRectangle (bounds, Theme::radiusL);
        const float bw = state.playing ? Theme::focusRingWidth : (problem ? Theme::borderWidth : 1.0f); // mock: 2 px accent while playing
        g.setColour (state.playing ? p.accent : (problem ? p.danger : p.divider));
        g.drawRoundedRectangle (bounds.reduced (bw * 0.5f), Theme::radiusL, bw);

        const auto hk = hotkeyText.isNotEmpty() ? hotkeyText : ja ("未割り当て");
        const Tone hkTone = hotkeyText.isNotEmpty() ? Tone::text : Tone::sub;
        const auto ext = baseName (def.file).fromLastOccurrenceOf (".", false, false).toLowerCase();
        const auto meta = state.status == SoundSlotState::Status::ready ? juce::String (state.lengthSeconds, 1) + ja (" 秒 ・ ") + ext : ext;

        if (isCompact())
        {
            auto r = getLocalBounds().reduced (Theme::space2);
            auto row = r.removeFromTop (Theme::touchMin);
            row.removeFromRight (Theme::touchMin + Theme::space2);
            drawText (g, slotNumber (index), row.removeFromLeft (Theme::space4 - Theme::space1), Theme::fontXS, p.textSub, juce::Justification::centredLeft, false, true);
            row.removeFromLeft (Theme::space1);
            drawText (g, displayName(), row, Theme::fontS, p.text, juce::Justification::centredLeft, true);
            auto statusRow = r.removeFromBottom (kBadgeRowH);
            const int bw2 = juce::jmin (badgeWidth (hk, true), statusRow.getWidth() / 2);
            paintBadge (g, statusRow.removeFromRight (bw2), hk, hkTone, true);
            statusRow.removeFromRight (Theme::space1);
            paintStatus (g, statusRow, st);
            paintProgress (g, r);
        }
        else
        {
            auto r = getLocalBounds().reduced (Theme::space3, Theme::space3 - Theme::space1);
            auto header = r.removeFromTop (kBadgeRowH);
            drawText (g, slotNumber (index), header.removeFromLeft (Theme::space4), Theme::fontXS, p.textSub, juce::Justification::centredLeft, false, true);
            const int bw2 = juce::jmin (badgeWidth (hk, true), header.getWidth());
            paintBadge (g, header.removeFromRight (bw2), hk, hkTone, true);
            r.removeFromTop (Theme::space2);
            auto row = r.removeFromTop (Theme::controlH);
            row.removeFromRight (Theme::controlH + Theme::space3);
            drawText (g, displayName(), row.removeFromTop (Theme::space4 - Theme::space1), Theme::fontM, p.text, juce::Justification::centredLeft, true);
            drawText (g, meta, row, Theme::fontXS, p.textSub, juce::Justification::centredLeft);
            auto statusRow = r.removeFromBottom (Theme::space4);
            const auto more = ja ("設定");
            const int moreW = int (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontXS), more)) + m::iconS;
            auto moreArea = statusRow.removeFromRight (moreW);
            drawText (g, more, moreArea.removeFromLeft (moreW - m::iconS), Theme::fontXS, p.textSub, juce::Justification::centredRight);
            drawIcon (g, Icon::chevronRight, moreArea.toFloat().withSizeKeepingCentre (float (m::iconS) * 0.7f, float (m::iconS) * 0.7f), p.textSub);
            statusRow.removeFromRight (Theme::space2);
            paintStatus (g, statusRow, st);
            paintProgress (g, r);
        }
        if (hasKeyboardFocus (false)) drawFocusRing (g, bounds.reduced (Theme::space1), Theme::radiusL);
    }

private:
    struct StatusInfo { juce::String text; Tone tone; Icon icon; };

    StatusInfo status() const
    {
        using S = SoundSlotState::Status;
        switch (state.status)
        {
            case S::loading: return { ja ("読み込み中…"), Tone::sub, Icon::speaker };
            case S::error: return { state.error.isNotEmpty() ? state.error : ja ("読み込めませんでした"), Tone::danger, Icon::warning };
            case S::missing: return { ja ("見つかりません"), Tone::danger, Icon::warning }; // E-12
            case S::empty: case S::ready: break;
        }
        auto t = state.playing ? ja ("再生中") : ja ("待機中");
        if (def.loop) t << ja (" ・ ループ");
        return { t, state.playing ? Tone::ok : Tone::sub, Icon::speaker };
    }

    static void paintStatus (juce::Graphics& g, juce::Rectangle<int> r, const StatusInfo& st)
    {
        const int s = Theme::space3 - Theme::space1 / 2;
        drawIcon (g, st.icon, r.removeFromLeft (s).toFloat().withSizeKeepingCentre (float (s), float (s)), toneColour (st.tone), 2.2f);
        r.removeFromLeft (Theme::space1 + Theme::space1 / 2);
        drawText (g, st.text, r, Theme::fontXS, toneColour (st.tone), juce::Justification::centredLeft, true);
    }

    /** Mock: a 4 px bar between the name row and the status row, only while playing. */
    void paintProgress (juce::Graphics& g, juce::Rectangle<int> gap) const
    {
        if (! state.playing || state.lengthSeconds <= 0.0) return;
        const auto& p = Theme::colours();
        const auto track = gap.toFloat().withSizeKeepingCentre (float (gap.getWidth()), float (kProgressH));
        const float radius = float (kProgressH) * 0.5f;
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (track, radius);
        const auto done = float (juce::jlimit (0.0, 1.0, state.positionSeconds / state.lengthSeconds));
        g.setColour (p.accent);
        g.fillRoundedRectangle (track.withWidth (juce::jmax (track.getHeight(), track.getWidth() * done)), radius);
    }

    struct EmptyRects { juce::Rectangle<int> number, text, button; };
    EmptyRects emptyLayout() const
    {
        // the line goes only when it does not fit (the compact rule is for filled cards, which hold more)
        const int numH = Theme::space3, lineH = Theme::space4 - Theme::space1;
        const bool roomy = getHeight() >= numH + lineH + Theme::touchMin + Theme::space2 * 4;
        const int textH = roomy ? lineH : 0;
        const int total = numH + Theme::space2 + (textH > 0 ? textH + Theme::space2 : 0) + Theme::touchMin;
        auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), total);
        EmptyRects e;
        e.number = r.removeFromTop (numH);
        r.removeFromTop (Theme::space2);
        if (textH > 0)
        {
            e.text = r.removeFromTop (textH);
            r.removeFromTop (Theme::space2);
        }
        e.button = r;
        return e;
    }

    int index;
    PlayButton play;
    PillButton choose;
    SoundboardSlotDef def;
    SoundSlotState state;
    juce::String hotkeyText;
};

// =============================================================================================== Toolbar
/** Title, "n / 12 割り当て済み", the ducking slider (F-06-7) and [すべて停止] (F-06-6). */
class Toolbar : public juce::Component
{
public:
    Toolbar()
        : title (ja ("サウンドボード"), Theme::fontM, Tone::text, true), count ({}, Theme::fontXS, Tone::sub, false, true),
          duckLabel (ja ("再生中の声"), Theme::fontS, Tone::sub), stop (ja ("すべて停止"), PillButton::Style::outline, Icon::stop)
    {
        for (auto* l : { &title, &count, &duckLabel })
        {
            l->setJustification (juce::Justification::centredLeft);
            l->setMaxLines (1);
            addAndMakeVisible (*l);
        }
        count.setComponentID ("soundboard.count");
        duck.setComponentID ("soundboard.ducking");
        duck.setTitle (ja ("再生中の声の音量（ダッキング）"));
        duck.setTooltip (ja ("効果音を鳴らしている間、自分の声をこの分だけ下げます（0〜-24 dB。0 dB で下げません）"));
        duck.setup (kDuckingDb.min, kDuckingDb.max, kDuckingDb.def, 0.5, [] (double v) { return formatDb (v); });
        stop.setComponentID ("soundboard.stopAll");
        stop.setTooltip (ja ("鳴っている効果音を、すべて止めます"));
        addAndMakeVisible (duck);
        addAndMakeVisible (stop);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space3, 0);
        const int stopW = stop.preferredWidth();
        stop.setBounds (r.removeFromRight (stopW).withSizeKeepingCentre (stopW, Theme::buttonH));
        r.removeFromRight (Theme::space3);
        const int titleW = title.singleLineWidth();
        title.setBounds (r.removeFromLeft (titleW).withSizeKeepingCentre (titleW, title.heightForWidth (titleW)));
        r.removeFromLeft (Theme::space3 + Theme::space1);
        const int labelW = duckLabel.singleLineWidth();
        const int countW = count.singleLineWidth();
        const int avail = r.getWidth() - labelW - Theme::space2;
        const int prefW = m::sliderW * 4 / 5 + ValueSlider::valueTextWidth;  // mock 200 + value
        const int minW = Theme::space5 * 3 + ValueSlider::valueTextWidth;
        const int sliderW = juce::jlimit (juce::jmin (minW, avail), prefW, avail - countW - Theme::space3);
        const bool showCount = avail - sliderW >= countW + Theme::space3;
        duck.setBounds (r.removeFromRight (sliderW).withSizeKeepingCentre (sliderW, Theme::space5));
        r.removeFromRight (Theme::space2);
        duckLabel.setBounds (r.removeFromRight (labelW).withSizeKeepingCentre (labelW, duckLabel.heightForWidth (labelW)));
        count.setVisible (showCount);
        if (showCount) count.setBounds (r.removeFromLeft (countW).withSizeKeepingCentre (countW, count.heightForWidth (countW)));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::colours().surface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);
    }

    TextLabel title, count, duckLabel;
    ValueSlider duck;
    PillButton stop;
};

// =============================================================================================== HintBar
/** The one-time hint (F-13-7), 3 lines, closable. */
class HintBar : public juce::Component
{
public:
    HintBar()
        : text (ja ("「ファイルを選ぶ」で、効果音（WAV / FLAC / Ogg / MP3、1 つ 60 秒まで）を割り当てます。\n"
                    "再生ボタンを押すと、相手にも聞こえます。スロットを押すと、音量・ループ・モニターに流すかを変えられます。\n"
                    "ホットキーは「設定」の「ホットキー」で割り当てます。"),
                Theme::fontS),
          close (ja ("案内を閉じる"), Icon::close)
    {
        setComponentID ("soundboard.hint");
        close.setComponentID ("soundboard.hint.close");
        text.setIcon (Icon::help);
        text.setTone (Tone::text);
        addAndMakeVisible (text);
        addAndMakeVisible (close);
    }

    int heightForWidth (int w) const { return text.heightForWidth (w - Theme::space3 * 2 - Theme::touchMin - Theme::space2) + Theme::space3 * 2; }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space3);
        close.setBounds (r.removeFromRight (Theme::touchMin).removeFromTop (Theme::touchMin).translated (0, -Theme::space1));
        r.removeFromRight (Theme::space2);
        text.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::colours().raised);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);
        if (onFirstPaint)
        {
            auto f = std::move (onFirstPaint);
            onFirstPaint = nullptr;
            juce::MessageManager::callAsync (f); // never change settings from inside paint()
        }
    }

    TextLabel text;
    IconButton close;
    std::function<void()> onFirstPaint;
};

// =============================================================================================== Grid
class Grid : public juce::Component
{
public:
    juce::OwnedArray<SlotCard> cards;
    int slotH = kSlotMaxH;

    void resized() override
    {
        const int w = (getWidth() - kGap * (kCols - 1)) / kCols;
        for (int i = 0; i < cards.size(); ++i)
            cards[i]->setBounds ((i % kCols) * (w + kGap), (i / kCols) * (slotH + kGap), w, slotH);
    }
};

// =============================================================================================== slot settings panel
/** "スロット NN の設定": file, volume (-24..+6 dB), one-shot / loop, retrigger, monitor (F-06-4, F-06-5), hotkey. */
class SlotSettingsPanel : public OverlayPanel
{
public:
    SlotSettingsPanel (AppController& ctl, Navigator& n, int s, std::function<void()> changedFn, std::function<void (int)> chooseFn)
        : OverlayPanel (ja ("スロット ") + slotNumber (s) + ja (" の設定"), [np = &n] { np->closeOverlay(); }), c (ctl), nav (n), slot (s),
          changed (std::move (changedFn)), choose (std::move (chooseFn))
    {
        setComponentID ("soundboard.settings");
        setSize (Theme::space5 * 18, Theme::space5 * 16);
        const auto def = c.getSoundboard().getSlotDef (slot);
        auto stack = std::make_unique<VStack> (0);

        // file
        {
            auto flow = std::make_unique<FlowBox> (Theme::space2);
            auto pick = std::make_unique<PillButton> (def.file.isEmpty() ? ja ("ファイルを選ぶ") : ja ("ファイルを変える"), PillButton::Style::outline, Icon::folder);
            pick->setComponentID ("soundboard.settings.choose");
            pick->onClick = [this] { auto ch = choose; const int sl = slot; auto* np = &nav; np->closeOverlay(); if (ch) ch (sl); };
            const int pw = pick->preferredWidth();
            flow->add (std::move (pick), pw, Theme::buttonH);
            if (def.file.isNotEmpty())
            {
                auto clear = std::make_unique<PillButton> (ja ("割り当てを解除"), PillButton::Style::outline);
                clear->setComponentID ("soundboard.settings.unassign");
                clear->onClick = [this]
                {
                    c.getSoundboard().clearSlot (slot);
                    c.saveSoundboard();
                    auto ch = changed;
                    auto* np = &nav;
                    np->closeOverlay();
                    if (ch) ch();
                };
                const int cw = clear->preferredWidth();
                flow->add (std::move (clear), cw, Theme::buttonH);
            }
            auto* fr = flow.get();
            const int nat = fr->naturalWidth();
            addRow (*stack, "ファイル", def.file.isNotEmpty() ? baseName (def.file) : ja ("未割り当て（WAV / FLAC / Ogg / MP3、60 秒まで）"), std::move (flow), nat,
                    juce::jmin (nat, Theme::space5 * 5), [fr] (int w) { return fr->heightForWidth (w); });
        }
        // volume
        {
            auto s2 = std::make_unique<ValueSlider>();
            s2->setComponentID ("soundboard.settings.volume");
            s2->setTitle (ja ("音量"));
            s2->setup (kSoundboardVolumeDb.min, kSoundboardVolumeDb.max, kSoundboardVolumeDb.def, 0.5, [] (double v) { return formatDb (v); });
            s2->setValue (def.volumeDb, juce::dontSendNotification);
            auto* raw = s2.get();
            raw->onValueChange = [this, raw] { apply ([v = float (raw->getValue())] (SoundboardSlotDef& d) { d.volumeDb = v; }); };
            addRow (*stack, "音量", ja ("この効果音の大きさです（-24〜+6 dB）"), std::move (s2), m::sliderW + ValueSlider::valueTextWidth,
                    m::sliderW / 2 + ValueSlider::valueTextWidth, [] (int) { return Theme::space5; });
        }
        // one-shot / loop
        {
            auto seg = std::make_unique<Segmented> (juce::StringArray { ja ("ワンショット"), ja ("ループ") });
            seg->setComponentID ("soundboard.settings.mode");
            seg->setSelected (def.loop ? 1 : 0);
            seg->onChange = [this] (int i) { apply ([i] (SoundboardSlotDef& d) { d.loop = i == 1; }); };
            const int w = seg->preferredWidth() + Theme::space2;
            addRow (*stack, "再生のしかた", ja ("ループは、停止ボタンか「すべて停止」を押すまで繰り返します"), std::move (seg), w, w, [] (int) { return Theme::buttonH; });
        }
        // retrigger
        {
            auto seg = std::make_unique<Segmented> (juce::StringArray { retriggerName (0), retriggerName (1), retriggerName (2) });
            seg->setComponentID ("soundboard.settings.retrigger");
            seg->setSelected (juce::jlimit (0, 2, def.retrigger));
            seg->onChange = [this] (int i) { apply ([i] (SoundboardSlotDef& d) { d.retrigger = i; }); };
            const int w = seg->preferredWidth() + Theme::space2;
            addRow (*stack, "もう一度押したとき", ja ("鳴っている間に、もう一度押したときの動きです"), std::move (seg), w, w, [] (int) { return Theme::buttonH; });
        }
        // monitor
        {
            auto t = std::make_unique<ToggleSwitch>();
            t->setComponentID ("soundboard.settings.monitor");
            t->setTitle (ja ("モニターにも流す"));
            t->setToggleState (def.toMonitor, juce::dontSendNotification);
            auto* raw = t.get();
            raw->onClick = [this, raw] { apply ([on = raw->getToggleState()] (SoundboardSlotDef& d) { d.toMonitor = on; }); };
            const int w = ToggleSwitch::preferredWidth();
            addRow (*stack, "モニターにも流す", ja ("自分のモニター（ヘッドホン）でも聞きます。相手には、いつも聞こえます"), std::move (t), w, w,
                    [] (int) { return Theme::toggleH + Theme::space2; });
        }
        // hotkey
        {
            auto b = std::make_unique<PillButton> (ja ("設定で変更"), PillButton::Style::outline);
            b->setComponentID ("soundboard.settings.hotkey");
            b->onClick = [this] { auto* np = &nav; np->closeOverlay(); np->showSettings (Navigator::SettingsSection::hotkeys); };
            const auto hk = c.getHotkeyText (soundAction (slot));
            const int w = b->preferredWidth();
            addRow (*stack, "ホットキー", hk.isNotEmpty() ? hk : ja ("未割り当て"), std::move (b), w, w, [] (int) { return Theme::buttonH; });
        }
        auto* sr = stack.get();
        setContent (std::move (stack), [sr] (int w) { return sr->heightForWidth (w); });
    }

private:
    static void addRow (VStack& stack, const char* title, const juce::String& desc, std::unique_ptr<juce::Component> control, int prefW, int minW,
                        std::function<int (int)> heightFor)
    {
        auto row = std::make_unique<SettingRow> (ja (title), desc, std::move (control), prefW, minW, std::move (heightFor));
        auto* raw = row.get();
        stack.add (std::move (row), [raw] (int w) { return raw->heightForWidth (w); });
    }

    void apply (const std::function<void (SoundboardSlotDef&)>& change)
    {
        auto& sb = c.getSoundboard();
        auto d = sb.getSlotDef (slot);
        change (d);
        sb.setSlotDef (slot, d);
        c.saveSoundboard();
        if (changed) changed();
    }

    AppController& c;
    Navigator& nav;
    int slot;
    std::function<void()> changed;
    std::function<void (int)> choose;
};
} // namespace

// =============================================================================================== Impl
struct SoundboardView::Impl final : juce::ChangeListener, private juce::Timer
{
    Impl (SoundboardView& o, AppController& ctl, Navigator& n) : owner (o), c (ctl), nav (n)
    {
        owner.addAndMakeVisible (toolbar);
        owner.addChildComponent (hint);
        viewport.setScrollBarsShown (true, false);
        viewport.setViewedComponent (&grid, false);
        owner.addAndMakeVisible (viewport);
        for (int i = 0; i < kSoundboardSlots; ++i)
        {
            auto* card = grid.cards.add (new SlotCard (i));
            card->onPlay = [this, i]
            {
                if (grid.cards[i]->isPlaying()) sb().stopSlot (i); // the button shows a stop square while the slot plays
                else sb().trigger (i);
                refresh();
            };
            card->onChoose = [this, i] { chooseFile (i); };
            card->onSettings = [this, i] { openSettings (i); };
            grid.addAndMakeVisible (card);
        }
        toolbar.stop.onClick = [this] { sb().stopAll(); refresh(); };
        toolbar.duck.onValueChange = [this]
        {
            const float v = float (toolbar.duck.getValue());
            c.updateSettings ([v] (Settings& s) { s.duckingDb = v; });
        };
        hint.close.onClick = [this]
        {
            hint.setVisible (false);
            layout();
        };

        // F-06-8: loads finish on a background thread; AppController forwards the soundboard's
        // state changes as change messages
        c.addChangeListener (this);
        refresh();
    }

    ~Impl() override
    {
        stopTimer();
        c.removeChangeListener (this);
        viewport.setViewedComponent (nullptr, false);
    }

    Soundboard& sb() { return c.getSoundboard(); }

    void refresh()
    {
        int assigned = 0;
        bool playing = false;
        for (int i = 0; i < kSoundboardSlots; ++i)
        {
            const auto def = sb().getSlotDef (i);
            const auto st = sb().getSlotState (i);
            if (def.file.isNotEmpty()) ++assigned;
            playing = playing || st.playing;
            grid.cards[i]->update (def, st, c.getHotkeyText (soundAction (i)));
        }
        // change messages come only when a sound starts or stops; the progress bars move on this timer
        if (! playing) stopTimer();
        else if (! isTimerRunning()) startTimerHz (20);
        toolbar.count.setText (juce::String (assigned) + " / " + juce::String (kSoundboardSlots) + ja (" 割り当て済み"));
        toolbar.duck.setValue (c.getSettings().duckingDb, juce::dontSendNotification);
        toolbar.resized();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void timerCallback() override
    {
        if (! owner.isShowing()) return;
        for (int i = 0; i < kSoundboardSlots; ++i)
            if (grid.cards[i]->isPlaying()) grid.cards[i]->setPosition (sb().getSlotState (i).positionSeconds);
    }

    void chooseFile (int slot)
    {
        chooser = std::make_unique<juce::FileChooser> (ja ("スロット ") + slotNumber (slot) + ja (" の効果音を選ぶ"), lastDir, "*.wav;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, slot] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            lastDir = f.getParentDirectory();
            sb().assignFile (slot, f); // async: loading -> ready / error (E-12..E-14)
            c.saveSoundboard();
            refresh();
        });
    }

    void openSettings (int slot)
    {
        auto safe = juce::Component::SafePointer<SoundboardView> (&owner);
        nav.showOverlay (std::make_unique<SlotSettingsPanel> (
            c, nav, slot, [safe] { if (safe != nullptr) safe->impl->refresh(); },
            [safe] (int s) { if (safe != nullptr) safe->impl->chooseFile (s); }));
    }

    void visibilityChanged()
    {
        if (! owner.isVisible())
        {
            if (hint.isVisible() && hintMarked) hint.setVisible (false); // shown once: gone after leaving the page
            return;
        }
        if (! c.getSettings().soundboardHintShown && ! hintMarked)
        {
            hint.setVisible (true);
            hint.onFirstPaint = [this, safe = juce::Component::SafePointer<SoundboardView> (&owner)]
            {
                if (safe == nullptr) return;
                c.updateSettings ([] (Settings& s) { s.soundboardHintShown = true; });
            };
            hintMarked = true;
            layout();
        }
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        toolbar.setBounds (r.removeFromTop (kToolbarH));
        r.removeFromTop (kGap);
        if (hint.isVisible())
        {
            hint.setBounds (r.removeFromTop (hint.heightForWidth (r.getWidth())));
            r.removeFromTop (kGap);
        }
        viewport.setBounds (r);
        int w = r.getWidth();
        int slotH = juce::jmin (kSlotMaxH, (r.getHeight() - kGap * (kRows - 1)) / kRows);
        if (slotH < kSlotCompactH)
        {
            slotH = kSlotCompactH;
            w -= viewport.getScrollBarThickness() + Theme::space2;
        }
        grid.slotH = slotH;
        grid.setBounds (0, 0, juce::jmax (0, w), slotH * kRows + kGap * (kRows - 1));
    }

    SoundboardView& owner;
    AppController& c;
    Navigator& nav;
    Toolbar toolbar;
    HintBar hint;
    juce::Viewport viewport;
    Grid grid;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastDir;
    bool hintMarked = false;
};

// =============================================================================================== SoundboardView
SoundboardView::SoundboardView (AppController& controller, Navigator& nav) { impl = std::make_unique<Impl> (*this, controller, nav); }
SoundboardView::~SoundboardView() = default;
void SoundboardView::resized() { impl->layout(); }
void SoundboardView::paint (juce::Graphics&) {}
void SoundboardView::visibilityChanged() { impl->visibilityChanged(); }
} // namespace koe::ui

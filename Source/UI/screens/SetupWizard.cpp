// S-04 初回セットアップ (docs/mockups/A-S04.dc.html, F-10). Three steps: virtual cable, devices, Discord.
// Like the mock it fills the whole window and draws its own header. The test tone and the cable probe
// run only while step 3 is shown and are always stopped when leaving it (or the wizard).
#include "UI/Screens.h"
#include "UI/screens/Common.h"

#include <array>

namespace koe::ui
{
using namespace screens;

namespace
{
constexpr int kNumSteps = 3;
constexpr int kStepsW = Theme::space5 * 9 + Theme::space2;       // mock 300
constexpr int kStepsWNarrow = Theme::space5 * 6;                 // 192
constexpr int kStepItemH = Theme::space5 * 2;                    // mock 14 + 2 lines + 14
constexpr int kCircle = Theme::space4 + Theme::space1;           // mock 28
constexpr float kHeardDb = -50.0f;                               // probe level counted as "届いた"

/** 1 px divider line inside a VStack. */
class Divider : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::colours().divider);
        g.fillRect (0, 0, getWidth(), 1);
    }
};

/** Left column: the three steps with done / current / upcoming circles. */
class StepList : public juce::Component
{
public:
    struct Item { juce::String title, sub; bool ok = true; };
    std::array<Item, kNumSteps> items;
    int current = 0;

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        auto r = getLocalBounds();
        const bool narrow = getWidth() < kStepsW;
        for (int i = 0; i < kNumSteps; ++i)
        {
            auto item = r.removeFromTop (kStepItemH);
            r.removeFromTop (Theme::space1 + Theme::space1 / 2);
            if (i == current)
            {
                g.setColour (p.raised);
                g.fillRoundedRectangle (item.toFloat(), Theme::radiusM);
            }
            auto in = item.reduced (Theme::space3, Theme::space3 - Theme::space1);
            const auto circle = in.removeFromLeft (kCircle).removeFromTop (kCircle).toFloat();
            in.removeFromLeft (Theme::space3 - Theme::space1);
            if (i < current)
            {
                g.setColour (items[size_t (i)].ok ? p.ok : p.warn);
                g.fillEllipse (circle);
                drawIcon (g, items[size_t (i)].ok ? Icon::check : Icon::warning, circle.withSizeKeepingCentre (float (Theme::space3), float (Theme::space3)), p.bg, 3.0f);
            }
            else
            {
                if (i == current) { g.setColour (p.accent); g.fillEllipse (circle); }
                else { g.setColour (p.border); g.drawEllipse (circle.reduced (0.75f), Theme::borderWidth); }
                drawText (g, juce::String (i + 1), circle.toNearestInt(), Theme::fontS, i == current ? p.onAccent : p.textSub, juce::Justification::centred, true, true);
            }
            const float titleSize = narrow ? Theme::fontS : Theme::fontM;
            drawText (g, items[size_t (i)].title, in.removeFromTop (lineH()), titleSize, i <= current ? p.text : p.textSub, juce::Justification::centredLeft, i == current);
            drawText (g, items[size_t (i)].sub, in.removeFromTop (Theme::space3 + Theme::space1 / 2), Theme::fontXS, p.textSub, juce::Justification::centredLeft);
        }
    }

private:
    static int lineH() { return Theme::space4 - Theme::space1 / 2; }
};

/** Segmented level meter of the cable probe (mock: 40 bars). */
class SegmentMeter : public juce::Component
{
public:
    void setLevel (float db)
    {
        level = db;
        repaint();
    }
    float getLevel() const noexcept { return level; }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        constexpr int n = 40;
        const float gap = float (Theme::space1) * 0.5f;
        const float w = (float (getWidth()) - gap * (n - 1)) / n;
        const float h = float (Theme::space3 - Theme::space1);
        const float y = (float (getHeight()) - h) * 0.5f;
        const int lit = juce::roundToInt (LevelMeter::dbToPos (level) * n);
        for (int i = 0; i < n; ++i)
        {
            g.setColour (i < lit ? p.ok : p.trackOff);
            g.fillRoundedRectangle (float (i) * (w + gap), y, w, h, 1.0f);
        }
    }

private:
    float level = -100.0f;
};

/** "仮想マイクに音が届いているか確認する" box (F-10-3). */
class TestBox : public juce::Component
{
public:
    TestBox()
        : title (ja ("仮想マイクに音が届いているか確認する"), Theme::fontS, Tone::text, true), button (ja ("テスト音を出す"), PillButton::Style::accentOutline, Icon::playFilled),
          result ({}, Theme::fontXS, Tone::sub, true)
    {
        setComponentID ("setup.testBox");
        button.setComponentID ("setup.testTone");
        result.setComponentID ("setup.probeResult");
        meter.setComponentID ("setup.probeMeter");
        result.setJustification (juce::Justification::centredLeft);
        for (auto* comp : std::initializer_list<juce::Component*> { &title, &button, &meter, &result }) addAndMakeVisible (comp);
    }

    void setRunning (bool on)
    {
        button.setButtonText (on ? ja ("テスト音を止める") : ja ("テスト音を出す"));
        button.setIcon (on ? Icon::stop : Icon::playFilled);
        button.setStyle (on ? PillButton::Style::primary : PillButton::Style::accentOutline);
        resized();
    }

    void setResult (const juce::String& text, Tone tone)
    {
        result.setText (text);
        result.setTone (tone);
        result.setIcon (tone == Tone::ok ? std::optional<Icon> (Icon::check) : (tone == Tone::sub ? std::nullopt : std::optional<Icon> (Icon::warning)));
        if (onHeightChanged) onHeightChanged();
    }

    int heightForWidth (int w) const
    {
        const int inner = w - Theme::space3 * 2;
        const bool stacked = inner < rowMinW();
        return Theme::space3 * 2 + title.heightForWidth (inner) + Theme::space3 - Theme::space1 + Theme::controlH
               + (stacked && result.getText().isNotEmpty() ? Theme::space2 + result.heightForWidth (inner) : 0);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space3);
        title.setBounds (r.removeFromTop (title.heightForWidth (r.getWidth())));
        r.removeFromTop (Theme::space3 - Theme::space1);
        const bool stacked = r.getWidth() < rowMinW();
        auto row = r.removeFromTop (Theme::controlH);
        const int bw = button.preferredWidth();
        button.setBounds (row.removeFromLeft (bw));
        row.removeFromLeft (Theme::space3);
        if (stacked)
        {
            meter.setBounds (row);
            r.removeFromTop (Theme::space2);
            result.setBounds (r.removeFromTop (result.heightForWidth (r.getWidth())));
            return;
        }
        const int rw = juce::jmin (resultW(), row.getWidth() / 2);
        result.setBounds (row.removeFromRight (rw));
        row.removeFromRight (Theme::space3);
        meter.setBounds (row);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        const auto b = getLocalBounds().toFloat();
        g.setColour (p.raised);
        g.fillRoundedRectangle (b, Theme::radiusL);
        g.setColour (p.divider);
        g.drawRoundedRectangle (b.reduced (0.5f), Theme::radiusL, 1.0f);
    }

    TextLabel title;
    PillButton button;
    SegmentMeter meter;
    TextLabel result;
    std::function<void()> onHeightChanged;

private:
    int resultW() const { return juce::jmax (Theme::space5 * 5, result.singleLineWidth()); }
    int rowMinW() const { return button.preferredWidth() + Theme::space3 * 2 + Theme::space5 * 4 + resultW(); }
};

std::unique_ptr<SettingRow> deviceRow (const char* title, const juce::String& desc, juce::ComboBox*& combo, TextLabel*& status, const juce::String& id)
{
    auto box = std::make_unique<LayoutBox>();
    auto cb = std::make_unique<juce::ComboBox>();
    combo = cb.get();
    cb->setComponentID (id);
    cb->setTitle (ja (title));
    auto st = std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::sub, true);
    status = st.get();
    st->setComponentID (id + ".status");
    auto* cr = combo;
    auto* sr = status;
    box->addAndMakeVisible (cb.release());
    box->addAndMakeVisible (st.release());
    box->layout = [cr, sr] (juce::Rectangle<int> r)
    {
        cr->setBounds (r.removeFromTop (Theme::controlH));
        r.removeFromTop (Theme::space2);
        sr->setBounds (r.removeFromTop (sr->heightForWidth (r.getWidth())));
    };
    return std::make_unique<SettingRow> (ja (title), desc, std::move (box), m::comboW, m::comboW - Theme::space5 * 2, [sr] (int w)
    {
        return Theme::controlH + (sr->getText().isNotEmpty() ? Theme::space2 + sr->heightForWidth (w) : 0);
    });
}

void fillDevices (juce::ComboBox& cb, const juce::StringArray& names, const juce::String& current)
{
    juce::StringArray items { ja ("未選択") };
    items.addArray (names);
    if (current.isNotEmpty() && ! names.contains (current)) items.add (current);
    bool same = cb.getNumItems() == items.size();
    for (int i = 0; same && i < items.size(); ++i) same = cb.getItemText (i) == items[i];
    if (! same)
    {
        cb.clear (juce::dontSendNotification);
        for (int i = 0; i < items.size(); ++i) cb.addItem (items[i], i + 1);
    }
    cb.setSelectedId (current.isEmpty() ? 1 : juce::jmax (1, items.indexOf (current) + 1), juce::dontSendNotification);
}
} // namespace

// =============================================================================================== Impl
struct SetupWizard::Impl final : juce::ChangeListener, juce::Timer, juce::ComponentListener
{
    Impl (SetupWizard& o, AppController& ctl, Navigator& n, std::function<void (bool)> done)
        : owner (o), c (ctl), nav (n), onFinished (std::move (done)), stepLabel ({}, Theme::fontXS, Tone::sub, false, true),
          title ({}, Theme::fontL, Tone::text, true), later (ja ("あとで設定する"), PillButton::Style::link),
          back (ja ("戻る"), PillButton::Style::outline), next (ja ("次へ"), PillButton::Style::primary)
    {
        stepLabel.setComponentID ("setup.step");
        title.setComponentID ("setup.title");
        later.setComponentID ("setup.later");
        back.setComponentID ("setup.back");
        next.setComponentID ("setup.next");
        title.setMaxLines (2);
        next.setPill (true);
        next.setFontSize (Theme::fontM);
        later.setTooltip (ja ("セットアップを閉じます。設定画面と [?] メニューから、あとで同じ案内を開けます"));
        for (auto* comp : std::initializer_list<juce::Component*> { &steps, &stepLabel, &title, &viewport, &later, &back, &next }) owner.addAndMakeVisible (comp);
        viewport.setScrollBarsShown (true, false);
        later.onClick = [this] { finish (false); };
        back.onClick = [this] { show (step - 1); };
        next.onClick = [this] { if (step == kNumSteps - 1) finish (true); else show (step + 1); };

        buildCable();
        buildDevices();
        buildDiscord();
        owner.addComponentListener (this);
        c.addChangeListener (this);
        show (0);
    }

    ~Impl() override
    {
        stopTest();
        c.removeChangeListener (this);
        owner.removeComponentListener (this);
        viewport.setViewedComponent (nullptr, false);
    }

    // ------------------------------------------------------------------------------- step 1: virtual cable (F-10-1, F-10-2)
    void buildCable()
    {
        auto& v = bodies[0];
        found = &addNote (v, ja ("仮想ケーブル（VB-CABLE）を検出しました。"), Tone::ok);
        found->setComponentID ("setup.cableFound");
        foundText = &v.addText (ja ("加工した声は仮想ケーブル（VB-Audio Virtual Cable）に送られます。Discord では、対になる「CABLE Output」をマイクとして選びます。"));
        missing = &addNote (v, ja ("仮想ケーブル（VB-CABLE）が見つかりません。"), Tone::warn);
        missing->setComponentID ("setup.cableMissing");
        missingParts.add (&v.addText (ja ("KoeLoom の声を Discord に届けるには、無料の仮想オーディオケーブル「VB-CABLE」が要ります。公式サイトから入手して導入してください。"
                                          "導入には管理者権限と、PC の再起動が要る場合があります。")));
        const char* install[] = { "［公式サイトを開く］を押し、VB-CABLE をダウンロードして ZIP を展開します。",
                                  "展開したフォルダのセットアップ（64 ビットの Windows では VBCABLE_Setup_x64.exe）を右クリックし、「管理者として実行」を選んで「Install Driver」を押します。",
                                  "再起動を求められたら、PC を再起動します。",
                                  "KoeLoom に戻り、［再検出］を押します。" };
        for (int i = 0; i < 4; ++i)
        {
            auto s = std::make_unique<NumberedStep> (i + 1, ja (install[i]));
            auto* raw = s.get();
            missingParts.add (&v.add (std::move (s), [raw] (int w) { return raw->heightForWidth (w); }));
        }
        auto flow = std::make_unique<FlowBox> (Theme::space2);
        auto open = std::make_unique<PillButton> (ja ("公式サイトを開く"), PillButton::Style::primary);
        open->setComponentID ("setup.openCable");
        open->setTooltip (kCableUrl);
        open->onClick = [] { juce::URL (kCableUrl).launchInDefaultBrowser(); };
        openButton = open.get();
        auto rescan = std::make_unique<PillButton> (ja ("再検出"), PillButton::Style::outline);
        rescan->setComponentID ("setup.rescan");
        rescan->onClick = [this] { c.rescanDevices(); refresh(); };
        const int ow = open->preferredWidth(), rw = rescan->preferredWidth();
        flow->add (std::move (open), ow, Theme::buttonH);
        flow->add (std::move (rescan), rw, Theme::buttonH);
        auto* fr = flow.get();
        v.add (std::move (flow), [fr] (int w) { return fr->heightForWidth (w); });
    }

    static NoteBox& addNote (VStack& v, const juce::String& text, Tone tone)
    {
        auto nb = std::make_unique<NoteBox> (text, tone);
        auto* raw = nb.get();
        raw->getLabel().setTone (tone == Tone::ok ? Tone::ok : Tone::text);
        v.add (std::move (nb), [raw] (int w) { return raw->heightForWidth (w); });
        return *raw;
    }

    // ------------------------------------------------------------------------------- step 2: devices (F-10-3)
    void buildDevices()
    {
        auto& v = bodies[1];
        v.addText (ja ("ふだん話すマイクと、加工した声の送り先を選びます。送り先には「CABLE Input」（Windows によっては「スピーカー (VB-Audio Virtual Cable)」と表示されます）を選びます。あとから設定画面でも変えられます。"), Theme::fontS, Tone::sub);
        auto in = deviceRow ("マイク（入力）", ja ("話すときに使うマイクです"), inputCombo, inputStatus, "setup.input");
        auto out = deviceRow ("出力先（仮想マイク）", ja ("Discord の入力デバイスには、対になる「CABLE Output」を選びます"), outputCombo, outputStatus, "setup.output");
        for (auto* row : { &in, &out })
        {
            auto* raw = row->get();
            v.add (std::move (*row), [raw] (int w) { return raw->heightForWidth (w); });
        }
        inputCombo->onChange = [this] { c.setInputDevice (inputCombo->getSelectedId() > 1 ? inputCombo->getText() : juce::String()); };
        outputCombo->onChange = [this] { c.setOutputDevice (outputCombo->getSelectedId() > 1 ? outputCombo->getText() : juce::String()); };

        // INTERFACES.md §7.4: asked once here, OFF by default (no network unless turned on); S-03 起動と常駐 > 更新 has it too
        auto t = std::make_unique<ToggleSwitch>(); // the device row above already ends with a divider
        autoUpdate = t.get();
        t->setComponentID ("setup.autoUpdate");
        t->setTitle (ja ("新しい版を自動で受け取る"));
        t->setToggleState (c.getSettings().autoUpdate, juce::dontSendNotification);
        t->onClick = [this] { c.updateSettings ([on = autoUpdate->getToggleState()] (Settings& s) { s.autoUpdate = on; }); };
        auto upd = std::make_unique<SettingRow> (ja ("新しい版を自動で受け取る"),
                                                 ja ("GitHub の公開ページを確認して新しい版をダウンロードし、終了時に置き換えます。OFF のときは通信しません"),
                                                 std::move (t), ToggleSwitch::preferredWidth(), ToggleSwitch::preferredWidth(),
                                                 [] (int) { return Theme::toggleH + Theme::space2; });
        upd->setDivider (false);
        auto* raw = upd.get();
        v.add (std::move (upd), [raw] (int w) { return raw->heightForWidth (w); });
    }

    // ------------------------------------------------------------------------------- step 3: Discord (F-10-3..F-10-5)
    void buildDiscord()
    {
        auto& v = bodies[2];
        const auto texts = discordSteps();
        for (int i = 0; i < texts.size(); ++i)
        {
            auto s = std::make_unique<NumberedStep> (i + 1, texts[i]);
            auto* raw = s.get();
            v.add (std::move (s), [raw] (int w) { return raw->heightForWidth (w); });
        }
        v.addText (discordRecommended(), Theme::fontXS, Tone::sub);
        v.add (std::make_unique<Divider>(), [] (int) { return 1; });
        auto& revert = v.addText (revertMicText(), Theme::fontS);
        revert.setIcon (Icon::mic);
        revert.setComponentID ("setup.revertMic");
        auto box = std::make_unique<TestBox>();
        test = box.get();
        test->onHeightChanged = [this] { layoutBody(); };
        test->button.onClick = [this] { if (testing) stopTest(); else startTest(); };
        v.add (std::move (box), [this] (int w) { return test->heightForWidth (w); });
        auto& howl = addNote (v, ja ("モニター（自分の声を聞く機能）を、スピーカーで使うとハウリングします。モニターはヘッドホンで使ってください。"), Tone::warn);
        howl.setComponentID ("setup.howling");
    }

    void startTest()
    {
        stopTest();
        if (! c.getStatus().running)
        {
            test->setResult (ja ("音声が止まっています。ステップ 2 でマイクと出力先を選んでください。"), Tone::danger);
            return;
        }
        c.setTestTone (true); // to the virtual mic only, never to the speakers
        testing = true;
        juce::String cableOut;
        for (auto& name : c.getInputDevices())
            if (AppController::isCableOutputName (name)) { cableOut = name; break; }
        if (cableOut.isEmpty())
            test->setResult (ja ("「CABLE Output」が見つからないため、届いたかを確かめられません。"), Tone::warn);
        else if (const auto err = c.getCableProbe().start (cableOut); err.isNotEmpty())
            test->setResult (err, Tone::danger);
        else
        {
            probing = true;
            test->setResult (ja ("確かめています…"), Tone::sub);
        }
        test->setRunning (true);
        heardTicks = 0;
        startTimerHz (30);
    }

    void stopTest()
    {
        stopTimer();
        if (testing) c.setTestTone (false);
        if (probing) c.getCableProbe().stop();
        const bool was = testing;
        testing = probing = false;
        if (test != nullptr && was)
        {
            test->setRunning (false);
            test->meter.setLevel (-100.0f);
        }
    }

    void timerCallback() override
    {
        if (! probing) return;
        const float db = juce::Decibels::gainToDecibels (c.getCableProbe().fetchPeak(), -100.0f);
        test->meter.setLevel (db);
        heardTicks = db > kHeardDb ? 30 : juce::jmax (0, heardTicks - 1); // keep "届いています" for ~1 s
        if (heardTicks > 0) test->setResult (ja ("音が届いています"), Tone::ok);
        else test->setResult (ja ("まだ届いていません。出力先が仮想ケーブル（VB-Audio Virtual Cable）か確かめてください。"), Tone::sub);
    }

    // ------------------------------------------------------------------------------- navigation
    void show (int s)
    {
        stopTest(); // the tone and the probe never outlive step 3
        step = juce::jlimit (0, kNumSteps - 1, s);
        const char* titles[] = { "仮想ケーブル（VB-CABLE）を確認する", "マイクと出力先を選ぶ", "Discord の入力デバイスを設定する" };
        stepLabel.setText (juce::String (step + 1) + " / " + juce::String (kNumSteps));
        title.setText (ja (titles[step]));
        back.setVisible (step > 0);
        next.setButtonText (step == kNumSteps - 1 ? ja ("完了して始める") : ja ("次へ"));
        viewport.setViewedComponent (&bodies[step], false);
        viewport.setViewPosition (0, 0);
        if (step == 2) test->setResult ({}, Tone::sub);
        refresh();
    }

    void finish (bool completed)
    {
        stopTest();
        c.updateSettings ([] (Settings& st) { st.setupDone = true; });
        auto done = onFinished; // may delete the wizard
        if (done) done (completed);
    }

    void refresh()
    {
        const bool cable = c.isVirtualCableInstalled();
        found->setVisible (cable);
        foundText->setVisible (cable);
        missing->setVisible (! cable);
        for (auto* comp : missingParts) comp->setVisible (! cable);
        openButton->setVisible (! cable);

        fillDevices (*inputCombo, c.getInputDevices(), c.getInputDevice());
        fillDevices (*outputCombo, c.getOutputDevices(), c.getOutputDevice());
        const auto in = c.getInputDevice(), out = c.getOutputDevice();
        inputStatus->setText (AppController::isCableOutputName (in) ? ja ("「CABLE Output」はマイクではありません。ループになるため開始できません") : juce::String());
        inputStatus->setTone (Tone::danger);
        inputStatus->setIcon (Icon::warning);
        if (AppController::isCableInputName (out))
        {
            outputStatus->setText (ja ("仮想ケーブルです"));
            outputStatus->setTone (Tone::ok);
            outputStatus->setIcon (Icon::check);
        }
        else
        {
            outputStatus->setText (out.isEmpty() ? ja ("未選択です。VB-Audio Virtual Cable の再生デバイス（CABLE Input）を選んでください") : ja ("仮想ケーブルではありません。Discord に声が届かない可能性があります"));
            outputStatus->setTone (Tone::warn);
            outputStatus->setIcon (Icon::warning);
        }

        steps.current = step;
        steps.items[0] = { ja ("仮想ケーブルを確認"), cable ? ja ("仮想ケーブルを検出しました") : ja ("まだ見つかりません"), cable };
        const auto devText = (in.isNotEmpty() ? in : ja ("未選択")) + ja (" → ") + (out.isNotEmpty() ? out : ja ("未選択"));
        steps.items[1] = { ja ("マイクと出力先を選ぶ"), step > 1 ? devText : ja ("使うマイクと、声の送り先"), in.isNotEmpty() && AppController::isCableInputName (out) };
        steps.items[2] = { ja ("Discord を設定する"), ja ("Discord 側の設定とテスト"), true };
        steps.items[size_t (step)].sub = ja ("いまここ");
        steps.repaint();
        layout();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void componentVisibilityChanged (juce::Component&) override
    {
        if (! owner.isVisible()) stopTest();
    }

    // ------------------------------------------------------------------------------- layout
    void layoutBody()
    {
        auto& body = bodies[step];
        const int vw = viewport.getWidth();
        if (vw <= 0) return;
        int w = vw, h = body.heightForWidth (w);
        if (h > viewport.getHeight())
        {
            w = vw - viewport.getScrollBarThickness() - Theme::space2;
            h = body.heightForWidth (w);
        }
        body.setBounds (0, 0, w, h);
        body.resized();
    }

    void layout()
    {
        const bool narrow = owner.getWidth() < Theme::narrowWidth;
        auto r = owner.getLocalBounds().reduced (Theme::space4).withTrimmedTop (-Theme::space2);
        headerArea = r.removeFromTop (Theme::headerH);
        r.removeFromTop (Theme::space3);
        steps.setBounds (r.removeFromLeft (narrow ? kStepsWNarrow : kStepsW).withHeight (kNumSteps * (kStepItemH + Theme::space2)));
        r.removeFromLeft (Theme::space4);
        cardArea = r;
        auto in = r.reduced (narrow ? Theme::space4 : Theme::space5, narrow ? Theme::space4 : Theme::space4 + Theme::space1);
        stepLabel.setBounds (in.removeFromTop (Theme::space3));
        in.removeFromTop (Theme::space1);
        title.setBounds (in.removeFromTop (title.heightForWidth (in.getWidth())));
        in.removeFromTop (Theme::space3 + Theme::space1);
        auto footer = in.removeFromBottom (Theme::pillH);
        in.removeFromBottom (Theme::space3);
        const int nw = juce::jmax (next.preferredWidth(), Theme::space5 * 4);
        next.setBounds (footer.removeFromRight (nw));
        footer.removeFromRight (Theme::space2 + Theme::space1);
        if (back.isVisible()) back.setBounds (footer.removeFromRight (Theme::space5 * 3).withSizeKeepingCentre (Theme::space5 * 3, Theme::buttonH));
        footer.removeFromRight (Theme::space2);
        const int lw = juce::jmin (later.preferredWidth(), footer.getWidth());
        later.setBounds (footer.removeFromLeft (lw));
        viewport.setBounds (in);
        layoutBody();
    }

    void paint (juce::Graphics& g)
    {
        const auto& p = Theme::colours();
        g.fillAll (p.bg);
        auto h = headerArea;
        const int logo = Theme::space5 - Theme::space1 / 2;
        drawIcon (g, Icon::logo, h.removeFromLeft (logo).toFloat().withSizeKeepingCentre (float (logo), float (logo)), p.text, 2.4f, p.accent);
        h.removeFromLeft (Theme::space3 - Theme::space1);
        const juce::String name ("KoeLoom");
        const int nw = int (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontL, true), name)) + 2;
        drawText (g, name, h.removeFromLeft (nw), Theme::fontL, p.text, juce::Justification::centredLeft, true);
        h.removeFromLeft (Theme::space2);
        drawText (g, ja ("初回セットアップ"), h, Theme::fontS, p.textSub, juce::Justification::centredLeft);
        g.setColour (p.surface);
        g.fillRoundedRectangle (cardArea.toFloat(), Theme::radiusL);
    }

    SetupWizard& owner;
    AppController& c;
    Navigator& nav;
    std::function<void (bool)> onFinished;
    StepList steps;
    TextLabel stepLabel, title;
    PillButton later, back, next;
    juce::Viewport viewport;
    VStack bodies[kNumSteps] { VStack (Theme::space3), VStack (Theme::space3), VStack (Theme::space3 - Theme::space1) };
    NoteBox *found = nullptr, *missing = nullptr;
    TextLabel* foundText = nullptr;
    juce::Array<juce::Component*> missingParts;
    PillButton* openButton = nullptr;
    juce::ComboBox *inputCombo = nullptr, *outputCombo = nullptr;
    TextLabel *inputStatus = nullptr, *outputStatus = nullptr;
    ToggleSwitch* autoUpdate = nullptr;
    TestBox* test = nullptr;
    juce::Rectangle<int> headerArea, cardArea;
    int step = 0, heardTicks = 0;
    bool testing = false, probing = false;
};

// =============================================================================================== SetupWizard
SetupWizard::SetupWizard (AppController& controller, Navigator& nav, std::function<void (bool)> onFinished)
{
    setComponentID ("setup");
    impl = std::make_unique<Impl> (*this, controller, nav, std::move (onFinished));
    setSize (Theme::defaultWidth, Theme::defaultHeight);
}

SetupWizard::~SetupWizard() = default;
void SetupWizard::resized() { impl->layout(); }
void SetupWizard::paint (juce::Graphics& g) { impl->paint (g); }
} // namespace koe::ui

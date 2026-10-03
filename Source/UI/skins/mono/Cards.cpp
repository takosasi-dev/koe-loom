// 案 C Mono: the top row (ボイチェン / マイクミュート / meters) and the cards ピッチ / フォルマント / 重ねる声, owner wave5/mono.
#include "UI/skins/mono/MonoParts.h"

#include <cmath>

namespace koe::ui::mono
{
namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kToggleW = Theme::toggleW + Theme::space2 + 28; // ToggleSwitch::preferredWidth()

juce::String dbText (double db)
{
    const double r = std::round (db * 10.0) / 10.0;
    const bool whole = std::abs (r - std::round (r)) < 1.0e-6;
    return (whole ? juce::String (juce::roundToInt (r)) : juce::String (r, 1)) + " dB";
}

juce::String stText (double st) { return mainui::formatSemitones (st) + " st"; }
juce::String degreeText (double d) { return mainui::formatSemitones (d) + ja (" 度"); }

void drawStatus (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, juce::Colour colour)
{
    drawIcon (g, icon, r.removeFromLeft (14).withSizeKeepingCentre (14, 14).toFloat(), colour, 2.6f);
    r.removeFromLeft (Theme::space1);
    drawText (g, text, r, Theme::fontXS, colour, juce::Justification::centredLeft, true);
}

int statusWidth (const juce::String& text) { return 14 + Theme::space1 + juce::roundToInt (textRunWidth (text, Theme::fontXS, true, false)) + 2; }

/** Card frame: surface, 1 px divider outline. */
void paintFrame (juce::Graphics& g, juce::Rectangle<int> r)
{
    const auto& p = P();
    const auto b = r.toFloat().reduced (0.5f);
    g.setColour (p.surface);
    g.fillRoundedRectangle (b, Theme::radiusM);
    g.setColour (p.divider);
    g.drawRoundedRectangle (b, Theme::radiusM, 1.0f);
}

void fillCombo (juce::ComboBox& box, const juce::StringArray& names, juce::StringArray& cache, const juce::String& selected)
{
    if (names != cache)
    {
        cache = names;
        box.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i) box.addItem (names[i], i + 1);
    }
    box.setSelectedId (names.indexOf (selected) + 1, juce::dontSendNotification);
}
} // namespace

// =============================================================================================== ボイチェン
VoiceBlock::VoiceBlock (AppController& ctl) : juce::Button (ja ("ボイチェン ON")), c (ctl)
{
    setComponentID ("tour.voiceToggle");
    setTooltip (ja ("ボイスチェンジャーの ON/OFF（OFF で元の声）"));
    onClick = [this] { c.setVoiceChangerOn (! c.isVoiceChangerOn()); };
    refresh();
}

void VoiceBlock::refresh()
{
    const bool on = c.isVoiceChangerOn();
    setButtonText (on ? ja ("ボイチェン ON") : ja ("ボイチェン OFF"));
    setToggleState (on, juce::dontSendNotification);
    repaint();
}

void VoiceBlock::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    const bool on = getToggleState();
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    auto ink = p.text;
    if (on)
    {
        g.setColour (highlighted || down ? p.accent.brighter (0.08f) : p.accent); // the only filled block on the page (D-23)
        g.fillRoundedRectangle (b, Theme::radiusM);
        ink = p.onAccent;
    }
    else
    {
        g.setColour (highlighted || down ? p.raised : p.surface);
        g.fillRoundedRectangle (b, Theme::radiusM);
        g.setColour (p.border);
        g.drawRoundedRectangle (b.reduced (0.5f), Theme::radiusM, Theme::borderWidth);
    }
    auto r = getLocalBounds().reduced (Theme::space3, Theme::space2);
    const auto state = on ? juce::String ("ON") : juce::String ("OFF");
    if (getHeight() >= Theme::space5 * 2 + Theme::space2)
    {
        drawText (g, ja ("ボイチェン"), r.removeFromTop (Theme::space3 + 2), Theme::fontXS, ink, juce::Justification::centredLeft, true);
        auto line = r.withSizeKeepingCentre (r.getWidth(), Theme::space5 + Theme::space1);
        drawIcon (g, Icon::power, line.removeFromLeft (Theme::space4 + 2).withSizeKeepingCentre (Theme::space4, Theme::space4).toFloat(), ink, 2.6f);
        line.removeFromLeft (Theme::space2);
        drawText (g, state, line, Theme::fontXL, ink, juce::Justification::centredLeft, true);
    }
    else
    {
        drawIcon (g, Icon::power, r.removeFromLeft (20).withSizeKeepingCentre (20, 20).toFloat(), ink, 2.6f);
        r.removeFromLeft (Theme::space2);
        g.setColour (ink);
        g.setFont (Theme::ui (Theme::fontM, true));
        g.drawFittedText (getButtonText(), r, juce::Justification::centredLeft, 1, 0.8f);
    }
    if (hasKeyboardFocus (true)) drawFocusRing (g, b, Theme::radiusM);
}

// =============================================================================================== マイクミュート
MuteBlock::MuteBlock (AppController& ctl) : juce::Button (ja ("マイクミュート")), c (ctl)
{
    setComponentID ("tour.mute");
    setTitle (ja ("マイクミュート"));
    setTooltip (ja ("マイクミュート（相手に声を送らない）"));
    onClick = [this] { c.setMicMuted (! c.isMicMuted()); };
    refresh();
}

void MuteBlock::refresh()
{
    const bool muted = c.isMicMuted();
    setButtonText (muted ? juce::String ("MUTE") : ja ("マイクミュート"));
    setToggleState (muted, juce::dontSendNotification);
    repaint();
}

void MuteBlock::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    const bool muted = getToggleState();
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    auto ink = p.text;
    if (muted)
    {
        g.setColour (p.danger);
        g.fillRoundedRectangle (b, Theme::radiusM);
        ink = p.onDanger;
    }
    else
    {
        g.setColour (highlighted || down ? p.raised : p.surface);
        g.fillRoundedRectangle (b, Theme::radiusM);
        g.setColour (p.border);
        g.drawRoundedRectangle (b.reduced (0.5f), Theme::radiusM, Theme::borderWidth);
    }
    auto r = getLocalBounds().reduced (Theme::space2);
    const auto f = Theme::ui (muted ? Theme::fontM : Theme::fontS, true);
    if (getHeight() >= Theme::space5 * 2 + Theme::space2)
    {
        const int iconS = Theme::space4;
        const int block = iconS + Theme::space1 + juce::roundToInt (f.getHeight());
        auto col = r.withSizeKeepingCentre (r.getWidth(), block);
        drawIcon (g, Icon::mic, col.removeFromTop (iconS).withSizeKeepingCentre (iconS, iconS).toFloat(), ink, 2.2f);
        col.removeFromTop (Theme::space1);
        g.setColour (ink);
        g.setFont (f);
        g.drawFittedText (getButtonText(), col, juce::Justification::centred, 1, 0.8f);
    }
    else
    {
        const float textW = juce::GlyphArrangement::getStringWidth (f, getButtonText());
        const int w = juce::jmin (r.getWidth(), 18 + Theme::space1 + int (std::ceil (textW)));
        auto line = r.withSizeKeepingCentre (w, r.getHeight());
        drawIcon (g, Icon::mic, line.removeFromLeft (18).withSizeKeepingCentre (18, 18).toFloat(), ink, 2.2f);
        line.removeFromLeft (Theme::space1);
        g.setColour (ink);
        g.setFont (f);
        g.drawFittedText (getButtonText(), line, juce::Justification::centredLeft, 1, 0.7f);
    }
    if (hasKeyboardFocus (true)) drawFocusRing (g, b, Theme::radiusM);
}

// =============================================================================================== 入力 / 出力
LevelRow::LevelRow (AppController& ctl, Navigator& n, bool out) : c (ctl), nav (n), output (out), meter (false)
{
    setComponentID (output ? "voice.output" : "voice.input");
    meter.setComponentID (output ? "tour.outputMeter" : "voice.inputMeter");
    meter.setShowScale (false);
    addAndMakeVisible (meter);
    if (output)
    {
        link = std::make_unique<LinkButton> (ja ("設定で変更"));
        link->setComponentID ("voice.outputDeviceLink");
        link->setTooltip (ja ("出力先（仮想マイク）は設定のデバイスで選びます"));
        link->onClick = [this] { nav.showSettings (Navigator::SettingsSection::devices); }; // F-01-11
        addAndMakeVisible (*link);
    }
    else
    {
        device.setComponentID ("voice.inputDevice");
        device.setTitle (ja ("入力デバイス"));
        device.setTooltip (ja ("声を拾うマイクを選びます"));
        device.setTextWhenNothingSelected (ja ("未選択"));
        device.setTextWhenNoChoicesAvailable (ja ("入力デバイスがありません"));
        device.onChange = [this] { if (device.getSelectedId() > 0) c.setInputDevice (device.getText()); };
        addAndMakeVisible (device);
    }
}

void LevelRow::setCompact (bool cp)
{
    compact = cp;
    resized();
    repaint();
}

void LevelRow::refresh()
{
    if (output)
    {
        deviceName = c.getOutputDevice();
        gainDb = c.getSettings().outputGainDb;
        setTooltip (ja ("出力ゲイン ") + juce::String (gainDb, 1) + ja (" dB（設定で変更）"));
    }
    else
    {
        fillCombo (device, c.getInputDevices(), items, c.getInputDevice());
        gateOn = c.getSettings().gateOn;
    }
    repaint();
}

void LevelRow::tick (const AppController::Meters& m, const AppController::Status& s)
{
    const float db = output ? m.outputDb : m.inputDb;
    meter.setLevel (db, output ? m.outputClip : m.inputClip);
    shown = db > shown ? db : juce::jmax (db, shown - 0.7f);
    const int lv = juce::jmax (-60, juce::roundToInt (shown));
    const bool clip = meter.isClipping();
    if (lv != levelText || s.gateOpen != gateOpen || clip != clipping || s.limiterActive != limiter)
    {
        levelText = lv;
        gateOpen = s.gateOpen;
        const bool statusChanged = clip != clipping || s.limiterActive != limiter;
        clipping = clip;
        limiter = s.limiterActive;
        if (output && statusChanged && ! compact) resized(); // the clip text changes width
        repaint();
    }
}

void LevelRow::resized()
{
    auto r = getLocalBounds();
    const int labelW = Theme::space5 + Theme::space1;
    if (compact)
    {
        labelArea = r.removeFromLeft (labelW);
        auto trail = r.removeFromRight (juce::jmin (r.getWidth() / 2, Theme::space5 * 5 + Theme::space2)); // 168
        r.removeFromRight (Theme::space2);
        valueArea = r.removeFromRight (Theme::space5);
        r.removeFromRight (Theme::space2);
        meter.setBounds (r);
        if (output)
        {
            link->setBounds (trail.removeFromRight (link->preferredWidth()));
            trail.removeFromRight (Theme::space1);
            nameArea = trail;
        }
        else
            device.setBounds (trail);
        statusArea = gainArea = {};
        return;
    }
    auto top = r.removeFromTop (Theme::space4 - 4); // 20
    labelArea = top.removeFromLeft (labelW);
    valueArea = top.removeFromRight (Theme::space5 + Theme::space2);
    top.removeFromRight (Theme::space2);
    meter.setBounds (top);
    r.removeFromTop (Theme::space1);
    r.removeFromLeft (labelW);
    if (output)
    {
        link->setBounds (r.removeFromRight (link->preferredWidth()));
        r.removeFromRight (Theme::space2);
        const auto status = clipping ? ja ("クリップあり") : limiter ? ja ("リミッター動作中") : ja ("クリップなし");
        const int sw = statusWidth (status);
        statusArea = r.getWidth() - sw >= Theme::space5 * 3 ? r.removeFromRight (sw) : juce::Rectangle<int>(); // the meter shows clipping too
        r.removeFromRight (Theme::space2);
        const auto gain = ja ("ゲイン ") + juce::String (gainDb, 1) + " dB";
        const int gw = juce::roundToInt (textRunWidth (gain, Theme::fontXS, false, true)) + Theme::space1;
        gainArea = r.getWidth() - gw >= Theme::space5 * 4 ? r.removeFromRight (gw) : juce::Rectangle<int>();
        nameArea = r;
    }
    else
    {
        statusArea = r.removeFromRight (statusWidth (ja ("ゲート閉")) + Theme::space1);
        r.removeFromRight (Theme::space2);
        device.setBounds (r);
    }
}

void LevelRow::paint (juce::Graphics& g)
{
    const auto& p = P();
    drawText (g, output ? ja ("出力") : ja ("入力"), labelArea, Theme::fontXS, p.textSub, juce::Justification::centredLeft, true);
    drawText (g, juce::String (levelText), valueArea, Theme::fontS, p.text, juce::Justification::centredRight, true, true);
    if (output)
    {
        const bool unset = deviceName.isEmpty();
        const bool cable = AppController::isCableInputName (deviceName);
        auto n = nameArea;
        drawIcon (g, cable ? Icon::check : Icon::warning, n.removeFromLeft (14).withSizeKeepingCentre (14, 14).toFloat(), cable ? p.ok : p.warn, 2.6f);
        n.removeFromLeft (Theme::space1);
        g.setColour (unset ? p.warn : p.text);
        g.setFont (Theme::ui (Theme::fontXS, true));
        g.drawFittedText (unset ? ja ("出力先 未選択") : deviceName, n, juce::Justification::centredLeft, 1, 0.8f);
        if (! gainArea.isEmpty())
            drawText (g, ja ("ゲイン ") + juce::String (gainDb, 1) + " dB", gainArea, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
        if (! statusArea.isEmpty())
        {
            if (clipping) drawStatus (g, statusArea, Icon::warning, ja ("クリップあり"), p.danger);
            else if (limiter) drawStatus (g, statusArea, Icon::warning, ja ("リミッター動作中"), p.warn); // F-08-2
            else drawStatus (g, statusArea, Icon::check, ja ("クリップなし"), p.ok);
        }
    }
    else if (! statusArea.isEmpty())
    {
        const auto gateText = ! gateOn ? ja ("ゲートOFF") : (gateOpen ? ja ("ゲート開") : ja ("ゲート閉"));
        drawStatus (g, statusArea, gateOn && gateOpen ? Icon::check : Icon::close, gateText, gateOn && gateOpen ? p.ok : p.textSub);
    }
}

// =============================================================================================== ピッチ / フォルマント
BigValueCard::BigValueCard (AppController& ctl, bool f)
    : c (ctl), formant (f), minus (juce::String::fromUTF8 ("\xe2\x88\x92"), PillButton::Style::outline), plus ("+", PillButton::Style::outline)
{
    const auto key = formant ? juce::String ("formant") : juce::String ("pitch");
    const auto name = formant ? ja ("フォルマント") : ja ("ピッチ");
    const auto& range = formant ? kFormantSt : kPitchSt;
    setComponentID (formant ? "tour.formant" : "tour.pitch");
    setTitle (name);
    slider.setComponentID ("mono." + key + ".slider");
    slider.setTitle (formant ? ja ("フォルマント（-6〜+6 st、初期値 0）") : ja ("ピッチ（-12〜+12 st、初期値 0）"));
    slider.setTooltip (slider.getTitle() + ja ("。ダブルクリックで 0、Shift で微調整"));
    slider.setRange (range.min, range.max, kPitchStep);
    slider.setDoubleClickReturnValue (true, range.def);
    slider.setVelocityModeParameters (1.0, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    slider.onValueChange = [this] { setValue (slider.getValue()); };
    addAndMakeVisible (slider);
    const auto stepText = formant ? juce::String ("0.5") : juce::String ("1");
    minus.setComponentID ("mono." + key + ".minus");
    minus.setTitle (name + ja (" を下げる"));
    minus.setTooltip (name + ja (" を ") + stepText + ja (" st 下げる"));
    minus.onClick = [this] { step (-1); };
    plus.setComponentID ("mono." + key + ".plus");
    plus.setTitle (name + ja (" を上げる"));
    plus.setTooltip (name + ja (" を ") + stepText + ja (" st 上げる"));
    plus.onClick = [this] { step (1); };
    for (auto* b : { &minus, &plus })
    {
        b->setFontSize (Theme::fontM);
        addAndMakeVisible (*b);
    }
    if (! formant)
    {
        toggle = std::make_unique<ToggleSwitch>();
        toggle->setComponentID ("voice.shifterToggle");
        toggle->setTitle (ja ("変換 ON/OFF"));
        toggle->setTooltip (ja ("声の変換の ON/OFF（OFF の間は重ねる声も鳴りません）"));
        toggle->onClick = [this] { c.setShifterEnabled (toggle->getToggleState()); };
        addAndMakeVisible (*toggle);
    }
    refresh();
}

void BigValueCard::setValue (double v)
{
    if (formant) c.setFormant (float (v));
    else c.setPitch (float (v));
    repaint();
}

void BigValueCard::step (int direction)
{
    const double size = formant ? 0.5 : 1.0;
    const auto& range = formant ? kFormantSt : kPitchSt;
    setValue (juce::jlimit (double (range.min), double (range.max), slider.getValue() + direction * size));
    refresh();
}

void BigValueCard::setCompact (bool cp)
{
    compact = cp;
    resized();
    repaint();
}

void BigValueCard::refresh()
{
    slider.setValue (formant ? c.getFormant() : c.getPitch(), juce::dontSendNotification);
    active = c.hasShifter();
    if (toggle != nullptr) toggle->setToggleState (active, juce::dontSendNotification);
    const auto& range = formant ? kFormantSt : kPitchSt;
    minus.setEnabled (slider.getValue() > range.min);
    plus.setEnabled (slider.getValue() < range.max);
    repaint();
}

void BigValueCard::resized()
{
    auto r = getLocalBounds().reduced (compact ? Theme::space2 + Theme::space1 : Theme::space3, compact ? Theme::space2 : Theme::space3 - 2);
    header = r.removeFromTop (Theme::space4 - 4);
    if (toggle != nullptr) toggle->setBounds (header.withLeft (header.getRight() - kToggleW + (compact ? Theme::space2 : 0)));
    hint = compact || r.getHeight() < Theme::space5 * 4 ? juce::Rectangle<int>() : r.removeFromBottom (Theme::space3);
    if (! hint.isEmpty()) r.removeFromBottom (Theme::space1);
    const int bh = compact ? Theme::space4 + Theme::space1 : Theme::touchMin;
    auto controls = r.removeFromBottom (bh);
    minus.setBounds (controls.removeFromLeft (bh));
    plus.setBounds (controls.removeFromRight (bh));
    controls.reduce (Theme::space2, 0);
    slider.setBounds (controls);
    r.removeFromBottom (Theme::space1);
    number = r;
}

void BigValueCard::paint (juce::Graphics& g)
{
    const auto& p = P();
    paintFrame (g, getLocalBounds());
    auto h = header;
    drawText (g, formant ? ja ("フォルマント") : ja ("ピッチ"), h, Theme::fontS, p.text, juce::Justification::centredLeft, true);
    if (toggle != nullptr)
    {
        h.removeFromRight (toggle->getWidth() + Theme::space1);
        drawText (g, ja ("変換"), h, Theme::fontXS, p.textSub, juce::Justification::centredRight, true);
    }

    // the huge value (F-14-12, the part of 案 C that A already took): mono digits, "st" beside them
    const auto value = mainui::formatSemitones (slider.getValue());
    const float size = juce::jlimit (Theme::fontXL, Theme::fontXL * 2.0f, float (number.getHeight()) * 0.9f);
    const auto ink = active ? p.text : p.textSub;
    auto n = number;
    const int vw = juce::roundToInt (std::ceil (textRunWidth (value, size, true, true)));
    drawText (g, value, n.removeFromLeft (juce::jmin (n.getWidth(), vw + Theme::space1)), size, ink, juce::Justification::centredLeft, true, true);
    n.removeFromLeft (Theme::space1);
    drawText (g, active ? juce::String ("st") : ja ("st（変換 OFF）"), n.withTrimmedTop (juce::roundToInt (size * 0.35f)), Theme::fontS, p.textSub,
              juce::Justification::centredLeft, false, true);
    if (! hint.isEmpty())
        drawText (g, formant ? ja ("Shift で微調整・ダブルクリックで 0") : ja ("ダブルクリックで 0 に戻る"), hint, Theme::fontXS, p.textSub);
}

ShifterGroup::ShifterGroup (AppController& c) : pitch (c, false), formant (c, true)
{
    setComponentID ("voice.shifter");
    addAndMakeVisible (pitch);
    addAndMakeVisible (formant);
}

void ShifterGroup::setCompact (bool compact)
{
    pitch.setCompact (compact);
    formant.setCompact (compact);
}

void ShifterGroup::refresh()
{
    pitch.refresh();
    formant.refresh();
}

void ShifterGroup::resized()
{
    auto r = getLocalBounds();
    const int gap = getWidth() < Theme::narrowWidth / 2 ? Theme::space2 : Theme::space3;
    pitch.setBounds (r.removeFromLeft ((r.getWidth() - gap) / 2));
    r.removeFromLeft (gap);
    formant.setBounds (r);
}

// =============================================================================================== 重ねる声
namespace
{
/** One voice's editor (the Studio page's layer box, same calls): ON, delete, pitch or degree, formant, level. */
class LayerEditPanel : public PanelBase, private juce::ChangeListener
{
public:
    LayerEditPanel (AppController& ctl, Navigator& n, int i)
        : PanelBase (n, ja ("声 ") + juce::String (i + 1)), c (ctl), index (i), remove (ja ("この声を削除"), Icon::close)
    {
        setComponentID ("mono.layerPanel");
        subtitle = ja ("重ねる声");
        toggle.setComponentID ("mono.layerPanel.toggle");
        toggle.setTitle (ja ("声 ") + juce::String (index + 1) + " ON/OFF");
        toggle.onClick = [this] { c.setLayerEnabled (index, toggle.getToggleState()); };
        addAndMakeVisible (toggle);
        remove.setComponentID ("mono.layerPanel.remove");
        remove.onClick = [this]
        {
            auto& ctl2 = c;
            const int k = index;
            nav.closeOverlay();
            ctl2.removeLayer (k);
        };
        addAndMakeVisible (remove);
        scale = c.getLayer (index).mode == LayerDef::Mode::scale;
        rows.add (new SliderRow (scale ? ja ("度数") : ja ("ピッチ"), scale ? degreeText : stText));
        rows.add (new SliderRow (ja ("フォルマント"), stText));
        rows.add (new SliderRow (ja ("レベル"), dbText));
        if (scale) rows[0]->slider.setRange (kLayerDegree.min, kLayerDegree.max, 1.0);
        else rows[0]->slider.setRange (kPitchSt.min, kPitchSt.max, kPitchStep);
        rows[1]->slider.setRange (kFormantSt.min, kFormantSt.max, kPitchStep);
        rows[2]->slider.setRange (kLayerLevelDb.min, kLayerLevelDb.max, 0.5);
        rows[0]->slider.setDoubleClickReturnValue (true, scale ? kLayerDegree.def : 0.0);
        rows[1]->slider.setDoubleClickReturnValue (true, 0.0);
        rows[2]->slider.setDoubleClickReturnValue (true, kLayerLevelDb.def);
        rows[0]->onChange = [this] (double v)
        {
            auto def = c.getLayer (index);
            if (scale) def.degree = juce::roundToInt (v);
            else def.pitchSt = float (v);
            c.setLayer (index, def);
        };
        rows[1]->onChange = [this] (double v) { auto def = c.getLayer (index); def.formantSt = float (v); c.setLayer (index, def); };
        rows[2]->onChange = [this] (double v) { auto def = c.getLayer (index); def.levelDb = float (v); c.setLayer (index, def); };
        for (auto* row : rows)
        {
            row->stacked = true;
            row->slider.setTooltip (ja ("声 ") + juce::String (index + 1) + ja ("の") + row->slider.getTitle());
            addAndMakeVisible (row);
        }
        update();
        c.addChangeListener (this);
        setSize (Theme::space5 * 15, headerHeight + 20 + Theme::space2 + 3 * SliderRow::stackedHeight + 2 * Theme::space2 + Theme::space4);
    }
    ~LayerEditPanel() override { c.removeChangeListener (this); }

    void resized() override
    {
        PanelBase::resized();
        auto r = contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4);
        auto top = r.removeFromTop (20);
        toggle.setBounds (top.removeFromRight (kToggleW));
        top.removeFromRight (Theme::space2);
        remove.setBounds (top.removeFromRight (20));
        r.removeFromTop (Theme::space2);
        for (auto* row : rows)
        {
            row->setBounds (r.removeFromTop (SliderRow::stackedHeight));
            r.removeFromTop (Theme::space2);
        }
    }

    void paint (juce::Graphics& g) override
    {
        PanelBase::paint (g);
        auto r = contentArea().reduced (Theme::space4, 0).removeFromTop (20);
        drawText (g, scale ? ja ("スケール連動") : ja ("固定のピッチ"), r, Theme::fontXS, P().textSub);
    }

private:
    void update()
    {
        const auto d = c.getLayer (index);
        toggle.setToggleState (d.enabled, juce::dontSendNotification);
        rows[0]->slider.setValue (scale ? double (d.degree) : double (d.pitchSt), juce::dontSendNotification);
        rows[1]->slider.setValue (d.formantSt, juce::dontSendNotification);
        rows[2]->slider.setValue (d.levelDb, juce::dontSendNotification);
        for (auto* row : rows) row->repaint();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (index >= c.getNumLayers()) nav.closeOverlay();
        else update();
    }
    AppController& c;
    const int index;
    bool scale = false;
    ToggleSwitch toggle;
    SquareIconButton remove;
    juce::OwnedArray<SliderRow> rows;
};
} // namespace

/** "声 1   -12 st ・ -10 dB   [ON]": click or Enter opens LayerEditPanel. */
class LayersCard::Row : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int height = Theme::space4 + Theme::space1; // 28

    Row (AppController& ctl, Navigator& n, int i) : c (ctl), nav (n), index (i)
    {
        setWantsKeyboardFocus (true);
        setComponentID ("voice.layer." + juce::String (index));
        setTitle (ja ("声 ") + juce::String (index + 1));
        setTooltip (ja ("押すと声 ") + juce::String (index + 1) + ja (" のピッチ・フォルマント・レベルを編集"));
        toggle.setComponentID ("voice.layer." + juce::String (index) + ".toggle");
        toggle.setTitle (ja ("声 ") + juce::String (index + 1) + " ON/OFF");
        toggle.onClick = [this] { c.setLayerEnabled (index, toggle.getToggleState()); };
        addAndMakeVisible (toggle);
    }

    void update (const LayerDef& d)
    {
        summary = (d.mode == LayerDef::Mode::scale ? degreeText (d.degree) : stText (d.pitchSt)) + ja (" ・ ") + dbText (d.levelDb);
        toggle.setToggleState (d.enabled, juce::dontSendNotification);
        repaint();
    }

    void resized() override { toggle.setBounds (getLocalBounds().removeFromRight (kToggleW - Theme::space2).withSizeKeepingCentre (kToggleW - Theme::space2, Theme::toggleH)); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto b = getLocalBounds().toFloat();
        if (isMouseOver (true)) { g.setColour (p.raised); g.fillRoundedRectangle (b, Theme::radiusS); }
        auto r = getLocalBounds().reduced (Theme::space1, 0).withTrimmedRight (kToggleW);
        const auto name = ja ("声 ") + juce::String (index + 1);
        drawText (g, name, r.removeFromLeft (juce::roundToInt (textRunWidth (name, Theme::fontXS, true, false)) + Theme::space2), Theme::fontXS, p.text,
                  juce::Justification::centredLeft, true);
        // narrow card: the pitch alone (the level is in the editor)
        const auto text = textRunWidth (summary, Theme::fontXS, false, true) <= float (r.getWidth()) ? summary : summary.upToFirstOccurrenceOf (ja (" ・"), false, false);
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (r);
            juce::AttributedString s;
            appendText (s, text, Theme::fontXS, false, true, toggle.getToggleState() ? p.text : p.textSub);
            s.setJustification (juce::Justification::centredLeft);
            s.setWordWrap (juce::AttributedString::none);
            s.draw (g, r.toFloat());
        }
        if (hasKeyboardFocus (false)) drawFocusRing (g, b, Theme::radiusS);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked()) open(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey) { open(); return true; }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void open() { nav.showOverlay (std::make_unique<LayerEditPanel> (c, nav, index)); }

private:
    AppController& c;
    Navigator& nav;
    const int index;
    ToggleSwitch toggle;
    juce::String summary;
};

LayersCard::LayersCard (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("声を追加")), resume (ja ("再開"))
{
    setComponentID ("voice.layers");
    add.setComponentID ("voice.layerAdd");
    add.setTooltip (ja ("重ねる声を足します（2 声まで）"));
    add.onClick = [this]
    {
        juce::String why;
        if (! c.addLayer (why)) nav.showToast (why);
    };
    addAndMakeVisible (add);
    resume.setComponentID ("voice.layersResume");
    resume.setTooltip (ja ("自動停止した重ねる声を再開します"));
    resume.onClick = [this] { c.resumeLayers(); };
    addChildComponent (resume);
    refresh();
}

LayersCard::~LayersCard() = default;

void LayersCard::setCompact (bool cp)
{
    compact = cp;
    refresh();
}

void LayersCard::refresh()
{
    const int n = c.getNumLayers();
    if (n != built)
    {
        built = n;
        rows.clear();
        for (int i = 0; i < n; ++i) addAndMakeVisible (rows.add (new Row (c, nav, i)));
    }
    for (int i = 0; i < rows.size(); ++i) rows[i]->update (c.getLayer (i));
    add.setVisible (n < kMaxLayers);
    add.setButtonText (compact ? ja ("声を追加") : ja ("声を追加（あと ") + juce::String (kMaxLayers - n) + ja (" 声）"));
    autoStopped = c.areLayersAutoStopped();
    resume.setVisible (autoStopped);
    resized();
    repaint();
}

void LayersCard::resized()
{
    auto r = getLocalBounds().reduced (compact ? Theme::space2 + Theme::space1 : Theme::space3, compact ? Theme::space2 : Theme::space3 - 2);
    header = r.removeFromTop (Theme::space4 - 4);
    if (resume.isVisible()) resume.setBounds (header.withLeft (header.getRight() - resume.preferredWidth()));
    r.removeFromTop (Theme::space1);
    // the big count when there is room (mock), else it moves into the header
    const int need = rows.size() * (Row::height + 2) + (add.isVisible() ? Row::height + Theme::space1 : 0);
    countArea = r.getHeight() - need >= Theme::space5 + Theme::space2 ? r.removeFromTop (r.getHeight() - need - Theme::space1) : juce::Rectangle<int>();
    if (! countArea.isEmpty()) r.removeFromTop (Theme::space1);
    for (auto* row : rows)
    {
        row->setBounds (r.removeFromTop (Row::height));
        r.removeFromTop (2);
    }
    if (add.isVisible())
    {
        r.removeFromTop (Theme::space1 - 2);
        add.setBounds (r.removeFromTop (Row::height));
    }
}

void LayersCard::paint (juce::Graphics& g)
{
    const auto& p = P();
    paintFrame (g, getLocalBounds());
    auto h = header;
    drawText (g, ja ("重ねる声"), h, Theme::fontS, p.text, juce::Justification::centredLeft, true);
    const auto count = juce::String (c.getNumLayers()) + " / " + juce::String (kMaxLayers);
    if (autoStopped)
    {
        h.removeFromRight (resume.preferredWidth() + Theme::space1);
        drawText (g, ja ("自動停止"), h, Theme::fontXS, p.warn, juce::Justification::centredRight, true);
    }
    else if (countArea.isEmpty())
        drawText (g, count, h, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
    if (! countArea.isEmpty())
    {
        const float size = juce::jlimit (Theme::fontXL, Theme::fontXL * 1.5f, float (countArea.getHeight()) * 0.9f);
        auto n = countArea;
        const auto num = juce::String (c.getNumLayers());
        drawText (g, num, n.removeFromLeft (juce::roundToInt (textRunWidth (num, size, true, true)) + Theme::space2), size, p.text,
                  juce::Justification::centredLeft, true, true);
        drawText (g, "/ " + juce::String (kMaxLayers), n.withTrimmedTop (juce::roundToInt (size * 0.3f)), Theme::fontM, p.textSub,
                  juce::Justification::centredLeft, false, true);
    }
    if (c.getNumLayers() == 0 && ! compact && countArea.isEmpty())
        drawText (g, ja ("重ねる声はありません"), add.getBounds().translated (0, -Row::height), Theme::fontXS, p.textSub, juce::Justification::centred);
}
} // namespace koe::ui::mono

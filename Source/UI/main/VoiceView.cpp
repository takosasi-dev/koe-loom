#include "UI/main/VoiceView.h"

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }

// Layout values from the approved mocks, rounded to tokens (§8.4).
constexpr int kRow = Theme::space4;                               // card header row
constexpr int kCardsWide = Theme::space5 * 8 + Theme::space2;      // 264
constexpr int kCardsWideMin = Theme::space5 * 6 + Theme::space2;   // 200
constexpr int kCardsNarrow = Theme::space5 * 4 + Theme::space4;    // 152
constexpr int kCardsNarrowMin = Theme::space5 * 4;                 // 128
constexpr int kMeterW = 26 + 12;                                   // LevelMeter scale + bar
constexpr int kToggleW = Theme::toggleW + Theme::space2 + 28;      // ToggleSwitch::preferredWidth()
constexpr int kKnobNarrow = Theme::knobBig * 3 / 4;                // 84: pitch / formant in the narrow layout

juce::String dbText (double db)
{
    const double r = std::round (db * 10.0) / 10.0;
    const bool whole = std::abs (r - std::round (r)) < 1.0e-6;
    return (whole ? juce::String (juce::roundToInt (r)) : juce::String (r, 1)) + " dB";
}

juce::String stText (double st) { return formatSemitones (st) + " st"; }
juce::String degreeText (double d) { return formatSemitones (d) + ja (" 度"); }

/** Small status line: icon + text in a colour (state never by colour alone, §8.3). */
void drawStatus (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, juce::Colour colour, float size)
{
    drawIcon (g, icon, r.removeFromLeft (16).withSizeKeepingCentre (16, 16).toFloat(), colour, 2.6f);
    r.removeFromLeft (Theme::space1 + 2);
    drawText (g, text, r, size, colour, juce::Justification::centredLeft, true);
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

// =============================================================================================== 聞き比べ / おまかせ (§9.4)
/** A die showing five (Icons.h has no dice): rounded square outline and five pips, fitted into area. */
void drawDice (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour ink)
{
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    const auto box = area.withSizeKeepingCentre (s, s).reduced (s * 0.1f);
    g.setColour (ink);
    g.drawRoundedRectangle (box, s * 0.18f, juce::jmax (1.5f, s * 0.09f));
    const float d = s * 0.17f;
    for (auto [fx, fy] : { std::pair (0.28f, 0.28f), std::pair (0.72f, 0.28f), std::pair (0.5f, 0.5f), std::pair (0.28f, 0.72f), std::pair (0.72f, 0.72f) })
        g.fillEllipse (box.getX() + box.getWidth() * fx - d / 2, box.getY() + box.getHeight() * fy - d / 2, d, d);
}

class ExtraButton : public juce::Button, private juce::Timer
{
public:
    enum class Kind { compare, random };
    ExtraButton (Kind k, AppController& ctl) : juce::Button (k == Kind::compare ? "compare" : "random"), kind (k), c (ctl)
    {
        setWantsKeyboardFocus (true);
        setComponentID (k == Kind::compare ? "voice.compare" : "voice.random");
        setTitle (k == Kind::compare ? ja ("聞き比べ（押している間だけ元の声）") : ja ("おまかせ（ランダムな声を作る）"));
        setTooltip (k == Kind::compare ? ja ("押している間だけ、変換しない元の声になります（スペースキーでも）。ボイチェン OFF のときは使えません")
                                       : ja ("おまかせ：ピッチ・フォルマント・エフェクトをランダムに組んだ声を作ります（気に入ったら保存）"));
    }
    ~ExtraButton() override { setHeld (false); }

    int preferredWidth (int h) const
    {
        if (kind == Kind::random && ! showText) return h;
        const auto f = Theme::ui (fontSize, true);
        const int textW = kind == Kind::compare ? juce::jmax (textWidth (f, ja ("聞き比べ")), textWidth (f, ja ("元の声"))) : textWidth (f, ja ("おまかせ"));
        return textW + 18 + 6 + Theme::space3 * 2 - 2;
    }

    /** Compare only: true sends setCompareHold (true) unless the button is disabled (voice changer OFF). */
    void setHeld (bool h)
    {
        if ((h && (kind != Kind::compare || ! isEnabled())) || held == h) return;
        held = h;
        heldByKey = heldByKey && h;
        c.setCompareHold (h);
        if (h) startTimerHz (10);
        else stopTimer();
        repaint();
    }
    /** The controller released it (preset load): follow without calling back. */
    void syncFromController()
    {
        if (! held || c.isCompareHeld()) return;
        held = heldByKey = false;
        stopTimer();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::Button::mouseDown (e);
        if (kind == Kind::compare) setHeld (true);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::Button::mouseUp (e);
        if (kind == Kind::compare && ! heldByKey) setHeld (false);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (! k.isKeyCode (juce::KeyPress::spaceKey)) return juce::Button::keyPressed (k);
        if (kind == Kind::random) triggerClick();
        else if (! held)
        {
            setHeld (true);
            heldByKey = held;
        }
        return true;
    }
    bool keyStateChanged (bool keyDown) override
    {
        if (heldByKey && ! juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::spaceKey))
        {
            setHeld (false);
            return true;
        }
        return juce::Button::keyStateChanged (keyDown);
    }
    void focusLost (FocusChangeType t) override
    {
        setHeld (false); // the window lost focus (or focus moved on)
        juce::Button::focusLost (t);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = P();
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const auto fill = held ? p.accent : (highlighted || down ? p.raised : juce::Colours::transparentBlack);
        const auto ink = held ? p.onAccent : p.text;
        if (! fill.isTransparent())
        {
            g.setColour (fill);
            g.fillRoundedRectangle (r, Theme::radiusM);
        }
        if (! held)
        {
            g.setColour (p.border);
            g.drawRoundedRectangle (r.reduced (0.75f), Theme::radiusM, Theme::borderWidth);
        }
        const auto label = kind == Kind::random ? (showText ? ja ("おまかせ") : juce::String()) : (held ? ja ("元の声") : ja ("聞き比べ"));
        const auto f = Theme::ui (fontSize, true);
        const float iconW = juce::jmin (18.0f, r.getHeight() - 8.0f), textW = label.isEmpty() ? 0.0f : float (textWidth (f, label));
        float x = r.getCentreX() - (iconW + (textW > 0.0f ? 6.0f + textW : 0.0f)) * 0.5f;
        const juce::Rectangle<float> iconArea (x, r.getCentreY() - iconW * 0.5f, iconW, iconW);
        if (kind == Kind::random) drawDice (g, iconArea, ink);
        else drawIcon (g, Icon::mic, iconArea, ink, held ? 2.4f : 2.0f);
        x += iconW + 6.0f;
        if (textW > 0.0f)
        {
            g.setColour (ink);
            g.setFont (f);
            g.drawText (label, juce::Rectangle<float> (x, r.getY(), textW + 2.0f, r.getHeight()), juce::Justification::centredLeft);
        }
        if (! isEnabled())
        {
            g.setColour (p.bg.withAlpha (0.55f));
            g.fillRoundedRectangle (r, Theme::radiusM);
        }
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusM);
    }

    const Kind kind;
    bool held = false, heldByKey = false, showText = true;
    float fontSize = Theme::fontS;

private:
    void timerCallback() override
    {
        if (! juce::Process::isForegroundProcess()) setHeld (false); // another app took the focus
    }
    AppController& c;
};

// =============================================================================================== preset bar
class PresetSelector : public juce::Button
{
public:
    PresetSelector() : juce::Button ("preset")
    {
        setWantsKeyboardFocus (true);
        setTitle (ja ("プリセットを選ぶ"));
        setTooltip (ja ("プリセットを選ぶ（押すと一覧から切り替え）"));
    }
    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& p = P();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (highlighted ? p.raised.brighter (0.04f) : p.raised);
        g.fillRoundedRectangle (r, Theme::radiusM);
        g.setColour (p.border);
        g.drawRoundedRectangle (r, Theme::radiusM, 1.0f);
        auto inner = getLocalBounds().reduced (Theme::space2 + Theme::space1, 0);
        drawIcon (g, Icon::chevronDown, inner.removeFromRight (16).withSizeKeepingCentre (16, 16).toFloat(), p.textSub);
        const auto nameF = Theme::ui (Theme::fontS, true);
        const int nameW = juce::jmin (inner.getWidth() - Theme::space4, textWidth (nameF, name) + Theme::space1);
        drawText (g, name, inner.removeFromLeft (nameW), Theme::fontS, p.text, juce::Justification::centredLeft, true);
        inner.removeFromLeft (Theme::space1 + 2);
        drawText (g, category + (modified ? ja ("・編集中") : juce::String()), inner, Theme::fontXS, p.textSub);
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusM);
    }
    juce::String name, category;
    bool modified = false;
};

class PresetBar : public juce::Component
{
public:
    PresetBar (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), list (ja ("一覧"), PillButton::Style::outline), save (ja ("保存"), PillButton::Style::outline),
          dup (ja ("複製"), PillButton::Style::outline), more (juce::String::fromUTF8 ("\xe2\x80\xa6"), PillButton::Style::outline),
          moreFavs ("+0", true), extras (ctl, n)
    {
        setComponentID ("tour.presets");
        addAndMakeVisible (extras); // §9.4: 聞き比べ / おまかせ at the bar's right end
        selector.setComponentID ("voice.presetSelector");
        selector.onClick = [this] { showPresetMenu(); };
        addAndMakeVisible (selector);
        list.setTooltip (ja ("プリセット一覧を開く（検索・お気に入り・名前の変更）"));
        list.onClick = [this] { nav.showPresetBrowser(); };
        save.setTooltip (ja ("いまの設定を保存する"));
        save.onClick = [this] { savePreset(); };
        dup.setTooltip (ja ("いまの設定を別の名前で保存する"));
        dup.onClick = [this] { askName (ja ("複製"), c.getCurrentPreset().name + ja (" のコピー")); };
        more.setTitle (ja ("保存、複製などのメニュー"));
        more.setTooltip (ja ("保存、複製などのメニュー"));
        more.onClick = [this] { showMoreMenu(); };
        for (auto* b : { &list, &save, &dup, &more }) addAndMakeVisible (*b);
        moreFavs.setComponentID ("voice.fav.more");
        moreFavs.setTooltip (ja ("ほかのお気に入りはプリセット一覧で"));
        moreFavs.onClick = [this] { nav.showPresetBrowser(); };
        addChildComponent (moreFavs);
        empty.setComponentID ("voice.fav.empty");
        empty.setText (ja ("★ お気に入りはまだありません"), juce::dontSendNotification);
        empty.setFont (Theme::ui (Theme::fontXS));
        empty.setColour (juce::Label::textColourId, P().textSub);
        addChildComponent (empty);
        refresh();
    }

    void setCompact (bool cp)
    {
        if (compact == cp) return;
        compact = cp;
        favSig = {};
        refresh();
    }

    void refresh()
    {
        const auto& cur = c.getCurrentPreset();
        selector.name = cur.name;
        selector.category = presetCategoryJa (cur.category());
        selector.modified = c.isCurrentPresetModified();
        selector.repaint();

        // favourites (F-05-12): first 4 (2 below 1000 px), "+n" for the rest, current one checked
        juce::StringArray ids;
        for (auto& id : c.getFavorites())
            if (c.getPresetLibrary().find (id.toStdString()) != nullptr) ids.add (id);
        const int shown = juce::jmin (ids.size(), compact ? 2 : 4);
        const auto sig = ids.joinIntoString (",") + "|" + juce::String (shown);
        if (sig != favSig)
        {
            favSig = sig;
            chips.clear();
            for (int i = 0; i < shown; ++i)
            {
                const auto id = ids[i].toStdString();
                auto* chip = chips.add (new ChipButton (c.getPresetLibrary().find (id)->name, false));
                chip->setComponentID ("voice.fav." + juce::String (i));
                chip->setTooltip (ja ("お気に入り ") + juce::String (i + 1) + ja ("：") + chip->getButtonText());
                chip->onClick = [this, id] { withLooperCheck (c, nav, *this, [this, id] { c.loadPreset (id); }); };
                addAndMakeVisible (chip);
            }
            moreFavs.setButtonText ("+" + juce::String (ids.size() - shown));
            moreFavs.setVisible (ids.size() > shown);
            empty.setVisible (ids.isEmpty());
        }
        for (int i = 0; i < chips.size(); ++i) chips[i]->setToggleState (ids[i].toStdString() == cur.id, juce::dontSendNotification);
        save.setVisible (! compact);
        dup.setVisible (! compact);
        more.setVisible (compact);
        extras.setStyle (false, false, compact ? Theme::fontXS : Theme::fontS);
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (compact ? Theme::space2 + Theme::space1 : Theme::space3 - 2, 0);
        const int bh = compact ? Theme::touchMin : Theme::buttonH;
        labelArea = r.removeFromLeft (textWidth (Theme::ui (Theme::fontXS), ja ("プリセット")) + 2);
        r.removeFromLeft (Theme::space2);
        const int selW = compact ? 190 : 220;
        selector.setBounds (r.removeFromLeft (selW).withSizeKeepingCentre (selW, bh));
        r.removeFromLeft (Theme::space2);
        for (auto* b : { &list, &save, &dup })
        {
            if (! b->isVisible()) continue;
            b->setBounds (r.removeFromLeft (b->preferredWidth()).withSizeKeepingCentre (b->preferredWidth(), bh));
            r.removeFromLeft (Theme::space2);
        }
        if (more.isVisible())
        {
            more.setBounds (r.removeFromRight (Theme::touchMin).withSizeKeepingCentre (Theme::touchMin, Theme::touchMin));
            r.removeFromRight (Theme::space2);
        }
        const int ew = extras.preferredWidth (bh);
        extras.setBounds (r.removeFromRight (ew).withSizeKeepingCentre (ew, bh));
        r.removeFromRight (Theme::space2);
        dividerX = r.getX() + Theme::space1;
        r.removeFromLeft (Theme::space2 + 1 + Theme::space1);
        starArea = r.removeFromLeft (compact ? 16 : 16 + Theme::space1 + textWidth (Theme::ui (Theme::fontXS), ja ("お気に入り")) + 2);
        r.removeFromLeft (Theme::space2);
        if (empty.isVisible()) empty.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 240)));
        const int moreW = moreFavs.isVisible() ? moreFavs.preferredWidth() + Theme::space2 : 0;
        auto favArea = r.withTrimmedRight (moreW);
        for (auto* chip : chips)
        {
            const int w = juce::jmin (chip->preferredWidth(), favArea.getWidth());
            chip->setBounds (favArea.removeFromLeft (w).withSizeKeepingCentre (w, Theme::touchMin));
            favArea.removeFromLeft (Theme::space1 + 2);
        }
        if (moreFavs.isVisible())
            moreFavs.setBounds (juce::Rectangle<int> (favArea.getX(), 0, moreFavs.preferredWidth() + Theme::space1, getHeight()).withSizeKeepingCentre (moreFavs.preferredWidth() + Theme::space1, Theme::touchMin));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        g.setColour (p.surface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);
        drawText (g, ja ("プリセット"), labelArea, Theme::fontXS, p.textSub);
        g.setColour (p.border);
        const int dh = compact ? Theme::space4 : Theme::space4 + Theme::space1;
        g.fillRect (dividerX, (getHeight() - dh) / 2, 1, dh);
        auto s = starArea;
        drawIcon (g, Icon::starFilled, s.removeFromLeft (16).withSizeKeepingCentre (15, 15).toFloat(), p.accent);
        if (! compact)
        {
            s.removeFromLeft (Theme::space1);
            drawText (g, ja ("お気に入り"), s, Theme::fontXS, p.textSub);
        }
    }

private:
    void showPresetMenu()
    {
        const auto& all = c.getPresetLibrary().all();
        const char* order[] = { "natural", "character", "device", "space", "layered", "user" };
        juce::PopupMenu m;
        for (auto* cat : order)
        {
            juce::PopupMenu sub;
            for (int i = 0; i < int (all.size()); ++i)
                if (all[size_t (i)].category() == cat)
                    sub.addItem (i + 1, all[size_t (i)].name, true, all[size_t (i)].id == c.getCurrentPreset().id);
            if (sub.getNumItems() > 0) m.addSubMenu (presetCategoryJa (cat), sub);
        }
        m.addSeparator();
        m.addItem (-1, ja ("プリセット一覧を開く"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (selector),
                         [safe = juce::Component::SafePointer<PresetBar> (this)] (int r)
                         {
                             if (safe == nullptr || r == 0) return;
                             if (r < 0) { safe->nav.showPresetBrowser(); return; }
                             const auto& list = safe->c.getPresetLibrary().all();
                             if (r - 1 >= int (list.size())) return;
                             const auto id = list[size_t (r - 1)].id;
                             auto* bar = safe.getComponent();
                             withLooperCheck (bar->c, bar->nav, *bar, [bar, id] { bar->c.loadPreset (id); }); // E-27
                         });
    }

    void showMoreMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, ja ("保存"));
        m.addItem (2, ja ("複製"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (more),
                         [safe = juce::Component::SafePointer<PresetBar> (this)] (int r)
                         {
                             if (safe == nullptr) return;
                             if (r == 1) safe->savePreset();
                             else if (r == 2) safe->askName (ja ("複製"), safe->c.getCurrentPreset().name + ja (" のコピー"));
                         });
    }

    /** User preset -> overwrite (F-05-2); built-in -> save as a new user preset. */
    void savePreset()
    {
        const auto* base = c.getPresetLibrary().find (c.getCurrentPreset().id);
        if (base != nullptr && ! base->builtin)
        {
            juce::String error;
            nav.showToast (c.overwriteCurrent (error) ? ja ("上書き保存しました。") : error);
            return;
        }
        askName (ja ("新しく保存"), c.getCurrentPreset().name);
    }

    void askName (const juce::String& title, const juce::String& initial)
    {
        auto panel = std::make_unique<ConfirmPanel> (nav, title, ja ("プリセットの名前を入れてください（32 文字まで）。"), ja ("保存"), nullptr);
        panel->addTextField (initial.substring (0, kPresetNameMaxChars), [this] (const juce::String& name)
        {
            if (name.isEmpty())
            {
                nav.showToast (ja ("名前を入れてください。"));
                return;
            }
            juce::String error;
            nav.showToast (c.saveCurrentAsNew (name, error) ? ja ("「") + name + ja ("」として保存しました。") : error);
        });
        nav.showOverlay (std::move (panel));
    }

    AppController& c;
    Navigator& nav;
    bool compact = false;
    PresetSelector selector;
    PillButton list, save, dup, more;
    juce::OwnedArray<ChipButton> chips;
    LinkButton moreFavs;
    VoiceExtras extras;
    juce::Label empty;
    juce::String favSig;
    juce::Rectangle<int> labelArea, starArea;
    int dividerX = 0;
};

// =============================================================================================== 01 入力
class InputControls : public juce::Component
{
public:
    explicit InputControls (AppController& ctl) : c (ctl), meterV (true), meterH (false)
    {
        setComponentID ("voice.input");
        device.setComponentID ("voice.inputDevice");
        device.setTitle (ja ("入力デバイス"));
        device.setTooltip (ja ("声を拾うマイクを選びます"));
        device.setTextWhenNothingSelected (ja ("未選択"));
        device.setTextWhenNoChoicesAvailable (ja ("入力デバイスがありません"));
        device.onChange = [this] { if (device.getSelectedId() > 0) c.setInputDevice (device.getText()); };
        addAndMakeVisible (device);
        meterH.setShowScale (false);
        addAndMakeVisible (meterV);
        addChildComponent (meterH);
        setCompact (false);
    }

    void setCompact (bool cp)
    {
        compact = cp;
        meterV.setVisible (! compact);
        meterH.setVisible (compact);
        meterV.setComponentID (compact ? juce::String() : "voice.inputMeter");
        meterH.setComponentID (compact ? "voice.inputMeter" : juce::String());
        resized();
    }

    void refresh()
    {
        fillCombo (device, c.getInputDevices(), items, c.getInputDevice());
        gateOn = c.getSettings().gateOn;
        repaint();
    }

    void tick (const AppController::Meters& m, const AppController::Status& s)
    {
        meterV.setLevel (m.inputDb, m.inputClip);
        meterH.setLevel (m.inputDb, m.inputClip);
        shown = m.inputDb > shown ? m.inputDb : juce::jmax (m.inputDb, shown - 0.7f);
        const int lv = juce::jmax (-60, juce::roundToInt (shown));
        if (lv != levelText || s.gateOpen != gateOpen)
        {
            levelText = lv;
            gateOpen = s.gateOpen;
            repaint();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (compact)
        {
            auto row = r.removeFromTop (Theme::space4 + Theme::space1);
            labelArea = row.removeFromLeft (Theme::space5);
            row.removeFromLeft (Theme::space2);
            gateArea = row.removeFromRight (textWidth (Theme::ui (Theme::fontXS, true), ja ("ゲート閉")) + 24);
            row.removeFromRight (Theme::space2);
            device.setBounds (row);
            r.removeFromTop (Theme::space1 + 2);
            auto meterRow = r.removeFromTop (Theme::space2 + Theme::space1);
            valueArea = meterRow.removeFromRight (Theme::space5);
            meterRow.removeFromRight (Theme::space2);
            meterH.setBounds (meterRow);
            return;
        }
        r.removeFromTop (kRow + Theme::space2);
        device.setBounds (r.removeFromTop (Theme::buttonH));
        r.removeFromTop (Theme::space2 + Theme::space1);
        meterV.setBounds (r.removeFromLeft (kMeterW).withTrimmedTop (Theme::space2).withTrimmedBottom (Theme::space1));
        r.removeFromLeft (Theme::space3);
        infoArea = r;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto gateText = ! gateOn ? ja ("OFF") : (gateOpen ? ja ("開") : ja ("閉"));
        const auto gateColour = gateOn && gateOpen ? p.ok : p.textSub;
        const auto gateIcon = gateOn && gateOpen ? Icon::check : Icon::close;
        const auto level = juce::String (levelText) + " dB";
        if (compact)
        {
            drawText (g, ja ("入力"), labelArea, Theme::fontXS, p.textSub);
            drawStatus (g, gateArea, gateIcon, ja ("ゲート") + gateText, gateColour, Theme::fontXS);
            drawText (g, juce::String (levelText), valueArea, Theme::fontXS, p.text, juce::Justification::centredRight, true, true);
            return;
        }
        drawCardHeader (g, getLocalBounds().removeFromTop (kRow), "01", ja ("入力"));
        auto r = infoArea;
        drawText (g, ja ("ゲート"), r.removeFromTop (kTextH), Theme::fontXS, p.textSub);
        drawStatus (g, r.removeFromTop (Theme::space4), gateIcon, gateText, gateColour, Theme::fontS);
        r.removeFromTop (Theme::space2);
        drawText (g, ja ("入力レベル"), r.removeFromTop (kTextH), Theme::fontXS, p.textSub);
        drawText (g, level, r.removeFromTop (Theme::space4), Theme::fontM, p.text, juce::Justification::centredLeft, true, true);
    }

private:
    static constexpr int kTextH = Theme::space3 + 2;
    AppController& c;
    juce::ComboBox device;
    juce::StringArray items;
    LevelMeter meterV, meterH;
    bool compact = false, gateOn = true, gateOpen = false;
    float shown = -100.0f;
    int levelText = -60;
    juce::Rectangle<int> labelArea, gateArea, valueArea, infoArea;
};

// =============================================================================================== 04 出力
class OutputControls : public juce::Component
{
public:
    OutputControls (AppController& ctl, Navigator& n) : c (ctl), nav (n), meterV (true), meterH (false), deviceLink (ja ("設定で変更"))
    {
        setComponentID ("voice.output");
        meterH.setShowScale (false);
        addAndMakeVisible (meterV);
        addChildComponent (meterH);
        deviceLink.setComponentID ("voice.outputDeviceLink");
        deviceLink.setTooltip (ja ("出力先（仮想マイク）は設定のデバイスで選びます"));
        deviceLink.onClick = [this] { nav.showSettings (Navigator::SettingsSection::devices); }; // F-01-11
        addAndMakeVisible (deviceLink);
        setCompact (false);
    }

    void setCompact (bool cp)
    {
        compact = cp;
        meterV.setVisible (! compact);
        meterH.setVisible (compact);
        meterV.setComponentID (compact ? juce::String() : "tour.outputMeter");
        meterH.setComponentID (compact ? "tour.outputMeter" : juce::String());
        resized();
    }

    void refresh()
    {
        deviceName = c.getOutputDevice();
        gainDb = c.getSettings().outputGainDb;
        repaint();
    }

    void tick (const AppController::Meters& m, const AppController::Status& s)
    {
        meterV.setLevel (m.outputDb, m.outputClip);
        meterH.setLevel (m.outputDb, m.outputClip);
        shown = m.outputDb > shown ? m.outputDb : juce::jmax (m.outputDb, shown - 0.7f);
        const int lv = juce::jmax (-60, juce::roundToInt (shown));
        const bool clip = (compact ? meterH : meterV).isClipping();
        if (lv != levelText || clip != clipping || s.limiterActive != limiter)
        {
            levelText = lv;
            clipping = clip;
            limiter = s.limiterActive;
            repaint();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (compact)
        {
            auto row = r.removeFromTop (Theme::space4 + Theme::space1);
            labelArea = row.removeFromLeft (Theme::space5);
            row.removeFromLeft (Theme::space2);
            gainArea = row.removeFromRight (textWidth (Theme::mono (Theme::fontXS), "-24.0 dB") + Theme::space1);
            row.removeFromRight (Theme::space2);
            deviceLink.setBounds (row.removeFromRight (deviceLink.preferredWidth()));
            row.removeFromRight (Theme::space1);
            nameArea = row;
            r.removeFromTop (Theme::space1 + 2);
            auto meterRow = r.removeFromTop (Theme::space2 + Theme::space1);
            valueArea = meterRow.removeFromRight (Theme::space5);
            meterRow.removeFromRight (Theme::space2);
            meterH.setBounds (meterRow);
            return;
        }
        r.removeFromTop (kRow + Theme::space2);
        // short cards (banners, small windows): one line per value and the meter beside everything
        tall = getHeight() >= kRow + Theme::space2 + 108 + Theme::space2 + 66;
        if (! tall)
        {
            meterV.setBounds (r.removeFromLeft (kMeterW).withTrimmedTop (Theme::space2));
            r.removeFromLeft (Theme::space3);
            infoArea = r;
            deviceLink.setBounds (r.removeFromBottom (Theme::space4 - 2).withWidth (deviceLink.preferredWidth()));
            nameArea = r.removeFromBottom (Theme::space4 - 2);
            destLabel = r.removeFromBottom (Theme::space3);
            return;
        }
        auto bottom = r.removeFromBottom (Theme::space3 + 2 + Theme::space4 + Theme::space4);
        destLabel = bottom.removeFromTop (Theme::space3 + 2);
        nameArea = bottom.removeFromTop (Theme::space4);
        deviceLink.setBounds (bottom.withWidth (deviceLink.preferredWidth()));
        r.removeFromBottom (Theme::space2);
        meterV.setBounds (r.removeFromLeft (kMeterW).withTrimmedTop (Theme::space2));
        r.removeFromLeft (Theme::space3);
        infoArea = r;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const bool unset = deviceName.isEmpty();
        const bool cable = AppController::isCableInputName (deviceName);
        const auto nameText = unset ? ja ("未選択") : deviceName;
        const auto nameIcon = cable ? Icon::check : Icon::warning;
        const auto nameColour = cable ? p.ok : p.warn;
        const auto gain = juce::String (gainDb, 1) + " dB";
        if (compact)
        {
            drawText (g, ja ("出力"), labelArea, Theme::fontXS, p.textSub);
            auto n = nameArea;
            drawIcon (g, nameIcon, n.removeFromLeft (16).withSizeKeepingCentre (16, 16).toFloat(), nameColour, 2.6f);
            n.removeFromLeft (Theme::space1 + 2);
            drawText (g, nameText, n, Theme::fontXS, unset ? p.warn : p.text, juce::Justification::centredLeft, true);
            drawText (g, gain, gainArea, Theme::fontXS, p.text, juce::Justification::centredRight, false, true);
            drawText (g, juce::String (levelText), valueArea, Theme::fontXS, p.text, juce::Justification::centredRight, true, true);
            return;
        }
        drawCardHeader (g, getLocalBounds().removeFromTop (kRow), "04", ja ("出力"));
        auto r = infoArea;
        auto clipRow = [&] (juce::Rectangle<int> row)
        {
            if (clipping) drawStatus (g, row, Icon::warning, ja ("あり"), p.danger, Theme::fontS);
            else if (limiter) drawStatus (g, row, Icon::warning, ja ("リミッター動作中"), p.warn, Theme::fontXS); // F-08-2
            else drawStatus (g, row, Icon::check, ja ("なし"), p.ok, Theme::fontS);
        };
        if (tall)
        {
            drawText (g, ja ("出力ゲイン"), r.removeFromTop (kTextH), Theme::fontXS, p.textSub);
            drawText (g, gain, r.removeFromTop (Theme::space4 - 2), Theme::fontM, p.text, juce::Justification::centredLeft, true, true);
            drawText (g, ja ("設定で変更"), r.removeFromTop (kTextH), Theme::fontXS, p.textSub);
            r.removeFromTop (Theme::space2);
            drawText (g, ja ("クリップ"), r.removeFromTop (kTextH), Theme::fontXS, p.textSub);
            clipRow (r.removeFromTop (Theme::space4));
        }
        else
        {
            auto row = r.removeFromTop (Theme::space4 - 2);
            drawText (g, ja ("出力ゲイン"), row, Theme::fontXS, p.textSub);
            drawText (g, gain, row, Theme::fontS, p.text, juce::Justification::centredRight, true, true);
            r.removeFromTop (Theme::space1);
            row = r.removeFromTop (Theme::space4 - 2);
            drawText (g, ja ("クリップ"), row.removeFromLeft (Theme::space5 * 2 - Theme::space2), Theme::fontXS, p.textSub);
            clipRow (row);
        }

        drawText (g, ja ("出力先"), destLabel, Theme::fontXS, p.textSub);
        auto n = nameArea;
        drawIcon (g, nameIcon, n.removeFromLeft (16).withSizeKeepingCentre (16, 16).toFloat(), nameColour, 2.6f);
        n.removeFromLeft (Theme::space2);
        drawText (g, nameText, n, Theme::fontS, unset ? p.warn : p.text, juce::Justification::centredLeft, true);
    }

private:
    static constexpr int kTextH = Theme::space3 + 2;
    AppController& c;
    Navigator& nav;
    LevelMeter meterV, meterH;
    LinkButton deviceLink;
    bool compact = false, clipping = false, limiter = false, tall = true;
    juce::String deviceName;
    float gainDb = 0.0f, shown = -100.0f;
    int levelText = -60;
    juce::Rectangle<int> labelArea, nameArea, gainArea, valueArea, infoArea, destLabel;
};

// =============================================================================================== 02 声の変換
class ShifterControls : public juce::Component
{
public:
    explicit ShifterControls (AppController& ctl) : c (ctl), pitch (Knob::Size::big), formant (Knob::Size::big)
    {
        setComponentID ("voice.shifter");
        pitch.setComponentID ("tour.pitch");
        formant.setComponentID ("tour.formant");
        pitch.setup (kPitchSt.min, kPitchSt.max, kPitchSt.def, kPitchStep, formatSemitones, "st");
        formant.setup (kFormantSt.min, kFormantSt.max, kFormantSt.def, kPitchStep, formatSemitones, "st");
        pitch.setLabel (ja ("ピッチ"));
        formant.setLabel (ja ("フォルマント"));
        pitch.setTitle (ja ("ピッチ（-12〜+12 st、初期値 0）"));
        formant.setTitle (ja ("フォルマント（-6〜+6 st、初期値 0）"));
        pitch.onValueChange = [this] { c.setPitch (float (pitch.getValue())); };
        formant.onValueChange = [this] { c.setFormant (float (formant.getValue())); };
        addAndMakeVisible (pitch);
        addAndMakeVisible (formant);
        toggle.setComponentID ("voice.shifterToggle");
        toggle.setTitle (ja ("変換 ON/OFF"));
        toggle.setTooltip (ja ("声の変換の ON/OFF（OFF の間は重ねる声も鳴りません）"));
        toggle.onClick = [this] { c.setShifterEnabled (toggle.getToggleState()); };
        addAndMakeVisible (toggle);
    }

    void setCompact (bool cp) { compact = cp; resized(); repaint(); }

    void refresh()
    {
        pitch.setValue (c.getPitch(), juce::dontSendNotification);
        formant.setValue (c.getFormant(), juce::dontSendNotification);
        toggle.setToggleState (c.hasShifter(), juce::dontSendNotification);
        pitch.repaint();
        formant.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (kRow);
        toggle.setBounds (header.removeFromRight (kToggleW - (compact ? Theme::space2 : 0)));
        header.removeFromRight (Theme::space2);
        toggleLabel = header.removeFromRight (textWidth (Theme::ui (Theme::fontXS, true), ja ("変換")) + Theme::space1);
        r.removeFromTop (compact ? Theme::space1 : Theme::space2);
        const int kw = compact ? kKnobNarrow : Theme::knobBig, kh = kw + int (Theme::fontS) + Theme::space2;
        hintArea = compact ? juce::Rectangle<int>() : r.removeFromBottom (Theme::space3);
        showHint = ! compact && r.getHeight() >= kh;
        const int y = r.getY() + juce::jmax (0, (r.getHeight() - kh) / 2);
        const int cell = r.getWidth() / 2;
        int i = 0;
        for (auto* k : { &pitch, &formant })
        {
            k->setDiameter (kw);
            k->setBounds (r.getX() + cell * i + (cell - kw) / 2, y, kw, kh);
            ++i;
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        drawCardHeader (g, getLocalBounds().removeFromTop (kRow), compact ? juce::String() : "02", ja ("声の変換"));
        drawText (g, ja ("変換"), toggleLabel, Theme::fontXS, p.text, juce::Justification::centredRight, true);
        if (showHint) drawText (g, ja ("ダブルクリックで 0 に戻る ・ Shift で微調整"), hintArea, Theme::fontXS, p.textSub, juce::Justification::centred);
    }

private:
    AppController& c;
    Knob pitch, formant;
    ToggleSwitch toggle;
    bool compact = false, showHint = true;
    juce::Rectangle<int> toggleLabel, hintArea;
};

// =============================================================================================== 03 重ねる声
juce::String layerSummary (const LayerDef& d)
{
    return (d.mode == LayerDef::Mode::scale ? degreeText (d.degree) : stText (d.pitchSt)) + ja (" ・ ") + dbText (d.levelDb);
}

/** Full editor of one voice (S-01 card 03, also inside LayerPanel). */
class LayerBox : public juce::Component
{
public:
    static constexpr int height = Theme::space2 * 2 + 20 + Theme::space1 + 3 * Theme::space3 + 2 * 2; // 92: one line per value
    static constexpr int kStackGap = Theme::space1 + 2;
    /** The mock's layout: label and value over each slider. */
    static constexpr int stackedHeight = Theme::space2 * 2 + 20 + Theme::space2 + 3 * SliderRow::stackedHeight + 2 * kStackGap; // 152

    LayerBox (AppController& ctl, Navigator& n, int i, bool framed)
        : c (ctl), nav (n), index (i), frame (framed), remove (ja ("この声を削除"), Icon::close)
    {
        setComponentID ("voice.layer." + juce::String (index));
        toggle.setComponentID ("voice.layer." + juce::String (index) + ".toggle");
        toggle.setTitle (ja ("声 ") + juce::String (index + 1) + " ON/OFF");
        toggle.onClick = [this] { c.setLayerEnabled (index, toggle.getToggleState()); };
        addAndMakeVisible (toggle);
        remove.onClick = [this] { c.removeLayer (index); };
        addAndMakeVisible (remove);
        const auto d = c.getLayer (index);
        scale = d.mode == LayerDef::Mode::scale;
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
            row->slider.setTooltip (ja ("声 ") + juce::String (index + 1) + ja ("の") + row->slider.getTitle());
            addAndMakeVisible (row);
        }
        update (d);
    }

    bool isScale() const { return scale; }

    void setStacked (bool s)
    {
        stacked = s;
        for (auto* row : rows) row->stacked = s;
        resized();
    }

    void update (const LayerDef& d)
    {
        toggle.setToggleState (d.enabled, juce::dontSendNotification);
        rows[0]->slider.setValue (scale ? double (d.degree) : double (d.pitchSt), juce::dontSendNotification);
        rows[1]->slider.setValue (d.formantSt, juce::dontSendNotification);
        rows[2]->slider.setValue (d.levelDb, juce::dontSendNotification);
        for (auto* row : rows) row->repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (frame ? Theme::space2 + Theme::space1 : 0, frame ? Theme::space2 : 0);
        auto header = r.removeFromTop (20);
        toggle.setBounds (header.removeFromRight (kToggleW));
        header.removeFromRight (Theme::space1);
        remove.setBounds (header.removeFromRight (20));
        r.removeFromTop (stacked ? Theme::space2 : Theme::space1);
        for (auto* row : rows)
        {
            row->setBounds (r.removeFromTop (stacked ? SliderRow::stackedHeight : Theme::space3));
            r.removeFromTop (stacked ? kStackGap : 2);
            row->resized();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        if (frame)
        {
            auto b = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (p.raised);
            g.fillRoundedRectangle (b, Theme::radiusM);
            g.setColour (p.divider);
            g.drawRoundedRectangle (b, Theme::radiusM, 1.0f);
        }
        auto r = getLocalBounds().reduced (frame ? Theme::space2 + Theme::space1 : 0, frame ? Theme::space2 : 0).removeFromTop (20);
        drawText (g, ja ("声 ") + juce::String (index + 1) + (scale ? ja ("（スケール連動）") : juce::String()), r.withTrimmedRight (kToggleW + 24),
                  Theme::fontS, p.text, juce::Justification::centredLeft, true);
    }

private:
    AppController& c;
    Navigator& nav;
    const int index;
    const bool frame;
    bool scale = false, stacked = false;
    ToggleSwitch toggle;
    SquareIconButton remove;
    juce::OwnedArray<SliderRow> rows;
};

/** Overlay with one voice's editor (compact layout: the card shows rows only). */
class LayerPanel : public PanelBase, private juce::ChangeListener
{
public:
    LayerPanel (AppController& ctl, Navigator& n, int i)
        : PanelBase (n, ja ("声 ") + juce::String (i + 1)), c (ctl), index (i), box (ctl, n, i, false)
    {
        subtitle = ja ("重ねる声");
        box.setStacked (true);
        addAndMakeVisible (box);
        c.addChangeListener (this);
        setSize (Theme::space5 * 15, headerHeight + LayerBox::stackedHeight + Theme::space4);
    }
    ~LayerPanel() override { c.removeChangeListener (this); }

    void resized() override
    {
        PanelBase::resized();
        box.setBounds (contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4));
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (index >= c.getNumLayers()) nav.closeOverlay();
        else box.update (c.getLayer (index));
    }
    AppController& c;
    const int index;
    LayerBox box;
};

/** One voice as a row ("声 1 / -12 st ・ -10 dB / ON"); click or Enter opens LayerPanel. */
class LayerRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int height = Theme::space5 + Theme::space1; // 36

    LayerRow (AppController& ctl, Navigator& n, int i) : c (ctl), nav (n), index (i)
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
        summary = layerSummary (d);
        toggle.setToggleState (d.enabled, juce::dontSendNotification);
        repaint();
    }

    void resized() override { toggle.setBounds (getLocalBounds().removeFromTop (height / 2 + 2).removeFromRight (kToggleW - Theme::space2)); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        if (isMouseOver (true)) { g.setColour (p.raised); g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusS); }
        auto r = getLocalBounds().reduced (Theme::space1, 0);
        auto top = r.removeFromTop (height / 2 + 2);
        drawText (g, ja ("声 ") + juce::String (index + 1), top.withTrimmedRight (kToggleW), Theme::fontXS, p.text, juce::Justification::centredLeft, true);
        drawText (g, summary, r, Theme::fontXS, toggle.getToggleState() ? p.text : p.textSub, juce::Justification::centredLeft, false, true);
        if (hasKeyboardFocus (false)) drawFocusRing (g, getLocalBounds().toFloat(), Theme::radiusS);
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

private:
    void open() { nav.showOverlay (std::make_unique<LayerPanel> (c, nav, index)); }
    AppController& c;
    Navigator& nav;
    const int index;
    ToggleSwitch toggle;
    juce::String summary;
};

class LayersControls : public juce::Component
{
public:
    LayersControls (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("声を追加")), resume (ja ("再開"))
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
    }

    void setCompact (bool cp) { compact = cp; sig = {}; refresh(); }

    void refresh()
    {
        const int n = c.getNumLayers();
        const bool full = ! compact && getHeight() >= fullHeight (n, LayerBox::height);
        stacked = full && getHeight() >= fullHeight (n, LayerBox::stackedHeight);
        juce::String s = juce::String (n) + (full ? "F" : "R");
        for (int i = 0; i < n; ++i) s << (c.getLayer (i).mode == LayerDef::Mode::scale ? "s" : "f");
        if (s != sig)
        {
            sig = s;
            boxes.clear();
            rowsList.clear();
            for (int i = 0; i < n; ++i)
            {
                if (full) addAndMakeVisible (boxes.add (new LayerBox (c, nav, i, true)));
                else addAndMakeVisible (rowsList.add (new LayerRow (c, nav, i)));
            }
            resized();
        }
        for (int i = 0; i < boxes.size(); ++i)
        {
            boxes[i]->setStacked (stacked);
            boxes[i]->update (c.getLayer (i));
        }
        for (int i = 0; i < rowsList.size(); ++i) rowsList[i]->update (c.getLayer (i));
        add.setVisible (n < kMaxLayers);
        add.setButtonText (compact ? ja ("声を追加") : ja ("声を追加（あと ") + juce::String (kMaxLayers - n) + ja (" 声）"));
        autoStopped = c.areLayersAutoStopped();
        resume.setVisible (autoStopped);
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (compact ? Theme::space3 + 2 : kRow);
        if (resume.isVisible()) resume.setBounds (header.removeFromRight (resume.preferredWidth()));
        r.removeFromTop (compact ? Theme::space1 : Theme::space2);
        for (auto* b : boxes)
        {
            b->setBounds (r.removeFromTop (stacked ? LayerBox::stackedHeight : LayerBox::height));
            r.removeFromTop (Theme::space2);
        }
        for (auto* row : rowsList)
        {
            row->setBounds (r.removeFromTop (LayerRow::height));
            r.removeFromTop (2);
        }
        if (add.isVisible())
        {
            const int h = compact ? Theme::space4 + Theme::space1 : Theme::buttonH;
            add.setBounds (r.removeFromTop (h));
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto header = getLocalBounds().removeFromTop (compact ? Theme::space3 + 2 : kRow);
        if (compact) drawText (g, ja ("重ねる声"), header.removeFromLeft (Theme::space5 * 2), Theme::fontXS, p.text, juce::Justification::centredLeft, true);
        else header = drawCardHeader (g, header, "03", ja ("重ねる声（0〜2）"));
        if (autoStopped)
        {
            header.removeFromRight (resume.preferredWidth() + Theme::space1);
            drawText (g, ja ("自動停止"), header, Theme::fontXS, p.warn, juce::Justification::centredRight, true);
        }
        else
            drawText (g, juce::String (c.getNumLayers()) + " / " + juce::String (kMaxLayers), header, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
        if (c.getNumLayers() == 0 && ! compact)
            drawText (g, ja ("重ねる声はありません"), getLocalBounds().withTrimmedTop (kRow + Theme::space2 + Theme::buttonH + Theme::space2).withHeight (Theme::space4),
                      Theme::fontXS, p.textSub, juce::Justification::centred);
    }

private:
    static int fullHeight (int n, int boxH)
    {
        return kRow + Theme::space2 + n * boxH + juce::jmax (0, n - 1) * Theme::space2 + (n < kMaxLayers ? Theme::space2 + Theme::buttonH : 0);
    }
    AppController& c;
    Navigator& nav;
    bool compact = false, autoStopped = false, stacked = false;
    juce::String sig;
    juce::OwnedArray<LayerBox> boxes;
    juce::OwnedArray<LayerRow> rowsList;
    DashedButton add;
    LinkButton resume;
};

// =============================================================================================== bottom bar
class MonitorPanel : public PanelBase, private juce::ChangeListener
{
public:
    MonitorPanel (AppController& ctl, Navigator& n) : PanelBase (n, ja ("モニター")), c (ctl), volume (ja ("音量"), dbText)
    {
        volume.slider.setRange (kMonitorVolumeDb.min, kMonitorVolumeDb.max, 0.5);
        volume.slider.setDoubleClickReturnValue (true, kMonitorVolumeDb.def);
        volume.onChange = [this] (double v) { c.setMonitorVolumeDb (float (v)); };
        addAndMakeVisible (volume);
        device.setTitle (ja ("モニターのデバイス"));
        device.setTextWhenNothingSelected (ja ("未選択"));
        device.setTextWhenNoChoicesAvailable (ja ("使えるデバイスがありません"));
        device.onChange = [this] { if (device.getSelectedId() > 0) c.setMonitorDevice (device.getText()); };
        addAndMakeVisible (device);
        c.addChangeListener (this);
        update();
        setSize (Theme::space5 * 15, headerHeight + Theme::space3 * 2 + Theme::controlH * 2 + Theme::space3 + Theme::space4 * 2);
    }
    ~MonitorPanel() override { c.removeChangeListener (this); }

    void resized() override
    {
        PanelBase::resized();
        auto r = contentArea().reduced (Theme::space4, 0);
        volume.setBounds (r.removeFromTop (Theme::controlH));
        r.removeFromTop (Theme::space2);
        device.setBounds (r.removeFromTop (Theme::controlH).withTrimmedLeft (volume.labelWidth));
        r.removeFromTop (Theme::space2);
        noteArea = r;
    }

    void paint (juce::Graphics& g) override
    {
        PanelBase::paint (g);
        drawText (g, ja ("デバイス"), device.getBounds().withX (Theme::space4).withWidth (volume.labelWidth), Theme::fontXS, P().textSub);
        drawText (g, ja ("スピーカーで使うとハウリングします。ヘッドホンで聞いてください。"), noteArea.removeFromTop (Theme::space4), Theme::fontXS, P().textSub);
    }

private:
    void update()
    {
        volume.slider.setValue (c.getSettings().monitorVolumeDb, juce::dontSendNotification);
        volume.repaint();
        fillCombo (device, c.getMonitorDevices(), items, c.getMonitorDevice());
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { update(); }
    AppController& c;
    SliderRow volume;
    juce::ComboBox device;
    juce::StringArray items;
    juce::Rectangle<int> noteArea;
};

class MonitorGroup : public juce::Component
{
public:
    MonitorGroup (AppController& ctl, Navigator& n) : c (ctl), nav (n), open (ja ("モニター"), true)
    {
        setComponentID ("tour.monitor");
        toggle.setComponentID ("voice.monitorToggle");
        toggle.setTitle (ja ("モニター ON/OFF"));
        toggle.setTooltip (ja ("自分の声を聞く（スピーカーだとハウリングします）"));
        toggle.onClick = [this]
        {
            const bool want = toggle.getToggleState();
            c.setMonitorOn (want);
            if (want && c.isMonitorOn() && c.isMonitorDeviceSpeaker())
                nav.showToast (ja ("スピーカーでモニターするとハウリングします。ヘッドホンで聞いてください。")); // F-08-5
            toggle.setToggleState (c.isMonitorOn(), juce::dontSendNotification);
        };
        addAndMakeVisible (toggle);
        volume.setComponentID ("voice.monitorVolume");
        volume.setTitle (ja ("モニターの音量"));
        volume.setRange (kMonitorVolumeDb.min, kMonitorVolumeDb.max, 0.5);
        volume.setDoubleClickReturnValue (true, kMonitorVolumeDb.def);
        volume.onValueChange = [this] { c.setMonitorVolumeDb (float (volume.getValue())); repaint(); };
        addAndMakeVisible (volume);
        device.setComponentID ("voice.monitorDevice");
        device.setTitle (ja ("モニターのデバイス"));
        device.setTextWhenNothingSelected (ja ("未選択"));
        device.setTextWhenNoChoicesAvailable (ja ("デバイスなし"));
        device.onChange = [this] { if (device.getSelectedId() > 0) c.setMonitorDevice (device.getText()); };
        addAndMakeVisible (device);
        open.setTooltip (ja ("モニターの音量とデバイス"));
        open.onClick = [this] { nav.showOverlay (std::make_unique<MonitorPanel> (c, nav)); };
        addChildComponent (open);
    }

    void setCompact (bool cp)
    {
        compact = cp;
        volume.setVisible (! compact);
        device.setVisible (! compact);
        open.setVisible (compact);
    }

    void clearIds()
    {
        for (auto* comp : std::initializer_list<juce::Component*> { this, &toggle, &volume, &device }) comp->setComponentID ({});
    }

    int preferredWidth() const
    {
        const int base = 18 + Theme::space2 + textWidth (Theme::ui (Theme::fontXS), ja ("モニター")) + Theme::space2 + Theme::space2 + kToggleW;
        return compact ? base : base + Theme::space2 + kVolumeW + Theme::space2 + kValueW + Theme::space2 + kDeviceW;
    }

    void refresh()
    {
        toggle.setToggleState (c.isMonitorOn(), juce::dontSendNotification);
        volume.setValue (c.getSettings().monitorVolumeDb, juce::dontSendNotification);
        fillCombo (device, c.getMonitorDevices(), items, c.getMonitorDevice());
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromLeft (18 + Theme::space2);
        const int labelW = textWidth (Theme::ui (Theme::fontXS), ja ("モニター")) + Theme::space2;
        labelArea = r.removeFromLeft (labelW);
        open.setBounds (labelArea);
        r.removeFromLeft (Theme::space2);
        toggle.setBounds (r.removeFromLeft (kToggleW));
        if (compact) return;
        r.removeFromLeft (Theme::space2);
        volume.setBounds (r.removeFromLeft (kVolumeW));
        r.removeFromLeft (Theme::space2);
        valueArea = r.removeFromLeft (kValueW);
        r.removeFromLeft (Theme::space2);
        device.setBounds (r.removeFromLeft (kDeviceW).withSizeKeepingCentre (kDeviceW, Theme::space5 - 2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        drawIcon (g, Icon::headphones, juce::Rectangle<float> (0.0f, (getHeight() - 18) * 0.5f, 18.0f, 18.0f), p.textSub);
        if (! compact)
        {
            drawText (g, ja ("モニター"), labelArea, Theme::fontXS, p.textSub);
            drawText (g, dbText (volume.getValue()), valueArea, Theme::fontXS, p.text, juce::Justification::centredLeft, false, true);
        }
    }

private:
    static constexpr int kVolumeW = Theme::space5 * 3 + Theme::space2 + 6, kValueW = Theme::space5 * 2 - 8, kDeviceW = Theme::space5 * 5 + Theme::space2 + 2;
    AppController& c;
    Navigator& nav;
    bool compact = false;
    ToggleSwitch toggle;
    LineSlider volume;
    juce::ComboBox device;
    juce::StringArray items;
    LinkButton open;
    juce::Rectangle<int> labelArea, valueArea;
};

/** Latency / XRUN / CPU / 自動停止 (F-08-3, F-08-4, F-08-7). Paint only. */
class StatusView : public juce::Component, public juce::SettableTooltipClient
{
public:
    StatusView()
    {
        setComponentID ("tour.status");
        setTooltip (ja ("推定遅延（100 ms を超えると警告）、音切れの回数、音声処理の CPU 負荷"));
    }

    void setCompact (bool cp) { compact = cp; repaint(); }

    bool update (const AppController::Status& s, bool anyAutoStopped)
    {
        const int lat = juce::roundToInt (s.latencyMs), cpuNow = juce::roundToInt (s.cpuPercent);
        if (lat == latency && s.latencyWarn == warn && s.xruns == xruns && cpuNow == cpu && anyAutoStopped == stopped) return false;
        latency = lat;
        warn = s.latencyWarn;
        xruns = s.xruns;
        cpu = cpuNow;
        stopped = anyAutoStopped;
        repaint();
        return true;
    }

    int preferredWidth() const
    {
        int w = 0;
        for (auto& item : items()) w += item.width + gap();
        return w - gap();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto r = getLocalBounds();
        for (auto& item : items())
        {
            auto cell = r.removeFromLeft (item.width);
            r.removeFromLeft (gap());
            const auto lf = Theme::ui (fontSize());
            drawText (g, item.label, cell.removeFromLeft (textWidth (lf, item.label)), fontSize(), item.labelColour);
            cell.removeFromLeft (Theme::space2);
            const auto vf = Theme::mono (fontSize(), true);
            drawText (g, item.value, cell.removeFromLeft (textWidth (vf, item.value) + 2), fontSize(), item.valueColour, juce::Justification::centredLeft, true, true);
            if (item.icon)
            {
                cell.removeFromLeft (Theme::space1 + 2);
                drawIcon (g, *item.icon, cell.removeFromLeft (15).withSizeKeepingCentre (15, 15).toFloat(), item.valueColour, 2.6f);
            }
            if (item.bar)
            {
                cell.removeFromLeft (Theme::space2);
                auto bar = cell.removeFromLeft (kBarW).withSizeKeepingCentre (kBarW, 6).toFloat();
                g.setColour (p.trackOff);
                g.fillRoundedRectangle (bar, 3.0f);
                g.setColour (cpu >= 70 ? p.warn : p.ok);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, cpu / 100.0f)), 3.0f);
            }
        }
    }

private:
    struct Item
    {
        juce::String label, value;
        juce::Colour labelColour, valueColour;
        std::optional<Icon> icon;
        bool bar = false;
        int width = 0;
    };

    float fontSize() const { return compact ? Theme::fontXS : Theme::fontS; }
    int gap() const { return compact ? Theme::space3 : Theme::space4; }

    std::vector<Item> items() const
    {
        const auto& p = P();
        std::vector<Item> v;
        auto latencyValue = juce::String (latency) + " ms";
        if (warn) latencyValue << (compact ? ja (" 超過") : ja ("（100 ms 超）"));
        v.push_back ({ compact ? ja ("遅延") : ja ("推定遅延"), latencyValue, p.textSub, warn ? p.warn : p.text, warn ? std::optional<Icon> (Icon::warning) : std::nullopt });
        v.push_back ({ "XRUN", juce::String (xruns), p.textSub, xruns > 0 ? p.warn : p.ok, xruns > 0 ? Icon::warning : Icon::check });
        v.push_back ({ "CPU", juce::String (cpu) + " %", p.textSub, cpu >= 70 ? p.warn : p.text, std::nullopt, ! compact });
        if (stopped) v.push_back ({ ja ("自動停止"), ja ("あり"), p.warn, p.warn, Icon::warning });
        for (auto& item : v)
        {
            item.width = textWidth (Theme::ui (fontSize()), item.label) + Theme::space2 + textWidth (Theme::mono (fontSize(), true), item.value) + 2;
            if (item.icon) item.width += Theme::space1 + 2 + 15;
            if (item.bar) item.width += Theme::space2 + kBarW;
        }
        return v;
    }

    static constexpr int kBarW = Theme::space5 * 2 + Theme::space2;
    bool compact = false, warn = false, stopped = false;
    int latency = 0, xruns = 0, cpu = 0;
};
} // namespace

// =============================================================================================== BottomBar
// =============================================================================================== VoiceExtras
struct VoiceExtras::Impl : private juce::ChangeListener
{
    Impl (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), compare (ExtraButton::Kind::compare, ctl), random (ExtraButton::Kind::random, ctl)
    {
        random.onClick = [this]
        {
            juce::String why;
            if (c.randomizeCurrent (uint32_t (juce::Random::getSystemRandom().nextInt()), why))
                nav.showToast (ja ("おまかせで声を作りました。気に入ったら「保存」で残せます。"));
            else nav.showToast (why);
        };
        c.addChangeListener (this);
        sync();
    }
    ~Impl() override { c.removeChangeListener (this); }

    void sync()
    {
        compare.setEnabled (c.isVoiceChangerOn()); // a disabled button cannot start a hold
        if (! c.isVoiceChangerOn()) compare.setHeld (false);
        compare.syncFromController();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { sync(); }

    AppController& c;
    Navigator& nav;
    ExtraButton compare, random;
    bool vertical = false;
};

VoiceExtras::VoiceExtras (AppController& c, Navigator& nav) : impl (std::make_unique<Impl> (c, nav))
{
    addAndMakeVisible (impl->compare);
    addAndMakeVisible (impl->random);
}

VoiceExtras::~VoiceExtras() = default;

void VoiceExtras::setStyle (bool vertical, bool randomText, float fontSize)
{
    impl->vertical = vertical;
    impl->random.showText = randomText;
    impl->compare.fontSize = impl->random.fontSize = fontSize;
    resized();
    repaint();
}

int VoiceExtras::preferredWidth (int height) const
{
    auto& i = *impl;
    if (i.vertical) return juce::jmax (i.compare.preferredWidth (height), i.random.preferredWidth (height));
    return i.compare.preferredWidth (height) + Theme::space2 + i.random.preferredWidth (height);
}

void VoiceExtras::resized()
{
    auto& i = *impl;
    auto r = getLocalBounds();
    if (i.vertical)
    {
        const int h = (r.getHeight() - Theme::space2) / 2;
        i.compare.setBounds (r.removeFromTop (h));
        i.random.setBounds (r.removeFromBottom (h));
        return;
    }
    i.random.setBounds (r.removeFromRight (juce::jmin (r.getWidth() / 2, i.random.preferredWidth (r.getHeight()))));
    r.removeFromRight (Theme::space2);
    i.compare.setBounds (r);
}

struct BottomBar::Impl
{
    Impl (AppController& ctl, Navigator& n) : c (ctl), monitor (ctl, n) {}
    AppController& c;
    MonitorGroup monitor;
    StatusView status;
    int ticks = 0;
};

BottomBar::BottomBar (AppController& c, Navigator& nav, bool tourTarget) : impl (std::make_unique<Impl> (c, nav))
{
    addAndMakeVisible (impl->monitor);
    addAndMakeVisible (impl->status);
    if (! tourTarget)
    {
        impl->monitor.clearIds();
        impl->status.setComponentID ({});
    }
    refresh();
}

BottomBar::~BottomBar() = default;

void BottomBar::setCompact (bool compact)
{
    impl->monitor.setCompact (compact);
    impl->status.setCompact (compact);
    resized();
}

void BottomBar::refresh() { impl->monitor.refresh(); }

void BottomBar::tick (const AppController::Status& s)
{
    auto& i = *impl;
    if (++i.ticks % 5 != 0) return; // auto-stop flags live in the engine; no change message for them
    bool stopped = i.c.areLayersAutoStopped();
    for (int k = 0; k < int (i.c.getChain().size()); ++k) stopped = stopped || i.c.isSlotAutoStopped (k);
    if (i.status.update (s, stopped) && i.status.getWidth() != i.status.preferredWidth()) resized();
}

/** Monitor on the left, status on the right (status width follows its numbers). */
void BottomBar::resized()
{
    auto& i = *impl;
    auto b = getLocalBounds().reduced (Theme::space1, 0);
    i.status.setBounds (b.removeFromRight (juce::jmin (b.getWidth(), i.status.preferredWidth())));
    i.monitor.setBounds (b.removeFromLeft (juce::jmin (b.getWidth() - Theme::space3, i.monitor.preferredWidth())));
}

// =============================================================================================== VoiceView
struct VoiceView::Impl
{
    Impl (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), presets (ctl, n), input (ctl), output (ctl, n), shifter (ctl), layers (ctl, n), chain (ctl, n), bottom (ctl, n, true)
    {
    }

    AppController& c;
    Navigator& nav;
    bool compact = false;
    PresetBar presets;
    InputControls input;
    OutputControls output;
    ShifterControls shifter;
    LayersControls layers;
    ChainStrip chain;
    BottomBar bottom; // voice.bottom
    std::vector<juce::Rectangle<int>> frames;
    int ticks = 0, layerDividerX = 0;
};

VoiceView::VoiceView (AppController& c, Navigator& nav) : impl (std::make_unique<Impl> (c, nav))
{
    setComponentID ("voice.view");
    auto& i = *impl;
    for (auto* comp : std::initializer_list<juce::Component*> { &i.presets, &i.input, &i.output, &i.shifter, &i.layers, &i.chain, &i.bottom })
        addAndMakeVisible (comp);
    i.bottom.setComponentID ("voice.bottom");
    refresh();
}

VoiceView::~VoiceView() = default;

ChainStrip& VoiceView::chainStrip() { return impl->chain; }
bool VoiceView::isCompact() const { return impl->compact; }

void VoiceView::setCompact (bool compact)
{
    auto& i = *impl;
    if (i.compact == compact && ! i.frames.empty()) return;
    i.compact = compact;
    i.presets.setCompact (compact);
    i.input.setCompact (compact);
    i.output.setCompact (compact);
    i.shifter.setCompact (compact);
    i.layers.setCompact (compact);
    i.chain.setCompact (compact);
    i.bottom.setCompact (compact);
    resized();
    repaint();
}

void VoiceView::refresh()
{
    auto& i = *impl;
    i.presets.refresh();
    i.input.refresh();
    i.output.refresh();
    i.shifter.refresh();
    i.layers.refresh();
    i.chain.refresh();
    i.bottom.refresh();
}

void VoiceView::tick()
{
    auto& i = *impl;
    const auto m = i.c.pollMeters();
    const auto s = i.c.getStatus();
    i.input.tick (m, s);
    i.output.tick (m, s);
    i.bottom.tick (s);
    if (++i.ticks % 5 == 0) i.chain.refresh(); // auto-stop badges
}

void VoiceView::resized()
{
    auto& i = *impl;
    const bool cp = i.compact;
    const int gap = cp ? Theme::space2 : Theme::space3;
    auto r = getLocalBounds();
    i.presets.setBounds (r.removeFromTop (cp ? Theme::pillH : Theme::headerH));
    r.removeFromTop (gap);
    i.bottom.setBounds (r.removeFromBottom (BottomBar::height (cp)));
    r.removeFromBottom (gap);

    // the chain keeps room for its knobs and ON/OFF (banners shrink it, §8.2.1); the cards give way first
    const int chainMin = ChainStrip::minHeight (cp);
    const int cardsH = juce::jlimit (cp ? kCardsNarrowMin : kCardsWideMin, cp ? kCardsNarrow : kCardsWide, r.getHeight() - gap - chainMin);
    auto cards = r.removeFromTop (cardsH);
    r.removeFromTop (gap);
    i.chain.setBounds (r);

    i.frames.clear();
    const int pad = cp ? Theme::space2 : Theme::space3;
    if (cp)
    {
        const int aW = juce::roundToInt (cards.getWidth() * 0.505f);
        auto a = cards.removeFromLeft (aW);
        cards.removeFromLeft (gap);
        auto b = cards;
        i.frames = { a, b };
        auto ca = a.reduced (Theme::space3, pad);
        const int knobW = kKnobNarrow * 2 + Theme::space3;
        i.shifter.setBounds (ca.removeFromLeft (juce::jmax (knobW, Theme::space5 * 6)));
        ca.removeFromLeft (Theme::space2 + 1 + Theme::space2);
        i.layerDividerX = ca.getX() - Theme::space2;
        i.layers.setBounds (ca);
        auto cb = b.reduced (Theme::space3, pad);
        cb.removeFromTop (kRow + Theme::space1); // "入力と出力"
        const int half = (cb.getHeight() - Theme::space2) / 2;
        i.input.setBounds (cb.removeFromTop (half));
        cb.removeFromTop (Theme::space2);
        i.output.setBounds (cb);
    }
    else
    {
        const float widths[] = { 232.0f, 296.0f, 280.0f, 232.0f };
        const float total = widths[0] + widths[1] + widths[2] + widths[3];
        const int avail = cards.getWidth() - 3 * gap;
        juce::Component* order[] = { &i.input, &i.shifter, &i.layers, &i.output };
        for (int k = 0; k < 4; ++k)
        {
            const int w = k == 3 ? cards.getWidth() : juce::roundToInt (avail * widths[k] / total);
            auto f = cards.removeFromLeft (w);
            cards.removeFromLeft (gap);
            i.frames.push_back (f);
            order[k]->setBounds (f.reduced (pad));
        }
    }
    i.layers.refresh();
}

void VoiceView::paint (juce::Graphics& g)
{
    const auto& p = Theme::colours();
    g.setColour (p.surface);
    for (auto& f : impl->frames) g.fillRoundedRectangle (f.toFloat(), Theme::radiusL);
    if (impl->compact && ! impl->frames.empty())
    {
        auto a = impl->frames[0];
        drawText (g, ja ("入力と出力"), impl->frames[1].reduced (Theme::space3, Theme::space2).removeFromTop (kRow), Theme::fontS, p.text,
                  juce::Justification::centredLeft, true);
        g.setColour (p.divider);
        g.fillRect (impl->layerDividerX, a.getY() + Theme::space2, 1, a.getHeight() - Theme::space3);
    }
}
} // namespace koe::ui::mainui

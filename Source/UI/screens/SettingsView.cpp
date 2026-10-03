// S-03 設定 (docs/mockups/A-S03.dc.html, A-S03-dev.dc.html). Sections without a mock (ホットキー,
// 起動と常駐, 外観, 詳細, 診断) use the same card / row structure.
//
// Wave 4 (INTERFACES.md §7): a search box above the section list, and in each card the everyday rows
// followed by a 「詳細な設定」 disclosure (one open / closed state for every card, Settings::settingsShowDetails).
// While searching, the matching rows of every section (detailed ones too) are shown grouped by section;
// rows stay in their own cards, the search only hides what does not match.
#include "UI/Screens.h"
#include "UI/ThemeLibrary.h"
#include "UI/main/Common.h"
#include "UI/screens/Common.h"
#include "UI/screens/ThemeEditor.h"

#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Platform/Hotkeys.h"

namespace koe::ui
{
using namespace screens;

std::function<juce::File (bool)>& settingsFileChooserForTests()
{
    static std::function<juce::File (bool)> hook;
    return hook;
}

namespace
{
constexpr int kNumSections = 7;

int sectionIndex (Navigator::SettingsSection s) { return int (s); }

/** Search text folded for matching: lower case, full-width ASCII to half-width, katakana to hiragana. */
juce::String fold (const juce::String& s)
{
    juce::String out;
    for (auto p = s.getCharPointer(); ! p.isEmpty();)
    {
        auto ch = p.getAndAdvance();
        if (ch >= 0xFF01 && ch <= 0xFF5E) ch -= 0xFEE0;
        else if (ch == 0x3000) ch = ' ';
        else if (ch >= 0x30A1 && ch <= 0x30F6) ch -= 0x60;
        out += juce::String::charToString (ch);
    }
    return out.toLowerCase();
}

/** S-03 外観: one round swatch per accent colour. The selected one carries a check mark (not colour alone);
    each is a focusable button with the colour's name as title and tooltip. */
class AccentSwatches : public juce::Component
{
public:
    static constexpr int swatch = Theme::touchMin, gap = Theme::space2 + Theme::space1;

    explicit AccentSwatches (bool dark)
    {
        for (int i = 0; i < Theme::numAccents; ++i)
        {
            auto* b = buttons.add (new Swatch (i, dark));
            b->setComponentID ("settings.accent." + juce::String (i));
            b->onClick = [this, i] { if (onChange) onChange (i); };
            addAndMakeVisible (b);
        }
    }

    static int preferredWidth() { return Theme::numAccents * swatch + (Theme::numAccents - 1) * gap; }
    void setSelected (int i)
    {
        for (int k = 0; k < buttons.size(); ++k) buttons[k]->setToggleState (k == i, juce::dontSendNotification);
    }
    juce::Button* getButton (int i) const { return buttons[i]; }
    std::function<void (int)> onChange;

    void resized() override
    {
        auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), swatch);
        for (auto* b : buttons)
        {
            b->setBounds (r.removeFromLeft (swatch));
            r.removeFromLeft (gap);
        }
    }

private:
    struct Swatch : public juce::Button
    {
        Swatch (int i, bool d) : juce::Button (Theme::accentName (i)), index (i), dark (d)
        {
            setTitle (Theme::accentName (i));
            setTooltip (Theme::accentName (i));
            setWantsKeyboardFocus (true);
        }
        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto& p = Theme::colours();
            const auto fill = Theme::accentColour (index, dark);
            const auto r = getLocalBounds().toFloat().reduced (1.0f + Theme::focusRingWidth * 0.5f);
            if (getToggleState() || highlighted)
            {
                g.setColour (getToggleState() ? p.text : p.border); // ring around the selected (or hovered) swatch
                g.drawEllipse (r, Theme::focusRingWidth);
            }
            const auto dot = getLocalBounds().toFloat().reduced (float (Theme::space1) + 1.0f);
            g.setColour (fill);
            g.fillEllipse (dot);
            if (getToggleState())
                drawIcon (g, Icon::check, dot.reduced (dot.getWidth() * 0.22f),
                          Theme::make (dark, index, Theme::tone()).onAccent, 2.4f);
            if (! isEnabled()) { g.setColour (p.surface.withAlpha (0.55f)); g.fillEllipse (dot); } // only used while 配色 is Studio
            if (hasKeyboardFocus (true)) drawFocusRing (g, dot, dot.getHeight() * 0.5f);
        }
        const int index;
        const bool dark; // the Studio theme's (Settings::darkTheme), also while another 配色 is in use
    };
    juce::OwnedArray<Swatch> buttons;
};

/** One hotkey line: action name, assigned key, [割り当て] (captures the next key), [×] clears. */
class HotkeyRow : public juce::Component
{
public:
    HotkeyRow (AppController& c, const juce::String& a) : controller (c), action (a), label (Hotkeys::actionLabel (a), Theme::fontS),
        key ({}, Theme::fontS, Tone::text, true, true), error ({}, Theme::fontXS, Tone::danger), assign (*this), clear (ja ("解除"), Icon::close)
    {
        label.setJustification (juce::Justification::centredLeft);
        label.setMaxLines (1);
        key.setJustification (juce::Justification::centredLeft);
        key.setMaxLines (1);
        addAndMakeVisible (label);
        addAndMakeVisible (key);
        addChildComponent (error);
        addAndMakeVisible (assign);
        addChildComponent (clear);
        assign.setComponentID ("hotkey.assign." + action);
        clear.setComponentID ("hotkey.clear." + action);
        error.setComponentID ("hotkey.error." + action);
        assign.onClick = [this] { startCapture(); };
        clear.onClick = [this]
        {
            controller.clearHotkey (action);
            showError ({});
            refresh();
        };
        refresh();
    }

    juce::String searchText() const
    {
        return label.getText() + " " + key.getText() + " " + ja ("ホットキー ショートカット キー") + (action == "pushToTalk" ? " PTT" : "");
    }

    void refresh()
    {
        const auto t = controller.getHotkeyText (action);
        key.setText (t.isNotEmpty() ? t : ja ("未割り当て"));
        key.setTone (t.isNotEmpty() ? Tone::text : Tone::sub);
        clear.setVisible (t.isNotEmpty() && ! capturing);
        assign.setButtonText (capturing ? ja ("キーを押してください") : ja ("割り当て"));
        assign.setStyle (capturing ? PillButton::Style::primary : PillButton::Style::outline);
        resized();
    }

    void showError (const juce::String& e)
    {
        error.setText (e);
        error.setVisible (e.isNotEmpty());
        if (onHeightChanged) onHeightChanged();
    }

    int heightForWidth (int width) const
    {
        return Theme::controlH + (error.isVisible() ? error.heightForWidth (width) + Theme::space1 : 0) + Theme::space1 * 2;
    }

    void startCapture()
    {
        capturing = true;
        showError ({});
        refresh();
        assign.grabKeyboardFocus();
    }

    void cancelCapture()
    {
        capturing = false;
        refresh();
    }

    bool isCapturing() const noexcept { return capturing; }

    /** Handles a key while capturing. Returns false to let the key pass (Tab). */
    bool captureKey (const juce::KeyPress& k)
    {
        const bool plain = ! k.getModifiers().isAnyModifierKeyDown();
        if (k.isKeyCode (juce::KeyPress::escapeKey) && plain) { cancelCapture(); return true; }
        if (k.isKeyCode (juce::KeyPress::tabKey) && plain) { cancelCapture(); return false; }
        if ((k.isKeyCode (juce::KeyPress::deleteKey) || k.isKeyCode (juce::KeyPress::backspaceKey)) && plain)
        {
            controller.clearHotkey (action);
            cancelCapture();
            return true;
        }
        int mods = 0, vk = 0;
        if (! keyPressToHotkey (k, mods, vk)) return true; // keep waiting
        capturing = false;
        if (hotkeyNeedsModifier (vk) && (mods & 3) == 0)
            showError (ja ("文字のキーだけでは割り当てられません。Ctrl か Alt と一緒に押してください。"));
        else
        {
            juce::String why;
            if (controller.setHotkey (action, mods, vk, why)) showError ({});
            else showError (why);
        }
        refresh();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (0, Theme::space1);
        auto line = r.removeFromTop (Theme::controlH);
        const int assignW = juce::jmax (assign.preferredWidth(), Theme::space5 * 5);
        clear.setBounds (line.removeFromRight (Theme::touchMin + Theme::space1).withSizeKeepingCentre (Theme::touchMin, Theme::touchMin));
        line.removeFromRight (Theme::space2);
        assign.setBounds (line.removeFromRight (assignW).withSizeKeepingCentre (assignW, Theme::buttonH));
        line.removeFromRight (Theme::space3);
        key.setBounds (line.removeFromRight (juce::jmin (Theme::space5 * 5, line.getWidth() / 2)));
        line.removeFromRight (Theme::space2);
        label.setBounds (line);
        if (error.isVisible())
        {
            r.removeFromTop (Theme::space1);
            error.setBounds (r.removeFromTop (error.heightForWidth (r.getWidth())));
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::colours().divider);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }

    std::function<void()> onHeightChanged;

private:
    struct CaptureButton : PillButton
    {
        explicit CaptureButton (HotkeyRow& r) : PillButton (ja ("割り当て"), Style::outline), row (r) {}
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (row.isCapturing())
            {
                swallow = true;
                return row.captureKey (k);
            }
            return PillButton::keyPressed (k);
        }
        bool keyStateChanged (bool isDown) override
        {
            if (row.isCapturing() || swallow)
            {
                if (! isDown) swallow = false;
                return true;
            }
            return PillButton::keyStateChanged (isDown);
        }
        void focusLost (FocusChangeType t) override
        {
            if (row.isCapturing()) row.cancelCapture();
            PillButton::focusLost (t);
        }
        HotkeyRow& row;
        bool swallow = false;
    };

    AppController& controller;
    juce::String action;
    TextLabel label, key, error;
    CaptureButton assign;
    IconButton clear;
    bool capturing = false;
};

/** The hotkey rows, tight and divided by their own lines. Rows hidden by the search take no space. */
class HotkeyList : public juce::Component
{
public:
    HotkeyRow& add (std::unique_ptr<HotkeyRow> r)
    {
        addAndMakeVisible (*r);
        rows.push_back (std::move (r));
        return *rows.back();
    }
    int heightForWidth (int w) const
    {
        int h = 0;
        for (auto& r : rows)
            if (r->isVisible()) h += r->heightForWidth (w);
        return h;
    }
    bool anyVisible() const
    {
        return std::any_of (rows.begin(), rows.end(), [] (auto& r) { return r->isVisible(); });
    }
    void resized() override
    {
        int y = 0;
        for (auto& r : rows)
            if (r->isVisible())
            {
                const int h = r->heightForWidth (getWidth());
                r->setBounds (0, y, getWidth(), h);
                y += h;
            }
    }
    std::vector<std::unique_ptr<HotkeyRow>> rows;
};

/** Label / value pairs for the diagnostics status (values in the mono font, F-14-8). */
class KeyValueList : public juce::Component
{
public:
    struct Item { juce::String key, value; Tone tone = Tone::text; };
    void setItems (std::vector<Item> i) { items = std::move (i); repaint(); }
    const std::vector<Item>& getItems() const { return items; }
    int heightForWidth (int) const { return int (items.size()) * Theme::space4 + Theme::space1; }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const int keyW = juce::jmin (Theme::space5 * 5, r.getWidth() / 3);
        for (auto& it : items)
        {
            auto line = r.removeFromTop (Theme::space4);
            drawText (g, it.key, line.removeFromLeft (keyW), Theme::fontS, Theme::colours().textSub);
            drawText (g, it.value, line, Theme::fontS, toneColour (it.tone), juce::Justification::centredLeft, true, true);
        }
    }

private:
    std::vector<Item> items;
};

/** Search results: the section columns (filtered) one under another, or a friendly message. */
class SearchResults : public juce::Component
{
public:
    SearchResults() : empty ({}, Theme::fontS, Tone::sub)
    {
        empty.setJustification (juce::Justification::centredTop);
        empty.setComponentID ("settings.search.empty");
        addChildComponent (empty);
    }
    int heightForWidth (int w) const
    {
        if (empty.isVisible()) return Theme::space5 + empty.heightForWidth (w);
        int h = 0;
        for (auto* c : columns())
            h += c->heightForWidth (w) + (h > 0 ? Theme::space3 : 0);
        return h;
    }
    void resized() override
    {
        auto r = getLocalBounds();
        if (empty.isVisible())
        {
            r.removeFromTop (Theme::space5);
            empty.setBounds (r.removeFromTop (empty.heightForWidth (r.getWidth())));
            return;
        }
        for (auto* c : columns())
        {
            c->setBounds (r.removeFromTop (c->heightForWidth (r.getWidth())));
            c->resized();
            r.removeFromTop (Theme::space3);
        }
    }
    std::vector<CardColumn*> columns() const
    {
        std::vector<CardColumn*> out;
        for (auto* ch : getChildren())
            if (auto* col = dynamic_cast<CardColumn*> (ch); col != nullptr && col->isVisible()) out.push_back (col);
        return out;
    }
    TextLabel empty;
};

juce::String rateText (double r) { return juce::String (r, 0) + " Hz"; }
juce::String bufferText (int b, double rate) { return juce::String (b) + ja (" サンプル（") + juce::String (1000.0 * b / rate, 1) + " ms" + ja ("）"); }

/** "500 ms", "80 Hz", "3 秒": the value (rounded to decimals) and its unit. */
std::function<juce::String (double)> unitText (int decimals, const char* unit)
{
    return [decimals, u = ja (unit)] (double v) { return juce::String (v, decimals) + " " + u; };
}
} // namespace

// =============================================================================================== Impl
struct SettingsView::Impl final : juce::ChangeListener, juce::Timer
{
    using BoolField = bool Settings::*;
    using IntField = int Settings::*;
    using FloatField = float Settings::*;

    /** everyday rows are always shown, detailed ones (and their sub headings) only when 「詳細な設定」 is open;
        plain blocks (hints, notes) are shown outside the search. Only items with search text can match. */
    enum class Kind { everyday, detailed, plain, heading };
    struct Item { juce::Component* comp; Kind kind; juce::String haystack; };
    struct Disclosure { PillButton* button; LayoutBox* box; int count; };

    Impl (SettingsView& o, AppController& ctl, Navigator& n) : owner (o), c (ctl), nav (n)
    {
        search.setComponentID ("settings.search");
        search.onTextChange = [this] { applySearch(); };
        search.onEscapeKey = [this] { setSearch ({}); };
        owner.addAndMakeVisible (search);

        const char* names[kNumSections] = { "デバイス", "環境設定", "ホットキー", "起動と常駐", "外観", "詳細", "診断" };
        for (int i = 0; i < kNumSections; ++i)
        {
            auto* b = navButtons.add (new NavButton (ja (names[i])));
            b->setComponentID (juce::String ("settings.nav.") + ids[i]);
            b->setRadioGroupId (1);
            b->onClick = [this, i] { show (i); };
            owner.addAndMakeVisible (b);
        }
        viewport.setScrollBarsShown (true, false);
        owner.addAndMakeVisible (viewport);

        buildDevices();
        buildEnvironment();
        buildHotkeys();
        buildStartup();
        buildAppearance();
        buildAdvanced();
        buildDiagnostics();
        for (int i = 0; i < kNumSections; ++i) sections[i]->setComponentID (juce::String ("settings.section.") + ids[i]);

        c.addChangeListener (this);
        refresh();
        show (0);
    }

    ~Impl() override
    {
        stopTimer();
        c.removeChangeListener (this);
        viewport.setViewedComponent (nullptr, false);
    }

    // ------------------------------------------------------------------------------- helpers
    static std::unique_ptr<SettingRow> row (const char* title, const juce::String& desc, std::unique_ptr<juce::Component> control, int prefW,
                                            int minW, std::function<int (int)> heightFor)
    {
        return std::make_unique<SettingRow> (ja (title), desc, std::move (control), prefW, minW, std::move (heightFor));
    }

    static std::function<int (int)> fixedH (int h) { return [h] (int) { return h; }; }

    std::unique_ptr<SettingRow> toggleRow (const char* title, const juce::String& desc, ToggleSwitch*& out, std::function<void (bool)> onChange)
    {
        auto t = std::make_unique<ToggleSwitch>();
        out = t.get();
        auto* raw = t.get();
        raw->setTitle (ja (title));
        raw->onClick = [raw, onChange] { onChange (raw->getToggleState()); };
        return row (title, desc, std::move (t), ToggleSwitch::preferredWidth(), ToggleSwitch::preferredWidth(), fixedH (Theme::toggleH + Theme::space2));
    }

    static int sliderBlockW (int sliderPart) { return sliderPart + ValueSlider::valueTextWidth; }

    /** Registers a row for the search (title, description and aliases) under the current kind. */
    SettingRow& reg (SettingRow& r, const char* aliases)
    {
        items.push_back ({ &r, building, fold (r.getTitle().getText() + " " + r.getDescription().getText() + " " + ja (aliases)) });
        if (building == Kind::detailed && ! disclosures.empty()) ++disclosures.back().count;
        return r;
    }

    void regBlock (juce::Component& comp, Kind kind, const juce::String& searchText = {})
    {
        items.push_back ({ &comp, kind, fold (searchText) });
    }

    /** Starts the detailed part of a card: the 「詳細な設定」 button, then every row added until endDetails(). */
    void beginDetails (SectionCard& card, const char* id)
    {
        auto box = std::make_unique<LayoutBox>();
        auto b = std::make_unique<PillButton> (ja ("詳細な設定"), PillButton::Style::ghost, Icon::chevronRight);
        auto* raw = b.get();
        raw->setComponentID (juce::String ("settings.details.") + id);
        raw->onClick = [this] { c.updateSettings ([] (Settings& s) { s.settingsShowDetails = ! s.settingsShowDetails; }); };
        box->addAndMakeVisible (b.release());
        box->layout = [raw] (juce::Rectangle<int> r) { raw->setBounds (r.withWidth (juce::jmin (r.getWidth(), raw->preferredWidth()))); };
        disclosures.push_back ({ raw, box.get(), 0 });
        card.addBlock (std::move (box), fixedH (Theme::buttonH));
        building = Kind::detailed;
    }

    void endDetails() { building = Kind::everyday; }

    void addHeading (SectionCard& card, const char* text)
    {
        auto l = std::make_unique<TextLabel> (ja (text), Theme::fontXS, Tone::sub, true);
        auto* raw = l.get();
        card.addBlock (std::move (l), [raw] (int w) { return raw->heightForWidth (w); });
        regBlock (*raw, Kind::heading);
    }

    static juce::String key (const char* k) { return juce::String ("settings.") + k; }

    SettingRow& addToggle (SectionCard& card, const char* k, const char* title, const char* desc, const char* aliases, BoolField f)
    {
        ToggleSwitch* t = nullptr;
        auto& r = card.addRow (toggleRow (title, ja (desc), t, [this, f] (bool on) { c.updateSettings ([f, on] (Settings& s) { s.*f = on; }); }));
        t->setComponentID (key (k));
        refreshers.push_back ([t, f] (const Settings& s) { t->setToggleState (s.*f, juce::dontSendNotification); });
        return reg (r, aliases);
    }

    /** Choice as a segmented control (or a combo box for long labels). values: the setting's value per label (default 0..n-1). */
    SettingRow& addChoice (SectionCard& card, const char* k, const char* title, const char* desc, const char* aliases, IntField f,
                           std::initializer_list<const char*> labels, std::vector<int> values = {}, bool combo = false)
    {
        juce::StringArray names;
        for (auto* l : labels) names.add (ja (l));
        auto toValue = [values] (int i) { return values.empty() ? i : values[size_t (i)]; };
        auto toIndex = [values] (int v)
        {
            if (values.empty()) return v;
            const auto it = std::find (values.begin(), values.end(), v);
            return it == values.end() ? -1 : int (it - values.begin());
        };
        if (combo)
        {
            auto cb = std::make_unique<juce::ComboBox>();
            auto* raw = cb.get();
            raw->setComponentID (key (k));
            raw->setTitle (ja (title));
            raw->setJustificationType (juce::Justification::centredLeft);
            for (int i = 0; i < names.size(); ++i) raw->addItem (names[i], i + 1);
            raw->onChange = [this, raw, f, toValue]
            {
                const int i = raw->getSelectedId() - 1;
                if (i >= 0) c.updateSettings ([f, v = toValue (i)] (Settings& s) { s.*f = v; });
            };
            refreshers.push_back ([raw, f, toIndex] (const Settings& s) { raw->setSelectedId (toIndex (s.*f) + 1, juce::dontSendNotification); });
            return reg (card.addRow (row (title, ja (desc), std::move (cb), m::comboW * 2 / 3, m::comboW / 2, fixedH (Theme::controlH))), aliases);
        }
        auto seg = std::make_unique<Segmented> (names);
        auto* raw = seg.get();
        raw->setComponentID (key (k));
        raw->setTitle (ja (title));
        raw->onChange = [this, f, toValue] (int i) { c.updateSettings ([f, v = toValue (i)] (Settings& s) { s.*f = v; }); };
        refreshers.push_back ([raw, f, toIndex] (const Settings& s) { raw->setSelected (toIndex (s.*f)); });
        const int w = raw->preferredWidth() + Theme::space4;
        return reg (card.addRow (row (title, ja (desc), std::move (seg), w, w, fixedH (Theme::buttonH))), aliases);
    }

    SettingRow& addSlider (SectionCard& card, const char* k, const char* title, const char* desc, const char* aliases, double min, double max,
                           double def, double step, std::function<juce::String (double)> format, std::function<double (const Settings&)> get,
                           std::function<void (Settings&, double)> set)
    {
        auto s = std::make_unique<ValueSlider>();
        auto* raw = s.get();
        raw->setComponentID (key (k));
        raw->setTitle (ja (title));
        raw->setup (min, max, def, step, std::move (format));
        raw->onValueChange = [this, raw, set]
        {
            const double v = raw->getValue();
            c.updateSettings ([set, v] (Settings& st) { set (st, v); });
        };
        refreshers.push_back ([raw, get] (const Settings& st) { raw->setValue (get (st), juce::dontSendNotification); });
        return reg (card.addRow (row (title, ja (desc), std::move (s), sliderBlockW (m::sliderW), sliderBlockW (m::sliderW / 2), fixedH (Theme::space5))),
                    aliases);
    }

    SettingRow& addFloat (SectionCard& card, const char* k, const char* title, const char* desc, const char* aliases, FloatField f, const Range& range,
                          double step, std::function<juce::String (double)> format)
    {
        return addSlider (card, k, title, desc, aliases, range.min, range.max, range.def, step, std::move (format),
                          [f] (const Settings& s) { return double (s.*f); }, [f] (Settings& s, double v) { s.*f = float (v); });
    }

    SettingRow& addInt (SectionCard& card, const char* k, const char* title, const char* desc, const char* aliases, IntField f, int min, int max,
                        int def, const char* unit)
    {
        return addSlider (card, k, title, desc, aliases, min, max, def, 1.0, unitText (0, unit), [f] (const Settings& s) { return double (s.*f); },
                          [f] (Settings& s, double v) { s.*f = juce::roundToInt (v); });
    }

    /** The row's control is enabled only while pred holds (e.g. 低域カットの周波数 while 低域カット is ON). */
    void enableIf (SettingRow& r, std::function<bool (const Settings&)> pred)
    {
        refreshers.push_back ([ctl = r.getControl(), pred] (const Settings& s) { ctl->setEnabled (pred (s)); });
    }

    // ------------------------------------------------------------------------------- devices
    struct DeviceControl
    {
        LayoutBox* box = nullptr;
        juce::ComboBox* combo = nullptr;
        TextLabel* status = nullptr;
        PillButton* link = nullptr;
        juce::StringArray items;
    };

    std::unique_ptr<juce::Component> makeDeviceControl (DeviceControl& d, const juce::String& id)
    {
        auto box = std::make_unique<LayoutBox>();
        d.box = box.get();
        auto combo = std::make_unique<juce::ComboBox>();
        d.combo = combo.get();
        combo->setComponentID (id);
        combo->setJustificationType (juce::Justification::centredLeft);
        auto statusLabel = std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::sub, true);
        d.status = statusLabel.get();
        statusLabel->setComponentID (id + ".status");
        auto link = std::make_unique<PillButton> (ja ("導入の手順を見る"), PillButton::Style::outline);
        d.link = link.get();
        link->setVisible (false);
        link->onClick = [this] { nav.showSetupWizard(); };
        box->addAndMakeVisible (combo.release());
        box->addAndMakeVisible (statusLabel.release());
        box->addChildComponent (link.release());
        auto* dp = &d;
        box->layout = [dp] (juce::Rectangle<int> r)
        {
            dp->combo->setBounds (r.removeFromTop (Theme::controlH));
            if (dp->status->getText().isNotEmpty())
            {
                r.removeFromTop (Theme::space2);
                dp->status->setBounds (r.removeFromTop (dp->status->heightForWidth (r.getWidth())));
            }
            if (dp->link->isVisible())
            {
                r.removeFromTop (Theme::space2);
                dp->link->setBounds (r.removeFromTop (Theme::buttonH).withWidth (dp->link->preferredWidth()));
            }
        };
        return box;
    }

    static int deviceControlHeight (const DeviceControl& d, int w)
    {
        int h = Theme::controlH;
        if (d.status->getText().isNotEmpty()) h += Theme::space2 + d.status->heightForWidth (w);
        if (d.link->isVisible()) h += Theme::space2 + Theme::buttonH;
        return h;
    }

    SectionCard& addCard (int section, const char* title, const char* subtitle)
    {
        auto& card = sections[section]->addCard (std::make_unique<SectionCard> (ja (title), ja (subtitle)));
        cards[section].push_back (&card);
        building = Kind::everyday;
        return card;
    }

    void buildDevices()
    {
        sections[0] = std::make_unique<CardColumn>();
        auto& card = addCard (0, "デバイス", "Windows の既定のデバイスは変えません");
        auto add = [&] (const char* title, const char* desc, DeviceControl& d, const char* id, const char* aliases)
        {
            auto* dp = &d;
            reg (card.addRow (row (title, ja (desc), makeDeviceControl (d, id), m::comboW, m::comboW - Theme::space5 * 2,
                                   [dp] (int w) { return deviceControlHeight (*dp, w); })),
                 aliases);
        };
        add ("出力先（仮想マイク）", "加工した声の送り先です。Discord の入力デバイスには、対になる「CABLE Output」を選びます", output, "settings.output",
             "CABLE Input VB-CABLE 仮想ケーブル 仮想マイク Discord デバイス");
        add ("入力（マイク）", "メイン画面でも変えられます", input, "settings.input", "マイク デバイス");
        add ("モニター出力", "自分の声を確かめるためのデバイスです。既定はオフで、ヘッドホンで使ってください", monitor, "settings.monitor",
             "モニター ヘッドホン 自分の声 デバイス");
        output.combo->onChange = [this] { c.setOutputDevice (output.combo->getSelectedId() > 1 ? output.combo->getText() : juce::String()); };
        input.combo->onChange = [this] { c.setInputDevice (input.combo->getSelectedId() > 1 ? input.combo->getText() : juce::String()); };
        monitor.combo->onChange = [this] { c.setMonitorDevice (monitor.combo->getSelectedId() > 1 ? monitor.combo->getText() : juce::String()); };

        beginDetails (card, "devices");
        addToggle (card, "wasapiExclusive", "排他モード", "入力と出力を KoeLoom だけで使い、遅延を減らします。断られたときは共有モードに戻します",
                   "排他 WASAPI exclusive 遅延 レイテンシ", &Settings::wasapiExclusive);
        addChoice (card, "inputChannel", "入力チャンネル", "ステレオのマイクで使うチャンネルです。自動は今までどおりです",
                   "チャンネル ステレオ モノラル 左 右 channel", &Settings::inputChannel, { "自動", "左だけ", "右だけ", "左右の平均" }, {}, true);
        addChoice (card, "monitorLatency", "モニターの遅延", "モニターの音が途切れるときは「安定」にします", "モニター 遅延 途切れ バッファ",
                   &Settings::monitorLatency, { "低遅延", "標準", "安定" });
        addInt (card, "reconnectSeconds", "再接続の間隔", "デバイスが外れたとき、つなぎ直しを試す間隔です（1〜10 秒）", "再接続 抜けた 外れた 復帰 デバイス",
                &Settings::reconnectSeconds, 1, 10, 1, "秒");
        endDetails();
    }

    static void fillCombo (DeviceControl& d, const juce::StringArray& names, const juce::String& placeholder, const juce::String& current)
    {
        juce::StringArray items { placeholder };
        items.addArray (names);
        if (current.isNotEmpty() && ! names.contains (current)) items.add (current);
        if (items != d.items)
        {
            d.items = items;
            d.combo->clear (juce::dontSendNotification);
            for (int i = 0; i < items.size(); ++i) d.combo->addItem (items[i], i + 1);
        }
        const int idx = current.isEmpty() ? 0 : items.indexOf (current, false, 1);
        d.combo->setSelectedId (juce::jmax (0, idx) + 1, juce::dontSendNotification);
    }

    void refreshDevices()
    {
        fillCombo (output, c.getOutputDevices(), ja ("未選択"), c.getOutputDevice());
        fillCombo (input, c.getInputDevices(), ja ("未選択"), c.getInputDevice());
        fillCombo (monitor, c.getMonitorDevices(), ja ("使わない"), c.getMonitorDevice());

        const auto out = c.getOutputDevice();
        const bool cable = c.isVirtualCableInstalled();
        if (AppController::isCableInputName (out))
        {
            output.status->setText (ja ("仮想ケーブルを検出しました"));
            output.status->setTone (Tone::ok);
            output.status->setIcon (Icon::check);
        }
        else
        {
            output.status->setText (out.isEmpty() ? (cable ? ja ("出力先が未選択です。「CABLE Input」を選んでください")
                                                           : ja ("仮想ケーブル（VB-CABLE）が見つかりません。導入すると、ここで選べます"))
                                                  : ja ("仮想ケーブルではありません。Discord に声が届かない可能性があります"));
            output.status->setTone (Tone::warn);
            output.status->setIcon (Icon::warning);
        }
        output.link->setVisible (! cable);

        input.status->setText (AppController::isCableOutputName (c.getInputDevice())
                                   ? ja ("入力が「CABLE Output」です。ループになるため開始できません。マイクを選んでください") : juce::String());
        input.status->setTone (Tone::danger);
        input.status->setIcon (Icon::warning);

        const bool speaker = c.isMonitorDeviceSpeaker();
        monitor.status->setText (speaker ? ja ("スピーカーのようです。モニターをスピーカーで使うとハウリングします。ヘッドホンで使ってください")
                                         : ja ("仮想ケーブルは選べません（相手に二重に流れます）"));
        monitor.status->setTone (speaker ? Tone::warn : Tone::sub);
        monitor.status->setIcon (speaker ? std::optional<Icon> (Icon::warning) : std::nullopt);
        for (auto* d : { &output, &input, &monitor }) d->box->resized();
    }

    // ------------------------------------------------------------------------------- environment
    void buildEnvironment()
    {
        sections[1] = std::make_unique<CardColumn>();
        auto& card = addCard (1, "環境設定", "プリセットを切り替えても変わらない設定です");

        // 入力ゲイン (F-03-4)
        {
            auto s = std::make_unique<ValueSlider>();
            inGain = s.get();
            s->setTitle (ja ("入力ゲイン"));
            s->setup (kInputGainDb.min, kInputGainDb.max, kInputGainDb.def, 0.5, [] (double v) { return formatDb (v); });
            s->onValueChange = [this] { c.setInputGainDb (float (inGain->getValue())); };
            reg (card.addRow (row ("入力ゲイン", ja ("マイクの音量を合わせます（-24〜+24 dB）"), std::move (s), sliderBlockW (m::sliderW),
                                   sliderBlockW (m::sliderW / 2), fixedH (Theme::space5))),
                 "音量 ボリューム マイク 入力");
        }
        // ノイズ抑制 (F-03-1)
        {
            auto box = std::make_unique<LayoutBox>();
            auto s = std::make_unique<ValueSlider>();
            nsMix = s.get();
            s->setTitle (ja ("ノイズ抑制の強さ"));
            s->setup (0.0, 100.0, kNoiseMix.def * 100.0, 1.0, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; });
            auto t = std::make_unique<ToggleSwitch>();
            nsToggle = t.get();
            t->setTitle (ja ("ノイズ抑制"));
            auto apply = [this] { c.setNoiseSuppression (nsToggle->getToggleState(), float (nsMix->getValue() / 100.0)); };
            s->onValueChange = apply;
            t->onClick = apply;
            box->addAndMakeVisible (s.release());
            box->addAndMakeVisible (t.release());
            box->layout = [this] (juce::Rectangle<int> r)
            {
                nsToggle->setBounds (r.removeFromRight (ToggleSwitch::preferredWidth()));
                r.removeFromRight (Theme::space3);
                nsMix->setBounds (r);
            };
            reg (card.addRow (row ("ノイズ抑制（RNNoise）", ja ("エアコンやキーボードの音を減らします。強さはドライ/ウェットの比率です"), std::move (box),
                                   sliderBlockW (m::sliderW * 2 / 3) + Theme::space3 + ToggleSwitch::preferredWidth(),
                                   sliderBlockW (m::sliderW / 2) + Theme::space3 + ToggleSwitch::preferredWidth(), fixedH (Theme::space5))),
                 "ノイズ除去 雑音 RNNoise");
        }
        // ノイズゲート (F-03-2)
        {
            auto box = std::make_unique<LayoutBox>();
            auto t = std::make_unique<ToggleSwitch>();
            gateToggle = t.get();
            t->setTitle (ja ("ノイズゲート"));
            auto thrLabel = std::make_unique<TextLabel> (ja ("しきい値"), Theme::fontXS, Tone::sub);
            thrLabel->setJustification (juce::Justification::centredLeft);
            auto* thrLabelRaw = thrLabel.get();
            auto s = std::make_unique<ValueSlider>();
            gateThr = s.get();
            s->setTitle (ja ("ゲートのしきい値"));
            s->setup (kGateThresholdDb.min, kGateThresholdDb.max, kGateThresholdDb.def, 1.0, [] (double v) { return formatDb (v, 0); });
            auto flow = std::make_unique<FlowBox> (Theme::space3);
            auto* flowRaw = flow.get();
            auto addNumber = [&] (const char* name, const Range& range, double step, int decimals, NumberField*& out)
            {
                auto pair = std::make_unique<LayoutBox>();
                auto l = std::make_unique<TextLabel> (ja (name), Theme::fontXS, Tone::sub);
                l->setJustification (juce::Justification::centredLeft);
                const int lw = l->singleLineWidth();
                auto f = std::make_unique<NumberField> (range.min, range.max, step, decimals, "ms");
                f->setTitle (ja (name));
                out = f.get();
                auto* lr = l.get();
                auto* fr = f.get();
                pair->addAndMakeVisible (l.release());
                pair->addAndMakeVisible (f.release());
                pair->layout = [lr, fr, lw] (juce::Rectangle<int> r)
                {
                    lr->setBounds (r.removeFromLeft (lw));
                    r.removeFromLeft (Theme::space2);
                    fr->setBounds (r);
                };
                flowRaw->add (std::move (pair), lw + Theme::space2 + m::numberW, m::numberH);
            };
            addNumber ("アタック", kGateAttackMs, 0.1, 1, gateAtt);
            addNumber ("ホールド", kGateHoldMs, 1.0, 0, gateHold);
            addNumber ("リリース", kGateReleaseMs, 1.0, 0, gateRel);
            auto apply = [this]
            {
                c.setGate (gateToggle->getToggleState(), float (gateThr->getValue()), float (gateAtt->getValue()), float (gateHold->getValue()),
                           float (gateRel->getValue()));
            };
            t->onClick = apply;
            s->onValueChange = apply;
            for (auto* f : { gateAtt, gateHold, gateRel }) f->onValueChange = [apply] (double) { apply(); };
            box->addAndMakeVisible (t.release());
            box->addAndMakeVisible (thrLabel.release());
            box->addAndMakeVisible (s.release());
            box->addAndMakeVisible (flow.release());
            const int thrLabelW = thrLabelRaw->singleLineWidth();
            box->layout = [this, thrLabelRaw, flowRaw, thrLabelW] (juce::Rectangle<int> r)
            {
                gateToggle->setBounds (r.removeFromTop (Theme::space4).removeFromRight (ToggleSwitch::preferredWidth())); // mock: right-aligned
                r.removeFromTop (Theme::space2);
                auto thr = r.removeFromTop (Theme::space5);
                thrLabelRaw->setBounds (thr.removeFromLeft (thrLabelW));
                thr.removeFromLeft (Theme::space2);
                gateThr->setBounds (thr.withWidth (juce::jmin (thr.getWidth(), sliderBlockW (m::sliderW))));
                r.removeFromTop (Theme::space2);
                flowRaw->setBounds (r.removeFromTop (flowRaw->heightForWidth (r.getWidth())));
            };
            const int prefW = juce::jmax (flowRaw->naturalWidth(), thrLabelW + Theme::space2 + sliderBlockW (m::sliderW * 2 / 3));
            const int minW = thrLabelW + Theme::space2 + sliderBlockW (m::sliderW / 2);
            reg (card.addRow (row ("ノイズゲート", ja ("一定より小さい音を切ります。開いているかはメイン画面に表示されます"), std::move (box), prefW, minW,
                                   [flowRaw] (int w) { return Theme::space4 + Theme::space2 + Theme::space5 + Theme::space2 + flowRaw->heightForWidth (w); })),
                 "ゲート しきい値 アタック ホールド リリース 雑音");
        }
        // 出力ゲイン (F-12-1, F-12-4)
        {
            auto box = std::make_unique<LayoutBox>();
            auto s = std::make_unique<ValueSlider>();
            outGain = s.get();
            s->setComponentID ("settings.outputGain.slider");
            s->setTitle (ja ("出力ゲイン"));
            s->setup (kOutputGainDb.min, kOutputGainDb.max, kOutputGainDb.def, kOutputGainStepDb, [] (double v) { return formatDb (v); });
            s->onValueChange = [this]
            {
                c.setOutputGainDb (float (outGain->getValue()));
                refreshOutputWarning();
            };
            auto w = std::make_unique<TextLabel> (ja ("リミッターが働きやすくなり、音が歪むことがあります"), Theme::fontXS, Tone::warn, true);
            outWarn = w.get();
            w->setComponentID ("settings.outputGainWarn");
            w->setIcon (Icon::warning);
            w->setVisible (false);
            box->addAndMakeVisible (s.release());
            box->addChildComponent (w.release());
            box->layout = [this] (juce::Rectangle<int> r)
            {
                outGain->setBounds (r.removeFromTop (Theme::space5));
                if (outWarn->isVisible())
                {
                    r.removeFromTop (Theme::space1);
                    outWarn->setBounds (r.removeFromTop (outWarn->heightForWidth (r.getWidth())));
                }
            };
            auto& r = reg (card.addRow (row ("出力ゲイン", ja ("基準は入力と同程度です。全体の音量を、ここで調整します。リミッター（-1 dBFS）は常に ON です（-24〜+12 dB）"),
                                             std::move (box), sliderBlockW (m::sliderW), sliderBlockW (m::sliderW / 2),
                                             [this] (int w) { return Theme::space5 + (outWarn->isVisible() ? Theme::space1 + outWarn->heightForWidth (w) : 0); })),
                           "音量 ボリューム 出力");
            r.setComponentID ("settings.outputGain");
        }

        beginDetails (card, "environment");
        addHeading (card, "声の処理");
        addChoice (card, "converterQuality", "変換の品質", "低遅延は遅れが少なく、高品質は声の変わり方がなめらかです。推定遅延も変わります",
                   "品質 遅延 レイテンシ 変換器 FFT quality", &Settings::converterQuality, { "低遅延", "標準", "高品質" });
        addToggle (card, "highPassOn", "低域カット", "ノイズ抑制の前で、机の振動や空調などの低い音を切ります",
                   "ハイパス high pass HPF ローカット 低音 振動 雑音", &Settings::highPassOn);
        enableIf (addFloat (card, "highPassHz", "低域カットの周波数", "これより低い音を切ります（20〜300 Hz）", "ハイパス high pass HPF ローカット 低音",
                            &Settings::highPassHz, kHighPassHz, 5.0, unitText (0, "Hz")),
                  [] (const Settings& s) { return s.highPassOn; });
        addToggle (card, "agcOn", "自動音量", "マイクの音量を自動でそろえます。ゲートの前で、ゆっくり動きます", "AGC 自動音量 オートゲイン 音量 そろえる",
                   &Settings::agcOn);
        enableIf (addFloat (card, "agcTargetDb", "自動音量の目標", "そろえる音量の目安です（-30〜-10 dB）", "AGC 自動音量 目標", &Settings::agcTargetDb,
                            kAgcTargetDb, 1.0, [] (double v) { return formatDb (v, 0); }),
                  [] (const Settings& s) { return s.agcOn; });
        enableIf (addFloat (card, "agcMaxGainDb", "自動音量の上限", "小さい声をどこまで持ち上げるかの上限です（0〜+24 dB）", "AGC 自動音量 上限",
                            &Settings::agcMaxGainDb, kAgcMaxGainDb, 1.0, [] (double v) { return formatDb (v, 0); }),
                  [] (const Settings& s) { return s.agcOn; });
        // the two ranges meet at 300 Hz; keep min < max here (clampSettings would put both back to the defaults)
        addSlider (card, "pitchMinHz", "ピッチ検出の下限", "オートピッチとスケール連動の声が、声の高さを探す範囲の下限です", "ピッチ 検出 音程 オートピッチ 範囲",
                   kPitchMinHz.min, kPitchMinHz.max, kPitchMinHz.def, 5.0, unitText (0, "Hz"), [] (const Settings& s) { return double (s.pitchMinHz); },
                   [] (Settings& s, double v) { s.pitchMinHz = juce::jmin (float (v), s.pitchMaxHz - 10.0f); });
        addSlider (card, "pitchMaxHz", "ピッチ検出の上限", "同じ範囲の上限です。高い音を声と間違えるときは下げます", "ピッチ 検出 音程 オートピッチ 範囲",
                   kPitchMaxHz.min, kPitchMaxHz.max, kPitchMaxHz.def, 10.0, unitText (0, "Hz"), [] (const Settings& s) { return double (s.pitchMaxHz); },
                   [] (Settings& s, double v) { s.pitchMaxHz = juce::jmax (float (v), s.pitchMinHz + 10.0f); });
        addFloat (card, "limiterCeilingDb", "リミッターの上限", "出力がこの大きさを超えないように抑えます（-6〜-0.1 dBFS）", "リミッター 上限 天井 音割れ クリップ",
                  &Settings::limiterCeilingDb, kLimiterCeilingSetDb, 0.1, [] (double v) { return formatDb (v); });
        addFloat (card, "limiterReleaseMs", "リミッターの戻り", "抑えたあと、元の音量に戻るまでの時間です", "リミッター リリース 戻り", &Settings::limiterReleaseMs,
                  kLimiterReleaseMs, 10.0, unitText (0, "ms"));
        addFloat (card, "presetCrossfadeMs", "プリセット切り替えのつなぎ", "プリセットを切り替えるとき、前の音と重ねてつなぐ時間です",
                  "クロスフェード 切り替え プリセット つなぎ", &Settings::presetCrossfadeMs, kPresetCrossfadeMs, 5.0, unitText (0, "ms"));
        addHeading (card, "サウンドボード");
        addInt (card, "soundboardMaxVoices", "同時に鳴らす数", "サウンドボードで同時に鳴らせる音の数です", "サウンドボード 効果音 同時 発音数",
                &Settings::soundboardMaxVoices, 1, kSoundboardMaxVoices, kSoundboardMaxVoices, "音");
        addFloat (card, "soundFadeMs", "効果音のフェード", "鳴らし始め・止めるときに、音量をなめらかに変える時間です", "サウンドボード 効果音 フェード",
                  &Settings::soundFadeMs, kSoundFadeMs, 5.0, unitText (0, "ms"));
        addFloat (card, "duckAttackMs", "ダッキングの効き始め", "効果音が鳴ったとき、声を下げきるまでの時間です", "ダッキング サウンドボード 効果音",
                  &Settings::duckAttackMs, kDuckAttackMs, 1.0, unitText (0, "ms"));
        addFloat (card, "duckReleaseMs", "ダッキングの戻り", "効果音が止まってから、声の音量が戻るまでの時間です", "ダッキング サウンドボード 効果音",
                  &Settings::duckReleaseMs, kDuckReleaseMs, 10.0, unitText (0, "ms"));
        addToggle (card, "monitorIncludeSoundboard", "モニターに効果音を流す", "効果音を自分のモニターでも聞きます。効果音ごとの設定とどちらも ON のときに流れます",
                   "サウンドボード 効果音 モニター", &Settings::monitorIncludeSoundboard);
        endDetails();
    }

    void refreshOutputWarning()
    {
        const bool warn = c.getSettings().outputGainDb > kOutputGainWarnDb;
        if (outWarn->isVisible() != warn)
        {
            outWarn->setVisible (warn);
            layoutContent();
        }
    }

    void refreshEnvironment()
    {
        const auto& s = c.getSettings();
        inGain->setValue (s.inputGainDb, juce::dontSendNotification);
        nsToggle->setToggleState (s.noiseSuppressionOn, juce::dontSendNotification);
        nsMix->setValue (s.noiseMix * 100.0, juce::dontSendNotification);
        nsMix->setEnabled (s.noiseSuppressionOn);
        gateToggle->setToggleState (s.gateOn, juce::dontSendNotification);
        gateThr->setValue (s.gateThresholdDb, juce::dontSendNotification);
        gateAtt->setValue (s.gateAttackMs);
        gateHold->setValue (s.gateHoldMs);
        gateRel->setValue (s.gateReleaseMs);
        for (juce::Component* comp : { (juce::Component*) gateThr, (juce::Component*) gateAtt, (juce::Component*) gateHold, (juce::Component*) gateRel })
            comp->setEnabled (s.gateOn);
        outGain->setValue (s.outputGainDb, juce::dontSendNotification);
        outWarn->setVisible (s.outputGainDb > kOutputGainWarnDb);
    }

    // ------------------------------------------------------------------------------- hotkeys (F-07)
    void buildHotkeys()
    {
        sections[2] = std::make_unique<CardColumn>();
        auto& card = addCard (2, "ホットキー", "ほかのアプリを使っている間も効きます。既定は未割り当てです");
        card.setComponentID ("settings.hotkeys");
        auto hint = std::make_unique<TextLabel> (ja ("［割り当て］を押してから、使いたいキーを押します。Esc で取り消し、Delete で解除します。文字のキーは Ctrl か Alt と組み合わせます。"),
                                                 Theme::fontXS, Tone::sub);
        auto* hintRaw = hint.get();
        hintRaw->setComponentID ("settings.hotkeys.hint"); // the tour's target: the card is taller than the window (E-32)
        card.addBlock (std::move (hint), [hintRaw] (int w) { return hintRaw->heightForWidth (w); });
        regBlock (*hintRaw, Kind::plain);

        addChoice (card, "pushToTalk", "プッシュトゥトーク", "ホットキー「プッシュトゥトーク」を押している間の動きです",
                   "PTT プッシュトゥトーク push to talk 押して話す 押している間 ミュート", &Settings::pushToTalk,
                   { "使わない", "押している間だけ話す", "押している間だけミュート" }, {}, true);
        beginDetails (card, "hotkeys");
        enableIf (addFloat (card, "pttReleaseMs", "離してから戻すまで", "キーを離したあと、声の終わりが切れないように待つ時間です", "PTT プッシュトゥトーク 離す",
                            &Settings::pttReleaseMs, kPttReleaseMs, 10.0, unitText (0, "ms")),
                  [] (const Settings& s) { return s.pushToTalk != 0; });
        addToggle (card, "hotkeyToasts", "ホットキーの通知", "ホットキーで切り替えたとき、画面に短く知らせます", "通知 トースト お知らせ", &Settings::hotkeyToasts);
        addToggle (card, "favoriteWrap", "お気に入りの循環", "「次のお気に入り」で最後まで行ったら、最初に戻ります", "お気に入り 循環 ループ 次 前",
                   &Settings::favoriteWrap);
        endDetails();

        auto list = std::make_unique<HotkeyList>();
        hotkeyList = list.get();
        for (auto& a : Hotkeys::allActions()) // whatever actions Hotkeys provides
        {
            auto& r = hotkeyList->add (std::make_unique<HotkeyRow> (c, a));
            r.onHeightChanged = [this] { layoutContent(); };
            hotkeyRows.push_back (&r);
        }
        card.addBlock (std::move (list), [this] (int w) { return hotkeyList->heightForWidth (w); });
        regBlock (*hotkeyList, Kind::plain);
    }

    // ------------------------------------------------------------------------------- startup (F-09) and updates (§7.4)
    void buildStartup()
    {
        sections[3] = std::make_unique<CardColumn>();
        auto& card = addCard (3, "起動と常駐", "ウィンドウを閉じても、トレイで動き続けます");
        reg (card.addRow (toggleRow ("最小化して起動", ja ("起動したとき、ウィンドウを出さずにトレイで動き始めます"), startMin,
                                     [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.startMinimized = on; }); })),
             "起動 最小化 トレイ");
        reg (card.addRow (toggleRow ("Windows の起動時に起動", ja ("サインインしたときに自動で起動します。管理者権限は要りません"), autoStart, [this] (bool on)
             {
                 juce::String err;
                 if (! c.setAutoStart (on, err))
                 {
                     autoStart->setToggleState (! on, juce::dontSendNotification);
                     nav.showToast (err.isNotEmpty() ? err : ja ("自動起動を切り替えられませんでした。"));
                 }
             })),
             "自動起動 スタートアップ サインイン");
        reg (card.addRow (toggleRow ("終了時に確認", ja ("トレイから終了するとき、使っているアプリの入力デバイスを元のマイクに戻すよう確認を出します"), confirmExit,
                                     [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.confirmOnExit = on; }); })),
             "終了 確認");
        addChoice (card, "closeAction", "× ボタンの動き", "ウィンドウの × を押したときの動きです。終了するときの確認は「終了時に確認」に従います",
                   "閉じる × バツ 終了 トレイ", &Settings::closeAction, { "トレイにしまう", "終了する" });
        beginDetails (card, "startup");
        addChoice (card, "startupVoice", "起動時のボイスチェンジャー", "起動したとき、ボイスチェンジャーを ON にするかどうかです", "起動 ON OFF ボイチェン",
                   &Settings::startupVoice, { "前回の状態", "ON", "OFF" });
        addToggle (card, "startupLastPreset", "前回のプリセットで起動", "OFF にすると、いつも「そのまま」で始めます", "起動 プリセット 前回",
                   &Settings::startupLastPreset);
        addToggle (card, "trayNotifications", "トレイの通知", "トレイから通知を出します", "通知 トレイ バルーン お知らせ", &Settings::trayNotifications);
        endDetails();

        auto& upd = addCard (3, "更新", "新しい版は GitHub の公開ページから受け取ります");
        addToggle (upd, "autoUpdate", "自動で更新する", "GitHub の公開ページを確認して新しい版をダウンロードし、終了時に置き換えます。OFF のときは通信しません",
                   "アップデート 更新 バージョン 自動 ダウンロード", &Settings::autoUpdate);
        {
            auto box = std::make_unique<LayoutBox>();
            auto state = std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::sub, true);
            updateState = state.get();
            state->setComponentID ("settings.updateState");
            state->setJustification (juce::Justification::centredLeft);
            state->setMaxLines (1);
            auto check = std::make_unique<PillButton> (ja ("今すぐ確認"), PillButton::Style::outline);
            checkNow = check.get();
            check->setComponentID ("settings.checkUpdates");
            check->onClick = [this]
            {
                c.checkForUpdates (true);
                refreshUpdate();
            };
            auto restart = std::make_unique<PillButton> (ja ("今すぐ再起動"), PillButton::Style::primary);
            restartNow = restart.get();
            restart->setComponentID ("settings.applyUpdate");
            restart->setVisible (false);
            restart->onClick = [this] { c.applyUpdateNow(); };
            box->addAndMakeVisible (state.release());
            box->addAndMakeVisible (check.release());
            box->addChildComponent (restart.release());
            box->layout = [this] (juce::Rectangle<int> r)
            {
                updateState->setBounds (r.removeFromTop (Theme::space4));
                r.removeFromTop (Theme::space1);
                auto line = r.removeFromTop (Theme::buttonH);
                checkNow->setBounds (line.removeFromLeft (checkNow->preferredWidth()));
                line.removeFromLeft (Theme::space2);
                restartNow->setBounds (line.removeFromLeft (juce::jmin (line.getWidth(), restartNow->preferredWidth())));
            };
            reg (upd.addRow (row ("バージョン", ja ("今の版は v") + KOELOOM_VERSION_STRING + ja (" です"), std::move (box), m::comboW, m::comboW / 2,
                                  fixedH (Theme::space4 + Theme::space1 + Theme::buttonH))),
                 "アップデート 更新 バージョン 今すぐ確認 再起動");
        }
        beginDetails (upd, "updates");
        addToggle (upd, "updateIncludePrerelease", "プレリリースも受け取る", "試験的な版（プレリリース）も更新の対象にします。OFF にすると正式版だけです",
                   "アップデート 更新 バージョン プレリリース ベータ", &Settings::updateIncludePrerelease);
        endDetails();
    }

    void refreshUpdate()
    {
        using S = AppController::UpdateState::Status;
        const auto u = c.getUpdateState();
        const auto v = "v" + u.version;
        juce::String text;
        Tone tone = Tone::sub;
        std::optional<Icon> icon;
        switch (u.status)
        {
            case S::idle: text = ja ("「今すぐ確認」で新しい版があるか調べます"); break;
            case S::checking: text = ja ("確認中…"); break;
            case S::upToDate: text = ja ("最新です"); tone = Tone::ok; icon = Icon::check; break;
            case S::downloading: text = v + ja (" をダウンロード中 ") + juce::String (juce::roundToInt (u.progress * 100.0f)) + "%"; break;
            case S::ready: text = v + ja (" の準備ができました"); tone = Tone::ok; icon = Icon::check; break;
            case S::failed: text = ja ("失敗: ") + u.error; tone = Tone::danger; icon = Icon::warning; break;
        }
        updateState->setText (text);
        updateState->setTone (tone);
        updateState->setIcon (icon);
        checkNow->setEnabled (u.status != S::checking && u.status != S::downloading);
        if (restartNow->isVisible() != (u.status == S::ready))
        {
            restartNow->setVisible (u.status == S::ready);
            if (auto* box = dynamic_cast<LayoutBox*> (restartNow->getParentComponent())) box->resized();
        }
    }

    // ------------------------------------------------------------------------------- appearance: 画面の配置 / 配色 (INTERFACES.md §8)
    static juce::String paletteFor (int layoutStyle)
    {
        return layoutStyle == 1 ? juce::String (ThemeLibrary::paperId) : layoutStyle == 2 ? juce::String (ThemeLibrary::monoId) : juce::String();
    }

    /** 「画面の配置」 (Settings::layoutStyle) and 「配色」 (Settings::themeId) are separate: any layout with any palette.
        Switching the layout to B / C offers that design's palette (「この案の配色にもする」). */
    void buildLayoutAndPalette (SectionCard& card)
    {
        const char* aliases = "スキン テーマ 配色 レイアウト デザイン 案 見た目 Studio Paper Mono";
        {
            auto box = std::make_unique<LayoutBox>();
            auto seg = std::make_unique<Segmented> (juce::StringArray { "A Studio", "B Paper", "C Mono" });
            auto* sg = seg.get();
            sg->setComponentID ("settings.layoutStyle");
            sg->setTitle (ja ("画面の配置"));
            sg->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.layoutStyle = i; }); };
            auto match = std::make_unique<PillButton> (ja ("この案の配色にもする"), PillButton::Style::accentOutline);
            auto* mb = match.get();
            mb->setComponentID ("settings.layoutPalette");
            mb->onClick = [this]
            {
                const auto id = paletteFor (c.getSettings().layoutStyle);
                c.updateSettings ([id] (Settings& s) { s.themeId = id; });
            };
            box->addAndMakeVisible (seg.release());
            box->addChildComponent (match.release());
            box->layout = [sg, mb] (juce::Rectangle<int> r)
            {
                sg->setBounds (r.removeFromTop (Theme::buttonH).withWidth (juce::jmin (r.getWidth(), sg->preferredWidth())));
                r.removeFromTop (Theme::space2);
                mb->setBounds (r.removeFromTop (Theme::buttonH).withWidth (juce::jmin (r.getWidth(), mb->preferredWidth())));
            };
            auto* bx = box.get();
            const int w = juce::jmax (sg->preferredWidth(), mb->preferredWidth()) + Theme::space4;
            reg (card.addRow (row ("画面の配置", ja ("ボイス画面の並べ方です。A は Studio（既定）、B は紙のような Paper、C は白黒の Mono です。配色とは別に選べます"),
                                   std::move (box), w, w, [mb] (int) { return Theme::buttonH + (mb->isVisible() ? Theme::space2 + Theme::buttonH : 0); })),
                 aliases);
            refreshers.push_back ([sg, mb, bx] (const Settings& s)
            {
                sg->setSelected (s.layoutStyle);
                mb->setVisible (s.layoutStyle != 0 && s.themeId != paletteFor (s.layoutStyle));
                bx->resized();
            });
        }
        {
            auto box = std::make_unique<LayoutBox>();
            auto cb = std::make_unique<juce::ComboBox>();
            auto* combo = cb.get();
            combo->setComponentID ("settings.themeId");
            combo->setTitle (ja ("配色"));
            combo->setJustificationType (juce::Justification::centredLeft);
            combo->onChange = [this, combo]
            {
                const int i = combo->getSelectedId() - 1;
                if (i >= 0 && i < paletteIds.size()) c.updateSettings ([id = paletteIds[i]] (Settings& s) { s.themeId = id; });
            };
            auto make = std::make_unique<PillButton> (ja ("新しく作る"), PillButton::Style::outline, Icon::plus);
            auto* mk = make.get();
            mk->setComponentID ("settings.themeNew");
            mk->onClick = [this] { nav.showOverlay (std::make_unique<ThemeEditor> (c, nav)); };
            auto edit = std::make_unique<PillButton> (ja ("編集"), PillButton::Style::outline);
            auto* ed = edit.get();
            ed->setComponentID ("settings.themeEdit");
            ed->onClick = [this]
            {
                if (const auto file = ThemeLibrary::userFile (c.getSettings().themeId); file.isNotEmpty())
                    nav.showOverlay (std::make_unique<ThemeEditor> (c, nav, file));
            };
            box->addAndMakeVisible (cb.release());
            box->addAndMakeVisible (make.release());
            box->addAndMakeVisible (edit.release());
            box->layout = [combo, mk, ed] (juce::Rectangle<int> r)
            {
                combo->setBounds (r.removeFromTop (Theme::controlH));
                r.removeFromTop (Theme::space2);
                auto buttons = r.removeFromTop (Theme::buttonH);
                mk->setBounds (buttons.removeFromLeft (mk->preferredWidth()));
                buttons.removeFromLeft (Theme::space2);
                ed->setBounds (buttons.removeFromLeft (ed->preferredWidth()));
            };
            const int w = juce::jmax (m::comboW * 2 / 3, mk->preferredWidth() + Theme::space2 + ed->preferredWidth());
            reg (card.addRow (row ("配色", ja ("画面の色の組み合わせです。Studio のときは下のテーマ・アクセントカラー・背景の色合いで決まります。自分で作ることもできます"),
                                   std::move (box), w, w, fixedH (Theme::controlH + Theme::space2 + Theme::buttonH))),
                 aliases);
            refreshers.push_back ([this, combo, ed] (const Settings& s)
            {
                // ponytail: reads the themes folder on every settings change; cache by folder time if that ever shows up
                juce::StringArray ids { "", ThemeLibrary::paperId, ThemeLibrary::monoId }, names { ja ("Studio（テーマ・アクセント・背景）"), "Paper", "Mono" };
                for (auto& e : ThemeLibrary::list())
                {
                    ids.add (ThemeLibrary::userId (e.file));
                    names.add (e.name);
                }
                if (ids != paletteIds || names != paletteNames)
                {
                    paletteIds = ids;
                    paletteNames = names;
                    combo->clear (juce::dontSendNotification);
                    for (int i = 0; i < names.size(); ++i) combo->addItem (names[i], i + 1);
                }
                combo->setSelectedId (ids.indexOf (s.themeId) + 1, juce::dontSendNotification);
                ed->setEnabled (ThemeLibrary::userFile (s.themeId).isNotEmpty());
            });
        }
    }

    // ------------------------------------------------------------------------------- appearance (F-14-2)
    void buildAppearance()
    {
        sections[4] = std::make_unique<CardColumn>();
        auto& card = addCard (4, "外観", "画面の見た目です");
        buildLayoutAndPalette (card);
        auto seg = std::make_unique<Segmented> (juce::StringArray { ja ("ダーク"), ja ("ライト") });
        theme = seg.get();
        seg->setComponentID ("settings.theme");
        seg->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.darkTheme = i == 0; }); };
        const int w = seg->preferredWidth() + Theme::space4;
        reg (card.addRow (row ("テーマ", ja ("既定はダークです。配色が Studio のときに使います"), std::move (seg), w, w, fixedH (Theme::buttonH))),
             "ダーク ライト 色 テーマ");

        auto sw = std::make_unique<AccentSwatches> (c.getSettings().darkTheme);
        accent = sw.get();
        sw->setComponentID ("settings.accent");
        sw->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.accentColour = i; }); };
        const int aw = AccentSwatches::preferredWidth();
        reg (card.addRow (row ("アクセントカラー", ja ("ボタンや ON の状態に使う色です。既定はシアンです"), std::move (sw), aw, aw, fixedH (Theme::touchMin))),
             "色 カラー アクセント");

        juce::StringArray tones;
        for (int i = 0; i < Theme::numTones; ++i) tones.add (Theme::toneName (i, c.getSettings().darkTheme));
        auto toneSeg = std::make_unique<Segmented> (tones);
        toneChoice = toneSeg.get();
        toneSeg->setComponentID ("settings.tone");
        toneSeg->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.backgroundTone = i; }); };
        const int tw = toneSeg->preferredWidth() + Theme::space4;
        reg (card.addRow (row ("背景の色合い", ja ("ダークとライトで別の 3 種類です。既定は標準です"), std::move (toneSeg), tw, tw, fixedH (Theme::buttonH))),
             "背景 色");

        std::vector<int> scales (std::begin (kUiScalePercents), std::end (kUiScalePercents));
        addChoice (card, "uiScalePercent", "拡大率", "画面全体の大きさです。ウィンドウの最小の大きさも同じ倍率になります",
                   "拡大 縮小 大きさ 文字の大きさ スケール ズーム", &Settings::uiScalePercent, { "90 %", "100 %", "110 %", "125 %", "150 %" }, scales);
        beginDetails (card, "appearance");
        addToggle (card, "alwaysOnTop", "常に手前に表示", "ほかのウィンドウより前に表示します", "最前面 手前 前面 top", &Settings::alwaysOnTop);
        addChoice (card, "animations", "画面の動き", "切り替えのときの動きです。既定は Windows の「アニメーション効果」に従います", "アニメーション 動き",
                   &Settings::animations, { "Windows に従う", "オン", "オフ" });
        addChoice (card, "meterFps", "メーターの更新", "60 にするとなめらかになりますが、少し重くなります", "メーター fps フレーム 更新",
                   &Settings::meterFps, { "30 fps", "60 fps" }, { 30, 60 });
        addFloat (card, "meterPeakHoldMs", "ピークの表示時間", "メーターの一番大きかった印を残す時間です", "メーター ピーク", &Settings::meterPeakHoldMs,
                  kMeterPeakHoldMs, 100.0, unitText (0, "ms"));
        addFloat (card, "tooltipDelayMs", "ツールチップが出るまで", "マウスを止めてから、説明が出るまでの時間です", "ツールチップ ヒント 説明",
                  &Settings::tooltipDelayMs, kTooltipDelayMs, 50.0, unitText (0, "ms"));
        addChoice (card, "knobSensitivity", "つまみの感度", "ドラッグしたときに、つまみが回る速さです", "つまみ ノブ ドラッグ 感度", &Settings::knobSensitivity,
                   { "ゆっくり", "標準", "速い" });
        addToggle (card, "knobWheel", "ホイールでつまみを回す", "OFF にすると、マウスのホイールは画面のスクロールだけに使います", "ホイール マウス つまみ ノブ スクロール",
                   &Settings::knobWheel);
        endDetails();
    }

    // ------------------------------------------------------------------------------- advanced (F-01-8, F-15-5, F-14-7)
    void buildAdvanced()
    {
        sections[5] = std::make_unique<CardColumn>();
        auto& card = addCard (5, "詳細", "困ったときだけ変えてください");
        {
            auto cb = std::make_unique<juce::ComboBox>();
            rateCombo = cb.get();
            cb->setComponentID ("settings.sampleRate");
            cb->onChange = [this]
            {
                const int i = rateCombo->getSelectedId() - 1;
                if (i >= 0 && i < rates.size()) c.setSampleRate (rates[i]);
            };
            reg (card.addRow (row ("サンプルレート", ja ("変換は 48000 Hz を基準にしています。ノイズ抑制は 48000 Hz でだけ使えます"), std::move (cb),
                                   m::comboW / 2 + Theme::space5, m::comboW / 2, fixedH (Theme::controlH))),
                 "サンプリング周波数 Hz");
        }
        {
            auto cb = std::make_unique<juce::ComboBox>();
            bufferCombo = cb.get();
            cb->setComponentID ("settings.bufferSize");
            cb->onChange = [this]
            {
                const int i = bufferCombo->getSelectedId() - 1;
                if (i >= 0 && i < buffers.size()) c.setBufferSize (buffers[i]);
            };
            reg (card.addRow (row ("バッファサイズ", ja ("小さいほど遅延が減り、大きいほど音切れしにくくなります"), std::move (cb), m::comboW / 2 + Theme::space5 * 2,
                                   m::comboW / 2, fixedH (Theme::controlH))),
                 "バッファ 遅延 レイテンシ 音切れ");
        }
        const bool fewCores = juce::SystemStats::getNumCpus() < kParallelMinLogicalCores;
        auto& mc = reg (card.addRow (toggleRow ("マルチコア処理（実験的）",
                                                ja ("変換器を複数のコアで動かします。効果は PC によって違います")
                                                    + (fewCores ? ja ("。この PC は論理コアが 2 以下のため、ON にできません") : juce::String()),
                                                multicore, [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.multicore = on; }); })),
                        "マルチコア CPU 並列");
        mc.setComponentID ("settings.multicore");
        multicore->setEnabled (! fewCores); // E-34
        reg (card.addRow (toggleRow ("ソフトウェア描画", ja ("画面が重いときに切り替えます。再起動後に反映されます"), softRender,
                                     [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.softwareRenderer = on; }); })),
             "描画 重い GPU Direct2D");

        // 書き出し / 読み込み / 初期化 (§7.3; the file work is AppController's)
        {
            auto flow = std::make_unique<FlowBox> (Theme::space2);
            auto ex = std::make_unique<PillButton> (ja ("書き出し"), PillButton::Style::outline);
            ex->setComponentID ("settings.export");
            ex->onClick = [this] { exportFile(); };
            auto im = std::make_unique<PillButton> (ja ("読み込み"), PillButton::Style::outline);
            im->setComponentID ("settings.import");
            im->onClick = [this] { importFile(); };
            const int ew = ex->preferredWidth(), iw = im->preferredWidth();
            flow->add (std::move (ex), ew, Theme::buttonH);
            flow->add (std::move (im), iw, Theme::buttonH);
            const int fw = flow->naturalWidth();
            auto* fr = flow.get();
            reg (card.addRow (row ("設定のバックアップ", ja ("設定をファイルに書き出したり、書き出したファイルから戻したりします"), std::move (flow), fw, fw,
                                   [fr] (int w) { return fr->heightForWidth (w); })),
                 "書き出し 読み込み エクスポート インポート バックアップ 保存 ファイル");
            auto reset = std::make_unique<PillButton> (ja ("すべて初期化"), PillButton::Style::outline);
            reset->setComponentID ("settings.reset");
            reset->onClick = [this] { confirmReset(); };
            const int rw = reset->preferredWidth();
            reg (card.addRow (row ("設定を初期化", ja ("設定を最初の状態に戻します。デバイス・お気に入り・ホットキーはそのまま残します"), std::move (reset), rw, rw,
                                   fixedH (Theme::buttonH))),
                 "初期化 リセット 元に戻す 既定");
        }
        beginDetails (card, "advanced");
        addChoice (card, "logLevel", "ログの詳しさ", "困ったときは「詳細」にして、診断情報と一緒に相談してください", "ログ log 記録 詳細", &Settings::logLevel,
                   { "エラーだけ", "標準", "詳細" });
        addInt (card, "logKeepDays", "ログを残す日数", "これより古いログは消します（1〜30 日）", "ログ log 日数 記録", &Settings::logKeepDays, 1, 30, 7, "日");
        endDetails();
    }

    void chooseFile (bool save, std::function<void (const juce::File&)> then)
    {
        if (auto& hook = settingsFileChooserForTests())
        {
            if (const auto f = hook (save); f != juce::File()) then (f);
            return;
        }
        const auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("KoeLoom-settings.json");
        chooser = std::make_unique<juce::FileChooser> (save ? ja ("設定の書き出し先") : ja ("読み込む設定ファイル（JSON）"), start, "*.json");
        const int mode = juce::FileBrowserComponent::canSelectFiles
                         | (save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode);
        chooser->launchAsync (mode, [then] (const juce::FileChooser& fc)
        {
            if (const auto f = fc.getResult(); f != juce::File()) then (f);
        });
    }

    void exportFile()
    {
        chooseFile (true, [this] (const juce::File& f)
        {
            juce::String err;
            if (c.exportSettings (f, err)) nav.showToast (ja ("設定を書き出しました。"));
            else nav.showToast (err.isNotEmpty() ? err : ja ("設定を書き出せませんでした。"));
        });
    }

    void importFile()
    {
        chooseFile (false, [this] (const juce::File& f)
        {
            juce::String err;
            if (c.importSettings (f, err)) nav.showToast (ja ("設定を読み込みました。"));
            else nav.showToast (err.isNotEmpty() ? err : ja ("設定を読み込めませんでした。"));
        });
    }

    void confirmReset()
    {
        auto panel = std::make_unique<mainui::ConfirmPanel> (nav, ja ("設定を初期化しますか"),
                                                             ja ("設定を最初の状態に戻します。デバイス・お気に入り・ホットキーはそのまま残ります。"), ja ("すべて初期化"),
                                                             [this]
                                                             {
                                                                 c.resetSettings();
                                                                 nav.showToast (ja ("設定を最初の状態に戻しました。"));
                                                             });
        panel->setComponentID ("settings.resetConfirm");
        nav.showOverlay (std::move (panel));
    }

    void refreshAdvanced()
    {
        const auto& s = c.getSettings();
        const auto newRates = c.getAvailableSampleRates();
        const auto newBuffers = c.getAvailableBufferSizes();
        if (newRates != rates || rateCombo->getNumItems() == 0)
        {
            rates = newRates;
            rateCombo->clear (juce::dontSendNotification);
            for (int i = 0; i < rates.size(); ++i) rateCombo->addItem (rateText (rates[i]), i + 1);
        }
        if (newBuffers != buffers || bufferCombo->getNumItems() == 0 || lastBufferRate != s.sampleRate)
        {
            buffers = newBuffers;
            lastBufferRate = s.sampleRate;
            bufferCombo->clear (juce::dontSendNotification);
            for (int i = 0; i < buffers.size(); ++i) bufferCombo->addItem (bufferText (buffers[i], s.sampleRate), i + 1);
        }
        rateCombo->setSelectedId (rates.indexOf (s.sampleRate) + 1, juce::dontSendNotification);
        bufferCombo->setSelectedId (buffers.indexOf (s.bufferSize) + 1, juce::dontSendNotification);
        multicore->setToggleState (s.multicore && multicore->isEnabled(), juce::dontSendNotification);
        softRender->setToggleState (s.softwareRenderer, juce::dontSendNotification);
    }

    // ------------------------------------------------------------------------------- diagnostics (F-11-3)
    void buildDiagnostics()
    {
        sections[6] = std::make_unique<CardColumn>();
        auto& card = addCard (6, "診断", "困ったときの確認用です。音声の内容は記録しません");
        auto kv = std::make_unique<KeyValueList>();
        status = kv.get();
        kv->setComponentID ("settings.diagStatus");
        card.addBlock (std::move (kv), [this] (int w) { return status->heightForWidth (w); });
        regBlock (*status, Kind::everyday, ja ("診断 状態 サンプルレート バッファ 推定遅延 遅延 XRUN 音切れ CPU"));
        auto flow = std::make_unique<FlowBox> (Theme::space2);
        auto copy = std::make_unique<PillButton> (ja ("診断情報をコピー"), PillButton::Style::outline);
        copy->setComponentID ("settings.copyDiagnostics");
        copy->onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (c.buildDiagnostics());
            nav.showToast (ja ("診断情報をコピーしました（ユーザー名を含むパスは伏せてあります）。"));
        };
        auto logs = std::make_unique<PillButton> (ja ("ログのフォルダを開く"), PillButton::Style::outline, Icon::folder);
        logs->setComponentID ("settings.openLogs");
        logs->onClick = []
        {
            auto dir = paths::logsDir();
            dir.createDirectory();
            dir.startAsProcess();
        };
        const int cw = copy->preferredWidth(), lw = logs->preferredWidth();
        flow->add (std::move (copy), cw, Theme::buttonH);
        flow->add (std::move (logs), lw, Theme::buttonH);
        auto* flowRaw = flow.get();
        card.addBlock (std::move (flow), [flowRaw] (int w) { return flowRaw->heightForWidth (w); });
        regBlock (*flowRaw, Kind::everyday, ja ("診断情報をコピー ログのフォルダを開く ログ 相談"));
        auto note = std::make_unique<TextLabel> (ja ("コピーした内容は、相談するときに貼り付けて使えます。ログはこの PC の中にだけ保存され、送信はしません。"), Theme::fontXS, Tone::sub);
        auto* noteRaw = note.get();
        card.addBlock (std::move (note), [noteRaw] (int w) { return noteRaw->heightForWidth (w); });
        regBlock (*noteRaw, Kind::plain);
    }

    void refreshStatus()
    {
        const auto s = c.getStatus();
        juce::String state;
        Tone tone = Tone::text;
        if (s.running) { state = ja ("動作中"); tone = Tone::ok; }
        else if (s.loopConfig) { state = ja ("停止中（ループ構成）"); tone = Tone::danger; }
        else if (s.deviceLost) { state = ja ("停止中（デバイスが見つかりません）"); tone = Tone::danger; }
        else if (s.error.isNotEmpty()) { state = ja ("停止中（") + s.error + ja ("）"); tone = Tone::danger; }
        else { state = ja ("停止中（入力か出力が未選択です）"); tone = Tone::warn; }
        const juce::String dash ("-");
        status->setItems ({
            { ja ("状態"), state, tone },
            { ja ("サンプルレート"), s.running ? rateText (s.sampleRate) : dash },
            { ja ("バッファ"), s.running ? juce::String (s.bufferSize) + ja (" サンプル") : dash },
            { ja ("推定遅延"), juce::String (s.latencyMs, 1) + " ms", s.latencyWarn ? Tone::warn : Tone::text },
            { "XRUN", juce::String (s.xruns), s.xruns > 0 ? Tone::warn : Tone::text },
            { "CPU", s.running ? juce::String (s.cpuPercent, 1) + " %" : dash },
        });
    }

    void timerCallback() override
    {
        if (status->isShowing()) refreshStatus();
        refreshUpdate(); // 確認中 / ダウンロード中 follow without a change message
    }

    // ------------------------------------------------------------------------------- search and 「詳細な設定」
    static bool matches (const juce::String& haystack, const juce::StringArray& terms)
    {
        if (haystack.isEmpty()) return false;
        for (auto& t : terms)
            if (! haystack.contains (t)) return false;
        return true;
    }

    bool searching() const { return query.isNotEmpty(); }

    /** Shows / hides rows, cards and columns for the detail state and the search. */
    void applyVisibility()
    {
        const bool open = c.getSettings().settingsShowDetails;
        juce::StringArray terms;
        terms.addTokens (fold (query), " ", "");
        terms.removeEmptyStrings();
        const bool find = ! terms.isEmpty();
        for (auto& it : items)
        {
            if (it.comp == hotkeyList) continue;
            it.comp->setVisible (find ? matches (it.haystack, terms) : it.kind == Kind::everyday || it.kind == Kind::plain || open);
        }
        for (auto* r : hotkeyRows) r->setVisible (! find || matches (fold (r->searchText()), terms));
        hotkeyList->setVisible (! find || hotkeyList->anyVisible());
        hotkeyList->resized();
        for (auto& d : disclosures)
        {
            d.button->setVisible (true);
            d.box->setVisible (! find);
            d.button->setButtonText (open ? ja ("詳細な設定を閉じる") : ja ("詳細な設定（") + juce::String (d.count) + ja (" 項目）"));
            d.button->setTitle (d.button->getButtonText());
            d.button->setIcon (open ? Icon::chevronDown : Icon::chevronRight);
            d.box->resized();
        }
        bool any = false;
        for (int i = 0; i < kNumSections; ++i)
        {
            bool colVisible = false;
            for (auto* card : cards[i])
            {
                card->setVisible (! find || card->hasVisibleItems());
                colVisible = colVisible || card->isVisible();
            }
            if (find) sections[i]->setVisible (colVisible);
            any = any || colVisible;
        }
        results.empty.setVisible (find && ! any);
        if (find && ! any)
            results.empty.setText (ja ("「") + query + ja ("」に当てはまる設定は見つかりませんでした。\n別の言葉（例: 音量、遅延、通知）で探してください。"));
    }

    void setSearch (const juce::String& text)
    {
        search.setText (text, false);
        applySearch();
    }

    void applySearch()
    {
        const auto q = search.getText().trim();
        if (q == query) return;
        const bool was = searching();
        query = q;
        if (searching())
        {
            if (! was)
            {
                viewport.setViewedComponent (&results, false);
                for (auto& s : sections) results.addChildComponent (s.get());
                for (auto* b : navButtons) b->setToggleState (false, juce::dontSendNotification);
            }
            applyVisibility();
            viewport.setViewPosition (0, 0);
            layoutContent();
        }
        else show (current);
    }

    bool keyPressed (const juce::KeyPress& k)
    {
        const auto mods = k.getModifiers();
        if (mods.isCtrlDown() && ! mods.isAltDown() && juce::CharacterFunctions::toUpperCase (juce::juce_wchar (k.getKeyCode())) == 'F')
        {
            search.grabKeyboardFocus();
            search.selectAll();
            return true;
        }
        if (k.isKeyCode (juce::KeyPress::escapeKey) && searching())
        {
            setSearch ({});
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------- common
    void refresh()
    {
        refreshDevices();
        refreshEnvironment();
        for (auto* r : hotkeyRows) if (! r->isCapturing()) r->refresh();
        const auto& s = c.getSettings();
        startMin->setToggleState (s.startMinimized, juce::dontSendNotification);
        autoStart->setToggleState (s.autoStart, juce::dontSendNotification);
        confirmExit->setToggleState (s.confirmOnExit, juce::dontSendNotification);
        theme->setSelected (s.darkTheme ? 0 : 1);
        accent->setSelected (s.accentColour);
        toneChoice->setSelected (s.backgroundTone);
        for (auto* studioOnly : std::initializer_list<juce::Component*> { theme, accent, toneChoice })
            studioOnly->setEnabled (s.themeId.isEmpty()); // 配色 Studio only
        for (auto& r : refreshers) r (s);
        refreshAdvanced();
        refreshStatus();
        refreshUpdate();
        applyVisibility();
        layoutContent();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void show (int index)
    {
        current = juce::jlimit (0, kNumSections - 1, index);
        if (searching())
        {
            search.setText ({}, false);
            query = {};
        }
        for (int i = 0; i < kNumSections; ++i) navButtons[i]->setToggleState (i == current, juce::dontSendNotification);
        viewport.setViewedComponent (sections[current].get(), false);
        sections[current]->setVisible (true);
        applyVisibility();
        viewport.setViewPosition (0, 0);
        startTimerHz (current == 6 ? 10 : 4);
        refreshStatus();
        layoutContent();
    }

    void layoutContent()
    {
        auto* comp = viewport.getViewedComponent();
        if (comp == nullptr) return;
        auto heightFor = [this, comp] (int w) { return comp == &results ? results.heightForWidth (w) : sections[current]->heightForWidth (w); };
        const int vw = viewport.getWidth(), vh = viewport.getHeight();
        if (vw <= 0) return;
        int w = vw;
        int h = heightFor (w);
        if (h > vh)
        {
            w = vw - viewport.getScrollBarThickness() - Theme::space2;
            h = heightFor (w);
        }
        comp->setBounds (0, 0, w, comp == &results ? h : juce::jmax (h, vh));
        comp->resized();
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        const int navW = owner.getWidth() + Theme::space5 < Theme::narrowWidth ? m::navWNarrow : m::navW;
        auto navArea = r.removeFromLeft (navW);
        search.setBounds (navArea.removeFromTop (Theme::controlH));
        navArea.removeFromTop (Theme::space2);
        for (auto* b : navButtons)
        {
            b->setBounds (navArea.removeFromTop (Theme::controlH));
            navArea.removeFromTop (Theme::space1 / 2);
        }
        r.removeFromLeft (Theme::space3);
        viewport.setBounds (r);
        layoutContent();
    }

    static constexpr const char* ids[kNumSections] = { "devices", "environment", "hotkeys", "startup", "appearance", "advanced", "diagnostics" };

    SettingsView& owner;
    AppController& c;
    Navigator& nav;
    SearchField search { ja ("設定を探す"), ja ("設定を名前や説明で探す（Ctrl+F）") };
    juce::String query;
    juce::OwnedArray<NavButton> navButtons;
    juce::Viewport viewport;
    SearchResults results;
    std::unique_ptr<CardColumn> sections[kNumSections];
    std::vector<SectionCard*> cards[kNumSections];
    int current = 0;

    Kind building = Kind::everyday;
    std::vector<Item> items;
    std::vector<Disclosure> disclosures;
    std::vector<std::function<void (const Settings&)>> refreshers;

    DeviceControl output, input, monitor;
    ValueSlider *inGain = nullptr, *nsMix = nullptr, *gateThr = nullptr, *outGain = nullptr;
    ToggleSwitch *nsToggle = nullptr, *gateToggle = nullptr;
    NumberField *gateAtt = nullptr, *gateHold = nullptr, *gateRel = nullptr;
    TextLabel* outWarn = nullptr;
    HotkeyList* hotkeyList = nullptr;
    std::vector<HotkeyRow*> hotkeyRows;
    ToggleSwitch *startMin = nullptr, *autoStart = nullptr, *confirmExit = nullptr, *multicore = nullptr, *softRender = nullptr;
    TextLabel* updateState = nullptr;
    PillButton *checkNow = nullptr, *restartNow = nullptr;
    Segmented* theme = nullptr;
    AccentSwatches* accent = nullptr;
    Segmented* toneChoice = nullptr;
    juce::StringArray paletteIds, paletteNames; // S-03 外観 「配色」 items: Settings::themeId and the shown name
    juce::ComboBox *rateCombo = nullptr, *bufferCombo = nullptr;
    juce::Array<double> rates;
    juce::Array<int> buffers;
    double lastBufferRate = 0.0;
    KeyValueList* status = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;
};

// =============================================================================================== SettingsView
SettingsView::SettingsView (AppController& controller, Navigator& nav)
{
    impl = std::make_unique<Impl> (*this, controller, nav);
}

SettingsView::~SettingsView() = default;

void SettingsView::showSection (Navigator::SettingsSection section) { impl->show (sectionIndex (section)); }

void SettingsView::setSearchText (const juce::String& text) { impl->setSearch (text); }

bool SettingsView::keyPressed (const juce::KeyPress& key) { return impl->keyPressed (key); }

void SettingsView::resized() { impl->layout(); }

void SettingsView::paint (juce::Graphics&) {}
} // namespace koe::ui

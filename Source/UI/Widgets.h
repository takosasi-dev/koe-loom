#pragma once

// Shared, theme-driven controls (F-14). Screens compose these; they never paint raw colours.
// Keyboard: every control takes focus (Tab), shows a 2 px accent focus ring (F-14-4).

#include "UI/Icons.h"
#include "UI/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace koe::ui
{
/** Paints the 2 px focus ring around r (call from paint() when hasKeyboardFocus). */
void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> r, float radius);

/** Japanese literal -> juce::String (juce::String(const char*) would mangle UTF-8). */
inline juce::String ja (const char* utf8) { return juce::String::fromUTF8 (utf8); }

// ---------------------------------------------------------------------------------------------
/**
    Arc knob (案 A). 270° arc from lower-left to lower-right, accent arc up to the value, a dot at
    the value. Big variant (112 px) prints the value large in the centre with its unit (F-14-12);
    small variant (40 px) is used in slots. Double-click = default (F-14-5), Shift+drag = fine,
    arrow keys step, Enter or right-click = type a value. The hover tooltip (F-13-6) shows
    "label：value（min〜max、初期値 def）".
*/
class Knob : public juce::Slider
{
public:
    enum class Size { big, small };
    explicit Knob (Size size = Size::small);

    /** min, max, default, step (0 = continuous). */
    void setup (double min, double max, double def, double step, std::function<juce::String (double)> formatValue,
                const juce::String& unit = {});
    /** The parameter's name (tooltip; the big variant also prints it under the knob). */
    void setLabel (const juce::String& text) { label = text; valueChanged(); }
    /** Knob circle size in px (defaults: Theme::knobBig / knobSmall). Arc, dot and value text scale with it. */
    void setDiameter (int px) { diameter = px; repaint(); }
    int getDiameter() const { return diameter; }
    juce::String getValueText() const { return format ? format (getValue()) : juce::String (getValue()); }

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed (const juce::KeyPress& k) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    /** Opens the inline editor for typing a value. */
    void editValue();

private:
    void valueChanged() override;
    Size size;
    int diameter;
    juce::String label, unit;
    std::function<juce::String (double)> format;
    double defaultValue = 0.0;
    std::unique_ptr<juce::TextEditor> editor;
};

// ---------------------------------------------------------------------------------------------
/** 40x22 switch with "ON"/"OFF" text beside it (state never shown by colour alone). */
class ToggleSwitch : public juce::Button
{
public:
    ToggleSwitch();
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    /** Width including the ON/OFF text. */
    static int preferredWidth() { return Theme::toggleW + Theme::space2 + 28; }
};

// ---------------------------------------------------------------------------------------------
/** Text (+ optional icon) button in the mock's styles. */
class PillButton : public juce::Button
{
public:
    enum class Style
    {
        primary,   // accent fill (ボイチェン ON, 完了して始める)
        outline,   // 1.5 px border (マイクミュート off, secondary actions)
        ghost,     // no border, subtle (header tabs, 設定で変更)
        danger,    // danger fill (マイクミュート on: red + "MUTE")
        tab,       // header navigation: raised fill when toggled on
        accentOutline, // 1.5 px accent border, accent text (S-07 追加, S-04 テスト音を出す)
        link           // underlined sub-tone text, no fill (S-04 あとで設定する)
    };
    PillButton (const juce::String& text, Style style, std::optional<Icon> icon = std::nullopt);
    void setStyle (Style s) { style = s; repaint(); }
    void setIcon (std::optional<Icon> i) { icon = i; repaint(); }
    /** Rounded "pill" corners (height/2) instead of radiusM. */
    void setPill (bool p) { pill = p; repaint(); }
    void setFontSize (float f) { fontSize = f; repaint(); }
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    int preferredWidth() const;

private:
    Style style;
    std::optional<Icon> icon;
    bool pill = false;
    float fontSize = Theme::fontS;
};

// ---------------------------------------------------------------------------------------------
/** Square icon-only button (needs a tooltip / accessible title). */
class IconButton : public juce::Button
{
public:
    IconButton (const juce::String& accessibleName, Icon icon, bool outlined = false);
    void setIcon (Icon i) { icon = i; repaint(); }
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    Icon icon;
    bool outlined;
};

// ---------------------------------------------------------------------------------------------
/**
    Peak meter (F-08-1): dB scale with -48/-24/-12/-6/0 ticks, 1.5 s peak hold, clip marker.
    setLevel() is fed at 30 fps with the peak (dBFS) since the last frame. Tick labels stay inside
    the bounds and the ones that would overlap are dropped on short meters (0 and -48 are kept first).
*/
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (bool vertical = true);
    void setLevel (float peakDb, bool clipped);
    void setShowScale (bool s) { showScale = s; repaint(); }
    bool isClipping() const { return clipHold > 0; }
    void paint (juce::Graphics& g) override;
    static float dbToPos (float db); // 0..1 on the meter's scale (-60 .. 0 dB)
    /** Vertical meter: the tick labels drawn at the current size, as (dB, bounds). */
    std::vector<std::pair<int, juce::Rectangle<int>>> scaleLabels() const;

private:
    bool vertical, showScale = true;
    float level = -100.0f, peak = -100.0f;
    int peakHold = 0, clipHold = 0;
};

// ---------------------------------------------------------------------------------------------
/** Horizontal slider with accent fill, round thumb and the value text at the right (settings). */
class ValueSlider : public juce::Slider
{
public:
    ValueSlider();
    void setup (double min, double max, double def, double step, std::function<juce::String (double)> formatValue);
    void paint (juce::Graphics& g) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    static constexpr int valueTextWidth = 84;

private:
    std::function<juce::String (double)> format;
};

// ---------------------------------------------------------------------------------------------
/** Paints a card: surface, radiusL corners, header "NN  Title" (mono number + bold title). Returns the content area. */
juce::Rectangle<int> paintCard (juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& number, const juce::String& title);
/** The 軽/中/重 badge (text, never colour alone). */
void paintWeightBadge (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool heavy);
/** Small helper: one line of text in the theme font. */
void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, float size, juce::Colour c,
               juce::Justification j = juce::Justification::centredLeft, bool bold = false, bool mono = false);
/** Appends text in the theme font. Mono text keeps only its ASCII in the mono face: Consolas has no kana,
    and its fallback drew Japanese ("4 / 10 スロット", "未割り当て") a size smaller than the UI face. */
void appendText (juce::AttributedString& s, const juce::String& text, float size, bool bold, bool mono, juce::Colour c);
/** Width of one line of text as drawText / appendText draw it. */
float textRunWidth (const juce::String& text, float size, bool bold, bool mono);

// ---------------------------------------------------------------------------------------------
/** The app's LookAndFeel: theme colours for stock JUCE widgets (ComboBox, PopupMenu, TextEditor,
    ScrollBar, Tooltip, AlertWindow) and the UI font as the default typeface. */
class KoeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KoeLookAndFeel();
    /** Re-applies the theme colours (after Theme::setDark). */
    void refreshColours();

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowTitleFont() override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
};
} // namespace koe::ui

#pragma once

// Shared building blocks for the ui-screens worker (S-02, S-03, S-04, S-06, help pages).
// Everything lives in koe::ui::screens so it never collides with the ui-main worker's names.
// Sizes are derived from Theme tokens only (F-14-1, AC-53); colours come from Theme::colours().

#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace koe::ui::screens
{
// ---------------------------------------------------------------------------------------------
/** Layout metrics of the mocks, rounded to the token grid (§8.4). */
namespace m
{
inline constexpr int navW = Theme::space5 * 7;                       // mock 224
inline constexpr int navWNarrow = Theme::slotWNarrow;                // 168
inline constexpr int cardPadX = Theme::space4;                       // mock 28
inline constexpr int cardHeaderH = Theme::pillH;                     // mock 44
inline constexpr int rowPadY = Theme::space3;                        // mock 14
inline constexpr int labelMaxW = Theme::space5 * 10 + Theme::space3; // mock 330 (+6: JUCE sets kana a little wider)
inline constexpr int labelMinW = Theme::space5 * 6;                  // below this a row stacks
inline constexpr int sliderW = Theme::space5 * 8;                    // mock 250 (+ value text)
inline constexpr int comboW = Theme::space5 * 10;                    // mock 320
inline constexpr int numberW = Theme::space5 * 2 + Theme::space3 + Theme::space1; // mock 84
inline constexpr int numberH = Theme::touchMin;                      // mock 32
inline constexpr int badgeH = Theme::space4;
inline constexpr int iconS = Theme::space3 + Theme::space1;          // 20 px icons in text lines
} // namespace m

// ---------------------------------------------------------------------------------------------
/** Container whose children are placed by a lambda (compound controls inside rows). */
class LayoutBox : public juce::Component
{
public:
    std::function<void (juce::Rectangle<int>)> layout;
    void resized() override
    {
        if (layout) layout (getLocalBounds());
    }
};

// ---------------------------------------------------------------------------------------------
enum class Tone { text, sub, accent, ok, warn, danger };
juce::Colour toneColour (Tone t);

/** Text that wraps (Japanese included) in the theme font. Not interactive. */
class TextLabel : public juce::Component
{
public:
    TextLabel (const juce::String& text = {}, float size = Theme::fontS, Tone tone = Tone::text, bool bold = false, bool mono = false);
    void setText (const juce::String& t);
    const juce::String& getText() const noexcept { return text; }
    void setTone (Tone t) { tone = t; repaint(); }
    Tone getTone() const noexcept { return tone; }
    void setIcon (std::optional<Icon> i) { icon = i; repaint(); }
    void setJustification (juce::Justification j) { just = j; repaint(); }
    void setMaxLines (int n) { maxLines = n; }
    /** Height needed at the given width (all lines, or maxLines). */
    int heightForWidth (int width) const;
    /** Width of the text on one line (+ icon). */
    int singleLineWidth() const;
    void paint (juce::Graphics& g) override;

private:
    juce::TextLayout layoutFor (float width) const;
    juce::String text;
    float size;
    Tone tone;
    bool bold, mono;
    std::optional<Icon> icon;
    juce::Justification just = juce::Justification::topLeft;
    int maxLines = 0;
};

// ---------------------------------------------------------------------------------------------
/** Left-aligned list button (settings sections, preset categories). Raised fill + bold when selected.
    Optional count at the right (mono). Up / Down move to the neighbour in the same parent. */
class NavButton : public juce::Button
{
public:
    explicit NavButton (const juce::String& text);
    void setCount (int n) { count = n; repaint(); }
    int getCount() const noexcept { return count; }
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    bool keyPressed (const juce::KeyPress& k) override;

private:
    int count = -1;
};

// ---------------------------------------------------------------------------------------------
/** Small boxed number with its unit inside (gate attack/hold/release). Type a value, or use Up/Down. */
class NumberField : public juce::TextEditor
{
public:
    NumberField (double min, double max, double step, int decimals, const juce::String& unit);
    void setValue (double v, bool notify = false);
    double getValue() const noexcept { return value; }
    std::function<void (double)> onValueChange;
    void paintOverChildren (juce::Graphics& g) override;
    bool keyPressed (const juce::KeyPress& k) override;

private:
    void commit();
    double min, max, step, value = 0.0;
    int decimals;
    juce::String unit;
};

// ---------------------------------------------------------------------------------------------
/** A settings row: bold title + wrapping description on the left, a control on the right.
    The row stacks the control under the text when the width is too small (800 x 560, §8.2.1). */
class SettingRow : public juce::Component
{
public:
    /** control: owned by the row. prefW/minW: preferred and minimum control width. heightFor: control height at a width. */
    SettingRow (const juce::String& title, const juce::String& description, std::unique_ptr<juce::Component> control, int prefW,
                int minW, std::function<int (int)> heightFor);
    juce::Component* getControl() const noexcept { return control.get(); }
    TextLabel& getTitle() noexcept { return title; }
    TextLabel& getDescription() noexcept { return desc; }
    void setDivider (bool d) { divider = d; repaint(); }
    int heightForWidth (int width) const;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    bool stacked (int width) const;
    int controlWidth (int width) const;
    TextLabel title, desc;
    std::unique_ptr<juce::Component> control;
    int prefW, minW;
    std::function<int (int)> heightFor;
    bool divider = true;
};

// ---------------------------------------------------------------------------------------------
/** Surface card with a header (title + subtitle, optional right-side component) and stacked rows. */
class SectionCard : public juce::Component
{
public:
    SectionCard (const juce::String& title, const juce::String& subtitle);
    SettingRow& addRow (std::unique_ptr<SettingRow> row);
    /** Any component as a full-width block (wrapping text, lists...). heightFor: height at a width. */
    void addBlock (std::unique_ptr<juce::Component> c, std::function<int (int)> heightFor);
    void setHeaderComponent (std::unique_ptr<juce::Component> c, int width);
    int heightForWidth (int width) const;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct Item
    {
        std::unique_ptr<juce::Component> comp;
        std::function<int (int)> heightFor;
    };
    TextLabel title, subtitle;
    std::vector<Item> items;
    std::unique_ptr<juce::Component> headerComp;
    int headerCompW = 0;
};

/** A vertical stack of cards (the content of one settings section). */
class CardColumn : public juce::Component
{
public:
    SectionCard& addCard (std::unique_ptr<SectionCard> card);
    int heightForWidth (int width) const;
    void resized() override;

private:
    std::vector<std::unique_ptr<SectionCard>> cards;
};

// ---------------------------------------------------------------------------------------------
/** Horizontal flow of components with fixed widths; wraps to more lines when needed. */
class FlowBox : public juce::Component
{
public:
    explicit FlowBox (int gap = Theme::space2) : gap (gap) {}
    void add (std::unique_ptr<juce::Component> c, int width, int height);
    void add (juce::Component& c, int width, int height); // not owned
    int heightForWidth (int width) const;
    int naturalWidth() const;
    void resized() override;

private:
    struct Item { juce::Component* c; int w, h; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<juce::Component>> owned;
    int gap;
};

// ---------------------------------------------------------------------------------------------
/** Row of radio-style buttons (ダーク / ライト, ワンショット / ループ...). */
class Segmented : public juce::Component
{
public:
    explicit Segmented (const juce::StringArray& labels);
    void setSelected (int index, bool notify = false);
    int getSelected() const noexcept { return selected; }
    int preferredWidth() const;
    std::function<void (int)> onChange;
    void resized() override;
    juce::Button* getButton (int i) const { return buttons[i]; }

private:
    juce::OwnedArray<PillButton> buttons;
    int selected = -1;
};

// ---------------------------------------------------------------------------------------------
/** In-component modal prompt (name input or confirmation), drawn over its parent with the overlay
    colour. Used inside overlays, where Navigator::showOverlay would replace the current overlay. */
class InlinePrompt : public juce::Component
{
public:
    /** withInput: shows a text field (initial text). onOk receives the text (or empty for confirmations). */
    InlinePrompt (const juce::String& title, const juce::String& message, bool withInput, const juce::String& initial,
                  const juce::String& okLabel, bool dangerous, std::function<void (const juce::String&)> onOk,
                  std::function<void()> onCancel);
    void resized() override;
    void paint (juce::Graphics& g) override;
    bool keyPressed (const juce::KeyPress& k) override;
    void parentHierarchyChanged() override;
    void ok();
    void cancel();
    juce::TextEditor* getEditor() { return input.get(); }

private:
    juce::Rectangle<int> panelBounds() const;
    TextLabel title, message;
    std::unique_ptr<juce::TextEditor> input;
    PillButton okButton, cancelButton;
    std::function<void (const juce::String&)> onOk;
    std::function<void()> onCancel;
};

// ---------------------------------------------------------------------------------------------
/** Plain panel used for overlays (help pages, soundboard slot settings): title + close (×),
    content in a scrolling viewport. Esc or × calls onClose. */
class OverlayPanel : public juce::Component
{
public:
    OverlayPanel (const juce::String& title, std::function<void()> onClose);
    /** Content is laid out at the viewport width; heightFor gives its height. */
    void setContent (std::unique_ptr<juce::Component> c, std::function<int (int)> heightFor);
    juce::Component* getContent() const noexcept { return content.get(); }
    void resized() override;
    void paint (juce::Graphics& g) override;
    bool keyPressed (const juce::KeyPress& k) override;
    IconButton& getCloseButton() noexcept { return closeButton; }

private:
    TextLabel title;
    IconButton closeButton;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> content;
    std::function<int (int)> heightFor;
    std::function<void()> onClose;
};

/** A vertical stack of components with per-item heights (help pages, wizard steps). */
class VStack : public juce::Component
{
public:
    explicit VStack (int gap = Theme::space2) : gap (gap) {}
    juce::Component& add (std::unique_ptr<juce::Component> c, std::function<int (int)> heightFor);
    TextLabel& addText (const juce::String& t, float size = Theme::fontS, Tone tone = Tone::text, bool bold = false);
    int heightForWidth (int width) const;
    void resized() override;

private:
    struct Item { std::unique_ptr<juce::Component> c; std::function<int (int)> h; };
    std::vector<Item> items;
    int gap;
};

/** Numbered step: circled number + wrapping text (Discord steps, VB-CABLE install steps). */
class NumberedStep : public juce::Component
{
public:
    NumberedStep (int number, const juce::String& text);
    int heightForWidth (int width) const;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    int number;
    TextLabel label;
};

/** Box with a border in a tone (warning box in S-04) holding wrapping text and an icon. */
class NoteBox : public juce::Component
{
public:
    NoteBox (const juce::String& text, Tone tone);
    int heightForWidth (int width) const;
    void resized() override;
    void paint (juce::Graphics& g) override;
    TextLabel& getLabel() noexcept { return label; }

private:
    Tone tone;
    TextLabel label;
};

// ---------------------------------------------------------------------------------------------
/** "Win32 RegisterHotKey" form of a JUCE key press: MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, VK code.
    Returns false for keys that cannot be a hotkey (modifier-only, unknown). */
bool keyPressToHotkey (const juce::KeyPress& key, int& modifiers, int& virtualKey);
/** True for keys that type text (letters, digits, space, punctuation): they need Ctrl or Alt. */
bool hotkeyNeedsModifier (int virtualKey);

/** Shared texts (S-04 and the help pages say the same thing). */
juce::StringArray discordSteps();
juce::String discordRecommended();
juce::String revertMicText();
inline constexpr const char* kCableUrl = "https://vb-audio.com/Cable/";

/** Recursively finds a child by component ID (Component::findChildWithID only looks one level down). */
juce::Component* findById (juce::Component& root, const juce::String& id);

/** Formats "+3.5 dB" / "0.0 dB" / "-12 dB". */
juce::String formatDb (double db, int decimals = 1);

/** Small outlined label (hotkey "Ctrl+1", "重ね 1 声"): XS bold text in a radiusS box, height m::badgeH - 4. */
int badgeWidth (const juce::String& text, bool mono);
void paintBadge (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, Tone tone, bool mono);
/** Dashed rounded outline (empty soundboard slot). */
void drawDashedRoundedRect (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness, juce::Colour c);
} // namespace koe::ui::screens

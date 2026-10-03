#pragma once

// Shared pieces of the ui-main screens (S-01, S-07, S-08, S-09): value formatting, small buttons,
// the overlay panel frame and the confirmation panel. Everything lives in koe::ui::mainui so it
// never collides with the ui-screens worker's private classes.

#include "App/AppController.h"
#include "Effects/IEffect.h"
#include "UI/Navigator.h"
#include "UI/Widgets.h"

#include <functional>

namespace koe::ui::mainui
{
/** Snapshots and tests switch animations off; Windows' "animation effects" too (F-14-6). */
bool& animationsOff();
inline bool animate() { return Theme::animationsEnabled() && ! animationsOff(); }

/** Value text with unit for an effect parameter, shared by the S-01 slots and S-09:
    dB 0.1 steps, Hz -> kHz from 1000, unitless 0..1 -> %, st, :1, ms, choices -> their label. */
juce::String formatParam (const ParamSpec& p, float v);
/** Tooltip label: "ドライブ（0〜36 dB、初期値 12 dB）" (F-13-6: name, range and unit, default). */
juce::String paramLabel (const ParamSpec& p);
/** Range, step and skew for a parameter on any slider (Knob / ValueSlider), then the value. */
void applyParamRange (juce::Slider& s, const ParamSpec& p, float value);
/** First two non-choice parameters (the slot's small knobs, F-04-22). */
std::vector<int> mainParams (const EffectInfo& info);
/** Pitch / formant / layer text: "-9", "+2.5" (0.1 steps, trailing .0 dropped). */
juce::String formatSemitones (double v);

/** Width of s in f, rounded up. */
int textWidth (const juce::Font& f, const juce::String& s);
/** paintCard's header without the card fill: mono "NN" + bold title in row. Returns the row right of the title. */
juce::Rectangle<int> drawCardHeader (juce::Graphics& g, juce::Rectangle<int> row, const juce::String& number, const juce::String& title);

/** Japanese name of a preset category id ("character" -> "キャラ"). */
juce::String presetCategoryJa (const juce::String& id);

/** Depth-first search by component ID (tour targets, tests). */
juce::Component* findById (juce::Component* root, const juce::String& id);
/** c and every parent below root are visible (root itself may be offscreen; isShowing() is false there). */
bool visibleWithin (const juce::Component* c, const juce::Component* root);

/** E-27: runs action now, or after "ルーパーの録音が消えます" when the looper holds a recording. */
void withLooperCheck (AppController& c, Navigator& nav, std::function<void()> action);
/** Same, for an action that uses `owner`: if owner is deleted while the confirmation is up (cards rebuilt
    by a hotkey), pressing 続ける does nothing instead of running on a deleted component. */
void withLooperCheck (AppController& c, Navigator& nav, juce::Component& owner, std::function<void()> action);
/** action, run only while owner still exists. */
std::function<void()> guardedBy (juce::Component& owner, std::function<void()> action);

// ---------------------------------------------------------------------------------------------
/** Small square outlined icon button (slot ◀ ▶ ×, banner ×). */
class SquareIconButton : public juce::Button
{
public:
    SquareIconButton (const juce::String& name, Icon icon);
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    Icon icon;
};

/** Underlined accent text ("設定で変更", "スキップ"). */
class LinkButton : public juce::Button
{
public:
    explicit LinkButton (const juce::String& text, bool subtle = false);
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    int preferredWidth() const;

private:
    bool subtle;
};

/** Dashed outline button with a plus ("エフェクトを追加", "声を追加"). */
class DashedButton : public juce::Button
{
public:
    explicit DashedButton (const juce::String& text, bool vertical = false);
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    bool vertical;
};

/** Thin horizontal slider (layer pitch/formant/level, monitor volume): 6 px track, accent fill
    (from 0 for ranges that cross 0), round thumb. Double-click = default, arrows step. */
class LineSlider : public juce::Slider
{
public:
    LineSlider();
    void paint (juce::Graphics& g) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
};

/** "label [----o----] value" in one row, or stacked (mock S-01 card 03): "label ... value" over a full-width
    slider. onChange runs after the value text is repainted. */
class SliderRow : public juce::Component
{
public:
    SliderRow (const juce::String& label, std::function<juce::String (double)> format);
    void paint (juce::Graphics& g) override;
    void resized() override;
    LineSlider slider;
    std::function<void (double)> onChange;
    int labelWidth = Theme::space5 * 2 + Theme::space2; // 72
    int valueWidth = Theme::space5 * 2;                 // 64
    bool stacked = false;
    static constexpr int stackedHeight = Theme::space5;  // 16 text + 16 slider

private:
    juce::String label;
    std::function<juce::String (double)> format;
};

/** 32 px pill chip. Favourite style: selected = raised fill + accent border + check mark.
    Filter style: selected = accent fill. */
class ChipButton : public juce::Button
{
public:
    ChipButton (const juce::String& text, bool filterStyle);
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    int preferredWidth() const;

private:
    bool filterStyle;
};

// ---------------------------------------------------------------------------------------------
/** Overlay panel frame (S-07 look): surface, border, 64 px header with title and ×.
    Subclasses lay out their content in contentArea(). Esc / × call nav.closeOverlay(). */
class PanelBase : public juce::Component
{
public:
    PanelBase (Navigator& nav, const juce::String& title);
    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& k) override;

protected:
    juce::Rectangle<int> contentArea() const;
    /** Text drawn right of the title (e.g. "追加先: スロット 5"). */
    juce::String subtitle;
    juce::String title;
    Navigator& nav;
    static constexpr int headerHeight = Theme::space5 * 2; // 64

private:
    IconButton closeButton;
};

/** "続けますか" style question with two buttons, optionally a name field (save as). */
class ConfirmPanel : public PanelBase
{
public:
    ConfirmPanel (Navigator& nav, const juce::String& title, const juce::String& message, const juce::String& okText,
                  std::function<void()> onOk, const juce::String& cancelText = {}, std::function<void()> onCancel = {});
    /** Adds a text field; onOkWithText receives its text. */
    void addTextField (const juce::String& initial, std::function<void (const juce::String&)> onOkWithText);
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::String message;
    PillButton ok, cancel;
    std::unique_ptr<juce::TextEditor> field;
    std::function<void()> onOk, onCancel;
    std::function<void (const juce::String&)> onOkText;
};
} // namespace koe::ui::mainui

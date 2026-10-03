#pragma once

// 案 C Mono S-01 parts (docs/mockups/C-S01.dc.html), owner wave5/mono. Colours come from Theme::colours() roles only:
// the monochrome look is the "builtin:mono" palette (wave5/themes); the page also reads with the Studio palettes.
// Every part carries the tour / test IDs of INTERFACES.md §8.3 for the same meaning as on the Studio page.
//
// Spec D-23 set C aside for three reasons, fixed here:
//  - the selected preset and ボイチェン ON looked the same (a white fill): the selected preset row is a check mark and
//    an outline on the raised colour, ON is the only filled block;
//  - the 248 px preset column narrowed the body in the smallest window: below 1000 px it folds into a preset button
//    that opens the same list as a panel;
//  - the chain runs top to bottom: allowed for C by INTERFACES.md §8.3, and the rack says 「処理は上から下へ」.

#include "UI/main/Common.h"

namespace koe::ui::mono
{
using mainui::ChipButton;
using mainui::DashedButton;
using mainui::LineSlider;
using mainui::LinkButton;
using mainui::PanelBase;
using mainui::SliderRow;
using mainui::SquareIconButton;

// =============================================================================================== presets
/** Search, category chips (全部 / ★ / categories), the preset list with stars, 保存 / 複製 / 一覧.
    The left column of the wide layout, and the content of PresetPanel in the compact one. */
class PresetSidebar : public juce::Component
{
public:
    PresetSidebar (AppController& c, Navigator& nav);
    ~PresetSidebar() override;
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    /** "all", "fav" or a category id ("character"). */
    void setFilter (const juce::String& id);
    juce::String getFilter() const { return filter; }
    void setSearchText (const juce::String& text);
    /** Preset ids listed, in order (tests). */
    juce::StringArray listedIds() const;

private:
    class Row;
    void rebuild();
    void layoutRows();
    void choose (const std::string& id);
    void savePreset();
    void askName (const juce::String& title, const juce::String& initial);
    AppController& c;
    Navigator& nav;
    juce::String filter, sig;
    juce::TextEditor search;
    juce::OwnedArray<ChipButton> chips;
    juce::StringArray chipIds;
    juce::Viewport view;
    juce::Component list;
    juce::OwnedArray<Row> rows;
    PillButton save, dup, browse;
    juce::Rectangle<int> listHeader;
    std::unique_ptr<juce::Label> empty;
};

/** The compact layout's preset list: PresetSidebar in an overlay panel (stays open while trying presets). */
class PresetPanel : public PanelBase, private juce::ChangeListener
{
public:
    PresetPanel (AppController& c, Navigator& nav);
    ~PresetPanel() override;
    void resized() override;
    PresetSidebar& sidebar() { return content; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { content.refresh(); }
    AppController& c;
    PresetSidebar content;
};

// =============================================================================================== top row
/** The big ボイチェン ON block: filled (accent) when ON, outlined "OFF" otherwise. */
class VoiceBlock : public juce::Button
{
public:
    explicit VoiceBlock (AppController& c);
    void refresh();
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    AppController& c;
};

/** マイクミュート: outlined, or the danger fill with "MUTE" (never colour alone, F-08-6). */
class MuteBlock : public juce::Button
{
public:
    explicit MuteBlock (AppController& c);
    void refresh();
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    AppController& c;
};

/** 入力 / 出力: label, level meter, value; under it (wide) the microphone and ゲート, or the 出力先, クリップ and
    「設定で変更」. Compact: one line, with the microphone list / the output name and link at the end. */
class LevelRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    LevelRow (AppController& c, Navigator& nav, bool output);
    void setCompact (bool compact);
    void refresh();
    void tick (const AppController::Meters& m, const AppController::Status& s);
    void resized() override;
    void paint (juce::Graphics& g) override;
    static int height (bool compact) { return compact ? Theme::space4 + Theme::space1 : Theme::space5 + Theme::space3 - Theme::space1; } // 28 / 44

private:
    AppController& c;
    Navigator& nav;
    const bool output;
    bool compact = false, gateOn = true, gateOpen = false, clipping = false, limiter = false;
    LevelMeter meter;
    juce::ComboBox device;
    juce::StringArray items;
    std::unique_ptr<LinkButton> link;
    juce::String deviceName;
    float gainDb = 0.0f, shown = -100.0f;
    int levelText = -60;
    juce::Rectangle<int> labelArea, valueArea, nameArea, statusArea, gainArea;
};

// =============================================================================================== cards
/** ピッチ / フォルマント: the value in huge mono digits, − / + and a slider. The pitch card carries 変換 ON/OFF. */
class BigValueCard : public juce::Component
{
public:
    BigValueCard (AppController& c, bool formant);
    void setCompact (bool compact);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
    void step (int direction);

private:
    void setValue (double v);
    AppController& c;
    const bool formant;
    bool compact = false, active = true;
    PillButton minus, plus;
    LineSlider slider;
    std::unique_ptr<ToggleSwitch> toggle; // pitch card only
    juce::Rectangle<int> header, number, hint;
};

/** voice.shifter: the two value cards side by side. */
class ShifterGroup : public juce::Component
{
public:
    explicit ShifterGroup (AppController& c);
    void setCompact (bool compact);
    void refresh();
    void resized() override;
    BigValueCard pitch, formant;
};

/** 重ねる声: count, one row per voice (opens LayerEditPanel), 声を追加, 自動停止 / 再開. */
class LayersCard : public juce::Component
{
public:
    LayersCard (AppController& c, Navigator& nav);
    ~LayersCard() override;
    void setCompact (bool compact);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class Row;
    AppController& c;
    Navigator& nav;
    bool compact = false, autoStopped = false;
    int built = -1;
    juce::OwnedArray<Row> rows;
    DashedButton add;
    LinkButton resume;
    juce::Rectangle<int> header, countArea;
};

// =============================================================================================== chain
/** The effect chain as a rack: one row per slot in processing order (top to bottom), drag to reorder. */
class ChainRack : public juce::Component
{
public:
    ChainRack (AppController& c, Navigator& nav);
    ~ChainRack() override;
    void setCompact (bool compact);
    /** Rebuilds the rows when the chain structure changed, otherwise updates values (and the 自動停止 marks). */
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    int numRows() const { return rows.size(); }
    juce::Viewport& viewport() { return view; }

    /** Header + rows of this many slots + the add button (the page keeps at least two rows in view). */
    static int heightFor (int slots, bool compact);
    static int rowHeight (bool compact) { return compact ? Theme::space5 + Theme::space2 + Theme::space1 : Theme::space5 + Theme::space3; } // 44 / 48

    class Row;
    void dragMove (Row& row, const juce::MouseEvent& e);
    void dragEnd (Row& row);

private:
    void layoutRows();
    juce::Rectangle<int> headerArea() const;
    AppController& c;
    Navigator& nav;
    bool compact = false;
    juce::StringArray types;
    juce::Viewport view;
    juce::Component content;
    juce::OwnedArray<Row> rows;
    DashedButton add;
    int dropIndex = -1, headerState = -1;
};
} // namespace koe::ui::mono

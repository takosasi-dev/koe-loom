#pragma once

// Overlay panels of the voice page: S-07 エフェクト選択 (docs/mockups/A-S07.dc.html) and
// S-09 スロット詳細 (no mock: same panel style as S-07, F-04-22).

#include "UI/main/Common.h"

namespace koe::ui::mainui
{
/** S-07: category filter, name search, rows with name / 軽中重 / description / 追加 (F-04-10, F-04-15). */
class EffectPicker : public PanelBase
{
public:
    /** insertAt < 0 appends; otherwise the new slot is moved to that position. */
    EffectPicker (AppController& c, Navigator& nav, int insertAt);
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    void setSearchText (const juce::String& text);
    /** -1 = すべて, otherwise an EffectCategory index. */
    void setCategory (int category);
    /** Types currently listed, in order (tests). */
    std::vector<std::string> listedTypes() const;

private:
    class Row;
    void rebuild();
    AppController& c;
    const int insertAt;
    int category = -1;
    juce::TextEditor search;
    juce::OwnedArray<ChipButton> filters;
    juce::Viewport view;
    juce::Component list;
    juce::OwnedArray<Row> rows;
    juce::Rectangle<int> footer;
};

/** S-09: every parameter of one slot (knobs, choices as ComboBox), ON/OFF, freeze / looper buttons. */
class SlotDetailPanel : public PanelBase, private juce::ChangeListener, private juce::Timer
{
public:
    SlotDetailPanel (AppController& c, Navigator& nav, int slot);
    ~SlotDetailPanel() override;
    void resized() override;
    void paint (juce::Graphics& g) override;

    int slotIndex() const { return slot; }
    int numKnobs() const;
    int numChoices() const;
    /** The control of parameter p (a Knob or a ComboBox), for tests. */
    juce::Component* controlFor (int paramIndex) const;
    /** "convolution" only (INTERFACES.md §9.3): the line under the file row, e.g. ファイルが見つかりません（x.wav）. */
    juce::String fileStatusText() const;
    bool fileStatusIsWarning() const;

private:
    struct Cell
    {
        int param = 0;
        std::unique_ptr<Knob> knob;
        std::unique_ptr<juce::ComboBox> combo;
        juce::Rectangle<int> labelArea, valueArea;
    };
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateValues();
    void updateActions();
    int columns() const;

    AppController& c;
    const int slot;
    const std::string type;
    const EffectInfo* info = nullptr;
    std::vector<Cell> cells;
    const bool bars = Theme::prefs().knobStyle == 1; // S-03 つまみの形 = 棒 (wave10/ui): lower, wider cells
    ToggleSwitch toggle;
    std::unique_ptr<PillButton> action1, action2;
    int uiState = -1;
    juce::Rectangle<int> descArea, footerText;
    // "convolution": ファイルを選ぶ… / the ir folder list / フォルダを開く, and the status line (INTERFACES.md §9.3)
    void pickIrFile();
    void refreshIrList();
    std::unique_ptr<PillButton> chooseFile, openFolder;
    std::unique_ptr<juce::ComboBox> irList;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String shownFile;
    juce::Rectangle<int> fileStatus;
    class ModRow; // 声の大きさで動かす (wave9/voice, INTERFACES.md §11.3)
    std::unique_ptr<ModRow> modRow;
    std::unique_ptr<juce::Component> spectrum; // 声の見える化 (wave10/viz, INTERFACES.md §12), above modRow
};
} // namespace koe::ui::mainui

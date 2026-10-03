// 案 B "Paper" S-01 (docs/mockups/B-S01.dc.html), owner wave5/paper (INTERFACES.md §8): the signal path strip
// (入力 › ノイズ抑制 › ゲート › 声の変換 › エフェクト › 出力 › 仮想マイク), the preset row (large name, text links),
// three columns ruled apart (入力 / 声の変換 + 重ねる声 / 出力), the chain as a numbered timeline and the shared
// bottom bar. Below 1000 px (§8.2.1) the columns become two (声の変換 | 入力と出力), and when banners leave too little
// height the strip (information only, every value is also in a column) gives way first.
// Same controller calls and tour IDs as the Studio page (VoiceView); colours only from Theme::colours() roles.

#include "UI/main/VoicePage.h"

#include "Effects/EffectRegistry.h"
#include "UI/main/VoiceView.h"
#include "UI/skins/paper/PaperChain.h"

#include <cmath>

namespace koe::ui::mainui
{
namespace
{
using paper::drawStatus;
using paper::PaperChain;
using paper::PaperSlider;
using paper::SegmentMeter;
using paper::TextLink;

const Palette& P() { return Theme::colours(); }

juce::String dbText (double db)
{
    const double r = std::round (db * 10.0) / 10.0;
    const bool whole = std::abs (r - std::round (r)) < 1.0e-6;
    return (whole ? juce::String (juce::roundToInt (r)) : juce::String (r, 1)) + " dB";
}
juce::String stText (double st) { return formatSemitones (st) + " st"; }
juce::String degreeText (double d) { return formatSemitones (d) + ja (" 度"); }

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

/** Level text that falls back slowly, like the Studio page's "入力レベル". */
struct LevelText
{
    float shown = -100.0f;
    int value = -60;
    bool feed (float db)
    {
        shown = db > shown ? db : juce::jmax (db, shown - 0.7f);
        const int v = juce::jmax (-60, juce::roundToInt (shown));
        return std::exchange (value, v) != v;
    }
};

/** Section heading of a column: bold title, optional small text at the right. */
void drawHeading (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, bool compact, const juce::String& right = {})
{
    const auto& p = P();
    drawText (g, title, r, compact ? Theme::fontXS : Theme::fontS, p.text, juce::Justification::centredLeft, true);
    if (right.isNotEmpty()) drawText (g, right, r, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
}

/** "label" in sub text, returns the rest of r. */
juce::Rectangle<int> drawLabel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    drawText (g, text, r.removeFromLeft (textWidth (Theme::ui (Theme::fontXS), text) + 2), Theme::fontXS, P().textSub);
    return r.withTrimmedLeft (Theme::space1);
}

// Row metrics of the 声の変換 column: roomy when there is height for it, tight otherwise (banners, 800×560).
struct Metrics
{
    int head, row, rowGap, rule, layerHead, layerRow;
    int shifterH() const { return head + Theme::space1 + 2 * row + rowGap; }
    int layersH (int n) const { return layerHead + n * (layerRow + 2) + (n == 0 ? Theme::space3 + 4 : 0); }
    int columnH (int n) const { return shifterH() + rule + layersH (n); }
};
Metrics metrics (bool compact, bool roomy)
{
    if (compact) return roomy ? Metrics { 20, 30, 2, 10, 20, 26 } : Metrics { 18, 26, 2, 6, 18, 22 };
    return roomy ? Metrics { 26, 40, 4, 16, 24, 28 } : Metrics { 22, 28, 2, 8, 20, 24 };
}

// =============================================================================================== signal path strip
class PathStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PathStrip (AppController& ctl) : c (ctl)
    {
        setComponentID ("paper.path");
        setTooltip (ja ("声の通り道（左から右へ処理します）。値はいまの状態です"));
    }

    static int height (bool compact) { return compact ? 40 : 52; }
    void setCompact (bool cp) { compact = cp; repaint(); }
    void refresh() { repaint(); }

    void tick (const AppController::Meters& m, const AppController::Status& s)
    {
        if (level.feed (m.inputDb) || s.gateOpen != gateOpen)
        {
            gateOpen = s.gateOpen;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        g.setColour (p.border);
        g.fillRect (0, 0, getWidth(), 1);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        const auto& st = c.getSettings();
        const auto device = c.getOutputDevice();
        const bool cable = AppController::isCableInputName (device);
        const bool gateShown = st.gateOn && gateOpen;
        struct Item { juce::String label, value; std::optional<Icon> icon; juce::Colour colour; bool mono; };
        const std::vector<Item> items {
            { ja ("入力"), juce::String (level.value) + " dB", std::nullopt, p.text, true },
            { ja ("ノイズ抑制"), st.noiseSuppressionOn ? ja ("ON") : ja ("OFF"), st.noiseSuppressionOn ? Icon::check : Icon::close,
              st.noiseSuppressionOn ? p.ok : p.textSub, true },
            { ja ("ゲート"), ! st.gateOn ? ja ("OFF") : (gateOpen ? ja ("開") : ja ("閉")), gateShown ? Icon::check : Icon::close, gateShown ? p.ok : p.textSub, false },
            { ja ("声の変換"), c.hasShifter() ? formatSemitones (c.getPitch()) + " / " + formatSemitones (c.getFormant()) + " st" : ja ("OFF"), std::nullopt,
              c.hasShifter() ? p.text : p.textSub, true },
            { ja ("エフェクト"), juce::String (int (c.getChain().size())) + ja (" スロット"), std::nullopt, p.text, true },
            { ja ("出力"), juce::String (st.outputGainDb, 1) + " dB", std::nullopt, p.text, true },
            { ja ("仮想マイク"), device.isEmpty() ? ja ("未選択") : device, cable ? Icon::check : Icon::warning, cable ? p.ok : p.warn, true },
        };
        const int n = int (items.size());
        const int chevW = compact ? 12 : 20;
        auto r = getLocalBounds().reduced (compact ? Theme::space1 : Theme::space3 - 4, 1);
        const int cellW = (r.getWidth() - (n - 1) * chevW) / n;
        for (int i = 0; i < n; ++i)
        {
            auto cell = i == n - 1 ? r : r.removeFromLeft (cellW);
            const auto& it = items[size_t (i)];
            drawText (g, it.label, cell.removeFromTop (cell.getHeight() / 2).withTrimmedTop (compact ? 2 : Theme::space1), Theme::fontXS, p.textSub);
            auto v = cell.withTrimmedBottom (compact ? 2 : Theme::space1);
            if (it.icon)
            {
                drawIcon (g, *it.icon, v.removeFromLeft (14).withSizeKeepingCentre (13, 13).toFloat(), it.colour, 2.6f);
                v.removeFromLeft (Theme::space1);
            }
            const auto valueColour = i == n - 1 && cable ? p.text : it.colour; // the device name reads as text, the check says OK
            drawText (g, it.value, v, compact ? Theme::fontXS : Theme::fontS, valueColour, juce::Justification::centredLeft, true, it.mono);
            if (i < n - 1) drawIcon (g, Icon::chevronRight, r.removeFromLeft (chevW).withSizeKeepingCentre (12, 12).toFloat(), p.textSub, 2.0f);
        }
    }

private:
    AppController& c;
    bool compact = false, gateOpen = false;
    LevelText level;
};

// =============================================================================================== preset row
class PresetSelector : public juce::Button
{
public:
    PresetSelector() : juce::Button ("preset")
    {
        setComponentID ("voice.presetSelector");
        setWantsKeyboardFocus (true);
        setTitle (ja ("プリセットを選ぶ"));
        setTooltip (ja ("プリセットを選ぶ（押すと一覧から切り替え）"));
    }
    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& p = P();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (highlighted ? p.raised : p.surface);
        g.fillRoundedRectangle (r, Theme::radiusS);
        g.setColour (p.border);
        g.drawRoundedRectangle (r, Theme::radiusS, 1.0f);
        auto inner = getLocalBounds().reduced (Theme::space2 + 2, 0);
        drawIcon (g, Icon::chevronDown, inner.removeFromRight (16).withSizeKeepingCentre (16, 16).toFloat(), p.textSub);
        inner.removeFromRight (Theme::space1);
        drawText (g, name, inner, Theme::fontS, p.text, juce::Justification::centredLeft, true);
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusS);
    }
    juce::String name;
};

class PresetRow : public juce::Component
{
public:
    PresetRow (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), list (ja ("一覧")), fav (ja ("お気に入り"), Theme::fontS, Icon::star), save (ja ("保存")), dup (ja ("複製"))
    {
        setComponentID ("tour.presets");
        selector.onClick = [this] { showPresetMenu(); };
        addAndMakeVisible (selector);
        list.setComponentID ("voice.presetList");
        list.setTooltip (ja ("プリセット一覧を開く（検索・お気に入り・名前の変更）"));
        list.onClick = [this] { nav.showPresetBrowser(); };
        fav.setComponentID ("voice.favorites");
        fav.onClick = [this] { showFavouriteMenu(); };
        save.setComponentID ("voice.presetSave");
        save.setTooltip (ja ("いまの設定を保存する"));
        save.onClick = [this] { savePreset(); };
        dup.setComponentID ("voice.presetDuplicate");
        dup.setTooltip (ja ("いまの設定を別の名前で保存する"));
        dup.onClick = [this] { askName (ja ("複製"), c.getCurrentPreset().name + ja (" のコピー")); };
        for (auto* b : { &list, &fav, &save, &dup }) addAndMakeVisible (b);
    }

    static int height (bool compact) { return compact ? 46 : 60; }
    void setCompact (bool cp) { compact = cp; resized(); repaint(); }

    void refresh()
    {
        selector.name = c.getCurrentPreset().name;
        selector.repaint();
        const bool isFav = c.isFavorite (c.getCurrentPreset().id);
        fav.setIcon (isFav ? Icon::starFilled : Icon::star); // filled = this preset is a favourite (shape, not colour)
        fav.setTooltip (isFav ? ja ("お気に入りから選ぶ（このプリセットはお気に入りです）") : ja ("お気に入りから選ぶ・このプリセットを追加する"));
        repaint();
    }

    void resized() override
    {
        const int lh = compact ? Theme::touchMin : Theme::buttonH;
        auto row = getLocalBounds().withTrimmedTop (labelH());
        for (auto* b : { &dup, &save, &fav, &list })
        {
            b->setBounds (row.removeFromRight (b->preferredWidth()).withSizeKeepingCentre (b->preferredWidth(), lh));
            row.removeFromRight (compact ? Theme::space3 - 4 : Theme::space4);
        }
        const int selW = compact ? 180 : 220;
        selector.setBounds (row.removeFromRight (selW).withSizeKeepingCentre (selW, lh));
        row.removeFromRight (Theme::space3);
        nameArea = row;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto& cur = c.getCurrentPreset();
        drawText (g, ja ("プリセット"), getLocalBounds().removeFromTop (labelH()), Theme::fontXS, p.textSub);
        auto r = nameArea;
        const float nameSize = compact ? Theme::fontL : Theme::fontXL;
        const auto meta = metaText();
        const int metaW = juce::jmin (r.getWidth() / 2, textWidth (Theme::ui (Theme::fontXS), meta) + 2);
        const int nameW = juce::jmin (r.getWidth() - metaW - Theme::space3, textWidth (Theme::ui (nameSize, true), cur.name) + 2);
        drawText (g, cur.name, r.removeFromLeft (nameW), nameSize, p.text, juce::Justification::centredLeft, true);
        r.removeFromLeft (Theme::space3);
        drawText (g, meta, r, Theme::fontXS, p.textSub);
    }

    void load (const std::string& id) { withLooperCheck (c, nav, *this, [this, id] { c.loadPreset (id); }); } // E-27

    void toggleFavourite()
    {
        juce::String why;
        if (! c.toggleFavorite (c.getCurrentPreset().id, why)) nav.showToast (why); // F-05-8: 9 at most
    }

private:
    int labelH() const { return compact ? 14 : 18; }

    /** "キャラ ・ 重ね 1 声 ・ 軽 2 ・ 中 2" (+ "・ 編集中"). */
    juce::String metaText() const
    {
        const auto& cur = c.getCurrentPreset();
        juce::StringArray parts { presetCategoryJa (cur.category()) };
        if (c.getNumLayers() > 0) parts.add (ja ("重ね ") + juce::String (c.getNumLayers()) + ja (" 声"));
        for (auto w : { EffectWeight::light, EffectWeight::medium, EffectWeight::heavy })
        {
            int count = 0;
            for (auto& s : c.getChain())
                if (auto* i = findEffectInfo (s.type); i != nullptr && i->weight == w) ++count;
            if (count > 0) parts.add (juce::String::fromUTF8 (weightNameJa (w)) + " " + juce::String (count));
        }
        if (c.isCurrentPresetModified()) parts.add (ja ("編集中"));
        return parts.joinIntoString (ja (" ・ "));
    }

    void showPresetMenu()
    {
        const auto& all = c.getPresetLibrary().all();
        const char* order[] = { "natural", "character", "device", "space", "layered", "user" };
        juce::PopupMenu m;
        for (auto* cat : order)
        {
            juce::PopupMenu sub;
            for (int i = 0; i < int (all.size()); ++i)
                if (all[size_t (i)].category() == cat) sub.addItem (i + 1, all[size_t (i)].name, true, all[size_t (i)].id == c.getCurrentPreset().id);
            if (sub.getNumItems() > 0) m.addSubMenu (presetCategoryJa (cat), sub);
        }
        m.addSeparator();
        m.addItem (-1, ja ("プリセット一覧を開く"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (selector),
                         [safe = juce::Component::SafePointer<PresetRow> (this)] (int r)
                         {
                             if (safe == nullptr || r == 0) return;
                             if (r < 0) { safe->nav.showPresetBrowser(); return; }
                             const auto& list = safe->c.getPresetLibrary().all();
                             if (r - 1 < int (list.size())) safe->load (list[size_t (r - 1)].id);
                         });
    }

    /** F-05-12: the favourites to switch to (the current one checked), and adding / removing the current preset. */
    void showFavouriteMenu()
    {
        juce::PopupMenu m;
        juce::StringArray ids;
        for (auto& id : c.getFavorites())
            if (c.getPresetLibrary().find (id.toStdString()) != nullptr) ids.add (id);
        for (int i = 0; i < ids.size(); ++i)
            m.addItem (i + 1, c.getPresetLibrary().find (ids[i].toStdString())->name, true, ids[i].toStdString() == c.getCurrentPreset().id);
        if (ids.isEmpty()) m.addItem (-2, ja ("お気に入りはまだありません"), false);
        m.addSeparator();
        m.addItem (100, c.isFavorite (c.getCurrentPreset().id) ? ja ("このプリセットをお気に入りから外す") : ja ("このプリセットをお気に入りに追加"));
        m.addItem (101, ja ("プリセット一覧で並べ替える"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (fav),
                         [safe = juce::Component::SafePointer<PresetRow> (this), ids] (int r)
                         {
                             if (safe == nullptr || r == 0) return;
                             if (r == 100) safe->toggleFavourite();
                             else if (r == 101) safe->nav.showPresetBrowser();
                             else if (r > 0 && r <= ids.size()) safe->load (ids[r - 1].toStdString());
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
    TextLink list, fav, save, dup;
    juce::Rectangle<int> nameArea;
};

// =============================================================================================== 入力
class InputColumn : public juce::Component
{
public:
    explicit InputColumn (AppController& ctl) : c (ctl)
    {
        setComponentID ("voice.input");
        device.setComponentID ("voice.inputDevice");
        device.setTitle (ja ("入力デバイス"));
        device.setTooltip (ja ("声を拾うマイクを選びます"));
        device.setTextWhenNothingSelected (ja ("未選択"));
        device.setTextWhenNoChoicesAvailable (ja ("入力デバイスがありません"));
        device.onChange = [this] { if (device.getSelectedId() > 0) c.setInputDevice (device.getText()); };
        addAndMakeVisible (device);
        meter.setComponentID ("voice.inputMeter");
        addAndMakeVisible (meter);
    }

    static int needed (bool compact)
    {
        return compact ? 26 + 4 + SegmentMeter::barH + 4 + 16 : 26 + 6 + Theme::buttonH + 12 + SegmentMeter::height (true) + 12 + 20;
    }

    void setCompact (bool cp)
    {
        compact = cp;
        meter.setShowScale (! cp);
        resized();
        repaint();
    }

    void refresh()
    {
        fillCombo (device, c.getInputDevices(), items, c.getInputDevice());
        repaint();
    }

    void tick (const AppController::Meters& m, const AppController::Status& s)
    {
        meter.setLevel (m.inputDb, m.inputClip);
        if (level.feed (m.inputDb) || s.gateOpen != gateOpen)
        {
            gateOpen = s.gateOpen;
            repaint();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (compact)
        {
            auto row = r.removeFromTop (26);
            labelArea = row.removeFromLeft (Theme::space5);
            row.removeFromLeft (Theme::space2);
            device.setBounds (row);
            r.removeFromTop (4);
            auto meterRow = r.removeFromTop (SegmentMeter::barH).withTrimmedLeft (Theme::space5 + Theme::space2);
            valueArea = meterRow.removeFromRight (Theme::space5).expanded (0, 3);
            meterRow.removeFromRight (Theme::space2);
            meter.setBounds (meterRow);
            r.removeFromTop (4);
            statusArea = r.removeFromTop (16).withTrimmedLeft (Theme::space5 + Theme::space2);
            return;
        }
        headArea = r.removeFromTop (26);
        r.removeFromTop (6);
        device.setBounds (r.removeFromTop (Theme::buttonH));
        r.removeFromTop (12);
        meter.setBounds (r.removeFromTop (SegmentMeter::height (true)));
        r.removeFromTop (12);
        statusArea = r.removeFromTop (20);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto& st = c.getSettings();
        if (compact)
        {
            drawText (g, ja ("入力"), labelArea, Theme::fontXS, p.text, juce::Justification::centredLeft, true);
            drawText (g, juce::String (level.value), valueArea, Theme::fontXS, p.text, juce::Justification::centredRight, true, true);
        }
        else
            drawHeading (g, headArea, ja ("入力"), false, juce::String (level.value) + " dB");

        // ゲート ✓ 開   ノイズ抑制 ✓ ON (text and icon, never colour alone)
        const float size = compact ? Theme::fontXS : Theme::fontS;
        const bool open = st.gateOn && gateOpen;
        auto r = drawLabel (g, statusArea, ja ("ゲート"));
        r.removeFromLeft (drawStatus (g, r, open ? Icon::check : Icon::close, ! st.gateOn ? ja ("OFF") : (gateOpen ? ja ("開") : ja ("閉")), open ? p.ok : p.textSub, size));
        r = drawLabel (g, r.withTrimmedLeft (compact ? Theme::space3 : Theme::space4), ja ("ノイズ抑制"));
        drawStatus (g, r, st.noiseSuppressionOn ? Icon::check : Icon::close, st.noiseSuppressionOn ? ja ("ON") : ja ("OFF"), st.noiseSuppressionOn ? p.ok : p.textSub, size, true);
    }

private:
    AppController& c;
    juce::ComboBox device;
    juce::StringArray items;
    SegmentMeter meter;
    LevelText level;
    bool compact = false, gateOpen = false;
    juce::Rectangle<int> headArea, labelArea, valueArea, statusArea;
};

// =============================================================================================== 出力
class OutputColumn : public juce::Component
{
public:
    OutputColumn (AppController& ctl, Navigator& n) : c (ctl), nav (n), deviceLink (ja ("設定で変更"), Theme::fontXS)
    {
        setComponentID ("voice.output");
        meter.setComponentID ("tour.outputMeter");
        addAndMakeVisible (meter);
        deviceLink.setComponentID ("voice.outputDeviceLink");
        deviceLink.setTooltip (ja ("出力先（仮想マイク）は設定のデバイスで選びます"));
        deviceLink.onClick = [this] { nav.showSettings (Navigator::SettingsSection::devices); }; // F-01-11
        addAndMakeVisible (deviceLink);
    }

    static int needed (bool compact) { return compact ? 26 + 4 + SegmentMeter::barH + 4 + 16 : 26 + 6 + SegmentMeter::height (true) + 12 + 40 + 12 + 24; }

    void setCompact (bool cp)
    {
        compact = cp;
        meter.setShowScale (! cp);
        resized();
        repaint();
    }

    void refresh()
    {
        deviceName = c.getOutputDevice();
        gainDb = c.getSettings().outputGainDb;
        repaint();
    }

    void tick (const AppController::Meters& m, const AppController::Status& s)
    {
        meter.setLevel (m.outputDb, m.outputClip);
        const bool changed = level.feed (m.outputDb);
        if (changed || meter.isClipping() != clipping || s.limiterActive != limiter)
        {
            clipping = meter.isClipping();
            limiter = s.limiterActive;
            repaint();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (compact)
        {
            auto row = r.removeFromTop (26);
            labelArea = row.removeFromLeft (Theme::space5);
            row.removeFromLeft (Theme::space2);
            deviceLink.setBounds (row.removeFromRight (deviceLink.preferredWidth()));
            row.removeFromRight (Theme::space2);
            nameArea = row;
            r.removeFromTop (4);
            auto meterRow = r.removeFromTop (SegmentMeter::barH).withTrimmedLeft (Theme::space5 + Theme::space2);
            valueArea = meterRow.removeFromRight (Theme::space5).expanded (0, 3);
            meterRow.removeFromRight (Theme::space2);
            meter.setBounds (meterRow);
            r.removeFromTop (4);
            infoArea = r.removeFromTop (16).withTrimmedLeft (Theme::space5 + Theme::space2);
            return;
        }
        headArea = r.removeFromTop (26);
        r.removeFromTop (6);
        meter.setBounds (r.removeFromTop (SegmentMeter::height (true)));
        r.removeFromTop (12);
        infoArea = r.removeFromTop (40);
        r.removeFromTop (12);
        auto row = r.removeFromTop (24);
        deviceLink.setBounds (row.removeFromRight (deviceLink.preferredWidth()));
        row.removeFromRight (Theme::space2);
        nameArea = row;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const bool unset = deviceName.isEmpty();
        const bool cable = AppController::isCableInputName (deviceName);
        const auto gain = juce::String (gainDb, 1) + " dB";
        auto clipStatus = [&] (juce::Rectangle<int> row, float size)
        {
            if (clipping) drawStatus (g, row, Icon::warning, ja ("あり"), p.danger, size);
            else if (limiter) drawStatus (g, row, Icon::warning, ja ("リミッター動作中"), p.warn, Theme::fontXS); // F-08-2
            else drawStatus (g, row, Icon::check, ja ("なし"), p.ok, size);
        };
        auto name = nameArea;
        if (compact)
        {
            drawText (g, ja ("出力"), labelArea, Theme::fontXS, p.text, juce::Justification::centredLeft, true);
            drawText (g, juce::String (level.value), valueArea, Theme::fontXS, p.text, juce::Justification::centredRight, true, true);
            auto r = drawLabel (g, infoArea, ja ("出力ゲイン"));
            drawText (g, gain, r.removeFromLeft (int (std::ceil (textRunWidth (gain, Theme::fontXS, true, true))) + 2), Theme::fontXS, p.text,
                      juce::Justification::centredLeft, true, true);
            r = drawLabel (g, r.withTrimmedLeft (Theme::space3), ja ("クリップ"));
            clipStatus (r, Theme::fontXS);
        }
        else
        {
            drawHeading (g, headArea, ja ("出力"), false, juce::String (level.value) + " dB");
            auto r = infoArea;
            auto left = r.removeFromLeft (r.getWidth() * 3 / 5);
            drawText (g, ja ("出力ゲイン（設定で変更）"), left.removeFromTop (16), Theme::fontXS, p.textSub);
            drawText (g, gain, left, Theme::fontM, p.text, juce::Justification::centredLeft, true, true);
            drawText (g, ja ("クリップ"), r.removeFromTop (16), Theme::fontXS, p.textSub);
            clipStatus (r, Theme::fontS);
            name = drawLabel (g, name, ja ("出力先")).withTrimmedLeft (Theme::space1);
        }
        drawIcon (g, cable ? Icon::check : Icon::warning, name.removeFromLeft (16).withSizeKeepingCentre (14, 14).toFloat(), cable ? p.ok : p.warn, 2.6f);
        name.removeFromLeft (Theme::space1 + 2);
        drawText (g, unset ? ja ("未選択") : deviceName, name, compact ? Theme::fontXS : Theme::fontS, cable ? p.text : p.warn, juce::Justification::centredLeft, true);
    }

private:
    AppController& c;
    Navigator& nav;
    SegmentMeter meter;
    TextLink deviceLink;
    LevelText level;
    bool compact = false, clipping = false, limiter = false;
    juce::String deviceName;
    float gainDb = 0.0f;
    juce::Rectangle<int> headArea, labelArea, nameArea, valueArea, infoArea;
};

// =============================================================================================== 声の変換
/** "ピッチ [------o--|------] -9 st": a flat slider with the value large at the right. Carries the tour ID. */
class ValueRow : public juce::Component
{
public:
    ValueRow (const juce::String& id, const juce::String& name, koe::Range range) : label (name)
    {
        setComponentID (id);
        slider.setRange (range.min, range.max, kPitchStep);
        slider.setDoubleClickReturnValue (true, range.def);
        slider.setTitle (name);
        slider.onValueChange = [this]
        {
            updateTooltip();
            repaint();
            if (onChange) onChange (slider.getValue());
        };
        addAndMakeVisible (slider);
        updateTooltip();
    }

    void setCompact (bool cp) { compact = cp; resized(); repaint(); }
    void setValue (double v) { slider.setValue (v, juce::dontSendNotification); updateTooltip(); repaint(); }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedLeft (labelW()).withTrimmedRight (valueW() + Theme::space2)); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto r = getLocalBounds();
        drawText (g, label, r.removeFromLeft (labelW()), compact ? Theme::fontXS : Theme::fontS, p.text, juce::Justification::centredLeft, true);
        drawText (g, stText (slider.getValue()), r.removeFromRight (valueW()), compact ? Theme::fontM : Theme::fontL, p.text, juce::Justification::centredRight, true, true);
    }

    PaperSlider slider;
    std::function<void (double)> onChange;

private:
    int labelW() const { return compact ? 76 : 96; }
    int valueW() const { return compact ? 68 : 88; }
    void updateTooltip()
    {
        slider.setTooltip (label + ja ("：") + stText (slider.getValue()) + ja ("（") + stText (slider.getMinimum()) + ja ("〜") + stText (slider.getMaximum())
                           + ja ("、初期値 0 st）ダブルクリックで 0 に戻る"));
    }
    juce::String label;
    bool compact = false;
};

class ShifterBlock : public juce::Component
{
public:
    explicit ShifterBlock (AppController& ctl)
        : c (ctl), pitch ("tour.pitch", ja ("ピッチ"), kPitchSt), formant ("tour.formant", ja ("フォルマント"), kFormantSt)
    {
        setComponentID ("voice.shifter");
        pitch.onChange = [this] (double v) { c.setPitch (float (v)); };
        formant.onChange = [this] (double v) { c.setFormant (float (v)); };
        addAndMakeVisible (pitch);
        addAndMakeVisible (formant);
        toggle.setComponentID ("voice.shifterToggle");
        toggle.setTitle (ja ("変換 ON/OFF"));
        toggle.setTooltip (ja ("声の変換の ON/OFF（OFF の間は重ねる声も鳴りません）"));
        toggle.onClick = [this] { c.setShifterEnabled (toggle.getToggleState()); };
        addAndMakeVisible (toggle);
    }

    void setLayout (bool cp, const Metrics& mt)
    {
        compact = cp;
        m = mt;
        pitch.setCompact (cp);
        formant.setCompact (cp);
        resized();
        repaint();
    }

    void refresh()
    {
        pitch.setValue (c.getPitch());
        formant.setValue (c.getFormant());
        toggle.setToggleState (c.hasShifter(), juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto head = r.removeFromTop (m.head);
        toggle.setBounds (head.removeFromRight (ToggleSwitch::preferredWidth() - (compact ? Theme::space2 : 0)));
        head.removeFromRight (Theme::space2);
        toggleLabel = head.removeFromRight (textWidth (Theme::ui (Theme::fontXS, true), ja ("変換")) + Theme::space1);
        headArea = head;
        r.removeFromTop (Theme::space1);
        pitch.setBounds (r.removeFromTop (m.row));
        r.removeFromTop (m.rowGap);
        formant.setBounds (r.removeFromTop (m.row));
    }

    void paint (juce::Graphics& g) override
    {
        drawHeading (g, headArea, ja ("声の変換"), compact);
        drawText (g, ja ("変換"), toggleLabel, Theme::fontXS, P().text, juce::Justification::centredRight, true);
    }

private:
    AppController& c;
    ValueRow pitch, formant;
    ToggleSwitch toggle;
    bool compact = false;
    Metrics m = metrics (false, true);
    juce::Rectangle<int> headArea, toggleLabel;
};

// =============================================================================================== 重ねる声
juce::String layerSummary (const LayerDef& d)
{
    return (d.mode == LayerDef::Mode::scale ? degreeText (d.degree) : stText (d.pitchSt)) + "   " + dbText (d.levelDb);
}

/** One voice's editor as an overlay (the row only shows the summary). */
class LayerPanel : public PanelBase, private juce::ChangeListener
{
public:
    LayerPanel (AppController& ctl, Navigator& n, int i)
        : PanelBase (n, ja ("声 ") + juce::String (i + 1)), c (ctl), index (i), remove (ja ("この声を削除"), Icon::close)
    {
        scale = c.getLayer (index).mode == LayerDef::Mode::scale;
        subtitle = scale ? ja ("重ねる声（スケール連動）") : ja ("重ねる声");
        toggle.setComponentID ("voice.layerPanel.toggle");
        toggle.setTitle (ja ("声 ") + juce::String (index + 1) + " ON/OFF");
        toggle.onClick = [this] { c.setLayerEnabled (index, toggle.getToggleState()); };
        addAndMakeVisible (toggle);
        remove.setComponentID ("voice.layerPanel.remove");
        remove.onClick = [this] { c.removeLayer (index); };
        addAndMakeVisible (remove);
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
            addAndMakeVisible (row);
        }
        update();
        c.addChangeListener (this);
        setSize (Theme::space5 * 15, headerHeight + Theme::space4 + Theme::space2 + 3 * (SliderRow::stackedHeight + Theme::space2) + Theme::space4);
    }
    ~LayerPanel() override { c.removeChangeListener (this); }

    void resized() override
    {
        PanelBase::resized();
        auto r = contentArea().reduced (Theme::space4, 0);
        auto top = r.removeFromTop (Theme::space4);
        remove.setBounds (top.removeFromRight (Theme::space4));
        top.removeFromRight (Theme::space2);
        toggle.setBounds (top.removeFromRight (ToggleSwitch::preferredWidth()));
        r.removeFromTop (Theme::space2);
        for (auto* row : rows)
        {
            row->setBounds (r.removeFromTop (SliderRow::stackedHeight));
            r.removeFromTop (Theme::space2);
        }
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

/** "声 1   -12 st   -10 dB   [ON]": click or Enter opens LayerPanel. */
class LayerRow : public juce::Component, public juce::SettableTooltipClient
{
public:
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
        scale = d.mode == LayerDef::Mode::scale;
        toggle.setToggleState (d.enabled, juce::dontSendNotification);
        repaint();
    }

    void resized() override { toggle.setBounds (getLocalBounds().removeFromRight (ToggleSwitch::preferredWidth() - Theme::space2)); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        if (isMouseOver (true)) { g.setColour (p.raised); g.fillRect (getLocalBounds()); }
        auto r = getLocalBounds().withTrimmedRight (ToggleSwitch::preferredWidth());
        const auto name = ja ("声 ") + juce::String (index + 1) + (scale ? ja (" 連動") : juce::String());
        drawText (g, name, r.removeFromLeft (Theme::space5 * 2), Theme::fontXS, p.text, juce::Justification::centredLeft, true);
        drawText (g, summary, r, Theme::fontXS, toggle.getToggleState() ? p.text : p.textSub, juce::Justification::centredLeft, false, true);
        g.setColour (p.divider);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
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
    bool scale = false;
    ToggleSwitch toggle;
    juce::String summary;
};

class LayersBlock : public juce::Component
{
public:
    LayersBlock (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("声を追加"), Theme::fontXS, Icon::plus), resume (ja ("再開"), Theme::fontXS)
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

    void setLayout (const Metrics& mt)
    {
        m = mt;
        resized();
        repaint();
    }

    void refresh()
    {
        const int n = c.getNumLayers();
        if (rows.size() != n)
        {
            rows.clear();
            for (int i = 0; i < n; ++i) addAndMakeVisible (rows.add (new LayerRow (c, nav, i)));
        }
        for (int i = 0; i < n; ++i) rows[i]->update (c.getLayer (i));
        add.setVisible (n < kMaxLayers);
        autoStopped = c.areLayersAutoStopped();
        resume.setVisible (autoStopped);
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto head = r.removeFromTop (m.layerHead);
        if (add.isVisible()) add.setBounds (head.removeFromRight (add.preferredWidth()));
        head.removeFromRight (Theme::space3);
        if (resume.isVisible()) resume.setBounds (head.removeFromRight (resume.preferredWidth()));
        for (auto* row : rows)
        {
            r.removeFromTop (2);
            row->setBounds (r.removeFromTop (m.layerRow));
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto head = getLocalBounds().removeFromTop (m.layerHead);
        const auto title = ja ("重ねる声（") + juce::String (c.getNumLayers()) + " / " + juce::String (kMaxLayers) + ja ("）");
        drawText (g, title, head.removeFromLeft (textWidth (Theme::ui (Theme::fontXS, true), title) + 2), Theme::fontXS, p.text, juce::Justification::centredLeft, true);
        if (autoStopped) drawStatus (g, head.withTrimmedLeft (Theme::space2), Icon::warning, ja ("自動停止"), p.warn, Theme::fontXS);
        if (c.getNumLayers() == 0)
            drawText (g, ja ("重ねる声はありません"), getLocalBounds().withTrimmedTop (m.layerHead).removeFromTop (Theme::space3 + 4), Theme::fontXS, p.textSub);
    }

private:
    AppController& c;
    Navigator& nav;
    bool autoStopped = false;
    Metrics m = metrics (false, true);
    juce::OwnedArray<LayerRow> rows;
    TextLink add, resume;
};

// =============================================================================================== the page
class PaperVoicePage : public VoicePage
{
public:
    PaperVoicePage (AppController& ctl, Navigator& n)
        : c (ctl), path (ctl), presets (ctl, n), input (ctl), output (ctl, n), shifter (ctl), layers (ctl, n), chain (ctl, n), bottom (ctl, n, true)
    {
        setComponentID ("voice.view");
        for (auto* comp : std::initializer_list<juce::Component*> { &path, &presets, &input, &shifter, &layers, &output, &chain, &bottom })
            addAndMakeVisible (comp);
        bottom.setComponentID ("voice.bottom");
        applyCompact();
        refresh();
    }

    void setCompact (bool cp) override
    {
        if (compact == cp) return;
        compact = cp;
        applyCompact();
        resized();
        repaint();
    }
    bool isCompact() const override { return compact; }

    void refresh() override
    {
        path.refresh();
        presets.refresh();
        input.refresh();
        output.refresh();
        shifter.refresh();
        layers.refresh();
        chain.refresh();
        bottom.refresh();
        if (c.getNumLayers() != layerCount) resized(); // the column's height depends on the number of voices
    }

    void tick() override
    {
        const auto m = c.pollMeters();
        const auto s = c.getStatus();
        path.tick (m, s);
        input.tick (m, s);
        output.tick (m, s);
        bottom.tick (s);
        if (++ticks % 5 == 0) chain.refresh(); // auto-stop labels
    }

    void resized() override
    {
        const bool cp = compact;
        const int gap = cp ? Theme::space2 : Theme::space3 - 4;
        const int n = layerCount = c.getNumLayers();
        auto r = getLocalBounds();
        bottom.setBounds (r.removeFromBottom (BottomBar::height (cp)));
        r.removeFromBottom (gap);
        bottomRuleY = r.getBottom() + gap / 2;

        // the columns need at least their tight rows; the strip (information only) gives way first when they do not fit
        const auto tight = metrics (cp, false), roomy = metrics (cp, true);
        const int ioMin = cp ? InputColumn::needed (true) + Theme::space2 + OutputColumn::needed (true)
                             : juce::jmax (InputColumn::needed (false), OutputColumn::needed (false));
        const int colsMin = juce::jmax (tight.columnH (n), ioMin);
        const int fixed = PresetRow::height (cp) + gap * 2 + PaperChain::minHeight (cp);
        const bool showPath = r.getHeight() - fixed - PathStrip::height (cp) - gap >= colsMin;
        path.setVisible (showPath);
        if (showPath)
        {
            path.setBounds (r.removeFromTop (PathStrip::height (cp)));
            r.removeFromTop (gap);
        }
        presets.setBounds (r.removeFromTop (PresetRow::height (cp)));
        r.removeFromTop (gap);
        // the chain keeps both parameter bars when it can; the columns take what they need, spare height stays below the chain
        const int avail = r.getHeight() - gap;
        const int chainH = juce::jlimit (PaperChain::minHeight (cp), PaperChain::idealHeight (cp), avail - colsMin);
        const int colsH = juce::jmin (avail - chainH, juce::jmax (roomy.columnH (n) + (cp ? 0 : Theme::space4), colsMin));
        auto cols = r.removeFromTop (colsH);
        r.removeFromTop (gap);
        chain.setBounds (r.withHeight (juce::jmax (chainH, juce::jmin (r.getHeight(), PaperChain::idealHeight (cp)))));

        const auto mt = cols.getHeight() >= roomy.columnH (n) ? roomy : tight;
        shifter.setLayout (cp, mt);
        layers.setLayout (mt);
        rules.clear();
        const int gutter = cp ? Theme::space4 : Theme::space5;
        if (cp)
        {
            auto left = cols.removeFromLeft ((cols.getWidth() - gutter) * 52 / 100);
            rules.push_back ({ left.getRight() + gutter / 2, cols.getY(), 1, cols.getHeight() });
            cols.removeFromLeft (gutter);
            placeShifter (left, mt, n);
            const int ioH = InputColumn::needed (true) + OutputColumn::needed (true);
            const int spare = juce::jmax (Theme::space2, juce::jmin (Theme::space4, cols.getHeight() - ioH));
            input.setBounds (cols.removeFromTop (InputColumn::needed (true)));
            cols.removeFromTop (spare);
            output.setBounds (cols.removeFromTop (OutputColumn::needed (true)));
        }
        else
        {
            const int side = juce::jlimit (240, 300, cols.getWidth() * 27 / 100);
            auto in = cols.removeFromLeft (side);
            rules.push_back ({ in.getRight() + gutter / 2, cols.getY(), 1, cols.getHeight() });
            cols.removeFromLeft (gutter);
            auto out = cols.removeFromRight (side);
            rules.push_back ({ out.getX() - gutter / 2, cols.getY(), 1, cols.getHeight() });
            cols.removeFromRight (gutter);
            input.setBounds (in.withHeight (juce::jmin (in.getHeight(), InputColumn::needed (false))));
            output.setBounds (out.withHeight (juce::jmin (out.getHeight(), OutputColumn::needed (false))));
            placeShifter (cols, mt, n);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (P().divider);
        for (auto& rule : rules) g.fillRect (rule);
        g.fillRect (0, bottomRuleY, getWidth(), 1);
    }

private:
    void applyCompact()
    {
        path.setCompact (compact);
        presets.setCompact (compact);
        input.setCompact (compact);
        output.setCompact (compact);
        chain.setCompact (compact);
        bottom.setCompact (compact);
    }

    void placeShifter (juce::Rectangle<int> col, const Metrics& mt, int n)
    {
        shifter.setBounds (col.removeFromTop (mt.shifterH()));
        rules.push_back ({ col.getX(), col.getY() + mt.rule / 2, col.getWidth(), 1 });
        col.removeFromTop (mt.rule);
        layers.setBounds (col.withHeight (juce::jmin (col.getHeight(), mt.layersH (n))));
    }

    AppController& c;
    bool compact = false;
    PathStrip path;
    PresetRow presets;
    InputColumn input;
    OutputColumn output;
    ShifterBlock shifter;
    LayersBlock layers;
    PaperChain chain;
    BottomBar bottom; // voice.bottom
    std::vector<juce::Rectangle<int>> rules;
    int bottomRuleY = 0, ticks = 0, layerCount = -1;
};
} // namespace

std::unique_ptr<VoicePage> makePaperVoicePage (AppController& c, Navigator& nav) { return std::make_unique<PaperVoicePage> (c, nav); }
} // namespace koe::ui::mainui

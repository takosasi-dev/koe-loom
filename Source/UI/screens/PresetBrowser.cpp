// S-06 プリセット一覧 (docs/mockups/A-S06.dc.html), shown as an overlay: categories with counts,
// name search, favourites (max 9), detail panel, CRUD (F-05-2) and import / export (F-05-4).
// The model has no description text, so rows and the detail panel show a summary built from the
// preset itself (category, pitch / formant, chain).
#include "UI/Screens.h"
#include "UI/screens/Common.h"

#include "Core/Constants.h"
#include "Effects/EffectRegistry.h"

#include <iterator>

namespace koe::ui
{
using namespace screens;

namespace
{
struct Category
{
    const char* id;
    const char* name;
};
constexpr Category kCategories[] = { { "all", "すべて" },     { "favorites", "お気に入り" }, { "natural", "自然" },      { "character", "キャラ" },
                                     { "device", "機器・メディア" }, { "space", "空間" },           { "layered", "重ね・揺れ" }, { "user", "ユーザー" } };
constexpr int kNumCategories = int (std::size (kCategories));

constexpr int kRowH = Theme::space5 + Theme::space4;                 // mock 60
constexpr int kDetailW = Theme::space5 * 9 + Theme::space4;          // mock 312
constexpr int kDetailWNarrow = Theme::space5 * 7 + Theme::space3;    // 240
constexpr int kColumnGap = Theme::space3 + Theme::space1;            // mock 20
constexpr int kLineH = Theme::space4;
constexpr int kChainLineH = Theme::space4 + Theme::space1;           // mock 30

juce::String categoryName (const juce::String& id)
{
    for (auto& c : kCategories)
        if (id == c.id) return ja (c.name);
    return id;
}

juce::String stText (float v)
{
    const bool whole = std::abs (v - std::round (v)) < 0.05f;
    const auto t = whole ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
    return (v > 0.04f ? "+" : "") + t;
}

juce::String effectName (const std::string& type)
{
    const auto* i = findEffectInfo (type);
    return i != nullptr ? ja (i->nameJa) : juce::String (type);
}

juce::String pitchText (const Preset& p) { return p.hasShifter ? stText (p.pitchSt) + " / " + stText (p.formantSt) + " st" : ja ("変換なし"); }

juce::String layerText (const LayerDef& l, int i)
{
    const char* keys[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    auto t = ja ("声 ") + juce::String (i + 1) + ": ";
    if (l.mode == LayerDef::Mode::fixed) t << stText (l.pitchSt) << " / " << stText (l.formantSt) << " st";
    else t << ja ("スケール ") << (l.degree > 0 ? "+" : "") << l.degree << ja (" 度（") << keys[juce::jlimit (0, 11, l.key)] << (l.minor ? ja (" マイナー）") : ja (" メジャー）"));
    t << ja ("、") << stText (l.levelDb) << " dB";
    if (! l.enabled) t << ja ("（OFF）");
    return t;
}

juce::String chainText (const Preset& p)
{
    if (p.chain.empty()) return ja ("エフェクトなし");
    juce::StringArray names;
    for (auto& s : p.chain) names.add (effectName (s.type));
    return names.joinIntoString (ja ("、"));
}

juce::String summaryText (const Preset& p)
{
    return categoryName (p.category()) + ja (" ・ ") + pitchText (p) + ja (" ・ ") + chainText (p);
}

int layerCount (const Preset& p) { return p.hasShifter ? int (p.layers.size()) : 0; }

// =============================================================================================== small parts
/** Search box with a magnifier icon and a placeholder (mock "プリセットを探す"). */
class SearchField : public juce::TextEditor
{
public:
    SearchField()
    {
        setFont (Theme::ui (Theme::fontS));
        setTextToShowWhenEmpty (ja ("プリセットを探す"), Theme::colours().textSub);
        setIndents (Theme::space5 + Theme::space1, (Theme::controlH - int (Theme::fontS) - Theme::space1) / 2);
        setTitle (ja ("プリセットを名前で探す"));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        juce::TextEditor::paintOverChildren (g);
        const float s = float (m::iconS) * 0.9f;
        drawIcon (g, Icon::search, juce::Rectangle<float> (float (Theme::space2 + Theme::space1), (float (getHeight()) - s) * 0.5f, s, s), Theme::colours().textSub);
    }
};

class StarButton : public juce::Button
{
public:
    StarButton() : juce::Button (ja ("お気に入り")) { setWantsKeyboardFocus (true); }

    void setOn (bool on)
    {
        favorite = on;
        setTitle (on ? ja ("お気に入りから外す") : ja ("お気に入りに入れる"));
        setTooltip (getTitle());
        repaint();
    }

    bool isOn() const noexcept { return favorite; }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = Theme::colours();
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (highlighted || down) { g.setColour (p.raised); g.fillEllipse (r); }
        const float s = r.getWidth() * 0.56f;
        drawIcon (g, favorite ? Icon::starFilled : Icon::star, r.withSizeKeepingCentre (s, s), favorite ? p.accent : p.textSub);
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, r.getHeight() * 0.5f);
    }

private:
    bool favorite = false;
};

// =============================================================================================== PresetRow
class PresetRow : public juce::Component
{
public:
    explicit PresetRow (const Preset& p) : preset (p)
    {
        setWantsKeyboardFocus (true);
        setComponentID ("presets.row." + juce::String (p.id));
        setTitle (p.name);
        star.setComponentID ("presets.star." + juce::String (p.id));
        addAndMakeVisible (star);
        star.onClick = [this] { if (onStar) onStar(); };
    }

    Preset preset;
    StarButton star;
    std::function<void()> onSelect, onUse, onStar;
    std::function<void (int)> onMove;

    void setState (bool sel, bool cur, bool fav)
    {
        selected = sel;
        current = cur;
        star.setOn (fav);
        repaint();
    }

    bool isSelected() const noexcept { return selected; }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space3 - Theme::space1, 0);
        star.setBounds (r.removeFromLeft (Theme::touchMin).withSizeKeepingCentre (Theme::touchMin, Theme::touchMin));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        if (selected)
        {
            g.setColour (p.raised);
            g.fillRoundedRectangle (b, Theme::radiusM);
            g.setColour (p.accent);
            g.drawRoundedRectangle (b.reduced (0.75f), Theme::radiusM, Theme::borderWidth);
        }
        else if (isMouseOver (true))
        {
            g.setColour (p.raised.withAlpha (0.5f));
            g.fillRoundedRectangle (b, Theme::radiusM);
        }
        auto r = getLocalBounds().reduced (Theme::space3 - Theme::space1, 0);
        r.removeFromLeft (Theme::touchMin + Theme::space2 + Theme::space1 / 2);

        // right side: "使用中", "重ね n 声", "n スロット"
        const auto slots = juce::String (int (preset.chain.size())) + ja (" スロット");
        const int slotsW = int (std::ceil (textRunWidth (slots, Theme::fontXS, false, true))) + 2;
        drawText (g, slots, r.removeFromRight (slotsW), Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
        auto badge = [&] (const juce::String& t, Tone tone)
        {
            const int w = badgeWidth (t, false);
            if (w > r.getWidth() / 3) return;
            r.removeFromRight (Theme::space2);
            paintBadge (g, r.removeFromRight (w).withSizeKeepingCentre (w, m::badgeH - Theme::space1), t, tone, false);
        };
        if (layerCount (preset) > 0) badge (ja ("重ね ") + juce::String (layerCount (preset)) + ja (" 声"), Tone::text);
        if (current) badge (ja ("使用中"), Tone::accent);
        r.removeFromRight (Theme::space2);

        auto text = r.withSizeKeepingCentre (r.getWidth(), kLineH - Theme::space1 + Theme::space3);
        auto nameLine = text.removeFromTop (kLineH - Theme::space1);
        if (current)
        {
            drawIcon (g, Icon::check, nameLine.removeFromLeft (Theme::space3).toFloat().withSizeKeepingCentre (float (Theme::space3), float (Theme::space3)), p.accent, 2.6f);
            nameLine.removeFromLeft (Theme::space1);
        }
        drawText (g, preset.name, nameLine, Theme::fontS, p.text, juce::Justification::centredLeft, true);
        drawText (g, summaryText (preset), text, Theme::fontXS, p.textSub, juce::Justification::centredLeft);
        if (hasKeyboardFocus (false)) drawFocusRing (g, b.reduced (Theme::focusRingWidth + 1.0f), Theme::radiusM);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { if (onSelect) onSelect(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onUse) onUse(); }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey) { if (onUse) onUse(); return true; }
        if (k == juce::KeyPress::spaceKey) { if (onSelect) onSelect(); return true; }
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            if (onMove) onMove (k == juce::KeyPress::upKey ? -1 : 1);
            return true;
        }
        return false;
    }

private:
    bool selected = false, current = false;
};

class RowList : public juce::Component
{
public:
    juce::OwnedArray<PresetRow> rows;

    int layoutRows (int width)
    {
        int y = 0;
        for (auto* r : rows)
            if (r->isVisible())
            {
                r->setBounds (0, y, width, kRowH);
                y += kRowH + Theme::space1 / 2;
            }
        return y;
    }
};

// =============================================================================================== DetailPanel
/** Right column: the selected preset's settings and its actions. */
class DetailPanel : public juce::Component
{
public:
    DetailPanel()
        : use (ja ("このプリセットを使う"), PillButton::Style::primary), duplicate (ja ("複製"), PillButton::Style::outline),
          rename (ja ("名前変更"), PillButton::Style::outline), remove (ja ("削除"), PillButton::Style::outline),
          overwrite (ja ("上書き保存"), PillButton::Style::outline), exportButton (ja ("エクスポート"), PillButton::Style::outline),
          note ({}, Theme::fontXS, Tone::sub)
    {
        use.setPill (true);
        use.setFontSize (Theme::fontM);
        use.setComponentID ("presets.use");
        duplicate.setComponentID ("presets.duplicate");
        rename.setComponentID ("presets.rename");
        remove.setComponentID ("presets.delete");
        overwrite.setComponentID ("presets.overwrite");
        exportButton.setComponentID ("presets.export");
        note.setComponentID ("presets.note");
        overwrite.setTooltip (ja ("いま使っているユーザープリセットに、いまの調整を保存します"));
        exportButton.setTooltip (ja ("このプリセットを JSON ファイルに書き出します"));
        addAndMakeVisible (use);
        for (auto* b : { &duplicate, &rename, &remove, &overwrite, &exportButton })
        {
            b->setFontSize (Theme::fontXS);
            actions.add (*b, b->preferredWidth(), Theme::buttonH);
        }
        addAndMakeVisible (actions);
        addAndMakeVisible (note);
    }

    void show (const Preset* p, bool isCurrent)
    {
        preset = p != nullptr ? std::optional<Preset> (*p) : std::nullopt;
        const bool has = p != nullptr, user = has && ! p->builtin;
        for (auto* b : { &use, &duplicate, &rename, &remove, &overwrite, &exportButton }) b->setVisible (has);
        note.setVisible (has);
        use.setEnabled (has);
        duplicate.setEnabled (has);
        exportButton.setEnabled (has);
        rename.setEnabled (user);
        remove.setEnabled (user);
        overwrite.setEnabled (user && isCurrent);
        if (! has) note.setText ({});
        else if (! user) note.setText (ja ("内蔵プリセットは、名前変更・削除・上書きができません。複製すると編集できます。"));
        else if (! isCurrent) note.setText (ja ("上書き保存は、いま使っているプリセットだけにできます。"));
        else note.setText (ja ("上書き保存で、いまの調整をこのプリセットに保存します。"));
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space4);
        const int w = r.getWidth();
        if (note.isVisible() && note.getText().isNotEmpty())
        {
            note.setBounds (r.removeFromBottom (note.heightForWidth (w)));
            r.removeFromBottom (Theme::space2);
        }
        actions.setBounds (r.removeFromBottom (actions.heightForWidth (w)));
        r.removeFromBottom (Theme::space2);
        use.setBounds (r.removeFromBottom (Theme::pillH));
        r.removeFromBottom (Theme::space3);
        textArea = r;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        g.setColour (p.surface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);
        auto r = textArea;
        if (! preset)
        {
            drawText (g, ja ("プリセットを選んでください"), r, Theme::fontS, p.textSub, juce::Justification::centred);
            return;
        }
        const auto& pr = *preset;
        drawText (g, pr.name, r.removeFromTop (Theme::space5), Theme::fontL, p.text, juce::Justification::centredLeft, true);
        drawText (g, categoryName (pr.category()) + (pr.builtin ? ja (" ・ 内蔵プリセット") : ja (" ・ ユーザープリセット")), r.removeFromTop (kLineH - Theme::space1),
                  Theme::fontXS, p.textSub, juce::Justification::centredLeft);
        auto divider = [&]
        {
            r.removeFromTop (Theme::space3 - Theme::space1);
            g.setColour (p.divider);
            g.fillRect (r.removeFromTop (1));
            r.removeFromTop (Theme::space3 - Theme::space1);
        };
        divider();
        auto kv = [&] (const juce::String& k, const juce::String& v)
        {
            auto line = r.removeFromTop (kLineH);
            const int kw = int (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontXS), k)) + Theme::space2;
            drawText (g, k, line.removeFromLeft (kw), Theme::fontXS, p.textSub, juce::Justification::centredLeft);
            drawText (g, v, line, Theme::fontS, p.text, juce::Justification::centredRight, true, true);
        };
        kv (ja ("ピッチ / フォルマント"), pitchText (pr));
        if (layerCount (pr) == 0) kv (ja ("重ねる声"), ja ("なし"));
        else
        {
            drawText (g, ja ("重ねる声"), r.removeFromTop (kLineH), Theme::fontXS, p.textSub, juce::Justification::centredLeft);
            for (int i = 0; i < layerCount (pr); ++i)
                drawText (g, layerText (pr.layers[size_t (i)], i), r.removeFromTop (kLineH - Theme::space1), Theme::fontXS, p.text,
                          juce::Justification::centredLeft, true);
        }
        divider();
        drawText (g, ja ("エフェクトチェーン（左から順に処理）"), r.removeFromTop (kLineH), Theme::fontXS, p.textSub, juce::Justification::centredLeft);
        if (pr.chain.empty())
        {
            drawText (g, ja ("エフェクトなし"), r.removeFromTop (kChainLineH), Theme::fontS, p.textSub, juce::Justification::centredLeft);
            return;
        }
        const int fits = juce::jmax (1, r.getHeight() / kChainLineH);
        const int n = int (pr.chain.size());
        const int shown = n <= fits ? n : fits - 1;
        for (int i = 0; i < shown; ++i)
        {
            auto line = r.removeFromTop (kChainLineH);
            drawText (g, juce::String (i + 1), line.removeFromLeft (Theme::space4 - Theme::space1), Theme::fontXS, p.textSub, juce::Justification::centredLeft,
                      false, true);
            line.removeFromLeft (Theme::space2);
            const auto& s = pr.chain[size_t (i)];
            drawText (g, effectName (s.type) + (s.enabled ? juce::String() : ja ("（OFF）")), line, Theme::fontS, s.enabled ? p.text : p.textSub,
                      juce::Justification::centredLeft);
        }
        if (shown < n)
            drawText (g, ja ("ほか ") + juce::String (n - shown) + ja (" 個"), r.removeFromTop (kChainLineH), Theme::fontXS, p.textSub, juce::Justification::centredLeft);
    }

    std::optional<Preset> preset;
    PillButton use, duplicate, rename, remove, overwrite, exportButton;
    FlowBox actions { Theme::space2 };
    TextLabel note;

private:
    juce::Rectangle<int> textArea;
};
} // namespace

// =============================================================================================== Impl
struct PresetBrowser::Impl final : juce::ChangeListener, juce::KeyListener
{
    Impl (PresetBrowser& o, AppController& ctl, Navigator& n)
        : owner (o), c (ctl), nav (n), title (ja ("プリセット一覧"), Theme::fontL, Tone::text, true), favCount ({}, Theme::fontXS, Tone::sub),
          listTitle ({}, Theme::fontM, Tone::text, true), listCount ({}, Theme::fontXS, Tone::sub), message ({}, Theme::fontXS, Tone::text, true),
          saveNew (ja ("新しく保存"), PillButton::Style::outline, Icon::plus), importButton (ja ("インポート"), PillButton::Style::outline, Icon::folder),
          close (ja ("閉じる"), Icon::close)
    {
        owner.setWantsKeyboardFocus (true);
        owner.addKeyListener (this);
        for (auto* l : { &title, &favCount, &listTitle, &listCount })
        {
            l->setJustification (juce::Justification::centredLeft);
            l->setMaxLines (1);
        }
        listCount.setJustification (juce::Justification::centredRight);
        favCount.setJustification (juce::Justification::centredRight);
        favCount.setComponentID ("presets.favCount");
        listTitle.setComponentID ("presets.listTitle");
        listCount.setComponentID ("presets.listCount");
        message.setComponentID ("presets.message");
        saveNew.setComponentID ("presets.saveNew");
        saveNew.setTooltip (ja ("いまの声（調整を含む）を、新しいユーザープリセットとして保存します"));
        importButton.setComponentID ("presets.import");
        importButton.setTooltip (ja ("JSON ファイルのプリセットを読み込みます（64 KB まで）"));
        close.setComponentID ("presets.close");
        search.setComponentID ("presets.search");
        for (auto* comp : std::initializer_list<juce::Component*> { &title, &favCount, &saveNew, &importButton, &close, &search, &listTitle, &listCount, &viewport, &detail })
            owner.addAndMakeVisible (comp);
        owner.addChildComponent (message);
        for (int i = 0; i < kNumCategories; ++i)
        {
            auto* b = categoryButtons.add (new NavButton (ja (kCategories[i].name)));
            b->setComponentID (juce::String ("presets.cat.") + kCategories[i].id);
            b->setRadioGroupId (2);
            b->onClick = [this, i] { setCategory (i); };
            owner.addAndMakeVisible (b);
        }
        viewport.setScrollBarsShown (true, false);
        viewport.setViewedComponent (&list, false);

        close.onClick = [this] { closeBrowser(); };
        saveNew.onClick = [this] { askSaveNew(); };
        importButton.onClick = [this] { importPreset(); };
        search.onTextChange = [this] { applyFilter(); };
        search.onEscapeKey = [this]
        {
            if (search.getText().isNotEmpty()) { search.setText ({}, false); applyFilter(); }
            else closeBrowser();
        };
        detail.use.onClick = [this] { useSelected(); };
        detail.duplicate.onClick = [this] { duplicateSelected(); };
        detail.rename.onClick = [this] { askRename(); };
        detail.remove.onClick = [this] { askDelete(); };
        detail.overwrite.onClick = [this] { overwriteCurrent(); };
        detail.exportButton.onClick = [this] { exportSelected(); };

        selectedId = c.getCurrentPreset().id;
        const auto cur = c.getCurrentPreset().category();
        int start = 0;
        for (int i = 0; i < kNumCategories; ++i)
            if (cur == kCategories[i].id) start = i;
        category = start;
        rebuildRows();
        c.addChangeListener (this);
    }

    ~Impl() override
    {
        c.removeChangeListener (this);
        owner.removeKeyListener (this);
        viewport.setViewedComponent (nullptr, false);
    }

    // ------------------------------------------------------------------------------- list
    PresetLibrary& lib() { return c.getPresetLibrary(); }

    void rebuildRows()
    {
        list.rows.clear();
        for (auto& p : lib().all())
        {
            auto* row = list.rows.add (new PresetRow (p));
            const auto id = p.id;
            row->onSelect = [this, id] { select (id, false); };
            row->onUse = [this, id] { select (id, false); useSelected(); };
            row->onStar = [this, id] { toggleFavorite (id); };
            row->onMove = [this, row] (int d) { moveSelection (row, d); };
            list.addAndMakeVisible (row);
        }
        refreshCounts();
        applyFilter();
    }

    bool inCategory (const Preset& p) const
    {
        const juce::String id (kCategories[category].id);
        if (id == "all") return true;
        if (id == "favorites") return c.isFavorite (p.id);
        return p.category() == id;
    }

    void applyFilter()
    {
        const auto q = search.getText().trim();
        int total = 0, shown = 0;
        for (auto* r : list.rows)
        {
            const bool in = inCategory (r->preset);
            const bool vis = in && (q.isEmpty() || r->preset.name.containsIgnoreCase (q));
            total += in ? 1 : 0;
            shown += vis ? 1 : 0;
            r->setVisible (vis);
        }
        for (int i = 0; i < kNumCategories; ++i) categoryButtons[i]->setToggleState (i == category, juce::dontSendNotification);
        listTitle.setText (ja (kCategories[category].name));
        listCount.setText (juce::String (total) + ja (" 種 ・ 表示 ") + juce::String (shown) + ja (" 件"));
        refreshRowStates();
        layoutList();
    }

    void refreshCounts()
    {
        int counts[kNumCategories] = {};
        for (auto& p : lib().all())
        {
            ++counts[0];
            if (c.isFavorite (p.id)) ++counts[1];
            for (int i = 2; i < kNumCategories; ++i)
                if (p.category() == kCategories[i].id) ++counts[i];
        }
        for (int i = 0; i < kNumCategories; ++i) categoryButtons[i]->setCount (counts[i]);
        favCount.setText (ja ("お気に入りは 9 件まで（") + juce::String (c.getFavorites().size()) + " / " + juce::String (kMaxFavorites) + ja ("）"));
    }

    void refreshRowStates()
    {
        const auto& cur = c.getCurrentPreset().id;
        for (auto* r : list.rows) r->setState (r->preset.id == selectedId, r->preset.id == cur, c.isFavorite (r->preset.id));
        const auto* p = lib().find (selectedId);
        detail.show (p, p != nullptr && p->id == cur);
    }

    void select (const std::string& id, bool scroll)
    {
        selectedId = id;
        refreshRowStates();
        if (scroll)
            for (auto* r : list.rows)
                if (r->preset.id == id && r->isVisible()) scrollTo (*r);
    }

    void moveSelection (PresetRow* from, int delta)
    {
        juce::Array<PresetRow*> vis;
        for (auto* r : list.rows) if (r->isVisible()) vis.add (r);
        if (auto* next = vis[vis.indexOf (from) + delta])
        {
            select (next->preset.id, true);
            next->grabKeyboardFocus();
        }
    }

    void scrollTo (juce::Component& r)
    {
        const int y = viewport.getViewPositionY(), h = viewport.getViewHeight();
        if (r.getY() < y) viewport.setViewPosition (0, r.getY());
        else if (r.getBottom() > y + h) viewport.setViewPosition (0, r.getBottom() - h);
    }

    void setCategory (int i)
    {
        category = juce::jlimit (0, kNumCategories - 1, i);
        viewport.setViewPosition (0, 0);
        applyFilter();
    }

    // ------------------------------------------------------------------------------- actions
    void showMessage (const juce::String& text, Tone tone)
    {
        message.setText (text);
        message.setTone (tone);
        message.setIcon (tone == Tone::ok ? std::optional<Icon> (Icon::check) : (tone == Tone::text ? std::nullopt : std::optional<Icon> (Icon::warning)));
        message.setVisible (text.isNotEmpty());
        layout();
    }

    void libraryChanged (const std::string& select_)
    {
        selectedId = select_;
        rebuildRows();
        select (select_, true);
    }

    void toggleFavorite (const std::string& id)
    {
        juce::String why;
        if (! c.toggleFavorite (id, why)) showMessage (why, Tone::danger); // F-05-8 / AC-21
        else showMessage ({}, Tone::text);
        refreshCounts();
        applyFilter();
    }

    void useSelected()
    {
        if (lib().find (selectedId) == nullptr) return;
        if (c.hasLooperRecording()) // E-27: confirm from the screen
        {
            prompt (ja ("ルーパーの録音が消えます"), ja ("プリセットを切り替えると、ルーパーに録音した声は消えます。切り替えますか？"), false, {}, ja ("切り替える"), true,
                    [this] (const juce::String&) { loadAndClose(); });
            return;
        }
        loadAndClose();
    }

    void loadAndClose()
    {
        c.loadPreset (selectedId);
        refreshRowStates();
        closeBrowser();
    }

    void closeBrowser()
    {
        auto* np = &nav;
        np->closeOverlay();
    }

    void askSaveNew()
    {
        prompt (ja ("新しく保存"), ja ("いまの声（調整を含む）を、ユーザープリセットとして保存します。名前は 32 文字までです。"), true, c.getCurrentPreset().name,
                ja ("保存する"), false, [this] (const juce::String& name)
        {
            juce::String err;
            if (! c.saveCurrentAsNew (name, err)) { showMessage (err, Tone::danger); return; }
            showMessage (ja ("「") + name + ja ("」を保存しました。"), Tone::ok);
            category = indexOfCategory ("user");
            libraryChanged (c.getCurrentPreset().id);
        });
    }

    void duplicateSelected()
    {
        std::string newId;
        juce::String err;
        if (! c.duplicatePreset (selectedId, newId, err)) { showMessage (err, Tone::danger); return; }
        const auto* p = lib().find (newId);
        showMessage (ja ("「") + (p != nullptr ? p->name : juce::String()) + ja ("」を作りました。名前変更や上書きができます。"), Tone::ok);
        category = indexOfCategory ("user");
        libraryChanged (newId);
    }

    void askRename()
    {
        const auto* p = lib().find (selectedId);
        if (p == nullptr || p->builtin) return;
        const auto id = selectedId;
        prompt (ja ("名前変更"), ja ("新しい名前を入れてください（32 文字まで）。"), true, p->name, ja ("変更する"), false, [this, id] (const juce::String& name)
        {
            juce::String err;
            if (! c.renamePreset (id, name, err)) { showMessage (err, Tone::danger); return; }
            showMessage (ja ("名前を「") + name + ja ("」に変えました。"), Tone::ok);
            libraryChanged (id);
        });
    }

    void askDelete()
    {
        const auto* p = lib().find (selectedId);
        if (p == nullptr || p->builtin) return;
        const auto id = selectedId;
        const auto name = p->name;
        prompt (ja ("「") + name + ja ("」を削除しますか"), ja ("削除したプリセットは元に戻せません。"), false, {}, ja ("削除する"), true, [this, id, name] (const juce::String&)
        {
            juce::String err;
            if (! c.removePreset (id, err)) { showMessage (err, Tone::danger); return; }
            showMessage (ja ("「") + name + ja ("」を削除しました。"), Tone::text);
            libraryChanged (c.getCurrentPreset().id);
        });
    }

    void overwriteCurrent()
    {
        juce::String err;
        if (! c.overwriteCurrent (err)) { showMessage (err, Tone::danger); return; }
        showMessage (ja ("「") + c.getCurrentPreset().name + ja ("」に上書き保存しました。"), Tone::ok);
        libraryChanged (c.getCurrentPreset().id);
    }

    void importPreset()
    {
        chooser = std::make_unique<juce::FileChooser> (ja ("読み込むプリセット（JSON）を選ぶ"), lastDir, "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            lastDir = f.getParentDirectory();
            std::string newId;
            PresetLoadReport report;
            if (! c.importPreset (f, newId, report))
            {
                showMessage (ja ("読み込めませんでした: ") + (report.rejectReason.isNotEmpty() ? report.rejectReason : f.getFileName()), Tone::danger);
                return;
            }
            const auto* p = lib().find (newId);
            const auto notices = report.toJapanese(); // E-21..E-24, E-30, E-10
            showMessage (ja ("「") + (p != nullptr ? p->name : f.getFileNameWithoutExtension()) + ja ("」を読み込みました。") + (notices.isNotEmpty() ? "\n" + notices : juce::String()),
                         notices.isNotEmpty() ? Tone::warn : Tone::ok);
            category = indexOfCategory ("user");
            libraryChanged (newId);
        });
    }

    void exportSelected()
    {
        const auto* p = lib().find (selectedId);
        if (p == nullptr) return;
        const auto id = selectedId;
        const auto dir = lastDir.isDirectory() ? lastDir : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        chooser = std::make_unique<juce::FileChooser> (ja ("プリセットの書き出し先"), dir.getChildFile (juce::File::createLegalFileName (p->name) + ".json"), "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, id] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
            lastDir = f.getParentDirectory();
            juce::String err;
            if (! lib().exportFile (id, f, err)) showMessage (err.isNotEmpty() ? err : ja ("書き出せませんでした。"), Tone::danger);
            else showMessage (ja ("「") + f.getFileName() + ja ("」に書き出しました。"), Tone::ok);
        });
    }

    static int indexOfCategory (const char* id)
    {
        for (int i = 0; i < kNumCategories; ++i)
            if (juce::String (kCategories[i].id) == id) return i;
        return 0;
    }

    void prompt (const juce::String& t, const juce::String& msg, bool input, const juce::String& initial, const juce::String& okLabel, bool danger,
                 std::function<void (const juce::String&)> onOk)
    {
        // the prompt is deleted later (we are inside its callback); a newer, visible prompt is kept
        auto finish = [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PresetBrowser> (&owner)]
            {
                if (safe != nullptr && safe->impl->activePrompt != nullptr && ! safe->impl->activePrompt->isVisible()) safe->impl->activePrompt.reset();
            });
        };
        activePrompt = std::make_unique<InlinePrompt> (t, msg, input, initial, okLabel, danger,
                                                       [this, onOk, finish] (const juce::String& text)
                                                       {
                                                           activePrompt->setVisible (false);
                                                           finish();
                                                           if (onOk) onOk (text);
                                                       },
                                                       [this, finish]
                                                       {
                                                           activePrompt->setVisible (false);
                                                           finish();
                                                       });
        activePrompt->setComponentID ("presets.prompt");
        owner.addAndMakeVisible (*activePrompt);
        activePrompt->setBounds (owner.getLocalBounds());
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        refreshCounts();
        applyFilter();
    }

    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k != juce::KeyPress::escapeKey) return false;
        if (activePrompt != nullptr && activePrompt->isVisible()) activePrompt->cancel();
        else closeBrowser();
        return true;
    }

    // ------------------------------------------------------------------------------- layout
    void layoutList()
    {
        const int vw = viewport.getWidth();
        if (vw <= 0) return;
        int h = list.layoutRows (vw);
        if (h > viewport.getHeight()) h = list.layoutRows (vw - viewport.getScrollBarThickness() - Theme::space1);
        list.setSize (h > viewport.getHeight() ? vw - viewport.getScrollBarThickness() - Theme::space1 : vw, h);
    }

    void layout()
    {
        const bool narrow = owner.getWidth() < Theme::narrowWidth - Theme::space5 * 3;
        auto r = owner.getLocalBounds().reduced (Theme::space4).withTrimmedTop (-Theme::space2);
        auto header = r.removeFromTop (Theme::headerH);
        close.setBounds (header.removeFromRight (Theme::controlH).withSizeKeepingCentre (Theme::controlH, Theme::controlH));
        header.removeFromRight (Theme::space3);
        const int iw = importButton.preferredWidth(), sw = saveNew.preferredWidth();
        importButton.setBounds (header.removeFromRight (iw).withSizeKeepingCentre (iw, Theme::buttonH));
        header.removeFromRight (Theme::space2);
        saveNew.setBounds (header.removeFromRight (sw).withSizeKeepingCentre (sw, Theme::buttonH));
        header.removeFromRight (Theme::space3);
        const int tw = title.singleLineWidth();
        title.setBounds (header.removeFromLeft (tw));
        header.removeFromLeft (Theme::space3);
        const int fw = favCount.singleLineWidth();
        favCount.setVisible (fw <= header.getWidth());
        favCount.setBounds (header.removeFromRight (juce::jmin (fw, header.getWidth())));
        if (message.isVisible())
        {
            r.removeFromTop (Theme::space1);
            message.setBounds (r.removeFromTop (message.heightForWidth (r.getWidth())));
        }
        r.removeFromTop (Theme::space3 - Theme::space1);

        auto left = r.removeFromLeft (narrow ? m::navWNarrow : m::navW);
        r.removeFromLeft (narrow ? Theme::space3 : kColumnGap);
        detail.setBounds (r.removeFromRight (narrow ? kDetailWNarrow : kDetailW));
        r.removeFromRight (narrow ? Theme::space3 : kColumnGap);

        search.setBounds (left.removeFromTop (Theme::controlH));
        left.removeFromTop (Theme::space3 - Theme::space1);
        for (auto* b : categoryButtons)
        {
            b->setBounds (left.removeFromTop (Theme::controlH));
            left.removeFromTop (Theme::space1 / 2);
        }

        auto listHeader = r.removeFromTop (Theme::buttonH);
        const int lw = juce::jmin (listCount.singleLineWidth(), listHeader.getWidth() / 2);
        listCount.setBounds (listHeader.removeFromRight (lw));
        listTitle.setBounds (listHeader.withTrimmedLeft (Theme::space1));
        r.removeFromTop (Theme::space1);
        viewport.setBounds (r);
        layoutList();
        if (! scrolledToSelection && viewport.getHeight() > 0)
        {
            scrolledToSelection = true; // open at the preset in use (mock: the current row is selected)
            for (auto* row : list.rows)
                if (row->preset.id == selectedId && row->isVisible()) viewport.setViewPosition (0, juce::jmax (0, row->getY() - viewport.getHeight() / 3));
        }
        if (activePrompt != nullptr) activePrompt->setBounds (owner.getLocalBounds());
    }

    void paint (juce::Graphics& g)
    {
        const auto& p = Theme::colours();
        g.setColour (p.bg);
        g.fillRoundedRectangle (owner.getLocalBounds().toFloat(), Theme::radiusL);
        g.setColour (p.border);
        g.drawRoundedRectangle (owner.getLocalBounds().toFloat().reduced (0.5f), Theme::radiusL, 1.0f);
    }

    PresetBrowser& owner;
    AppController& c;
    Navigator& nav;
    TextLabel title, favCount, listTitle, listCount, message;
    PillButton saveNew, importButton;
    IconButton close;
    SearchField search;
    juce::OwnedArray<NavButton> categoryButtons;
    juce::Viewport viewport;
    RowList list;
    DetailPanel detail;
    std::unique_ptr<InlinePrompt> activePrompt;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastDir;
    std::string selectedId;
    int category = 0;
    bool scrolledToSelection = false;
};

// =============================================================================================== PresetBrowser
PresetBrowser::PresetBrowser (AppController& controller, Navigator& nav)
{
    impl = std::make_unique<Impl> (*this, controller, nav);
    setComponentID ("presets");
    setSize (Theme::defaultWidth - Theme::space5 * 2, Theme::defaultHeight - Theme::space5 * 2);
}

PresetBrowser::~PresetBrowser() = default;
void PresetBrowser::resized() { impl->layout(); }
void PresetBrowser::paint (juce::Graphics& g) { impl->paint (g); }
} // namespace koe::ui

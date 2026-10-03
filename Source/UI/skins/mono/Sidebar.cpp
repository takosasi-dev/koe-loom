// 案 C Mono: the preset column (docs/mockups/C-S01.dc.html, left), owner wave5/mono.
#include "UI/skins/mono/MonoParts.h"

#include <algorithm>

namespace koe::ui::mono
{
namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kChipH = Theme::space4 + Theme::space1;  // 28
constexpr int kRowH = Theme::space5 + Theme::space1;   // 36
constexpr int kCheckW = 14 + Theme::space2;            // the check mark's column (names stay aligned)

struct Filter { const char* id; const char* label; const char* tip; };
const Filter kFilters[] = { { "all", "全部", "すべてのプリセット" },
                            { "fav", "\xe2\x98\x85", "お気に入り（★ を押すと追加・解除、9 個まで）" },
                            { "natural", "自然", "自然" },
                            { "character", "キャラ", "キャラ" },
                            { "device", "機器", "機器・メディア" },
                            { "space", "空間", "空間" },
                            { "layered", "重ね", "重ね・揺れ" },
                            { "user", "ユーザー", "自分で保存したプリセット" } };

juce::String filterTitle (const juce::String& id)
{
    if (id == "all") return ja ("全部");
    if (id == "fav") return ja ("お気に入り");
    return mainui::presetCategoryJa (id);
}
} // namespace

// =============================================================================================== row
/** One preset: check mark + outline when it is the one in use (never the filled look of ボイチェン ON, D-23), star. */
class PresetSidebar::Row : public juce::Button
{
public:
    Row (PresetSidebar& o, const Preset& p) : juce::Button (p.name), id (p.id), owner (o), star (ja ("お気に入り"), Icon::star)
    {
        setComponentID ("mono.preset." + juce::String (id));
        setTitle (p.name);
        setTooltip (p.name + ja ("（") + mainui::presetCategoryJa (p.category()) + ja ("）"));
        onClick = [this] { owner.choose (id); };
        star.setComponentID ("mono.preset." + juce::String (id) + ".star");
        star.onClick = [this]
        {
            juce::String why;
            if (! owner.c.toggleFavorite (id, why)) owner.nav.showToast (why);
        };
        addAndMakeVisible (star);
    }

    void update (bool isCurrent, bool isFav, bool modified)
    {
        current = isCurrent;
        edited = modified;
        star.setIcon (isFav ? Icon::starFilled : Icon::star);
        star.setTooltip (isFav ? ja ("お気に入りから外す") : ja ("お気に入りに入れる"));
        star.setToggleState (isFav, juce::dontSendNotification);
        setToggleState (isCurrent, juce::dontSendNotification);
        repaint();
    }

    void resized() override { star.setBounds (getLocalBounds().removeFromRight (getHeight()).reduced (Theme::space1)); }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = P();
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        if (current || highlighted || down)
        {
            g.setColour (p.raised);
            g.fillRoundedRectangle (b, Theme::radiusS);
        }
        if (current)
        {
            g.setColour (p.accent);
            g.drawRoundedRectangle (b.reduced (0.5f), Theme::radiusS, Theme::borderWidth);
        }
        auto r = getLocalBounds().reduced (Theme::space2, 0).withTrimmedRight (getHeight() - Theme::space1);
        auto check = r.removeFromLeft (kCheckW);
        if (current) drawIcon (g, Icon::check, check.withSizeKeepingCentre (14, 14).toFloat().withX (float (check.getX())), p.accent, 2.8f);
        if (current && edited)
        {
            const auto t = ja ("編集中");
            drawText (g, t, r.removeFromRight (juce::roundToInt (textRunWidth (t, Theme::fontXS, false, false)) + Theme::space1), Theme::fontXS, p.textSub,
                      juce::Justification::centredRight);
        }
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontS, current));
        g.drawFittedText (getButtonText(), r, juce::Justification::centredLeft, 1, 0.85f);
        if (hasKeyboardFocus (false)) drawFocusRing (g, b, Theme::radiusS);
    }

    const std::string id;

private:
    PresetSidebar& owner;
    bool current = false, edited = false;
    IconButton star;
};

// =============================================================================================== sidebar
PresetSidebar::PresetSidebar (AppController& ctl, Navigator& n)
    : c (ctl), nav (n), save (ja ("保存"), PillButton::Style::outline), dup (ja ("複製"), PillButton::Style::outline),
      browse (ja ("一覧"), PillButton::Style::outline)
{
    setComponentID ("mono.presets");
    search.setComponentID ("mono.presetSearch");
    search.setTitle (ja ("プリセットを検索"));
    search.setTextToShowWhenEmpty (ja ("プリセットを検索"), P().textSub);
    search.setFont (Theme::ui (Theme::fontS));
    search.setIndents (Theme::space5 - 2, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { search.setText ({}); };
    addAndMakeVisible (search);

    const auto cur = c.getCurrentPreset().category();
    filter = cur.isNotEmpty() ? cur : juce::String ("all");
    for (auto& f : kFilters)
    {
        auto* chip = chips.add (new ChipButton (ja (f.label), true));
        chip->setComponentID ("mono.filter." + juce::String (f.id));
        chip->setTooltip (ja (f.tip));
        chip->onClick = [this, id = juce::String (f.id)] { setFilter (id); };
        chipIds.add (f.id);
        addAndMakeVisible (chip);
    }

    view.setComponentID ("voice.presetSelector");
    view.setTitle (ja ("プリセット一覧"));
    view.setViewedComponent (&list, false);
    view.setScrollBarsShown (true, false);
    view.setScrollBarThickness (Theme::space2);
    addAndMakeVisible (view);

    save.setComponentID ("mono.presetSave");
    save.setTooltip (ja ("いまの設定を保存する（内蔵プリセットは新しい名前で）"));
    save.onClick = [this] { savePreset(); };
    dup.setComponentID ("mono.presetDuplicate");
    dup.setTooltip (ja ("いまの設定を別の名前で保存する"));
    dup.onClick = [this] { askName (ja ("複製"), c.getCurrentPreset().name + ja (" のコピー")); };
    browse.setComponentID ("mono.presetBrowser");
    browse.setTooltip (ja ("プリセット一覧を開く（名前の変更・削除・読み込み）"));
    browse.onClick = [this] { nav.showPresetBrowser(); };
    for (auto* b : { &save, &dup, &browse })
    {
        b->setFontSize (Theme::fontXS);
        addAndMakeVisible (*b);
    }
    refresh();
}

PresetSidebar::~PresetSidebar() = default;

void PresetSidebar::setFilter (const juce::String& id)
{
    filter = id;
    refresh();
}

void PresetSidebar::setSearchText (const juce::String& text) { search.setText (text, true); refresh(); }

juce::StringArray PresetSidebar::listedIds() const
{
    juce::StringArray out;
    for (auto* r : rows) out.add (juce::String (r->id));
    return out;
}

void PresetSidebar::refresh()
{
    const auto& all = c.getPresetLibrary().all();
    const bool anyUser = std::any_of (all.begin(), all.end(), [] (const Preset& p) { return ! p.builtin; });
    if (filter == "user" && ! anyUser) filter = "all";
    for (int i = 0; i < chips.size(); ++i)
    {
        chips[i]->setVisible (chipIds[i] != "user" || anyUser);
        chips[i]->setToggleState (chipIds[i] == filter, juce::dontSendNotification);
    }

    // the rows depend on the filter, the search text, the library and (for ★) the favourites
    const auto query = search.getText().trim();
    juce::String s = filter + "|" + query + "|";
    for (auto& p : all) s << juce::String (p.id) << ":" << p.name << ",";
    if (filter == "fav") s << c.getFavorites().joinIntoString (",");
    if (s != sig)
    {
        sig = s;
        rebuild();
    }
    const auto& cur = c.getCurrentPreset();
    for (auto* r : rows) r->update (r->id == cur.id, c.isFavorite (r->id), c.isCurrentPresetModified());
    resized();
    repaint();
}

void PresetSidebar::rebuild()
{
    rows.clear();
    const auto query = search.getText().trim();
    auto matches = [&query] (const Preset& p) { return query.isEmpty() || p.name.containsIgnoreCase (query); };
    std::vector<const Preset*> shown;
    if (filter == "fav")
    {
        for (auto& id : c.getFavorites())
            if (auto* p = c.getPresetLibrary().find (id.toStdString()); p != nullptr && matches (*p)) shown.push_back (p);
    }
    else
        for (auto& p : c.getPresetLibrary().all())
            if ((filter == "all" || p.category() == filter) && matches (p)) shown.push_back (&p);

    for (auto* p : shown) list.addAndMakeVisible (rows.add (new Row (*this, *p)));
    if (shown.empty())
    {
        if (empty == nullptr)
        {
            empty = std::make_unique<juce::Label>();
            empty->setComponentID ("mono.presetEmpty");
            empty->setFont (Theme::ui (Theme::fontXS));
            empty->setColour (juce::Label::textColourId, P().textSub);
            empty->setJustificationType (juce::Justification::topLeft);
            list.addAndMakeVisible (*empty);
        }
        empty->setText (query.isNotEmpty() ? ja ("見つかりません。別のことばで探してください。")
                        : filter == "fav"  ? ja ("お気に入りはまだありません。★ を押すと入ります。")
                                           : ja ("プリセットがありません。"),
                        juce::dontSendNotification);
        empty->setVisible (true);
    }
    else if (empty != nullptr)
        empty->setVisible (false);
}

void PresetSidebar::layoutRows()
{
    const int w = view.getWidth() - (view.isVerticalScrollBarShown() ? view.getScrollBarThickness() : 0);
    int y = 0;
    for (auto* r : rows)
    {
        r->setBounds (0, y, w, kRowH);
        y += kRowH + 2;
    }
    if (empty != nullptr && empty->isVisible())
    {
        empty->setBounds (0, 0, w, kRowH * 2);
        y = kRowH * 2;
    }
    list.setSize (w, juce::jmax (y, 1));
}

void PresetSidebar::resized()
{
    auto r = getLocalBounds();
    search.setBounds (r.removeFromTop (Theme::buttonH));
    r.removeFromTop (Theme::space2 + Theme::space1);

    // chips flow into as many rows as they need
    int x = r.getX(), y = r.getY();
    const int gap = Theme::space1 + 2;
    for (auto* chip : chips)
    {
        if (! chip->isVisible()) continue;
        const int w = juce::jmin (r.getWidth(), chip->preferredWidth() - Theme::space2);
        if (x > r.getX() && x + w > r.getRight())
        {
            x = r.getX();
            y += kChipH + gap;
        }
        chip->setBounds (x, y, w, kChipH);
        x += w + gap;
    }
    r.setTop (y + kChipH + Theme::space2 + Theme::space1);

    auto actions = r.removeFromBottom (Theme::space4 + Theme::space1);
    const int aw = (actions.getWidth() - 2 * Theme::space2) / 3;
    for (auto* b : { &save, &dup, &browse })
    {
        b->setBounds (actions.removeFromLeft (aw));
        actions.removeFromLeft (Theme::space2);
    }
    r.removeFromBottom (Theme::space2);
    listHeader = r.removeFromTop (Theme::space4 - 2);
    r.removeFromTop (Theme::space1);
    view.setBounds (r);
    layoutRows();
    layoutRows(); // again once the scroll bar state is known

    // keep the preset in use in view
    for (auto* row : rows)
        if (row->getToggleState() && view.getHeight() >= kRowH * 3)
        {
            const auto area = view.getViewArea();
            if (row->getY() < area.getY() || row->getBottom() > area.getBottom())
            {
                const int pitch = kRowH + 2, centred = row->getY() - (area.getHeight() - kRowH) / 2;
                view.setViewPosition (0, juce::jmax (0, (centred + pitch / 2) / pitch * pitch)); // whole rows at the top
            }
        }
}

void PresetSidebar::paint (juce::Graphics& g)
{
    const auto& p = P();
    drawText (g, filterTitle (filter), listHeader, Theme::fontXS, p.textSub);
    drawText (g, juce::String (rows.size()) + ja (" 種"), listHeader, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
}

void PresetSidebar::paintOverChildren (juce::Graphics& g)
{
    const auto sb = search.getBounds();
    drawIcon (g, Icon::search, juce::Rectangle<int> (sb.getX() + Theme::space2, sb.getY(), 16, sb.getHeight()).withSizeKeepingCentre (16, 16).toFloat(),
              P().textSub);
}

void PresetSidebar::choose (const std::string& id)
{
    // the controller outlives this list (a panel may close before a looper confirmation is answered)
    auto& ctl = c;
    mainui::withLooperCheck (c, nav, [&ctl, id] { ctl.loadPreset (id); }); // E-27
}

/** User preset -> overwrite (F-05-2); built-in -> save as a new user preset. Same as the Studio page. */
void PresetSidebar::savePreset()
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

void PresetSidebar::askName (const juce::String& title, const juce::String& initial)
{
    auto& ctl = c;
    auto& n = nav;
    auto panel = std::make_unique<mainui::ConfirmPanel> (nav, title, ja ("プリセットの名前を入れてください（32 文字まで）。"), ja ("保存"), nullptr);
    panel->addTextField (initial.substring (0, kPresetNameMaxChars), [&ctl, &n] (const juce::String& name)
    {
        if (name.isEmpty())
        {
            n.showToast (ja ("名前を入れてください。"));
            return;
        }
        juce::String error;
        n.showToast (ctl.saveCurrentAsNew (name, error) ? ja ("「") + name + ja ("」として保存しました。") : error);
    });
    nav.showOverlay (std::move (panel));
}

// =============================================================================================== panel
PresetPanel::PresetPanel (AppController& ctl, Navigator& n) : PanelBase (n, ja ("プリセット")), c (ctl), content (ctl, n)
{
    setComponentID ("mono.presetPanel");
    subtitle = ja ("押すとすぐ切り替わります");
    addAndMakeVisible (content);
    c.addChangeListener (this);
    setSize (Theme::space5 * 13, Theme::space5 * 30); // the overlay shrinks it to the window
}

PresetPanel::~PresetPanel() { c.removeChangeListener (this); }

void PresetPanel::resized()
{
    PanelBase::resized();
    content.setBounds (contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4));
}
} // namespace koe::ui::mono

#include "UI/screens/ThemeEditor.h"

#include "UI/ThemeLibrary.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace koe::ui
{
using namespace screens;

std::function<juce::File (bool)>& themeFileChooserForTests()
{
    static std::function<juce::File (bool)> hook;
    return hook;
}

namespace
{
constexpr int kRoleW = Theme::slotWNarrow;                        // two columns beside the selector at 800 x 560
constexpr int kSelectorW = Theme::space5 * 9;                     // 288
constexpr int kSelectorH = Theme::space5 * 9;                     // 288
constexpr int kSideBySide = Theme::space5 * 20;                   // content width for roles | selector
constexpr int kPreviewH = Theme::space5 * 6 + Theme::space3;      // 208
constexpr int kTableRowH = Theme::touchMin - Theme::space1;       // 28

int failures (const Palette& p)
{
    int n = 0;
    for (auto& k : Theme::contrastChecks (p)) n += k.ratio < k.min ? 1 : 0;
    return n;
}

juce::String ratioText (double r) { return juce::String (r, 2) + " : 1"; }

/** One palette role: a swatch in the edited colour, the role's name and its hex. Selected = raised fill + bold. */
class RoleButton : public juce::Button
{
public:
    RoleButton (int r, const Palette& edited) : juce::Button (Theme::roleName (r)), role (r), palette (edited)
    {
        setTitle (Theme::roleName (r));
        setWantsKeyboardFocus (true);
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& p = Theme::colours();
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (getToggleState() || highlighted)
        {
            g.setColour (getToggleState() ? p.raised : p.text.withAlpha (0.06f));
            g.fillRoundedRectangle (r, Theme::radiusS);
        }
        if (getToggleState())
        {
            g.setColour (p.text);
            g.drawRoundedRectangle (r, Theme::radiusS, 1.0f);
        }
        auto area = getLocalBounds().reduced (Theme::space2, 0);
        const auto sw = area.removeFromLeft (Theme::space4).withSizeKeepingCentre (Theme::space4 - Theme::space1, Theme::space4 - Theme::space1).toFloat();
        g.setColour (Theme::role (palette, role));
        g.fillRoundedRectangle (sw, Theme::radiusS);
        g.setColour (p.border);
        g.drawRoundedRectangle (sw, Theme::radiusS, 1.0f);
        area.removeFromLeft (Theme::space2);
        drawText (g, Theme::roleName (role), area, Theme::fontXS, p.text, juce::Justification::centredLeft, getToggleState());
        if (hasKeyboardFocus (true)) drawFocusRing (g, r.reduced (Theme::focusRingWidth + 1.0f), Theme::radiusS);
    }

    const int role;

private:
    const Palette& palette;
};

/** A mini card in the edited colours: text, sub text, accent button, toggles, slider, ok / warn / danger. */
class Preview : public juce::Component
{
public:
    explicit Preview (const Palette& edited) : p (edited) { setTitle (ja ("見本")); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (p.bg);
        g.fillRoundedRectangle (r, Theme::radiusM);
        auto card = r.reduced (float (Theme::space3));
        g.setColour (p.surface);
        g.fillRoundedRectangle (card, Theme::radiusL);
        g.setColour (p.divider);
        g.drawRoundedRectangle (card.reduced (0.5f), Theme::radiusL, 1.0f);
        auto in = card.reduced (float (Theme::space3)).toNearestInt();

        // the raised chip at the top right, then the title lines beside it
        auto top = in.removeFromTop (Theme::space5 + Theme::space3);
        auto chip = top.removeFromRight (Theme::space5 * 3).withSizeKeepingCentre (Theme::space5 * 3, Theme::space5).toFloat();
        g.setColour (p.raised);
        g.fillRoundedRectangle (chip, Theme::radiusS);
        g.setColour (p.border);
        g.drawRoundedRectangle (chip.reduced (0.5f), Theme::radiusS, 1.0f);
        drawText (g, ja ("浮いた面"), chip.toNearestInt(), Theme::fontXS, p.text, juce::Justification::centred);
        drawText (g, ja ("見本のカード"), top.removeFromTop (Theme::space4), Theme::fontM, p.text, juce::Justification::centredLeft, true);
        drawText (g, ja ("補足の文字はこの色で描きます"), top, Theme::fontXS, p.textSub);
        in.removeFromTop (Theme::space2);
        g.setColour (p.divider);
        g.fillRect (in.removeFromTop (1));
        in.removeFromTop (Theme::space2 + Theme::space1);

        // buttons and switches
        auto row = in.removeFromTop (Theme::touchMin);
        auto button = row.removeFromLeft (Theme::space5 * 4).toFloat();
        g.setColour (p.accent);
        g.fillRoundedRectangle (button, Theme::radiusM);
        drawText (g, ja ("ボイチェン ON"), button.toNearestInt(), Theme::fontS, p.onAccent, juce::Justification::centred, true);
        row.removeFromLeft (Theme::space2);
        auto outline = row.removeFromLeft (Theme::space5 * 3).toFloat();
        g.setColour (p.border);
        g.drawRoundedRectangle (outline.reduced (0.75f), Theme::radiusM, Theme::borderWidth);
        drawText (g, ja ("取り消し"), outline.toNearestInt(), Theme::fontS, p.text, juce::Justification::centred, true);
        row.removeFromLeft (Theme::space3);
        for (const bool on : { true, false })
        {
            auto t = row.removeFromLeft (Theme::toggleW + Theme::space2 + Theme::space5);
            auto track = t.removeFromLeft (Theme::toggleW).withSizeKeepingCentre (Theme::toggleW, Theme::toggleH).toFloat();
            const float rad = Theme::toggleH * 0.5f;
            g.setColour (on ? p.accent : p.trackOff);
            g.fillRoundedRectangle (track, rad);
            g.setColour (on ? p.accent : p.border);
            g.drawRoundedRectangle (track.reduced (0.75f), rad, Theme::borderWidth);
            const float d = Theme::toggleH - 6.0f;
            g.setColour (on ? p.onAccent : p.textSub);
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ on ? track.getRight() - rad : track.getX() + rad, track.getCentreY() }));
            t.removeFromLeft (Theme::space1);
            drawText (g, on ? "ON" : "OFF", t, Theme::fontXS, on ? p.text : p.textSub, juce::Justification::centredLeft, true, true);
        }
        in.removeFromTop (Theme::space3);

        // slider
        row = in.removeFromTop (Theme::space4);
        auto value = row.removeFromRight (Theme::space5 * 2);
        auto line = row.withSizeKeepingCentre (row.getWidth() - Theme::space3, 4).toFloat();
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (line, 2.0f);
        g.setColour (p.accent);
        g.fillRoundedRectangle (line.withWidth (line.getWidth() * 0.6f), 2.0f);
        g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ line.getX() + line.getWidth() * 0.6f, line.getCentreY() }));
        drawText (g, "60 %", value, Theme::fontXS, p.text, juce::Justification::centredRight, false, true);
        in.removeFromTop (Theme::space3);

        // status colours (text, never colour alone) and the danger fill
        row = in.removeFromTop (Theme::space4);
        const std::pair<juce::Colour, const char*> states[] = { { p.ok, "正常" }, { p.warn, "注意" }, { p.danger, "危険" } };
        for (auto& [col, text] : states)
        {
            auto s = row.removeFromLeft (Theme::space5 * 2 + Theme::space2);
            g.setColour (col);
            g.fillEllipse (s.removeFromLeft (Theme::space2 + Theme::space1).withSizeKeepingCentre (Theme::space2 + Theme::space1, Theme::space2 + Theme::space1).toFloat());
            s.removeFromLeft (Theme::space1);
            drawText (g, ja (text), s, Theme::fontS, col, juce::Justification::centredLeft, true);
        }
        auto mute = row.removeFromLeft (Theme::space5 * 2 + Theme::space2).toFloat();
        g.setColour (p.danger);
        g.fillRoundedRectangle (mute, Theme::radiusS);
        drawText (g, "MUTE", mute.toNearestInt(), Theme::fontXS, p.onDanger, juce::Justification::centred, true, true);
    }

private:
    const Palette& p;
};

/** The contrast rules for the edited palette: the pairs below their minimum first, marked 「不足」 (not colour alone). */
class ContrastTable : public juce::Component
{
public:
    explicit ContrastTable (const Palette& edited) : p (edited) { setTitle (ja ("コントラスト")); }

    static int columns (int width) { return width >= kSideBySide ? 2 : 1; }
    int heightForWidth (int width) const
    {
        const int n = int (Theme::contrastChecks (p).size());
        return (n + columns (width) - 1) / columns (width) * kTableRowH;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& ui = Theme::colours();
        auto checks = Theme::contrastChecks (p);
        std::stable_sort (checks.begin(), checks.end(), [] (auto& a, auto& b) { return (a.ratio < a.min) > (b.ratio < b.min); });
        const int cols = columns (getWidth()), perCol = (int (checks.size()) + cols - 1) / cols;
        const int colW = (getWidth() - (cols - 1) * Theme::space3) / cols;
        for (int i = 0; i < int (checks.size()); ++i)
        {
            const auto& k = checks[size_t (i)];
            const bool bad = k.ratio < k.min;
            auto r = juce::Rectangle<int> ((i / perCol) * (colW + Theme::space3), (i % perCol) * kTableRowH, colW, kTableRowH);
            if (i % perCol > 0)
            {
                g.setColour (ui.divider);
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
            }
            auto sample = r.removeFromLeft (Theme::space5 + Theme::space2).reduced (0, Theme::space1).toFloat();
            g.setColour (Theme::role (p, k.bg));
            g.fillRoundedRectangle (sample, Theme::radiusS);
            g.setColour (ui.border);
            g.drawRoundedRectangle (sample.reduced (0.5f), Theme::radiusS, 1.0f);
            drawText (g, "Aa", sample.toNearestInt(), Theme::fontXS, Theme::role (p, k.fg), juce::Justification::centred, true);
            r.removeFromLeft (Theme::space2);
            auto result = r.removeFromRight (Theme::space5 * 2);
            auto ratio = r.removeFromRight (Theme::space5 * 2 + Theme::space3);
            result.removeFromLeft (Theme::space2);
            drawText (g, Theme::roleName (k.fg) + " / " + Theme::roleName (k.bg), r, Theme::fontXS, ui.text);
            drawText (g, ratioText (k.ratio), ratio, Theme::fontXS, ui.text, juce::Justification::centredRight, false, true);
            if (bad)
            {
                auto icon = result.removeFromLeft (Theme::space3).withSizeKeepingCentre (Theme::space3 - 2, Theme::space3 - 2).toFloat();
                drawIcon (g, Icon::warning, icon, ui.warn, 1.6f);
            }
            drawText (g, bad ? ja ("不足") : "OK", result, Theme::fontXS, bad ? ui.warn : ui.textSub, juce::Justification::centredRight, true);
        }
    }

private:
    const Palette& p;
};
} // namespace

// =============================================================================================== Impl
struct ThemeEditor::Impl final : juce::ChangeListener
{
    Impl (ThemeEditor& o, AppController& ctl, const juce::String& startFile) : owner (o), c (ctl)
    {
        data.name = ja ("マイテーマ");
        data.colours = Theme::colours();
        data.dark = Theme::isDark();

        auto body = std::make_unique<VStack> (Theme::space3);
        stack = body.get();

        // name, dark / light, 保存
        name.setComponentID ("themeEditor.name");
        name.setTitle (ja ("テーマの名前"));
        name.setInputRestrictions (ThemeLibrary::maxNameLength);
        name.setFont (Theme::ui (Theme::fontS));
        name.setJustification (juce::Justification::centredLeft);
        name.setIndents (Theme::space3, 0);
        dark.setComponentID ("themeEditor.dark");
        dark.setTitle (ja ("ダークかライトか"));
        dark.onChange = [this] (int i) { data.dark = i == 0; };
        save.setComponentID ("themeEditor.save");
        save.onClick = [this] { saveTheme(); };
        auto header = std::make_unique<LayoutBox>();
        header->addAndMakeVisible (name);
        header->addAndMakeVisible (dark);
        header->addAndMakeVisible (save);
        header->layout = [this] (juce::Rectangle<int> r)
        {
            const int dw = dark.preferredWidth(), sw = save.preferredWidth();
            if (stackedHeader (r.getWidth()))
            {
                name.setBounds (r.removeFromTop (Theme::controlH));
                r.removeFromTop (Theme::space2);
                dark.setBounds (r.removeFromLeft (dw).withHeight (Theme::buttonH));
                save.setBounds (r.removeFromRight (sw).withHeight (Theme::buttonH));
                return;
            }
            save.setBounds (r.removeFromRight (sw).withSizeKeepingCentre (sw, Theme::buttonH));
            r.removeFromRight (Theme::space2);
            dark.setBounds (r.removeFromRight (dw).withSizeKeepingCentre (dw, Theme::buttonH));
            r.removeFromRight (Theme::space3);
            name.setBounds (r);
        };
        stack->add (std::move (header), [this] (int w) { return stackedHeader (w) ? Theme::controlH + Theme::space2 + Theme::buttonH : Theme::controlH; });

        message = &own (std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::text), "themeEditor.message",
                        [this] (int w) { return message->heightForWidth (w); });
        message->setVisible (false);

        // roles | selector
        heading ("色を選ぶ（役割ごと）");
        auto pick = std::make_unique<LayoutBox>();
        for (int r = 0; r < Theme::numRoles; ++r)
        {
            auto* b = roles.add (new RoleButton (r, data.colours));
            b->setComponentID (juce::String ("themeEditor.role.") + Theme::roleKey (r));
            b->onClick = [this, r] { selectRole (r); };
            roleFlow.add (*b, kRoleW, Theme::touchMin);
        }
        pick->addAndMakeVisible (roleFlow);
        selector.setComponentID ("themeEditor.selector");
        selector.setTitle (ja ("色"));
        selector.addChangeListener (this);
        pick->addAndMakeVisible (selector);
        pick->layout = [this] (juce::Rectangle<int> r)
        {
            if (r.getWidth() >= kSideBySide)
            {
                selector.setBounds (r.removeFromRight (kSelectorW).withHeight (kSelectorH));
                r.removeFromRight (Theme::space3);
                roleFlow.setBounds (r.withHeight (roleFlow.heightForWidth (r.getWidth())));
                return;
            }
            roleFlow.setBounds (r.removeFromTop (roleFlow.heightForWidth (r.getWidth())));
            r.removeFromTop (Theme::space3);
            selector.setBounds (r.removeFromTop (kSelectorH).withWidth (juce::jmin (r.getWidth(), kSelectorW + Theme::space5 * 2)));
        };
        stack->add (std::move (pick), [this] (int w)
        {
            return w >= kSideBySide ? juce::jmax (kSelectorH, roleFlow.heightForWidth (w - kSelectorW - Theme::space3))
                                    : roleFlow.heightForWidth (w) + Theme::space3 + kSelectorH;
        });

        heading ("見本");
        auto prev = std::make_unique<Preview> (data.colours);
        preview = prev.get();
        preview->setComponentID ("themeEditor.preview");
        stack->add (std::move (prev), [] (int) { return kPreviewH; });

        heading ("コントラスト");
        summary = &own (std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::ok), "themeEditor.contrastSummary",
                        [this] (int w) { return summary->heightForWidth (w); });
        auto table = std::make_unique<ContrastTable> (data.colours);
        contrast = table.get();
        contrast->setComponentID ("themeEditor.contrast");
        stack->add (std::move (table), [this] (int w) { return contrast->heightForWidth (w); });

        // the saved themes
        heading ("自分のテーマ");
        auto importRow = std::make_unique<LayoutBox>();
        importButton = std::make_unique<PillButton> (ja ("読み込み"), PillButton::Style::outline, Icon::folder);
        auto* imp = importButton.get();
        importRow->addAndMakeVisible (imp);
        imp->setComponentID ("themeEditor.import");
        imp->onClick = [this] { importTheme(); };
        importRow->layout = [imp] (juce::Rectangle<int> r) { imp->setBounds (r.withWidth (juce::jmin (r.getWidth(), imp->preferredWidth()))); };
        stack->add (std::move (importRow), [] (int) { return Theme::buttonH; });
        list = &own (std::make_unique<ListHolder>(), "themeEditor.list", [this] (int w) { return list->heightForWidth (w); });

        owner.setContent (std::move (body), [this] (int w) { return stack->heightForWidth (w); });

        if (startFile.isNotEmpty()) edit (startFile);
        name.setText (data.name, false);
        dark.setSelected (data.dark ? 0 : 1);
        selectRole (0);
        rebuildList();
        refreshColours();
        coloursChanged();
    }

    ~Impl() override { selector.removeChangeListener (this); }

    /** Adds a component the stack owns; returns it. */
    template <typename C>
    C& own (std::unique_ptr<C> comp, const char* id, std::function<int (int)> heightFor)
    {
        comp->setComponentID (id);
        return static_cast<C&> (stack->add (std::move (comp), std::move (heightFor)));
    }

    /** The saved themes; replaced as a whole when the list changes. The previous list is kept until the next change:
        its button may be the one whose click is still running (one rebuild per click). */
    struct ListHolder : juce::Component
    {
        std::unique_ptr<VStack> inner, retired;
        void set (std::unique_ptr<VStack> s)
        {
            if (inner != nullptr) removeChildComponent (inner.get());
            retired = std::move (inner);
            inner = std::move (s);
            addAndMakeVisible (*inner);
            resized();
        }
        int heightForWidth (int w) const { return inner != nullptr ? inner->heightForWidth (w) : 0; }
        void resized() override { if (inner != nullptr) inner->setBounds (getLocalBounds()); }
    };

    static bool stackedHeader (int w) { return w < kSideBySide; }

    void heading (const char* text)
    {
        auto l = std::make_unique<TextLabel> (ja (text), Theme::fontS, Tone::text, true);
        auto* raw = l.get();
        stack->add (std::move (l), [raw] (int w) { return raw->heightForWidth (w); });
    }

    void refreshColours()
    {
        const auto& p = Theme::colours();
        selector.setColour (juce::ColourSelector::backgroundColourId, p.surface);
        selector.setColour (juce::ColourSelector::labelTextColourId, p.text);
        name.setTextToShowWhenEmpty (ja ("テーマの名前"), p.textSub);
    }

    void selectRole (int r)
    {
        role = r;
        for (auto* b : roles) b->setToggleState (b->role == r, juce::dontSendNotification);
        selector.setCurrentColour (Theme::role (data.colours, r).withAlpha (1.0f), juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        auto col = selector.getCurrentColour();
        if (role == 13) col = col.withAlpha (Theme::role (data.colours, role).getAlpha()); // overlay keeps its alpha
        setColour (role, col);
    }

    void setColour (int r, juce::Colour col)
    {
        Theme::role (data.colours, r) = col;
        coloursChanged();
    }

    /** The sample, the swatches and the contrast table follow the edited palette at once. */
    void coloursChanged()
    {
        for (auto* b : roles) b->repaint();
        preview->repaint();
        const int n = failures (data.colours);
        summary->setText (n > 0 ? ja ("コントラストが足りない組み合わせが ") + juce::String (n) + ja (" 件あります（表の「不足」）。保存はできますが、文字や枠が読みにくくなります。")
                               : ja ("すべての組み合わせが基準（文字 4.5:1、枠 3:1）を満たしています。"));
        summary->setTone (n > 0 ? Tone::warn : Tone::ok);
        summary->setIcon (n > 0 ? std::optional<Icon> (Icon::warning) : std::optional<Icon> (Icon::check));
        contrast->repaint();
        owner.resized();
    }

    void showMessage (const juce::String& text, Tone tone)
    {
        message->setText (text);
        message->setTone (tone);
        message->setVisible (text.isNotEmpty());
        owner.resized();
    }

    // ------------------------------------------------------------------------------- file actions
    void edit (const juce::String& f)
    {
        juce::String err;
        const auto t = ThemeLibrary::load (ThemeLibrary::fileFor (f), err);
        if (! t)
        {
            showMessage (err, Tone::danger);
            return;
        }
        data = *t;
        file = f;
        name.setText (data.name, false);
        dark.setSelected (data.dark ? 0 : 1);
        selectRole (role);
        coloursChanged();
        rebuildList();
        showMessage (ja ("「") + data.name + ja ("」を編集しています。"), Tone::text);
    }

    /** Saves (a new file the first time) and uses the theme: the whole app is rebuilt in it (MainComponent::applyTheme). */
    void saveTheme()
    {
        juce::String err;
        const auto n = ThemeLibrary::checkName (name.getText(), err);
        if (n.isEmpty()) return showMessage (err, Tone::danger);
        data.name = n;
        if (file.isEmpty()) file = ThemeLibrary::saveNew (data, err);
        else if (! ThemeLibrary::save (file, data, err)) return showMessage (err, Tone::danger);
        if (file.isEmpty()) return showMessage (err, Tone::danger);
        const int bad = failures (data.colours);
        showMessage (ja ("「") + n + ja ("」を保存して、配色に使いました。")
                         + (bad > 0 ? ja ("コントラストが足りない組み合わせが ") + juce::String (bad) + ja (" 件あります。") : juce::String()),
                     bad > 0 ? Tone::warn : Tone::ok);
        const auto id = ThemeLibrary::userId (file);
        if (c.getSettings().themeId == id) Theme::invalidate(); // same id, new colours
        c.updateSettings ([id] (Settings& s) { s.themeId = id; });
        rebuildList();
    }

    void askRename (const ThemeLibrary::Entry& e)
    {
        prompt (ja ("名前変更"), ja ("新しい名前を入れてください（32 文字まで）。"), true, e.name, ja ("変更する"), false, [this, f = e.file] (const juce::String& text)
        {
            juce::String err;
            if (! ThemeLibrary::rename (f, text, err)) return showMessage (err, Tone::danger);
            if (f == file)
            {
                data.name = text.trim();
                name.setText (data.name, false);
            }
            showMessage (ja ("名前を「") + text.trim() + ja ("」に変えました。"), Tone::ok);
            listChanged();
        });
    }

    void duplicate (const ThemeLibrary::Entry& e)
    {
        juce::String err;
        if (ThemeLibrary::duplicate (e.file, err).isEmpty()) return showMessage (err, Tone::danger);
        showMessage (ja ("「") + e.name + ja ("」を複製しました。"), Tone::ok);
        listChanged();
    }

    void askDelete (const ThemeLibrary::Entry& e)
    {
        prompt (ja ("「") + e.name + ja ("」を削除しますか"), ja ("削除したテーマは元に戻せません。使っている場合は Studio の配色に戻ります。"), false, {}, ja ("削除する"), true,
                [this, e] (const juce::String&)
        {
            juce::String err;
            if (! ThemeLibrary::remove (e.file, err)) return showMessage (err, Tone::danger);
            if (e.file == file) file = {}; // the colours stay here as a new, unsaved theme
            showMessage (ja ("「") + e.name + ja ("」を削除しました。"), Tone::text);
            if (c.getSettings().themeId == ThemeLibrary::userId (e.file)) c.updateSettings ([] (Settings& s) { s.themeId = {}; });
            listChanged();
        });
    }

    void exportTheme (const ThemeLibrary::Entry& e)
    {
        chooseFile (true, e.file, [this, e] (juce::File f)
        {
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
            juce::String err;
            if (! ThemeLibrary::exportFile (e.file, f, err)) return showMessage (err, Tone::danger);
            showMessage (ja ("「") + f.getFileName() + ja ("」に書き出しました。"), Tone::ok);
        });
    }

    void importTheme()
    {
        chooseFile (false, {}, [this] (const juce::File& f)
        {
            juce::String err;
            const auto newFile = ThemeLibrary::importFile (f, err);
            if (newFile.isEmpty()) return showMessage (ja ("読み込めませんでした。") + err, Tone::danger);
            showMessage (ja ("「") + f.getFileName() + ja ("」を読み込みました。「編集」で色を変えたり、S-03 の「配色」で選んだりできます。"), Tone::ok);
            listChanged();
        });
    }

    void chooseFile (bool saving, const juce::String& defaultName, std::function<void (juce::File)> then)
    {
        if (auto& hook = themeFileChooserForTests())
        {
            if (const auto f = hook (saving); f != juce::File()) then (f);
            return;
        }
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        chooser = std::make_unique<juce::FileChooser> (saving ? ja ("テーマの書き出し先") : ja ("読み込むテーマ（JSON）"),
                                                       saving ? docs.getChildFile (defaultName + ".json") : docs, "*.json");
        const int mode = juce::FileBrowserComponent::canSelectFiles
                         | (saving ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode);
        chooser->launchAsync (mode, [then] (const juce::FileChooser& fc)
        {
            if (const auto f = fc.getResult(); f != juce::File()) then (f);
        });
    }

    /** S-03's 「配色」 list follows on the controller's change message. */
    void listChanged()
    {
        rebuildList();
        c.updateSettings ([] (Settings&) {});
    }

    void rebuildList()
    {
        auto fresh = std::make_unique<VStack> (Theme::space2);
        auto* raw = fresh.get();
        const auto entries = ThemeLibrary::list();
        if (entries.empty())
            raw->addText (ja ("まだありません。色を選んで「保存」すると、ここに並びます。"), Theme::fontXS, Tone::sub);
        for (auto& e : entries)
        {
            auto& label = raw->addText (e.name + (e.file == file ? ja ("（編集中）") : juce::String()), Theme::fontS, Tone::text, true);
            label.setComponentID ("themeEditor.item." + e.file);
            auto flow = std::make_unique<FlowBox> (Theme::space2);
            const std::pair<const char*, const char*> actions[] = { { "edit", "編集" }, { "rename", "名前変更" }, { "duplicate", "複製" },
                                                                    { "delete", "削除" }, { "export", "書き出し" } };
            for (auto& [id, text] : actions)
            {
                auto b = std::make_unique<PillButton> (ja (text), PillButton::Style::outline);
                b->setComponentID (juce::String ("themeEditor.") + id + "." + e.file);
                b->setTitle (ja (text) + " " + e.name);
                const juce::String action (id);
                b->onClick = [this, e, action] { run (action, e); }; // a rebuild keeps this list alive (ListHolder::retired)
                const int bw = b->preferredWidth();
                flow->add (std::move (b), bw, Theme::buttonH);
            }
            auto* fr = flow.get();
            raw->add (std::move (flow), [fr] (int w) { return fr->heightForWidth (w); });
        }
        list->set (std::move (fresh)); // rebuilt as a whole: a handful of themes
        owner.resized();
    }

    void run (const juce::String& action, const ThemeLibrary::Entry& e)
    {
        if (action == "edit") edit (e.file);
        else if (action == "rename") askRename (e);
        else if (action == "duplicate") duplicate (e);
        else if (action == "delete") askDelete (e);
        else if (action == "export") exportTheme (e);
    }

    void prompt (const juce::String& t, const juce::String& msg, bool input, const juce::String& initial, const juce::String& okLabel, bool danger,
                 std::function<void (const juce::String&)> onOk)
    {
        // the prompt is deleted later (we are inside its callback); a newer, visible prompt is kept
        auto finish = [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ThemeEditor> (&owner)]
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
        activePrompt->setComponentID ("themeEditor.prompt");
        owner.addAndMakeVisible (*activePrompt);
        activePrompt->setBounds (owner.getLocalBounds());
    }

    ThemeEditor& owner;
    AppController& c;
    ThemeData data;
    juce::String file;
    int role = 0;

    VStack* stack = nullptr;
    juce::TextEditor name;
    Segmented dark { juce::StringArray { ja ("ダーク"), ja ("ライト") } };
    PillButton save { ja ("保存"), PillButton::Style::primary };
    TextLabel* message = nullptr;
    juce::OwnedArray<RoleButton> roles;
    FlowBox roleFlow { Theme::space2 };
    juce::ColourSelector selector { juce::ColourSelector::showColourAtTop | juce::ColourSelector::editableColour | juce::ColourSelector::showColourspace };
    Preview* preview = nullptr;
    TextLabel* summary = nullptr;
    ContrastTable* contrast = nullptr;
    ListHolder* list = nullptr;
    std::unique_ptr<PillButton> importButton;
    std::unique_ptr<InlinePrompt> activePrompt;
    std::unique_ptr<juce::FileChooser> chooser;
};

// =============================================================================================== ThemeEditor
ThemeEditor::ThemeEditor (AppController& controller, Navigator& nav, const juce::String& file)
    : OverlayPanel (ja (file.isEmpty() ? "配色を作る" : "配色の編集"), [&nav] { nav.closeOverlay(); })
{
    setComponentID ("themeEditor");
    setSize (screens::m::comboW * 2 + Theme::space5 * 7, Theme::defaultHeight - Theme::space5 * 2);
    impl = std::make_unique<Impl> (*this, controller, file);
}

ThemeEditor::~ThemeEditor() = default;

const ThemeData& ThemeEditor::working() const { return impl->data; }
juce::String ThemeEditor::editingFile() const { return impl->file; }

void ThemeEditor::setRoleColour (int role, juce::Colour colour)
{
    impl->selectRole (juce::jlimit (0, Theme::numRoles - 1, role));
    impl->setColour (impl->role, colour);
    impl->selector.setCurrentColour (colour.withAlpha (1.0f), juce::dontSendNotification);
}

bool ThemeEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && impl->activePrompt != nullptr && impl->activePrompt->isVisible())
    {
        impl->activePrompt->cancel();
        return true;
    }
    return OverlayPanel::keyPressed (k);
}

void ThemeEditor::lookAndFeelChanged()
{
    OverlayPanel::lookAndFeelChanged();
    if (impl != nullptr) impl->refreshColours();
}
} // namespace koe::ui

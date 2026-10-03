// 案 C Mono S-01 (docs/mockups/C-S01.dc.html), owner wave5/mono: preset column | ボイチェン / マイクミュート / meters,
// ピッチ / フォルマント / 重ねる声, the chain rack, the bottom line. The page draws its own header (ownsHeader), so it
// also carries the ways to 設定 / ヘルプ (ツアー) / 音源 (サウンドボード). Below 1000 px (§8.2.1) the preset column folds into a
// preset button that opens the same list as a panel. Parts: UI/skins/mono/MonoParts.h.
#include "UI/main/VoicePage.h"

#include "UI/main/VoiceView.h"
#include "UI/skins/mono/MonoParts.h"

namespace koe::ui::mainui
{
namespace
{
using namespace koe::ui::mono;

const Palette& P() { return Theme::colours(); }

constexpr int kSideW = Theme::space5 * 7 + Theme::space4;          // 248 (mock)
constexpr int kTopH = Theme::space5 * 3;                           // 96: ボイチェン / ミュート / meters
constexpr int kTopHCompact = Theme::space5 * 2 - Theme::space1;    // 60
constexpr int kVoiceW = Theme::space5 * 7 + Theme::space2 + 4;     // 236
constexpr int kMuteW = Theme::space5 * 4 + Theme::space3 + 4;      // 148
constexpr int kCardsMax = Theme::space5 * 5 + Theme::space3;       // 176
constexpr int kCardsMin = Theme::space5 * 4 + Theme::space4;       // 152
constexpr int kCardsMaxCompact = Theme::space5 * 3 + Theme::space4; // 120
constexpr int kCardsMinCompact = Theme::space5 * 3 + Theme::space2; // 104

/** Compact layout: the preset in use as a button (opens PresetPanel). tour.presets is the frame, the button the selector. */
class PresetPicker : public juce::Component
{
public:
    PresetPicker (AppController& ctl, Navigator& n) : c (ctl), nav (n), button (*this)
    {
        button.setComponentID ("voice.presetSelector");
        addAndMakeVisible (button);
    }

    void refresh()
    {
        const auto& cur = c.getCurrentPreset();
        name = cur.name;
        category = presetCategoryJa (cur.category()) + (c.isCurrentPresetModified() ? ja ("・編集中") : juce::String());
        button.repaint();
    }

    void resized() override { button.setBounds (getLocalBounds()); }

private:
    class Button : public juce::Button
    {
    public:
        explicit Button (PresetPicker& o) : juce::Button ("preset"), owner (o)
        {
            setWantsKeyboardFocus (true);
            setTitle (ja ("プリセットを選ぶ"));
            setTooltip (ja ("プリセットを選ぶ（一覧・検索・お気に入り・保存）"));
            onClick = [this] { owner.nav.showOverlay (std::make_unique<PresetPanel> (owner.c, owner.nav)); };
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto& p = P();
            const auto b = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (highlighted ? p.raised.brighter (0.04f) : p.raised);
            g.fillRoundedRectangle (b, Theme::radiusM);
            g.setColour (p.border);
            g.drawRoundedRectangle (b, Theme::radiusM, 1.0f);
            auto r = getLocalBounds().reduced (Theme::space2 + Theme::space1, 0);
            drawIcon (g, Icon::chevronDown, r.removeFromRight (16).withSizeKeepingCentre (16, 16).toFloat(), p.textSub);
            r.removeFromRight (Theme::space1);
            const int nameW = juce::jmin (r.getWidth() - Theme::space5, textWidth (Theme::ui (Theme::fontS, true), owner.name) + Theme::space1);
            drawText (g, owner.name, r.removeFromLeft (nameW), Theme::fontS, p.text, juce::Justification::centredLeft, true);
            r.removeFromLeft (Theme::space1 + 2);
            drawText (g, owner.category, r, Theme::fontXS, p.textSub);
            if (hasKeyboardFocus (true)) drawFocusRing (g, b, Theme::radiusM);
        }

    private:
        PresetPicker& owner;
    };

    AppController& c;
    Navigator& nav;
    juce::String name, category;
    Button button;
};

class MonoVoicePage : public VoicePage
{
public:
    MonoVoicePage (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), sidebar (ctl, n), picker (ctl, n), voice (ctl), mute (ctl), input (ctl, n, false), output (ctl, n, true),
          shifter (ctl), layers (ctl, n), rack (ctl, n), bottom (ctl, n, true),
          settings (ja ("設定"), PillButton::Style::outline), help (ja ("ヘルプ"), PillButton::Style::outline, Icon::help),
          sound (ja ("音源"), PillButton::Style::outline, Icon::speaker)
    {
        setComponentID ("voice.view");
        settings.setComponentID ("mono.settings");
        settings.setTooltip (ja ("設定（デバイス・ホットキー・外観など）"));
        settings.onClick = [this] { nav.showPage (Navigator::Page::settings); };
        help.setComponentID ("mono.help");
        help.setTooltip (ja ("ヘルプ（ガイドツアー・Discord の設定手順など）"));
        help.onClick = [this] { showHelpMenu(); };
        sound.setComponentID ("mono.soundboard");
        sound.setTooltip (ja ("サウンドボード（効果音を鳴らす）"));
        sound.onClick = [this] { nav.showPage (Navigator::Page::soundboard); };
        bottom.setComponentID ("voice.bottom");
        bottom.setCompact (true); // monitor switch + 遅延 / XRUN / CPU fit the narrower body (the volume is behind 「モニター」)
        for (auto* comp : std::initializer_list<juce::Component*> { &sidebar, &picker, &voice, &mute, &input, &output, &shifter, &layers, &rack,
                                                                    &bottom, &settings, &help, &sound })
            addAndMakeVisible (comp);
        applyCompact();
        refresh();
    }

    bool ownsHeader() const override { return true; }
    bool isCompact() const override { return compact; }

    void setCompact (bool cp) override
    {
        if (compact == cp && laidOut) return;
        compact = cp;
        applyCompact();
        resized();
        repaint();
    }

    void refresh() override
    {
        sidebar.refresh();
        picker.refresh();
        voice.refresh();
        mute.refresh();
        input.refresh();
        output.refresh();
        shifter.refresh();
        layers.refresh();
        rack.refresh();
        bottom.refresh();
        layersStopped = c.areLayersAutoStopped();
    }

    void tick() override
    {
        const auto m = c.pollMeters();
        const auto s = c.getStatus();
        input.tick (m, s);
        output.tick (m, s);
        bottom.tick (s);
        if (++ticks % 5 == 0) // auto-stop flags live in the engine: no change message for them
        {
            rack.refresh();
            if (c.areLayersAutoStopped() != layersStopped)
            {
                layersStopped = ! layersStopped;
                layers.refresh();
            }
        }
    }

    void resized() override
    {
        laidOut = true;
        if (compact) layoutCompact (getLocalBounds());
        else layoutWide (getLocalBounds());
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        if (! sideArea.isEmpty())
        {
            g.setColour (p.surface);
            g.fillRect (sideArea);
            g.setColour (p.divider);
            g.fillRect (sideArea.getRight() - 1, sideArea.getY(), 1, sideArea.getHeight());
        }
        auto logo = logoArea;
        const int s = compact ? Theme::space4 : Theme::space4 + 2;
        drawIcon (g, Icon::logo, logo.removeFromLeft (s).withSizeKeepingCentre (s, s).toFloat(), p.text, 2.4f, p.accent);
        logo.removeFromLeft (Theme::space2 + 2);
        drawText (g, "KoeLoom", logo, compact ? Theme::fontM : Theme::fontL, p.text, juce::Justification::centredLeft, true);
    }

private:
    void applyCompact()
    {
        sidebar.setVisible (! compact);
        picker.setVisible (compact);
        // the visible preset part is the tour's step 2 target
        sidebar.setComponentID (compact ? "mono.presets" : "tour.presets");
        picker.setComponentID (compact ? "tour.presets" : "mono.presetPicker");
        if (auto* list = findById (&sidebar, compact ? "voice.presetSelector" : "mono.presetList")) // one selector ID at a time
            list->setComponentID (compact ? "mono.presetList" : "voice.presetSelector");
        if (auto* button = findById (&picker, compact ? "mono.presetButton" : "voice.presetSelector"))
            button->setComponentID (compact ? "voice.presetSelector" : "mono.presetButton");
        input.setCompact (compact);
        output.setCompact (compact);
        shifter.setCompact (compact);
        layers.setCompact (compact);
        rack.setCompact (compact);
        for (auto* b : { &settings, &help, &sound }) b->setFontSize (compact ? Theme::fontXS : Theme::fontS);
    }

    void layoutWide (juce::Rectangle<int> r)
    {
        sideArea = r.removeFromLeft (kSideW);
        auto s = sideArea.reduced (Theme::space3);
        logoArea = s.removeFromTop (Theme::space5);
        s.removeFromTop (Theme::space2 + Theme::space1);
        auto navRow = s.removeFromBottom (Theme::buttonH);
        const int bw = (navRow.getWidth() - 2 * Theme::space2) / 3;
        for (auto* b : { &settings, &help, &sound })
        {
            b->setBounds (navRow.removeFromLeft (bw));
            navRow.removeFromLeft (Theme::space2);
        }
        s.removeFromBottom (Theme::space2 + Theme::space1);
        sidebar.setBounds (s);

        auto m = r.withTrimmedLeft (Theme::space4).withTrimmedRight (Theme::space4).withTrimmedTop (Theme::space4).withTrimmedBottom (Theme::space3);
        bottom.setBounds (m.removeFromBottom (BottomBar::height (true)));
        m.removeFromBottom (Theme::space3);

        auto top = m.removeFromTop (kTopH);
        voice.setBounds (top.removeFromLeft (kVoiceW));
        top.removeFromLeft (Theme::space3);
        mute.setBounds (top.removeFromLeft (kMuteW));
        top.removeFromLeft (Theme::space4);
        layoutMeters (top, false);
        m.removeFromTop (Theme::space3);

        const int cardsH = juce::jlimit (kCardsMin, kCardsMax, m.getHeight() - Theme::space3 - ChainRack::heightFor (2, false));
        auto cards = m.removeFromTop (cardsH);
        m.removeFromTop (Theme::space3);
        // mock: 280 + 280 | 232
        const int shifterW = juce::roundToInt (float (cards.getWidth() - Theme::space3) * 560.0f / 792.0f) + Theme::space3 / 2;
        shifter.setBounds (cards.removeFromLeft (shifterW));
        cards.removeFromLeft (Theme::space3);
        layers.setBounds (cards);
        rack.setBounds (m);
    }

    void layoutCompact (juce::Rectangle<int> r)
    {
        sideArea = {};
        auto m = r.reduced (Theme::space2 + Theme::space1);
        auto row1 = m.removeFromTop (Theme::controlH);
        for (auto* b : { &sound, &help, &settings })
        {
            const int w = b->preferredWidth();
            b->setBounds (row1.removeFromRight (w).withSizeKeepingCentre (w, Theme::touchMin));
            row1.removeFromRight (Theme::space2);
        }
        logoArea = row1.removeFromLeft (Theme::space4 + Theme::space2 + 2 + textWidth (Theme::ui (Theme::fontM, true), "KoeLoom"));
        row1.removeFromLeft (Theme::space3);
        const int pw = juce::jmin (row1.getWidth(), Theme::space5 * 8);
        picker.setBounds (row1.removeFromLeft (pw).withSizeKeepingCentre (pw, Theme::buttonH));
        m.removeFromTop (Theme::space2);

        bottom.setBounds (m.removeFromBottom (BottomBar::height (true)));
        m.removeFromBottom (Theme::space2);

        auto top = m.removeFromTop (kTopHCompact);
        voice.setBounds (top.removeFromLeft (Theme::space5 * 5 + Theme::space2)); // 168
        top.removeFromLeft (Theme::space2);
        mute.setBounds (top.removeFromLeft (Theme::space5 * 4 - Theme::space2));    // 120
        top.removeFromLeft (Theme::space3);
        layoutMeters (top, true);
        m.removeFromTop (Theme::space2);

        const int cardsH = juce::jlimit (kCardsMinCompact, kCardsMaxCompact, m.getHeight() - Theme::space2 - ChainRack::heightFor (1, true));
        auto cards = m.removeFromTop (cardsH);
        m.removeFromTop (Theme::space2);
        const int third = (cards.getWidth() - 2 * Theme::space2) / 3;
        shifter.setBounds (cards.removeFromLeft (third * 2 + Theme::space2));
        cards.removeFromLeft (Theme::space2);
        layers.setBounds (cards);
        rack.setBounds (m);
    }

    void layoutMeters (juce::Rectangle<int> area, bool cp)
    {
        const int h = LevelRow::height (cp);
        const int gap = juce::jlimit (0, Theme::space2, area.getHeight() - 2 * h);
        auto block = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), 2 * h + gap));
        input.setBounds (block.removeFromTop (h));
        block.removeFromTop (gap);
        output.setBounds (block.removeFromTop (h));
    }

    void showHelpMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, ja ("ガイドツアー"));
        m.addItem (2, ja ("Discord の設定手順"));
        m.addItem (3, ja ("元のマイクに戻す方法"));
        m.addItem (4, ja ("ライセンス表示"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (help), [&n = nav] (int r)
        {
            if (r == 1) n.startTour (false);
            else if (r == 2) n.showHelp (Navigator::HelpTopic::discordSetup);
            else if (r == 3) n.showHelp (Navigator::HelpTopic::revertMic);
            else if (r == 4) n.showHelp (Navigator::HelpTopic::licenses);
        });
    }

    AppController& c;
    Navigator& nav;
    bool compact = false, laidOut = false, layersStopped = false;
    int ticks = 0;
    PresetSidebar sidebar;
    PresetPicker picker;
    VoiceBlock voice;
    MuteBlock mute;
    LevelRow input, output;
    ShifterGroup shifter;
    LayersCard layers;
    ChainRack rack;
    BottomBar bottom;
    PillButton settings, help, sound;
    juce::Rectangle<int> sideArea, logoArea;
};
} // namespace

std::unique_ptr<VoicePage> makeMonoVoicePage (AppController& c, Navigator& nav) { return std::make_unique<MonoVoicePage> (c, nav); }
} // namespace koe::ui::mainui

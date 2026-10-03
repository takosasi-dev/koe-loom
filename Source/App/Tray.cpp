#include "App/Tray.h"

#include "UI/Icons.h"
#include "UI/Theme.h"

namespace koe
{
namespace
{
juce::Image makeTrayImage()
{
    juce::Image img (juce::Image::ARGB, 32, 32, true);
    juce::Graphics g (img);
    g.setColour (ui::Theme::dark().bg);
    g.fillRoundedRectangle (0.0f, 0.0f, 32.0f, 32.0f, 7.0f);
    ui::drawIcon (g, ui::Icon::logo, { 3.0f, 3.0f, 26.0f, 26.0f }, ui::Theme::dark().text, 2.8f, ui::Theme::dark().accent);
    return img;
}
} // namespace

TrayIcon::TrayIcon (AppController& c) : controller (c)
{
    const auto img = makeTrayImage();
    setIconImage (img, img);
    refresh();
}

void TrayIcon::refresh()
{
    juce::String t ("KoeLoom");
    t << juce::String::fromUTF8 (" — ボイチェン ") << (controller.isVoiceChangerOn() ? "ON" : "OFF");
    if (controller.isMicMuted()) t << " / MUTE";
    setIconTooltip (t);

    const auto news = newDangerNotices (controller.getNotices(), announced); // always, so turning it on never replays old ones
    if (! news.empty() && controller.getSettings().trayNotifications && ! juce::Process::isForegroundProcess())
        showInfoBubble ("KoeLoom", news.front().text);
}

std::vector<Notice> newDangerNotices (const std::vector<Notice>& now, juce::StringArray& seen)
{
    std::vector<Notice> fresh;
    juce::StringArray keys;
    for (auto& n : now)
    {
        if (n.level != NoticeLevel::danger) continue;
        keys.add (n.key);
        if (! seen.contains (n.key)) fresh.push_back (n);
    }
    seen = keys;
    return fresh;
}

void TrayIcon::mouseDoubleClick (const juce::MouseEvent&)
{
    if (controller.onShowWindowRequest) controller.onShowWindowRequest();
}

void TrayIcon::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && ! e.mods.isLeftButtonDown()) return;
    auto u8 = [] (const char* s) { return juce::String::fromUTF8 (s); };
    juce::PopupMenu m;
    m.addItem (u8 ("KoeLoom を開く"), [this] { controller.performAction ("show"); });
    m.addSeparator();
    m.addItem (u8 ("ボイチェン ON"), true, controller.isVoiceChangerOn(), [this] { controller.performAction ("voiceToggle"); refresh(); });
    m.addItem (u8 ("マイクミュート"), true, controller.isMicMuted(), [this] { controller.performAction ("muteToggle"); refresh(); });
    juce::PopupMenu fav;
    const auto favs = controller.getFavorites();
    for (int i = 0; i < favs.size(); ++i)
    {
        const auto* p = controller.getPresetLibrary().find (favs[i].toStdString());
        if (p == nullptr) continue;
        const bool isCurrent = controller.getCurrentPreset().id == p->id;
        fav.addItem (juce::String (i + 1) + "  " + p->name, true, isCurrent, [this, i] { controller.loadFavorite (i, true); });
    }
    if (favs.isEmpty()) fav.addItem (u8 ("お気に入りはまだありません"), false, false, nullptr);
    m.addSubMenu (u8 ("お気に入り"), fav);
    m.addSeparator();
    m.addItem (u8 ("終了"), [this] { controller.performAction ("quit"); });
    // the tray menu only dismisses correctly when the process is in the foreground (Windows)
    juce::Process::makeForegroundProcess();
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}
} // namespace koe

#include "Platform/Hotkeys.h"

#include "Core/Constants.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

const wchar_t* const kWindowClass = L"KoeLoomHotkeyWindow";

juce::String keyName (int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return juce::String::charToString (juce::juce_wchar (vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + juce::String (vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return u8 ("テンキー") + juce::String (vk - VK_NUMPAD0);

    switch (vk)
    {
        case VK_MULTIPLY: return u8 ("テンキー*");
        case VK_ADD: return u8 ("テンキー+");
        case VK_SUBTRACT: return u8 ("テンキー-");
        case VK_DECIMAL: return u8 ("テンキー.");
        case VK_DIVIDE: return u8 ("テンキー/");
        case VK_SEPARATOR: return u8 ("テンキー,");
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        case VK_BACK: return "BackSpace";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_LEFT: return "Left";
        case VK_UP: return "Up";
        case VK_RIGHT: return "Right";
        case VK_DOWN: return "Down";
        case VK_PAUSE: return "Pause";
        case VK_SCROLL: return "ScrollLock";
        case VK_SNAPSHOT: return "PrintScreen";
        case VK_NUMLOCK: return "NumLock";
        case VK_CAPITAL: return "CapsLock";
        case VK_APPS: return "Menu";
        case VK_CONVERT: return u8 ("変換");
        case VK_NONCONVERT: return u8 ("無変換");
        case VK_KANA: return u8 ("カナ");
        case VK_KANJI: return u8 ("漢字");
        case 0xF3: case 0xF4: return u8 ("半角/全角"); // VK_OEM_AUTO / VK_OEM_ENLW on JIS keyboards
        case VK_VOLUME_MUTE: return u8 ("音量ミュート");
        case VK_VOLUME_DOWN: return u8 ("音量-");
        case VK_VOLUME_UP: return u8 ("音量+");
        case VK_MEDIA_NEXT_TRACK: return u8 ("次の曲");
        case VK_MEDIA_PREV_TRACK: return u8 ("前の曲");
        case VK_MEDIA_STOP: return u8 ("停止");
        case VK_MEDIA_PLAY_PAUSE: return u8 ("再生/一時停止");
        default: break;
    }

    // Symbol keys: ask the current keyboard layout (a JIS keyboard's VK_OEM_1 is ':', a US one's is ';').
    if ((vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2)
    {
        const auto ch = MapVirtualKeyW (UINT (vk), MAPVK_VK_TO_CHAR) & 0x7fffu;
        if (ch != 0) return juce::String::charToString (juce::juce_wchar (ch));
    }
    return "0x" + juce::String::toHexString (vk).toUpperCase();
}
} // namespace

struct Hotkeys::Impl
{
    Hotkeys& owner;
    HWND hwnd = nullptr;
    juce::StringArray registered; // index + 1 = RegisterHotKey id

    explicit Impl (Hotkeys& o) : owner (o)
    {
        const auto instance = GetModuleHandleW (nullptr);
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW (&wc); // fails harmlessly when already registered

        // Message-only window created on the message thread: JUCE's loop (GetMessage/DispatchMessage)
        // hands WM_HOTKEY to wndProc on that same thread.
        hwnd = CreateWindowExW (0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (hwnd != nullptr) SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (this));
    }

    ~Impl()
    {
        unregisterAll();
        if (hwnd != nullptr) DestroyWindow (hwnd);
    }

    void unregisterAll()
    {
        if (hwnd != nullptr)
            for (int i = 0; i < registered.size(); ++i) UnregisterHotKey (hwnd, i + 1);
        registered.clear();
    }

    static LRESULT CALLBACK wndProc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_HOTKEY)
            if (auto* self = reinterpret_cast<Impl*> (GetWindowLongPtrW (h, GWLP_USERDATA)))
            {
                const auto action = self->registered[int (wp) - 1]; // copy: the handler may re-apply bindings
                if (action.isNotEmpty() && self->owner.onHotkey) self->owner.onHotkey (action);
                return 0;
            }
        return DefWindowProcW (h, msg, wp, lp);
    }
};

Hotkeys::Hotkeys() : impl (std::make_unique<Impl> (*this)) {}
Hotkeys::~Hotkeys() = default;

std::vector<HotkeyBinding> Hotkeys::apply (const std::vector<HotkeyBinding>& bindings)
{
    impl->unregisterAll();
    std::vector<HotkeyBinding> failed;
    for (auto& b : bindings)
    {
        if (b.virtualKey == 0) continue;
        const int id = impl->registered.size() + 1;
        const UINT mods = UINT (b.modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT;
        if (impl->hwnd != nullptr && RegisterHotKey (impl->hwnd, id, mods, UINT (b.virtualKey)))
            impl->registered.add (b.action);
        else
            failed.push_back (b);
    }
    return failed;
}

void Hotkeys::unregisterAll() { impl->unregisterAll(); }

juce::String Hotkeys::describe (int modifiers, int virtualKey)
{
    if (virtualKey == 0) return {};
    juce::StringArray parts;
    if (modifiers & MOD_CONTROL) parts.add ("Ctrl");
    if (modifiers & MOD_ALT) parts.add ("Alt");
    if (modifiers & MOD_SHIFT) parts.add ("Shift");
    if (modifiers & MOD_WIN) parts.add ("Win");
    parts.add (keyName (virtualKey));
    return parts.joinIntoString ("+");
}

juce::StringArray Hotkeys::allActions()
{
    juce::StringArray a { "voiceToggle", "muteToggle", "favoriteNext", "favoritePrev" };
    for (int i = 1; i <= kMaxFavorites; ++i) a.add ("favorite." + juce::String (i));
    for (int i = 1; i <= kMaxSlots; ++i) a.add ("slot." + juce::String (i));
    for (int i = 1; i <= kSoundboardSlots; ++i) a.add ("sound." + juce::String (i));
    for (int i = 1; i <= kSoundboardSlots; ++i) a.add ("soundStop." + juce::String (i));
    a.addArray (juce::StringArray { "soundStopAll", "freezeToggle", "looperRecPlay", "looperClear", "pushToTalk" });
    a.add ("recordToggle"); // wave 8 (INTERFACES.md §10)
    for (int i = 1; i <= kMomentarySlots; ++i) a.add ("momentary." + juce::String (i));
    return a;
}

bool Hotkeys::isKeyDown (int virtualKey)
{
    if (keyStateForTests) return keyStateForTests (virtualKey);
    return (GetAsyncKeyState (virtualKey) & 0x8000) != 0;
}

juce::String Hotkeys::actionLabel (const juce::String& action)
{
    const auto n = action.fromFirstOccurrenceOf (".", false, false);
    if (action == "voiceToggle") return u8 ("ボイチェン ON/OFF");
    if (action == "muteToggle") return u8 ("マイクミュート");
    if (action == "favoriteNext") return u8 ("次のお気に入り");
    if (action == "favoritePrev") return u8 ("前のお気に入り");
    if (action.startsWith ("favorite.")) return u8 ("お気に入り ") + n;
    if (action.startsWith ("slot.")) return u8 ("スロット ") + n + u8 (" の ON/OFF");
    if (action.startsWith ("sound.")) return u8 ("サウンド ") + n;
    if (action.startsWith ("soundStop.")) return u8 ("効果音 ") + n + u8 (" を止める");
    if (action == "soundStopAll") return u8 ("サウンドを全停止");
    if (action == "freezeToggle") return u8 ("フリーズの切替");
    if (action == "looperRecPlay") return u8 ("ルーパーの録音/再生");
    if (action == "looperClear") return u8 ("ルーパーの消去");
    if (action == "pushToTalk") return u8 ("プッシュトゥトーク（押している間）");
    if (action == "recordToggle") return u8 ("録音の開始 / 停止");
    if (action.startsWith ("momentary.")) return u8 ("押している間のエフェクト ") + n;
    return action;
}
} // namespace koe

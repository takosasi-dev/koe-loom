#include "Platform/AutoStart.h"

namespace koe::autostart
{
namespace
{
const char* const kRunValue = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\KoeLoom";
}

juce::String commandFor (const juce::File& exe) { return "\"" + exe.getFullPathName() + "\" --autostart"; }

static juce::String thisCommand() { return commandFor (juce::File::getSpecialLocation (juce::File::currentExecutableFile)); }

bool setEnabled (bool enabled, juce::String& error)
{
    if (enabled)
    {
        if (juce::WindowsRegistry::setValue (kRunValue, thisCommand())) return true;
        error = juce::String::fromUTF8 ("自動起動を登録できませんでした（レジストリに書き込めません）。");
        return false;
    }
    if (! juce::WindowsRegistry::valueExists (kRunValue) || juce::WindowsRegistry::deleteValue (kRunValue)) return true;
    error = juce::String::fromUTF8 ("自動起動を解除できませんでした（レジストリの値を消せません）。");
    return false;
}

/** True only when the Run value starts this exe (a value left by a moved or other copy counts as off). */
bool isEnabled() { return juce::WindowsRegistry::getValue (kRunValue).equalsIgnoreCase (thisCommand()); }
} // namespace koe::autostart

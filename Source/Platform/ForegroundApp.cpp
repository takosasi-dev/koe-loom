#include "Platform/ForegroundApp.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <iterator>

namespace koe::foreground
{
namespace
{
juce::String programOf (HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId (hwnd, &pid);
    if (pid == 0) return {};
    HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return {}; // e.g. an elevated program: unknown
    wchar_t path[MAX_PATH * 2] {};
    DWORD len = DWORD (std::size (path));
    const bool ok = QueryFullProcessImageNameW (h, 0, path, &len) != 0;
    CloseHandle (h);
    return ok ? juce::File (juce::String (path)).getFileName() : juce::String();
}

BOOL CALLBACK collect (HWND hwnd, LPARAM param)
{
    if (! IsWindowVisible (hwnd) || GetWindow (hwnd, GW_OWNER) != nullptr || GetWindowTextLengthW (hwnd) == 0) return TRUE;
    auto& out = *reinterpret_cast<juce::StringArray*> (param);
    const auto name = programOf (hwnd);
    if (name.isNotEmpty() && ! isSelf (name)) out.addIfNotAlreadyThere (name, true);
    return TRUE;
}
} // namespace

bool isSelf (const juce::String& fileName)
{
    return fileName.equalsIgnoreCase ("KoeLoom.exe")
           || fileName.equalsIgnoreCase (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFileName());
}

juce::String programInFront()
{
    if (programInFrontForTests) return programInFrontForTests();
    HWND hwnd = GetForegroundWindow();
    return hwnd != nullptr ? programOf (hwnd) : juce::String();
}

juce::StringArray visiblePrograms()
{
    juce::StringArray out;
    if (visibleProgramsForTests) out = visibleProgramsForTests();
    else EnumWindows (collect, reinterpret_cast<LPARAM> (&out));
    for (int i = out.size(); --i >= 0;)
        if (isSelf (out[i])) out.remove (i);
    out.removeDuplicates (true);
    out.sortNatural();
    return out;
}
} // namespace koe::foreground

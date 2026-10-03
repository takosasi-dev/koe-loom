#pragma once

#include <juce_core/juce_core.h>

namespace koe::paths
{
/** %APPDATA%\KoeLoom, or %KOELOOM_DATA_DIR% when set (tests and dev runs must set it). */
inline juce::File dataDir()
{
    auto env = juce::SystemStats::getEnvironmentVariable ("KOELOOM_DATA_DIR", {});
    auto dir = env.isNotEmpty() ? juce::File (env)
                                : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("KoeLoom");
    return dir;
}
inline juce::File settingsFile() { return dataDir().getChildFile ("settings.json"); }
inline juce::File presetsDir() { return dataDir().getChildFile ("presets"); }
inline juce::File soundboardFile() { return dataDir().getChildFile ("soundboard.json"); }
inline juce::File logsDir() { return dataDir().getChildFile ("logs"); }
} // namespace koe::paths

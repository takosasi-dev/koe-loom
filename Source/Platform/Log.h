#pragma once

// Log level and retention (S-03 詳細: logLevel / logKeepDays, INTERFACES.md §7.3).
// The app logs through juce::Logger (koeloom.log, a juce::FileLogger set up by Main.cpp). A plain
// juce::Logger::writeToLog() counts as "standard"; logging::write() tags a line as error or detail.

#include <juce_core/juce_core.h>

namespace koe::logging
{
enum Level { error = 0, standard = 1, detail = 2 }; // Settings::logLevel: 0 エラーだけ / 1 標準 / 2 詳細

void setLevel (int level) noexcept;
int getLevel() noexcept;
/** Writes the line if the current level includes it (errors always). */
void write (int level, const juce::String& message);

/** Sits in front of another logger and drops lines above the current level. */
class FilterLogger final : public juce::Logger
{
public:
    explicit FilterLogger (juce::Logger* target) : target (target) {}
    juce::Logger* getTarget() const noexcept { return target; }

protected:
    void logMessage (const juce::String& message) override;

private:
    juce::Logger* target;
};

/** Deletes the files directly in dir last modified more than keepDays days before now. Returns how many. */
int deleteOldFiles (const juce::File& dir, int keepDays, juce::Time now = juce::Time::getCurrentTime());
} // namespace koe::logging

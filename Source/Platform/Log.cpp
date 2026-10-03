#include "Platform/Log.h"

#include <atomic>

namespace koe::logging
{
namespace
{
std::atomic<int> currentLevel { standard };
thread_local int pendingLevel = standard; // level of the line being written on this thread

/** juce::Logger::logMessage is protected; a derived class may name it to call it on another logger. */
struct Access : juce::Logger
{
    static void forward (juce::Logger& l, const juce::String& m) { (l.*(&Access::logMessage)) (m); }
};
} // namespace

void setLevel (int level) noexcept { currentLevel.store (juce::jlimit (int (error), int (detail), level)); }
int getLevel() noexcept { return currentLevel.load(); }

void write (int level, const juce::String& message)
{
    if (level > getLevel()) return;
    pendingLevel = level;
    juce::Logger::writeToLog (message);
    pendingLevel = standard;
}

void FilterLogger::logMessage (const juce::String& message)
{
    if (pendingLevel <= getLevel() && target != nullptr) Access::forward (*target, message);
}

int deleteOldFiles (const juce::File& dir, int keepDays, juce::Time now)
{
    const auto cutoff = now - juce::RelativeTime::days (keepDays);
    int n = 0;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
        if (f.getLastModificationTime() < cutoff && f.deleteFile()) ++n;
    return n;
}
} // namespace koe::logging

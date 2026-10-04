#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace koe::tools
{
/** KoeLoom.exe --bench-presets [csvPath] (INTERFACES.md §11, owner wave9/stream): the CPU cost of every built-in preset,
    rendered offline through the full VoiceProcessor at 48 kHz / 480-sample blocks. Writes a CSV (default: bench.csv next
    to the exe). Returns 0 on success. Never opens a device, a window or the user's data. */
int benchPresets (const juce::String& csvPath);

/** One preset at one converter quality. Times are per 480-sample block, microseconds; each is the median over the runs
    (other programs share the CPU, so single runs jump). realtimePct = mean / block duration (10 ms) * 100. */
struct BenchRow
{
    std::string id;
    int quality = 0;
    double meanUs = 0.0, p95Us = 0.0, maxUs = 0.0, realtimePct = 0.0;
};

/** The measurement behind benchPresets: the first maxPresets built-in presets (0 = all) x qualities 0..2, `seconds` of the
    Calibrate input (referenceSpeechOrSynth, looped) after 0.5 s of warm-up that is not timed, `runs` times each. */
std::vector<BenchRow> runBench (int maxPresets, double seconds, int runs);

/** "id,quality,meanUs,p95Us,maxUs,realtimePct,cpu" and one line per row. */
juce::String benchCsv (const std::vector<BenchRow>& rows);
} // namespace koe::tools

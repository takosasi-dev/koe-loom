// 声の見える化 (INTERFACES.md §12.3). Owner: wave10/viz.
// Taps: input 4 (the device input, before anything) and output 2 (what goes to the virtual mic), attached by the first
// pollSpectrum() and detached ~1 s after the last one (the same scheme as pollPitch). The FFT runs here, on the message thread.

#include "App/AppController.h"

namespace koe
{
namespace
{
using Tap = VoiceProcessor::TapPoint;
constexpr int kVizIdleTicks = 30; // ~1 s of timer ticks without pollSpectrum() -> detach
} // namespace

bool AppController::pollSpectrum (std::array<float, kSpectrumBands>& inDb, std::array<float, kSpectrumBands>& outDb)
{
    auto& v = viz;
    v.idleTicks = 0;
    if (! v.attached)
    {
        if (v.inRing == nullptr)
        {
            v.inRing = std::make_unique<TapRing>();
            v.outRing = std::make_unique<TapRing>();
        }
        v.inRing->clear();
        v.outRing->clear();
        v.inSpectrum.reset();
        v.outSpectrum.reset();
        processor.setTap (Tap::input, 4, v.inRing.get());
        processor.setTap (Tap::output, 2, v.outRing.get());
        v.attached = true;
    }
    if (const auto* fake = VizData::testSpectra)
    {
        inDb = fake->in;
        outDb = fake->out;
        return true;
    }
    const double rate = processor.getSampleRate();
    const bool gotIn = v.inSpectrum.poll (*v.inRing, rate, inDb);
    const bool gotOut = v.outSpectrum.poll (*v.outRing, rate, outDb);
    return gotIn || gotOut;
}

void AppController::tickViz()
{
    auto& v = viz;
    if (v.attached && ++v.idleTicks > kVizIdleTicks) // nobody looks: no copying on the audio thread, no FFT
    {
        processor.setTap (Tap::input, 4, nullptr);
        processor.setTap (Tap::output, 2, nullptr);
        v.attached = false;
    }
}

void AppController::shutdownViz()
{
    auto& v = viz;
    if (! v.attached) return;
    processor.setTap (Tap::input, 4, nullptr);
    processor.setTap (Tap::output, 2, nullptr);
    v.attached = false;
}
} // namespace koe

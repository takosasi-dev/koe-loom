#pragma once

// 声の見える化 (INTERFACES.md §12). Owner: wave10/viz. AppController keeps one VizData as a private member; put this
// feature's state here (the tap rings, the analyser ...) so AppController.h stays untouched.

#include "Engine/Spectrum.h"
#include "Engine/VoiceAnalysis.h"

#include <memory>

namespace koe
{
struct VizData
{
    VizData() = default;
    VizData (const VizData&) = delete;
    VizData& operator= (const VizData&) = delete;

    // ---- input tap 4 / output tap 2, attached by pollSpectrum(), detached ~1 s after the last poll ----
    std::unique_ptr<TapRing> inRing, outRing; // kept until destruction: the audio thread may still be in push() after a detach
    SpectrumAnalyser inSpectrum, outSpectrum;
    bool attached = false;
    int idleTicks = 0;

    // ---- snapshots (no sound there) and tests: message thread only. Non-null: pollSpectrum() answers these. ----
    struct Spectra { SpectrumBands in {}, out {}; };
    static inline const Spectra* testSpectra = nullptr;
};
} // namespace koe

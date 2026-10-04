// 声の高さのメーター and 自分の声で音量合わせ (INTERFACES.md §10.3). Owner: wave8/analysis.
// Taps: input 1 / output 1 (pitch), input 2 (the calibration take). The measuring runs in a CalibrationJob.

#include "App/AppController.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
using Tap = VoiceProcessor::TapPoint;
juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
constexpr int kPitchIdleTicks = 30; // ~1 s of timer ticks without pollPitch() -> detach
} // namespace

// ============================================================================ 声の高さ
AppController::PitchReading AppController::pollPitch()
{
    auto& a = analysis;
    a.pitchIdleTicks = 0;
    if (! a.pitchAttached)
    {
        if (a.inRing == nullptr)
        {
            a.inRing = std::make_unique<TapRing>();
            a.outRing = std::make_unique<TapRing>();
        }
        a.inRing->clear();
        a.outRing->clear();
        a.inMeter.reset();
        a.outMeter.reset();
        processor.setTap (Tap::input, 1, a.inRing.get());
        processor.setTap (Tap::output, 1, a.outRing.get());
        a.pitchAttached = true;
    }
    const double rate = processor.getSampleRate();
    return { a.inMeter.poll (*a.inRing, rate), a.outMeter.poll (*a.outRing, rate) };
}

// ============================================================================ 音量合わせ
bool AppController::startCalibration (juce::String& whyNot)
{
    auto& a = analysis;
    if (a.phase == AnalysisData::Phase::recording || a.phase == AnalysisData::Phase::analysing)
    {
        whyNot = jp ("音量合わせの途中です。");
        return false;
    }
    if (a.retired != nullptr)
    {
        whyNot = jp ("前の測定を止めています。少し待ってから、もう一度押してください。");
        return false;
    }
    if (! ((engine != nullptr && engine->isRunning()) || AnalysisData::testDevicesRunning))
    {
        whyNot = jp ("入力デバイスが動いていません。設定で入力と出力を選んでください。");
        return false;
    }
    a.takeRate = processor.getSampleRate();
    a.takeReleasePending = false; // detached since at least the last user action
    a.take.start (int (std::ceil (kCalibrationSeconds * a.takeRate)));
    processor.setTap (Tap::input, 2, &a.take);
    a.takeAttached = true;
    a.phase = AnalysisData::Phase::recording;
    a.error = {};
    a.presetsAdjusted = 0;
    sendChangeMessage();
    return true;
}

void AppController::cancelCalibration()
{
    auto& a = analysis;
    if (a.takeAttached)
    {
        processor.setTap (Tap::input, 2, nullptr);
        a.takeAttached = false;
        a.takeReleasePending = true; // freed on the next tick: the audio thread may be inside push() right now
    }
    if (a.job != nullptr)
    {
        a.job->cancel(); // finishes the preset it is on; tickAnalysis() drops it then (nothing is stored)
        a.retired = std::move (a.job);
    }
    if (a.phase == AnalysisData::Phase::recording || a.phase == AnalysisData::Phase::analysing)
    {
        a.phase = AnalysisData::Phase::idle;
        sendChangeMessage();
    }
}

AppController::CalibrationState AppController::getCalibrationState() const
{
    const auto& a = analysis;
    CalibrationState s;
    s.phase = CalibrationState::Phase (int (a.phase));
    if (a.phase == AnalysisData::Phase::recording)
        s.progress = float (a.take.recorded()) / float (std::max (1, a.take.capacity()));
    else if (a.phase == AnalysisData::Phase::analysing && a.job != nullptr)
        s.progress = a.job->getProgress();
    else if (a.phase == AnalysisData::Phase::done)
        s.progress = 1.0f;
    s.presetsAdjusted = a.presetsAdjusted;
    s.error = a.error;
    return s;
}

void AppController::clearCalibration()
{
    updateSettings ([] (Settings& s)
    {
        s.calibratedTrimDb.clear();
        s.calibratedAt = {};
    });
    if (analysis.phase == AnalysisData::Phase::done || analysis.phase == AnalysisData::Phase::failed)
    {
        analysis.phase = AnalysisData::Phase::idle;
        sendChangeMessage();
    }
}

void AppController::tickAnalysis()
{
    auto& a = analysis;
    // the meter is only fed while someone looks at it
    if (a.pitchAttached && ++a.pitchIdleTicks > kPitchIdleTicks)
    {
        processor.setTap (Tap::input, 1, nullptr);
        processor.setTap (Tap::output, 1, nullptr);
        a.pitchAttached = false;
    }

    if (a.retired != nullptr && a.retired->isFinished()) a.retired.reset();
    if (a.takeReleasePending && ! a.takeAttached)
    {
        a.take.release();
        a.takeReleasePending = false;
    }

    auto fail = [this, &a] (const juce::String& why)
    {
        cancelCalibration();
        a.phase = AnalysisData::Phase::failed;
        a.error = why;
        sendChangeMessage();
    };

    if (a.phase == AnalysisData::Phase::recording)
    {
        const bool running = (engine != nullptr && engine->isRunning()) || AnalysisData::testDevicesRunning;
        if (! running)
            fail (jp ("入力デバイスが止まったため、音量合わせをやめました。"));
        else if (std::abs (processor.getSampleRate() - a.takeRate) > 1.0)
            fail (jp ("サンプリングレートが変わったため、音量合わせをやめました。もう一度始めてください。"));
        else if (a.take.full())
        {
            processor.setTap (Tap::input, 2, nullptr);
            a.takeAttached = false;
            a.takeReleasePending = true;
            auto voice = a.take.samples();
            const float gain = dsp::dbToGain (settings.inputGainDb); // what the voice changer hears
            for (auto& v : voice) v *= gain;
            if (const auto why = checkCalibrationTake (voice, a.takeRate); why.isNotEmpty())
                return fail (why);

            const auto* reference = library->find ("natural-asis");
            std::vector<Preset> presets;
            for (auto& p : library->all())
                if (p.builtin && (AnalysisData::testPresetIds.empty()
                                  || std::find (AnalysisData::testPresetIds.begin(), AnalysisData::testPresetIds.end(), p.id) != AnalysisData::testPresetIds.end()))
                    presets.push_back (p);
            if (reference == nullptr || presets.empty())
                return fail (jp ("内蔵プリセットが見つからないため、測れませんでした。"));
            a.job = std::make_unique<CalibrationJob> (std::move (voice), a.takeRate, *reference, std::move (presets));
            a.job->start();
            a.phase = AnalysisData::Phase::analysing;
            sendChangeMessage();
        }
    }
    else if (a.phase == AnalysisData::Phase::analysing && a.job != nullptr && a.job->isFinished())
    {
        const auto trims = a.job->getTrims();
        a.job.reset();
        int adjusted = 0;
        for (auto& [id, trim] : trims)
            if (const auto* p = library->find (id.toStdString()); p != nullptr && std::abs (trim - p->outputTrimDb) >= 0.1f) ++adjusted;
        updateSettings ([&trims] (Settings& s)
        {
            s.calibratedTrimDb = trims;
            s.calibratedAt = juce::Time::getCurrentTime().toISO8601 (true);
        });
        a.presetsAdjusted = adjusted;
        a.phase = AnalysisData::Phase::done;
        sendChangeMessage();
    }
}

void AppController::shutdownAnalysis()
{
    auto& a = analysis;
    if (a.pitchAttached)
    {
        processor.setTap (Tap::input, 1, nullptr);
        processor.setTap (Tap::output, 1, nullptr);
        a.pitchAttached = false;
    }
    if (a.takeAttached)
    {
        processor.setTap (Tap::input, 2, nullptr);
        a.takeAttached = false;
    }
    a.job.reset();     // signals and waits (at most the preset it is on)
    a.retired.reset();
    if (a.phase == AnalysisData::Phase::recording || a.phase == AnalysisData::Phase::analysing) a.phase = AnalysisData::Phase::idle;
}
} // namespace koe

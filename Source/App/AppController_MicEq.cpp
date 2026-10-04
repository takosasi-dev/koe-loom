// マイクの癖の補正 (INTERFACES.md §11.3). Owner: wave9/voice.
// Measuring: input tap 3 records kMicEqSeconds (like 音量合わせ in AppController_Analysis.cpp), a MicEqJob works out the
// gains in the background. Applying: a MicEqFilter set as the processor's input filter while micEqOn and measured.

#include "App/AppController.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
using Tap = VoiceProcessor::TapPoint;
constexpr int kTapIndex = 3; // INTERFACES.md §11.1: input tap 3 is ours
juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
bool wantsMicEq (const Settings& s) { return s.micEqOn && int (s.micEqGainsDb.size()) == kMicEqBands; }
} // namespace

bool AppController::startMicEqMeasure (juce::String& whyNot)
{
    auto& m = micEq;
    if (m.phase == MicEqData::Phase::recording || m.phase == MicEqData::Phase::analysing)
    {
        whyNot = jp ("マイク補正の測定の途中です。");
        return false;
    }
    if (m.retired != nullptr)
    {
        whyNot = jp ("前の測定を止めています。少し待ってから、もう一度押してください。");
        return false;
    }
    if (analysis.phase == AnalysisData::Phase::recording || analysis.phase == AnalysisData::Phase::analysing)
    {
        whyNot = jp ("音量合わせの途中です。終わってから測ってください。");
        return false;
    }
    if (! ((engine != nullptr && engine->isRunning()) || MicEqData::testDevicesRunning))
    {
        whyNot = jp ("入力デバイスが動いていません。設定で入力と出力を選んでください。");
        return false;
    }
    m.takeRate = processor.getSampleRate();
    m.takeReleasePending = false; // detached since at least the last user action
    m.take.start (int (std::ceil (kMicEqSeconds * m.takeRate)));
    processor.setTap (Tap::input, kTapIndex, &m.take);
    m.takeAttached = true;
    m.phase = MicEqData::Phase::recording;
    m.error = {};
    sendChangeMessage();
    return true;
}

void AppController::cancelMicEqMeasure()
{
    auto& m = micEq;
    if (m.takeAttached)
    {
        processor.setTap (Tap::input, kTapIndex, nullptr);
        m.takeAttached = false;
        m.takeReleasePending = true; // freed on the next tick: the audio thread may be inside push() right now
    }
    if (m.job != nullptr)
    {
        m.job->cancel();
        m.retired = std::move (m.job);
    }
    if (m.phase == MicEqData::Phase::recording || m.phase == MicEqData::Phase::analysing)
    {
        m.phase = MicEqData::Phase::idle;
        sendChangeMessage();
    }
}

AppController::MicEqState AppController::getMicEqState() const
{
    const auto& m = micEq;
    MicEqState s;
    s.phase = MicEqState::Phase (int (m.phase));
    if (m.phase == MicEqData::Phase::recording)
        s.progress = float (m.take.recorded()) / float (std::max (1, m.take.capacity()));
    else if (m.phase == MicEqData::Phase::done)
        s.progress = 1.0f;
    s.error = m.error;
    return s;
}

void AppController::setMicEqOn (bool on)
{
    updateSettings ([on] (Settings& s) { s.micEqOn = on; });
    applyMicEq();
}

void AppController::clearMicEq()
{
    updateSettings ([] (Settings& s)
    {
        s.micEqOn = false;
        s.micEqGainsDb.clear();
        s.micEqAt = {};
    });
    applyMicEq();
    if (micEq.phase == MicEqData::Phase::done || micEq.phase == MicEqData::Phase::failed)
    {
        micEq.phase = MicEqData::Phase::idle;
        sendChangeMessage();
    }
}

void AppController::applyMicEq()
{
    auto& m = micEq;
    if (! wantsMicEq (settings))
    {
        if (m.filter != nullptr) m.filter->setEnabled (false); // fades out; tickMicEq takes it out of the path then
        return;
    }
    if (m.filter == nullptr) m.filter = std::make_unique<MicEqFilter>();
    const double rate = processor.getSampleRate();
    if (settings.micEqGainsDb != m.builtGains || rate != m.builtRate) // a new measurement, or the device reopened at another rate
    {
        m.filter->setGains (settings.micEqGainsDb, rate);
        m.builtGains = settings.micEqGainsDb;
        m.builtRate = rate;
    }
    m.filter->setEnabled (true);
    if (! m.attached)
    {
        processor.setInputFilter (m.filter.get());
        m.attached = true;
    }
}

void AppController::tickMicEq()
{
    auto& m = micEq;
    // the settings can change elsewhere (updateSettings, import): follow them
    const bool want = wantsMicEq (settings);
    if ((want && (! m.attached || settings.micEqGainsDb != m.builtGains || processor.getSampleRate() != m.builtRate))
        || (! want && m.attached))
        applyMicEq();
    if (m.filter != nullptr)
    {
        if (m.attached && ! want && m.filter->isIdle())
        {
            processor.setInputFilter (nullptr); // faded out: out of the path, the default sound bit for bit
            m.attached = false;
        }
        m.filter->collectGarbage();
    }

    if (m.retired != nullptr && m.retired->isFinished()) m.retired.reset();
    if (m.takeReleasePending && ! m.takeAttached)
    {
        m.take.release();
        m.takeReleasePending = false;
    }

    auto fail = [this, &m] (const juce::String& why)
    {
        cancelMicEqMeasure();
        m.phase = MicEqData::Phase::failed;
        m.error = why;
        sendChangeMessage();
    };

    if (m.phase == MicEqData::Phase::recording)
    {
        const bool running = (engine != nullptr && engine->isRunning()) || MicEqData::testDevicesRunning;
        if (! running)
            fail (jp ("入力デバイスが止まったため、測定をやめました。"));
        else if (std::abs (processor.getSampleRate() - m.takeRate) > 1.0)
            fail (jp ("サンプリングレートが変わったため、測定をやめました。もう一度始めてください。"));
        else if (m.take.full())
        {
            processor.setTap (Tap::input, kTapIndex, nullptr);
            m.takeAttached = false;
            m.takeReleasePending = true;
            auto voice = m.take.samples();
            const float gain = dsp::dbToGain (settings.inputGainDb); // what the filter hears (it sits after the input gain)
            for (auto& v : voice) v *= gain;
            if (const auto why = checkCalibrationTake (voice, m.takeRate); why.isNotEmpty())
                return fail (why);
            m.job = std::make_unique<MicEqJob> (std::move (voice), m.takeRate);
            m.job->start();
            m.phase = MicEqData::Phase::analysing;
            sendChangeMessage();
        }
    }
    else if (m.phase == MicEqData::Phase::analysing && m.job != nullptr && m.job->isFinished())
    {
        const auto gains = m.job->getGains();
        const auto error = m.job->getError();
        m.job.reset();
        if (gains.empty()) return fail (error);
        updateSettings ([&gains] (Settings& s)
        {
            s.micEqGainsDb = gains;
            s.micEqAt = juce::Time::getCurrentTime().toISO8601 (true);
            s.micEqOn = true;
        });
        applyMicEq();
        m.phase = MicEqData::Phase::done;
        sendChangeMessage();
    }
}

void AppController::shutdownMicEq()
{
    auto& m = micEq;
    if (m.takeAttached)
    {
        processor.setTap (Tap::input, kTapIndex, nullptr);
        m.takeAttached = false;
    }
    if (m.attached)
    {
        processor.setInputFilter (nullptr); // the filter itself lives until the controller goes (after the devices close)
        m.attached = false;
    }
    m.job.reset();     // signals and waits
    m.retired.reset();
    if (m.phase == MicEqData::Phase::recording || m.phase == MicEqData::Phase::analysing) m.phase = MicEqData::Phase::idle;
}
} // namespace koe

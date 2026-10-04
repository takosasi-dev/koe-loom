// 試し録り and WAV recording (INTERFACES.md §10.3). Owner: wave8/capture.
// Ports: input tap 0 (the take records the raw device input), the input source (the take loops in its place), output tap 0
// (the WAV gets what goes to the virtual mic) and setOutputMuted (the virtual mic is silent while the take plays).

#include "App/AppController.h"
#include "Core/Paths.h"

#include <cmath>
#include <utility>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
bool rateDiffers (double a, double b) { return std::abs (a - b) > 1.0; }
} // namespace

// ============================================================================ 試し録り
bool AppController::startTestTake (juce::String& whyNot)
{
    auto& cap = capture;
    if (! CaptureData::assumeDevicesRunningForTests && ! (engine != nullptr && engine->isRunning()))
    {
        whyNot = u8 ("入力デバイスが動いていません。設定のデバイスで入力と出力を選んでください。");
        return false;
    }
    if (cap.take != nullptr && cap.take->isPlaying())
    {
        whyNot = u8 ("再生を止めてから録ってください。");
        return false;
    }
    if (cap.take != nullptr && cap.take->isRecording()) return true;
    processor.setTap (VoiceProcessor::TapPoint::input, 0, nullptr);
    cap.retire (std::move (cap.take)); // the old take is gone
    cap.take = std::make_unique<TakeRecorder> (processor.getSampleRate(), kTestTakeMaxSeconds); // allocates here, not on the audio thread
    processor.setTap (VoiceProcessor::TapPoint::input, 0, cap.take.get());
    cap.takeTapOn = true;
    sendChangeMessage();
    return true;
}

void AppController::stopTestTake()
{
    auto& cap = capture;
    if (cap.take == nullptr) return;
    if (cap.takeTapOn)
    {
        cap.take->stopRecording();
        processor.setTap (VoiceProcessor::TapPoint::input, 0, nullptr);
        cap.takeTapOn = false;
        if (cap.take->getLength() == 0) cap.retire (std::move (cap.take));
    }
    else if (cap.take->isPlaying())
    {
        cap.take->stopPlayback();
        processor.setInputSource (nullptr);
        processor.setOutputMuted (false);
        removeNotice ("take.playing");
        if (std::exchange (cap.takeTurnedMonitorOn, false) && monitorOn) setMonitorOn (false);
    }
    sendChangeMessage();
}

bool AppController::playTestTake (juce::String& whyNot)
{
    auto& cap = capture;
    if (cap.take != nullptr && cap.take->isPlaying()) return true;
    if (cap.take == nullptr || cap.take->getLength() == 0)
    {
        whyNot = u8 ("先に試し録りをしてください。");
        return false;
    }
    if (! CaptureData::assumeDevicesRunningForTests && ! (engine != nullptr && engine->isRunning()))
    {
        whyNot = u8 ("オーディオデバイスが動いていません。設定のデバイスで入力と出力を選んでください。");
        return false;
    }
    if (micMuted)
    {
        whyNot = u8 ("マイクミュート中は試し録りを聞けません。ミュートを解除してください。");
        return false;
    }
    if (settings.monitorDevice.isEmpty())
    {
        whyNot = u8 ("モニターの出力先を設定してください（設定のデバイス）。試し録りはモニターで聞きます。");
        return false;
    }
    if (cap.takeTapOn) stopTestTake(); // stops recording, keeps the take
    if (! isMonitorOn())
    {
        setMonitorOn (true);
        if (! isMonitorOn())
        {
            whyNot = u8 ("モニターを ON にできませんでした。");
            return false;
        }
        cap.takeTurnedMonitorOn = true;
    }
    processor.setOutputMuted (true); // the other side never hears the take
    cap.take->startPlayback();
    processor.setInputSource (cap.take.get());
    addNotice ({ "take.playing", NoticeLevel::info, u8 ("試し録りを再生しています（相手には送っていません）"), false, u8 ("止める"), "takeStop" });
    sendChangeMessage();
    return true;
}

void AppController::clearTestTake()
{
    stopTestTake();
    capture.retire (std::move (capture.take));
    sendChangeMessage();
}

AppController::TakeState AppController::getTestTakeState() const
{
    const auto* t = capture.take.get();
    if (t == nullptr) return TakeState::empty;
    if (t->isPlaying()) return TakeState::playing;
    if (capture.takeTapOn && t->isRecording()) return TakeState::recording;
    return t->getLength() > 0 ? TakeState::ready : TakeState::empty;
}

float AppController::getTestTakeSeconds() const
{
    const auto* t = capture.take.get();
    return t != nullptr ? float (t->getLength() / t->getSampleRate()) : 0.0f;
}

float AppController::getTestTakePosition() const
{
    const auto* t = capture.take.get();
    return t != nullptr && t->isPlaying() ? float (t->getPlayPosition() / t->getSampleRate()) : 0.0f;
}

// ============================================================================ WAV 録音
bool AppController::startWavRecording (juce::String& whyNot)
{
    auto& cap = capture;
    if (cap.wav != nullptr) return true;
    if (! CaptureData::assumeDevicesRunningForTests && ! (engine != nullptr && engine->isRunning()))
    {
        whyNot = u8 ("オーディオデバイスが動いていません。設定のデバイスで入力と出力を選んでください。");
        return false;
    }
    const auto dir = paths::recordingsDir();
    const auto name = "KoeLoom " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    auto file = dir.getChildFile (name + ".wav");
    for (int i = 2; file.exists(); ++i) file = dir.getChildFile (name + " (" + juce::String (i) + ").wav");
    cap.wav = WavRecorder::create (file, processor.getSampleRate(), whyNot);
    if (cap.wav == nullptr) return false;
    if (std::exchange (CaptureData::failNextWavWriteForTests, false)) cap.wav->failNextWriteForTests();
    processor.setTap (VoiceProcessor::TapPoint::output, 0, cap.wav.get());
    removeNotice ("wav.failed");
    addNotice ({ "wav.recording", NoticeLevel::info, u8 ("録音しています"), false, u8 ("止める"), "wavStop" });
    sendChangeMessage();
    return true;
}

void AppController::stopWavRecording()
{
    auto& cap = capture;
    if (cap.wav == nullptr) return;
    processor.setTap (VoiceProcessor::TapPoint::output, 0, nullptr);
    cap.wav->finish(); // writes the rest and closes the file (tickCapture reports a failure before calling this)
    if (cap.wav->getSeconds() > 0.0) cap.lastWav = cap.wav->getFile();
    else cap.wav->getFile().deleteFile(); // nothing recorded: no empty file left behind
    cap.retire (std::move (cap.wav));
    removeNotice ("wav.recording");
    sendChangeMessage();
}

bool AppController::isWavRecording() const { return capture.wav != nullptr; }
double AppController::getWavRecordingSeconds() const { return capture.wav != nullptr ? capture.wav->getSeconds() : 0.0; }
juce::File AppController::getLastWavFile() const { return capture.lastWav; }

// ============================================================================ timer / shutdown
void AppController::tickCapture()
{
    auto& cap = capture;
    for (auto it = cap.retired.begin(); it != cap.retired.end();)
        it = --it->ticksLeft <= 0 ? cap.retired.erase (it) : it + 1;

    const double rate = processor.getSampleRate();
    const bool running = CaptureData::assumeDevicesRunningForTests || (engine != nullptr && engine->isRunning());
    if (cap.take != nullptr)
    {
        if (rateDiffers (cap.take->getSampleRate(), rate)) // the devices were reopened at another rate
        {
            clearTestTake();
            toast (u8 ("サンプルレートが変わったため、試し録りを消しました。録り直してください。"));
        }
        else if (cap.takeTapOn && cap.take->isFull())
        {
            processor.setTap (VoiceProcessor::TapPoint::input, 0, nullptr);
            cap.takeTapOn = false;
            toast (juce::String (int (kTestTakeMaxSeconds)) + u8 (" 秒になったので、試し録りを止めました。"));
            sendChangeMessage();
        }
        else if (cap.take->isPlaying() && (! running || ! isMonitorOn()))
        {
            stopTestTake();
            toast (running ? u8 ("モニターが止まったため、試し録りの再生を止めました。")
                           : u8 ("オーディオデバイスが止まったため、試し録りの再生を止めました。"));
        }
    }

    if (cap.wav != nullptr)
    {
        const auto failure = cap.wav->getFailure();
        const auto file = cap.wav->getFile();
        if (failure != WavRecorder::Failure::none)
        {
            stopWavRecording();
            juce::String text;
            if (failure == WavRecorder::Failure::overflow)
                text = u8 ("書き込みが追いつかなかったため、録音を止めました。");
            else if (file.getBytesFreeOnVolume() < 64 * 1024 * 1024)
                text = u8 ("ディスクの空きが足りないため、録音を止めました。");
            else
                text = u8 ("ファイルに書き込めなかったため、録音を止めました。");
            if (cap.lastWav == file) text << u8 ("（そこまでは ") << file.getFileName() << u8 (" に残っています）");
            addNotice ({ "wav.failed", NoticeLevel::warning, text });
            sendChangeMessage();
        }
        else if (rateDiffers (cap.wav->getSampleRate(), rate))
        {
            stopWavRecording();
            toast (u8 ("サンプルレートが変わったため、録音を止めました。"));
        }
    }
}

void AppController::shutdownCapture()
{
    stopTestTake();
    stopWavRecording(); // closes the file before the app ends
    processor.setTap (VoiceProcessor::TapPoint::input, 0, nullptr);
    processor.setTap (VoiceProcessor::TapPoint::output, 0, nullptr);
    processor.setInputSource (nullptr);
}
} // namespace koe

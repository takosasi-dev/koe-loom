#pragma once

#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Dsp/IVoiceShifter.h"
#include "Dsp/Limiter.h"
#include "Dsp/NoiseGate.h"
#include "Dsp/NoiseSuppressor.h"
#include "Engine/EffectChain.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace koe
{
/** Extra audio mixed after the voice (soundboard, test tone). Audio thread; add into the buffers. */
class IAuxSource
{
public:
    virtual ~IAuxSource() = default;
    /** Add this block's samples into toOutput (virtual mic) and toMonitor (monitor only mix). */
    virtual void render (float* toOutput, float* toMonitor, int numSamples) = 0;
    /** Gain to apply to the voice while this source plays (ducking, F-06-7). 1 = none. */
    virtual float voiceDuckGain() const { return 1.0f; }
};

/** Receives the monitor mix once per block (audio thread). */
class IMonitorSink
{
public:
    virtual ~IMonitorSink() = default;
    virtual void push (const float* samples, int numSamples) = 0;
};

/** Pitch provider for scale-locked layers (Phase 5, F-02-11). Audio thread. */
class IScaleLayerPitch
{
public:
    virtual ~IScaleLayerPitch() = default;
    /** Analyse the voice input block (before the shifters). */
    virtual void analyse (const float* voice, int numSamples) = 0;
    /** Semitone shift for a layer, or NaN when no pitch is detected (layer fades to silence, E-28). */
    virtual float layerSemitones (int key, bool minor, int degree) const = 0;
};

/** Receives the samples at one point of the path (INTERFACES.md §10.1). Audio thread: no allocation, lock or I/O. */
class IAudioTap
{
public:
    virtual ~IAudioTap() = default;
    virtual void push (const float* samples, int numSamples) = 0;
};

/** Stands in for the device input (試し録り playback). Audio thread. Return false to use the device input this block. */
class IInputSource
{
public:
    virtual ~IInputSource() = default;
    virtual bool render (float* dest, int numSamples) = 0;
};

/** Processes the voice after the voice changer path (shifter, layers, chain, trim; or the dry voice when it is OFF)
    and before ducking / soundboard / output gain (押している間だけのエフェクト). Audio thread, in place. */
class IVoicePostProcessor
{
public:
    virtual ~IVoicePostProcessor() = default;
    virtual void process (float* samples, int numSamples) = 0;
};

/**
    The whole signal path of spec §5.2, independent of any audio device so it can be rendered offline
    (spec §9.4). All setters are thread-safe (atomics) and take effect smoothly; process() is the only
    audio-thread entry point. Mono in, stereo (identical L/R, no -3 dB, F-12-2) out.
*/
class VoiceProcessor
{
public:
    VoiceProcessor();
    ~VoiceProcessor();

    /** Shifter implementation for the main voice and the layers (default: createConverterShifter at the converter
        quality). Call before prepare(). A factory set here ignores the converter quality. */
    void setShifterFactory (std::function<std::unique_ptr<IVoiceShifter>()> factory);
    /** Message thread. Converter quality 0..2 (S-03 詳細, createConverterShifter). After prepare() this builds a new
        set of converters (main + layers) and hands it to the audio thread, which warms it up and crossfades to it
        (click-free, allocation-free). The reported latency follows at once. */
    void setConverterQuality (int quality);
    int getConverterQuality() const noexcept { return quality; }

    /** Allocates everything. Not real-time safe. Also starts the 100 ms fade-in (F-09-5). */
    void prepare (double sampleRate, int maxBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }
    /** Chains handed to requestChain() must be created with this block size. */
    int getMaxBlockSize() const noexcept { return maxBlock; }

    /** Audio thread. Any numSamples (internally split to maxBlockSize). outR may be nullptr. */
    void process (const float* in, float* outL, float* outR, int numSamples);

    // ---- environment (F-03, F-12) ----
    void setInputGainDb (float db) noexcept { inputGainDb.store (kInputGainDb.clamp (db)); }
    void setNoiseSuppression (bool on, float mix) noexcept { noiseOn.store (on); noiseMix.store (kNoiseMix.clamp (mix)); }
    void setGate (bool on, float thresholdDb, float attackMs, float holdMs, float releaseMs) noexcept;
    void setOutputGainDb (float db) noexcept { outputGainDb.store (kOutputGainDb.clamp (db)); }

    // ---- detailed settings (S-03 「詳細な設定」, INTERFACES.md §7) ----
    /** Input low cut (12 dB/oct) after the input gain, before noise suppression. Crossfaded on toggle, cutoff glides. */
    void setHighPass (bool on, float hz) noexcept { highPassHz.store (kHighPassHz.clamp (hz)); highPassOn.store (on); }
    /** Automatic input level after noise suppression, before the gate: slow (rises ~2 s, falls ~0.3 s), up to
        maxGainDb of boost and 24 dB of cut, holds while the input is below -50 dBFS RMS. */
    void setAgc (bool on, float targetDb, float maxGainDb) noexcept;
    /** Output limiter ceiling (glides over ~10 ms) and release. */
    void setLimiter (float ceilingDb, float releaseMs) noexcept;
    /** Length of the chain swap crossfade (F-04-6, default 30 ms) for the next requestChain(). */
    void setChainCrossfadeMs (float ms) noexcept { chainFadeMs.store (kPresetCrossfadeMs.clamp (ms)); }

    // ---- voice (F-02, F-08-8) ----
    void setVoiceChangerOn (bool on) noexcept { voiceOn.store (on); }
    void setMicMute (bool on) noexcept { micMute.store (on); }
    /** hasShifter false = 変換 OFF (R-P3): the main voice is the delayed dry signal regardless of pitch/formant. */
    void setShifter (bool hasShifter, float pitchSt, float formantSt) noexcept;
    void setTrimDb (float db) noexcept { trimDb.store (kTrimDb.clamp (db)); }

    struct LayerParams
    {
        bool active = false;
        bool scale = false;  // scale mode (Phase 5)
        float pitchSt = 0.0f, formantSt = 0.0f, levelDb = -6.0f;
        int key = 0, degree = 2;
        bool minor = false;
    };
    void setLayer (int index, const LayerParams& p) noexcept;
    /** Watchdog stop (F-02-8): stays stopped until clearLayerAutoStop(). */
    void autoStopLayers() noexcept;
    bool areLayersAutoStopped() const noexcept { return layersAutoStopped.load(); }
    /** Audio thread (watchdog): is any layer converter currently processing? */
    bool anyLayerRunning() const noexcept;
    void clearLayerAutoStop() noexcept { layersAutoStopped.store (false); }
    void setScaleLayerPitch (IScaleLayerPitch* provider) noexcept { scalePitch.store (provider); }

    // ---- chain (F-04-6) ----
    /** Message thread. The new chain fades in over 30 ms; a still-pending previous request is discarded (E-20). */
    void requestChain (std::unique_ptr<EffectChain> chain);
    /** Message thread: delete chains the audio thread has finished with. Call from a timer. */
    void collectGarbage();
    /** The most recently requested chain (message thread view). Valid until the next requestChain(). */
    EffectChain* getRequestedChain() const noexcept { return requested; }
    /** The chain the audio thread is currently running (for the watchdog, audio thread). */
    EffectChain* getActiveChainAudio() const noexcept { return active; }

    // ---- aux / monitor ----
    void setAuxSource (IAuxSource* src) noexcept { aux.store (src); }
    void setMonitorSink (IMonitorSink* sink) noexcept { monitor.store (sink); }

    // ---- wave 8 hooks (INTERFACES.md §10.1). Message thread sets, audio thread reads; nullptr = none. ----
    enum class TapPoint { input, output, sent };
    static constexpr int kTapsPerPoint = 6; // wave 10 (INTERFACES.md §12): input 4 and output 2 are 声の見える化's
    /** input: the device input as it arrives (before the input source, gain and everything else; may hold non-finite
        samples). output: what goes to the virtual mic (after the limiter and fade-in, before setOutputMuted).
        sent (wave 9, INTERFACES.md §11): exactly what the virtual mic gets (after setOutputMuted). */
    void setTap (TapPoint point, int index, IAudioTap* tap) noexcept
    {
        auto& taps = point == TapPoint::input ? inputTaps : (point == TapPoint::output ? outputTaps : sentTaps);
        taps[size_t (index)].store (tap, std::memory_order_release);
    }
    void setInputSource (IInputSource* src) noexcept { inputSource.store (src, std::memory_order_release); }
    void setPostProcessor (IVoicePostProcessor* p) noexcept { postProcessor.store (p, std::memory_order_release); }
    /** Silences the virtual mic only (30 ms fade); the monitor and the output tap keep the sound. */
    void setOutputMuted (bool muted) noexcept { outputMuted.store (muted); }
    /** wave 9 (マイクの癖の補正, INTERFACES.md §11): processes the input in place after the input gain and low cut, before
        noise suppression (so everything after, monitor included, hears the corrected voice). Audio thread. */
    void setInputFilter (IVoicePostProcessor* f) noexcept { inputFilter.store (f, std::memory_order_release); }

    // ---- state for the UI ----
    struct MeterValues { float inputPeak = 0, outputPeak = 0; bool inputClip = false, outputClip = false; };
    /** Peak since the last call (linear), clip flags since the last call. */
    MeterValues fetchMeters() noexcept;
    bool isGateOpen() const noexcept { return gateOn.load() ? gate.isOpen() : true; }
    bool fetchLimiterActive() noexcept { return limiter.fetchAndClearActive(); }
    /** Algorithm latency (samples): limiter + noise suppression (if on) + shifter and chain (if voice on).
        Message thread view: follows setConverterQuality() and requestChain() at once. */
    int getLatencySamples() const noexcept;
    int getShifterLatencySamples() const noexcept { return shifterLatency; }
    long long getNonFiniteInputCount() const noexcept { return nanInputs.load(); }
    /** Restart the 100 ms fade-in (device change, F-09-5). */
    void triggerFadeIn() noexcept { fadeInRequest.store (true); }

    // ---- tests ----
    void setTestBusyMicros (int us) noexcept { testBusyMicros.store (us); }

private:
    /** The converters of one quality (main voice + layers) and their audio-thread state. Built on the message
        thread (makeShifterSet), run and swapped on the audio thread. */
    struct ShifterSet
    {
        std::unique_ptr<IVoiceShifter> main;
        std::array<std::unique_ptr<IVoiceShifter>, kMaxLayers> layers;
        int latency = 0;
        bool mainRunning = false;
        int mainWarmLeft = 0;
        dsp::Ramp mainShiftMix;
        std::array<bool, kMaxLayers> layerRunning {};
        std::array<int, kMaxLayers> layerWarmLeft {};
        std::array<dsp::Ramp, kMaxLayers> layerGain;
    };
    /** Per-block voice parameters, read once and shared by the running and the incoming converter set. */
    struct VoiceBlock
    {
        bool desired = false;
        float pitch = 0.0f, formant = 0.0f;
        struct Layer { bool act = false, cut = false; float pitch = 0.0f, formant = 0.0f, gain = 0.0f; };
        std::array<Layer, kMaxLayers> layers {};
    };

    void processBlock (const float* in, float* outL, float* outR, int n);
    void processVoicePath (float* x, int n);
    std::unique_ptr<ShifterSet> makeShifterSet (int q) const;
    void renderShifters (ShifterSet& s, const float* x, float* out, int n, const VoiceBlock& v);
    bool retireCurrentSet() noexcept;

    std::function<std::unique_ptr<IVoiceShifter> (int)> shifterFactory;
    double sampleRate = kSampleRate;
    int maxBlock = kMaxBlockSize;
    int quality = 1;          // message thread
    bool prepared = false;    // message thread

    // ---- control atomics ----
    std::atomic<float> inputGainDb { 0.0f }, noiseMix { 1.0f }, outputGainDb { 0.0f }, trimDb { 0.0f };
    std::atomic<bool> noiseOn { false }, gateOn { false }, micMute { false }, voiceOn { true };
    std::atomic<bool> hasShifter { false };
    std::atomic<float> pitchSt { 0.0f }, formantSt { 0.0f };
    struct LayerAtomics
    {
        std::atomic<bool> active { false }, scale { false }, minor { false };
        std::atomic<float> pitchSt { 0.0f }, formantSt { 0.0f }, levelDb { -6.0f };
        std::atomic<int> key { 0 }, degree { 2 };
    };
    std::array<LayerAtomics, kMaxLayers> layerCtl;
    std::atomic<bool> layersAutoStopped { false };
    std::atomic<IScaleLayerPitch*> scalePitch { nullptr };
    std::atomic<IAuxSource*> aux { nullptr };
    std::atomic<IMonitorSink*> monitor { nullptr };
    std::array<std::atomic<IAudioTap*>, kTapsPerPoint> inputTaps {}, outputTaps {}, sentTaps {};
    std::atomic<IVoicePostProcessor*> inputFilter { nullptr };
    std::atomic<IInputSource*> inputSource { nullptr };
    std::atomic<IVoicePostProcessor*> postProcessor { nullptr };
    std::atomic<bool> outputMuted { false };
    std::atomic<bool> fadeInRequest { false };
    std::atomic<int> testBusyMicros { 0 };
    std::atomic<bool> highPassOn { false }, agcOn { false };
    std::atomic<float> highPassHz { kHighPassHz.def }, agcTargetDb { kAgcTargetDb.def }, agcMaxGainDb { kAgcMaxGainDb.def };
    std::atomic<float> limiterCeilingDb { kLimiterCeilingDb }, limiterReleaseMs { kLimiterReleaseMs.def };
    std::atomic<float> chainFadeMs { kChainSwapFadeMs };

    // ---- DSP ----
    NoiseSuppressor ns;
    NoiseGate gate;
    Limiter limiter;
    dsp::Biquad highPass;
    float highPassCurHz = kHighPassHz.def;
    float agcPower = 0.0f, agcPowerCoeff = 0.0f, agcGainDb = 0.0f;
    std::unique_ptr<ShifterSet> cur;                 // audio thread owns (prepare() builds it)
    ShifterSet* next = nullptr;                      // audio thread owns: warming up / crossfading in
    std::atomic<ShifterSet*> pendingSet { nullptr }; // message thread -> audio thread
    std::array<std::atomic<ShifterSet*>, 4> retiredSets {};
    int swapWait = 0;
    dsp::Ramp swapFade;
    dsp::DelayLine dryDelay;
    int shifterLatency = 0;  // message thread view (the latest requested set)

    dsp::Ramp inGain, outGain, trimGain, muteGain, outMuteGain, voiceMix, nsMix, startFade, duckGain, highPassMix, agcGain;
    int layerCutSteps = 1;
    bool voicePathRunning = false;

    // ---- chain swap ----
    EffectChain* active = nullptr;       // audio thread owns
    EffectChain* fadingOut = nullptr;    // audio thread owns
    EffectChain* requested = nullptr;    // message thread view
    std::atomic<EffectChain*> pending { nullptr };
    std::array<std::atomic<EffectChain*>, 8> retired {};
    dsp::Ramp chainFade;

    // ---- buffers (prepared) ----
    std::vector<float> bufIn, bufNs, bufVoice, bufMain, bufNext, bufShift, bufLayer, bufChainOld, bufMon, bufAuxOut, bufSource, bufTap;

    // ---- meters ----
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<bool> inClip { false }, outClip { false };
    std::atomic<long long> nanInputs { 0 };
};
} // namespace koe

#include "Engine/VoiceProcessor.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
void atomicMax (std::atomic<float>& a, float v) noexcept
{
    float cur = a.load (std::memory_order_relaxed);
    while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
}

constexpr int kMaxConverterLatency = 4096;  // dry delay room for any quality's converter (quality 2: 2047)
// automatic input level (S-03 詳細): slow on purpose, never pumps on syllables
constexpr float kAgcPowerMs = 300.0f, kAgcRiseMs = 2000.0f, kAgcFallMs = 300.0f;
constexpr float kAgcHoldBelowDb = -50.0f, kAgcMaxCutDb = 24.0f;
} // namespace

VoiceProcessor::VoiceProcessor()
{
    // candidate B: Signalsmith (A) misses AC-04 by tens of cents at blocks short enough for §5.4
    shifterFactory = [] (int q) { return createConverterShifter (q); };
}

VoiceProcessor::~VoiceProcessor()
{
    delete pending.exchange (nullptr);
    if (fadingOut != active) delete fadingOut;
    delete active;
    for (auto& r : retired) delete r.exchange (nullptr);
    delete pendingSet.exchange (nullptr);
    delete next;
    for (auto& r : retiredSets) delete r.exchange (nullptr);
}

void VoiceProcessor::setShifterFactory (std::function<std::unique_ptr<IVoiceShifter>()> factory)
{
    shifterFactory = [f = std::move (factory)] (int) { return f(); };
}

void VoiceProcessor::setGate (bool on, float thresholdDb, float attackMs, float holdMs, float releaseMs) noexcept
{
    gate.setParams (kGateThresholdDb.clamp (thresholdDb), kGateAttackMs.clamp (attackMs), kGateHoldMs.clamp (holdMs), kGateReleaseMs.clamp (releaseMs));
    gateOn.store (on);
}

void VoiceProcessor::setAgc (bool on, float targetDb, float maxGainDb) noexcept
{
    agcTargetDb.store (kAgcTargetDb.clamp (targetDb));
    agcMaxGainDb.store (kAgcMaxGainDb.clamp (maxGainDb));
    agcOn.store (on);
}

void VoiceProcessor::setLimiter (float ceilingDb, float releaseMs) noexcept
{
    limiterCeilingDb.store (kLimiterCeilingSetDb.clamp (ceilingDb));
    limiterReleaseMs.store (kLimiterReleaseMs.clamp (releaseMs));
    limiter.setCeilingDb (limiterCeilingDb.load());
    limiter.setReleaseMs (limiterReleaseMs.load());
}

void VoiceProcessor::setShifter (bool has, float p, float f) noexcept
{
    hasShifter.store (has);
    pitchSt.store (kPitchSt.clamp (p));
    formantSt.store (kFormantSt.clamp (f));
}

void VoiceProcessor::setLayer (int index, const LayerParams& p) noexcept
{
    if (index < 0 || index >= kMaxLayers) return;
    auto& l = layerCtl[size_t (index)];
    l.scale.store (p.scale);
    l.pitchSt.store (kPitchSt.clamp (p.pitchSt));
    l.formantSt.store (kFormantSt.clamp (p.formantSt));
    l.levelDb.store (kLayerLevelDb.clamp (p.levelDb));
    l.key.store (std::clamp (p.key, 0, 11));
    l.degree.store (std::clamp (p.degree, -7, 7));
    l.minor.store (p.minor);
    l.active.store (p.active);
}

void VoiceProcessor::autoStopLayers() noexcept { layersAutoStopped.store (true); }

bool VoiceProcessor::anyLayerRunning() const noexcept
{
    for (const auto* s : { cur.get(), next })
        if (s != nullptr)
            for (bool r : s->layerRunning)
                if (r) return true;
    return false;
}

std::unique_ptr<VoiceProcessor::ShifterSet> VoiceProcessor::makeShifterSet (int q) const
{
    auto s = std::make_unique<ShifterSet>();
    s->main = shifterFactory (q);
    s->main->prepare (sampleRate, maxBlock);
    for (auto& l : s->layers)
    {
        l = shifterFactory (q);
        l->prepare (sampleRate, maxBlock);
    }
    s->latency = s->main->getLatencySamples();
    s->mainShiftMix.prepare (sampleRate, kShifterCrossfadeMs);
    s->mainShiftMix.snap (0.0f);
    for (auto& g : s->layerGain) { g.prepare (sampleRate, kLayerFadeMs); g.snap (0.0f); }
    return s;
}

void VoiceProcessor::setConverterQuality (int q)
{
    q = std::clamp (q, 0, 2);
    if (q == quality) return;
    quality = q;
    if (! prepared) return; // prepare() builds the converters at this quality
    auto s = makeShifterSet (q);
    if (s->latency + maxBlock + 8 > dryDelay.capacity()) return; // a custom factory far beyond any quality: keep the old one
    shifterLatency = s->latency;
    delete pendingSet.exchange (s.release(), std::memory_order_acq_rel); // a set the audio thread never took
}

void VoiceProcessor::prepare (double sr, int maxBlockSize)
{
    const int block = std::clamp (maxBlockSize, 64, kMaxBlockSize);
    if (sr != sampleRate || block != maxBlock)
    {
        // a chain is built for one rate and block size: a longer block would overrun its buffers. Drop it
        // (no audio thread runs during prepare); the owner requests a new chain for the new settings.
        delete pending.exchange (nullptr);
        if (fadingOut != active) delete fadingOut;
        delete active;
        active = fadingOut = requested = nullptr;
    }
    sampleRate = sr;
    maxBlock = block;

    ns.prepare (sr, maxBlock);
    gate.prepare (sr);
    limiter.prepare (sr, limiterCeilingDb.load(), 1.0f, limiterReleaseMs.load());

    // converters at the current quality; anything still on its way is dropped (no audio thread runs here)
    delete pendingSet.exchange (nullptr);
    delete next;
    next = nullptr;
    for (auto& r : retiredSets) delete r.exchange (nullptr);
    cur = makeShifterSet (quality);
    shifterLatency = cur->latency;
    dryDelay.prepare (std::max (shifterLatency, kMaxConverterLatency) + maxBlock + 8);
    swapWait = 0;
    swapFade.prepare (sr, kChainSwapFadeMs);
    swapFade.snap (0.0f);

    for (auto* b : { &bufIn, &bufNs, &bufVoice, &bufMain, &bufNext, &bufShift, &bufLayer, &bufChainOld, &bufMon, &bufAuxOut })
        b->assign (size_t (maxBlock), 0.0f);

    inGain.prepare (sr, kParamSmoothMs);     inGain.snap (dsp::dbToGain (inputGainDb.load()));
    outGain.prepare (sr, kParamSmoothMs);    outGain.snap (dsp::dbToGain (outputGainDb.load()));
    trimGain.prepare (sr, kParamSmoothMs);   trimGain.snap (dsp::dbToGain (trimDb.load()));
    muteGain.prepare (sr, kMuteFadeMs);      muteGain.snap (micMute.load() ? 0.0f : 1.0f);
    voiceMix.prepare (sr, kVoiceToggleFadeMs); voiceMix.snap (voiceOn.load() ? 1.0f : 0.0f);
    nsMix.prepare (sr, 20.0f);               nsMix.snap (noiseOn.load() && ns.isAvailable() ? 1.0f : 0.0f);
    startFade.prepare (sr, kStartupFadeMs);  startFade.snap (0.0f); startFade.setTarget (1.0f);
    duckGain.prepare (sr, 50.0f);            duckGain.snap (1.0f);
    chainFade.prepare (sr, kChainSwapFadeMs); chainFade.snap (1.0f);
    highPassMix.prepare (sr, 20.0f);         highPassMix.snap (0.0f);
    agcGain.prepare (sr, kParamSmoothMs);    agcGain.snap (1.0f);
    highPass.reset();
    agcPowerCoeff = dsp::onePoleCoeff (kAgcPowerMs, sr);
    agcPower = 0.0f;
    agcGainDb = 0.0f;
    layerCutSteps = std::max (1, int (sr * kScaleLayerCutMs * 0.001));

    voicePathRunning = voiceOn.load();
    prepared = true;
}

void VoiceProcessor::process (const float* in, float* outL, float* outR, int numSamples)
{
    // gate-closed silence decays IIR states into denormals: 10-90x CPU without this (effects-a measurement)
    juce::ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < numSamples; pos += maxBlock)
    {
        const int n = std::min (maxBlock, numSamples - pos);
        processBlock (in + pos, outL + pos, outR != nullptr ? outR + pos : nullptr, n);
    }
}

void VoiceProcessor::processBlock (const float* in, float* outL, float* outR, int n)
{
    if (const int us = testBusyMicros.load (std::memory_order_relaxed); us > 0)
    {
        const auto until = juce::Time::getHighResolutionTicks() + juce::Time::secondsToHighResolutionTicks (us * 1.0e-6);
        while (juce::Time::getHighResolutionTicks() < until) {}
    }
    if (fadeInRequest.exchange (false))
    {
        startFade.snap (0.0f);
        startFade.setTarget (1.0f);
    }

    // 1) input: non-finite scrub (E-17), input gain, meter
    inGain.setTarget (dsp::dbToGain (inputGainDb.load (std::memory_order_relaxed)));
    float ip = 0.0f;
    bool clip = false;
    long long bad = 0;
    for (int i = 0; i < n; ++i)
    {
        float v = in[i];
        if (! std::isfinite (v)) { v = 0.0f; ++bad; }
        if (std::abs (v) >= 0.999f) clip = true;
        v *= inGain.next();
        if (std::abs (v) >= 1.0f) clip = true;
        ip = std::max (ip, std::abs (v));
        bufIn[size_t (i)] = v;
    }
    if (bad > 0) nanInputs.fetch_add (bad, std::memory_order_relaxed);
    atomicMax (inPeak, ip);
    if (clip) inClip.store (true, std::memory_order_relaxed);

    // 1b) input low cut (S-03 詳細), before noise suppression: crossfaded on toggle, the cutoff glides per block
    const bool hpWant = highPassOn.load (std::memory_order_relaxed);
    if (hpWant && highPassMix.value <= 0.0f && ! highPassMix.isRamping())
    {
        highPass.reset();
        highPassCurHz = highPassHz.load (std::memory_order_relaxed);
        highPass.setHighpass (sampleRate, highPassCurHz);
    }
    highPassMix.setTarget (hpWant ? 1.0f : 0.0f);
    if (highPassMix.value > 0.0f || highPassMix.isRamping())
    {
        const float want = highPassHz.load (std::memory_order_relaxed);
        if (want != highPassCurHz)
        {
            highPassCurHz += 0.3f * (want - highPassCurHz);
            if (std::abs (want - highPassCurHz) < 0.5f) highPassCurHz = want;
            highPass.setHighpass (sampleRate, highPassCurHz);
        }
        for (int i = 0; i < n; ++i)
        {
            const float m = highPassMix.next();
            const float y = highPass.process (bufIn[size_t (i)]);
            bufIn[size_t (i)] += m * (y - bufIn[size_t (i)]);
        }
    }

    // 2) noise suppression, crossfaded on toggle because it changes the path delay
    const bool nsWant = noiseOn.load (std::memory_order_relaxed) && ns.isAvailable();
    if (nsWant && nsMix.value <= 0.0f && ! nsMix.isRamping()) ns.reset(); // stale FIFO from last time
    nsMix.setTarget (nsWant ? 1.0f : 0.0f);
    if (nsMix.value > 0.0f || nsMix.isRamping())
    {
        std::copy (bufIn.begin(), bufIn.begin() + n, bufNs.begin());
        ns.process (bufNs.data(), n, noiseMix.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i)
        {
            const float m = nsMix.next();
            bufIn[size_t (i)] += m * (bufNs[size_t (i)] - bufIn[size_t (i)]);
        }
    }

    // 2b) automatic input level (S-03 詳細), before the gate: slow, holds while the input is quiet
    const bool agcWant = agcOn.load (std::memory_order_relaxed);
    if (agcWant || agcGain.value != 1.0f || agcGain.isRamping())
    {
        float target = 1.0f;
        if (agcWant)
        {
            for (int i = 0; i < n; ++i)
            {
                const float s2 = bufIn[size_t (i)] * bufIn[size_t (i)];
                agcPower = s2 + agcPowerCoeff * (agcPower - s2);
            }
            const float maxGain = agcMaxGainDb.load (std::memory_order_relaxed);
            const float levelDb = 10.0f * std::log10 (agcPower + 1.0e-12f);
            if (levelDb > kAgcHoldBelowDb)
            {
                const float want = std::clamp (agcTargetDb.load (std::memory_order_relaxed) - levelDb, -kAgcMaxCutDb, maxGain);
                const float tauMs = want > agcGainDb ? kAgcRiseMs : kAgcFallMs;
                agcGainDb = want + std::exp (-float (n) / (float (sampleRate) * tauMs * 0.001f)) * (agcGainDb - want);
            }
            agcGainDb = std::min (agcGainDb, maxGain);
            target = dsp::dbToGain (agcGainDb);
        }
        else
        {
            agcPower = 0.0f; // next time starts from 0 dB
            agcGainDb = 0.0f;
        }
        agcGain.setTarget (target, agcWant ? n : agcGain.steps); // linear over the block (no zipper); 30 ms back to 1
        for (int i = 0; i < n; ++i) bufIn[size_t (i)] *= agcGain.next();
    }

    // 3) gate, 4) mic mute (30 ms, F-08-8)
    gate.process (bufIn.data(), n, ! gateOn.load (std::memory_order_relaxed));
    muteGain.setTarget (micMute.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    for (int i = 0; i < n; ++i) bufIn[size_t (i)] *= muteGain.next();

    // 5) voice changer path (shifter + layers + chain + trim) mixed with the dry path
    processVoicePath (bufIn.data(), n);

    // 6) ducking + aux (soundboard). Monitor mix = voice + aux "to monitor" part.
    auto* a = aux.load (std::memory_order_acquire);
    duckGain.setTarget (a != nullptr ? std::clamp (a->voiceDuckGain(), 0.0f, 1.0f) : 1.0f);
    for (int i = 0; i < n; ++i)
    {
        bufVoice[size_t (i)] *= duckGain.next();
        bufMon[size_t (i)] = bufVoice[size_t (i)];
    }
    if (a != nullptr) a->render (bufVoice.data(), bufMon.data(), n);

    // 7) output gain -> limiter -> non-finite scrub -> fade-in -> stereo
    outGain.setTarget (dsp::dbToGain (outputGainDb.load (std::memory_order_relaxed)));
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.next();
        bufVoice[size_t (i)] *= g;
        bufMon[size_t (i)] *= g;
    }
    limiter.process (bufVoice.data(), n);
    float op = 0.0f;
    const float ceiling = limiter.getCeiling();
    for (int i = 0; i < n; ++i)
    {
        float v = bufVoice[size_t (i)];
        if (! std::isfinite (v)) v = 0.0f;
        const float f = startFade.next();
        v *= f;
        outL[i] = v;
        if (outR != nullptr) outR[i] = v;
        op = std::max (op, std::abs (v));
        float m = std::clamp (bufMon[size_t (i)], -ceiling, ceiling) * f;
        bufMon[size_t (i)] = std::isfinite (m) ? m : 0.0f;
    }
    atomicMax (outPeak, op);
    if (op >= ceiling * 0.999f) outClip.store (true, std::memory_order_relaxed);
    if (auto* sink = monitor.load (std::memory_order_acquire)) sink->push (bufMon.data(), n);
}

bool VoiceProcessor::retireCurrentSet() noexcept
{
    for (auto& r : retiredSets)
    {
        ShifterSet* expected = nullptr;
        if (r.compare_exchange_strong (expected, cur.get()))
        {
            cur.release();
            cur.reset (next);
            next = nullptr;
            return true;
        }
    }
    return false; // every retire slot is full: keep both and retry next block
}

void VoiceProcessor::renderShifters (ShifterSet& s, const float* x, float* out, int n, const VoiceBlock& v)
{
    // the delayed dry signal is the main voice when there is nothing to convert (F-02-9)
    for (int i = 0; i < n; ++i) out[i] = dryDelay.readInt (s.latency + n - 1 - i);

    // ---- main voice ----
    if (v.desired && ! s.mainRunning)
    {
        s.main->setPitchSemitones (v.pitch);
        s.main->setFormantSemitones (v.formant);
        s.main->reset();
        s.mainRunning = true;
        s.mainWarmLeft = s.latency;
        s.mainShiftMix.snap (0.0f);
    }
    if (s.mainRunning)
    {
        if (v.desired)
        {
            s.main->setPitchSemitones (v.pitch);
            s.main->setFormantSemitones (v.formant);
        }
        s.main->process (x, bufShift.data(), n);
        if (s.mainWarmLeft > 0) s.mainWarmLeft -= n;
        s.mainShiftMix.setTarget (v.desired && s.mainWarmLeft <= 0 ? 1.0f : 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const float m = s.mainShiftMix.next();
            out[i] += m * (bufShift[size_t (i)] - out[i]);
        }
        if (! v.desired && ! s.mainShiftMix.isRamping() && s.mainShiftMix.value <= 0.0f) s.mainRunning = false;
    }

    // ---- layers (fixed order: main, layer 1, layer 2 — F-15-3) ----
    for (int li = 0; li < kMaxLayers; ++li)
    {
        const auto& l = v.layers[size_t (li)];
        auto& shifter = *s.layers[size_t (li)];
        auto& gain = s.layerGain[size_t (li)];
        auto& running = s.layerRunning[size_t (li)];
        auto& warm = s.layerWarmLeft[size_t (li)];
        if (l.act && ! running)
        {
            shifter.setPitchSemitones (l.pitch);
            shifter.setFormantSemitones (l.formant);
            shifter.reset();
            running = true;
            warm = s.latency;
            gain.snap (0.0f);
        }
        if (! running) continue;
        if (l.act)
        {
            shifter.setPitchSemitones (l.pitch);
            shifter.setFormantSemitones (l.formant);
        }
        shifter.process (x, bufLayer.data(), n);
        if (warm > 0) warm -= n;
        if (l.cut) gain.setTarget (0.0f, layerCutSteps);
        else gain.setTarget (l.act && warm <= 0 ? l.gain : 0.0f);
        for (int i = 0; i < n; ++i) out[i] += gain.next() * bufLayer[size_t (i)];
        if (! l.act && ! gain.isRamping() && gain.value <= 0.0f) running = false;
    }
}

void VoiceProcessor::processVoicePath (float* x, int n)
{
    // keep the dry delay fed even while the voice changer is OFF so switching ON never replays stale audio
    for (int i = 0; i < n; ++i) dryDelay.push (x[i]);

    // a converter set of another quality: warm it up beside the running one, then crossfade (like the chain swap)
    if (next == nullptr)
        if (auto* s = pendingSet.exchange (nullptr, std::memory_order_acq_rel))
        {
            next = s;
            swapWait = s->latency + std::max (1, int (sampleRate * kLayerFadeMs * 0.001)); // its own warm-up fades done
            swapFade.snap (0.0f);
        }

    const bool on = voiceOn.load (std::memory_order_relaxed);
    voiceMix.setTarget (on ? 1.0f : 0.0f);
    if (! on && ! voiceMix.isRamping() && voiceMix.value <= 0.0f)
    {
        voicePathRunning = false;
        if (next != nullptr) retireCurrentSet(); // nothing is heard: switch at once
        std::copy (x, x + n, bufVoice.begin());
        return;
    }
    if (! voicePathRunning)
    {
        // OFF -> ON: start effects and converters from a clean state
        voicePathRunning = true;
        if (active != nullptr) active->resetAll();
        cur->mainRunning = false;
        cur->mainShiftMix.snap (0.0f);
        cur->layerRunning.fill (false);
        for (auto& g : cur->layerGain) g.snap (0.0f);
    }

    // ---- this block's voice parameters ----
    VoiceBlock vb;
    const bool hs = hasShifter.load (std::memory_order_relaxed);
    vb.pitch = pitchSt.load (std::memory_order_relaxed);
    vb.formant = formantSt.load (std::memory_order_relaxed);
    vb.desired = hs && (vb.pitch != 0.0f || vb.formant != 0.0f);
    auto* sp = scalePitch.load (std::memory_order_acquire);
    if (sp != nullptr)
    {
        bool anyScale = false;
        for (auto& l : layerCtl) anyScale = anyScale || (l.active.load() && l.scale.load());
        if (anyScale) sp->analyse (x, n);
    }
    const bool layersStopped = layersAutoStopped.load (std::memory_order_relaxed);
    for (int li = 0; li < kMaxLayers; ++li)
    {
        auto& ctl = layerCtl[size_t (li)];
        auto& l = vb.layers[size_t (li)];
        l.act = ctl.active.load (std::memory_order_relaxed) && hs && ! layersStopped;
        l.pitch = ctl.pitchSt.load (std::memory_order_relaxed);
        if (l.act && ctl.scale.load (std::memory_order_relaxed))
        {
            l.pitch = sp != nullptr ? sp->layerSemitones (ctl.key.load(), ctl.minor.load(), ctl.degree.load()) : std::nanf ("");
            if (! std::isfinite (l.pitch)) l.act = false, l.cut = true; // no pitch detected: silent within 20 ms (E-28)
            else l.pitch = kPitchSt.clamp (l.pitch);
        }
        l.formant = ctl.formantSt.load (std::memory_order_relaxed);
        l.gain = dsp::dbToGain (ctl.levelDb.load (std::memory_order_relaxed));
    }

    // ---- main voice + layers, crossfaded to the incoming converter set when there is one ----
    renderShifters (*cur, x, bufMain.data(), n, vb);
    if (next != nullptr)
    {
        renderShifters (*next, x, bufNext.data(), n, vb);
        if (swapWait > 0)
            swapWait -= n;
        else
        {
            swapFade.setTarget (1.0f);
            for (int i = 0; i < n; ++i)
            {
                const float c = swapFade.next();
                bufMain[size_t (i)] += c * (bufNext[size_t (i)] - bufMain[size_t (i)]);
            }
            if (! swapFade.isRamping()) retireCurrentSet();
        }
    }

    // ---- chain, swapped with a crossfade (F-04-6, presetCrossfadeMs) ----
    if (! chainFade.isRamping() && fadingOut == nullptr)
    {
        if (auto* nextChain = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            fadingOut = active;
            active = nextChain;
            chainFade.snap (0.0f);
            chainFade.setTarget (1.0f, std::max (1, int (sampleRate * double (chainFadeMs.load (std::memory_order_relaxed)) * 0.001)));
        }
    }
    if (chainFade.isRamping())
    {
        std::copy (bufMain.begin(), bufMain.begin() + n, bufChainOld.begin());
        if (fadingOut != nullptr) fadingOut->process (bufChainOld.data(), n);
        if (active != nullptr) active->process (bufMain.data(), n);
        for (int i = 0; i < n; ++i)
        {
            const float c = chainFade.next();
            bufMain[size_t (i)] = bufChainOld[size_t (i)] + c * (bufMain[size_t (i)] - bufChainOld[size_t (i)]);
        }
    }
    else if (active != nullptr)
    {
        active->process (bufMain.data(), n);
    }
    if (! chainFade.isRamping() && fadingOut != nullptr)
    {
        for (auto& r : retired)
        {
            EffectChain* expected = nullptr;
            if (r.compare_exchange_strong (expected, fadingOut)) { fadingOut = nullptr; break; }
        }
        // if every retire slot is full, keep it (unprocessed) and retry next block
    }

    // ---- preset trim, then ON/OFF crossfade with the undelayed dry path (F-02-3) ----
    trimGain.setTarget (dsp::dbToGain (trimDb.load (std::memory_order_relaxed)));
    for (int i = 0; i < n; ++i)
    {
        const float on01 = voiceMix.next();
        const float wet = bufMain[size_t (i)] * trimGain.next();
        bufVoice[size_t (i)] = x[i] + on01 * (wet - x[i]);
    }
}

void VoiceProcessor::requestChain (std::unique_ptr<EffectChain> chain)
{
    auto* raw = chain.release();
    delete pending.exchange (raw, std::memory_order_acq_rel); // a request the audio thread never took (E-20)
    requested = raw;
}

void VoiceProcessor::collectGarbage()
{
    for (auto& r : retired) delete r.exchange (nullptr, std::memory_order_acq_rel);
    for (auto& r : retiredSets) delete r.exchange (nullptr, std::memory_order_acq_rel);
}

VoiceProcessor::MeterValues VoiceProcessor::fetchMeters() noexcept
{
    MeterValues m;
    m.inputPeak = inPeak.exchange (0.0f);
    m.outputPeak = outPeak.exchange (0.0f);
    m.inputClip = inClip.exchange (false);
    m.outputClip = outClip.exchange (false);
    return m;
}

int VoiceProcessor::getLatencySamples() const noexcept
{
    int l = limiter.getLatencySamples();
    if (noiseOn.load() && ns.isAvailable()) l += ns.getLatencySamples();
    if (voiceOn.load())
    {
        l += shifterLatency;
        if (requested != nullptr) l += requested->getLatencySamples();
    }
    return l;
}
} // namespace koe

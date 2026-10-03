#pragma once

// Helpers for the device experiments (T1, T4). Devices are opened only through openDevice(), which
// passes explicit names to AudioDeviceManager::initialise(xml, selectDefaultDeviceOnFailure = false)
// so JUCE never falls back to the system default devices (speakers).

#include "p0_common.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace p0
{
/** Full device name of `type` containing `part` (case-insensitive); empty part = the type's default. */
inline juce::String findDeviceName (juce::AudioDeviceManager& dm, const juce::String& typeName, const juce::String& part, bool isInput)
{
    for (auto* t : dm.getAvailableDeviceTypes())
    {
        if (t->getTypeName() != typeName) continue;
        t->scanForDevices();
        const auto names = t->getDeviceNames (isInput);
        if (part.isEmpty()) return names[t->getDefaultDeviceIndex (isInput)];
        for (const auto& n : names)
            if (n.containsIgnoreCase (part)) return n;
    }
    return {};
}

/** Opens type/in/out at 48 kHz with the given buffer. inName or outName may be empty (unused side). */
inline juce::String openDevice (juce::AudioDeviceManager& dm, const juce::String& typeName, const juce::String& inName,
                                const juce::String& outName, int buffer)
{
    juce::XmlElement xml ("DEVICESETUP");
    xml.setAttribute ("deviceType", typeName);
    xml.setAttribute ("audioInputDeviceName", inName);
    xml.setAttribute ("audioOutputDeviceName", outName);
    xml.setAttribute ("audioDeviceRate", 48000.0);
    xml.setAttribute ("audioDeviceBufferSize", buffer);
    const auto err = dm.initialise (inName.isEmpty() ? 0 : 2, outName.isEmpty() ? 0 : 2, &xml, false);
    if (err.isNotEmpty()) return err;
    return dm.getCurrentAudioDevice() == nullptr ? juce::String ("no device opened") : juce::String();
}

/** Refuses outputs other than the virtual cable unless --allow-any-output (nothing reaches speakers). */
inline bool outputIsSafe (const juce::String& outName, const Args& args)
{
    return outName.containsIgnoreCase ("CABLE Input") || args.has ("--allow-any-output");
}

inline juce::String deviceSummary (juce::AudioDeviceManager& dm)
{
    auto* d = dm.getCurrentAudioDevice();
    if (d == nullptr) return "none";
    juce::AudioDeviceManager::AudioDeviceSetup s;
    dm.getAudioDeviceSetup (s);
    return dm.getCurrentAudioDeviceType() + " | in '" + s.inputDeviceName + "' | out '" + s.outputDeviceName + "' | "
           + juce::String (d->getCurrentSampleRate(), 0) + " Hz | buffer " + juce::String (d->getCurrentBufferSizeSamples())
           + " | reported latency in " + juce::String (d->getInputLatencyInSamples()) + " out " + juce::String (d->getOutputLatencyInSamples())
           + " samples";
}
} // namespace p0

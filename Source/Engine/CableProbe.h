#pragma once

#include <juce_core/juce_core.h>

#include <memory>

namespace koe
{
/**
    Setup wizard check "仮想マイクに音が届いているか" (S-04, F-10-3): opens the recording side of the
    virtual cable (an input whose name contains "CABLE Output") on its own and reports its peak level.
    Message thread. Never outputs sound.
*/
class CableProbe
{
public:
    CableProbe();
    ~CableProbe();
    /** Returns an error (Japanese) or empty. inputName: the "CABLE Output" device to listen to. */
    juce::String start (const juce::String& inputName);
    void stop();
    bool isRunning() const;
    /** Peak since the last call, linear. */
    float fetchPeak();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace koe

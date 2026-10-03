// AppController regressions found in review (lead). Category "Integration".

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Tests/TestUtil.h"

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}
} // namespace

class AppControllerTests : public juce::UnitTest
{
public:
    AppControllerTests() : juce::UnitTest ("AppController review", "Integration") {}

    void runTest() override
    {
        beginTest ("Save as new with the converter OFF: the layers it drops (E-30) stay silent when the converter is turned ON again");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            c.setVoiceChangerOn (true);
            auto& vp = c.getProcessorForTests();
            const auto in = synthVoice (1.0, 7, kSr, false);
            std::vector<float> out (size_t (kBlock), 0.0f);
            auto render = [&]
            {
                for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data(), nullptr, kBlock);
            };
            juce::String why;
            expect (c.addLayer (why));
            render();
            expect (vp.anyLayerRunning(), "the added layer sounds");
            c.setShifterEnabled (false);
            render();
            expect (! vp.anyLayerRunning(), "silent while the converter is OFF");
            expect (c.saveCurrentAsNew (juce::String::fromUTF8 ("変換 OFF"), why), why);
            expectEquals (c.getNumLayers(), 0); // the working preset is now the saved one, without layers
            c.setShifterEnabled (true);
            render();
            expect (! vp.anyLayerRunning(), "a layer the working preset no longer has must not sound");
            c.shutdown();
        }

        beginTest ("F-01-6: a monitor device saved as the virtual cable (hand-edited settings.json) is not kept");
        {
            freshDataDir();
            Settings s;
            s.monitorDevice = "CABLE Input (VB-Audio Virtual Cable)";
            expect (saveSettings (s, paths::settingsFile()));
            AppController c (false);
            c.startup();
            expect (c.getSettings().monitorDevice.isEmpty(), c.getSettings().monitorDevice);
            c.shutdown();
        }

        beginTest ("F-01-4 for the monitor: a lost device shows OFF with a notice, comes back by itself, OFF stops waiting");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            auto hasNotice = [&c]
            {
                for (auto& n : c.getNotices()) if (n.key == "monitor.lost") return true;
                return false;
            };
            auto ticks = [&c] (int k) { for (int i = 0; i < k; ++i) c.tickForTests(); };
            c.setMonitorDevice ("Headphones (Test)");
            c.setMonitorOn (true);
            expect (c.isMonitorOn());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1); // within 2 s
            expect (! c.isMonitorOn());
            expect (! c.getStatus().monitorOn);
            expect (hasNotice());
            ticks (30); // ~1 s later it retries; AppController (false) opens no device, so the retry succeeds
            expect (c.isMonitorOn());
            expect (! hasNotice());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1);
            expect (! c.isMonitorOn());
            c.setMonitorOn (true); // the toggle retries at once
            expect (c.isMonitorOn());
            expect (! hasNotice());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1);
            c.setMonitorOn (false); // stop waiting
            expect (! hasNotice());
            ticks (60);
            expect (! c.isMonitorOn());
            c.shutdown();
        }
    }
};

static AppControllerTests appControllerTests;
} // namespace koe

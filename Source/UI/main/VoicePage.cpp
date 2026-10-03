#include "UI/main/VoicePage.h"

#include "UI/main/VoiceView.h"

namespace koe::ui::mainui
{
std::unique_ptr<VoicePage> makeVoicePage (int layoutStyle, AppController& c, Navigator& nav)
{
    if (layoutStyle == 1) return makePaperVoicePage (c, nav);
    if (layoutStyle == 2) return makeMonoVoicePage (c, nav);
    return std::make_unique<VoiceView> (c, nav);
}
} // namespace koe::ui::mainui

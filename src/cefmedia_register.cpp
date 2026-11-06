#include "cefmedia_register.h"

#include "cefmedia_output.h"
#include "cefmedia_source.h"
#include "channel_status_tracker.h"

bool RegisterCefMediaPlugins()
{
    RegisterCefMediaSource();
    RegisterCefMediaOutput();
    // Channel-occupancy tracker (status receive thread shared by the source
    // properties dialogs' live channel-state labels). Module-lifetime hold —
    // properties can be opened any time; the dock holds its own subscription.
    ChannelStatusTracker::Instance().Start();
    return true;
}

void UnregisterCefMediaPlugins()
{
    ChannelStatusTracker::Instance().Stop();
    UnregisterCefMediaSource();
    UnregisterCefMediaOutput();
}

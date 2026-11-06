#include "channel_status_tracker.h"

#include <atomic>

#include "cefmedia_api.h"
#include "cefmedia_consumer.h"

namespace {

// Guards RefreshPropertiesTask against overlapping runs: a queued-but-not-yet-run
// task is skipped by comparing the bitmap it was queued for with the one that
// arrives later (the next change re-queues; nothing is lost).
std::atomic<unsigned int> g_refresh_target{0};

} // namespace

ChannelStatusTracker& ChannelStatusTracker::Instance()
{
    static ChannelStatusTracker instance;
    return instance;
}

void ChannelStatusTracker::Start()
{
    CefMediaApi::Instance().SetStatusCallback(&ChannelStatusTracker::OnStatus, this);
    CefMediaApi::Instance().StatusStart();
}

void ChannelStatusTracker::Stop()
{
    CefMediaApi::Instance().RemoveStatusCallback(&ChannelStatusTracker::OnStatus, this);
    CefMediaApi::Instance().StatusStop();
}

void ChannelStatusTracker::OnStatus(unsigned int bitmap, void* obj)
{
    auto* self = static_cast<ChannelStatusTracker*>(obj);
    const unsigned int prev = self->bitmap_.exchange(bitmap, std::memory_order_relaxed);
    if (prev == bitmap) {
        return; // 5 s resend heartbeat / no actual change: nothing to refresh
    }
    // Queue one main-thread refresh per change. g_refresh_target dedupes bursts
    // (a tab releasing + another acquiring within one tick): the queued task
    // always re-reads the LATEST bitmap via Instance().Bitmap(), so a skipped
    // intermediate state is fine — only the last task per target value runs.
    unsigned int expected = 0;
    if (!g_refresh_target.compare_exchange_strong(expected, bitmap,
                                                  std::memory_order_relaxed)) {
        return; // a refresh task is already pending; it reads the latest bitmap
    }
    obs_queue_task(OBS_TASK_UI, &ChannelStatusTracker::RefreshPropertiesTask, nullptr, false);
}

void ChannelStatusTracker::RefreshPropertiesTask(void* /*param*/)
{
    g_refresh_target.store(0, std::memory_order_relaxed);
    // obs_source_update_properties is safe on any registered source: with no
    // open properties dialog it just fires the signal (no listener = no-op);
    // with one open, OBS 32.2.2 rebuilds the form on the UI thread (we are on it).
    // Sources themselves are owned by the consumer registry — enumerate it.
    CefVideoConsumer::Instance().ForEachSource([](obs_source_t* source) {
        obs_source_update_properties(source);
    });
}

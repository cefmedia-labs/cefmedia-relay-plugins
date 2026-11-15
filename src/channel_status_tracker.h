// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_CHANNEL_STATUS_TRACKER_H
#define CEFMEDIA_CHANNEL_STATUS_TRACKER_H

#include <obs.h>

#include <atomic>

// Shared channel-occupancy tracker (module-lifetime singleton): subscribes ONE
// status callback to the ipc client for the whole module (the dock holds its own
// separate subscription) and exposes the latest CefMedia Relay bitmap.
//
// Consumers:
//  - the source properties dialog: the channel dropdown embeds the state into the
//    item names ("● 1" occupied / "○ 1" free — the OBS property API has no
//    per-item icons), and the tracker refreshes any OPEN properties dialog by
//    calling obs_source_update_properties on registered sources (OBS 32.2.2's
//    OBSBasicProperties listens to the "update_properties" signal and rebuilds
//    the form, preserving the selected value);
//  - any code that just wants the current bitmap (atomic read).
class ChannelStatusTracker {
public:
    static ChannelStatusTracker& Instance();

    // Latest CefMedia Relay-reported bitmap (bit i = channel i occupied). Written on
    // the status receive thread, read anywhere (relaxed: indicator-level data).
    unsigned int Bitmap() const { return bitmap_.load(std::memory_order_relaxed); }

    // Hold/release the status receive thread (refcounted in the ipc client;
    // idempotent per holder pattern — one Start per interested module).
    void Start();
    void Stop();

private:
    ChannelStatusTracker() = default;
    ~ChannelStatusTracker() = default;
    ChannelStatusTracker(const ChannelStatusTracker&) = delete;
    ChannelStatusTracker& operator=(const ChannelStatusTracker&) = delete;

    // Status receive thread: store the bitmap, and when it CHANGED queue a
    // main-thread task that refreshes open properties dialogs of our sources.
    static void OnStatus(unsigned int bitmap, void* obj);
    // OBS_TASK_UI: call obs_source_update_properties for every registered source
    // (a closed dialog costs a cheap no-op rebuild-on-next-open; an open one
    // rebuilds immediately with fresh item names).
    static void RefreshPropertiesTask(void* param);

    std::atomic<unsigned int> bitmap_{0};
};

#endif // CEFMEDIA_CHANNEL_STATUS_TRACKER_H

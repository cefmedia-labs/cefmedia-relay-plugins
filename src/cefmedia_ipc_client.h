// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_IPC_CLIENT_H
#define CEFMEDIA_IPC_CLIENT_H

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "cefmedia.h" // frame contract (cefmedia_frames.h) + callback typedefs

// ---------------------------------------------------------------------------
// CefMediaIpcClient — the OBS-side libipc client.
//
// The plugin links libipc directly and receives frames from
// the CefMedia Relay process over the per-channel broadcast routes
//   "cefmedia.video.<ch>" / "cefmedia.audio.<ch>"  (<ch> 0-based 0..4, see
//   CefMediaChannelSuffix below; one message = one frame block [96B header]
//   [payload], see cefmedia_frames.h)
// and sends the playback rate over "cefmedia.rate" (fixed 96B message with a
// 0-based channel field routing it to the CefMedia Relay tab that owns that channel).
// CefMedia Relay also broadcasts the channel-occupancy bitmap over
// "cefmedia.status" (single fixed-name channel; RegisterStatusCallback).
//
// Crash isolation: when OBS dies CefMedia Relay's send path sees no receivers
// (libipc conn_count -> 0 / force_push kicks the dead receiver) and the relay
// switches to drop-oldest — CefMedia Relay never wedges. When CefMedia Relay dies
// the recv here just times out; a restarted CefMedia Relay broadcasts on the same
// channel names and this client picks the stream up again automatically.
//
// Fan-out (per channel): the source plugin (preview) and the output plugin
// (recorder) each register callbacks; both are invoked per frame on the
// channel's receive thread (idempotent (fn,obj) append). Callbacks run on the
// receive threads and must be quick (copy-and-enqueue, never encode — the fan-out
// thread serves every consumer on the channel).
//
// Receive threads are lazy and per channel: the first subscriber of channel N
// spawns that channel's video+audio threads, the last unsubscriber joins them
// — idle channels cost zero threads, matching CefMedia Relay's on-demand relay
// (cefmedia_channel_start/stop). The status thread follows the same pattern.
// ---------------------------------------------------------------------------
class CefMediaIpcClient {
public:
    static CefMediaIpcClient& Instance();

    // Register/unregister a frame callback for one capture channel (0-based
    // 0..4; out-of-range is clamped). Idempotent per (fn, obj). Unregister is
    // mandatory before the |obj| lifetime ends (e.g. output stop/destroy):
    // a left-behind entry is a dangling pointer the next frame would call.
    void SetVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel);
    void SetAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel);
    void RemoveVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel);
    void RemoveAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel);

    // Occupancy-bitmap callback (channel-independent, single fixed thread).
    void SetStatusCallback(ChannelStatusCallback callback, void* obj);
    void RemoveStatusCallback(ChannelStatusCallback callback, void* obj);

    // Start the receive threads of one channel (refcounted per channel: first
    // start spawns, last stop joins).
    void ReaderStart(int channel);
    void ReaderStop(int channel);

    // Start/stop the status receive thread (refcounted the same way; usually
    // held once by the dock for the occupancy indicator).
    void StatusStart();
    void StatusStop();

    // Send the playback rate to CefMedia Relay for one capture channel
    // (fire-and-forget; best effort). |recording| rides the same message
    // (rate-msg recording field, 2026-11-15): the dock heartbeat re-fires every
    // second while recording (any speed, 1x included) with recording=true, the
    // stop/restore-1x paths send recording=false — CefMedia Relay latches
    // per-channel recording state from it (10s expiry self-heals an OBS crash).
    void RateSet(int rate_permille, int channel, bool recording);

private:
    CefMediaIpcClient() = default;
    ~CefMediaIpcClient();
    CefMediaIpcClient(const CefMediaIpcClient&) = delete;
    CefMediaIpcClient& operator=(const CefMediaIpcClient&) = delete;

    static constexpr int kChannelCount = CEFMEDIA_CHANNEL_COUNT;

    struct VideoCallbackEntry { VideoFrameCallback fn; void* obj; };
    struct AudioCallbackEntry { AudioBufferCallback fn; void* obj; };

    // Per-channel receive state: one thread pair + one callback list each.
    struct ChannelState {
        std::vector<VideoCallbackEntry> video_callbacks;
        std::vector<AudioCallbackEntry> audio_callbacks;
        std::thread* video_thread = nullptr;
        std::thread* audio_thread = nullptr;
        std::atomic<int> refcount{0};
        std::atomic<bool> stop_requested{false};
        // Serializes ReaderStart's spawn against ReaderStop's join+delete on
        // this channel: the refcount is atomic, but the thread-pair slot is
        // not — callers may run on different threads (e.g. the preview's
        // detach worker vs. a source attaching on the OBS UI thread).
        std::mutex reader_mutex;
    };

    static void VideoRecvThreadProc(CefMediaIpcClient* self, int channel);
    static void AudioRecvThreadProc(CefMediaIpcClient* self, int channel);
    static void StatusRecvThreadProc(CefMediaIpcClient* self);

    ChannelState channels_[kChannelCount];
    std::mutex callbacks_mutex_;

    std::vector<ChannelStatusCallback> status_callbacks_; // parallel obj list
    std::vector<void*> status_objs_;
    std::thread* status_thread_ = nullptr;
    std::atomic<int> status_refcount_{0};
    std::atomic<bool> status_stop_requested_{false};
};

#endif // CEFMEDIA_IPC_CLIENT_H

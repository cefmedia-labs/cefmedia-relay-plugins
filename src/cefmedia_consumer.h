// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_CONSUMER_H
#define CEFMEDIA_CONSUMER_H

#include <obs.h>

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "cefmedia_api.h"
#include "channel_frame_binding.h" // per-channel callback+reader lifecycle (RAII)
#include "frontend_api.h"
#include "timeline.h"

// Module-level singleton consumer: libipc receive threads -> OBS async sources.
//
// 5-channel model: every registered source is bound to a capture
// channel (0-based, chosen in the source properties, default channel 0 = UI
// "1"). Frames arrive on the per-channel receive threads carrying that channel;
// each frame is pushed only to the sources bound to it (same-channel sources
// all get it). Each channel owns an OutputTimeline — a shared timeline would be
// torn apart by two channels' interleaved streams (different PTS bases).
//
// Threading model:
//  - AddSource/RemoveSource are called on the OBS main thread (source create/destroy);
//  - frame callbacks run on the CefMediaIpcClient per-channel receive threads
//    (libipc routes cefmedia.video.<ch> / cefmedia.audio.<ch>); conversion +
//    obs_source_output_video / output_audio happen synchronously inside the
//    callback (OBS copies internally);
//  - mutex_ is held while outputting: RemoveSource holding the lock naturally waits for
//    in-flight callbacks to finish, so after destroy no callback references the source
//    pointer (no use-after-free);
//  - StopCapture is called OUTSIDE the lock: reader Stop joins the reader threads, so
//    holding the lock while waiting would deadlock with threads waiting on the lock;
//    calling outside the lock lets the join finish the in-flight callback first.
//
// Reader references are per channel and refcounted inside the ipc client: the
// first source on channel N calls ReaderStart(N), the last one leaving calls
// ReaderStop(N) (the recorder plugin may still hold its own reference on any
// channel — fan-out keeps those threads alive).
class CefVideoConsumer {
public:
    static CefVideoConsumer& Instance();

    // channel: 0-based capture channel the source displays (1-based in the UI).
    void AddSource(obs_source_t* source, int channel);
    // Rebind a registered source to another channel (the properties dropdown
    // changed): moves the registry entry, starts the new channel's reader and
    // releases the old one when it runs empty. OBS main thread.
    void UpdateSourceChannel(obs_source_t* source, int channel);
    void RemoveSource(obs_source_t* source);

    // Enumerate registered sources under the registry lock (callback must be
    // quick and must NOT call back into Add/Remove/Update — recursive_mutex
    // makes re-entry safe anyway, but the iteration snapshot stays consistent
    // only without mutations). Used e.g. to refresh open properties dialogs.
    void ForEachSource(const std::function<void(obs_source_t*)>& fn);

private:
    CefVideoConsumer() = default;
    ~CefVideoConsumer() = default;
    CefVideoConsumer(const CefVideoConsumer&) = delete;
    CefVideoConsumer& operator=(const CefVideoConsumer&) = delete;

    struct SourceEntry {
        obs_source_t* source;
        int channel; // 0-based capture channel
    };

    void StartIfNeeded(int channel);
    void StopCapture(int channel); // called outside the lock, idempotent

    void OnVideoFrame(cefmedia_video_frame* frame, const uint8_t* payload, int channel);
    void OnAudioFrame(cefmedia_audio_buffer* buffer, const uint8_t* payload, int channel);

    // Auto fit: on the first frame, run "Fit to Screen" for this source in its scene
    // (OBS_BOUNDS_SCALE_INNER + canvas size), once, without fighting later manual
    // transforms.
    void MaybeQueueAutoFit(obs_source_t* source);
    static void ApplyFitTask(void* param); // OBS main thread
    void ApplyFitToScreen(obs_source_t* source);
    static bool FindSourceItem(obs_scene_t* scene, obs_sceneitem_t* item, void* param);

    static void VideoCallback(cefmedia_video_frame* frame, const uint8_t* payload, void* obj,
                              int channel);
    static void AudioCallback(cefmedia_audio_buffer* buffer, const uint8_t* payload, void* obj,
                              int channel);

    std::recursive_mutex mutex_;
    std::vector<SourceEntry> sources_;
    std::vector<obs_source_t*> fit_queued_; // sources with auto-fit already queued (tracked
                                            // per source lifetime)
    CefMediaApi& api_ = CefMediaApi::Instance();
    // Output timeline per channel: absorbs PTS forward jumps (seek / break), keeps output
    // continuous with audio and video sharing one base. One per channel — two channels'
    // streams carry unrelated PTS epochs and would corrupt a shared timeline.
    OutputTimeline timeline_[CEFMEDIA_CHANNEL_COUNT];
    // Per-channel reader binding (frame callbacks + reader reference). "Started"
    // == bindings_[ch].channel() >= 0. Normally detached before static teardown:
    // OBS destroys sources (-> RemoveSource -> StopCapture) before unloading
    // modules, so the destructor-time Detach is a no-op in the normal path.
    // LAST member on purpose (reverse destruction order): Detach must run while
    // timeline_ (touched by in-flight callbacks) is still alive.
    ChannelFrameBinding bindings_[CEFMEDIA_CHANNEL_COUNT]{
        {&VideoCallback, &AudioCallback, this}, {&VideoCallback, &AudioCallback, this},
        {&VideoCallback, &AudioCallback, this}, {&VideoCallback, &AudioCallback, this},
        {&VideoCallback, &AudioCallback, this}};
};

#endif // CEFMEDIA_CONSUMER_H

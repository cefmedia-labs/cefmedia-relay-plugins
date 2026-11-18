// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "cefmedia_consumer.h"

#include "cefmedia.h"
#include "frame_convert.h"

namespace {

// Scene-item lookup context (shared by FindSourceItem and ApplyFitToScreen).
struct ItemFindCtx {
    obs_source_t* source;
    obs_sceneitem_t* item;
};

// Source-settings key of the capture-channel selection (1-based UI value).
constexpr const char* kChannelSetting = "channel";

// Read the source's channel selection (1-based in settings; default 1) and map
// it to the 0-based internal channel (clamped into 0..4).
int SourceChannel(obs_source_t* source)
{
    const int64_t ui_channel = obs_data_get_int(obs_source_get_settings(source), kChannelSetting);
    int ch = static_cast<int>(ui_channel) - 1;
    if (ch < 0 || ch >= CEFMEDIA_CHANNEL_COUNT) {
        ch = 0;
    }
    return ch;
}

// Source-settings key marking that auto-fit already ran for this source. Written
// back into the source's settings (persisted by OBS with the scene collection), so
// the fit happens once per source ever — not once per OBS session. Without this,
// every OBS restart re-fit on the first frame and reset the user's manually moved
// item.
constexpr const char* kFitDoneSetting = "auto_fit_done";

} // namespace

CefVideoConsumer& CefVideoConsumer::Instance()
{
    static CefVideoConsumer instance;
    return instance;
}

void CefVideoConsumer::AddSource(obs_source_t* source, int channel, int output_mode)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sources_.push_back({source, channel, output_mode});
    StartIfNeeded(channel);
}

void CefVideoConsumer::UpdateSourceOutputMode(obs_source_t* source, int output_mode)
{
    bool became_audio_only = false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (SourceEntry& e : sources_) {
        if (e.source == source) {
            if (e.output_mode != output_mode) {
                e.output_mode = output_mode;
                became_audio_only = output_mode == kOutputAudioOnly;
            }
            break;
        }
    }
    if (became_audio_only) {
        // Switching to audio-only stops video output permanently; the async
        // source would otherwise keep rendering its cached last frame forever.
        obs_source_output_video(source, NULL);
    }
}

void CefVideoConsumer::UpdateSourceChannel(obs_source_t* source, int channel)
{
    int old_channel = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (SourceEntry& e : sources_) {
            if (e.source == source) {
                if (e.channel == channel) {
                    return; // no change
                }
                old_channel = e.channel;
                e.channel = channel;
                break;
            }
        }
    }
    if (old_channel < 0) {
        return; // not registered (should not happen)
    }
    StartIfNeeded(channel); // may register callbacks + spawn the new channel's threads
    // Clear the async frame cache: the source keeps rendering its last frame
    // (OBS async sources freeze on the newest frame), which after a channel
    // change is the OLD channel's picture. Until the new channel's first frame
    // arrives — or forever, if that channel is idle — the stale picture would
    // linger on the source. obs_source_output_video(NULL) deactivates the
    // async texture (blank until the new channel delivers).
    obs_source_output_video(source, NULL);
    // Release the old channel's reader reference if this was its last source
    // (outside the lock — ReaderStop joins threads; same contract as RemoveSource).
    bool old_empty = true;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const SourceEntry& e : sources_) {
            if (e.channel == old_channel) {
                old_empty = false;
                break;
            }
        }
    }
    if (old_empty) {
        StopCapture(old_channel);
    }
}

void CefVideoConsumer::RemoveSource(obs_source_t* source)
{
    std::vector<int> stopped_channels;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto it = sources_.begin(); it != sources_.end(); ++it) {
            if (it->source == source) {
                stopped_channels.push_back(it->channel);
                sources_.erase(it);
                break;
            }
        }
        // After removal, the next re-create should auto-fit again; clear the queued mark.
        for (auto it = fit_queued_.begin(); it != fit_queued_.end(); ++it) {
            if (*it == source) {
                fit_queued_.erase(it);
                break;
            }
        }
    }
    // Stop capture outside the lock (per channel: only channels with no source
    // left release their reader reference — the recorder may still hold the
    // channel alive through its own registration). Reader Stop joins the reader
    // threads; holding the lock would deadlock with in-flight callbacks waiting
    // on it. Called outside the lock, the join naturally finishes the current
    // callback before the threads exit.
    for (int ch : stopped_channels) {
        bool channel_empty = true;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            for (const SourceEntry& e : sources_) {
                if (e.channel == ch) {
                    channel_empty = false;
                    break;
                }
            }
        }
        if (channel_empty) {
            StopCapture(ch);
        }
    }
}

void CefVideoConsumer::ForEachSource(const std::function<void(obs_source_t*)>& fn)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const SourceEntry& e : sources_) {
        fn(e.source);
    }
}

void CefVideoConsumer::StartIfNeeded(int channel)
{
    if (bindings_[channel].channel() >= 0) {
        return; // already bound
    }
    if (!api_.Load()) {
        // Load is a no-op facade (no external DLL) and cannot fail; kept
        // for call-site symmetry. The source stays black if CefMedia Relay is
        // absent (recv timeouts), OBS reports no module error.
        return;
    }
    // Re-anchor this channel's output timeline on each capture session.
    timeline_[channel].Reset();
    // Bind the reader to this channel: registers the frame callbacks and starts
    // its receive threads (ChannelFrameBinding::Attach). The reader threads
    // retry with timeout polls internally: when the CefMedia Relay is not
    // running / restarts, OBS arriving first still attaches automatically.
    bindings_[channel].Attach(channel);
}

void CefVideoConsumer::StopCapture(int channel)
{
    // Idempotent full unbind: unregisters the frame callbacks (the pre-binding
    // code left them registered here, relying on the singleton lifetime and the
    // idempotent re-register in StartIfNeeded) and releases the reader
    // reference (joins the receive threads). Must be called OUTSIDE the lock.
    bindings_[channel].Detach();
}

void CefVideoConsumer::VideoCallback(cefmedia_video_frame* frame, const uint8_t* payload,
                                     void* obj, int channel)
{
    static_cast<CefVideoConsumer*>(obj)->OnVideoFrame(frame, payload, channel);
}

void CefVideoConsumer::AudioCallback(cefmedia_audio_buffer* buffer, const uint8_t* payload,
                                     void* obj, int channel)
{
    static_cast<CefVideoConsumer*>(obj)->OnAudioFrame(buffer, payload, channel);
}

void CefVideoConsumer::OnVideoFrame(cefmedia_video_frame* frame, const uint8_t* payload,
                                    int channel)
{
    if (frame->flags & CefFrameFlag_End) {
        // End-of-stream notification (header only, no picture data): the source
        // preview does not consume it (only the recorder's auto-stop does).
        return;
    }
    // Output timeline (per channel): absorb PTS forward jumps, produce continuous ns
    // timestamps (shares one base with audio on the same channel).
    const int64_t ns = timeline_[channel].OnVideoFrameUs(frame->timestamp);

    obs_source_frame obs_frame = {};
    if (!CefVideoToObsFrame(frame, payload, &obs_frame)) {
        return;
    }
    obs_frame.timestamp = static_cast<uint64_t>(ns);

    // Hold the lock while outputting: RemoveSource (main thread) must wait for the
    // in-flight callback to finish before dropping the source from the registry, so no
    // callback references the source pointer after OBS destroys it.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const SourceEntry& entry : sources_) {
        if (entry.channel != channel) {
            continue; // another channel's source: not ours to feed
        }
        if (entry.output_mode == kOutputAudioOnly) {
            continue; // audio-only source: no picture (auto-fit is moot too)
        }
        obs_source_output_video(entry.source, &obs_frame);
        MaybeQueueAutoFit(entry.source);
    }
}

void CefVideoConsumer::OnAudioFrame(cefmedia_audio_buffer* buffer, const uint8_t* payload,
                                    int channel)
{
    if (buffer->flags & CefFrameFlag_End) {
        return; // end-of-stream notification: no sample data
    }
    const int64_t ns = timeline_[channel].OnAudioFrameUs(buffer->timestamp);

    obs_source_audio obs_audio = {};
    if (!CefAudioToObsAudio(buffer, payload, &obs_audio)) {
        return;
    }
    obs_audio.timestamp = static_cast<uint64_t>(ns);

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const SourceEntry& entry : sources_) {
        if (entry.channel != channel) {
            continue;
        }
        if (entry.output_mode == kOutputVideoOnly) {
            continue; // video-only source: no audio (no doubled same-channel mix)
        }
        obs_source_output_audio(entry.source, &obs_audio);
    }
}

void CefVideoConsumer::MaybeQueueAutoFit(obs_source_t* source)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    for (obs_source_t* s : fit_queued_) {
        if (s == source) {
            return; // already queued / handled, no repeat
        }
    }

    // Only auto-fit sources with "Auto Fit to Screen" enabled; also mark disabled ones to
    // avoid re-checking every frame.
    if (!obs_data_get_bool(obs_source_get_settings(source), "auto_fit")) {
        fit_queued_.push_back(source);
        return;
    }
    // One fit per source ever (not per session): the persisted flag survives OBS
    // restarts, so a user-adjusted item is not snapped back to full screen every
    // launch. Deleting the source and re-adding it starts fresh (new settings).
    if (obs_data_get_bool(obs_source_get_settings(source), kFitDoneSetting)) {
        fit_queued_.push_back(source);
        return;
    }

    // Hold a reference while queued: the task runs on the OBS main thread and the source
    // may be destroyed meanwhile; get_ref keeps the memory valid.
    if (obs_source_get_ref(source) == NULL) {
        return;
    }
    fit_queued_.push_back(source);
    obs_queue_task(OBS_TASK_UI, &CefVideoConsumer::ApplyFitTask, source, false);
}

void CefVideoConsumer::ApplyFitTask(void* param)
{
    obs_source_t* source = static_cast<obs_source_t*>(param);
    if (source && obs_data_get_bool(obs_source_get_settings(source), "auto_fit")) {
        CefVideoConsumer::Instance().ApplyFitToScreen(source);
    }
    obs_source_release(source);
}

void CefVideoConsumer::ApplyFitToScreen(obs_source_t* source)
{
    FrontendApi& fe = FrontendApi::Instance();
    if (!fe.Load() || !fe.get_scenes) {
        return; // not a frontend environment (no obs-frontend-api) or not loaded: skip
    }

    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
        return;
    }

    ObsFrontendSourceList scenes = {};
    fe.get_scenes(&scenes);
    if (!scenes.array || scenes.num == 0) {
        return;
    }

    // Iterate all scenes (not only the current one), matching scene items by source
    // pointer.
    obs_sceneitem_t* target = nullptr;
    for (size_t i = 0; i < scenes.num; ++i) {
        obs_scene_t* scene = obs_scene_from_source(scenes.array[i]);
        if (!scene) {
            continue;
        }
        ItemFindCtx ctx = {source, nullptr};
        obs_scene_enum_items(scene, &CefVideoConsumer::FindSourceItem, &ctx);
        if (ctx.item) {
            target = ctx.item;
            break;
        }
    }

    // Release the list filled by obs_frontend_get_scenes: elements are addref'd.
    // Key: the array is allocated by obs's darray with _aligned_malloc (ALIGNED_MALLOC) —
    // it must be freed with bfree (->_aligned_free); CRT free() corrupts the heap
    // (0xc0000374).
    for (size_t i = 0; i < scenes.num; ++i) {
        obs_source_release(scenes.array[i]);
    }
    bfree(scenes.array);

    if (!target) {
        return;
    }

    // Fit to Screen = OBS_BOUNDS_SCALE_INNER + canvas size (matches the OBS UI). Under
    // bounds mode, later source resolution changes (quality switch) keep the item full
    // screen instead of drifting with the source.
    struct vec2 canvas = {static_cast<float>(ovi.base_width),
                          static_cast<float>(ovi.base_height)};
    obs_sceneitem_set_bounds_type(target, OBS_BOUNDS_SCALE_INNER);
    obs_sceneitem_set_bounds(target, &canvas);
    // Persist "fit done" in the source settings (saved with the scene collection):
    // next OBS start must not re-fit and reset the user's later manual placement.
    // obs_source_get_settings returns an addref'd reference — release it here.
    obs_data_t* settings = obs_source_get_settings(source);
    obs_data_set_bool(settings, kFitDoneSetting, true);
    obs_data_release(settings);
}

bool CefVideoConsumer::FindSourceItem(obs_scene_t*, obs_sceneitem_t* item, void* param)
{
    ItemFindCtx* ctx = static_cast<ItemFindCtx*>(param);
    if (obs_sceneitem_get_source(item) == ctx->source) {
        ctx->item = item;
        return false; // found, stop iterating
    }
    return true;
}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "cefmedia_source.h"

#include <obs-module.h>

#include "cefmedia_consumer.h"
#include "channel_status_tracker.h"
#include "i18n.h"

namespace {

const char* cefmedia_source_getname(void*)
{
    return CEFMEDIA_SOURCE_NAME;
}

// Source-settings key of the capture-channel selection. 1-based in the UI
// (1..5); internally mapped to the 0-based channel (value - 1) everywhere else.
constexpr const char* kChannelSetting = "channel";

// Source-settings key of the per-source stream filter (what this source
// outputs: both streams / video only / audio only). Values match
// CefVideoConsumer::SourceOutputMode; 0 = both keeps pre-existing sources
// (setting absent) behaving as before.
constexpr const char* kOutputModeSetting = "output_mode";

int ClampOutputMode(int64_t mode)
{
    if (mode < CefVideoConsumer::kOutputBoth ||
        mode > CefVideoConsumer::kOutputAudioOnly) {
        return CefVideoConsumer::kOutputBoth;
    }
    return static_cast<int>(mode);
}

// create/destroy/update run on the OBS main thread; async frames are pushed on the
// per-channel ipc receive threads, so the source only registers/unregisters on the
// singleton consumer with its channel binding.
void* cefmedia_source_create(obs_data_t* settings, obs_source_t* source)
{
    // Async source setup:
    //  - unbuffered=false: let OBS pace by frame timestamp (smooth delivery jitter, drop
    //    stale frames); NOT "always show latest", or CefMedia Relay-side fast-forward/stutter
    //    would pass through verbatim into scene recording.
    //  - decoupled=true: this source's audio/video share one output timeline aligned by
    //    the plugin itself; OBS does not need wall-clock timing_adjust.
    obs_source_set_async_unbuffered(source, false);
    obs_source_set_async_decoupled(source, true);

    int ch = static_cast<int>(obs_data_get_int(settings, kChannelSetting)) - 1;
    if (ch < 0 || ch >= CEFMEDIA_CHANNEL_COUNT) {
        ch = 0; // default channel 1 (0-based 0)
    }
    const int mode =
        ClampOutputMode(obs_data_get_int(settings, kOutputModeSetting));
    CefVideoConsumer::Instance().AddSource(source, ch, mode);
    UNUSED_PARAMETER(settings);
    return source;
}

void cefmedia_source_destroy(void* data)
{
    obs_source_t* source = reinterpret_cast<obs_source_t*>(data);
    // Note: no more obs_source_output_video may be called after destroy returns;
    // RemoveSource clears the reference under lock, so later frame-callback snapshots
    // no longer include this source.
    CefVideoConsumer::Instance().RemoveSource(source);
}

void cefmedia_source_update(void* data, obs_data_t* settings)
{
    // Channel change: rebind the source to the newly selected capture channel.
    // Output-mode change: per-source stream filter, takes effect from the next
    // frame on (no reader restart involved).
    // auto_fit is read at first-frame auto-fit time and needs no handling here.
    obs_source_t* source = reinterpret_cast<obs_source_t*>(data);
    if (!source) {
        return;
    }
    int ch = static_cast<int>(obs_data_get_int(settings, kChannelSetting)) - 1;
    if (ch < 0 || ch >= CEFMEDIA_CHANNEL_COUNT) {
        ch = 0;
    }
    CefVideoConsumer::Instance().UpdateSourceChannel(source, ch);
    CefVideoConsumer::Instance().UpdateSourceOutputMode(
        source, ClampOutputMode(obs_data_get_int(settings, kOutputModeSetting)));
}

static obs_properties_t* cefmedia_source_get_properties(void*)
{
    obs_properties_t* props = obs_properties_create();
    // Capture channel: which CefMedia Relay tab (the one holding this channel) this
    // source displays. CefMedia Relay assigns channels 1..5 to tabs as they enable
    // capture (the tab's favicon badge shows its number); the dock's occupancy
    // dots mirror the same state. Labels via the locale (T()) — en-US carries
    // the canonical strings, zh-CN overrides them.
    obs_property_t* channel = obs_properties_add_list(props, kChannelSetting,
                                                      T("Props.Channel"),
                                                      OBS_COMBO_TYPE_LIST,
                                                      OBS_COMBO_FORMAT_INT);
    obs_property_set_long_description(channel, T("Props.Channel.Hint"));
    // Each item embeds its live occupancy state in the NAME (filled circle =
    // a tab owns the channel, outlined = free — same colors/meaning as the
    // dock's dots; the OBS property API has no per-item icons). The stored
    // value stays the plain number, so settings/saved scenes are unaffected.
    // When the bitmap changes, ChannelStatusTracker refreshes any open
    // properties dialog (obs_source_update_properties -> form rebuild).
    const unsigned int bitmap = ChannelStatusTracker::Instance().Bitmap();
    for (int i = 1; i <= CEFMEDIA_CHANNEL_COUNT; ++i) {
        char label[8];
        snprintf(label, sizeof(label), "%s%d",
                 ((bitmap >> (i - 1)) & 1u) ? "● " : "○ ", i);
        obs_property_list_add_int(channel, label, i);
    }
    // Stream filter: which of the channel's streams this source outputs.
    // Both is the default (pre-existing sources keep their behavior); the two
    // only-modes exist to avoid a doubled same-channel audio mix when a tab
    // is on screen twice, and to let audio be captured without a picture.
    // Item labels reuse the recorder-output locale keys (same phrases).
    obs_property_t* output = obs_properties_add_list(
        props, kOutputModeSetting, T("Props.OutputContent"), OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(output, T("Props.Source.VideoAudio"),
                              CefVideoConsumer::kOutputBoth);
    obs_property_list_add_int(output, T("Props.Source.VideoOnly"),
                              CefVideoConsumer::kOutputVideoOnly);
    obs_property_list_add_int(output, T("Props.Source.AudioOnly"),
                              CefVideoConsumer::kOutputAudioOnly);
    obs_properties_add_bool(props, "auto_fit", T("Props.AutoFit"));
    return props;
}

static void cefmedia_source_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, kChannelSetting, 1);
    obs_data_set_default_int(settings, kOutputModeSetting,
                             CefVideoConsumer::kOutputBoth);
    obs_data_set_default_bool(settings, "auto_fit", true);
}

} // namespace

void RegisterCefMediaSource()
{
    static struct obs_source_info cefmedia_source_info = {};
    cefmedia_source_info.id = CEFMEDIA_SOURCE_ID;
    cefmedia_source_info.type = OBS_SOURCE_TYPE_INPUT;
    cefmedia_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
    cefmedia_source_info.get_name = cefmedia_source_getname;
    cefmedia_source_info.create = cefmedia_source_create;
    cefmedia_source_info.destroy = cefmedia_source_destroy;
    cefmedia_source_info.update = cefmedia_source_update;
    cefmedia_source_info.get_properties = cefmedia_source_get_properties;
    cefmedia_source_info.get_defaults = cefmedia_source_get_defaults;
    cefmedia_source_info.icon_type = OBS_ICON_TYPE_CAMERA;

    obs_register_source(&cefmedia_source_info);
}

void UnregisterCefMediaSource()
{
    // Source types registered via obs_register_source are owned by libobs; there is no
    // explicit unregister — libobs releases them on module unload.
}

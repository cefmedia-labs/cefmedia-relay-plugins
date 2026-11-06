#ifndef _CEFMEDIA_OBS_H
#define _CEFMEDIA_OBS_H

// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository, licensed under GPL-2.0 (see the repository LICENSE).

// OBS-side interface header. The OBS plugin links libipc directly and talks to
// the CefMedia Relay process over the fixed-name channels ("cefmedia.video" /
// "cefmedia.audio" receive, "cefmedia.rate" send; see cefmedia_ipc_client.h).
// This header only carries the frame contract types the consumers use.
//
// Up to CEFMEDIA_CHANNEL_COUNT tabs capture in
// parallel. Channel identity rides on the transport names — the video/audio
// broadcast routes exist per channel ("cefmedia.video.<ch>" /
// "cefmedia.audio.<ch>", <ch> 0-based 0..4; see CefMediaIpcClient), the rate
// message routes by its channel field, and CefMedia Relay broadcasts the channel
// occupancy bitmap over the "cefmedia.status" channel. Frame callbacks carry
// the 0-based channel of the route they were received on (the per-channel
// receive thread of CefMediaIpcClient stamps it); the status callback carries
// the occupancy bitmap (bit i set = channel i owned by a capturing tab).
//
// Struct layouts MUST stay identical to the CEF-side copies of
// cefmedia_frames.h, or the wire format misaligns.

#include "cefmedia_frames.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef void (*VideoFrameCallback)(cefmedia_video_frame* videoFrame,
                                   const uint8_t* payload, void* obj, int channel);
typedef void (*AudioBufferCallback)(cefmedia_audio_buffer* audioBuffer,
                                    const uint8_t* payload, void* obj, int channel);
// channel_bitmap: bit i (0-based) set = capture channel i is owned by a tab.
typedef void (*ChannelStatusCallback)(unsigned int channel_bitmap, void* obj);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _CEFMEDIA_FRAMES_H
#define _CEFMEDIA_FRAMES_H

// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository, licensed under GPL-2.0 (see the repository LICENSE).

// ---------------------------------------------------------------------------
// cefmedia frame contract.
//
// One media frame travels as ONE contiguous memory block:
//   [fixed-size header][payload bytes]
// The payload starts at (block address + header size) — derivable on both ends
// from the fixed header size (CEFMEDIA_VIDEO_HEADER_SIZE /
// CEFMEDIA_AUDIO_HEADER_SIZE). Header and payload are atomic: one block is one
// complete frame, never split into separate header/payload messages.
//
// Channels: up to CEFMEDIA_CHANNEL_COUNT tabs capture simultaneously. Channel
// identity rides on the transport names (shm segment suffix / libipc route
// suffix), NOT on the frame headers — video/audio headers carry no channel
// field. The rate message carries a channel routing field, and a status
// message broadcasts the channel-occupancy bitmap to OBS.
//
// Format enums are two-sided: the renderer (A side) writes Chromium media::
// enum integers; the relay translates them to FFmpeg enums (same data layout,
// a pure tag remap) before frames reach OBS. Unsupported mappings are the
// *_NONE sentinels (consumers drop such frames).
//
// This header is duplicated on both sides of the transport (CEF side and this
// plugin-side copy) and the layouts MUST stay identical (shared-memory /
// wire alignment).
// ---------------------------------------------------------------------------

#include <stdint.h>

// Fixed header sizes (bytes). Part of the wire format: the payload area of a
// frame block always starts at (block address + header size). Pinned to 96 to
// leave room for future fields; the static_asserts at the bottom of this
// header fail the build if a field edit drifts the actual struct size.
#define CEFMEDIA_VIDEO_HEADER_SIZE 96
#define CEFMEDIA_AUDIO_HEADER_SIZE 96

// Channel count. Channels are numbered 1..5 in
// the UI; internally (rate messages, shm/route name suffixes) they are
// 0-based (0..4). Channel resources are created on demand: CefMedia Relay
// process calls cefmedia_channel_start(ch) when a tab acquires channel ch and
// cefmedia_channel_stop(ch) when it releases it.
#define CEFMEDIA_CHANNEL_COUNT 5

// libipc channel names (relay<->OBS). The video/audio routes
// exist per channel -- the sender side (relay) appends the 0-based channel
// suffix, e.g. "cefmedia.video.0" .. "cefmedia.video.4" (see
// CefMediaChannelName helpers in cefmediaipc / cefmedia_ipc_client). The
// rate and status channels stay single fixed names (multiplexed by message
// fields, see cefmedia_rate_msg / cefmedia_status_msg).
#define CEFMEDIA_IPC_VIDEO_CHANNEL "cefmedia.video"
#define CEFMEDIA_IPC_AUDIO_CHANNEL "cefmedia.audio"
#define CEFMEDIA_IPC_RATE_CHANNEL  "cefmedia.rate"
#define CEFMEDIA_IPC_STATUS_CHANNEL "cefmedia.status"

#ifdef __cplusplus
extern "C" {
#endif

// Frame flag bits (carry the old isEnded; isAd is reserved).
enum {
	CefFrameFlag_None = 0,
	CefFrameFlag_End  = 1 << 0, // end-of-stream notification: header only, no payload
	CefFrameFlag_Ad   = 1 << 1, // reserved: ad-showing at capture time (the renderer
	                            // currently filters ads at the source, so this stays 0
	                            // until the ad path needs to signal mid-stream)
	CefFrameFlag_Drm  = 1 << 2, // DRM-protected content: payload is black; the relay
	                            // replaces it with a "DRM protected" placeholder
	                            // before delivery (EME streams are never captured)
};

// Explicit planar layout. Video payload bytes are laid out as per-plane
// blocks at plane[i].offset.
#define CEFMEDIA_MAX_PLANES 3

typedef struct {
	uint32_t offset; // byte offset of this plane inside the payload area
	uint32_t stride; // row pitch in bytes
	uint32_t size;   // plane size in bytes
} cefmedia_plane;

// Video frame header (96 bytes, see CEFMEDIA_VIDEO_HEADER_SIZE).
// The payload area of a video frame block starts at
// (block address + CEFMEDIA_VIDEO_HEADER_SIZE).
typedef struct {
	int      format;       // A side: Chromium media::VideoPixelFormat enum int;
	                       // B side (relay-translated): FFmpeg AVPixelFormat enum
	                       // int (same data layout, the relay only remaps the
	                       // enum). AV_PIX_FMT_NONE = unsupported -> drop.
	int      width;
	int      height;
	float    frame_rate;
	int64_t  timestamp;    // microseconds, source PTS
	uint32_t flags;        // CefFrameFlag_* bits
	uint8_t  plane_count;  // 1..CEFMEDIA_MAX_PLANES
	uint8_t  reserved[31]; // pad to the fixed 96-byte header (room to grow)
	cefmedia_plane planes[CEFMEDIA_MAX_PLANES];
} cefmedia_video_frame;

// The payload area capacity for video.
#define CEFMEDIA_VIDEO_PAYLOAD_MAX (1024 * 1024 * 60) // 60MB

// Audio buffer header (96 bytes, see CEFMEDIA_AUDIO_HEADER_SIZE).
// The payload area of an audio frame block starts at
// (block address + CEFMEDIA_AUDIO_HEADER_SIZE).
typedef struct {
	int      format;         // A side: Chromium media::SampleFormat enum int;
	                         // B side (relay-translated): FFmpeg AVSampleFormat
	                         // enum int. AV_SAMPLE_FMT_NONE = unsupported.
	int      channel_layout;
	int      channel_count;
	int      sample_rate;
	int64_t  timestamp;      // microseconds
	int64_t  duration;       // microseconds
	uint32_t flags;          // CefFrameFlag_* bits
	uint32_t data_size;      // total payload bytes (all channels)
	                         // Planar audio: channel i starts at
	                         // i * (data_size / channel_count) - the same
	                         // per-channel block layout the old data[] used.
	uint8_t  reserved[56];   // pad to the fixed 96-byte header (room to grow)
} cefmedia_audio_buffer;

// The payload area capacity for audio.
#define CEFMEDIA_AUDIO_PAYLOAD_MAX (1024 * 1024 * 8) // 8MB

// Speed-control message (OBS -> CefMedia Relay, libipc channel "cefmedia.rate"):
// a single fixed-size struct sent as one message. CefMedia Relay receives the
// rate as a push (no polling latency) and a libipc reconnect survives CefMedia
// Relay restarts.
//
// |channel| routes the rate to one capture channel (0-based
// 0..4; CefMedia Relay applies it to the tab owning that channel). Senders
// that leave channel=0 keep working against channel 0.
typedef struct {
	int32_t rate_permille; // playback rate in permille (1000 = 1x)
	int32_t channel;       // 0-based capture channel this rate applies to
	uint8_t reserved[88];  // pad to 96 bytes (room for future commands; a
	                       // future command set would add a cmd tag first)
} cefmedia_rate_msg;

// Channel-occupancy status message (CefMedia Relay -> OBS, libipc channel
// "cefmedia.status"): one fixed-size struct. CefMedia Relay sends
// it whenever the channel ownership bitmap changes and re-sends it on a slow
// timer so a late-started OBS learns the current state.
typedef struct {
	uint32_t channel_bitmap; // bit i set = channel i (0-based) is owned by a
	                         // capturing tab (bits 5..31 reserved, always 0)
	uint8_t  reserved[92];   // pad to 96 bytes
} cefmedia_status_msg;

#ifdef __cplusplus
}
#endif

// Layout guards (after the type definitions): the header sizes are part of
// the wire format, so a field edit that silently changes sizeof fails the
// build instead of misaligning the shared memory / message stream.
#ifdef __cplusplus
static_assert(sizeof(cefmedia_video_frame) == CEFMEDIA_VIDEO_HEADER_SIZE,
              "cefmedia_video_frame layout drifted");
static_assert(sizeof(cefmedia_audio_buffer) == CEFMEDIA_AUDIO_HEADER_SIZE,
              "cefmedia_audio_buffer layout drifted");
static_assert(sizeof(cefmedia_rate_msg) == CEFMEDIA_AUDIO_HEADER_SIZE,
              "cefmedia_rate_msg layout drifted");
static_assert(sizeof(cefmedia_status_msg) == CEFMEDIA_AUDIO_HEADER_SIZE,
              "cefmedia_status_msg layout drifted");
#endif

#endif // _CEFMEDIA_FRAMES_H

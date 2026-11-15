// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_CHANNEL_FRAME_BINDING_H
#define CEFMEDIA_CHANNEL_FRAME_BINDING_H

#include "cefmedia.h" // frame contract + callback typedefs

// ---------------------------------------------------------------------------
// ChannelFrameBinding — owns ONE consumer's binding to ONE capture channel.
//
// Registers the video (and optionally audio) frame callbacks with the ipc
// client and holds one reader reference. The pairing invariants every consumer
// previously kept BY HAND live here:
//   - every Set*FrameCallback gets a matching Remove*FrameCallback BEFORE the
//     registered |obj| lifetime ends (a left-behind entry is a dangling
//     pointer the next frame would call);
//   - every ReaderStart gets a matching ReaderStop (per-channel receive
//     threads are refcounted: first start spawns, last stop joins);
//   - destruction detaches first — the mandatory unregister is impossible
//     to forget.
//
// NOT a fit for the cefmedia_detect.cpp frame probe: that one is a module-
// lifetime sniffer (no reader reference, obj = nullptr, never unregisters).
//
// Threading: Attach/Detach must be serialized per binding by the caller — the
// OBS UI thread, or a caller-owned worker that keeps Detach's ReaderStop join
// off the UI thread (the channel preview does exactly that). The ipc client
// serializes the resulting spawn/join against concurrent ReaderStart/Stop from
// other consumers (per-channel reader mutex).
//
// Ownership note: the binding stores the raw |obj| it is constructed with.
// As a class member it must therefore be declared LAST (members destruct in
// reverse order) so Detach runs while the owner's other members — and the
// owner subobject the callbacks cast to — are still alive.
// ---------------------------------------------------------------------------
class ChannelFrameBinding {
public:
    // |audio| may be nullptr for a video-only binding (no audio callback is
    // registered; the reader still runs both receive threads for the channel —
    // that is the ipc client's granularity).
    ChannelFrameBinding(VideoFrameCallback video, AudioBufferCallback audio,
                        void* obj);

    ChannelFrameBinding(const ChannelFrameBinding&) = delete;
    ChannelFrameBinding& operator=(const ChannelFrameBinding&) = delete;

    ~ChannelFrameBinding(); // Detach()

    // Register the callbacks + take the reader reference. Idempotent for the
    // same channel; attaching to another channel detaches first.
    // |channel| < 0 = detach request (mirrors Detach()).
    void Attach(int channel);

    // Remove the callbacks + release the reader reference. Idempotent; safe
    // on a never-attached binding.
    void Detach();

    // 0-based bound channel; -1 = detached.
    int channel() const { return channel_; }

private:
    VideoFrameCallback video_;
    AudioBufferCallback audio_;
    void* obj_;
    int channel_ = -1;
};

#endif // CEFMEDIA_CHANNEL_FRAME_BINDING_H

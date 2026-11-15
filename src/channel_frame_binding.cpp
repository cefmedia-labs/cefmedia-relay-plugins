// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "channel_frame_binding.h"

#include "cefmedia_api.h"

ChannelFrameBinding::ChannelFrameBinding(VideoFrameCallback video,
                                         AudioBufferCallback audio, void* obj)
    : video_(video), audio_(audio), obj_(obj)
{
}

ChannelFrameBinding::~ChannelFrameBinding()
{
    Detach(); // the mandatory unregister can no longer be forgotten
}

void ChannelFrameBinding::Attach(int channel)
{
    if (channel_ == channel)
    {
        return; // idempotent (re-Attach on the bound channel is a no-op)
    }
    Detach();
    if (channel < 0)
    {
        return; // negative channel = detach request
    }
    CefMediaApi::Instance().SetVideoFrameCallback(video_, obj_, channel);
    if (audio_)
    {
        CefMediaApi::Instance().SetAudioFrameCallback(audio_, obj_, channel);
    }
    CefMediaApi::Instance().ReaderStart(channel);
    channel_ = channel;
}

void ChannelFrameBinding::Detach()
{
    if (channel_ < 0)
    {
        return;
    }
    const int channel = channel_;
    channel_ = -1; // first: a re-entrant Detach sees a detached binding
    // Order matters: unregister the callbacks BEFORE joining the receive
    // threads (ReaderStop), so no callback can fire into a half-torn-down
    // consumer between the two steps.
    CefMediaApi::Instance().RemoveVideoFrameCallback(video_, obj_, channel);
    if (audio_)
    {
        CefMediaApi::Instance().RemoveAudioFrameCallback(audio_, obj_, channel);
    }
    CefMediaApi::Instance().ReaderStop(channel); // joins the receive threads
}

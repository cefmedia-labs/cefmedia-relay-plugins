// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_API_H
#define CEFMEDIA_API_H

#include "cefmedia.h"

// Facade kept from the cefmedia.dll-loader era (all call sites unchanged):
// everything now forwards to CefMediaIpcClient (libipc linked directly into
// this plugin). "Load" always succeeds — there is no external DLL anymore;
// CefMedia Relay-not-running case is handled inside the client (recv timeouts /
// silent rate sends).
//
// 5-channel model: frame callbacks / reader lifecycle / rate all
// take a 0-based capture channel (0..4, clamped); a status callback observes
// CefMedia Relay's channel-occupancy bitmap.
class CefMediaApi {
public:
    static CefMediaApi& Instance();

    // Always true (kept for call-site compatibility; no external DLL to load).
    bool Load();

    bool IsLoaded() const { return true; }

    // ---- frame callbacks + receive lifecycle (forwarded to CefMediaIpcClient) ----
    void SetVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel);
    void SetAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel);
    void SetStatusCallback(ChannelStatusCallback callback, void* obj);
    // Mandatory before the registered |obj| lifetime ends (output stop/destroy):
    // a left-behind entry would be a dangling pointer on the next frame.
    void RemoveVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel);
    void RemoveAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel);
    void RemoveStatusCallback(ChannelStatusCallback callback, void* obj);
    void ReaderStart(int channel);
    void ReaderStop(int channel);
    void StatusStart();
    void StatusStop();

    // ---- speed control (OBS -> CefMedia Relay, libipc "cefmedia.rate"; channel-routed) ----
    // |recording| rides the rate message (recording-state signalling): true on
    // the dock heartbeat / speed changes while recording, false on every
    // stop/restore-1x path.
    void RateSet(int rate_permille, int channel, bool recording);

private:
    CefMediaApi() = default;
    ~CefMediaApi() = default;
    CefMediaApi(const CefMediaApi&) = delete;
    CefMediaApi& operator=(const CefMediaApi&) = delete;
};

#endif // CEFMEDIA_API_H

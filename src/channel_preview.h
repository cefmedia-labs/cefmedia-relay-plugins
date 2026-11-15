// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_CHANNEL_PREVIEW_H
#define CEFMEDIA_CHANNEL_PREVIEW_H

// Qt quarantine note: the plugin deliberately restricts Qt to designated
// translation units (currently recorder_dock.cpp and this file) so the rest of
// the codebase stays Qt-free. No moc anywhere: no Q_OBJECT — signals are wired
// via lambdas and plain std::function hooks.

#include <QtGui/QImage>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QWidget>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "cefmedia.h" // frame contract (cefmedia_frames.h): cefmedia_video_frame is a
                      // typedef'd anonymous struct — NOT forward-declarable, hence the
                      // real header here (plain C, no side effects)
#include "channel_frame_binding.h" // callback + reader lifecycle (RAII)

struct SwsContext;   // ffmpeg, opaque (defined in libswscale/swscale.h)

// ---------------------------------------------------------------------------
// ChannelPreviewWindow — the Channel combo's live preview popup.
//
// While the dock's Channel dropdown is open, this small frameless always-on-top
// tool window renders the video of the item the user is highlighting (hover /
// keyboard) — the content follows the highlighted channel in real time; a free
// channel shows black (no reader threads are spawned for it). The popup dies
// with the dropdown (video only — audio stays with the source/output plugins).
// One window at a time, owned by RecorderPanel (raw pointer; WA_DeleteOnClose +
// a destroyed() hook keep the panel pointer honest).
//
// Frame path: the per-channel receive thread hands the frame to OnFrame, which
// converts it (swscale, BT.709 limited -> full-range BGRA, matching what
// frame_convert.cpp hands OBS) into a mutex-guarded QImage slot; a ~30 fps
// QTimer on the UI thread picks up the latest converted frame and repaints.
// The callback only converts-and-stores (never encodes, never touches widgets)
// per the ipc client's fan-out contract; a slow UI thread just means skipped
// intermediate frames, never backpressure.
//
// Channel lifecycle (callback registration + reader reference) is owned by the
// ChannelFrameBinding member — it MUST stay the last member (reverse
// destruction order: it detaches while everything it touches is still alive),
// which also makes an explicit destructor unnecessary.
// ---------------------------------------------------------------------------
class ChannelPreviewWindow : public QWidget
{
public:
    ChannelPreviewWindow();
    ~ChannelPreviewWindow();

    ChannelPreviewWindow(const ChannelPreviewWindow&) = delete;
    ChannelPreviewWindow& operator=(const ChannelPreviewWindow&) = delete;

    // Live-follow entry point (the dropdown's highlighted item): frames flow
    // only when |occupied| is true — a free channel shows black (nothing to
    // bind, no reader threads spawned). The caller owns the occupancy decision
    // (the CefMedia Relay bitmap is the dock's state, not this window's).
    // Asynchronous: the rebind is posted to the binding worker (see below), so
    // this returns immediately and the popup shows black until the new
    // channel's first frame lands.
    void FollowChannel(int channel, bool occupied);

private:
    static void OnFrameThunk(cefmedia_video_frame* videoFrame, const uint8_t* payload,
                             void* obj, int channel);

    // Receive thread: validate, convert into the guarded slot. Returns fast on
    // anything unsupported (same drop-on-unsupported rule as every consumer).
    void OnFrame(cefmedia_video_frame* header, const uint8_t* payload);

    // Binding worker (the only thread that touches binding_): executes the
    // latest posted FollowChannel command. Attaching/detaching here keeps the
    // ReaderStop join inside Detach — up to ~100 ms when the preview holds the
    // channel's last reader reference — off the UI thread.
    void BindingWorkerLoop();

    void paintEvent(QPaintEvent* event) override;

    // Drop the guarded frame slot (shown_ is UI-thread-only; frame_ is the
    // receive-thread side — both under the mutex; the next paint shows black).
    void ClearFrameSlot();

    std::mutex mutex_;      // guards the receive-thread <-> UI-thread frame slot
    QImage frame_;          // latest converted frame (receive thread writes)
    QImage shown_;          // frame being displayed (UI thread only)
    bool frame_ready_ = false;
    SwsContext* sws_ = nullptr;
    int src_fmt_ = -1;      // AVPixelFormat of the current converter
                            // (AV_PIX_FMT_NONE == -1; int to keep ffmpeg out of
                            // this header)
    int src_w_ = 0;
    int src_h_ = 0;
    QTimer* paint_timer_ = nullptr;
    // Deferred binding commands (UI thread posts, BindingWorkerLoop consumes;
    // rapid hover switches coalesce — only the latest command stays pending).
    std::thread worker_;
    std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    bool cmd_pending_ = false;
    int cmd_channel_ = -1;
    bool cmd_occupied_ = false;
    bool worker_exit_ = false;
    // Currently-followed target, UI thread only (dedupes the repeated
    // highlighted/entered events the combo emits for the same item).
    int followed_channel_ = -1;
    bool followed_occupied_ = false;
    // Last member (see the lifecycle note above): video-only binding — the
    // preview registers no audio callback. Only BindingWorkerLoop and the
    // destructor touch it.
    ChannelFrameBinding binding_;
};

// QComboBox with popup open/close hooks: virtual overrides (no Q_OBJECT/moc —
// the panel installs plain std::function callbacks). The live channel preview
// tracks the dropdown lifecycle: open = show the popup, close = dismiss it.
class ChannelCombo : public QComboBox
{
public:
    using QComboBox::QComboBox;
    std::function<void()> popup_shown;  // installed by RecorderPanel
    std::function<void()> popup_hidden;

protected:
    void showPopup() override;
    void hidePopup() override;
};

#endif // CEFMEDIA_CHANNEL_PREVIEW_H

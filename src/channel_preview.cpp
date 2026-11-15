// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "channel_preview.h"

#include <QtGui/QPainter>

#include "cefmedia_api.h"

extern "C" {
#include <libswscale/swscale.h> // YUV->RGB conversion (swscale is already linked
                                // for the recorder engine — no new dependency)
}

// ---------------------------------------------------------------------------
// ChannelPreviewWindow (declaration/docs in channel_preview.h)
// ---------------------------------------------------------------------------

ChannelPreviewWindow::ChannelPreviewWindow()
    : QWidget(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint),
      // Video-only binding: the preview registers no audio callback (the
      // reader still runs both receive threads for the channel — ipc client
      // granularity). Last member: detaches first on destruction.
      binding_(&ChannelPreviewWindow::OnFrameThunk, nullptr, this)
{
    resize(320, 180); // frameless: no title bar, no user resize/move —
                      // the panel positions it against the Channel combo

    // UI-thread paint pump: swap in the latest converted frame (shallow copy —
    // QImage uses atomic refcounts, so the cross-thread handoff under the mutex
    // is safe) and schedule a repaint.
    paint_timer_ = new QTimer(this);
    paint_timer_->setInterval(33); // ~30 fps; the source delivers 25-60 fps
    connect(paint_timer_, &QTimer::timeout, this, [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frame_ready_)
            {
                // SWAP, not copy: a QImage assignment would push frame_'s
                // refcount to 2, making the receive thread's next non-const
                // frame_.bits() detach — a full-frame memcpy under the mutex on
                // every frame. Swapping the buffers keeps both at refcount 1:
                // shown_ gets the new frame, and the receive thread overwrites
                // the recycled buffer in place.
                std::swap(shown_, frame_);
                frame_ready_ = false;
            }
        }
        update();
    });
    paint_timer_->start();

    worker_ = std::thread(&ChannelPreviewWindow::BindingWorkerLoop, this);
}

ChannelPreviewWindow::~ChannelPreviewWindow()
{
    // Stop the worker first (it touches binding_), then unregister the frame
    // callbacks BEFORE the data members die — between member destruction and
    // binding_'s own destructor-time Detach a receive thread could otherwise
    // fire OnFrame into half-destroyed members.
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        worker_exit_ = true;
    }
    cmd_cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    binding_.Detach();
}

void ChannelPreviewWindow::BindingWorkerLoop()
{
    for (;;)
    {
        int channel;
        bool occupied;
        {
            std::unique_lock<std::mutex> lock(cmd_mutex_);
            cmd_cv_.wait(lock, [this] { return cmd_pending_ || worker_exit_; });
            if (worker_exit_)
            {
                return;
            }
            channel = cmd_channel_;
            occupied = cmd_occupied_;
            cmd_pending_ = false;
        }
        // The ReaderStop join inside Detach (preview = last reader reference on
        // the channel) and the receive-thread spawn inside ReaderStart both run
        // here — the UI thread only posted the command and moved on.
        if (occupied && channel >= 0)
        {
            binding_.Attach(channel);
        }
        else
        {
            binding_.Detach();
        }
    }
}

void ChannelPreviewWindow::FollowChannel(int channel, bool occupied)
{
    if (channel == followed_channel_ && occupied == followed_occupied_)
    {
        return; // repeated highlight events for the same item
    }
    followed_channel_ = channel;
    followed_occupied_ = occupied;
    // The rebind runs asynchronously on the worker, so old-channel frames can
    // still arrive in the gap — drop the slot now so the popup shows black
    // until the followed channel's own first frame lands (content always
    // matches the highlighted item, never a stale glimpse of the previous one).
    ClearFrameSlot();
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        cmd_channel_ = channel;
        cmd_occupied_ = occupied;
        cmd_pending_ = true;
    }
    cmd_cv_.notify_one();
}

void ChannelPreviewWindow::ClearFrameSlot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = QImage();
    shown_ = QImage();
    frame_ready_ = false;
}

void ChannelPreviewWindow::OnFrameThunk(cefmedia_video_frame* videoFrame,
                                        const uint8_t* payload, void* obj,
                                        int /*channel*/)
{
    static_cast<ChannelPreviewWindow*>(obj)->OnFrame(videoFrame, payload);
}

void ChannelPreviewWindow::OnFrame(cefmedia_video_frame* header, const uint8_t* payload)
{
    if (!header || !payload)
    {
        return;
    }
    if (header->flags & CefFrameFlag_End)
    {
        return; // header-only end-of-stream notification, nothing to draw
    }
    const AVPixelFormat src = static_cast<AVPixelFormat>(header->format);
    if (src == AV_PIX_FMT_NONE || header->width <= 0 || header->height <= 0 ||
        header->width > 8192 || header->height > 8192 || header->plane_count == 0 ||
        header->plane_count > CEFMEDIA_MAX_PLANES)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // (Re)build the converter + the write buffer when anything mismatches.
    // The buffer MUST match the converter's geometry on every frame: the UI
    // timer swaps buffers (double-buffering, no COW copy), so frame_ can hold
    // a PREVIOUS channel's / previous size's image after a rebind — non-null
    // but the wrong size, which would overflow it in sws_scale below. Hence
    // frame_'s own geometry is part of the check (a null QImage has 0x0).
    if (!sws_ || static_cast<int>(src) != src_fmt_ || header->width != src_w_ ||
        header->height != src_h_ || frame_.width() != src_w_ ||
        frame_.height() != src_h_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
        src_fmt_ = static_cast<int>(src);
        src_w_ = header->width;
        src_h_ = header->height;
        frame_ = QImage(src_w_, src_h_, QImage::Format_ARGB32); // BGRA on LE
        if (frame_.isNull())
        {
            return;
        }
        sws_ = sws_getContext(src_w_, src_h_, src, src_w_, src_h_, AV_PIX_FMT_BGRA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_)
        {
            return; // unsupported pixel format — same drop rule as the relay
        }
        // Capture YUV is BT.709 LIMITED range (the frame contract's color
        // statement, see frame_convert.cpp); sws would otherwise assume
        // BT.601 and HD content renders washed out.
        const int* coefs = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(sws_, coefs, 1 /*src limited*/, coefs,
                                 0 /*dst full (RGB)*/, 0, 1 << 16, 1 << 16);
    }

    const uint8_t* src_data[CEFMEDIA_MAX_PLANES] = {};
    int src_stride[CEFMEDIA_MAX_PLANES] = {};
    for (int i = 0; i < header->plane_count; ++i)
    {
        if (header->planes[i].stride <= 0)
        {
            return;
        }
        src_data[i] = payload + header->planes[i].offset;
        src_stride[i] = static_cast<int>(header->planes[i].stride);
    }
    // frame_ is guaranteed non-null and src-sized here (the rebuild check above
    // covers the swapped-in stale/empty buffer cases).
    uint8_t* dst_data[4] = {frame_.bits(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {static_cast<int>(frame_.bytesPerLine()), 0, 0, 0};
    if (sws_scale(sws_, src_data, src_stride, 0, src_h_, dst_data, dst_stride) ==
        src_h_)
    {
        frame_ready_ = true;
    }
}

void ChannelPreviewWindow::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (shown_.isNull())
    {
        return; // no data (free channel / not yet bound / stream stalled): black
    }
    // Aspect-preserving fit with letterbox. Scale in device pixels so the
    // output is crisp on HiDPI, then paint in logical pixels.
    const QSizeF target = QSizeF(size()) * devicePixelRatioF();
    QImage scaled = shown_.scaled(
        QSize(static_cast<int>(target.width()), static_cast<int>(target.height())),
        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(devicePixelRatioF());
    painter.drawImage((width() - scaled.width() / devicePixelRatioF()) / 2.0,
                      (height() - scaled.height() / devicePixelRatioF()) / 2.0,
                      scaled);
}

// ---------------------------------------------------------------------------
// ChannelCombo (declaration/docs in channel_preview.h)
// ---------------------------------------------------------------------------

void ChannelCombo::showPopup()
{
    QComboBox::showPopup();
    if (popup_shown)
    {
        popup_shown();
    }
}

void ChannelCombo::hidePopup()
{
    QComboBox::hidePopup();
    if (popup_hidden)
    {
        popup_hidden();
    }
}

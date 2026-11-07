#include "cefmedia_recorder.h"

#include <obs-module.h>

#include <chrono>

namespace {

// The received frame headers carry FFmpeg format enums directly (AVPixelFormat /
// AVSampleFormat): the relay translates the renderer's Chromium media:: enum
// integers before frames enter its ring buffer, and maps unsupported formats to
// the *_NONE sentinel. So TakeVideoFrame/TakeAudioFrame just cast the tag and
// validate; no Chromium->FFmpeg mapping table is needed here.
AVPixelFormat MapVideoFormat(int fmt)
{
    return static_cast<AVPixelFormat>(fmt);
}

AVSampleFormat MapAudioFormat(int fmt)
{
    return static_cast<AVSampleFormat>(fmt);
}

// Per-plane "bytes per pixel x plane width x plane height" (Chromium 4:2:0 layout).
struct PlaneSpec { int bpp; int pw; int ph; };

bool PlaneSpecs(AVPixelFormat fmt, int w, int h, PlaneSpec out[4], int* planes)
{
    switch (fmt) {
    case AV_PIX_FMT_YUV420P:
        out[0] = {1, w, h};
        out[1] = {1, w / 2, h / 2};
        out[2] = {1, w / 2, h / 2};
        *planes = 3;
        return true;
    case AV_PIX_FMT_NV12:
        out[0] = {1, w, h};
        out[1] = {2, w / 2, h / 2}; // interleaved UV, 2 bytes per pixel
        *planes = 2;
        return true;
    case AV_PIX_FMT_YUV420P10LE:
        out[0] = {2, w, h};
        out[1] = {2, w / 2, h / 2};
        out[2] = {2, w / 2, h / 2};
        *planes = 3;
        return true;
    case AV_PIX_FMT_P010LE:
        out[0] = {2, w, h};
        out[1] = {4, w / 2, h / 2}; // interleaved UV, 10-bit 4 bytes per pixel
        *planes = 2;
        return true;
    default:
        return false;
    }
}

constexpr int64_t kSyncGapThresholdUs = 30000;   // pad silence only when audio lags video >30 ms
constexpr int64_t kMaxSyncGapUs = 2 * 1000000;   // max silence per pad, 2 s
constexpr int64_t kFirstFrameTimeoutMs = 1000;   // first-frame single-side timeout
// CFR interval measurement window (adapted CFR): wait this long / this many frames for
// the header fps to stabilize before opening the encode chain. The renderer's
// FrameRateEstimator needs 3-15 samples to converge, so 1 s @30 fps (30 frames) is
// comfortably above convergence; kFpsStableRun identical values guard against a
// mid-window rate flip.
constexpr int64_t kFpsMeasureWindowMs = 1000;    // max wait for a stable fps estimate
constexpr int kFpsMeasureMaxFrames = 64;         // hard bound on buffered frames
constexpr int kFpsStableRun = 8;                 // consecutive identical fps values needed
// Backward video PTS jump above this = a real seek / next-video switch (source PTS
// restarts from ~0, or the user scrubs) -> re-anchor the timeline (continuous-timeline
// mode). Below it = out-of-order delivery jitter -> drop the frame (keeps container PTS
// monotonic for the muxer).
constexpr int64_t kBackwardReanchorUs = 500000;
// Forward video PTS jump above this = a real seek forward (user drags the progress
// bar) -> absorb it (re-anchor with segment accounting: the container timeline
// continues at the current end, the skipped span simply does not exist). Without
// this the jump becomes a container hole and every following frame pads silence
// against it (a real session showed a ~3574 s silent stretch). At or below it the
// normal CFR gap fill bridges the step (dropped frames / ad-gap jitter).
constexpr int64_t kForwardReanchorUs = 1000000;  // 1 s (manual-seek granularity)
// How long after a re-anchor the queue head keeps being swept for late old-source
// audio chunks (they traverse the relay pipeline for a few seconds after the switch).
constexpr int64_t kAudioSweepWindowMs = 5000;
// Forward-sweep tolerance: chunks newer than cut - this may already be post-cut
// content (the decoder can emit the new position immediately) — only chunks older
// than that are unambiguous pre-cut residue.
constexpr int64_t kForwardSweepTolUs = 200000;
// Media-clock crawl: after a seek/quality switch the CefMedia Relay media clock can advance
// in sub-millisecond steps (< 2 ms per frame, measured 0.26 ms) while frames keep
// arriving at full rate — the container timeline would compress thousands of real
// frames into a few seconds. Below this per-frame source delta counts as a crawl
// step (normal cadences are 8.3 ms @120fps to 40 ms @25fps, well above).
constexpr int64_t kCrawlDeltaUs = 2000;
// Consecutive crawl steps before pacing kicks in (isolated single micro-steps exist
// on healthy streams too).
constexpr int kCrawlRunMin = 5;
// Source-domain alignment hold waiver: the audio capture point is the DECODE side,
// which at >1x playback runs a bounded media-time lead ahead of the render-side video
// (~2.5 s at 3x, measured). A chunk whose src PTS is ahead of the video frame by less
// than this is decode-side lookahead -> hold it in the queue until the video render
// position catches up (this hold is what keeps the encoded audio content pinned to
// the picture; without it the container gate alone admits chunks as fast as they
// arrive and the lookahead is baked into the file as audio leading the video). A gap
// at or beyond this means the two source clocks have DIVERGED (media-clock crawl:
// render pts crawls while decode pts advance normally) — the hold
// would never release, so it is waived and the container gate becomes the only bound.
constexpr int64_t kSourceDivergeWaiveUs = 10 * 1000000;
// (2026-11-07) The 500 ms container-gate lead allowance added 2026-11-04 is GONE.
// It was needed when a full gate DROPPED arriving chunks (3x renderer starvation
// tore the audio, 230 frames / 4.6 s); the live paths now HOLD at a full gate
// instead, so held audio drains aligned once video_end grows again — no tear, and
// no allowance to bake orphan audio past the video end (a 3x popup-stall session
// ended with audio_pos pinned at video_end+500 ms and a 500 ms audio lead baked
// into the file from the starved span). The gate is strict video_end everywhere
// now; orphan audio is dropped at the stop drain instead.
// Pad budget for the source-domain hold: after a re-anchor CefMedia Relay's audio and
// video clocks can sit a few seconds apart (the audio pipeline re-anchors at a
// slightly different position than the video render clock), so the hold can pin the
// queue head for a long stretch while FillSilence pads the gap — one session showed
// 24 s of inserted digital silence. When cumulative padding since the last
// real-audio chunk exceeds this budget, the hold is waived (FIFO encode) until real
// audio flows again: the silence damage is bounded, and any residual content lead is
// bounded by the queued depth at the waive moment.
constexpr int64_t kHoldPadBudgetUs = 5 * 1000000;

} // namespace

CefMediaRecorder::~CefMediaRecorder()
{
    StopRecord();
}

void CefMediaRecorder::StartRecord(const std::string& path, const std::string& container,
                                   const std::string& vcodec,
                                   int vbitrate_kbps, int abitrate_kbps,
                                   const std::string& source)
{
    StopRecord(); // stop first if the previous recording was not fully stopped (idempotent)

    path_ = path;
    container_ = container;
    vcodec_ = vcodec;
    vbitrate_kbps_ = vbitrate_kbps;
    abitrate_kbps_ = abitrate_kbps;
    want_video_ = (source != "a"); // "va"/"v" record video; only "a" skips it
    want_audio_ = (source != "v"); // "va"/"a" record audio; only "v" skips it

    {
        std::lock_guard<std::mutex> lk(qmutex_);
        video_q_.clear();
        audio_q_.clear();
    }
    stopping_ = false;
    started_ = true;
    blog(LOG_INFO,
         "[cefmedia] recorder StartRecord: path=%s fmt=%s codec=%s vbit=%d abit=%d source=%s",
         path_.c_str(), container_.c_str(), vcodec_.c_str(), vbitrate_kbps_, abitrate_kbps_,
         source.c_str());
    worker_ = new std::thread(&CefMediaRecorder::WorkerProc, this);
}

void CefMediaRecorder::StopRecord()
{
    if (!started_.exchange(false))
    {
        return; // idempotent
    }
    stopping_ = true;
    qcv_.notify_all();
    if (worker_)
    {
        worker_->join();
        delete worker_;
        worker_ = nullptr;
    }
    core_.Finalize(); // fallback (no-op if the worker already finalized)
}

void CefMediaRecorder::RequestStopAsync()
{
    if (!started_.exchange(false))
    {
        return; // already stopping / stopped
    }
    stopping_ = true;
    qcv_.notify_all();
    // No join: the worker finalizes (flush + trailer + close) and exits on its own.
    // The caller (reader callback thread) must not block here; the output layer joins
    // on the UI thread via StopRecord (idempotent flag, immediate join of an exited
    // thread).
}

int64_t CefMediaRecorder::GetFileSize() const
{
    return core_.FileSize();
}

void CefMediaRecorder::VideoCallback(cefmedia_video_frame* frame, const uint8_t* payload,
                                     void* obj, int /*channel*/)
{
    static_cast<CefMediaRecorder*>(obj)->OnVideoFrame(frame, payload);
}

void CefMediaRecorder::AudioCallback(cefmedia_audio_buffer* buffer, const uint8_t* payload,
                                     void* obj, int /*channel*/)
{
    static_cast<CefMediaRecorder*>(obj)->OnAudioBuffer(buffer, payload);
}

void CefMediaRecorder::OnVideoFrame(cefmedia_video_frame* in, const uint8_t* payload)
{
    if (stopping_.load() || !want_video_)
    {
        // want_video_ == false (Source = Audio Only): drop before the queue — the frame
        // is never copied, keeping the callback cheap (receive-thread hold stays
        // minimal).
        return;
    }
    if (in->flags & CefFrameFlag_End)
    {
        // End-of-stream notification frame (header only, no picture data):
        // never encode it into the output file. With auto-stop enabled: REQUEST the stop
        // (flag + notify only, NO join — this runs inside the receive fan-out; the worker
        // finalizes on its own and the output layer joins on the UI thread).
        blog(LOG_INFO, "[cefmedia] recorder: video end-of-stream frame%s",
             auto_stop_ ? " -> auto-stopping the recording" : " skipped");
        if (auto_stop_)
        {
            RequestStopAsync();
        }
        return;
    }
    AVFrame* f = TakeVideoFrame(in, payload);
    if (f)
    {
        // Stamp the header's measured fps for the worker's CFR-adaptation window
        // (frame_rate is the renderer's bucketed estimator value, 0 = unknown).
        source_fps_.store((int64_t)in->frame_rate, std::memory_order_relaxed);
        PushVideo(f);
    }
}

void CefMediaRecorder::OnAudioBuffer(cefmedia_audio_buffer* in, const uint8_t* payload)
{
    if (stopping_.load() || !want_audio_)
    {
        return; // same drop-at-enqueue for Source = Video Only
    }
    if (in->flags & CefFrameFlag_End)
    {
        // Same end-of-stream notification as video: skip (no sample data).
        blog(LOG_DEBUG, "[cefmedia] recorder: audio end-of-stream frame skipped");
        return;
    }
    AVFrame* f = TakeAudioFrame(in, payload);
    if (f)
    {
        PushAudio(f);
    }
}

AVFrame* CefMediaRecorder::TakeVideoFrame(cefmedia_video_frame* in, const uint8_t* payload)
{
    const AVPixelFormat fmt = MapVideoFormat(in->format);
    if (fmt == AV_PIX_FMT_NONE || in->width <= 0 || in->height <= 0)
    {
        return NULL;
    }

    PlaneSpec specs[4];
    int planes = 0;
    if (!PlaneSpecs(fmt, in->width, in->height, specs, &planes))
    {
        return NULL;
    }

    AVFrame* f = av_frame_alloc();
    f->format = fmt;
    f->width = in->width;
    f->height = in->height;
    if (av_frame_get_buffer(f, 32) < 0)
    {
        av_frame_free(&f);
        return NULL;
    }

    // Copy row by row by stride (the wire stride is not guaranteed to match the
    // AVFrame linesize). Plane layout per the frame contract: payload + planes[i].offset.
    for (int i = 0; i < planes; ++i)
    {
        const int row_bytes = specs[i].pw * specs[i].bpp;
        const uint8_t* src = payload + in->planes[i].offset;
        uint8_t* dst = f->data[i];
        for (int r = 0; r < specs[i].ph; ++r)
        {
            memcpy(dst, src, row_bytes);
            dst += f->linesize[i];
            src += in->planes[i].stride;
        }
    }

    f->pts = in->timestamp;
    return f;
}

AVFrame* CefMediaRecorder::TakeAudioFrame(cefmedia_audio_buffer* in, const uint8_t* payload)
{
    const AVSampleFormat fmt = MapAudioFormat(in->format);
    if (fmt == AV_SAMPLE_FMT_NONE || in->channel_count <= 0 || in->sample_rate <= 0 ||
        in->data_size == 0)
    {
        return NULL;
    }
    const int bps = av_get_bytes_per_sample(fmt);
    const int src_samples = (int)(in->data_size / bps / in->channel_count);
    if (src_samples <= 0)
    {
        return NULL;
    }

    AVFrame* f = av_frame_alloc();
    f->nb_samples = src_samples;
    f->format = fmt;
    av_channel_layout_default(&f->ch_layout, in->channel_count);
    f->sample_rate = in->sample_rate;
    f->duration = (int64_t)src_samples * 1000000 / in->sample_rate; // us (stop-drain drop log)
    if (av_frame_get_buffer(f, 0) < 0)
    {
        av_frame_free(&f);
        return NULL;
    }

    // IPC audio is stored per-channel in contiguous blocks (channel i starts at
    // i * (data_size / channel_count), per the frame contract): planar formats split
    // each channel, interleaved copies the whole block.
    if (av_sample_fmt_is_planar(fmt))
    {
        const int per_ch = src_samples * bps;
        const uint8_t* src = payload;
        for (int i = 0; i < in->channel_count; ++i)
        {
            memcpy(f->extended_data[i], src, per_ch);
            src += per_ch;
        }
    }
    else
    {
        memcpy(f->extended_data[0], payload, in->data_size);
    }

    f->pts = in->timestamp;
    return f;
}

// Push with wait-when-full (never drop). The callback thread polls in 10 ms slices so
// a stop flips through quickly and the receive callback stays responsive between
// slices. Backpressure propagates: full queue -> reader waits -> relay ring fills ->
// relay gates the renderer — the same "slow the source down" model as a hardware
// capture card.
bool CefMediaRecorder::PushVideo(AVFrame* f)
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lk(qmutex_);
            if (video_q_.size() < kVideoQueueCap)
            {
                video_q_.push_back(f);
                qcv_.notify_one();
                return true;
            }
        }
        if (stopping_.load())
        {
            av_frame_free(&f);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool CefMediaRecorder::PushAudio(AVFrame* f)
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lk(qmutex_);
            if (audio_q_.size() < kAudioQueueCap)
            {
                audio_q_.push_back(f);
                qcv_.notify_one();
                return true;
            }
        }
        if (stopping_.load())
        {
            av_frame_free(&f);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

AVFrame* CefMediaRecorder::PopVideo()
{
    std::lock_guard<std::mutex> lk(qmutex_);
    if (video_q_.empty())
    {
        return NULL;
    }
    AVFrame* f = video_q_.front();
    video_q_.pop_front();
    return f;
}

AVFrame* CefMediaRecorder::PopAudio()
{
    std::lock_guard<std::mutex> lk(qmutex_);
    if (audio_q_.empty())
    {
        return NULL;
    }
    AVFrame* f = audio_q_.front();
    audio_q_.pop_front();
    return f;
}

AVFrame* CefMediaRecorder::PeekAudio()
{
    std::lock_guard<std::mutex> lk(qmutex_);
    return audio_q_.empty() ? NULL : audio_q_.front();
}

bool CefMediaRecorder::QueuesEmpty()
{
    std::lock_guard<std::mutex> lk(qmutex_);
    return video_q_.empty() && audio_q_.empty();
}

void CefMediaRecorder::NotifyError()
{
    void (*cb)(void*) = nullptr;
    void* obj = nullptr;
    {
        std::lock_guard<std::mutex> lk(error_mutex_);
        cb = error_cb_;
        obj = error_obj_;
    }
    if (cb)
    {
        cb(obj); // called outside the lock
    }
}

void CefMediaRecorder::WorkerProc()
{
    // ---- First-frame wait: "va" requires BOTH first frames (no single-side timeout —
    //      a start-race must not degrade the recording to audio-only; the 5 s warning
    //      covers the wait). Explicit single-stream modes ("v"/"a") wait for their side
    //      only and keep the 1 s single-side timeout so a missing other side never
    //      blocks the start (it is dropped by the callbacks anyway).
    AVFrame* first_v = NULL;
    AVFrame* first_a = NULL;
    bool seen = false;
    bool no_frame_warned = false;
    const auto t0 = std::chrono::steady_clock::now();

    while (!stopping_.load())
    {
        if (!first_v && want_video_)
        {
            first_v = PopVideo();
        }
        if (!first_a && want_audio_)
        {
            first_a = PopAudio();
        }
        if ((!want_video_ || first_v) && (!want_audio_ || first_a))
        {
            break;
        }
        // Both-sides mode ("va"): no single-side timeout — video stalling >1 s while
        // audio flowed must NOT degrade the recording to an audio-only chain (the
        // start-race made occasional audio-only files; the source gate alone does not
        // close it). Wait until both first frames arrive (or stop); the 5 s warning
        // below tells the user what is happening. Explicit single-stream modes ("v"/"a")
        // keep the 1 s timeout so a missing other side never blocks the start.
        if (seen && want_video_ && want_audio_)
        {
            continue; // re-arm the pop loop; the 50 ms wait at the bottom still applies
        }
        if (seen)
        {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() >=
                kFirstFrameTimeoutMs)
            {
                break;
            }
        }
        if (first_v || first_a)
        {
            seen = true;
        }
        else if (!no_frame_warned)
        {
            // Still no frame after 5 s: CefMedia Relay not running / not capturing? Warn once,
            // keep waiting.
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() >= 5000)
            {
                no_frame_warned = true;
                blog(LOG_WARNING,
                     "[cefmedia] recorder: no CefMedia Relay frames for 5 s (CefMedia Relay not running / "
                     "not capturing?)");
            }
        }
        {
            std::unique_lock<std::mutex> lk(qmutex_);
            qcv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    if (stopping_.load() || (!first_v && !first_a))
    {
        blog(LOG_INFO, "[cefmedia] recorder worker exit (no frames: v=%d a=%d stopping=%d)",
             first_v != NULL, first_a != NULL, stopping_.load());
        if (first_v) av_frame_free(&first_v);
        if (first_a) av_frame_free(&first_a);
        return;
    }

    // ---- Lazily build the encode chain (using the first-frame params) ----
    const bool has_v = (first_v != NULL);
    const bool has_a = (first_a != NULL);
    const int w = has_v ? first_v->width : 0;
    const int h = has_v ? first_v->height : 0;
    const int sr = has_a ? first_a->sample_rate : 0;
    const int ch = has_a ? first_a->ch_layout.nb_channels : 0;

    // ---- CFR interval: measure the source fps from the frame header before Open ----
    // The renderer's FrameRateEstimator reports a bucketed int fps per frame header
    // (0 = unknown / not yet stable). Buffer up to kFpsMeasureWindowMs of frames while
    // waiting for kFpsStableRun consecutive identical non-zero values; a 30 fps fallback
    // covers the unknown case (the historical hard-coded value). The chosen interval
    // only sets the CFR cadence (gap-fill step, GOP, crawl pacing) — real frames encode
    // at their own source PTS either way, so a wrong estimate never corrupts timing.
    double fps = 30.0; // GOP estimate + container nominal rate; does not drive PTS math
    int64_t interval_us = 33333;
    if (has_v)
    {
        std::deque<AVFrame*> buffered; // measurement-window frames (time order)
        int stable_run = 0;
        int64_t last_fps = -1;
        const auto t_start = std::chrono::steady_clock::now();
        while (!stopping_.load())
        {
            AVFrame* f = PopVideo();
            if (!f)
            {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_start)
                        .count() >= kFpsMeasureWindowMs)
                {
                    break; // window closed without a stable estimate
                }
                std::unique_lock<std::mutex> lk(qmutex_);
                qcv_.wait_for(lk, std::chrono::milliseconds(10));
                continue;
            }
            const int64_t fps_hdr = (int64_t)f->width > 0
                                        ? source_fps_.load(std::memory_order_relaxed)
                                        : 0;
            if (fps_hdr > 0 && fps_hdr == last_fps)
            {
                ++stable_run;
            }
            else
            {
                stable_run = (fps_hdr > 0) ? 1 : 0;
                last_fps = fps_hdr;
            }
            buffered.push_back(f);
            if (stable_run >= kFpsStableRun)
            {
                break; // stable estimate reached
            }
            if ((int)buffered.size() >= kFpsMeasureMaxFrames)
            {
                break; // bounded buffer regardless of stability
            }
        }
        if (stable_run >= kFpsStableRun && last_fps > 0)
        {
            fps = (double)last_fps;
            interval_us = (int64_t)(1000000.0 / fps + 0.5);
            blog(LOG_INFO, "[cefmedia] recorder: source fps %d (stable over %d frames) -> "
                           "CFR interval %lld us",
                 (int)last_fps, stable_run, (long long)interval_us);
        }
        else
        {
            blog(LOG_INFO, "[cefmedia] recorder: source fps unknown in measurement window "
                           "-> CFR fallback 30 fps (interval 33333 us)");
        }
        // Re-queue the measurement frames in time order: push_front in REVERSE so the
        // oldest frame lands at the queue head (the first frame goes back in too).
        for (auto it = buffered.rbegin(); it != buffered.rend(); ++it)
        {
            std::lock_guard<std::mutex> lk(qmutex_);
            video_q_.push_front(*it);
        }
        if (first_v)
        {
            // first_v was popped before the window; it is the oldest — front of the queue.
            std::lock_guard<std::mutex> lk(qmutex_);
            video_q_.push_front(first_v);
            first_v = NULL; // ownership moved into the queue (freed by the main loop)
        }
    }

    if (!core_.Open(path_, container_, vcodec_, w, h, fps, vbitrate_kbps_, sr, ch, abitrate_kbps_))
    {
        blog(LOG_ERROR, "[cefmedia] recorder: FFmpeg stream setup failed (no encoder / invalid "
             "params) w=%d h=%d sr=%d ch=%d codec=%s",
             w, h, sr, ch, vcodec_.c_str());
        if (first_v) av_frame_free(&first_v);
        if (first_a) av_frame_free(&first_a);
        NotifyError();
        return;
    }
    blog(LOG_INFO, "[cefmedia] recorder: FFmpeg ready w=%d h=%d sr=%d ch=%d codec=%s",
         w, h, sr, ch, vcodec_.c_str());

    // Re-queue the first frames (push_front keeps time order) for the main loop to encode
    // normally — otherwise the first frame is neither written nor leaked.
    if (first_v)
    {
        std::lock_guard<std::mutex> lk(qmutex_);
        video_q_.push_front(first_v);
    }
    if (first_a)
    {
        std::lock_guard<std::mutex> lk(qmutex_);
        audio_q_.push_front(first_a);
    }

    // ---- Main loop: video frames drive audio (drain audio up to this frame's source PTS,
    //      pad silence when audio lags). Source = Video Only / Audio Only: exactly one
    //      side flows (the callbacks drop the other), so only one of the two branches
    //      ever runs — single-stream recording needs no cross-side sync. ----
    // CFR fill step: measured from the source above (fallback 30 fps). The fill only
    // affects the bridging cadence inside gaps, not the real frames' playback speed.
    int64_t video_base_us = INT64_MIN;    // video container normalization base (first kept
                                          // frame's source PTS)
    int64_t video_max_src_us = INT64_MIN; // max source PTS (baseline for dropping backward
                                          // jitter)
    int64_t video_end_norm_us = INT64_MIN; // video timeline end (last encoded frame PTS +
                                           // interval) — bounds the audio drain at stop
    int64_t drop_us_total_ = 0;            // stop-drain surplus audio dropped (diagnostics)
    // Post-switch stale-audio sweep: after a re-anchor (either direction), old-source
    // audio already inside the relay pipeline keeps arriving for a few seconds
    // (decode-side lookahead). Sweeping only at the seam misses those late arrivals —
    // they would sit at the queue head forever and block all new-source audio behind
    // them. For kAudioSweepWindowMs after the switch, every loop iteration drops
    // queue-head chunks on the sweep side of the floor:
    //   mode 1 (backward switch): drop chunks with src pts >= the floor (old timeline
    //     top; new-source chunks start near 0 and pass untouched);
    //   mode 2 (forward switch): drop chunks with src pts < the floor (pre-cut
    //     residue; post-cut chunks have pts >= cut and pass).
    int64_t audio_sweep_floor_us_ = INT64_MIN;
    int audio_sweep_mode_ = 0;             // 1 = backward (drop >= floor), 2 = forward (drop < floor)
    int64_t audio_sweep_deadline_ms_ = 0;
    int64_t dropped_us_sweep_ = 0;         // sweep-dropped stale audio, cumulative (diagnostics)
    int forward_absorb_count_ = 0;         // forward-seek absorbs, cumulative (diagnostics)
    // Media-clock crawl pacing (see kCrawlDeltaUs): consecutive sub-delta source steps.
    int crawl_run_ = 0;                    // current consecutive crawl-step count
    bool crawl_active_ = false;            // pacing episode in progress
    int64_t crawl_steps_total_ = 0;        // paced frames, cumulative (diagnostics)
    int crawl_episodes_ = 0;               // pacing episodes, cumulative (diagnostics)
    // Container-domain audio gate overflows (diagnostics): audio decoded faster than
    // the container timeline can accept (decode-side lookahead during crawl/hold) is
    // dropped once the gate is full instead of inflating the audio track.
    int64_t gate_dropped_frames_ = 0;
    int64_t gate_dropped_us_ = 0;
    // Backward-jitter drops (<= kBackwardReanchorUs, silent by design): a burst here
    // erases video content while audio keeps flowing — the perceived audio-ahead
    // signature of a popup that stops delivering frames. Zero logging made such a
    // burst invisible; count and throttle-log it.
    int64_t backward_dropped_frames_ = 0;
    int64_t last_backward_log_ms_ = 0;
    // Frozen-video gaps: the next video frame's src PTS skipped > 10 intervals (frames
    // lost upstream) — the CFR fill freezes the picture over the span while audio keeps
    // flowing. Perceived as audio ahead of video for the gap duration. Same observability
    // fix as the backward-jitter counter.
    int64_t frozen_gap_count_ = 0;
    int64_t frozen_gap_us_total_ = 0;
    int64_t last_gap_log_ms_ = 0;
    // Cumulative FillSilence padding since the last real-audio encode (the src-hold
    // pad budget, see kHoldPadBudgetUs).
    int64_t padded_since_audio_us_ = 0;
    bool hold_waived_ = false;             // pad budget exhausted -> hold disabled
                                           // until real audio encodes again
    while (true)
    {
        // Progress flag for the idle wait below: an iteration that neither encoded a
        // video frame nor encoded/dropped audio (e.g. audio held beyond the video end)
        // must sleep — otherwise the loop busy-spins while waiting for video coverage.
        bool progressed = false;

        // Wall clock for this iteration (sweep window expiry + re-anchor deadline).
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // Post-switch stale-audio sweep (see the declaration above): drop queue-head
        // chunks from the PREVIOUS source while the sweep window is open, on the sweep
        // side of the floor (mode 1: backward switch, old top; mode 2: forward switch,
        // pre-cut residue). Also expires the sweep once the window closes.
        if (audio_sweep_floor_us_ != INT64_MIN)
        {
            if (now_ms > audio_sweep_deadline_ms_)
            {
                audio_sweep_floor_us_ = INT64_MIN;
                audio_sweep_mode_ = 0;
            }
            else
            {
                bool swept = false;
                while (true)
                {
                    AVFrame* stale = PeekAudio();
                    if (!stale)
                    {
                        break;
                    }
                    if (audio_sweep_mode_ == 1 ? stale->pts >= audio_sweep_floor_us_
                                               : stale->pts < audio_sweep_floor_us_)
                    {
                        PopAudio();
                        dropped_us_sweep_ += stale->duration;
                        av_frame_free(&stale);
                        swept = true;
                    }
                    else
                    {
                        break;
                    }
                }
                if (swept)
                {
                    blog(LOG_INFO,
                         "[cefmedia] recorder: sweep dropped stale audio up to %lld ms "
                         "total (previous-source chunks arriving after the switch)",
                         (long long)(dropped_us_sweep_ / 1000));
                    progressed = true;
                }
            }
        }

        AVFrame* vf = PopVideo();
        if (vf)
        {
            progressed = true;
            // Jump classification. All
            // decisions compare the incoming frame's source PTS against the source-PTS
            // high-water mark:
            //  - backward small (<= 500 ms): out-of-order delivery jitter -> drop;
            //  - backward large (> 500 ms): seek back / next video -> re-anchor at the
            //    container end (continuous timeline, sweep old-source audio);
            //  - forward small (<= 1 s): normal cadence / minor gap -> CFR fill covers
            //    it (capped by kMaxGapFillFrames);
            //  - forward large (> 1 s): seek forward -> ABSORB it (re-anchor; the
            //    skipped span must not become a container hole — a 568 s hole made every
            //    later frame pad 2 s of silence — a real session showed a 3574 s silent tail);
            //  - forward micro (< 2 ms): media-clock crawl -> paced by F3 below.
            bool reanchored = false;
            bool forward_absorbed = false;
            const int64_t old_max_src_us = video_max_src_us;
            if (video_max_src_us != INT64_MIN && vf->pts != video_max_src_us)
            {
                const int64_t delta = vf->pts - video_max_src_us;
                if (delta < 0)
                {
                    const int64_t backward = -delta;
                    if (backward <= kBackwardReanchorUs)
                    {
                        ++backward_dropped_frames_;
                        if (now_ms - last_backward_log_ms_ > 1000)
                        {
                            last_backward_log_ms_ = now_ms;
                            blog(LOG_WARNING,
                                 "[cefmedia] recorder: backward jitter drop %lld ms (burst "
                                 "erases video content, audio keeps flowing)",
                                 (long long)(backward / 1000));
                        }
                        av_frame_free(&vf);
                        vf = NULL;
                    }
                    else
                    {
                        // Re-anchor: the new source's first frame lands at the current output
                        // end (video_end_norm_us, or 0 before any video was encoded).
                        // Subsequent new-source frames ride the same base.
                        blog(LOG_INFO,
                             "[cefmedia] recorder: video jump backward %lld us -> re-anchor "
                             "base to %lld (video end %lld us)",
                             (long long)backward, (long long)(vf->pts - video_end_norm_us),
                             (long long)video_end_norm_us);
                        video_base_us =
                            vf->pts - (video_end_norm_us == INT64_MIN ? 0 : video_end_norm_us);
                        reanchored = true;
                    }
                }
                else if (delta > kForwardReanchorUs)
                {
                    // Forward seek absorb: mirror of the backward re-anchor. The new
                    // source's first frame lands at the current output end; the skipped
                    // span does not exist in the container.
                    blog(LOG_INFO,
                         "[cefmedia] recorder: video jump forward %lld us -> absorb "
                         "(base to %lld, video end %lld us)",
                         (long long)delta, (long long)(vf->pts - video_end_norm_us),
                         (long long)video_end_norm_us);
                    video_base_us =
                        vf->pts - (video_end_norm_us == INT64_MIN ? 0 : video_end_norm_us);
                    forward_absorbed = true;
                    ++forward_absorb_count_;
                }
                else if (delta < kCrawlDeltaUs)
                {
                    // Micro step: count it; pacing engages once a full run accumulates
                    // (handled below, before this frame is encoded).
                    ++crawl_run_;
                }
                else
                {
                    // Normal cadence (or end of a crawl episode).
                    if (crawl_active_)
                    {
                        blog(LOG_INFO,
                             "[cefmedia] recorder: media clock recovered after %d crawl "
                             "step(s), source cadence %.1f ms/frame",
                             crawl_run_, delta / 1000.0);
                    }
                    crawl_run_ = 0;
                    crawl_active_ = false;
                }
            }
            if (vf)
            {
                // Track the source-PTS high-water mark for the jump checks
                // above. This MUST happen on every kept frame (normal path raises it,
                // re-anchor lowers it to the new source's first frame) — losing this
                // update leaves it at INT64_MIN and the checks never fire again.
                video_max_src_us = vf->pts;
                if (video_base_us == INT64_MIN)
                {
                    // First frame as the base -> the video container starts at 0, aligned
                    // with audio (sample count starts at 0); fixes the "black screen at the
                    // start" (video used to start at the raw source clock, which CefMedia Relay
                    // media clock does not reset, so video arrived late).
                    video_base_us = vf->pts;
                }

                // Media-clock crawl pacing: after kCrawlRunMin consecutive micro steps the
                // CefMedia Relay's media clock is crawling (sub-ms steps at full frame rate). Re-anchor so
                // this frame lands at the current container end — the container then
                // advances a full interval per frame instead of compressing thousands of
                // real frames into seconds (a real session squeezed 16775 frames into its
                // last 4.3 s). The clock recovering (>= kCrawlDeltaUs step) returns to the
                // normal path automatically; the base tracks it, so the transition is
                // seamless.
                if (!reanchored && !forward_absorbed && crawl_run_ >= kCrawlRunMin)
                {
                    if (!crawl_active_)
                    {
                        crawl_active_ = true;
                        ++crawl_episodes_;
                        blog(LOG_INFO,
                             "[cefmedia] recorder: media clock crawling (%d micro-steps "
                             "< %lld us) -> pacing at %.1f fps, episode #%d",
                             crawl_run_, (long long)kCrawlDeltaUs,
                             1000000.0 / (double)interval_us, crawl_episodes_);
                    }
                    video_base_us = vf->pts - video_end_norm_us;
                    ++crawl_steps_total_;
                }

                const int64_t video_end_prev_us = video_end_norm_us; // pre-update snapshot
                const int64_t norm_pts_us = vf->pts - video_base_us;
                video_end_norm_us = norm_pts_us + interval_us;
                // Frozen-gap probe: the source PTS skipped more than 10 intervals since
                // the previous frame without a re-anchor/absorb (those land the new
                // frame AT the old end, gap 0) — frames were lost upstream and
                // EncodeVideoCfr will freeze the picture over the span while audio
                // keeps flowing. Sub-1s gaps don't trigger the absorb log, so this is
                // the only observable trace of a popup stopping delivery.
                if (video_end_prev_us != INT64_MIN && norm_pts_us > video_end_prev_us)
                {
                    const int64_t gap_us = norm_pts_us - video_end_prev_us;
                    if (gap_us > 10 * interval_us)
                    {
                        ++frozen_gap_count_;
                        frozen_gap_us_total_ += gap_us;
                        if (now_ms - last_gap_log_ms_ > 1000)
                        {
                            last_gap_log_ms_ = now_ms;
                            blog(LOG_WARNING,
                                 "[cefmedia] recorder: video pts gap %lld ms -> picture "
                                 "frozen over the span (frames lost upstream), audio keeps "
                                 "flowing",
                                 (long long)(gap_us / 1000));
                        }
                    }
                }

                if (reanchored || forward_absorbed)
                {
                    // First sweep at the seam. The cut point in the source domain:
                    //  - backward switch: the cut happens at CefMedia Relay's render clock
                    //    T_cut = the old video_max_src_us — every audio chunk decoded
                    //    BEFORE the cut (old source, decode-side lookahead ~2 s at 2x)
                    //    has src PTS >= T_cut; every new-source chunk has src PTS ~ 0.
                    //    Sweep floor on it, drop >= floor (mode 1).
                    //  - forward seek: the cut happens at the seek target = this frame's
                    //    pts; late pre-cut residue has src PTS below it. Drop < floor
                    //    with a tolerance for chunks that may already be post-cut
                    //    content (mode 2).
                    if (forward_absorbed)
                    {
                        audio_sweep_floor_us_ = vf->pts - kForwardSweepTolUs;
                        audio_sweep_mode_ = 2;
                    }
                    else
                    {
                        audio_sweep_floor_us_ = old_max_src_us;
                        audio_sweep_mode_ = 1;
                    }
                    audio_sweep_deadline_ms_ = now_ms + kAudioSweepWindowMs;
                    int64_t flushed_us = 0;
                    int flushed_frames = 0;
                    while (true)
                    {
                        AVFrame* stale = PeekAudio();
                        if (!stale)
                        {
                            break;
                        }
                        const bool stale_side =
                            audio_sweep_mode_ == 1 ? stale->pts >= audio_sweep_floor_us_
                                                   : stale->pts < audio_sweep_floor_us_;
                        if (!stale_side)
                        {
                            break;
                        }
                        PopAudio();
                        flushed_us += stale->duration;
                        av_frame_free(&stale);
                        flushed_frames++;
                    }
                    if (flushed_frames > 0)
                    {
                        blog(LOG_INFO,
                             "[cefmedia] recorder: re-anchor flushed %d stale audio "
                             "frame(s), %lld ms (%s decode lookahead)",
                             flushed_frames, (long long)(flushed_us / 1000),
                             forward_absorbed ? "pre-cut" : "previous-source");
                    }
                }
                else
                {
                    // 1) Encode audio in FIFO order under TWO bounds:
                    //    a) Source-domain alignment hold: chunks whose src PTS is ahead
                    //       of this frame's video src PTS are decode-side lookahead —
                    //       they wait in the queue until the video render position
                    //       reaches them (audio is pinned to the video frame being
                    //       encoded). This hold is what keeps the
                    //       encoded audio CONTENT aligned with the picture: audio is
                    //       captured at the decode side, which at >1x runs ~2.5 s
                    //       (media time) ahead of the render side; without the hold
                    //       that lead is baked into the file (a real session's audio
                    //       leading the video by a few seconds after each seek).
                    //       WAIVED when the gap exceeds kSourceDivergeWaiveUs: a gap
                    //       that large means the two source clocks diverged (media-
                    //       clock crawl — render pts crawls while decode pts advance)
                    //       and the hold would never release; the
                    //       container bound below then takes over.
                    //    b) Container bound: AudioPositionUs (sample-count timeline,
                    //       epoch-free) + this chunk must not run past the video end —
                    //       the audio TRACK can never outgrow the video TIMELINE
                    //       regardless of clock pathologies (one pathological session
                    //       produced a 4210 s audio track).
                    //    Audio OLDER than the anchor (pts < video_base, pre-roll
                    //    accumulated before the first video frame arrived, or chunks
                    //    straddling a re-anchor cut) is dropped — boundary rule: the
                    //    recording starts at the first video frame.
                    while (true)
                    {
                        AVFrame* af_queued = PeekAudio();
                        if (!af_queued)
                        {
                            break;
                        }
                        const int64_t src_gap = af_queued->pts - vf->pts;
                        if (!hold_waived_ && src_gap > 0 && src_gap <= kSourceDivergeWaiveUs)
                        {
                            break; // decode-side lookahead: hold for the render position
                        }
                        if (core_.AudioPositionUs() + af_queued->duration >
                            video_end_norm_us)
                        {
                            break; // gate full: hold (video_end grows with the next frames)
                        }
                        PopAudio();
                        if (af_queued->pts < video_base_us)
                        {
                            blog(LOG_INFO,
                                 "[cefmedia] recorder: pre-roll audio dropped (%lld ms, pts "
                                 "%lld < video base %lld)",
                                 (long long)(af_queued->duration / 1000),
                                 (long long)af_queued->pts, (long long)video_base_us);
                        }
                        else
                        {
                            core_.EncodeAudio(af_queued);
                            if (hold_waived_)
                            {
                                hold_waived_ = false;
                                blog(LOG_INFO,
                                     "[cefmedia] recorder: pad budget satisfied, source "
                                     "hold re-engaged");
                            }
                        }
                        padded_since_audio_us_ = 0; // real audio flowed (encoded or pre-roll)
                        av_frame_free(&af_queued);
                    }
                }

                // 2) Audio lags video (compared in the normalized domain, >30 ms) -> pad
                //    silence (cap 2 s). After a re-anchor the new source's audio starts
                //    near the new anchor; this pad bridges the seam (bounded, 2 s).
                //    Padding is budgeted (kHoldPadBudgetUs): if the source hold keeps
                //    the queue pinned and the pad keeps growing past the budget, waive
                //    the hold — encode FIFO instead of padding on (one session padded
                //    24 s of inserted silence).
                const int64_t gap = norm_pts_us - core_.AudioPositionUs();
                if (gap > kSyncGapThresholdUs)
                {
                    const int64_t pad_us = gap > kMaxSyncGapUs ? kMaxSyncGapUs : gap;
                    core_.FillSilenceUs(pad_us);
                    padded_since_audio_us_ += pad_us;
                    if (!hold_waived_ && padded_since_audio_us_ > kHoldPadBudgetUs)
                    {
                        hold_waived_ = true;
                        blog(LOG_WARNING,
                             "[cefmedia] recorder: %lld ms padded since last real audio "
                             "(source hold pinned the queue) -> waiving the hold, encoding "
                             "FIFO",
                             (long long)(padded_since_audio_us_ / 1000));
                    }
                }

                // 3) CFR encode: normalized source PTS + gap fill (no fast-forward / no
                //    time compression).
                core_.EncodeVideoCfr(vf, norm_pts_us, interval_us);
                av_frame_free(&vf);
            }
        }
        else
        {
            // No video frame this iteration (audio-only recording, or video waiting on
            // its 30 fps cadence): audio may NEVER run past
            // the video timeline end (bounded by the last encoded video PTS). Our audio
            // is captured at the DECODE side (a full
            // CefMedia Relay audio-buffer ahead of the playhead, ~2 s at 2x), video at the
            // RENDER side (2-3 frames): unbounded encoding here would let the audio
            // content outrun the video permanently (the +2 s track-length skew seen in
            // 2x files) and turn every decode-side hiccup into a FillSilence hole.
            //
            // video_end_norm_us == INT64_MIN: no video encoded yet — mid-recording hold
            // audio in the queue until the first video frame anchors the timeline (it
            // encodes then, loop 1). STOPPING with no anchor: no video timeline will
            // ever exist (audio-only recording, or video never arrived) — encode
            // freely, otherwise the exit condition (QueuesEmpty) can never fire.
            AVFrame* af = NULL;
            if (!has_v || video_end_norm_us == INT64_MIN)
            {
                if (!stopping_.load() && has_v)
                {
                    af = PeekAudio() ? PopAudio() : NULL;
                    if (af)
                    {
                        progressed = true;
                    }
                }
                else
                {
                    af = PopAudio();
                    if (af)
                    {
                        progressed = true;
                    }
                }
            }
            else
            {
                // Video timeline live. Mid-recording: encode only what the video
                // timeline already covers — the gate is the CONTAINER position
                // (AudioPositionUs, sample-count timeline, epoch-free) against
                // video_end: audio ahead of the end is decode-side lookahead, held in
                // the queue until video frames arrive to cover it. (The previous
                // per-frame norm comparison depended on the shared video_base, whose
                // meaning changes at every re-anchor — the sample-count position has
                // no epochs and is the same bound the video branch uses.)
                // STOPPING: every frame must leave the queue (encode or drop) or the
                // exit condition below can never fire — in particular a frame that
                // PARTIALLY overlaps the video end (starts before it, ends after it)
                // would otherwise be picked up and put back forever (deadlock, no
                // trailer written).
                AVFrame* af_queued = PeekAudio();
                if (af_queued)
                {
                    if (!stopping_.load())
                    {
                        if (core_.AudioPositionUs() + af_queued->duration <=
                            video_end_norm_us)
                        {
                            af = PopAudio();
                            progressed = true;
                        }
                        // else: gate full (audio at/over the video end) -> hold; the
                        // timeline grows with the next video frames and the chunk is
                        // admitted then (progressed stays false -> idle sleep below).
                        // No lead allowance: orphan audio past the video end (its video
                        // never arrives — popup lost frames) must not bake into the file
                        // as a permanent audio lead.
                    }
                    else
                    {
                        PopAudio();
                        progressed = true;
                        if (core_.AudioPositionUs() >= video_end_norm_us)
                        {
                            // The gate is already at/past the video end: no coverage
                            // ahead, the container timeline never reaches this frame
                            // -> drop.
                            const int64_t dropped =
                                af_queued->duration > 0
                                    ? af_queued->duration
                                    : (int64_t)af_queued->nb_samples * 1000000LL /
                                          (af_queued->sample_rate ? af_queued->sample_rate
                                                                  : 1);
                            drop_us_total_ += dropped;
                            gate_dropped_frames_ += 1;
                            gate_dropped_us_ += dropped;
                            blog(LOG_INFO,
                                 "[cefmedia] recorder: audio past video end dropped (%lld "
                                 "ms this frame, %lld ms total) video_end=%lld us "
                                 "audio_pos=%lld us",
                                 (long long)(dropped / 1000),
                                 (long long)(drop_us_total_ / 1000),
                                 (long long)video_end_norm_us,
                                 (long long)core_.AudioPositionUs());
                        }
                        else
                        {
                            // Starts before the end (fully inside or partially
                            // overlapping): encode — the trailing samples past the end
                            // are bounded by one AAC frame (~23 ms) and trimmed by
                            // TruncateAudioToUs on the way out.
                            af = af_queued;
                        }
                        if (!af)
                        {
                            av_frame_free(&af_queued);
                        }
                    }
                }
            }
            if (af)
            {
                core_.EncodeAudio(af);
                av_frame_free(&af);
            }
        }

        // Stop and queues empty -> exit (StopRecord joins here). On the way out, align
        // the audio timeline to the video end (trim the partial framing buffer if the
        // encoded audio already overshot). The drain rules above guarantee every
        // queued audio frame leaves the queue once stopping_ is set (encode or drop),
        // so QueuesEmpty() is reachable in all paths.
        if (stopping_.load() && QueuesEmpty())
        {
            if (video_end_norm_us != INT64_MIN)
            {
                // Same bound the drain used: the audio track ends exactly at the video
                // end (no lead allowance — only a partial-overlap chunk's trailing
                // samples, bounded by one AAC frame, ever need trimming).
                core_.TruncateAudioToUs(video_end_norm_us);
            }
            break;
        }

        // Idle: nothing encoded/dropped this iteration (no video frame, audio held
        // back waiting for video coverage or queues empty). Sleep on the condition
        // variable instead of busy-spinning; PushVideo/PushAudio notify and wake us.
        if (!progressed)
        {
            std::unique_lock<std::mutex> lk(qmutex_);
            qcv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    // ---- Session diagnostics (one summary line per mechanism that fired) ----
    if (forward_absorb_count_ > 0 || gate_dropped_frames_ > 0 || crawl_episodes_ > 0 ||
        backward_dropped_frames_ > 0 || frozen_gap_count_ > 0)
    {
        blog(LOG_INFO,
             "[cefmedia] recorder session summary: forward absorbs=%d, sweep dropped "
             "%lld ms, gate overflow dropped %lld frame(s) / %lld ms, crawl episodes=%d "
             "(%lld paced frame(s)), backward jitter dropped %lld frame(s), frozen video "
             "gaps=%lld (%lld ms)",
             forward_absorb_count_, (long long)(dropped_us_sweep_ / 1000),
             gate_dropped_frames_, (long long)(gate_dropped_us_ / 1000),
             crawl_episodes_, crawl_steps_total_, backward_dropped_frames_,
             frozen_gap_count_, (long long)(frozen_gap_us_total_ / 1000));
    }

    // ---- Finalize: flush encoders -> write trailer -> close file ----
    core_.Finalize();
}

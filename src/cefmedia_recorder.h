#ifndef CEFMEDIA_RECORDER_H
#define CEFMEDIA_RECORDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include "cefmedia.h" // frame contract + callback typedefs
#include "recorder_ffmpeg.h"

// Recorder (the OBS output data path): copies cefmedia reader callback frames into
// queues, a worker thread consumes them -> FFmpeg encode -> mux to file (engine in
// FfmpegRecorderCore).
//
// Threading model (queue semantics):
//  - OnVideoFrame/OnAudioBuffer run on the libipc receive threads: they only "copy by
//    stride into a queue" and **wait when the queue is full (drop nothing — wait-when-full
//    backpressure semantics)**; never encode — the callback cost stays ~ms even under
//    backpressure;
//  - the worker thread is independent: first frame (both audio+video ready / single-side
//    1 s timeout) -> lazily build the encode chain (core.Open) -> video PTS uses the
//    source PTS directly (CFR gap fill, no fast-forward / no time compression), audio uses
//    the "sample-count timeline" (inside core); on each video frame the audio queue is
//    drained up to this frame's *source PTS*, and lagging audio is bridged with FillSilence
//    (A/V aligned in the source-PTS domain, in sync after ad skips / source switches);
//  - StopRecord is idempotent (atomic exchange guards against double-stop), sets the stop
//    flag + joins the worker (worker drains the queues, flushes the encoders, writes the
//    trailer, closes the file).
class CefMediaRecorder {
public:
    CefMediaRecorder() = default;
    ~CefMediaRecorder();
    CefMediaRecorder(const CefMediaRecorder&) = delete;
    CefMediaRecorder& operator=(const CefMediaRecorder&) = delete;

    // OBS output side. container: "mkv"/"mp4"; vcodec: "libx264"/"h264_nvenc"/"h264_qsv".
    // source: "va" (video+audio) / "v" (video only) / "a" (audio only) — the other side's
    // frames are dropped at enqueue time (never queued, never encoded).
    void StartRecord(const std::string& path, const std::string& container,
                     const std::string& vcodec, int vbitrate_kbps, int abitrate_kbps,
                     const std::string& source = "va");
    void StopRecord();

    // Stop REQUEST from a reader callback thread: flips the stop flag so the worker
    // drains, finalizes (flush + trailer + close) and exits on its own — NO join here
    // (the caller may BE inside the fan-out that must not block or self-join; the
    // output layer joins on the UI thread afterwards via StopRecord's idempotence).
    // The started_ flag is cleared, so IsStarted() reports false once this ran.
    void RequestStopAsync();

    // Whether a recording is in progress (started_ flag). False once RequestStopAsync
    // or StopRecord ran — the output layer uses the false edge to finish finalization
    // on the UI thread.
    bool IsStarted() const { return started_.load(); }

    // Auto-stop switch: when true, a video frame carrying CefFrameFlag_End (the
    // renderer's playback-ended notification) stops the recording through the
    // error-callback path (safe from the reader thread; the output layer then
    // finalizes and resets state like any other stop). Must be called before
    // StartRecord (snapshot at start).
    void SetAutoStop(bool enabled) { auto_stop_ = enabled; }
    bool AutoStop() const { return auto_stop_; }

    // cefmedia ipc receive callbacks (CefMediaIpcClient threads, thread-safe; obj = this).
    // Registered on ONE capture channel (the output's channel binding), so the frames
    // arriving here are always that channel's — the channel parameter is unused.
    static void VideoCallback(cefmedia_video_frame* frame, const uint8_t* payload, void* obj,
                              int channel);
    static void AudioCallback(cefmedia_audio_buffer* buffer, const uint8_t* payload, void* obj,
                              int channel);
    void OnVideoFrame(cefmedia_video_frame* frame, const uint8_t* payload);
    void OnAudioBuffer(cefmedia_audio_buffer* buffer, const uint8_t* payload);

    int64_t GetFileSize() const;
    bool IsRecording() const { return started_.load(); }
    // Actual recorded duration (us): the file-timeline position the audio sample-count
    // stream has reached (video is aligned to the same source-PTS timeline by
    // FillSilence). Stops advancing while the CefMedia Relay pauses/stalls — unlike the wall
    // clock this reflects how much content is really in the file.
    int64_t GetRecordedDurationUs() const { return core_.AudioPositionUs(); }

    // Init failure (no usable encoder / stream build failed) callback — the OBS output
    // side uses it to signal_stop.
    void SetErrorCallback(void (*cb)(void*), void* obj)
    {
        std::lock_guard<std::mutex> lk(error_mutex_);
        error_cb_ = cb;
        error_obj_ = obj;
    }

private:
    void WorkerProc();
    AVFrame* TakeVideoFrame(cefmedia_video_frame* in, const uint8_t* payload); // copy message
                                                // block -> AVFrame (callback thread)
    AVFrame* TakeAudioFrame(cefmedia_audio_buffer* in, const uint8_t* payload);
    bool PushVideo(AVFrame* f); // wait while full (returns false on stop); never drops
    bool PushAudio(AVFrame* f);
    AVFrame* PopVideo();
    AVFrame* PopAudio();
    AVFrame* PeekAudio();
    bool QueuesEmpty();
    void NotifyError();

    // Queues (callback thread pushes / worker pops). Deep + wait-when-full (no drops):
    // video 300 frames (~10 s @30 fps, ~3.3 s @3x speed); audio 4096 frames (~87 s @48k).
    // The push side waits in 10 ms polls while the worker drains (each push releases and
    // re-acquires), source display is not starved.
    std::deque<AVFrame*> video_q_;
    std::deque<AVFrame*> audio_q_;
    std::mutex qmutex_;
    std::condition_variable qcv_;
    static constexpr size_t kVideoQueueCap = 300;   // video queue (frames)
    static constexpr size_t kAudioQueueCap = 4096;  // audio queue (frames, ~87 s @48k)

    // State
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    // Source-reported frame rate (fps, from the frame header; 0 = unknown). Stamped by
    // OnVideoFrame on the receive thread, read by the worker during the CFR-adaptation
    // measurement window. float only to avoid a header include for std::atomic<float>
    // quirks — the value is integral anyway (renderer FPS estimator buckets).
    std::atomic<int64_t> source_fps_{0};
    std::thread* worker_ = nullptr;

    // Params (snapshot at StartRecord)
    std::string path_, container_, vcodec_;
    int vbitrate_kbps_ = 0, abitrate_kbps_ = 0;
    bool want_video_ = true; // source selection snapshot: which sides to record
    bool want_audio_ = true;
    bool auto_stop_ = false; // stop the recording on an End-flag video frame

    // Engine
    FfmpegRecorderCore core_;

    // Error reporting (callback never invoked under the lock, to avoid deadlock)
    void (*error_cb_)(void*) = nullptr;
    void* error_obj_ = nullptr;
    std::mutex error_mutex_;
};

#endif // CEFMEDIA_RECORDER_H

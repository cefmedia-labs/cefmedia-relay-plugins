// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef RECORDER_FFMPEG_H
#define RECORDER_FFMPEG_H

#include <cstdint>
#include <mutex>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// FFmpeg recording core: wraps MP4/MKV writing + H.264/AAC encoding + pixel/sample
// conversion + audio framing.
//
// Mechanisms: standard FFmpeg
// EncodeVideoFrame/ProcessAudioFrame/FillSilence/NewSilenceFrame/StopRecord patterns.
// Threading:
//   - called only from CefMediaRecorder's worker thread (EncodeVideoCfr/EncodeAudio/
//     FillSilence/Finalize); no internal threads;
//   - av_interleaved_write_frame is guarded by write_mutex_;
//   - video PTS is given by the caller as the **source PTS** (us, used directly, CFR gap
//     fill, no absorption / no compression), rescaled internally to the stream time_base;
//     audio PTS uses the "sample-count timeline" (nb_samples * encodeCount / sr), bridged
//     to the video timeline by the caller's FillSilence (aligned in the source-PTS domain).
//
// Known limitation: video size/framerate are fixed at Open from
// the first frame; mid-stream resolution/framerate changes do not rebuild the stream
// (CefMediaRecorder handles source switches by a fresh Start).
class FfmpegRecorderCore {
public:
    FfmpegRecorderCore() = default;
    ~FfmpegRecorderCore();
    FfmpegRecorderCore(const FfmpegRecorderCore&) = delete;
    FfmpegRecorderCore& operator=(const FfmpegRecorderCore&) = delete;

    // Probe whether an encoder is available (avcodec_find_encoder_by_name). name:
    // "libx264"/"h264_nvenc"/"h264_qsv"/"aac".
    static bool ProbeEncoder(const char* name);

    // Open the container + build video/audio streams and encoders + write_header.
    //   path        full output file path
    //   container  "mkv" / "mp4"
    //   vcodec     video encoder name ("libx264" / "h264_nvenc" / "h264_qsv")
    //   width/height/fps       video params (first frame)
    //   vbitrate_kbps          video bitrate
    //   sample_rate/channels   audio params (first frame)
    //   abitrate_kbps          audio bitrate
    // Returns false on failure (all allocations already freed internally).
    bool Open(const std::string& path, const std::string& container,
              const std::string& vcodec,
              int width, int height, double fps, int vbitrate_kbps,
              int sample_rate, int channels, int abitrate_kbps);

    // Encode and mux one video frame (CFR). src is a source-format AVFrame (shared-memory
    // copy, I420/NV12/10-bit), converted internally via sws to the encoder pix_fmt.
    // pts_us is the **source PTS** (us, used directly, no absorption/compression); gaps
    // from source-PTS forward jumps / dropped frames are filled with the previous frame
    // stepped by interval_us (capped at
    // kMaxGapFillFrames) — no fast-forward, dropped frames become a smooth hold.
    // src==NULL flushes the video encoder.
    bool EncodeVideoCfr(AVFrame* src, int64_t pts_us, int64_t interval_us);

    // Encode and mux one audio frame. src is a source sample frame (shared-memory copy,
    // any Chromium sample format), swr-lazily rebuilt internally + converted to the
    // encoder format + framed into fixed frame_size buffers. PTS follows sample count.
    bool EncodeAudio(AVFrame* src);

    // Pad silence: generate nSamples silence samples (in the most recent source audio
    // params, same format as real audio), through the same swr+encode path — keeps the
    // audio timeline continuous (the key to A/V sync after ad skips / source switches).
    void FillSilence(int64_t nSamples);

    // Pad silence by microseconds: converts to sample count at the encoder rate then
    // calls FillSilence.
    void FillSilenceUs(int64_t us);

    // Align the audio timeline to the video end at finalize time (the closing
    // counterpart of FillSilence): drop queued audio frames that start beyond
    // audio_target_us and trim the tail of the framing buffer so the audio stream ends
    // where the video does. Without this, an audio queue that is ahead at Stop (typical
    // at 3x speed: audio enters the queue at source-sample pace while the video driver
    // only drains up to the current video PTS) keeps encoding past the last video frame
    // — the player then shows a frozen last frame over ~seconds of leftover audio.
    // audio_target_us: video-timeline position (us) the audio must not exceed.
    void TruncateAudioToUs(int64_t audio_target_us);

    // Encoder sample rate (for audio timeline math; 0 when there is no audio stream).
    int SampleRate() const;

    // Finalize: flush encoders -> write trailer -> close file -> free everything.
    void Finalize();

    // Bytes written so far (feeds OBS get_total_bytes).
    int64_t FileSize() const;

    // Current audio sample position (us, sample-count timeline) — CefMediaRecorder uses
    // it to tell whether audio lags video.
    int64_t AudioPositionUs() const;

private:
    bool OpenVideoStream(const std::string& vcodec, int width, int height, double fps, int vbitrate_kbps);
    bool OpenAudioStream(int sample_rate, int channels, int abitrate_kbps);
    bool EncodeVideoFrame(AVFrame* pFrame);   // send/receive + WritePacket
    bool EncodeAudioFrame(AVFrame* pFrame);   // one full frame (frame_size samples) to encoder
    void WritePacket(AVPacket* pkt);
    AVFrame* NewAudioFrame(int nSamples, int nSampleFmt, int nSampleRate, int nChannels);
    AVFrame* NewSilenceFrame(int nSamples);
    void EnsureSwr(AVFrame* pFrame);          // lazily build/rebuild Swr (channels/rate/format changes)
    void PushResampled(int nSamples);         // feed the resample frame into audio_cache framing

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    AVCodecContext* vcodec_ = nullptr;
    AVCodecContext* acodec_ = nullptr;
    AVPacket* vpkt_ = nullptr;
    AVPacket* apkt_ = nullptr;

    SwsContext* sws_ = nullptr;
    AVFrame* video_dst_ = nullptr;   // encoder pix_fmt target frame (reused)
    AVFrame* video_prev_ = nullptr;  // previous frame in encoder format (CFR fill;
                                     // av_frame_copy deep copy, does not share buffers with
                                     // video_dst_, so the next sws cannot overwrite it)
    int64_t next_pts_us_ = INT64_MIN; // CFR counter: container us position the next frame /
                                      // fill frame should occupy
    int64_t last_encoded_pts_us_ = INT64_MIN; // pts of the last encoded frame (monotonicity
                                              // guard baseline)
    static constexpr int64_t kMaxGapFillFrames = 1000; // per-gap fill cap (guards against
                                                       // pathological cases)
    int encoder_pix_fmt_ = AV_PIX_FMT_NONE;

    SwrContext* swr_ = nullptr;
    AVFrame* resample_frame_ = nullptr;
    AVFrame* audio_cache_ = nullptr; // fixed frame_size framing buffer
    int audio_cache_count_ = 0;      // samples currently filled in the cache
    int64_t audio_encode_frames_ = 0; // full frames sent to the encoder (sample-count
                                      // timeline denominator)

    // Most recent source audio params (FillSilence / swr rebuild baseline)
    int last_src_fmt_ = AV_SAMPLE_FMT_NONE;
    int last_src_channels_ = 0;
    int last_src_sample_rate_ = 0;

    // Position of consumed samples within the swr output frame (PushResampled copies it
    // into audio_cache in chunks)
    int resample_pos_ = 0;

    int64_t file_size_ = 0;
    mutable std::mutex write_mutex_;
};

#endif // RECORDER_FFMPEG_H

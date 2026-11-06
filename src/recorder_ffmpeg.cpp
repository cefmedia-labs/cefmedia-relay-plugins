#include "recorder_ffmpeg.h"

#include <obs-module.h>

#include <cstring>

#include <libavutil/samplefmt.h>

namespace {

// Max samples per single silence chunk (avoids a giant one-shot allocation).
constexpr int64_t kMaxSilenceSamples = 102400;

// us time base (container base for video/audio PTS).
const AVRational kUsBase{1, AV_TIME_BASE};

int AudioFrameSize(const AVCodecContext* ctx)
{
    return ctx && ctx->frame_size > 0 ? ctx->frame_size : 1024;
}

} // namespace

bool FfmpegRecorderCore::ProbeEncoder(const char* name)
{
    return name && avcodec_find_encoder_by_name(name) != nullptr;
}

FfmpegRecorderCore::~FfmpegRecorderCore()
{
    Finalize();
}

bool FfmpegRecorderCore::Open(const std::string& path, const std::string& container,
                              const std::string& vcodec,
                              int width, int height, double fps, int vbitrate_kbps,
                              int sample_rate, int channels, int abitrate_kbps)
{
    // Cross-recording state reset: counters such as audio_encode_frames_ must reset or
    // the second recording's audio PTS starts at the previous recording's leftover count
    // (measured: a reused core's first audio packet landed at 18.88 s = leftover 887
    // frames). CFR state is cleared too.
    audio_encode_frames_ = 0;
    audio_cache_count_ = 0;
    resample_pos_ = 0;
    last_src_fmt_ = AV_SAMPLE_FMT_NONE;
    last_src_channels_ = 0;
    last_src_sample_rate_ = 0;
    file_size_ = 0;
    next_pts_us_ = INT64_MIN;
    last_encoded_pts_us_ = INT64_MIN;

    if (width % 2) width -= 1;
    if (height % 2) height -= 1;

    // Container: mkv -> matroska / mp4 -> mp4 (selected by name).
    const char* fmt_name = (container == "mp4") ? "mp4" : "matroska";
    if (avformat_alloc_output_context2(&fmt_ctx_, NULL, fmt_name, NULL) < 0 || !fmt_ctx_)
    {
        return false;
    }

    // Single-side stream support: first-frame single-side timeout (build only the side
    // that exists).
    const bool has_video = (width > 0 && height > 0);
    const bool has_audio = (sample_rate > 0 && channels > 0);
    if (!has_video && !has_audio)
    {
        return false;
    }
    if (has_video && !OpenVideoStream(vcodec, width, height, fps, vbitrate_kbps))
    {
        return false;
    }
    if (has_audio && !OpenAudioStream(sample_rate, channels, abitrate_kbps))
    {
        return false;
    }

    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&fmt_ctx_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            return false;
        }
    }

    // No movflags=+faststart: measured on this FFmpeg build + plugin config, faststart
    // makes av_write_trailer return EINVAL and moov is never written -> MP4 without moov
    // is unplayable (mkv is unaffected, matroska has no moov). faststart also only moves
    // moov to the front after a successful trailer, so it adds no crash-recovery value for
    // local recordings. moov at the
    // end is the standard layout and players support it.
    if (avformat_write_header(fmt_ctx_, NULL) < 0)
    {
        return false;
    }

    return true;
}

namespace {

// Build + configure + open the video encoder (given encoder name / size / bitrate).
// Returns nullptr on failure (encoder missing, or runtime open failure — e.g. outdated
// NVENC driver). GLOBALHEADER depends on the container.
AVCodecContext* BuildVideoEncoder(const std::string& name, int width, int height, double fps,
                                  int vbitrate_kbps, const AVFormatContext* fmt)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec)
    {
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        return nullptr;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->gop_size = (int)(fps + 0.5);
    if (ctx->gop_size < 20)
    {
        ctx->gop_size = 20;
    }
    ctx->max_b_frames = 2;
    ctx->time_base = kUsBase; // container PTS in us

    AVDictionary* opts = NULL;
    if (name == "libx264")
    {
        // x264 uses the CRF quality mode, no fixed bitrate.
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "crf", "18", 0);
        av_dict_set(&opts, "threads", "auto", 0);
    }
    else
    {
        ctx->bit_rate = (int64_t)vbitrate_kbps * 1000;
    }

    if (name == "h264_qsv")
    {
        ctx->pix_fmt = AV_PIX_FMT_NV12;
    }

    if (fmt && (fmt->oformat->flags & AVFMT_GLOBALHEADER))
    {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ctx, codec, &opts) < 0)
    {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

} // namespace

bool FfmpegRecorderCore::OpenVideoStream(const std::string& vcodec,
                                         int width, int height, double fps, int vbitrate_kbps)
{
    // Build the stream with the requested encoder first; if a hardware encoder fails to
    // open at runtime (outdated NVENC driver / no QSV device etc.), fall back to software
    // x264.
    std::string name = vcodec;
    AVCodecContext* ctx = BuildVideoEncoder(name, width, height, fps, vbitrate_kbps, fmt_ctx_);
    if (!ctx && name != "libx264")
    {
        blog(LOG_WARNING, "[cefmedia] video encoder '%s' open failed, fallback to libx264",
             name.c_str());
        name = "libx264";
        ctx = BuildVideoEncoder(name, width, height, fps, vbitrate_kbps, fmt_ctx_);
    }
    if (!ctx)
    {
        return false;
    }

    video_stream_ = avformat_new_stream(fmt_ctx_, NULL);
    video_stream_->id = fmt_ctx_->nb_streams - 1;
    video_stream_->time_base = ctx->time_base;
    if (avcodec_parameters_from_context(video_stream_->codecpar, ctx) < 0)
    {
        avcodec_free_context(&ctx);
        return false;
    }

    encoder_pix_fmt_ = ctx->pix_fmt;
    vcodec_ = ctx;
    vpkt_ = av_packet_alloc();

    // sws target frame (encoder pix_fmt, reused buffer; av_frame refcount keeps reuse
    // after send safe).
    video_dst_ = av_frame_alloc();
    video_dst_->format = encoder_pix_fmt_;
    video_dst_->width = width;
    video_dst_->height = height;
    if (av_frame_get_buffer(video_dst_, 32) < 0)
    {
        return false;
    }
    return true;
}

bool FfmpegRecorderCore::OpenAudioStream(int sample_rate, int channels, int abitrate_kbps)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
    {
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        return false;
    }
    av_channel_layout_default(&ctx->ch_layout, channels);
    ctx->sample_rate = sample_rate;
    ctx->bit_rate = (int64_t)abitrate_kbps * 1000;
    ctx->time_base.num = 1;
    ctx->time_base.den = sample_rate;
    ctx->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

    if (fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER)
    {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* opts = NULL;
    if (strcmp(codec->name, "aac") == 0)
    {
        av_dict_set(&opts, "aac_coder", "fast", 0);
    }
    if (avcodec_open2(ctx, codec, &opts) < 0)
    {
        avcodec_free_context(&ctx);
        return false;
    }

    audio_stream_ = avformat_new_stream(fmt_ctx_, NULL);
    audio_stream_->id = fmt_ctx_->nb_streams - 1;
    audio_stream_->time_base = ctx->time_base;
    if (avcodec_parameters_from_context(audio_stream_->codecpar, ctx) < 0)
    {
        avcodec_free_context(&ctx);
        return false;
    }

    acodec_ = ctx;
    apkt_ = av_packet_alloc();

    audio_cache_ = NewAudioFrame(AudioFrameSize(acodec_), acodec_->sample_fmt,
                                 acodec_->sample_rate, acodec_->ch_layout.nb_channels);
    if (!audio_cache_)
    {
        return false;
    }
    return true;
}

bool FfmpegRecorderCore::EncodeVideoCfr(AVFrame* src, int64_t pts_us, int64_t interval_us)
{
    if (src == NULL)
    {
        return EncodeVideoFrame(NULL); // flush
    }
    if (!vcodec_ || !video_dst_)
    {
        return false;
    }
    if (interval_us <= 0)
    {
        interval_us = 33333;
    }

    sws_ = sws_getCachedContext(sws_, src->width, src->height, (AVPixelFormat)src->format,
                                video_dst_->width, video_dst_->height,
                                (AVPixelFormat)video_dst_->format,
                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_)
    {
        return false;
    }
    sws_scale(sws_, src->data, src->linesize, 0, src->height,
              video_dst_->data, video_dst_->linesize);

    // Monotonicity guard: frames whose pts does not increase (out-of-order / residual
    // backward jitter) are dropped, keeping container PTS/DTS monotonic (the MP4 muxer
    // rejects non-monotonic dts; MKV playback is cleaner too). The worker already drops
    // backward-jitter frames; this is a safety net.
    if (last_encoded_pts_us_ != INT64_MIN && pts_us <= last_encoded_pts_us_)
    {
        return true;
    }

    // CFR gap fill: on a source-PTS forward jump (dropped frames / ad skip) with a gap
    // >= half a frame interval, step-fill with the previous frame by interval_us up to this
    // frame's PTS — container time advances continuously without compressing real intervals
    // (no fast-forward; dropped frames become a smooth hold). The half-interval threshold
    // avoids filling a duplicate every frame on slight cadence drift. Capped to guard
    // against pathological cases.
    if (video_prev_ && next_pts_us_ != INT64_MIN && pts_us > next_pts_us_ &&
        pts_us - next_pts_us_ >= interval_us / 2)
    {
        int64_t fills = 0;
        while (next_pts_us_ < pts_us && fills < kMaxGapFillFrames)
        {
            video_prev_->pts = av_rescale_q(next_pts_us_, kUsBase, vcodec_->time_base);
            EncodeVideoFrame(video_prev_);
            next_pts_us_ += interval_us;
            ++fills;
        }
    }

    video_dst_->pts = av_rescale_q(pts_us, kUsBase, vcodec_->time_base);
    EncodeVideoFrame(video_dst_);
    last_encoded_pts_us_ = pts_us;
    next_pts_us_ = pts_us + interval_us;

    // Cache this frame (encoder format) for the next fill. av_frame_copy deep-copies, so
    // it does not share buffers with video_dst_ (the next sws cannot overwrite it).
    if (!video_prev_)
    {
        video_prev_ = av_frame_alloc();
        video_prev_->format = video_dst_->format;
        video_prev_->width = video_dst_->width;
        video_prev_->height = video_dst_->height;
        if (av_frame_get_buffer(video_prev_, 32) < 0)
        {
            av_frame_free(&video_prev_);
            return false;
        }
    }
    av_frame_copy(video_prev_, video_dst_);
    return true;
}

bool FfmpegRecorderCore::EncodeVideoFrame(AVFrame* pFrame)
{
    if (!vcodec_)
    {
        return false;
    }
    if (avcodec_send_frame(vcodec_, pFrame) < 0)
    {
        return false;
    }
    while (true)
    {
        int ret = avcodec_receive_packet(vcodec_, vpkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            break;
        }
        av_packet_rescale_ts(vpkt_, vcodec_->time_base, video_stream_->time_base);
        vpkt_->stream_index = video_stream_->id;
        WritePacket(vpkt_);
        av_packet_unref(vpkt_);
    }
    return true;
}

bool FfmpegRecorderCore::EncodeAudio(AVFrame* src)
{
    if (!acodec_ || !src || src->nb_samples <= 0 || src->sample_rate <= 0 ||
        src->ch_layout.nb_channels <= 0)
    {
        return false;
    }

    EnsureSwr(src);
    if (!swr_)
    {
        return false;
    }

    int nRequire = (int)swr_get_out_samples(swr_, src->nb_samples);
    if (nRequire <= 0)
    {
        nRequire = (int)((int64_t)src->nb_samples * acodec_->sample_rate / src->sample_rate) + 256;
    }

    if (!resample_frame_ || resample_frame_->nb_samples < nRequire)
    {
        if (resample_frame_)
        {
            av_frame_free(&resample_frame_);
        }
        resample_frame_ = NewAudioFrame(nRequire, acodec_->sample_fmt,
                                        acodec_->sample_rate, acodec_->ch_layout.nb_channels);
        if (!resample_frame_)
        {
            return false;
        }
    }

    int n = (int)swr_convert(swr_, resample_frame_->data, resample_frame_->nb_samples,
                             (const uint8_t**)src->extended_data, src->nb_samples);
    if (n < 0)
    {
        return false;
    }

    // Record the most recent source params (FillSilence baseline / swr rebuild check).
    last_src_fmt_ = src->format;
    last_src_channels_ = src->ch_layout.nb_channels;
    last_src_sample_rate_ = src->sample_rate;

    resample_pos_ = 0;
    PushResampled(n);
    return true;
}

void FfmpegRecorderCore::EnsureSwr(AVFrame* pFrame)
{
    if (swr_ && last_src_fmt_ == pFrame->format &&
        last_src_channels_ == pFrame->ch_layout.nb_channels &&
        last_src_sample_rate_ == pFrame->sample_rate)
    {
        return;
    }
    if (swr_)
    {
        swr_free(&swr_);
    }

    if (swr_alloc_set_opts2(&swr_,
                            &acodec_->ch_layout, (AVSampleFormat)acodec_->sample_fmt,
                            acodec_->sample_rate,
                            &pFrame->ch_layout, (AVSampleFormat)pFrame->format,
                            pFrame->sample_rate, 0, NULL) < 0)
    {
        swr_ = NULL;
        return;
    }
    if (swr_init(swr_) < 0)
    {
        swr_free(&swr_);
    }
}

void FfmpegRecorderCore::PushResampled(int nSamples)
{
    while (nSamples > 0 && audio_cache_)
    {
        const int frame_sz = audio_cache_->nb_samples;
        const int space = frame_sz - audio_cache_count_;
        const int nCopy = (nSamples < space) ? nSamples : space;

        const int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)audio_cache_->format);
        const int cache_channels = audio_cache_->ch_layout.nb_channels;
        if (av_sample_fmt_is_planar((AVSampleFormat)audio_cache_->format))
        {
            const int len = nCopy * bytes_per_sample;
            const int src_off = resample_pos_ * bytes_per_sample;
            const int dst_off = audio_cache_count_ * bytes_per_sample;
            const int nCh = (cache_channels < resample_frame_->ch_layout.nb_channels)
                                ? cache_channels : resample_frame_->ch_layout.nb_channels;
            for (int i = 0; i < nCh; ++i)
            {
                memcpy(audio_cache_->extended_data[i] + dst_off,
                       resample_frame_->data[i] + src_off, len);
            }
        }
        else
        {
            const int len = nCopy * bytes_per_sample * cache_channels;
            const int src_off = resample_pos_ * bytes_per_sample * resample_frame_->ch_layout.nb_channels;
            const int dst_off = audio_cache_count_ * bytes_per_sample * cache_channels;
            memcpy(audio_cache_->extended_data[0] + dst_off,
                   resample_frame_->data[0] + src_off, len);
        }

        audio_cache_count_ += nCopy;
        resample_pos_ += nCopy;
        nSamples -= nCopy;

        if (audio_cache_count_ >= frame_sz)
        {
            audio_cache_->pts = (int64_t)frame_sz * audio_encode_frames_ * 1000000 / acodec_->sample_rate;
            audio_cache_->duration = (int64_t)frame_sz * 1000000 / acodec_->sample_rate;
            audio_encode_frames_++;
            EncodeAudioFrame(audio_cache_);

            av_frame_free(&audio_cache_);
            audio_cache_ = NewAudioFrame(frame_sz, acodec_->sample_fmt,
                                         acodec_->sample_rate, acodec_->ch_layout.nb_channels);
            audio_cache_count_ = 0;
        }
    }
}

bool FfmpegRecorderCore::EncodeAudioFrame(AVFrame* pFrame)
{
    if (!acodec_)
    {
        return false;
    }
    pFrame->pts = av_rescale_q(pFrame->pts, kUsBase, acodec_->time_base);
    if (avcodec_send_frame(acodec_, pFrame) < 0)
    {
        return false;
    }
    while (true)
    {
        int ret = avcodec_receive_packet(acodec_, apkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            break;
        }
        av_packet_rescale_ts(apkt_, acodec_->time_base, audio_stream_->time_base);
        apkt_->stream_index = audio_stream_->id;
        WritePacket(apkt_);
        av_packet_unref(apkt_);
    }
    return true;
}

void FfmpegRecorderCore::FillSilence(int64_t nSamples)
{
    if (last_src_sample_rate_ <= 0 || last_src_channels_ <= 0)
    {
        return; // no audio baseline yet (no real audio received), nothing to pad
    }
    while (nSamples > 0)
    {
        const int chunk = (nSamples > kMaxSilenceSamples) ? (int)kMaxSilenceSamples : (int)nSamples;
        AVFrame* sil = NewSilenceFrame(chunk);
        if (!sil)
        {
            break;
        }
        EncodeAudio(sil); // through the same swr+encode path, padded silence is isomorphic
                          // with real audio
        nSamples -= chunk;
    }
}

void FfmpegRecorderCore::FillSilenceUs(int64_t us)
{
    if (!acodec_ || us <= 0)
    {
        return;
    }
    const int64_t samples = us * acodec_->sample_rate / 1000000;
    if (samples > 0)
    {
        FillSilence(samples);
    }
}

void FfmpegRecorderCore::TruncateAudioToUs(int64_t audio_target_us)
{
    // Called between the queue drain and Finalize: audio_position counts encoded samples
    // (sample-count timeline = container audio PTS). Everything beyond the video end is
    // Stop-moment drain surplus (no video covers it) — cut it wherever it currently
    // lives: in queued-not-yet-encoded samples is not applicable here (the caller
    // already dropped those frames); here we only trim the in-flight encoding state:
    //   - the partial framing buffer (audio_cache_count_ samples beyond the target)
    //   - full frames would need packet-level surgery; since this runs once right after
    //     the drain, the overshoot is at most one frame plus the partial buffer, and the
    //     rest was never encoded — see CefMediaRecorder::WorkerProc, which bounds the
    //     audio drain by the video end before calling this.
    if (!acodec_ || !audio_cache_)
    {
        return;
    }
    const int64_t encoded_us = AudioPositionUs();
    const int64_t overshoot_us = encoded_us - audio_target_us;
    if (overshoot_us <= 0)
    {
        return; // audio does not run past the video end — nothing to trim
    }
    const int64_t overshoot_samples = overshoot_us * acodec_->sample_rate / 1000000;
    if (overshoot_samples >= audio_cache_count_)
    {
        // The overshoot reaches past the partial buffer (a full surplus frame was already
        // encoded): zero the partial buffer; the one already-encoded surplus frame stays
        // (packet-level removal is not possible at this stage; bounded to one AAC frame
        // ~21 ms — inaudible).
        audio_cache_count_ = 0;
    }
    else
    {
        audio_cache_count_ -= (int)overshoot_samples;
    }
}

int FfmpegRecorderCore::SampleRate() const
{
    return acodec_ ? acodec_->sample_rate : 0;
}

AVFrame* FfmpegRecorderCore::NewSilenceFrame(int nSamples)
{
    AVFrame* p = av_frame_alloc();
    if (!p)
    {
        return NULL;
    }
    p->nb_samples = nSamples;
    p->format = last_src_fmt_;
    av_channel_layout_default(&p->ch_layout, last_src_channels_);
    p->sample_rate = last_src_sample_rate_;
    if (av_frame_get_buffer(p, 0) < 0)
    {
        av_frame_free(&p);
        return NULL;
    }
    av_samples_set_silence(p->extended_data, 0, nSamples, last_src_channels_,
                           (AVSampleFormat)last_src_fmt_);
    return p;
}

AVFrame* FfmpegRecorderCore::NewAudioFrame(int nSamples, int nSampleFmt, int nSampleRate,
                                           int nChannels)
{
    AVFrame* p = av_frame_alloc();
    if (!p)
    {
        return NULL;
    }
    p->nb_samples = nSamples;
    p->format = nSampleFmt;
    av_channel_layout_default(&p->ch_layout, nChannels);
    p->sample_rate = nSampleRate;
    if (av_frame_get_buffer(p, 0) < 0)
    {
        av_frame_free(&p);
        return NULL;
    }
    return p;
}

void FfmpegRecorderCore::Finalize()
{
    if (!fmt_ctx_)
    {
        return;
    }

    // 1) flush the video encoder
    EncodeVideoFrame(NULL);

    // 2) flush the audio encoder + pad the trailing partial frame (AAC needs a full
    //    frame_size; zero-fill avoids dropping samples)
    if (acodec_)
    {
        if (audio_cache_ && audio_cache_count_ > 0 && audio_cache_count_ < audio_cache_->nb_samples)
        {
            av_samples_set_silence(audio_cache_->extended_data, audio_cache_count_,
                                   audio_cache_->nb_samples - audio_cache_count_,
                                   audio_cache_->ch_layout.nb_channels,
                                   (AVSampleFormat)audio_cache_->format);
            audio_cache_->pts = (int64_t)audio_cache_->nb_samples * audio_encode_frames_ * 1000000 / acodec_->sample_rate;
            audio_cache_->duration = (int64_t)audio_cache_->nb_samples * 1000000 / acodec_->sample_rate;
            audio_encode_frames_++;
            EncodeAudioFrame(audio_cache_);
        }
        avcodec_send_frame(acodec_, NULL);
        while (true)
        {
            int ret = avcodec_receive_packet(acodec_, apkt_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            if (ret < 0)
            {
                break;
            }
            av_packet_rescale_ts(apkt_, acodec_->time_base, audio_stream_->time_base);
            apkt_->stream_index = audio_stream_->id;
            WritePacket(apkt_);
            av_packet_unref(apkt_);
        }
    }

    // 3) write the trailer + close the file
    av_write_trailer(fmt_ctx_);
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
    {
        avio_closep(&fmt_ctx_->pb);
    }

    // 4) free everything
    avformat_free_context(fmt_ctx_);
    fmt_ctx_ = NULL;
    video_stream_ = NULL;
    audio_stream_ = NULL;
    if (vcodec_) { avcodec_free_context(&vcodec_); vcodec_ = NULL; }
    if (acodec_) { avcodec_free_context(&acodec_); acodec_ = NULL; }
    if (vpkt_) { av_packet_free(&vpkt_); vpkt_ = NULL; }
    if (apkt_) { av_packet_free(&apkt_); apkt_ = NULL; }
    if (sws_) { sws_freeContext(sws_); sws_ = NULL; }
    if (swr_) { swr_free(&swr_); swr_ = NULL; }
    if (video_dst_) { av_frame_free(&video_dst_); video_dst_ = NULL; }
    if (resample_frame_) { av_frame_free(&resample_frame_); resample_frame_ = NULL; }
    if (audio_cache_) { av_frame_free(&audio_cache_); audio_cache_ = NULL; }
    if (video_prev_) { av_frame_free(&video_prev_); video_prev_ = NULL; }
    next_pts_us_ = INT64_MIN;
    last_encoded_pts_us_ = INT64_MIN;
    audio_cache_count_ = 0;
    file_size_ = 0;
}

int64_t FfmpegRecorderCore::FileSize() const
{
    return file_size_;
}

int64_t FfmpegRecorderCore::AudioPositionUs() const
{
    if (!acodec_)
    {
        return 0;
    }
    const int64_t frame_sz = AudioFrameSize(acodec_);
    const int64_t total = audio_encode_frames_ * frame_sz + audio_cache_count_;
    return total * 1000000 / acodec_->sample_rate;
}

void FfmpegRecorderCore::WritePacket(AVPacket* pkt)
{
    std::lock_guard<std::mutex> lk(write_mutex_);
    if (!fmt_ctx_)
    {
        return;
    }
    if (av_interleaved_write_frame(fmt_ctx_, pkt) < 0)
    {
        return; // write failed (disk full etc.): silent; the caller counts dropped frames
    }
    if (fmt_ctx_->pb)
    {
        file_size_ = avio_tell(fmt_ctx_->pb);
    }
}

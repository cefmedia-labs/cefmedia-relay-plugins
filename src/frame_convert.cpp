#include "frame_convert.h"

// Format values on the wire are the FFmpeg enums (AVPixelFormat /
// AVSampleFormat): the relay translates the renderer's Chromium media:: enum
// integers before frames enter its send path. The data layouts are identical
// between the two numberings (I420 == YUV420P, etc.), so this is a tag remap —
// no pixel/sample data conversion.
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

bool CefVideoToObsFrame(cefmedia_video_frame* in, const uint8_t* payload,
                        obs_source_frame* out)
{
    enum video_format fmt;
    switch (in->format) {
    case AV_PIX_FMT_YUV420P:
        fmt = VIDEO_FORMAT_I420;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        fmt = VIDEO_FORMAT_I010;
        break;
    case AV_PIX_FMT_NV12:
        fmt = VIDEO_FORMAT_NV12;
        break;
    case AV_PIX_FMT_P010LE:
        fmt = VIDEO_FORMAT_P010;
        break;
    default:
        return false; // Unsupported (the relay passes unknown formats through)
    }

    if (in->width == 0 || in->height == 0) {
        return false;
    }

    const int planes = (fmt == VIDEO_FORMAT_NV12 || fmt == VIDEO_FORMAT_P010) ? 2 : 3;

    out->format = fmt;
    out->width = in->width;
    out->height = in->height;
    out->flip = false;
    out->flags = 0;
    out->trc = VIDEO_TRC_DEFAULT;
    out->max_luminance = 0;

    // Reference the message-block planes directly: obs_source_output_video copies
    // internally. Plane layout per the frame contract: payload + planes[i].offset.
    for (int i = 0; i < planes; ++i) {
        out->data[i] = const_cast<uint8_t*>(payload) + in->planes[i].offset;
        out->linesize[i] = in->planes[i].stride;
    }
    for (int i = planes; i < MAX_AV_PLANES; ++i) {
        out->data[i] = nullptr;
        out->linesize[i] = 0;
    }

    // Web-encoded video is usually BT.709 limited range; 10-bit is shown as SDR 709 for
    // now (Phase 3 will correct per the source color space).
    out->full_range = false;
    video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL, out->color_matrix,
                                out->color_range_min, out->color_range_max);
    return true;
}

bool CefAudioToObsAudio(cefmedia_audio_buffer* in, const uint8_t* payload,
                        obs_source_audio* out)
{
    enum audio_format fmt;
    switch (in->format) {
    case AV_SAMPLE_FMT_U8:
        fmt = AUDIO_FORMAT_U8BIT;
        break;
    case AV_SAMPLE_FMT_S16:
        fmt = AUDIO_FORMAT_16BIT;
        break;
    case AV_SAMPLE_FMT_S32:
        fmt = AUDIO_FORMAT_32BIT;
        break;
    case AV_SAMPLE_FMT_FLT:
        fmt = AUDIO_FORMAT_FLOAT;
        break;
    case AV_SAMPLE_FMT_S16P:
        fmt = AUDIO_FORMAT_16BIT_PLANAR;
        break;
    case AV_SAMPLE_FMT_FLTP:
        fmt = AUDIO_FORMAT_FLOAT_PLANAR;
        break;
    case AV_SAMPLE_FMT_S32P:
        fmt = AUDIO_FORMAT_32BIT_PLANAR;
        break;
    case AV_SAMPLE_FMT_U8P:
        fmt = AUDIO_FORMAT_U8BIT_PLANAR;
        break;
    default:
        return false;
    }

    const int channels = in->channel_count;
    if (channels <= 0 || in->sample_rate <= 0 || in->data_size == 0) {
        return false;
    }

    const size_t bytes_per_sample = get_audio_bytes_per_channel(fmt);
    if (bytes_per_sample == 0) {
        return false;
    }
    const uint32_t frames =
        static_cast<uint32_t>(in->data_size / bytes_per_sample / static_cast<size_t>(channels));
    if (frames == 0) {
        return false;
    }

    // IPC audio is stored per-channel in contiguous blocks (channel i starts at
    // i * (data_size / channel_count), per the frame contract); planar formats
    // point each channel at its block, interleaved uses data[0] only.
    const bool planar = is_audio_planar(fmt);
    const size_t per_channel_bytes = static_cast<size_t>(frames) * bytes_per_sample;
    for (int i = 0; i < MAX_AV_PLANES; ++i) {
        if (planar && i < channels) {
            out->data[i] = const_cast<uint8_t*>(payload) + static_cast<size_t>(i) * per_channel_bytes;
        } else {
            out->data[i] = (i == 0) ? const_cast<uint8_t*>(payload) : nullptr;
        }
    }

    out->frames = frames;
    out->format = fmt;
    out->speakers = CefChannelCountToSpeakers(channels);
    out->samples_per_sec = static_cast<uint32_t>(in->sample_rate);
    return true;
}

enum speaker_layout CefChannelCountToSpeakers(int channels)
{
    switch (channels) {
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    case 6:
        return SPEAKERS_5POINT1;
    case 8:
        return SPEAKERS_7POINT1;
    default:
        return SPEAKERS_UNKNOWN;
    }
}

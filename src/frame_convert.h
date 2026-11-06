#ifndef FRAME_CONVERT_H
#define FRAME_CONVERT_H

#include <obs.h>

#include <cstdint>

#include "cefmedia.h"

// Maps cefmedia frame blocks ([96B header][payload] blocks) to the
// *struct fields* of obs_source_frame / obs_source_audio (format / size / plane
// pointers / channels). data[] points directly into the received message block:
// the caller must call obs_source_output_video / output_audio synchronously
// inside the cefmedia callback (OBS copies internally; the buffer is reusable
// after the callback returns). The timestamp is written separately by the caller
// (OutputTimeline) into out->timestamp.
//
// Returns false if the format is unsupported or the frame is invalid (caller should skip).
bool CefVideoToObsFrame(cefmedia_video_frame* in, const uint8_t* payload,
                        obs_source_frame* out);
bool CefAudioToObsAudio(cefmedia_audio_buffer* in, const uint8_t* payload,
                        obs_source_audio* out);

// Channel count -> obs speaker layout (SPEAKERS_UNKNOWN when unsupported; OBS falls back
// to stereo).
enum speaker_layout CefChannelCountToSpeakers(int channels);

#endif // FRAME_CONVERT_H

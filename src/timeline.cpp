// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "timeline.h"

#include <chrono>

namespace {

// Forward-jump absorption threshold: >100 ms is a real jump (seek / source break). With
// max-PTS-delta detection, B-frame reorder noise (<= one frame interval ~40 ms) never
// triggers; a real jump (0.1 s to seconds) always does.
constexpr int64_t kAbsorbThresholdUs = 100000; // 100 ms

// Large backward-jump threshold: >2 s is a backward seek, re-anchored as a clean new
// segment (output timeline restarts from the beginning).
constexpr int64_t kBackwardResetUs = 2000000; // 2 s

// Debounce window: audio/video frames of one seek arrive within milliseconds of each
// other and each would detect the jump; only the first absorption inside the window is
// allowed, so the base does not get the jump added twice (which would roll output back).
constexpr int64_t kAbsorbGuardMs = 250; // 250 ms

int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void OutputTimeline::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    base_us_ = INT64_MIN;
    video_max_pts_us_ = INT64_MIN;
    audio_max_pts_us_ = INT64_MIN;
    last_absorb_mono_ms_ = INT64_MIN;
}

int64_t OutputTimeline::OnVideoFrameUs(int64_t ts_us)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (base_us_ == INT64_MIN) {
        base_us_ = ts_us;
        video_max_pts_us_ = ts_us;
        return 0;
    }

    if (ts_us > video_max_pts_us_) {
        int64_t jump = ts_us - video_max_pts_us_;
        if (jump > kAbsorbThresholdUs && CanAbsorbLocked()) {
            base_us_ += jump;
            MarkAbsorbLocked();
        }
        video_max_pts_us_ = ts_us;
    } else if (video_max_pts_us_ - ts_us > kBackwardResetUs) {
        // Large backward jump (backward seek): re-anchor as a clean new segment.
        base_us_ = ts_us;
        video_max_pts_us_ = ts_us;
    }

    return (ts_us - base_us_) * 1000;
}

int64_t OutputTimeline::OnAudioFrameUs(int64_t ts_us)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (base_us_ == INT64_MIN) {
        base_us_ = ts_us;
        audio_max_pts_us_ = ts_us;
        return 0;
    }

    if (ts_us > audio_max_pts_us_) {
        int64_t jump = ts_us - audio_max_pts_us_;
        // Audio participates in absorption too: in a seek where audio arrives before video,
        // audio absorbs first; when video then detects it inside the window, CanAbsorbLocked
        // skips it, avoiding a double add.
        if (jump > kAbsorbThresholdUs && CanAbsorbLocked()) {
            base_us_ += jump;
            MarkAbsorbLocked();
        }
        audio_max_pts_us_ = ts_us;
    } else if (audio_max_pts_us_ - ts_us > kBackwardResetUs) {
        base_us_ = ts_us;
        audio_max_pts_us_ = ts_us;
    }

    return (ts_us - base_us_) * 1000;
}

bool OutputTimeline::CanAbsorbLocked()
{
    if (last_absorb_mono_ms_ == INT64_MIN) {
        return true;
    }
    return (NowMs() - last_absorb_mono_ms_) >= kAbsorbGuardMs;
}

void OutputTimeline::MarkAbsorbLocked()
{
    last_absorb_mono_ms_ = NowMs();
}

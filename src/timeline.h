#ifndef TIMELINE_H
#define TIMELINE_H

#include <cstdint>
#include <mutex>

// Output timeline controller: maps cefmedia media:: timestamps (us) to OBS output
// timestamps (ns), absorbing forward PTS jumps (seek / source switch / stream break) so
// the output timeline stays continuous.
//
// Key points:
//  - Audio and video share the same base. On a real seek both jump together; absorbing
//    with the shared base keeps both streams continuous and in sync.
//  - A video-only break (audio continuous) is absorbed for video only; audio briefly lags
//    then catches up on its own (self-healing, jump <= a few hundred ms).
//  - Jump detection uses the recent MAX PTS rather than the previous frame: under B-frame
//    reordering the previous frame's PTS can swing back, so a direct diff would mistake a
//    frame-interval multiple for a jump; a delta against the max PTS only fires on a real
//    jump / re-anchor frame.
//  - 250 ms debounce: one seek is detected once by each of audio/video; only one
//    absorption is allowed, so the jump is not added to the base twice.
class OutputTimeline {
public:
    OutputTimeline() = default;
    OutputTimeline(const OutputTimeline&) = delete;
    OutputTimeline& operator=(const OutputTimeline&) = delete;

    // Returns the output timestamp (ns) this frame should use. ts_us is the cefmedia
    // media:: timestamp (us).
    int64_t OnVideoFrameUs(int64_t ts_us);
    int64_t OnAudioFrameUs(int64_t ts_us);

    // Reset when a capture session restarts (before ReaderStart).
    void Reset();

private:
    bool CanAbsorbLocked();
    void MarkAbsorbLocked();

    std::mutex mutex_;
    int64_t base_us_ = INT64_MIN;           // output = (ts - base) * 1000
    int64_t video_max_pts_us_ = INT64_MIN;  // recent max video PTS (reliable jump baseline
                                            // under B-frame reordering)
    int64_t audio_max_pts_us_ = INT64_MIN;
    int64_t last_absorb_mono_ms_ = INT64_MIN; // debounce: one seek absorbed only once
};

#endif // TIMELINE_H

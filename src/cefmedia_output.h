#ifndef CEFMEDIA_OUTPUT_H
#define CEFMEDIA_OUTPUT_H

#include <cstdint>
#include <string>

// OBS output plugin: records the CefMedia Relay stream directly from the
// libipc receive callbacks to a file.
//
// - Registered as a raw AV output (OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO), but
//   obs_output_begin_data_capture is NOT called — frames come from the external
//   libipc receive fan-out, not the OBS main compositor;
// - start(): CefMediaApi::Load() -> register reader callbacks (idempotent append) ->
//   ReaderStart (refcount++) -> recorder.StartRecord; stop(): recorder.StopRecord ->
//   ReaderStop (refcount--, stops the reader threads only at zero);
// - coexists with the source plugin via the ipc client's multi-callback fan-out +
//   reference counting.
//
// Manual control: Tools -> "CefMedia Recorder" toggle + the Qt dock recorder panel
// (recorder_dock), both driving the shared control surface below (single state source,
// stop signal as the error fallback). The container/encoder/bitrate/speed come from
// recorder.cfg (see cefmedia_output.cpp LoadRecorderPrefs). A Settings->Output
// properties dialog is optional; obs_output_active is unreliable for this raw output,
// so the controller tracks state itself.
#define CEFMEDIA_OUTPUT_ID "cefmedia_output"
#define CEFMEDIA_OUTPUT_NAME "CefMedia Recorder"

// Called from obs_module_load (register the output type + frontend controller).
void RegisterCefMediaOutput();

// Called from obs_module_unload (release the menu output + unregister frontend callbacks).
void UnregisterCefMediaOutput();

// ---- Shared recording control surface (UI thread; Tools menu + dock panel both call
// these; all state lives in cefmedia_output.cpp) ----

// Start a new recording: re-read prefs, generate a timestamped path, create/update the
// output, obs_output_start. Returns false when already recording or the start failed.
bool RecorderStartNew();

// Stop the current recording (idempotent; finalize the recorder + release the reader).
void RecorderStopCurrent();

// Recording state (self-maintained; obs_output_active is always false for raw outputs).
bool RecorderIsRecording();

// File size written so far (0 when not recording).
int64_t RecorderFileSize();

// Playback speed multiplier in permille (1000 = 1x; valid set 1000/1500/2000/3000 =
// 1x/1.5x/2x/3x). Only applied to CefMedia Relay WHILE RECORDING (the dock gates the
// RateSet call; stopping the recording restores 1x); the selection persists to
// recorder.cfg for the next start.
int RecorderSpeed();

// Capture channel the recorder is bound to (0-based 0..4; UI shows 1..5). The
// selection persists to recorder.cfg ("channel" key, UI value 1..5); effective on
// the next start.
int RecorderChannel();

// Set the capture channel (0-based; clamped/validated), persists to recorder.cfg;
// effective on the next start.
void RecorderSetChannel(int channel);

// Set the speed multiplier (validates to the permille set above), persists to
// recorder.cfg immediately; CefMedia Relay-side application is the dock's job (recording-gated).
void RecorderSetSpeed(int speed_permille);

// Output container format ("mkv"/"mp4").
std::string RecorderFormat();

// Set the container format (validates to mkv|mp4), persists to recorder.cfg; effective
// on the next start.
void RecorderSetFormat(const std::string& format);

// Which streams to record: "va" (video+audio) / "v" (video only) / "a" (audio only).
std::string RecorderSource();

// Set the source selection (validates to va|v|a), persists to recorder.cfg; effective on
// the next start (the recorder drops the other side's frames at enqueue time).
void RecorderSetSource(const std::string& source);

// Video encoder preference for the next recording ("auto" / "libx264" /
// "h264_nvenc" / "h264_qsv"; "auto" probes NVENC -> QSV -> x264 at start).
std::string RecorderVcodec();

// Set the video encoder (validates to the set above, anything else falls back to
// "auto"), persists to recorder.cfg; effective on the next start.
void RecorderSetVcodec(const std::string& vcodec);

// Video bitrate in kbps for the next recording (clamped to 500..60000).
int RecorderVbitrate();

// Set the video bitrate (clamped to 500..60000), persists to recorder.cfg; effective on
// the next start.
void RecorderSetVbitrate(int vbitrate_kbps);

// Audio bitrate in kbps for the next recording (clamped to 32..512).
int RecorderAbitrate();

// Set the audio bitrate (clamped to 32..512), persists to recorder.cfg; effective on
// the next start.
void RecorderSetAbitrate(int abitrate_kbps);

// Auto-stop: end the recording when a video frame carries CefFrameFlag_End (CefMedia Relay's
// playback-ended notification after the current video's last frame). Persists to
// recorder.cfg; effective on the next start.
bool RecorderAutoStop();

// Set the auto-stop switch (persists to recorder.cfg; effective on the next start).
void RecorderSetAutoStop(bool enabled);

// CefMedia Relay executable path for the recording-start auto-launch probe (recorder.cfg
// key cefmedia_path; legacy browser_path still read; empty = disabled). When set and
// a start sees no frames for ~4 s, CefMedia Relay is launched with --enable-cefmedia
// (ProcessSingleton folds redundant launches). Re-reads recorder.cfg on every call
// (the detection module cefmedia_detect.cpp reads it for its one-shot check; a manual
// cfg edit between sessions is picked up).
std::string RecorderCefMediaPath();

// Set the CefMedia Relay executable path (persists to recorder.cfg immediately,
// under the cefmedia_path key). Also the persistence entry for the detection
// module's probed-path hit.
void RecorderSetCefMediaPath(const std::string& path);

// Output folder (expanded absolute path; the default is %USERPROFILE%\Videos\CefMedia
// when nothing was configured).
std::string RecorderFolder();

// Set the output folder: creates the directory (returns false and keeps the old value
// when the path is empty / cannot be created), persists to recorder.cfg; effective on
// the next start.
bool RecorderSetFolder(const std::string& folder);

// Actual recorded duration (us): the file-timeline position encoding has reached (0 when
// not recording). Stops advancing while the CefMedia Relay pauses/stalls.
int64_t RecorderRecordedDurationUs();

// Auto-stop finalizer: completes an async stop requested from the reader thread
// (RequestStopAsync on the End-flag frame). UI THREAD ONLY (joins the recorder worker +
// releases the reader reference). The dock's 1 s timer calls it; no-op when nothing is
// pending (still recording / not recording).
void RecorderFinalizeIfPending();

// ---- CefMediaRelay install/run detection lives in cefmedia_detect.h:
// CefMediaRelayLaunchState / DetectCefMediaRelayOnce /
// CefMediaRelayLaunchStateNow / CefMediaRelayExePath live there;
// recorder_dock.cpp includes cefmedia_detect.h for the hint row, and the
// detection module reads/persists cefmedia_path through the two accessors
// above. ----

#endif // CEFMEDIA_OUTPUT_H

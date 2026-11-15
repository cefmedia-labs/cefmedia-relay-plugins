// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "cefmedia_output.h"

#include <obs-module.h>
#include <obs.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <windows.h>

#include "cefmedia.h"
#include "cefmedia_api.h"
#include "cefmedia_recorder.h"
#include "channel_frame_binding.h" // reader binding (callbacks + reader ref, RAII)
#include "recorder_prefs.h"        // MenuPrefs + recorder.cfg load/save
#include "frontend_api.h"
#include "recorder_dock.h"
#include "cefmedia_detect.h"
#include "i18n.h"
#include "path_util.h"

namespace {

// obs_frontend_event enum value (OBS_FRONTEND_EVENT_FINISHED_LOADING, frontend-api.h).
constexpr int kFrontendFinishedLoading = 26;

// ---- Frontend controller: Tools menu start/stop + setting persistence ----
// OBS's Settings->Output dropdown does not necessarily list plugin raw outputs, so the
// "CefMedia Recorder" toggle is added to the Tools menu via obs-frontend-api (visible in
// any output mode). Frames/recording are handled by the registered cefmedia_output type;
// this code only creates/starts/stops/releases the output and persists format/encoder/
// bitrate preferences.
// The preference model + cfg file IO live in recorder_prefs.{h,cpp}; this alias
// keeps the control-surface call sites below unchanged.
MenuPrefs& g_prefs = RecorderPrefs();
obs_output_t* g_menu_output = nullptr;
struct CefMediaOutputData; // forward declaration (full definition below)
// Private data of the menu output (recorded at create). Used to finalize the recorder
// directly on menu stop — obs_output_stop is a no-op for a raw output without
// begin_data_capture (active is always false).
CefMediaOutputData* g_menu_output_data = nullptr;
bool g_menu_added = false;
// Menu start/stop state: does not rely on obs_output_active (a raw output never calls
// begin_data_capture, OBS never marks it active — always false); self-maintained, with the
// stop signal as a fallback reset (error stops also recover).
bool g_menu_recording = false;

// ---------------------------------------------------------------------------
// CefMedia Relay auto-launch (recorder.cfg key: cefmedia_path; legacy browser_path still read).
//
// A recording start that sees no frames within the probe window launches the
// configured CefMedia Relay with --enable-cefmedia. Detection is frame-driven (not
// process-driven): CefMedia Relay may be running but silent (no capture target /
// page without video), and a redundant CreateProcess is harmless either way —
// the Chromium ProcessSingleton hands the command line to the first process
// and exits. The probe callback is registered at module load so it observes
// frames whenever they flow, independent of any recording.
// ---------------------------------------------------------------------------

// CefMediaRelay install/run detection, frame-liveness probe and launch live in
// cefmedia_detect.{h,cpp}; this file keeps only the recording
// control surface and calls into it (see FrontendEventCallback / RecorderStartNew).
// The detection module reads/persists cefmedia_path through the accessors below.

struct CefMediaOutputData {
    CefMediaOutputData()
        // The binding stores &recorder (its callbacks are static members of
        // CefMediaRecorder); |recorder| is declared before |reader|, so the
        // pointer is valid from construction through destruction.
        : reader(&CefMediaRecorder::VideoCallback, &CefMediaRecorder::AudioCallback,
                 &recorder)
    {
    }

    obs_output_t* output = nullptr;
    CefMediaRecorder recorder;
    // Reader binding (frame-callback registration + reader reference), bound
    // at start(). Declared right after |recorder| (its callbacks cast to the
    // recorder) and detached first on destruction — see ChannelFrameBinding.
    ChannelFrameBinding reader;
    int channel = 0;           // capture channel this recording is bound to (0-based)

    std::string path;
    std::string format = "mkv";
    std::string vcodec = "auto";
    int vbitrate_kbps = 8000;
    int abitrate_kbps = 160;
    std::string source = "va"; // "va" (video+audio) / "v" (video only) / "a" (audio only)
    bool autostop = false; // stop the recording on an End-flag video frame
};

// Forward declarations (error callback registered in create, defined below).
static void OnRecorderError(void* obj);
static void StopMenuRecording();

// ReaderStop exactly once (both the error path and the normal stop go through here;
// idempotent — ChannelFrameBinding::Detach). Unregisters the recorder's frame
// callbacks (mandatory before the recorder dies — a left-behind registry entry would
// be a dangling pointer on the next frame) and releases the reader reference
// (refcount--; the channel's threads stay alive while the source plugin — or another
// recording — still holds one).
void ReleaseReader(CefMediaOutputData* d)
{
    d->reader.Detach();
}

// Resolve vcodec: "auto" -> probe the fallback chain (NVENC -> QSV -> x264).
std::string ResolveCodec(const std::string& pref)
{
    if (pref == "auto")
    {
        if (FfmpegRecorderCore::ProbeEncoder("h264_nvenc")) return "h264_nvenc";
        if (FfmpegRecorderCore::ProbeEncoder("h264_qsv")) return "h264_qsv";
        if (FfmpegRecorderCore::ProbeEncoder("libx264")) return "libx264";
        return "";
    }
    return FfmpegRecorderCore::ProbeEncoder(pref.c_str()) ? pref : "";
}

const char* get_name(void*)
{
    return CEFMEDIA_OUTPUT_NAME;
}

void* create(obs_data_t* settings, obs_output_t* output)
{
    auto* d = new CefMediaOutputData;
    d->output = output;
    d->channel = g_prefs.channel; // start-time snapshot of the channel binding
    d->recorder.SetErrorCallback(&OnRecorderError, d);
    g_menu_output_data = d; // the menu output is the only one; record its data for direct
                            // finalization
    return d;
}

void destroy(void* data)
{
    auto* d = static_cast<CefMediaOutputData*>(data);
    if (g_menu_output_data == d)
    {
        g_menu_output_data = nullptr;
    }
    if (d)
    {
        delete d;
    }
}

bool start(void* data)
{
    auto* d = static_cast<CefMediaOutputData*>(data);

    if (d->path.empty())
    {
        blog(LOG_WARNING, "[cefmedia] output start failed: output file path not set");
        obs_output_set_last_error(d->output, "Output file path not set");
        return false;
    }
    if (!CefMediaApi::Instance().Load())
    {
        blog(LOG_WARNING, "[cefmedia] output start failed: failed to load cefmedia.dll");
        obs_output_set_last_error(d->output, "Failed to load cefmedia.dll");
        return false;
    }
    // The video-encoder probe only gates recordings that actually encode video
    // ("va"/"v"). Source = Audio Only skips it: a machine without any H.264
    // encoder (slim FFmpeg build, broken NVENC/QSV driver) must still be able to
    // record audio-only — the AAC encoder is independent of the H.264 probe.
    const std::string codec = (d->source != "a") ? ResolveCodec(d->vcodec) : "";
    if (d->source != "a" && codec.empty())
    {
        blog(LOG_WARNING, "[cefmedia] output start failed: no usable video encoder");
        obs_output_set_last_error(d->output,
                                  "No usable video encoder (needs libx264 / NVENC / QSV)");
        return false;
    }

    // Bind the reader to the recording's channel: registers the frame callbacks
    // (idempotent append; only the bound channel's frames reach the recorder) +
    // ReaderStart(channel) (refcount++; the first consumer on that channel
    // spawns its threads). Idempotent (ChannelFrameBinding::Attach).
    d->reader.Attach(d->channel);

    d->recorder.SetAutoStop(d->autostop);
    d->recorder.StartRecord(d->path, d->format, codec, d->vbitrate_kbps, d->abitrate_kbps,
                            d->source);
    blog(LOG_INFO,
         "[cefmedia] output started: path=%s format=%s codec=%s source=%s channel=%d "
         "autostop=%d",
         d->path.c_str(), d->format.c_str(), codec.c_str(), d->source.c_str(),
         d->channel + 1, d->autostop ? 1 : 0);
    return true;
}

void stop(void* data, uint64_t /*ts*/)
{
    auto* d = static_cast<CefMediaOutputData*>(data);
    // Complete the join outside the lock (holding the lock while stopping would deadlock
    // with reader/worker threads that hold it inside their callbacks).
    d->recorder.StopRecord();
    ReleaseReader(d);
    blog(LOG_INFO, "[cefmedia] output stopped");
}

// Init failure (no encoder / stream setup failed) report. Runs on the recorder WORKER
// thread (it cannot join itself) — just signal the output; the worker already finalized
// and exited, so a later StopRecord's join returns immediately. OBS fires the stop
// signal synchronously ON THIS THREAD, so OnMenuOutputStop (the signal handler) must
// never join reader threads — see its comment.
//
// Auto-stop (End-flag frame) does NOT go through here: OnVideoFrame only requests the
// stop (RequestStopAsync — flag + notify, no join, no signal) and the dock's 1 s timer
// detects the started-flag drop and finalizes on the UI thread
// (RecorderFinalizeIfPending). Everything join-like stays off the reader threads.
static void OnRecorderError(void* obj)
{
    auto* d = static_cast<CefMediaOutputData*>(obj);
    obs_output_signal_stop(d->output, OBS_OUTPUT_ENCODE_ERROR);
}

static obs_properties_t* get_properties(void*)
{
    obs_properties_t* p = obs_properties_create();

    obs_properties_add_path(p, "path", T("Props.OutputFile"), OBS_PATH_FILE_SAVE,
                            "MKV (*.mkv);;MP4 (*.mp4)", NULL);

    obs_property_t* fmt = obs_properties_add_list(p, "format", T("Props.Container"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(fmt, T("Props.Container.Mkv"), "mkv");
    obs_property_list_add_string(fmt, T("Props.Container.Mp4"), "mp4");

    obs_property_t* vc = obs_properties_add_list(p, "vcodec", T("Props.Encoder"),
                                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(vc, T("Props.Encoder.Auto"), "auto");
    obs_property_list_add_string(vc, T("Props.Encoder.X264"), "libx264");
    obs_property_list_add_string(vc, T("Props.Encoder.Nvenc"), "h264_nvenc");
    obs_property_list_add_string(vc, T("Props.Encoder.Qsv"), "h264_qsv");

    obs_property_t* src = obs_properties_add_list(p, "source", T("Props.Source"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(src, T("Props.Source.VideoAudio"), "va");
    obs_property_list_add_string(src, T("Props.Source.VideoOnly"), "v");
    obs_property_list_add_string(src, T("Props.Source.AudioOnly"), "a");

    // Capture channel: which CefMedia Relay tab's stream is recorded (the tab whose
    // favicon badge shows this number). 1-based here, 0-based internally.
    obs_property_t* ch = obs_properties_add_list(p, "channel", T("Props.Channel"),
                                                 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int i = 1; i <= CEFMEDIA_CHANNEL_COUNT; ++i)
    {
        char label[16];
        snprintf(label, sizeof(label), "%d", i);
        obs_property_list_add_int(ch, label, i);
    }
    obs_properties_add_bool(p, "autostop", T("Props.Autostop"));

    obs_properties_add_int(p, "vbitrate", T("Props.VideoBitrate"), 500, 60000, 500);
    obs_properties_add_int(p, "abitrate", T("Props.AudioBitrate"), 32, 512, 32);
    return p;
}

static void get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "format", "mkv");
    obs_data_set_default_string(settings, "vcodec", "auto");
    obs_data_set_default_string(settings, "source", "va");
    obs_data_set_default_int(settings, "vbitrate", 8000);
    obs_data_set_default_int(settings, "abitrate", 160);
    obs_data_set_default_int(settings, "channel", 1); // UI value (1-based)
    obs_data_set_default_bool(settings, "autostop", false);
}

static void update(void* data, obs_data_t* settings)
{
    auto* d = static_cast<CefMediaOutputData*>(data);
    d->path = obs_data_get_string(settings, "path");
    d->format = obs_data_get_string(settings, "format");
    d->vcodec = obs_data_get_string(settings, "vcodec");
    d->vbitrate_kbps = (int)obs_data_get_int(settings, "vbitrate");
    d->abitrate_kbps = (int)obs_data_get_int(settings, "abitrate");
    d->source = obs_data_get_string(settings, "source");
    const int ui_channel = (int)obs_data_get_int(settings, "channel");
    // 1-based UI value -> 0-based internal; out-of-range keeps the default.
    if (ui_channel >= 1 && ui_channel <= CEFMEDIA_CHANNEL_COUNT)
    {
        d->channel = ui_channel - 1;
    }
    d->autostop = obs_data_get_bool(settings, "autostop");
    // Push to the recorder immediately (update can arrive while the output is
    // active — properties Apply mid-recording): the End frame is a one-shot, so a
    // deferred snapshot at the next start would miss it. start() sets it again
    // from the same settings; the store is idempotent (atomic member).
    d->recorder.SetAutoStop(d->autostop);
}

static uint64_t get_total_bytes(void* data)
{
    return (uint64_t)static_cast<CefMediaOutputData*>(data)->recorder.GetFileSize();
}

// Frames come from the external shared memory (reader fan-out); obs_output_begin_data_capture
// is never called, so raw_video/raw_audio are never invoked by OBS — empty implementations
// only satisfy the registration check (with OBS_OUTPUT_VIDEO/AUDIO flags these callbacks
// must exist, or obs_register_output rejects).
static void raw_video(void*, struct video_data*)
{
}

static void raw_audio(void*, struct audio_data*)
{
}

// ---- Frontend controller implementation ----

static std::string MakeTimestampedPath(const std::string& format)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::string dir = EffectiveRecordFolder();
    MakeDirRecursive(dir); // ignore if it already exists
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s\\cefmedia_%04d%02d%02d_%02d%02d%02d.%s",
             dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             format.c_str());
    return buf; // UTF-8 end to end (folder from EnvUtf8/cfg; FFmpeg avio_open takes UTF-8)
}

// Setting persistence (obs_frontend_save_callback: saving=true store / false load).
static void FrontendSaveCallback(void* save_data, bool saving, void*)
{
    obs_data_t* d = static_cast<obs_data_t*>(save_data);
    if (!d)
    {
        return;
    }
    if (saving)
    {
        obs_data_set_string(d, "CefMediaOutputFormat", g_prefs.format.c_str());
        obs_data_set_string(d, "CefMediaOutputVcodec", g_prefs.vcodec.c_str());
        obs_data_set_int(d, "CefMediaOutputVbitrate", g_prefs.vbitrate_kbps);
        obs_data_set_int(d, "CefMediaOutputAbitrate", g_prefs.abitrate_kbps);
        obs_data_set_int(d, "CefMediaOutputSpeed", g_prefs.speed_permille);
        obs_data_set_int(d, "CefMediaOutputChannel", g_prefs.channel);
        obs_data_set_string(d, "CefMediaOutputFolder", g_prefs.folder.c_str());
        obs_data_set_string(d, "CefMediaOutputSource", g_prefs.source.c_str());
        obs_data_set_bool(d, "CefMediaOutputAutostop", g_prefs.autostop);
    }
    else
    {
        const char* f = obs_data_get_string(d, "CefMediaOutputFormat");
        if (f && *f)
        {
            g_prefs.format = f;
        }
        const char* v = obs_data_get_string(d, "CefMediaOutputVcodec");
        if (v && *v)
        {
            g_prefs.vcodec = v;
        }
        const int vb = (int)obs_data_get_int(d, "CefMediaOutputVbitrate");
        if (vb > 0)
        {
            g_prefs.vbitrate_kbps = vb;
        }
        const int ab = (int)obs_data_get_int(d, "CefMediaOutputAbitrate");
        if (ab > 0)
        {
            g_prefs.abitrate_kbps = ab;
        }
        const int sp = (int)obs_data_get_int(d, "CefMediaOutputSpeed");
        // permille (1000/1500/2000/3000) since the 1.5x change; a legacy small value
        // (1/2/3 from older saves) is treated as an integer multiplier.
        if (sp == 1000 || sp == 1500 || sp == 2000 || sp == 3000)
        {
            g_prefs.speed_permille = sp;
        }
        else if (sp >= 1 && sp <= 3)
        {
            g_prefs.speed_permille = sp * 1000;
        }
        const int fo_ch = (int)obs_data_get_int(d, "CefMediaOutputChannel");
        if (fo_ch >= 0 && fo_ch < CEFMEDIA_CHANNEL_COUNT)
        {
            g_prefs.channel = fo_ch;
        }
        const char* fo = obs_data_get_string(d, "CefMediaOutputFolder");
        if (fo && *fo)
        {
            g_prefs.folder = fo;
        }
        const char* so = obs_data_get_string(d, "CefMediaOutputSource");
        if (so && *so)
        {
            g_prefs.source = so;
        }
        g_prefs.autostop = obs_data_get_bool(d, "CefMediaOutputAutostop");
    }
}

// Recording finished (normal or error) -> reset the menu state (obs_output_active is
// unreliable for raw outputs, so it cannot be used).
//
// THREADING: this is the "stop" signal handler — OBS emits signals SYNCHRONOUSLY on
// the emitting thread. Normally that is the OBS main thread (manual stop), but the
// init-failure path emits from the recorder worker thread. NEVER join reader or worker
// threads here (ReleaseReader → ReaderStop joins the reader threads; calling it from
// inside a reader callback context = self-join, which is undefined behavior). The
// reader reference is released exclusively on the UI
// thread (StopMenuRecording from the manual stop / dock timer finalizer).
static void OnMenuOutputStop(void*, calldata_t*)
{
    g_menu_recording = false;
    // Restore 1x in CefMedia Relay: speed only applies while recording (the dock's
    // heartbeat stops with the recording; without this CefMedia Relay would stay at the
    // last applied rate). RateSet is a libipc push (non-blocking); safe from any
    // thread; no-ops when the CefMedia Relay is absent. Channel-routed: only the
    // recorded tab resets (other tabs' own rates are untouched). recording=false
    // clears the relay-side recording latch (toolbar lock + recording badge).
    CefMediaApi::Instance().RateSet(1000, g_prefs.channel, /*recording=*/false);
    RecorderDockNotifyStateChanged(); // two-state menu text + dock button sync
}

// Directly finalize the current menu recording (same as info->stop): recorder.StopRecord
// (join worker + flush + trailer) + release the reader reference. obs_output_stop is a
// no-op for raw outputs, so this must be called directly. UI THREAD ONLY (ReaderStop
// joins reader threads).
static void StopMenuRecording()
{
    if (!g_menu_output_data || !g_menu_output)
    {
        return;
    }
    if (g_menu_output_data->output != g_menu_output)
    {
        return; // data mismatch (should not happen); do not stop the wrong one
    }
    g_menu_output_data->recorder.StopRecord();
    ReleaseReader(g_menu_output_data);
}

// UI-thread finalization for the auto-stop path (called from the dock's 1 s timer):
// when an async stop was requested (started_ flag cleared by RequestStopAsync on the
// reader thread) while the menu state still says recording, finish the stop — join the
// (already exited) worker, release the reader reference, reset state, restore 1x.
void RecorderFinalizeIfPendingImpl()
{
    if (!g_menu_recording || !g_menu_output_data)
    {
        return;
    }
    if (g_menu_output_data->recorder.IsStarted())
    {
        return; // still recording (or the async stop has not flipped the flag yet)
    }
    blog(LOG_INFO, "[cefmedia] recorder: auto-stop finalize (UI thread)");
    StopMenuRecording(); // StopRecord (immediate join of the exited worker) + ReleaseReader
    g_menu_recording = false;
    CefMediaApi::Instance().RateSet(1000, g_prefs.channel, false); // restore 1x on the recorded
                                                            // channel (same as any other stop)
    RecorderDockNotifyStateChanged();
}

// ---- Shared control surface (declared in cefmedia_output.h; the Tools menu and the Qt
// dock panel both drive recording through these so there is one state source).
// Outside the anonymous namespace: recorder_dock.cpp calls these via the header. ----

} // namespace

// Auto-stop finalizer: called by the dock's 1 s timer (UI thread) to complete an
// async stop requested from the reader thread (RequestStopAsync). No-op when there is
// nothing pending (still recording / not recording).
void RecorderFinalizeIfPending()
{
    RecorderFinalizeIfPendingImpl();
}

bool RecorderStartNew()
{
    if (g_menu_recording)
    {
        return false;
    }
    // Re-read the config file on every start (no settings UI: edit recorder.cfg to change
    // format/encoder/bitrate).
    LoadRecorderPrefs();

    obs_data_t* s = obs_data_create();
    obs_data_set_string(s, "format", g_prefs.format.c_str());
    obs_data_set_string(s, "vcodec", g_prefs.vcodec.c_str());
    obs_data_set_int(s, "vbitrate", g_prefs.vbitrate_kbps);
    obs_data_set_int(s, "abitrate", g_prefs.abitrate_kbps);
    obs_data_set_string(s, "source", g_prefs.source.c_str());
    obs_data_set_int(s, "channel", g_prefs.channel + 1); // UI value (1-based)
    obs_data_set_bool(s, "autostop", g_prefs.autostop);
    const std::string record_path = MakeTimestampedPath(g_prefs.format);
    obs_data_set_string(s, "path", record_path.c_str());

    if (g_menu_output)
    {
        obs_output_update(g_menu_output, s); // reuse the stopped output with the new path
    }
    else
    {
        g_menu_output = obs_output_create(CEFMEDIA_OUTPUT_ID, "CefMedia Recorder", s, NULL);
        if (!g_menu_output)
        {
            blog(LOG_WARNING, "[cefmedia] menu: obs_output_create failed");
            obs_data_release(s);
            return false;
        }
        // obs_output_create does not call info->update automatically — fill it in with this
        // settings (otherwise path is empty and start() fails with "output file path not
        // set").
        obs_output_update(g_menu_output, s);
        // Connect the stop signal: any recording end (normal/error) resets the state.
        signal_handler_connect(obs_output_get_signal_handler(g_menu_output), "stop",
                               OnMenuOutputStop, NULL);
    }
    obs_data_release(s);

    blog(LOG_INFO, "[cefmedia] menu: starting recording format=%s vcodec=%s channel=%d",
         g_prefs.format.c_str(), g_prefs.vcodec.c_str(), g_prefs.channel + 1);
    if (!obs_output_start(g_menu_output))
    {
        return false;
    }
    g_menu_recording = true;
    // CefMediaRelay auto-launch probe (cefmedia_detect.cpp): when a path is
    // configured/probed, watch for frames for a few seconds and launch it on
    // total silence. The probe only fires from a genuine start (guard vs.
    // re-entrancy inside ArmCefMediaRelayLaunchIfSilent).
    ArmCefMediaRelayLaunchIfSilent();
    RecorderDockNotifyStateChanged(); // two-state menu text + dock button sync
    return true;
}

void RecorderStopCurrent()
{
    if (!g_menu_recording)
    {
        return;
    }
    blog(LOG_INFO, "[cefmedia] menu: stopping recording");
    StopMenuRecording(); // obs_output_stop is a no-op for raw outputs, finalize directly
    g_menu_recording = false;
    // Restore 1x in CefMedia Relay on the recorded channel (speed applies only while
    // recording; see OnMenuOutputStop for the error-stop counterpart).
    CefMediaApi::Instance().RateSet(1000, g_prefs.channel, false);
    RecorderDockNotifyStateChanged(); // two-state menu text + dock button sync
}

bool RecorderIsRecording()
{
    return g_menu_recording;
}

int64_t RecorderFileSize()
{
    if (!g_menu_recording || !g_menu_output_data)
    {
        return 0;
    }
    return g_menu_output_data->recorder.GetFileSize();
}

int RecorderSpeed()
{
    // Permille (1000 = 1x; {1000,1500,2000,3000} valid). The dock feeds this straight
    // into CefMediaApi::RateSet (which also takes permille).
    return g_prefs.speed_permille;
}

int RecorderChannel()
{
    // 0-based capture channel the next recording binds to (UI value = +1).
    return g_prefs.channel;
}

// Write recorder.cfg with the current prefs (used by every RecorderSet* so a dock change
// is persisted immediately; the config file is the no-UI settings entry).

void RecorderSetChannel(int channel)
{
    if (channel < 0 || channel >= CEFMEDIA_CHANNEL_COUNT)
    {
        return; // invalid selection: keep the old value
    }
    if (channel == g_prefs.channel)
    {
        return;
    }
    g_prefs.channel = channel;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder channel set to %d (effective on the next start)",
         channel + 1);
}

void RecorderSetSpeed(int speed_permille)
{
    // Valid set only ({1000,1500,2000,3000}); no CefMedia Relay side effect here — the dock
    // gates the RateSet call on RecorderIsRecording (speed changes while not recording
    // just persist for the next start).
    if (speed_permille != 1000 && speed_permille != 1500 && speed_permille != 2000 &&
        speed_permille != 3000)
    {
        return;
    }
    if (speed_permille == g_prefs.speed_permille)
    {
        return;
    }
    g_prefs.speed_permille = speed_permille;
    SaveRecorderPrefs();
    blog(LOG_INFO,
         "[cefmedia] recorder speed set to %d permille (applied to CefMediaRelay while "
         "recording; stopping restores 1x)",
         speed_permille);
}

bool RecorderAutoStop()
{
    return g_prefs.autostop;
}

void RecorderSetAutoStop(bool enabled)
{
    if (enabled == g_prefs.autostop)
    {
        return;
    }
    g_prefs.autostop = enabled;
    SaveRecorderPrefs();
    // Push to the LIVE recording too: the End frame is a one-shot, so deferring the
    // switch to the next start would miss it — the toggle must be visible to the
    // running recorder's OnVideoFrame (auto_stop_ is atomic). The recorder member
    // exists whenever the output data exists (created before start, freed at destroy).
    if (g_menu_output_data)
    {
        g_menu_output_data->recorder.SetAutoStop(enabled);
    }
    blog(LOG_INFO, "[cefmedia] recorder autostop set to %d%s",
         enabled ? 1 : 0, g_menu_recording ? " (live, recording)" : "");
}

// The configured cefmedia_path (recorder.cfg; re-read on every call — the
// detection module (cefmedia_detect.cpp) reads it for its one-shot check and
// auto-launch probe, so a manual cfg edit between OBS sessions is picked up).
std::string RecorderCefMediaPath()
{
    LoadRecorderPrefs();
    return g_prefs.cefmedia_path;
}

// Set CefMedia Relay executable path used by the recording-start auto-launch probe
// (empty disables auto-launch). Persisted to recorder.cfg immediately. Also the
// persistence entry for cefmedia_detect.cpp's probed-path hit.
void RecorderSetCefMediaPath(const std::string& path)
{
    if (path == g_prefs.cefmedia_path)
    {
        return;
    }
    g_prefs.cefmedia_path = path;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder cefmedia_path set to %s",
         path.empty() ? "(disabled)" : path.c_str());
}

std::string RecorderFormat()
{
    return g_prefs.format;
}

void RecorderSetFormat(const std::string& format)
{
    if (format != "mkv" && format != "mp4")
    {
        return;
    }
    if (format == g_prefs.format)
    {
        return;
    }
    g_prefs.format = format;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder format set to %s (effective on the next start)",
         format.c_str());
}

std::string RecorderSource()
{
    return g_prefs.source;
}

void RecorderSetSource(const std::string& source)
{
    if (source != "va" && source != "v" && source != "a")
    {
        return;
    }
    if (source == g_prefs.source)
    {
        return;
    }
    g_prefs.source = source;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder source set to %s (effective on the next start)",
         source.c_str());
}

std::string RecorderVcodec()
{
    return g_prefs.vcodec;
}

void RecorderSetVcodec(const std::string& vcodec)
{
    // Validate to the known set; anything else (hand-edited cfg included) falls back
    // to "auto" so a typo can never disable recording entirely.
    const std::string& v =
        (vcodec == "libx264" || vcodec == "h264_nvenc" || vcodec == "h264_qsv") ? vcodec
                                                                               : std::string("auto");
    if (v == g_prefs.vcodec)
    {
        return;
    }
    g_prefs.vcodec = v;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder vcodec set to %s (effective on the next start)",
         v.c_str());
}

int RecorderVbitrate()
{
    return g_prefs.vbitrate_kbps;
}

void RecorderSetVbitrate(int vbitrate_kbps)
{
    const int v = std::max(500, std::min(60000, vbitrate_kbps));
    if (v == g_prefs.vbitrate_kbps)
    {
        return;
    }
    g_prefs.vbitrate_kbps = v;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder vbitrate set to %d kbps (effective on the next start)", v);
}

int RecorderAbitrate()
{
    return g_prefs.abitrate_kbps;
}

void RecorderSetAbitrate(int abitrate_kbps)
{
    const int a = std::max(32, std::min(512, abitrate_kbps));
    if (a == g_prefs.abitrate_kbps)
    {
        return;
    }
    g_prefs.abitrate_kbps = a;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder abitrate set to %d kbps (effective on the next start)", a);
}

std::string RecorderFolder()
{
    return EffectiveRecordFolder();
}

bool RecorderSetFolder(const std::string& folder)
{
    // Accept only a creatable absolute path: the directory must exist after a recursive
    // create attempt (drives the dock's folder picker; invalid input keeps the old value).
    if (folder.empty() || folder.size() < 3 || folder[1] != ':')
    {
        blog(LOG_WARNING, "[cefmedia] recorder folder rejected (not an absolute path): %s",
             folder.c_str());
        return false;
    }
    if (!MakeDirRecursive(folder))
    {
        blog(LOG_WARNING, "[cefmedia] recorder folder rejected (cannot create): %s",
             folder.c_str());
        return false;
    }
    if (folder == g_prefs.folder)
    {
        return true;
    }
    g_prefs.folder = folder;
    SaveRecorderPrefs();
    blog(LOG_INFO, "[cefmedia] recorder folder set to %s", folder.c_str());
    return true;
}

int64_t RecorderRecordedDurationUs()
{
    if (!g_menu_recording || !g_menu_output_data)
    {
        return 0;
    }
    return g_menu_output_data->recorder.GetRecordedDurationUs();
}

// Tools menu toggle: stop when recording, otherwise start a new recording (same control
// surface as the dock panel).
static void ToggleRecording(void*)
{
    if (g_menu_recording)
    {
        RecorderStopCurrent();
    }
    else
    {
        RecorderStartNew();
    }
}

// Add the Tools menu item + the dock panel after FINISHED_LOADING (the menu / main window
// may not exist yet at obs_module_load).
static void FrontendEventCallback(int event, void*)
{
    if (event != kFrontendFinishedLoading || g_menu_added)
    {
        return;
    }
    g_menu_added = true;
    FrontendApi& fe = FrontendApi::Instance();
    if (fe.add_tools_menu_item)
    {
        // Two-state label (rewritten by RecorderDockNotifyStateChanged on every state
        // transition); the initial text matches the not-recording state. kMenuIdle is a
        // locale KEY resolved through T() — the dock-side rewrite resolves the same key
        // and caches it (g_menu_text_idle), so registration and rewrite always agree.
        fe.add_tools_menu_item(T(kMenuIdle), &ToggleRecording, NULL);
    }
    CreateRecorderDock();
    // One-shot mutual check (cefmedia_detect.cpp): CefMediaRelay
    // installed but not running -> Yes/No launch prompt (never a silent
    // launch); not installed -> the dock shows a hint line (the module logs
    // its own result). Runs on the UI thread after FINISHED_LOADING (a prefs
    // read + a Toolhelp snapshot: quick, no network; the prompt only appears
    // when the relay is actually not running).
    DetectCefMediaRelayOnce();
}

void RegisterCefMediaOutput()
{
    static struct obs_output_info info = {};
    info.id = CEFMEDIA_OUTPUT_ID;
    info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO;
    info.get_name = get_name;
    info.create = create;
    info.destroy = destroy;
    info.start = start;
    info.stop = stop;
    info.update = update;
    info.get_defaults = get_defaults;
    info.get_properties = get_properties;
    info.get_total_bytes = get_total_bytes;
    // No get_dropped_frames callback: the deep wait-when-full queues never drop
    // frames while a recording runs, so the count would always be zero.
    info.raw_video = raw_video;
    info.raw_audio = raw_audio;
    obs_register_output(&info);

    // Frame-liveness probe registration (cefmedia_detect.cpp): channel 0's video
    // route, before any recorder/source logic in registration order, so the
    // auto-launch probe observes frames before other logic. Just stamps a tick
    // — never blocks.
    RegisterCefMediaRelayFrameProbe();

    // Frontend controller: register the save callback (load prefs) + event callback (add
    // the menu after FINISHED_LOADING). obs-frontend-api is loaded at runtime; when the
    // dll is missing / the environment is not the frontend these pointers are nullptr and
    // are simply skipped.
    FrontendApi& fe = FrontendApi::Instance();
    if (fe.Load())
    {
        if (fe.add_save_callback)
        {
            fe.add_save_callback(&FrontendSaveCallback, NULL);
        }
        if (fe.add_event_callback)
        {
            fe.add_event_callback(&FrontendEventCallback, NULL);
        }
    }
}

// Module unload (called by obs_module_unload): release the menu output + unregister the
// frontend callbacks + remove the dock.
void UnregisterCefMediaOutput()
{
    DestroyRecorderDock();
    if (g_menu_output)
    {
        if (g_menu_recording)
        {
            StopMenuRecording(); // obs_output_active is unreliable for raw outputs, finalize
                                 // directly
            // Module unload mid-recording: restore 1x like any other stop path.
            CefMediaApi::Instance().RateSet(1000, g_prefs.channel, false);
        }
        obs_output_release(g_menu_output);
        g_menu_output = nullptr;
    }
    FrontendApi& fe = FrontendApi::Instance();
    if (fe.IsLoaded())
    {
        if (fe.remove_save_callback)
        {
            fe.remove_save_callback(&FrontendSaveCallback, NULL);
        }
        if (fe.remove_event_callback)
        {
            fe.remove_event_callback(&FrontendEventCallback, NULL);
        }
    }
}

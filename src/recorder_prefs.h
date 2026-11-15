// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_RECORDER_PREFS_H
#define CEFMEDIA_RECORDER_PREFS_H

#include <string>

// ---------------------------------------------------------------------------
// Recorder preferences: the recorder.cfg persistence layer (extracted from
// cefmedia_output.cpp — that file keeps the output callbacks + control surface,
// this file owns the config model and its file IO).
//
// Config entry when there is no UI: %APPDATA%\obs-studio\plugin_config\
// cefmedia-relay-plugins\recorder.cfg (line format key=value). Re-read before
// each start, so a manual edit takes effect on the next click; every setter
// saves immediately (the config file is the no-UI settings entry).
//
// All functions run on the OBS UI thread (the control surface's callers).
// ---------------------------------------------------------------------------

// The preference model with defaults (also the first-run cfg body).
struct MenuPrefs {
    std::string format = "mkv";
    std::string vcodec = "auto";
    int vbitrate_kbps = 8000;
    int abitrate_kbps = 160;
    // Playback speed in permille (1000 = 1x). Valid set {1000, 1500, 2000, 3000}
    // (1x / 1.5x / 2x / 3x). Only applied to CefMedia Relay while recording (the
    // dock gates the RateSet call on RecorderIsRecording); stopping the recording
    // restores 1x in CefMedia Relay while the combo keeps this selection for the
    // next start.
    int speed_permille = 1000;
    std::string folder;        // output folder (empty = %USERPROFILE%\Videos\CefMedia default)
    std::string source = "va"; // which streams to record: "va" (video+audio) / "v" / "a"
    // Capture channel the recorder binds to: 0-based 0..4 (UI shows 1..5; the
    // cfg key stores the 1-based UI value). Default channel 0 (UI 1).
    int channel = 0;
    // Stop the recording automatically when a video frame carries CefFrameFlag_End
    // (the renderer's playback-ended notification fires after the current video's
    // last frame).
    bool autostop = false;
    // CefMedia Relay executable path (empty = never auto-launch). When set, a
    // recording start that sees no frames arriving launches CefMedia Relay with
    // --enable-cefmedia (the Chromium ProcessSingleton folds a redundant launch
    // into the running instance, so this is safe even when CefMedia Relay is
    // already up but merely silent).
    std::string cefmedia_path;
};

// The module-lifetime preference instance everything reads and mutates.
MenuPrefs& RecorderPrefs();

// %APPDATA%\obs-studio\plugin_config\cefmedia-relay-plugins\recorder.cfg
std::string RecorderConfigPath();

// Default output folder: %USERPROFILE%\Videos\CefMedia (USERPROFILE is read as
// UTF-8 — getenv would return the ANSI code page string and mangle a non-ASCII
// user name).
std::string DefaultRecordFolder();

// Effective output folder: the configured value, or the default when empty.
std::string EffectiveRecordFolder();

// Read recorder.cfg into RecorderPrefs() (invalid values fall back to defaults;
// first run writes the defaults to a new file so they are visible and editable).
void LoadRecorderPrefs();

// Persist RecorderPrefs() to recorder.cfg (used by every setter so a change is
// saved immediately).
void SaveRecorderPrefs();

#endif // CEFMEDIA_RECORDER_PREFS_H

#ifndef CEFMEDIA_DETECT_H
#define CEFMEDIA_DETECT_H

#include <cstdint>
#include <string>

// CefMediaRelay install/run detection + launch (split out of
// cefmedia_output.cpp — recording control stays there, everything that only
// answers "is CefMediaRelay installed / running / how to start it" lives
// here).
//
// Surfaces:
//   - DetectCefMediaRelayOnce(): the one-shot mutual check. Runs once at OBS
//     FINISHED_LOADING (from cefmedia_output.cpp's frontend event callback):
//     configured path empty -> probe default install locations + the suite
//     installer's Uninstall registry key, persisting a hit through
//     RecorderSetCefMediaPath (cefmedia_output.h); running -> nothing;
//     installed but not running -> Yes/No prompt, launch with --enable-cefmedia
//     on confirm (the Chromium ProcessSingleton folds a redundant start) or
//     latch kDeclined on dismiss; not installed -> kMissing (the dock shows a
//     hint line, no MessageBox inside OBS).
//   - ArmLaunchIfSilent(): called on every recording start; watches the frame
//     tick for ~4 s and launches CefMediaRelay when nothing flows.
//
// Prefs coupling: the configured cefmedia_path is read through
// RecorderCefMediaPath() (which re-reads recorder.cfg, so a manual edit
// between OBS sessions is picked up) and a probe hit persists through
// RecorderSetCefMediaPath() — no callback indirection.
//
// Naming: "CefMediaRelay" is the product name; the recorder.cfg key stays
// "cefmedia_path" (legacy key "browser_path" still read; documented in the README).

// Result of the one-shot install/run check (the dock's hint row reads it).
enum class CefMediaRelayLaunchState {
    kRunning,    // CefMediaRelay process detected: nothing to do
    kLaunched,   // installed but not running: user confirmed the launch prompt
    kDeclined,   // installed but not running: user dismissed the launch prompt
    kMissing,    // path unset/invalid and no default/registry install found
    kNotChecked, // check never ran
};

// The one-shot check (see above). Returns the resulting state.
CefMediaRelayLaunchState DetectCefMediaRelayOnce();

// The result of the start-time check (the dock reads it for the hint line).
CefMediaRelayLaunchState CefMediaRelayLaunchStateNow();

// The effective path the check resolved (configured, probed, or ""; the
// recording-start probe uses it as its launch target).
std::string CefMediaRelayExePath();

// Register the liveness callback on channel 0 (called once at module register;
// stamps a tick per video frame — the recording-start probe watches it).
void RegisterCefMediaRelayFrameProbe();

// Recording-start auto-launch: when armed and no frame arrives within ~4 s,
// launch CefMediaRelay (path = CefMediaRelayExePath(), empty -> no-op).
void ArmCefMediaRelayLaunchIfSilent();

// Launch CefMediaRelay right now (utf8 path + --enable-cefmedia). Returns
// false when the path does not exist or CreateProcess fails. Also used by the
// recording-start probe thread.
bool LaunchCefMediaRelay(const std::string& exe_path);

// Yes/No confirmation before an automatic CefMediaRelay launch (user informed
// consent — the launch must never be silent). Resolves the localized question
// through T("Launch.Confirm") (en-US canonical text; missing key -> built-in
// English fallback). Returns true when the user chose Yes. Windows only (the
// auto-launch feature itself is Windows only).
bool ConfirmCefMediaRelayLaunch();

#endif // CEFMEDIA_DETECT_H

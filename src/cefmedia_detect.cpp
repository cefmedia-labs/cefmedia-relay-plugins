// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "cefmedia_detect.h"

#include <obs-module.h>
#include <obs.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <windows.h>
#include <tlhelp32.h>

#include "cefmedia.h"       // cefmedia_video_frame + VideoFrameCallback typedef
#include "cefmedia_api.h"   // CefMediaApi (frame callback registration, channel 0)
#include "cefmedia_output.h" // RecorderCefMediaPath / RecorderSetCefMediaPath (prefs)
#include "i18n.h"           // T (Launch.Confirm prompt text)
#include "path_util.h"      // FileExists / Utf8ToWide / WideToUtf8 (UTF-8 paths)

// ---------------------------------------------------------------------------
// CefMediaRelay install/run detection + launch (see cefmedia_detect.h).
// The configured cefmedia_path is read through RecorderCefMediaPath() (which
// re-reads recorder.cfg — the cfg file may have been edited between OBS
// sessions) and a probe hit persists through RecorderSetCefMediaPath(); the
// recorder.cfg format stays owned by cefmedia_output.cpp.
// ---------------------------------------------------------------------------

namespace {

// The bare file name of an exe path ("CefMediaRelay.exe"), lowercased for
// Toolhelp compare; empty for an empty input.
std::string ExeNameLower(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    for (char& c : name)
    {
        c = (char)tolower((unsigned char)c);
    }
    return name;
}

// Whether a process with the given image file name is running (case-insensitive;
// name = "cefmediarelay.exe" style bare file name).
bool IsProcessRunning(const std::string& exe_name_lower)
{
    if (exe_name_lower.empty())
    {
        return false;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            std::wstring wide(pe.szExeFile);
            std::string name;
            name.reserve(wide.size());
            for (wchar_t wc : wide)
            {
                name += (char)tolower((unsigned char)wc);
            }
            if (name == exe_name_lower)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Registry lookup for a non-default install location: the suite installer
// (Inno Setup) writes Uninstall\{AppId}_is1\InstallLocation. Returns the exe
// path, or "" when no key/value yields an existing file. The AppId GUID must
// match installer\cefmedia_suite.iss [Setup] AppId.
std::string ProbeFromRegistry()
{
    struct RegKey { HKEY root; const wchar_t* path; };
    // LM 64-bit view (admin per-machine), LM WOW6432Node (32-bit view fallback),
    // HKCU (a non-admin per-user install lands here).
    const RegKey keys[] = {
        {HKEY_LOCAL_MACHINE,
         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
         L"{7B4E2C60-8D9A-4E11-9F2A-3F5C6D7E8A90}_is1"},
        {HKEY_LOCAL_MACHINE,
         L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
         L"{7B4E2C60-8D9A-4E11-9F2A-3F5C6D7E8A90}_is1"},
        {HKEY_CURRENT_USER,
         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
         L"{7B4E2C60-8D9A-4E11-9F2A-3F5C6D7E8A90}_is1"},
    };
    for (const RegKey& k : keys)
    {
        HKEY key = NULL;
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            continue;
        }
        wchar_t loc[MAX_PATH] = {};
        DWORD size = sizeof(loc);
        DWORD type = 0;
        std::string exe;
        if (RegQueryValueExW(key, L"InstallLocation", NULL, &type, (LPBYTE)loc,
                             &size) == ERROR_SUCCESS &&
            type == REG_SZ && loc[0])
        {
            std::wstring root(loc);
            if (root.back() != L'\\')
            {
                root += L'\\';
            }
            root += L"CefMediaRelay.exe";
            const DWORD attr = GetFileAttributesW(root.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                exe = WideToUtf8(root);
            }
        }
        RegCloseKey(key);
        if (!exe.empty())
        {
            return exe;
        }
    }
    return std::string();
}

// Default install candidates (first existing one wins), then the registry.
// Mirrors the suite installer's default layout
// ({autopf}\CefMedia Relay\CefMediaRelay.exe).
std::string ProbeInstallPath()
{
    const char* candidates[] = {
        "C:\\Program Files\\CefMedia Relay\\CefMediaRelay.exe",
        "C:\\Program Files (x86)\\CefMedia Relay\\CefMediaRelay.exe",
    };
    for (const char* path : candidates)
    {
        if (FileExists(path))
        {
            return path;
        }
    }
    return ProbeFromRegistry();}

// ---- module state ----

// Result of the one-shot check (the dock's hint row reads it).
CefMediaRelayLaunchState g_launch_state = CefMediaRelayLaunchState::kNotChecked;
// The path the check resolved (configured / probed; "" when missing).
std::string g_resolved_path;

// Steady-clock ms of the last video frame seen by the liveness callback
// (0 = none yet); the recording-start probe watches it.
std::atomic<int64_t> g_frame_tick_ms{0};
// Monotonic probe guard: one probe thread at a time.
std::atomic<bool> g_probe_running{false};

// Reader-callback liveness probe (registered on channel 0's video route, so it
// runs before the source/output fan-out callbacks of that channel; body must
// stay trivial). The probe only answers "is ANY video flowing" — the default
// capture channel always carries the primary tab, so a per-channel probe set
// is unnecessary; a CefMediaRelay launched without capture still lands on
// channel 0.
void OnProbeVideoFrame(cefmedia_video_frame* /*frame*/, const uint8_t* /*payload*/,
                       void* /*obj*/, int /*channel*/)
{
    g_frame_tick_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
}

// Probe thread: wait up to kProbeWindowMs for the frame tick to move past
// start_tick; on silence, ask the user before launching CefMediaRelay (the
// prompt blocks this probe thread only — the recording has already started,
// it just won't get frames until a relay is up). Detached, single instance;
// clears the running guard on every exit path.
void ProbeThread(int64_t start_tick, std::string exe_path)
{
    struct GuardReset
    {
        ~GuardReset() { g_probe_running.store(false, std::memory_order_release); }
    } guard;
    // Silence window before the launch prompt. Safe to keep short: on timeout
    // the probe re-checks the process list first (a booting relay is never
    // prompted, a running-but-silent one is left alone), and a redundant launch
    // folds into the running instance anyway. 2 s: just above a slow page's
    // first-frame latency, half the old wait-to-prompt.
    constexpr int kProbeWindowMs = 2000;
    for (int waited = 0; waited < kProbeWindowMs; waited += 250)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (g_frame_tick_ms.load(std::memory_order_relaxed) > start_tick)
        {
            return; // frames flowing, CefMediaRelay is alive — nothing to do
        }
    }
    blog(LOG_INFO,
         "[cefmedia] no frames for %d ms after recording start", kProbeWindowMs);
    // Silence != "not running": a relay freshly launched by the startup prompt
    // (or by the user) can take tens of seconds to deliver its first frame
    // (process init, session restore, tab capture bring-up). Re-check the
    // process before prompting — prompting while it boots would offer to
    // launch a relay that already runs (the duplicate prompt).
    if (IsProcessRunning(ExeNameLower(exe_path)))
    {
        blog(LOG_INFO,
             "[cefmedia] CefMediaRelay is running but has not delivered frames yet "
             "(booting or not capturing) — not prompting");
        return;
    }
    blog(LOG_INFO, "[cefmedia] CefMediaRelay is not running, asking to launch it");
    if (ConfirmCefMediaRelayLaunch())
    {
        LaunchCefMediaRelay(exe_path);
    }
}

} // namespace

bool LaunchCefMediaRelay(const std::string& exe_path)
{
    const std::wstring wide_path = Utf8ToWide(exe_path);
    if (wide_path.empty() || wide_path.size() >= MAX_PATH)
    {
        blog(LOG_WARNING, "[cefmedia] CefMediaRelay launch: invalid path encoding");
        return false;
    }
    DWORD attr = GetFileAttributesW(wide_path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        blog(LOG_WARNING, "[cefmedia] CefMediaRelay launch: path not found: %s",
             exe_path.c_str());
        return false;
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // Command line: quoted exe path + the capture switch (mutable buffer required).
    std::wstring cmdline = std::wstring(L"\"") + wide_path + L"\" --enable-cefmedia";
    if (!CreateProcessW(NULL, cmdline.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        blog(LOG_WARNING, "[cefmedia] CefMediaRelay launch: CreateProcess failed (err=%lu)",
             GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    blog(LOG_INFO, "[cefmedia] CefMediaRelay launch: started %s (pid %lu)",
         exe_path.c_str(), pi.dwProcessId);
    return true;
}

bool ConfirmCefMediaRelayLaunch()
{
#if defined(_WIN32)
    // Localized question via the plugin locale (T/obs_module_text); the key is
    // present in every locale ini (Launch.Confirm). English fallback covers a
    // missing key (obs_module_text renders the key itself in that case).
    const char* msg = T("Launch.Confirm");
    std::wstring text;
    if (msg && std::string("Launch.Confirm") != msg)
    {
        text = Utf8ToWide(msg);
    }
    if (text.empty())
    {
        text = L"CefMedia Relay is not running.\n\n"
               L"Start CefMedia Relay now? (the browser that relays video & audio "
               L"to OBS)";
    }
    const int choice = ::MessageBoxW(NULL, text.c_str(), L"CefMedia Relay",
                                     MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    const bool confirmed = (choice == IDYES);
    blog(LOG_INFO, "[cefmedia] CefMediaRelay launch prompt: %s",
         confirmed ? "confirmed" : (choice == IDNO ? "declined" : "dismissed"));
    return confirmed;
#else
    return true; // non-Windows has no auto-launch feature; unreachable here
#endif
}

CefMediaRelayLaunchState DetectCefMediaRelayOnce()
{
    if (g_launch_state != CefMediaRelayLaunchState::kNotChecked)
    {
        return g_launch_state; // one-shot guard
    }
    // Configured path first (recorder.cfg cefmedia_path; may be user-edited).
    const std::string configured = RecorderCefMediaPath();
    if (!configured.empty())
    {
        g_resolved_path = configured;
    }
    else
    {
        // First-run convenience: probe the default install location / the suite
        // installer's registry key and persist the hit, so the recording-start
        // auto-launch works without manual configuration.
        const std::string probed = ProbeInstallPath();
        if (!probed.empty())
        {
            g_resolved_path = probed;
            RecorderSetCefMediaPath(probed); // persist to recorder.cfg
            blog(LOG_INFO, "[cefmedia] cefmedia_path probed default: %s", probed.c_str());
        }
    }
    if (g_resolved_path.empty())
    {
        g_launch_state = CefMediaRelayLaunchState::kMissing;
        blog(LOG_WARNING,
             "[cefmedia] CefMediaRelay not found (cefmedia_path empty/invalid and no "
             "default install detected) — see the dock panel hint");
        return g_launch_state;
    }
    if (IsProcessRunning(ExeNameLower(g_resolved_path)))
    {
        g_launch_state = CefMediaRelayLaunchState::kRunning;
        return g_launch_state;
    }
    // Installed but not running: ask first (the user must stay in control of
    // what gets started from OBS). Confirm -> launch (the Chromium
    // ProcessSingleton folds a redundant launch); dismiss/No -> latch
    // kDeclined (nothing started, the dock hint row stays hidden — the relay
    // exists, the user just chose not to start it now; the recording-start
    // probe asks again when the relay is genuinely not running — it re-checks
    // the process list first, so a still-booting relay never re-prompts).
    if (ConfirmCefMediaRelayLaunch())
    {
        if (LaunchCefMediaRelay(g_resolved_path))
        {
            g_launch_state = CefMediaRelayLaunchState::kLaunched;
        }
        else
        {
            g_launch_state = CefMediaRelayLaunchState::kMissing; // path gone since probe
        }
    }
    else
    {
        g_launch_state = CefMediaRelayLaunchState::kDeclined;
    }
    return g_launch_state;
}

CefMediaRelayLaunchState CefMediaRelayLaunchStateNow()
{
    return g_launch_state;
}

std::string CefMediaRelayExePath()
{
    return g_resolved_path;
}

void RegisterCefMediaRelayFrameProbe()
{
    // Channel 0: CefMediaRelay always assigns the first capturing tab there.
    // Registering on one channel only keeps the probe cheap and still answers
    // "is any capture flowing" for the launch decision.
    CefMediaApi::Instance().SetVideoFrameCallback(&OnProbeVideoFrame, NULL, 0);
}

void ArmCefMediaRelayLaunchIfSilent()
{
    if (g_resolved_path.empty() || g_probe_running.exchange(true))
    {
        return;
    }
    const int64_t start_tick = g_frame_tick_ms.load(std::memory_order_relaxed);
    std::thread(ProbeThread, start_tick, g_resolved_path)
        .detach(); // single-instance guarded by g_probe_running; exits by itself
}

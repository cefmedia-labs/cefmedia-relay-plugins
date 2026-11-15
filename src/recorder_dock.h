// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef RECORDER_DOCK_H
#define RECORDER_DOCK_H

// Two-state Tools menu text: locale ini KEYS (resolved through T()/obs_module_text at
// registration; the TRANSLATED strings are cached in recorder_dock.cpp and the state
// rewrite matches against those — see RecorderDockNotifyStateChanged).
constexpr const char* kMenuIdle = "Menu.Start";
constexpr const char* kMenuRecording = "Menu.Stop";

// Qt dock recorder panel (Start/Stop toggle + Format/Speed/Folder selectors + Output
// display: recorded content time / file size).
//
// Qt is deliberately quarantined into two designated translation units:
// recorder_dock.cpp and channel_preview.{h,cpp} (the live Channel preview popup).
// No Qt headers leak into other translation units, and the panel classes have no
// Q_OBJECT (signals connected via lambdas / std::function hooks) so no moc step is
// needed. The panel is registered with OBS through the runtime-resolved
// obs_frontend_add_dock_by_id; OBS owns the dock and the widget afterwards (its main
// window holds them in a shared_ptr list and frees them on shutdown).
//
// Called on the OBS UI thread from the FINISHED_LOADING frontend event (cefmedia_output.cpp).
void CreateRecorderDock();

// Remove the dock (module unload; no-op when it was never created).
void DestroyRecorderDock();

// Recording state changed (UI thread): update the Tools menu entry to its two-state
// text ("CefMedia Start Recording" / "CefMedia Stop Recording"). Implemented on the
// dock side because it needs Qt (finds the QAction in the main window's Tools menu);
// safe to call even when the dock was never created (menu may still exist).
void RecorderDockNotifyStateChanged();

#endif // RECORDER_DOCK_H

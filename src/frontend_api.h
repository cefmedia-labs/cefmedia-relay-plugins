// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef FRONTEND_API_H
#define FRONTEND_API_H

#include <windows.h>

#include <cstddef>

#include <obs.h>
#include <util/config-file.h> // config_t (user config: DockState re-apply)

// Runtime loader for obs-frontend-api.dll (same pattern as cefmedia_api).
//
// These functions belong to the OBS UI layer (frontend) and only work while the plugin
// runs as an OBS frontend plugin; they are resolved via LoadLibrary + GetProcAddress
// instead of static linking: when the dll is missing / the environment is not the
// frontend, resolution fails and the caller skips — the plugin does not refuse to load.
//
// obs_frontend_get_scenes fills a list whose elements are already addref'd; the caller
// must obs_source_release each element and free(array). The struct mirrors
// frontend-api.h's DARRAY (union{darray / array, num, cap}) so no frontend headers are
// needed.
struct ObsFrontendSourceList {
    obs_source_t** array;
    size_t num;
    size_t cap;
};

class FrontendApi {
public:
    static FrontendApi& Instance();

    // Idempotent: returns true when loaded; false on failure.
    bool Load();

    bool IsLoaded() const { return module_ != nullptr; }

    // Resolved function pointers (nullptr when not loaded / not resolved).
    void (*get_scenes)(ObsFrontendSourceList* list);

    // ---- Tools menu / event / save callbacks (menu start-stop of the recorder) ----
    // obs_frontend_add_tools_menu_item(name, cb, data)
    void (*add_tools_menu_item)(const char* name, void (*cb)(void*), void* data);
    // obs_frontend_add/remove_event_callback (event as int, ABI matches the enum)
    void (*add_event_callback)(void (*cb)(int event, void*), void* data);
    void (*remove_event_callback)(void (*cb)(int event, void*), void* data);
    // obs_frontend_add/remove_save_callback (save_data is actually obs_data_t*)
    void (*add_save_callback)(void (*cb)(void* save_data, bool saving, void*), void* data);
    void (*remove_save_callback)(void (*cb)(void* save_data, bool saving, void*), void* data);

    // ---- Docks (recorder panel; widget is a QWidget*, OBS takes ownership) ----
    // obs_frontend_add_dock_by_id(id, title, widget) — wraps the widget in an OBSDock,
    // stores it in the main window's extraDocks (shared_ptr) and adds a Docks-menu entry.
    // Returns false when the id is already used (duplicate registration).
    // obs_frontend_remove_dock(id) — OBS removes (and frees) the dock + widget.
    bool (*add_dock_by_id)(const char* id, const char* title, void* widget);
    void (*remove_dock)(const char* id);
    // Main window QWidget* + user config (global.ini): used to re-apply the saved
    // DockState after a plugin dock registers (OBS restores dock layout before plugins
    // load, so plugin docks miss their saved geometry/visibility otherwise).
    void* (*get_main_window)();
    config_t* (*get_user_config)();

private:
    FrontendApi() = default;
    ~FrontendApi();
    FrontendApi(const FrontendApi&) = delete;
    FrontendApi& operator=(const FrontendApi&) = delete;

    bool ResolveFunctions();

    HMODULE module_ = nullptr;
};

#endif // FRONTEND_API_H

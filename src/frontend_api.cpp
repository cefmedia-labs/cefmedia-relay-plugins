#include "frontend_api.h"

#include <string>

namespace {

std::wstring ModuleDir(LPCWSTR module_name)
{
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) {
        return std::wstring();
    }
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (len == 0) {
        return std::wstring();
    }
    std::wstring path(buf, len);
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* file)
{
    return dir + L"\\" + file;
}

} // namespace

FrontendApi& FrontendApi::Instance()
{
    static FrontendApi instance;
    return instance;
}

FrontendApi::~FrontendApi()
{
    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool FrontendApi::Load()
{
    if (module_) {
        return true;
    }

    // Candidate paths: plugin dir -> obs bin dir (where obs-frontend-api.dll lives) ->
    // system search.
    std::wstring plugin_dir = ModuleDir(L"cefmedia-relay-plugins.dll");
    std::wstring obs_dir = ModuleDir(L"obs.dll");

    std::wstring candidates[3];
    size_t count = 0;
    if (!plugin_dir.empty()) {
        candidates[count++] = JoinPath(plugin_dir, L"obs-frontend-api.dll");
    }
    if (!obs_dir.empty() && obs_dir != plugin_dir) {
        candidates[count++] = JoinPath(obs_dir, L"obs-frontend-api.dll");
    }
    // Last is always the bare name: in the OBS frontend process this dll is already
    // loaded, so LoadLibrary on the bare name hits it.
    candidates[count++] = L"obs-frontend-api.dll";

    for (size_t i = 0; i < count; ++i) {
        HMODULE mod = LoadLibraryW(candidates[i].c_str());
        if (!mod) {
            continue;
        }
        module_ = mod;
        if (ResolveFunctions()) {
            return true;
        }
        FreeLibrary(mod);
        module_ = nullptr;
    }
    return false;
}

bool FrontendApi::ResolveFunctions()
{
    get_scenes = reinterpret_cast<void (*)(ObsFrontendSourceList*)>(
        GetProcAddress(module_, "obs_frontend_get_scenes"));

    add_tools_menu_item = reinterpret_cast<void (*)(const char*, void (*)(void*), void*)>(
        GetProcAddress(module_, "obs_frontend_add_tools_menu_item"));
    add_event_callback = reinterpret_cast<void (*)(void (*)(int, void*), void*)>(
        GetProcAddress(module_, "obs_frontend_add_event_callback"));
    remove_event_callback = reinterpret_cast<void (*)(void (*)(int, void*), void*)>(
        GetProcAddress(module_, "obs_frontend_remove_event_callback"));
    add_save_callback = reinterpret_cast<void (*)(void (*)(void*, bool, void*), void*)>(
        GetProcAddress(module_, "obs_frontend_add_save_callback"));
    remove_save_callback = reinterpret_cast<void (*)(void (*)(void*, bool, void*), void*)>(
        GetProcAddress(module_, "obs_frontend_remove_save_callback"));

    add_dock_by_id = reinterpret_cast<bool (*)(const char*, const char*, void*)>(
        GetProcAddress(module_, "obs_frontend_add_dock_by_id"));
    remove_dock =
        reinterpret_cast<void (*)(const char*)>(GetProcAddress(module_, "obs_frontend_remove_dock"));
    get_main_window =
        reinterpret_cast<void* (*)()>(GetProcAddress(module_, "obs_frontend_get_main_window"));
    get_user_config =
        reinterpret_cast<config_t* (*)()>(GetProcAddress(module_, "obs_frontend_get_user_config"));

    return get_scenes != nullptr;
}

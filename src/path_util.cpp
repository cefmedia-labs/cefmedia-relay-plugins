// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "path_util.h"

#include <windows.h>

#include <cstdio>

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                      static_cast<int>(utf8.size()), NULL, 0);
    if (n <= 0)
    {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &wide[0], n);
    return wide;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                      static_cast<int>(wide.size()), NULL, 0,
                                      NULL, NULL);
    if (n <= 0)
    {
        return std::string();
    }
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &utf8[0], n, NULL, NULL);
    return utf8;
}

std::string EnvUtf8(const char* name)
{
    if (!name || !*name)
    {
        return std::string();
    }
    wchar_t wide_name[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 128) == 0)
    {
        return std::string();
    }
    wchar_t value[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(wide_name, value, MAX_PATH);
    if (n <= 0 || n >= MAX_PATH)
    {
        return std::string(); // unset, empty, or too long — treat as absent
    }
    return WideToUtf8(value);
}

bool MakeDirRecursive(const std::string& utf8_path)
{
    if (utf8_path.empty())
    {
        return false;
    }
    const std::wstring wide = Utf8ToWide(utf8_path);
    std::wstring cur;
    size_t i = 0;
    while (i < wide.size())
    {
        // Skip separators (and the "C:" drive-prefix colon position, which is not a
        // separable component).
        const size_t next = wide.find_first_of(L"\\/", i);
        const size_t end = (next == std::wstring::npos) ? wide.size() : next;
        if (end > i)
        {
            if (cur.empty())
            {
                cur = wide.substr(0, end);
            }
            else
            {
                cur += L"\\" + wide.substr(i, end - i);
            }
            if (cur.size() > 2 && cur[1] == L':') // skip bare "C:" — not a creatable dir
            {
                CreateDirectoryW(cur.c_str(), NULL); // ignore "already exists"
            }
        }
        if (next == std::wstring::npos)
        {
            break;
        }
        i = next + 1;
    }
    const DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::string& utf8_path)
{
    if (utf8_path.empty())
    {
        return false;
    }
    const DWORD attr = GetFileAttributesW(Utf8ToWide(utf8_path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

FILE* OpenFileUtf8(const std::string& utf8_path, const char* mode)
{
    if (utf8_path.empty() || !mode)
    {
        return NULL;
    }
    wchar_t wide_mode[8];
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8) == 0)
    {
        return NULL;
    }
    return _wfsopen(Utf8ToWide(utf8_path).c_str(), wide_mode, _SH_DENYNO);
}

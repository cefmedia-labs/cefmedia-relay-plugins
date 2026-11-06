#ifndef PATH_UTIL_H
#define PATH_UTIL_H

#include <string>

// Windows-side path helpers: everything the plugin stores (recorder.cfg values,
// OBS settings strings, log text) is UTF-8, but the Win32 "ANSI" file APIs
// (getenv / fopen / CreateDirectoryA) interpret paths in the system code page —
// a non-ASCII user name or folder (e.g. a Chinese %USERPROFILE%) mangles under
// that conversion. These helpers keep every file path in UTF-8 and do the
// wide conversion exactly at the Win32 boundary.

// UTF-8 -> UTF-16 (CP_UTF8). Empty in -> empty out; invalid sequences -> U+FFFD
// (per MultiByteToWideChar default handling).
std::wstring Utf8ToWide(const std::string& utf8);

// UTF-16 -> UTF-8 (CP_UTF8). Empty in -> empty out.
std::string WideToUtf8(const std::wstring& wide);

// Read an environment variable as UTF-8 (GetEnvironmentVariableW under the
// hood — getenv() would hand back the ANSI code page string). Returns ""
// when the variable is unset or empty.
std::string EnvUtf8(const char* name);

// Create every missing level of a directory path (CreateDirectoryW is
// single-level). Returns false only when the leaf still does not exist
// afterwards. Accepts an empty path (false) and drive-root paths (true).
bool MakeDirRecursive(const std::string& utf8_path);

// Whether |utf8_path| resolves to an existing file (not a directory).
bool FileExists(const std::string& utf8_path);

// fopen equivalent that takes a UTF-8 path (_wfsopen under the hood).
FILE* OpenFileUtf8(const std::string& utf8_path, const char* mode);

#endif // PATH_UTIL_H

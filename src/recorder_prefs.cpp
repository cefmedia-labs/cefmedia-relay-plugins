// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#include "recorder_prefs.h"

#include <obs-module.h>

#include <cstdio>
#include <cstring>

#include "cefmedia.h"   // CEFMEDIA_CHANNEL_COUNT
#include "path_util.h"  // EnvUtf8 / MakeDirRecursive / OpenFileUtf8 — UTF-8 paths,
                        // wide Win32 calls underneath (the ANSI code page would
                        // mangle non-ASCII user names)

namespace {

// Trim spaces/tabs from both ends of a cfg value (editor artifacts / trailing
// whitespace). Path values keep their INNER spaces — only the ends are trimmed.
std::string TrimCopy(const std::string& v)
{
    const size_t b = v.find_first_not_of(" \t");
    if (b == std::string::npos)
    {
        return std::string();
    }
    const size_t e = v.find_last_not_of(" \t");
    return v.substr(b, e - b + 1);
}

// The recorder.cfg body — the SINGLE writer for both the first-run defaults and
// every save (the key list must never fork into hand-kept copies).
void WritePrefsBody(FILE* f, const MenuPrefs& p)
{
    fprintf(f, "# CefMedia Recorder config (re-read on the next menu click)\n");
    fprintf(f, "format=%s\n", p.format.c_str());
    fprintf(f, "vcodec=%s\n", p.vcodec.c_str());
    fprintf(f, "vbitrate=%d\n", p.vbitrate_kbps);
    fprintf(f, "abitrate=%d\n", p.abitrate_kbps);
    fprintf(f, "speed_permille=%d\n", p.speed_permille);
    fprintf(f, "channel=%d\n", p.channel + 1); // UI value (1-based)
    fprintf(f, "folder=%s\n", p.folder.empty() ? DefaultRecordFolder().c_str()
                                               : p.folder.c_str());
    fprintf(f, "source=%s\n", p.source.c_str());
    fprintf(f, "autostop=%d\n", p.autostop ? 1 : 0);
    fprintf(f, "cefmedia_path=%s\n", p.cefmedia_path.c_str());
}

// One cfg key -> how it lands in MenuPrefs. Table-driven (was an if-else
// chain): adding a key is one row. All handlers are captureless lambdas
// (convert to plain function pointers). File order matters for the speed keys:
// the legacy multiplier is applied when read, the permille key later in the
// file overrides it.
struct PrefsKey {
    const char* key;
    void (*apply)(MenuPrefs&, const char* val);
};

constexpr PrefsKey kPrefsKeys[] = {
    {"format", [](MenuPrefs& p, const char* v) { p.format = v; }},
    {"vcodec", [](MenuPrefs& p, const char* v) { p.vcodec = v; }},
    {"vbitrate", [](MenuPrefs& p, const char* v) { p.vbitrate_kbps = atoi(v); }},
    {"abitrate", [](MenuPrefs& p, const char* v) { p.abitrate_kbps = atoi(v); }},
    {"speed_permille",
     [](MenuPrefs& p, const char* v) { p.speed_permille = atoi(v); }}, // wins over legacy
    {"speed",
     [](MenuPrefs& p, const char* v) {
         // Legacy integer multiplier key (1/2/3); the permille key later in the
         // file overrides, so both spellings coexist.
         const int legacy = atoi(v);
         if (legacy >= 1 && legacy <= 3)
         {
             p.speed_permille = legacy * 1000;
         }
     }},
    {"folder", [](MenuPrefs& p, const char* v) { p.folder = TrimCopy(v); }},
    {"source", [](MenuPrefs& p, const char* v) { p.source = v; }},
    {"channel",
     [](MenuPrefs& p, const char* v) {
         // 1-based UI value (1..5); 0-based internally. Out-of-range falls
         // back to channel 0 via the clamp below.
         const int ui = atoi(v);
         p.channel = (ui >= 1 && ui <= CEFMEDIA_CHANNEL_COUNT) ? ui - 1 : 0;
     }},
    {"autostop", [](MenuPrefs& p, const char* v) { p.autostop = atoi(v) != 0; }},
    {"browser_path",
     [](MenuPrefs& p, const char* v) {
         // Legacy key (pre 1.0.1): same semantics as cefmedia_path; the
         // new key later in the file overrides, so both spellings coexist.
         p.cefmedia_path = TrimCopy(v);
     }},
    {"cefmedia_path", [](MenuPrefs& p, const char* v) { p.cefmedia_path = TrimCopy(v); }},
};

} // namespace

MenuPrefs& RecorderPrefs()
{
    static MenuPrefs prefs;
    return prefs;
}

std::string RecorderConfigPath()
{
    const std::string appdata = EnvUtf8("APPDATA");
    if (appdata.empty())
    {
        return std::string();
    }
    return appdata + "\\obs-studio\\plugin_config\\cefmedia-relay-plugins\\recorder.cfg";
}

std::string DefaultRecordFolder()
{
    const std::string home = EnvUtf8("USERPROFILE");
    return (home.empty() ? std::string("C:") : home) + "\\Videos\\CefMedia";
}

std::string EffectiveRecordFolder()
{
    return RecorderPrefs().folder.empty() ? DefaultRecordFolder()
                                          : RecorderPrefs().folder;
}

void LoadRecorderPrefs()
{
    MenuPrefs& p = RecorderPrefs();
    const std::string path = RecorderConfigPath();
    if (path.empty())
    {
        return;
    }
    const std::string dir = path.substr(0, path.find_last_of('\\'));
    MakeDirRecursive(dir); // ignore if it already exists

    FILE* f = OpenFileUtf8(path, "r");
    if (!f)
    {
        f = OpenFileUtf8(path, "w"); // first run: write defaults, visible and editable
        if (f)
        {
            WritePrefsBody(f, p);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64] = {0};
        char val[512] = {0};
        if (sscanf(line, " %63[^= \t\r\n] = %511[^\r\n]", key, val) != 2)
        {
            continue;
        }
        for (const PrefsKey& k : kPrefsKeys)
        {
            if (strcmp(key, k.key) == 0)
            {
                k.apply(p, val);
                break;
            }
        }
    }
    fclose(f);
    if (p.format != "mkv" && p.format != "mp4")
    {
        p.format = "mkv"; // invalid container, fall back
    }
    if (p.speed_permille != 1000 && p.speed_permille != 1500 && p.speed_permille != 2000 &&
        p.speed_permille != 3000)
    {
        p.speed_permille = 1000; // invalid speed multiplier, fall back
    }
    if (p.source != "va" && p.source != "v" && p.source != "a")
    {
        p.source = "va"; // invalid source selection, fall back
    }
    if (p.channel < 0 || p.channel >= CEFMEDIA_CHANNEL_COUNT)
    {
        p.channel = 0; // invalid channel, fall back (defensive; parse clamps too)
    }
    blog(LOG_INFO,
         "[cefmedia] recorder prefs: format=%s vcodec=%s vbit=%d abit=%d speed=%d permille "
         "source=%s channel=%d autostop=%d",
         p.format.c_str(), p.vcodec.c_str(), p.vbitrate_kbps, p.abitrate_kbps,
         p.speed_permille, p.source.c_str(), p.channel + 1, p.autostop ? 1 : 0);
}

void SaveRecorderPrefs()
{
    const std::string path = RecorderConfigPath();
    if (path.empty())
    {
        return;
    }
    FILE* f = OpenFileUtf8(path, "w");
    if (!f)
    {
        return;
    }
    WritePrefsBody(f, RecorderPrefs());
    fclose(f);
}

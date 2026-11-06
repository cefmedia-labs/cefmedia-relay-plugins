#ifndef CEFMEDIA_I18N_H
#define CEFMEDIA_I18N_H

// Locale helper: obs_module_text with the en-US ini as the canonical fallback set
// (OBS_MODULE_USE_DEFAULT_LOCALE in cefmedia_module.cpp wires the lookup; a missing
// key renders the key itself, so every key must exist in data/.../locale/en-US.ini).

#include <obs-module.h>

inline const char* T(const char* key)
{
    return obs_module_text(key);
}

#endif // CEFMEDIA_I18N_H

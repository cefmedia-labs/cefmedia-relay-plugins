#include <obs-module.h>

#include "cefmedia_register.h"

// Module entry point: registers both plugin types (source + output) through the unified
// registration surface in cefmedia_register.cpp.
bool obs_module_load(void)
{
    return RegisterCefMediaPlugins();
}

void obs_module_unload(void)
{
    UnregisterCefMediaPlugins();
}

OBS_DECLARE_MODULE()
// i18n: locale ini files live in data/cefmedia-relay-plugins/locale/ (the subdir must
// equal the module name). OBS calls obs_module_set_locale with the UI language before
// obs_module_load; lookups go through obs_module_text (missing keys return the key).
OBS_MODULE_USE_DEFAULT_LOCALE("cefmedia-relay-plugins", "en-US")

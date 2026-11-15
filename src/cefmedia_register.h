// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 CefMedia Labs. Part of the cefmedia-relay-plugins
// repository (see the repository LICENSE).
#ifndef CEFMEDIA_REGISTER_H
#define CEFMEDIA_REGISTER_H

// Single registration entry point for this module's plugin types:
//   - source (cefmedia_relay_source, see cefmedia_source.cpp)
//   - output (cefmedia_output, see cefmedia_output.cpp)
//
// obs_module_load / obs_module_unload only call these two functions; each type keeps its
// own registration in its own file.

bool RegisterCefMediaPlugins();
void UnregisterCefMediaPlugins();

#endif // CEFMEDIA_REGISTER_H

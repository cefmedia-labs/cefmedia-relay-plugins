#ifndef CEFMEDIA_SOURCE_H
#define CEFMEDIA_SOURCE_H

// OBS source plugin: presents the CefMedia Relay stream (frames received over the
// libipc broadcast routes) as an async source (`cefmedia_relay_source`) that shows in
// scenes and follows scene recording.
//
// The heavy lifting lives in CefVideoConsumer (cefmedia_consumer.cpp); this file only
// wires the obs_source_info glue and the registration entry point — symmetric with
// cefmedia_output.cpp for the output plugin.

#define CEFMEDIA_SOURCE_ID "cefmedia_relay_source"
#define CEFMEDIA_SOURCE_NAME "CefMedia Relay Source"

// Called from obs_module_load (register the source type).
void RegisterCefMediaSource();

// Called from obs_module_unload (no-op for source types, kept for symmetry).
void UnregisterCefMediaSource();

#endif // CEFMEDIA_SOURCE_H

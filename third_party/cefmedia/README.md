# cefmedia/

**This is NOT a third-party dependency.** It is a synced copy of the CefMedia
frame contract, vendored so that this repository builds standalone (the plugin
embeds its own libipc and never links `cefmedia.dll`).

- Canonical source: `cefmedia_frames.h` and `cefmedia.h` in the companion
  CefMedia relay project (closed source; this copy is synced from there).
- The frame header layout is repeated in a few places and must stay identical.
  If you change any frame-header field (planes / flags / timestamp / message
  structs), update all copies together or the wire format silently misaligns.
- Treat this directory as **read-only within this repo**: edit the canonical
  source in the suite repo first, then copy over.

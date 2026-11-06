# Third-party notices

This plugin builds on the following third-party components. This repository
is licensed under the GNU General Public License version 2 (see the `LICENSE`
file at the repository root); the notices below cover the bundled and
dynamically-linked third-party parts.

## cpp-ipc (libipc)

- Upstream: https://github.com/mutouyun/cpp-ipc
- License: MIT (`third_party\libipc\LICENSE`)
- Vendored as pristine upstream source (`third_party\libipc\include` + `src`),
  built by `libipc_static.vcxproj` and statically linked — see
  `third_party\libipc\VENDORED.md` for the vendored commit and how to rebuild.

## OBS Studio SDK and sample code

- Upstream: https://github.com/obsproject/obs-studio
- License: GPL-2.0 (`third_party\obs-studio-32.2.2\COPYING`); individual
  vendored dependencies keep their own license files in place.
- `third_party\obs-sdk\` carries the obs.h SDK headers and an import library
  generated from the installed obs.dll. At runtime the plugin loads
  `obs-frontend-api.dll` from the running OBS instance.

## FFmpeg

- Upstream: https://ffmpeg.org
- License: LGPL-2.1-or-later (details and relinking notes in
  `third_party\ffmpeg\LICENSE.md`).
- No FFmpeg binary is shipped with this repository. The build links import
  libraries; at runtime the FFmpeg DLLs come from the user's OBS Studio
  installation.

## Qt 6

- Upstream: https://www.qt.io / https://code.qt.io
- License: LGPL-3.0 or commercial.
- No Qt binary is shipped with this repository. The recorder dock panel is
  compiled against Qt 6.7 headers (developer kit, not part of this
  repository); at runtime it links the Qt 6 DLLs already loaded by OBS Studio
  (dynamic linking, LGPL obligations satisfied by OBS's own distribution).
  Qt headers are quarantined in `src\recorder_dock.cpp` (no moc step — the
  panel class has no Q_OBJECT).

## cefmedia frame-contract headers

- `third_party\cefmedia\cefmedia.h` / `cefmedia_frames.h` — the wire-format
  contract shared with the CefMedia browser-side library.
- Copyright (C) 2026 CefMedia Labs. Distributed as part of this repository
  under the repository's GPL-2.0 license.

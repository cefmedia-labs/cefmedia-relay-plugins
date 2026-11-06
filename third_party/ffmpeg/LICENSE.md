# FFmpeg licensing notice

This repository does **not** distribute any FFmpeg binary. The build links
FFmpeg **import libraries** (headers + .lib from the gyan.dev
`ffmpeg-8.1.2-full_build-shared` package, `third_party\ffmpeg\include` and
`third_party\ffmpeg\lib`), and at runtime the plugin resolves the FFmpeg DLLs
(avcodec-62, avformat-62, avutil-60, swscale-9, swresample-6) **shipped by OBS
Studio itself** in its installation directory — the same major version the
plugin was compiled against.

FFmpeg is licensed under the GNU Lesser General Public License (LGPL)
version 2.1 or later. You can obtain the FFmpeg source code at
https://ffmpeg.org/download.html .

Because this plugin dynamically links the FFmpeg libraries that the user's
OBS Studio installation provides (and does not redistribute them), the LGPL
relinking obligations for those libraries are satisfied by OBS Studio's own
FFmpeg distribution. If you re-package this plugin with your own FFmpeg
binaries, the full LGPL terms apply to that distribution and you must provide
the corresponding source (or a written offer) for the FFmpeg build you ship.

The `full_build` package from gyan.dev historically includes GPL-enabled
components; the plugin itself only uses the LGPL API surface (libavcodec /
libavformat / libavutil / libswscale / libswresample) and imposes no
additional terms on it.

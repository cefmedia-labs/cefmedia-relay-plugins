# CefMedia Relay Plugins

Repository: <https://github.com/cefmedia-labs/cefmedia-relay-plugins>

Presents CefMedia Relay's captured audio/video frames (via libipc channels) as
an OBS async source (**source plugin**), and can record them directly to a media file
(**output plugin**). One DLL registers both types, coexisting through the ipc client's
multi-callback fan-out.

**5-channel capture**: CefMedia Relay assigns capture channels 1..5 to tabs
as they enable capture (lowest free first; the tab's favicon badge shows its channel
number). Up to five tabs capture in parallel with full pipeline isolation — each
channel has its own shared memory, ring buffers, libipc routes and receive threads.
The source properties pick which channel a source displays (default 1); the recorder
dock binds the recording to a channel and shows 5 occupancy dots mirroring the
CefMedia Relay's live channel bitmap. All three ends (CEF patch, cefmedia.dll, this plugin)
must ship together — the channel routing lives in names and message fields.

License: **GPL-2.0** (see `LICENSE`). Third-party components (cpp-ipc, OBS SDK,
FFmpeg, Qt 6) are covered by `third_party\THIRD-PARTY-NOTICES.md`; none of their
binaries are redistributed — FFmpeg and Qt resolve to the DLLs OBS Studio itself
ships. This repository builds standalone: it does not require the CefMedia tree
(the libipc static library is vendored under `third_party\libipc\`, see
`VENDORED.md` there for the local patches it carries).

## AI-Assistance Disclosure

Per the OBS Project's resource policy, this discloses how AI tools were used to
develop this plugin: **Claude Code** (Anthropic) was used as an AI coding assistant
for drafting and refactoring code, writing documentation, and debugging assistance.
The architecture and feature design, the review of every change, the testing in
OBS, and all release decisions are human-led. This repository is maintained by
CefMedia Labs.

## Support & Issues

Please report bugs and feature requests on the repository's issue tracker:
<https://github.com/cefmedia-labs/cefmedia-relay-plugins/issues>.
Discussion in the resource's forum thread is also fine — but the tracker is
where issues get fixed.

## Architecture

```
CefMedia Relay app (CefMediaRelay.exe)
  channel owner table (chrome layer, 5 slots): tab enables capture -> lowest free
    channel, favicon badge 1..5; cefmedia_channel_start(ch) builds that channel's
    relay on demand (shutdown frees it)
  renderer media thread -> cefmedia.dll client -> shared memory A.<ch> (per channel,
    double-buffered)
    -> relay-process IpcRelay (one instance per occupied channel)
      -> libipc broadcast routes "cefmedia.video.<ch>" / "cefmedia.audio.<ch>"
         (<ch> 0-based 0..4; mutouyun/cpp-ipc, one message = one frame block
         [96B header][payload])  +  "cefmedia.status" (occupancy bitmap, 5s resend)
        -> OBS process: cefmedia-relay-plugins.dll (own libipc, talks to CefMedia Relay
           process directly — cefmedia.dll is NOT loaded by the plugin)
          |-> source plugin: per-channel callback -> obs_source_output_video / audio
          |      -> OBS async source "CefMedia Relay" (channel picked in properties)
          `-> output plugin: channel-bound callback enqueue -> worker -> FFmpeg
                 (H.264/AAC) -> MKV/MP4 file; rate pushes carry the channel field
```

- **ipc client fan-out, per channel** (`src/cefmedia_ipc_client.cpp`): the source and
  output plugins each register a callback **on a capture channel**; one receive-thread
  pair per channel consumes that channel's libipc routes and broadcasts to its
  callbacks (snapshot + invoke); `ReaderStart/Stop(channel)` are reference-counted per
  channel — source and recorder can be active on any mix of channels at once. The
  status channel ("cefmedia.status") carries CefMedia Relay's 5-bit occupancy bitmap to
  the dock's indicator dots (CefMedia Relay resends every 5 s, so a late-started OBS fills
  in). Speed control goes the other way over "cefmedia.rate" (96B push message with a
  0-based channel field — routed to the tab owning that channel).
- Video: the relay remaps the renderer's Chromium pixel-format tag to the matching FFmpeg
  `AVPixelFormat` (I420->YUV420P, NV12, YUV420P10LE, P010LE) before frames enter its ring
  buffer — a tag remap, the pixel data is untouched (layouts are identical); the source
  plugin's data[i] points directly into the received message block, the output plugin
  copies in by stride.
- Audio: the relay remaps the Chromium sample-format tag to the matching FFmpeg
  `AVSampleFormat` (U8/S16/S32/FLT/S16P/FLTP/S32P/U8P) the same way; split per channel block.
- Timestamps: the source plugin uses `OutputTimeline` (us->ns, absorbs forward jumps /
  re-anchors on backward jumps); the output plugin records CFR on a frame-count timeline:
  deep wait-when-full queues (300 video / 4096 audio frames,
  never drop), video PTS = normalized source PTS with gap fill from the previous frame
  (capped, no fast-forward / no time compression), audio on the sample-count timeline with
  FillSilence bridging (A/V realigned after ad skips / source switches). Seeking and clock
  pathologies use segment accounting: backward jumps
  >500 ms re-anchor (continuous timeline + 5 s stale-audio sweep), forward jumps >1 s are
  ABSORBED (the skipped span does not exist in the file — a container hole would otherwise
  cascade 2 s silence pads onto every following frame), a media-clock crawl (≥5 consecutive
  sub-2 ms source steps) is paced at the CFR interval so real frames keep normal playback
  cadence, and the audio gate is container-domain (`AudioPositionUs + chunk <= video end`,
  epoch-free) — the audio track can never outgrow the video timeline regardless of
  decode/render clock divergence.
- Recorder control surface (`cefmedia_output.h`): `RecorderStartNew/StopCurrent/...` are
  the single state source driven by both the Tools menu toggle and the Qt dock panel;
  the recording binds to the channel selected in the dock (persisted to recorder.cfg,
  effective next start); playback speed (1x/1.5x/2x/3x) persists to recorder.cfg and is
  **mapped to CefMedia Relay per channel** (the panel sends the rate over the libipc
  "cefmedia.rate" channel with the recorded channel's number; the CefMedia Relay process
  re-injects `playbackRate` into the elements of the tab owning that channel — other
  tabs keep their own rate; stopping the recording restores 1x on that channel only).

## Crash isolation

The libipc message model removed the old shared-memory failure mode where an OBS crash
permanently leaked a `sem_empty` semaphore token and wedged the relay pipeline. Behavior
matrix now:

| Failure | CefMedia Relay side | OBS side |
|---|---|---|
| OBS process dies | send sees no receivers (`conn_count` -> 0) / libipc `force_push` kicks the dead receiver's reference bit -> relay switches to drop-oldest; **CefMedia Relay never wedges**, page video keeps playing | — |
| CefMedia Relay dies / restarts | — | recv times out (bounded 100 ms polls); a restarted CefMedia Relay broadcasts on the same channel names and OBS re-attaches automatically |
| Receiver alive but stalled (e.g. OBS source audio buffer saturates and blocks the receive callbacks) | libipc `send()`'s internal wait can be re-armed indefinitely by the stalled receiver's broadcasts — measured: one `send()` stuck 3605 s. Two mitigations: (1) vendored libipc patch — no 64-byte fragment fallback when the large-message chunk pool (32 chunks) exhausts (fragment mode used to collapse throughput to 0.02 fps and never recover); (2) relay consumer watchdog — if the delivery heartbeat stalls > 5 s while the ring is full, the consumer switches to drop-oldest (logged `Relay video: delivery stalled ... watchdog`), so the page keeps playing at the cost of frames the dead session would never have delivered anyway | recording sees the stream stall; the session that was recording during the stall is already unrecoverable (delivery was frozen), so dropping costs nothing that was salvageable |
| Either side hangs (not crashed) | healthy backpressure (delivery ~0.5 s/frame worst case) still means **zero dropped frames** — the watchdog only trips on a stalled delivery heartbeat, never on slow-but-moving delivery | recording stalls but OBS stays responsive (callbacks copy-and-enqueue, encode is on the worker thread) |

Recording-integrity rule (unchanged): while delivery is healthy or merely slow, the
"backpressure over tearing" rule guarantees zero frame loss — verified in a 3x-speed
session where a 7.8 s delivery dip produced a gap-free 401 s file. The watchdog and
the libipc patch only change what happens after delivery has *collapsed* (send stuck /
fragment mode), where nothing was being recorded anyway.

Shared memory A (renderer<->relay) still uses named mutex/semaphores, but both ends are
in the CefMedia Relay process tree — a relay crash takes its renderers with it, so no
cross-process token leak is reachable there.

## Build

### Prerequisites

- **Visual Studio 2022** with the *Desktop development with C++* workload (v143
  toolset) and a Windows 10/11 SDK. Everything below that is already in this
  repository — nothing else needs to be installed.
- **Qt 6 dev kit** (x64, MSVC-compatible build) — needed only for the recorder
  dock panel's headers/import libs; the runtime DLLs come from OBS itself. The
  vcxproj defaults to `D:\Qt\Qt6.7.2_vs2019_x64`; point it at your install with
  `msbuild ... /p:CefMediaQtDir="C:\path\to\Qt"` (any Qt 6.7+ works; Qt 6 minor
  releases are binary-compatible).

### Build

```
msbuild CefMediaPlugins.vcxproj /p:Configuration=Release /p:Platform=x64
```
Output: `x64\Release\cefmedia-relay-plugins.dll` (the vendored libipc builds first,
driven by the project's `BuildLibipc` target). Dependencies:
- `third_party\obs-sdk\` — OBS 32.2.2 SDK headers + `obs.lib`.
- `third_party\cefmedia\cefmedia.h` — cefmedia shared-memory struct definitions.
- **FFmpeg 8.1.2** (`third_party\ffmpeg\` — headers + import libs from gyan.dev's
  `ffmpeg-8.1.2-full_build-shared` package) — the output plugin's recording engine. It
  links the **same FFmpeg OBS bundles** (avcodec-62/avformat-62/avutil-60/swscale-9/
  swresample-6 in OBS's bin dir), so no FFmpeg DLLs ship with the plugin.
- **Qt 6** (dev kit, see Prerequisites above for `CefMediaQtDir`) — the
  recorder dock panel. Qt 6 minor releases are binary-compatible: compiled against 6.7,
  resolved to the Qt 6.11.1 runtime OBS bundles at load time. Qt headers live only in
  `recorder_dock.cpp`; the panel class has no Q_OBJECT (lambda connects), so no moc run
  is needed. `/Zc:__cplusplus` is required by Qt headers.
- **libipc** (mutouyun/cpp-ipc, MIT) is **vendored as complete source** at
  `third_party\libipc\` (`include\` + `src\`, built by `libipc_static.vcxproj`
  via the project's `BuildLibipc` target) and linked by explicit path — see
  VENDORED.md there for the vendored commit. Building this
  plugin needs NO external source tree. The plugin talks to the CefMedia Relay
  process itself — **cefmedia.dll is not loaded anymore**. obs-frontend-api.dll
  stays a runtime LoadLibrary (missing it degrades gracefully).

## Deployment

Per-user (no admin). **All four names must equal the compiled-in module name**
(package root dir, DLL filename, data subdir, manifest `id`/`name`). The module
name comes from the vcxproj `TargetName`; renaming is a whole-set operation
(TargetName, package dir, manifest together; a partial rename made OBS skip the
package entirely, no scan attempt in the log at all; recorder.cfg's plugin_config
dir follows the new module name):
```
C:\ProgramData\obs-studio\plugins\cefmedia-relay-plugins\bin\64bit\cefmedia-relay-plugins.dll
C:\ProgramData\obs-studio\plugins\cefmedia-relay-plugins\data\manifest.json
C:\ProgramData\obs-studio\plugins\cefmedia-relay-plugins\data\locale\en-US.ini
C:\ProgramData\obs-studio\plugins\cefmedia-relay-plugins\data\locale\zh-CN.ini
```
The `data\` layout is FLAT (no module-name subdir): OBS 32.2.2's per-user scan
uses the data pattern `plugins\%module%\data` (frontend/widgets/OBSBasic.cpp,
AddExtraModulePaths) — `manifest.json` and `locale\<lang>.ini` are looked up
directly under `data\`. Nesting them one level deeper makes the locale load fail
(OBS logs "Failed to load 'en-US' text for module") and the UI shows raw keys.
Or copy as admin to `C:\Program Files\obs-studio\obs-plugins\64bit\`.

> **cefmedia.dll is NOT deployed to the plugin dir**:
> the plugin carries its own libipc and talks to the CefMedia Relay process directly;
> cefmedia.dll ships only with CefMedia Relay (CefMedia Relay install dir). A leftover copy in the plugin
> dir is inert (nothing loads it).

> **FFmpeg comes from OBS itself**: the plugin eagerly links FFmpeg 8.1.2
> (avcodec-62/avformat-62/avutil-60/swscale-9/swresample-6, the exact major set OBS 32.2.2
> ships in its bin dir). Those DLLs are loaded at OBS startup / resolved from OBS's bin
> directory, so **no FFmpeg DLLs are deployed with the plugin**. If the plugin refuses to
> load ("failed to load module"), verify OBS's FFmpeg DLLs are present in
> `C:\Program Files\obs-studio\bin\64bit\`.

> **Deployment gotcha**: all four names must agree with the compiled-in module
> name (derived from the build TargetName): package root, DLL filename, data
> subdir, manifest `id`/`name`. Any mismatch (e.g. renaming the package
> dir alone) makes OBS skip the package silently — not even a scan line in the
> log. Rename is a whole-set operation: change TargetName first, all layers follow.

## Usage & Verification

1. Start CefMedia Relay; the first capturing tab
   takes channel 1 automatically (its favicon shows the white badge "1").
2. Restart OBS.
3. **Source plugin**: add source -> "CefMedia Relay" -> properties -> **Capture
   Channel** (1..5, default 1) picks which tab's stream shows; multiple sources can
   show different channels (or the same one) at once.
4. **Output plugin** (records the CefMedia Relay stream independently, scene-agnostic): open the
   **Docks -> CefMedia Relay panel** — a Start/Stop toggle plus a **Settings** button; the
   grid carries the frequently-touched settings: **Channel** (1..5, the tab whose favicon
   badge shows this number gets recorded; disabled while recording — next-start
   semantics), the 5 occupancy dots next to it mirror CefMedia Relay's live channel
   bitmap, **Speed** (1x/1.5x/2x/3x, **applied to the recorded channel's tab only while
   recording** — pushed over the libipc rate channel with the channel field on change
   and re-asserted every 3 s (covers page navigations / new tabs); stopping the
   recording restores 1x on that channel while the combo keeps its selection for the
   next start), and the **Autostop** checkbox (end the recording when CefMedia Relay
   reports playback end). The **Settings** dialog holds the rest: Source (video+audio /
   video only / audio only), Container (mkv/mp4), Video encoder (auto / libx264 /
   NVENC / QSV), Video/Audio bitrate, and the output Folder (editable + browse +
   open-in-Explorer, default `%USERPROFILE%\Videos\CefMedia`). A recording info row,
   **Output**, shows the **actual recorded content time** on the file timeline — stops
   while CefMedia Relay pauses — and the output file size, centered, visible from the
   first recording on (idle shows "00:00:00 / 0.0 MB"). Or use **Tools -> CefMedia
   Relay · Start/Stop Recording** as the menu fallback (both stay in sync; the menu
   path records the recorder.cfg channel).
   Everything the dialog sets (and channel/speed) can alternatively be configured in
   `%APPDATA%\obs-studio\plugin_config\cefmedia-relay-plugins\recorder.cfg`
   (keys: `format` mkv|mp4, `vcodec`, `vbitrate`, `abitrate`, `channel` 1..5
   (0-based internally; out-of-range falls back to 1), `speed_permille`
   1000|1500|2000|3000 (legacy `speed` 1|2|3 still read), `folder` — output
   directory, `cefmedia_path` — CefMedia Relay executable for the recording-start
   auto-launch probe; default `format=mkv`; set `format=mp4` to record MP4);
   container/encoder/bitrate/folder/channel changes are effective on the next start.
   **CefMedia Relay auto-launch**: when `cefmedia_path` is set and a recording start sees
   no frames for ~4 s, the plugin asks Yes/No before starting CefMedia Relay
   (localized prompt, `Launch.Confirm` key; the launch is never
   silent). The one-shot check at OBS startup asks the same way. The Chromium
   ProcessSingleton folds redundant launches, so this is safe even when CefMedia Relay
   is already running.
   **UI languages**: the plugin ships the full OBS locale set (77
   `data/locale/*.ini` files mirroring OBS 32.2.2's language list); the
   dock/menus/properties follow OBS's UI language, and OBS falls back to the
   en-US file (canonical key set) for any missing key.
5. **Coexistence check**: record while the source is showing -> both streams present, no
   mutual stutter.
6. Verify: MKV/MP4 playable, frame rate/duration correct; the recorded file is CFR at
   the SOURCE's measured frame rate (the frame header carries the renderer's bucketed
   fps estimate — 24 fps film records at 24, 60 fps at 60; unknown/unstable falls back
   to the historical 30 fps; the measurement window adds ≤1 s before encoding starts).
   Source gaps become smooth holds via gap fill + silence padding; audio/video track
   lengths align within one AAC frame (~23 ms) — the
   worker gates the audio drain at the video timeline end
   and drops pre-roll audio before the first video frame; killing CefMedia Relay
   mid-recording finalizes the file without crashing and auto-recovers on restart;
   44.1k/48k and stereo/5.1 audio work.

## Audio Drop Policy (relay side)

While OBS is attached (libipc has receivers on the channel), the relay's audio/video
rings NEVER drop (TakeFreeWait blocks until the delivery thread frees a slot — stop is
checked in the wait predicate, so shutdown is safe; backpressure briefly freezes the
renderer pipeline instead of tearing audio). Chunks are dropped only when there is NO
receiver (OBS not running, or just died — libipc reports the disconnect) — drop-oldest
keeps CefMedia Relay fluid. Every audio drop is logged to the CefMedia Relay log
(`%APPDATA%\CefMedia Relay\cefmedia_log.log`) as
`Relay audio: dropped N chunk(s) (no receiver)` — a healthy session has zero such lines.

## Single-Instance Relay (Chromium process singleton)

No arbitration code exists in cefmedia.dll — and none is needed for normal launches.
Chrome native mode is the full Chrome stack with its **ProcessSingleton**: launching
CefMediaRelay.exe again hands the command line to the first process (a new window opens
there) and exits, so there is always exactly one CefMedia Relay process and
`cefmedia_relay_start` runs once per process (verified with a one-shot log probe
in the DLL). The default cache_path is fixed to `%LOCALAPPDATA%\CefMediaRelay`
(`--cache-path` overrides), which pins the ProcessSingleton lock directory and further
narrows the bypass window. Residual risk: launching with a different `--cache-path`
bypasses the singleton — two relays would interleave on the fixed-name shm A / libipc
channels (audio tearing). If tearing ever reappears, check the
launch parameters first.

## Known Limits

- x64 only (OBS 28+ has no 32-bit).
- The output plugin fixes video size/framerate at the first frame; mid-stream resolution
  changes do not rebuild the stream (a source switch is handled by a fresh Start).
- Color defaults to BT.709 limited range; the 10-bit I010 path awaits calibration.
- cefmedia.dll must be built with the plain Release|x64 configuration (logging is
  always on; no separate test configuration exists).
- 5-channel capture needs all three ends from the same batch (CEF patch, cefmedia.dll,
  this plugin): channel routing rides on route-name suffixes and message fields, so an
  old cefmedia.dll broadcasts single-channel names the new plugin never hears (and vice
  versa).
- A channel whose relay was torn down (tab closed while OBS still records it) shows a
  stalled recording; the plugin recovers automatically when that channel is re-acquired
  (same fault-tolerance path as a CefMedia Relay restart).

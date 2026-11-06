# Vendored: libipc (mutouyun/cpp-ipc)

- Upstream: https://github.com/mutouyun/cpp-ipc
- License: MIT (see `LICENSE`)
- Vendored commit: `2e28547cd32b22c2e1f2c85d22d0882810838503` (2026-02-19)

This directory vendors the **pristine upstream source** (`src\` + `include\`)
at the commit above — no local modifications (verifiable with
`diff -r <upstream-checkout>/src src`). It is built by
`libipc_static.vcxproj` (a project in `CefMediaPlugins.sln`) and linked into
`cefmedia-relay-plugins.dll` via ProjectReference — the plugin needs no
prebuilt libipc binary and no external source tree.

Why no patches: the plugin only RECEIVES on this library (frame routes) and
sends a single 96-byte rate message every few seconds — far below the scale
where the large-message chunk-pool exhaustion bug (a relay-side send-path
pathology, fixed in the companion CefMedia relay project) can manifest.

## Rebuilding

Open `CefMediaPlugins.sln` (this directory's `libipc_static.vcxproj` is a
project in it) and build `Release|x64`; the plugin project references the
library, so building the plugin builds libipc first.

## Updating the vendored commit

Replace `src\` and `include\` with the new upstream revision (keep `LICENSE`
in sync), re-run the byte-identical diff above, then rebuild.

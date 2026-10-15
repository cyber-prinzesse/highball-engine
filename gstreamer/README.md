# GStreamer for the arm64 engine

Wine's winegstreamer plays the videos games open through Media Foundation and DirectShow (MP4 and H.264,
WMV, WebM, Ogg). Without it MSFS 2024's start screen stays black. `build-arm64.yml` takes the official
macOS packages from gstreamer.freedesktop.org (pinned in `inputs-arm64.json`) and installs nothing:

- `unpkg.py` unpacks one component of a package expanded with `pkgutil --expand`, all of it or only some
  paths. The payloads are pbzx (xz chunks around a cpio archive). `pkgutil --expand-full` would unpack every
  component, 4.7 GB for the devel package.
- `prune.py` keeps the plugins winegstreamer can use for games and exactly the libraries they and
  winegstreamer link, then thins everything to arm64 and signs it ad hoc: 46 MB with the licence texts,
  against 355 MB for the components it starts from. applemedia is kept (VideoToolbox decoding, vtdec_hw
  outranks avdec_h264) with GStreamer's own MoltenVK through libgstvulkan, a second copy next to Wine's in
  the same process, measured harmless on 2026-10-15.

The framework goes to `frameworks/GStreamer.framework`, as Sikarugir's template framework does for the Intel
engines. `winegstreamer.so` finds it through one rpath, `@loader_path/../../GStreamer.framework/Versions/1.0/lib`,
which is `engine/lib/GStreamer.framework`, the link Highball makes to `frameworks/` (the tarball carries it
too). Highball's `GST_PLUGIN_PATH` is the only variable needed. Checked on an M4 (2026-10-11): MSFS 2024's 4K
start video decodes at about four times real time in an x86-64 program under FEX, with no DYLD variable.

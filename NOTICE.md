# Components and licences

| Component | Source | Licence | Bundled |
|---|---|---|---|
| Wine (CrossOver sources) | media.codeweavers.com/pub/crossover/source | LGPL-2.1-or-later | yes, as the engine |
| Highball patch series | patches/ | LGPL-2.1-or-later | yes |
| MoltenVK | github.com/KhronosGroup/MoltenVK | Apache-2.0 | yes |
| DXVK (macOS build) | github.com/doitsujin/dxvk, Gcenx/DXVK-macOS | zlib | as a renderer overlay |
| DXMT | github.com/3Shain/dxmt | see upstream LICENSE | as a renderer overlay |
| winetricks | github.com/Winetricks/winetricks | LGPL-2.1 | as a tool |
| wine-mono, wine-gecko | dl.winehq.org | MIT / MPL | as Wine expects |
| D3DMetal (Apple Game Porting Toolkit) | Apple | Apple licence | never; gated by the app |
| Wine (WineHQ upstream, arm64 line) | dl.winehq.org/wine/source | LGPL-2.1-or-later | yes, as the arm64 engine |
| FEX (libarm64ecfex.dll and libwow64fex.dll, built from FEX 2610 with fex/patches/, or Hangover's build) | github.com/FEX-Emu/FEX, github.com/AndreRH/hangover | MIT | yes, in the arm64 engine, as xtajit64.dll and xtajit.dll |
| FEX Unix helper, Darwin build | fex/fexunixlib_darwin.cpp (after FEX's Source/Windows/UnixLib) | MIT | yes, in the arm64 engine |
| Host libraries of the arm64 engine (Homebrew builds): MoltenVK, FreeType, libpng, GnuTLS with nettle, GMP, libtasn1, libidn2, libunistring, p11-kit and gettext's libintl, sdl2-compat and SDL3 | formulae.brew.sh (each formula's upstream) | Apache-2.0 (MoltenVK), FTL (FreeType), libpng, LGPL-2.1-or-later (GnuTLS, libtasn1, libintl), LGPL-3.0-or-later or GPL-2.0-or-later (nettle, GMP, libidn2, libunistring), BSD-3-Clause (p11-kit), zlib (SDL) | yes, in the arm64 engine's lib/ |
| GStreamer 1.28.7, the official macOS framework pruned by gstreamer/prune.py: GStreamer with its base, good and bad plugins and gst-libav, FFmpeg (libavcodec, libavformat, libavfilter, libavutil, libswresample, libswscale), GLib, ORC, libffi, PCRE2, proxy-libintl, zlib, bzip2, dav1d, FLAC, mpg123, Ogg, Opus, Theora, Vorbis | gstreamer.freedesktop.org | LGPL-2.1-or-later (GStreamer, GLib, FFmpeg as GStreamer builds it, proxy-libintl, mpg123), BSD-2-Clause or BSD-3-Clause (dav1d, ORC, FLAC, Ogg, Opus, Theora, Vorbis, PCRE2), MIT (libffi), zlib, bzip2; the texts are in the framework's share/licenses | yes, in the arm64 engine's frameworks/ |
| llvm-mingw | github.com/mstorsjo/llvm-mingw | Apache-2.0 with LLVM exception | build tool only |

Copyright and licence texts of each component are kept unchanged inside the built engine.

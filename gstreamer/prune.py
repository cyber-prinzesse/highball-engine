#!/usr/bin/env python3
# gstreamer/prune.py <GStreamer.framework> [--thin]: keeps the plugins Wine's winegstreamer can use for games (Media
# Foundation and DirectShow playback: containers, parsers, decoders, conversion) and exactly the libraries they and
# winegstreamer link (an otool closure over @rpath names), and drops everything else. Optionally thins every Mach-O
# to arm64 and signs it ad hoc. applemedia (VideoToolbox decoding, vtdec_hw outranks avdec_h264) comes with
# GStreamer's own MoltenVK through libgstvulkan, a second copy next to Wine's in the same process; measured
# harmless (Microsoft Flight Simulator 2024's 4K intro and loading videos decoded in hardware on 2026-10-15,
# where FFmpeg had taken two cores for the whole boot). Prints what stays.
import os, subprocess, sys

KEEP_PLUGINS = """
coreelements typefindfunctions app playback videoconvertscale videofilter audioconvert audioresample volume rawparse
pbtypes deinterlace isomp4 matroska avi ogg flv asf mpegtsdemux mpegpsdemux id3demux apetag audioparsers
videoparsersbad vorbis theora opus flac mpg123 dav1d libav wavparse applemedia
""".split()
WINE_LIBS = """libgstreamer-1.0.0.dylib libgstvideo-1.0.0.dylib libgstaudio-1.0.0.dylib libgsttag-1.0.0.dylib
libgstbase-1.0.0.dylib libgobject-2.0.0.dylib libglib-2.0.0.dylib libintl.8.dylib""".split()
KEEP_TOOLS = ["bin/gst-inspect-1.0", "bin/gst-launch-1.0", "libexec/gstreamer-1.0/gst-plugin-scanner"]

fw = os.path.abspath(sys.argv[1]); thin = "--thin" in sys.argv
V = os.path.join(fw, "Versions/1.0"); lib = os.path.join(V, "lib"); pdir = os.path.join(lib, "gstreamer-1.0")

def deps(f):
    out = subprocess.run(["otool", "-L", f], capture_output=True, text=True).stdout.splitlines()[1:]
    return [l.split()[0][len("@rpath/"):] for l in out if l.split()[0].startswith("@rpath/")]

plugins = [os.path.join(pdir, "libgst%s.dylib" % p) for p in KEEP_PLUGINS]
missing = [p for p in plugins if not os.path.exists(p)]
plugins = [p for p in plugins if os.path.exists(p)]
roots = plugins + [os.path.join(lib, n) for n in WINE_LIBS] + [os.path.join(V, t) for t in KEEP_TOOLS]
needed, todo = set(), list(roots)
while todo:
    f = todo.pop()
    for d in deps(f):
        leaf = os.path.basename(d)
        if leaf in needed or not os.path.exists(os.path.join(lib, leaf)): continue
        needed.add(leaf); todo.append(os.path.join(lib, leaf))
needed |= set(WINE_LIBS)
# keep the files the needed names resolve to (version symlinks)
keep_files = set()
for n in needed:
    p = os.path.join(lib, n)
    keep_files.add(p); keep_files.add(os.path.realpath(p))
removed = 0
for n in os.listdir(pdir):
    p = os.path.join(pdir, n)
    if p not in plugins: os.remove(p); removed += 1
for n in os.listdir(lib):
    p = os.path.join(lib, n)
    if os.path.isdir(p) and not os.path.islink(p):
        if n != "gstreamer-1.0": subprocess.run(["rm", "-rf", p]); removed += 1
    elif p not in keep_files and os.path.realpath(p) not in keep_files:
        os.remove(p); removed += 1
for sub in ("bin", "libexec/gstreamer-1.0"):
    d = os.path.join(V, sub)
    for n in os.listdir(d):
        p = os.path.join(d, n)
        if os.path.relpath(p, V) not in KEEP_TOOLS: os.remove(p) if not os.path.isdir(p) else subprocess.run(["rm", "-rf", p]); removed += 1
# the framework's umbrella binary re-exports libraries that are gone now; nothing of Wine's links it
for p in (os.path.join(V, "GStreamer"), os.path.join(fw, "GStreamer")):
    if os.path.lexists(p): os.remove(p)
if thin:
    for root, dirs, files in os.walk(V):
        for n in files:
            p = os.path.join(root, n)
            if os.path.islink(p): continue
            archs = subprocess.run(["lipo", "-archs", p], capture_output=True, text=True).stdout.split()
            if "x86_64" in archs and "arm64" in archs:
                subprocess.run(["lipo", "-thin", "arm64", p, "-output", p], check=True)
                subprocess.run(["codesign", "-f", "-s", "-", p], check=True, capture_output=True)
print("plugins kept:", len(plugins), "missing:", [os.path.basename(m) for m in missing])
print("libraries kept:", len(needed), "files removed:", removed)

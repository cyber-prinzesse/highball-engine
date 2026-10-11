#!/usr/bin/env python3
# gstreamer/unpkg.py <component.pkg folder> <dest> [cpio pattern ...]: unpacks one component of a package expanded
# with `pkgutil --expand` into dest, all of it or only the members matching the patterns (cpio -i syntax, members
# are named ./path). The Payload is pbzx: xz-compressed chunks around a cpio archive. `pkgutil --expand-full` would
# unpack every component, which for GStreamer's devel package is 4.7 GB.
import lzma, os, struct, subprocess, sys

src, dest, patterns = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(dest, exist_ok=True)
with open(os.path.join(src, "Payload"), "rb") as f:
    if f.read(4) != b"pbzx":
        sys.exit("%s: not a pbzx payload" % src)
    f.read(8)
    cpio = subprocess.Popen(["cpio", "-idm", "--quiet"] + patterns, stdin=subprocess.PIPE, cwd=dest)
    while True:
        head = f.read(16)
        if len(head) < 16:
            break
        _, length = struct.unpack(">QQ", head)
        chunk = f.read(length)
        cpio.stdin.write(lzma.decompress(chunk) if chunk[:6] == b"\xfd7zXZ\x00" else chunk)
    cpio.stdin.close()
    if cpio.wait():
        sys.exit("cpio failed for %s" % src)

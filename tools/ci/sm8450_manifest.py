#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Build a standalone repo manifest for the SM8450 kernel-6.6 Kleaf tree.
#
# The cupid-development sm8450-build manifest is written as a local
# manifest on top of a full LineageOS checkout.  Only the kernel platform
# is needed to produce boot/vendor_boot/dtbo/vendor_dlkm, so take the
# LineageOS kernel-6.6 snippet and the cupid projects that live under
# kernel/platform/, and wrap them with the remotes LineageOS defines.
#
# usage: sm8450_manifest.py <kernel-6.6.xml> <sm8450-manifest.xml> <out.xml>

import sys
import xml.etree.ElementTree as ET

KERNEL_PREFIX = "kernel/platform/kernel-6.6/"


def main():
    if len(sys.argv) != 4:
        sys.exit(f"usage: {sys.argv[0]} <kernel-6.6.xml> <sm8450-manifest.xml> <out.xml>")

    snippet = ET.parse(sys.argv[1]).getroot()
    cupid = ET.parse(sys.argv[2]).getroot()

    out = ET.Element("manifest")
    ET.SubElement(out, "remote", name="aosp",
                  fetch="https://android.googlesource.com", review="android-review.googlesource.com")
    ET.SubElement(out, "remote", name="github", fetch="https://github.com")
    ET.SubElement(out, "default", remote="github", revision="lineage-24.0-6.6",
                  **{"sync-j": "8", "sync-c": "true"})

    paths = set()
    for proj in snippet.findall("project"):
        out.append(proj)
        paths.add(proj.get("path"))

    for proj in cupid.findall("project"):
        path = proj.get("path", "")
        if not path.startswith(KERNEL_PREFIX):
            continue
        if path in paths:
            sys.exit(f"duplicate project path: {path}")
        out.append(proj)
        paths.add(path)

    if not any(p.endswith("/sm8450-build") for p in paths):
        sys.exit("sm8450-build project missing from cupid manifest")

    ET.indent(out)
    ET.ElementTree(out).write(sys.argv[3], encoding="UTF-8", xml_declaration=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Build a standalone repo manifest for the kernel-6.6 Kleaf platform
# (build/kernel, prebuilts, bazel deps) from the LineageOS kernel-6.6
# snippet.  The SM8450 sources themselves (kernel, devicetrees, external
# modules, sm8450-build) all live in the DoraCore tree and are mapped
# into the platform by the workflow, so no vendor project is synced.
#
# usage: sm8450_manifest.py <kernel-6.6.xml> <out.xml>

import sys
import xml.etree.ElementTree as ET


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <kernel-6.6.xml> <out.xml>")

    snippet = ET.parse(sys.argv[1]).getroot()

    out = ET.Element("manifest")
    ET.SubElement(out, "remote", name="aosp",
                  fetch="https://android.googlesource.com", review="android-review.googlesource.com")
    ET.SubElement(out, "default", remote="aosp", **{"sync-j": "8", "sync-c": "true"})

    projects = snippet.findall("project")
    if not any(p.get("path", "").endswith("/build/kernel") for p in projects):
        sys.exit("build/kernel project missing from the kernel-6.6 snippet")
    for proj in projects:
        if proj.get("remote", "aosp") != "aosp":
            sys.exit(f"unexpected remote for {proj.get('path')}: {proj.get('remote')}")
        out.append(proj)

    ET.indent(out)
    ET.ElementTree(out).write(sys.argv[2], encoding="UTF-8", xml_declaration=True)


if __name__ == "__main__":
    main()

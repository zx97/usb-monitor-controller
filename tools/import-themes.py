#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Import themes, fonts and icons from a turing-smart-screen-python checkout.
#
# The upstream repository carries ~56 MB of theme files and ~190 MB of fonts for
# screen sizes this controller may never drive. This tool imports only what is
# needed:
#
#   tools/import-themes.py                         # bundled themes at 320x480
#   tools/import-themes.py --all                   # every theme (56 MB)
#   tools/import-themes.py --size 480x800          # one resolution
#   tools/import-themes.py --fonts none            # reuse an external font root
#   tools/import-themes.py --source /path/to/checkout --icons
#
# Fonts: by default only the families referenced by the imported themes are
# copied, and only their .ttf/.otf files plus licence files (the preview images
# are not used by the renderer). Themes keep their own licence and author.

import argparse
import glob
import os
import shutil
import sys

try:
    import yaml
except ImportError:  # pragma: no cover - the tool is optional
    sys.stderr.write("PyYAML is required: pip install pyyaml\n")
    sys.exit(1)

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

FONT_SUFFIXES = (".ttf", ".otf", ".ttc")
LICENCE_NAMES = ("LICENSE", "LICENSE.txt", "OFL.txt", "COPYING", "NOTICE")


def background_size(theme_dir):
    """(width, height) of the theme background, or None."""
    path = os.path.join(theme_dir, "background.png")
    if Image is not None:
        try:
            with Image.open(path) as image:
                return image.size
        except Exception:
            pass
    # Fall back to the PNG IHDR header (bytes 16..24), no dependency needed.
    try:
        with open(path, "rb") as stream:
            header = stream.read(24)
        if header[:8] == b"\x89PNG\r\n\x1a\n":
            return (int.from_bytes(header[16:20], "big"),
                    int.from_bytes(header[20:24], "big"))
    except OSError:
        pass
    return None


def collect_fonts(node, families):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "FONT" and isinstance(value, str) and "/" in value:
                families.add(value.split("/")[0])
            collect_fonts(value, families)
    elif isinstance(node, list):
        for value in node:
            collect_fonts(value, families)


def theme_fonts(theme_dir):
    families = set()
    try:
        with open(os.path.join(theme_dir, "theme.yaml"), encoding="utf-8") as stream:
            collect_fonts(yaml.safe_load(stream), families)
    except Exception:
        pass
    return families


def copy_theme(source, destination):
    os.makedirs(destination, exist_ok=True)
    copied = 0
    for name in sorted(os.listdir(source)):
        origin = os.path.join(source, name)
        if not os.path.isfile(origin):
            continue
        shutil.copy2(origin, os.path.join(destination, name))
        copied += 1
    return copied


def copy_fonts(source_fonts, destination_fonts, families):
    copied = []
    for family in sorted(families):
        origin = os.path.join(source_fonts, family)
        if not os.path.isdir(origin):
            sys.stderr.write("  font family missing upstream: %s\n" % family)
            continue
        target = os.path.join(destination_fonts, family)
        os.makedirs(target, exist_ok=True)
        for name in sorted(os.listdir(origin)):
            origin_file = os.path.join(origin, name)
            if not os.path.isfile(origin_file):
                continue
            lower = name.lower()
            wanted = lower.endswith(FONT_SUFFIXES) or any(
                lower.startswith(licence.lower()) for licence in LICENCE_NAMES)
            if not wanted:
                continue
            shutil.copy2(origin_file, os.path.join(target, name))
            copied.append(os.path.join(family, name))
    return copied


def total_size(path):
    return sum(os.path.getsize(f) for f in glob.glob(os.path.join(path, "**", "*"), recursive=True)
               if os.path.isfile(f))


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default=os.path.join(repo, "..", "turing-smart-screen-python"),
                        help="turing-smart-screen-python checkout")
    parser.add_argument("--dest", default=os.path.join(repo, "res"),
                        help="destination res/ directory")
    parser.add_argument("--size", action="append", default=[],
                        help="only themes whose background is WxH (repeatable)")
    parser.add_argument("--all", action="store_true", help="import every theme")
    parser.add_argument("--fonts", default="auto", choices=("auto", "all", "none"),
                        help="font families to import (default: those the themes use)")
    parser.add_argument("--icons", action="store_true", help="also copy res/icons")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    source_themes = os.path.join(args.source, "res", "themes")
    if not os.path.isdir(source_themes):
        sys.stderr.write("no themes in %s (use --source)\n" % source_themes)
        return 1

    sizes = {tuple(int(part) for part in value.lower().split("x")) for value in args.size}
    if not sizes and not args.all:
        sizes = {(320, 480)}  # the only screen this project validates

    selected = []
    skipped = 0
    for directory in sorted(glob.glob(os.path.join(source_themes, "*"))):
        if not os.path.isfile(os.path.join(directory, "theme.yaml")):
            continue
        size = background_size(directory)
        if args.all or size in sizes:
            selected.append(directory)
        else:
            skipped += 1

    if not selected:
        sys.stderr.write("no theme matched %s\n" % (",".join("%dx%d" % s for s in sizes) or "all"))
        return 1

    families = set()
    for directory in selected:
        families |= theme_fonts(directory)

    label = "all sizes" if args.all else ",".join("%dx%d" % s for s in sorted(sizes))
    print("themes  : %d selected (%s), %d skipped" % (len(selected), label, skipped))
    print("fonts   : %s" % (", ".join(sorted(families)) or "(none)"))

    if args.dry_run:
        for directory in selected:
            print("  %s" % os.path.basename(directory))
        return 0

    for directory in selected:
        name = os.path.basename(directory)
        if name == "demo-320x480":
            continue  # bundled theme, never overwrite it
        copy_theme(directory, os.path.join(args.dest, "themes", name))

    copied_fonts = []
    if args.fonts != "none":
        source_fonts = os.path.join(args.source, "res", "fonts")
        wanted = families if args.fonts == "auto" else set(
            name for name in os.listdir(source_fonts)
            if os.path.isdir(os.path.join(source_fonts, name)))
        copied_fonts = copy_fonts(source_fonts, os.path.join(args.dest, "fonts"), wanted)

    icons_copied = 0
    if args.icons:
        source_icons = os.path.join(args.source, "res", "icons")
        if os.path.isdir(source_icons):
            shutil.copytree(source_icons, os.path.join(args.dest, "icons"), dirs_exist_ok=True)
            icons_copied = len(glob.glob(os.path.join(source_icons, "**", "*"), recursive=True))

    print("imported: %d theme(s), %d font file(s), %d icon file(s)"
          % (len(selected), len(copied_fonts), icons_copied))
    print("size of %s: %.1f MB" % (os.path.join(args.dest, "themes"),
                                   total_size(os.path.join(args.dest, "themes")) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())

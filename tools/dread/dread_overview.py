#!/usr/bin/env python3
"""Export Dread's authored coarse map-download regions into the package manifest."""

import os
import argparse
import json
from collections import OrderedDict
from pathlib import Path

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
MANIFEST = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen")) / "manifest.json"  # an asset-carrying (pre-1.0.0) package dir; never the release package


def color_bytes(color):
    """Convert BCMDL float RGB to its exact UNORM8 identity."""
    return tuple(max(0, min(255, int(round(component * 255.0)))) for component in color)


def region_kind(rgb):
    r, g, b = rgb
    if r:
        return "zone"
    if g:
        return "transport" if b else "station"
    if b:
        return "room"
    return None


def build_overview_regions(positions, colors, triangles):
    """Group intact triangles by native RGB identity, preserving first-seen order."""
    groups = OrderedDict()
    for triangle in triangles:
        identities = {color_bytes(colors[index]) for index in triangle}
        if len(identities) != 1:
            raise ValueError(f"map-room triangle is not flat-colored: {triangle}: {identities}")
        rgb = identities.pop()
        kind = region_kind(rgb)
        if kind is None:
            continue
        region = groups.setdefault(
            rgb,
            {"id": f"0x{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X}", "kind": kind, "t": []},
        )
        region["t"].append(
            [round(coordinate, 1) for index in triangle for coordinate in positions[index]]
        )
    return list(groups.values())


def matching_delimiter(text, start):
    pairs = {"{": "}", "[": "]"}
    opening = text[start]
    closing = pairs[opening]
    depth = 0
    quoted = escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unterminated JSON delimiter at {start}")


def patch_overview_regions(text, area, regions):
    """Patch one area without reformatting any existing manifest content."""
    marker = f'   "{area}": {{'
    area_start = text.index(marker) + len(marker) - 1
    area_end = matching_delimiter(text, area_start)
    property_marker = '    "overview_regions": '
    property_start = text.find(property_marker, area_start, area_end)
    value = json.dumps(regions, indent=1).replace("\n", "\n    ")
    if property_start >= 0:
        value_start = property_start + len(property_marker)
        value_end = matching_delimiter(text, value_start)
        return text[:value_start] + value + text[value_end + 1:]
    insertion = f',\n{property_marker}{value}'
    return text[:area_end] + insertion + text[area_end:]


def export_manifest(manifest_path, romfs_path):
    from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
    from mercury_engine_data_structures.game_check import Game
    from mercury_engine_data_structures.romfs import ExtractedRomFs
    from dread_room_classes import load

    original = manifest_path.read_text()
    manifest = json.loads(original)
    editor = FileTreeEditor(ExtractedRomFs(romfs_path), Game.DREAD)
    total = 0
    exported_by_area = {}
    for area, area_data in manifest["map"]["areas"].items():
        positions, colors, triangles, metadata = load(editor, area)
        regions = build_overview_regions(positions, colors, triangles)
        exported_by_area[area] = regions
        exported = sum(len(region["t"]) for region in regions)
        total += exported
        if exported != len(triangles):
            raise ValueError(f"{area}: exported {exported}/{len(triangles)} nonzero triangles")
        print(
            f"{area}: {len(regions)} identities, {exported} triangles, "
            f"bbox {metadata['bbox_min']}..{metadata['bbox_max']}"
        )
    updated = original
    for area, regions in exported_by_area.items():
        updated = patch_overview_regions(updated, area, regions)
    manifest_path.write_text(updated)
    print(f"wrote {total} triangles to {manifest_path}")
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--romfs", type=Path, default=ROMFS)
    args = parser.parse_args()
    export_manifest(args.manifest, args.romfs)


if __name__ == "__main__":
    main()

# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Open/close fades for the Echoes companion's in-page panels.

Each panel's widgets share one runtime 22 "anim" group gated by the panel's own
visibility point: the group fades on every open/close while the page is shown,
snaps when the page is entered, and stops redrawing once settled. The anim gate
also hides a widget while closed; every widget here is already hidden by that
gate or a narrower one, so visibility and taps are unchanged.
"""


def _needs(*names):
    return set(names)


PANELS = [
    {
        "name": "stamp_tray",
        "page": "map",
        "panel_gate": "echo.stamp.panel",
        "group": "echo.popup.stamp_tray",
        "from": "fade",
        "ms": 180,
        # bg, 4x(button bg/pin icon/tap), cursor highlight (kind-specific), close button (bg/
        # label/tap) -- every widget inside the [982,132,250,818] right-edge tray.
        "needs": _needs(
            "echo.stamp.panel",
            "echo.stamp.kind.1", "echo.stamp.kind.4", "echo.stamp.kind.5", "echo.stamp.kind.6",
        ),
    },
    {
        "name": "warp_panel",
        "page": "map",
        "panel_gate": "echo.warp.panel",
        "group": "echo.popup.warp_panel",
        "from": "fade",
        "ms": 180,
        # bg, warp glyph, confirm (bg/label/tap, shown only once a waypoint is selected),
        # close button (bg/label/tap) -- same right-edge tray convention as the stamp tray.
        "needs": _needs("echo.warp.panel", "echo.warp.confirm.show"),
    },
    {
        "name": "dungeon_floor_menu",
        "page": "map",
        "panel_gate": "echo.dungeon.floor.menu",
        "group": "echo.popup.dungeon_menu",
        "from": "fade",
        "ms": 170,
        # The panel-bg is 10 fixed-height variants (gated by row count); the repeat rows
        # (bg/label/cursor/tap) share one template rect that does not include the
        # repeat_row_dy offset, so the auto box union would undershoot the lower rows --
        # pinned explicitly below.
        "needs": _needs(
            *[f"echo.dungeon.floor.menu.{n}" for n in range(1, 11)],
            "echo.dungeon.floor.menu",
            "echo.dungeon.floor.{i}.selected",
        ),
        # [766,112,188,818] is the largest background variant; the repeat rows can run a
        # little past it (row 9 at dy=792 + row height 80), so this is padded to 950 tall
        # (112+950=1062, inside the 1080 canvas) purely to avoid a transient fade-in clip.
        "box": [766, 112, 188, 950],
    },
]


def apply(manifest):
    for panel in PANELS:
        pages = panel["pages"] if "pages" in panel else [panel["page"]]
        for page in manifest["pages"]:
            if page["id"] not in pages:
                continue
            for widget in page["widgets"]:
                if widget.get("need_bind") in panel["needs"]:
                    anim = {"bind": panel["panel_gate"], "group": panel["group"],
                            "from": panel["from"], "ms": panel["ms"], "easing": "ease_out"}
                    if "box" in panel:
                        anim["box"] = list(panel["box"])
                    widget["anim"] = anim
    return manifest

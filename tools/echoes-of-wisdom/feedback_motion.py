# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Short, eased page fades and rail art cross-fades for the Echoes companion.

page_binds switch pages with a fade; the quick-slot rail art cross-fades with the
runtime 22 Image "src_fade_ms" whenever its source key changes. Both settle and
stop redrawing on their own.
"""
import re
FADE_EASE = "ease_out"
FADE_MS_STANDARD = 170  # within the shared 160-180ms band
FADE_MS_QUICK = 130  # held picker + waiting: brief asks for a quicker fade
QUICK_PAGES = {"waiting", "held"}
RAIL_FADE_MS = 150
_QUICK_ICON = re.compile(r"^echo\.quick\.(\d+)\.icon$")


def apply(manifest):
    for bind in manifest["page_binds"]:
        target = bind["when_equal"]["page"]
        bind["when_equal"] = {
            "page": target, "transition": "fade",
            "duration_ms": FADE_MS_QUICK if target in QUICK_PAGES else FADE_MS_STANDARD,
            "easing": FADE_EASE,
        }
    for page in manifest["pages"]:
        for widget in page["widgets"]:
            match = _QUICK_ICON.match(widget.get("src_bind", ""))
            if match and widget.get("need_bind") == f"echo.quick.{match.group(1)}.valid":
                widget["src_fade_ms"] = RAIL_FADE_MS
    return manifest

# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Haptic vocabulary for the Echoes companion manifest.

Runtime 22 contract (see localbuild/feedback/BRIEF.md): the "haptics" block is
off unless present; precedence per tick is widget "haptic" > action "haptic"
> haptics.<kind>, and only the strongest haptic plays. hold/swipe/refused
always ignore widget/action overrides, so they can only be tuned here.

Classification below is data-driven: every action name is matched against an
explicit table (confirm list, then forced-off patterns, then "was this action
ever reached by a visible widget"). Anything that falls through unmatched -
including an action nobody has written yet - lands on "off" rather than a
strength, by construction (see _classify's final fallback).
"""

# Global vocabulary. hold/swipe default to "heavy"/"light" per the runtime
# when omitted, but no widget in this generator uses on_hold/on_swipe_* (the
# repeated/draggable/drop widgets only ever set on_tap or drop_action), so
# both are set to "off" deliberately: there is nothing to buzz for yet, and a
# future on_hold/on_swipe_* added without revisiting this file should stay
# silent rather than surprise-buzz at "heavy".
_GLOBAL_HAPTICS = {
    "enabled": True,
    "respect_system": True,
    "tap": "click",
    "write": "confirm",
    "select": "click",
    "drag": "light",
    "drop": "confirm",
    "marker": "click",
    # Off on purpose: image widgets have no tap gate in runtime 22, so a closed
    # enabled_bind (e.g. a plain Map tap outside stamp mode) also counts as refused
    # and would buzz "reject" on ordinary taps.
    "refused": "off",
    "hold": "off",
    "swipe": "off",
}

# Actions that change persistent game/companion state once a player reaches
# them through a visible widget: select-for-hold, equip, quick-assign/equip,
# favorite, the actual inventory action, warp execution, stamp placement and
# the native combat/tutorial button presses. Everything else a widget can
# reach is navigation/tab/row-inspect/open-close-panel/sort/view-choice, i.e.
# the lighter "click" vocabulary (handled by the fallback in _classify).
_CONFIRM_ACTIONS = {
    "select_echo", "equip_echo", "quick_equip",
    "quick_assign_0", "quick_assign_1", "quick_assign_2",
    "favorite_selected", "inventory_action", "warp_travel", "stamp_tap",
    "combat_native_x", "combat_native_y", "combat_native_a",
    "prologue_native_x", "prologue_native_y", "prologue_native_a",
}

# Name patterns that stay silent even if a widget happens to tap them
# (map_reset_*) or if they are driven by the enforce loop / persistence
# bridges rather than touch (drive_*, menu_*, native_*, *_persist_*,
# *_restore_*). Checked before the "was this reached by a widget" fallback.
_FORCED_OFF_PREFIXES = ("map_reset_", "drive_", "menu_", "native_")
_FORCED_OFF_SUBSTRINGS = ("_persist_", "_restore_")


def _is_forced_off(name):
    return name.startswith(_FORCED_OFF_PREFIXES) or any(
        token in name for token in _FORCED_OFF_SUBSTRINGS
    )


def _reached_actions(manifest):
    """Actions a player can reach directly from a visible widget: on_tap,
    on_hold/on_swipe_* (none exist today, but a future one should count),
    and drop targets. Anything else is only wired through "enforce" entries
    or a module-internal sequence (e.g. current_echo, prologue_inspect,
    sort_next/sort_previous are defined but never on_tap'd by any widget)."""
    reached = set()
    for page in manifest.get("pages", []):
        for widget in page.get("widgets", []):
            for key, value in widget.items():
                if key == "on_tap" or key == "drop_action" or (
                    isinstance(key, str) and (key.startswith("on_hold") or key.startswith("on_swipe"))
                ):
                    reached.add(value)
    return reached


def _classify(name, reached):
    if name in _CONFIRM_ACTIONS:
        return "confirm"
    if _is_forced_off(name):
        return "off"
    if name not in reached:
        return "off"
    return "click"


def apply(manifest):
    manifest["haptics"] = dict(_GLOBAL_HAPTICS)

    actions = manifest.get("actions", {})
    reached = _reached_actions(manifest)
    for name, action in actions.items():
        action["haptic"] = _classify(name, reached)

    # Widget overrides beat action haptics. ~108 tap widgets currently force
    # "haptic":"off" on top of actions that should now buzz; drop that
    # override so the action's strength applies. Keep the override only
    # where the widget's own action is itself automated/silent (map_reset_*,
    # confirmed off above) - there is nothing to un-silence there.
    #
    # Repeated widgets (e.g. the map_reset buttons) can name their action
    # with a literal "{i}" the runtime substitutes per instance, so that
    # string is never a real key in `actions`. Classify the widget's raw
    # on_tap/drop_action text directly with the same rules instead of doing
    # a dict lookup - map_reset_field_{i}/map_reset_dungeon_{i} still match
    # the "map_reset_" prefix and resolve to "off" either way.
    for page in manifest.get("pages", []):
        for widget in page.get("widgets", []):
            if widget.get("haptic") != "off":
                continue
            action_name = widget.get("on_tap") or widget.get("drop_action")
            if action_name is not None and _classify(action_name, reached) == "off":
                continue
            del widget["haptic"]
    return manifest

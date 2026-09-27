#!/usr/bin/env python3
"""Turn the Metroid Dread 1.0.0 package manifest into its asset-free form.

What changes (everything else is copied untouched):
  * map.atlas            file:icons/icons.png -> the game's own minimap icon sheet in romfs
                         (romfs:/textures/system/minimap/icons/icons.bctex; the runtime's BCTEX
                         decoder yields the same RGBA8, byte for byte).
  * map.areas_src        "module:dread:areas": the module rebuilds every area's map data from the
                         player's romfs (bmmap, brfld, bmscc, maproom bcmdl) on first use.
  * map.areas.<area>     keeps only what we authored: the geometry references (now module:
                         sources) with the layer kinds/colours/flags, and the room-category styling.
                         Each room category names the maproom class its polygons come from
                         ("polys_from"). Everything derived from level data (min/max, icons,
                         camera_rects, occluders, vignettes, overview_regions, water_pools,
                         water_changes, category polys) is dropped: the module generates it.
  * page labels          whose text is a string of the game's localization (us_english.txt) bind
                         to "loc:<KEY>" instead; the module publishes those strings from romfs.
  * min_runtime          12 (the module-data runtime).

  dread_af_manifest.py <in manifest.json> <out manifest.json> [us_english.json]
"""
import json
import sys

ATLAS = "romfs:/textures/system/minimap/icons/icons.bctex"
CATEGORY_CLASS = {"emmi": "emmy", "station": "special", "transport": "transport"}
DERIVED_AREA_KEYS = {"min", "max", "icons", "camera_rects", "occluders", "vignettes",
                     "overview_regions", "water_pools", "water_changes"}
HEADER = "#FF7FD4E8"  # section headers / titles use the header colour on the page

# Page text -> localization key. Headers (drawn in the header colour) use the section/title keys,
# items the item-name keys. Every key's us_english.txt value equals the literal it replaces
# (checked below when us_english.json is given).
ITEM_KEYS = {
    "POWER BEAM": "GUI_SAMUSMENU_NAME_POWER_BEAM",
    "WIDE BEAM": "GUI_SAMUSMENU_NAME_WIDE_BEAM",
    "PLASMA BEAM": "GUI_SAMUSMENU_NAME_PLASMA_BEAM",
    "WAVE BEAM": "GUI_SAMUSMENU_NAME_WAVE_BEAM",
    "HYPER BEAM": "GUI_SAMUSMENU_NAME_HYPER_BEAM",
    "CHARGE BEAM": "GUI_SAMUSMENU_NAME_CHARGE_BEAM",
    "DIFFUSION BEAM": "GUI_SAMUSMENU_NAME_DIFUSSION_CHARGE",
    "GRAPPLE BEAM": "GUI_SAMUSMENU_NAME_GRAPPLE_BEAM",
    "MISSILE": "GUI_SAMUSMENU_NAME_MISSILE",
    "SUPER MISSILE": "GUI_SAMUSMENU_NAME_SUPER_MISSILE",
    "ICE MISSILE": "GUI_SAMUSMENU_NAME_ICE_MISSILE",
    "STORM MISSILE": "GUI_SAMUSMENU_NAME_MULTI_LOCKON_MISSILE",
    "BOMB": "GUI_SAMUSMENU_NAME_BOMB",
    "CROSS BOMB": "GUI_SAMUSMENU_NAME_LINE_BOMB",
    "POWER BOMB": "GUI_SAMUSMENU_NAME_POWER_BOMB",
    "PHANTOM CLOAK": "GUI_SAMUSMENU_NAME_OPTICAL_CAMO",
    "FLASH SHIFT": "GUI_SAMUSMENU_NAME_GHOST_DASH",
    "PULSE RADAR": "GUI_SAMUSMENU_NAME_SONAR",
    "POWER SUIT": "GUI_SAMUSMENU_NAME_POWER_SUIT",
    "VARIA SUIT": "GUI_SAMUSMENU_NAME_VARIA_SUIT",
    "GRAVITY SUIT": "GUI_SAMUSMENU_NAME_GRAVITY_SUIT",
    "METROID SUIT": "GUI_SAMUSMENU_NAME_HYPER_SUIT",
    "MORPH BALL": "GUI_SAMUSMENU_NAME_MORPHBALL",
    "SPIDER MAGNET": "GUI_SAMUSMENU_NAME_SPIDER_MAGNET",
    "SPEED BOOSTER": "GUI_SAMUSMENU_NAME_SPEED_BOOSTER",
    "SPIN BOOST": "GUI_SAMUSMENU_NAME_DOUBLE_JUMP",
    "SPACE JUMP": "GUI_SAMUSMENU_NAME_SPACE_JUMP",
    "SCREW ATTACK": "GUI_SAMUSMENU_NAME_SCREW_ATTACK",
}
HEADER_KEYS = {
    "MISSION LOG": "GUI_MISSIONLOG_TITLE",
    "SAMUS": "GUI_SAMUSMENU_TITLE",
    "BEAM": "GUI_SAMUSMENU_SECTION_BEAMS",
    "MISSILE": "GUI_SAMUSMENU_SECTION_MISSILE",
    "BOMB": "GUI_SAMUSMENU_SECTION_BOMBS",
    "AEION": "GUI_SAMUSMENU_SECTION_AEION",
    "SUIT": "GUI_SAMUSMENU_SECTION_SUIT",
    "MORPH BALL": "GUI_SAMUSMENU_SECTION_MORPHBALL",
    "MISC.": "GUI_SAMUSMENU_SECTION_MISC",
}


def module_geo(ref):
    # "file:map/s010_cave.water.geo" -> "module:dread:map/s010_cave.water.geo" (the ".emmy." in
    # the name is what the module's AreaHasEmmi looks for, so the path shape is kept)
    assert ref.startswith("file:map/") and ref.endswith(".geo"), ref
    return "module:dread:" + ref[len("file:"):]


def convert(man, loc=None):
    out = json.loads(json.dumps(man))
    out["min_runtime"] = 12
    m = out["map"]
    assert m["atlas"] == "file:icons/icons.png", m["atlas"]
    m["atlas"] = ATLAS
    m["areas_src"] = "module:dread:areas"
    for name, area in m["areas"].items():
        for k in DERIVED_AREA_KEYS:
            area.pop(k, None)
        area["geo"] = module_geo(area["geo"])
        for layer in area.get("layers", []):
            layer["geo"] = module_geo(layer["geo"])
        for cat in area.get("room_categories", []):
            cat.pop("polys", None)
            cat["polys_from"] = "maproom:" + CATEGORY_CLASS[cat["id"]]
        unknown = set(area) - {"geo", "layers", "room_categories"}
        assert not unknown, (name, unknown)
    return out, bind_loc_labels(out, loc)


def bind_loc_labels(man, loc=None):
    """Bind page labels whose text is one of the game's localization strings to "loc:<KEY>"
    (in place). Returns the number of labels changed; running it twice changes nothing."""
    replaced = 0
    for page in man["pages"]:
        for w in page["widgets"]:
            if w.get("type") != "label" or "text" not in w:
                continue
            table = HEADER_KEYS if w.get("color") == HEADER else ITEM_KEYS
            key = table.get(w["text"])
            if key is None:
                continue
            if loc is not None:
                assert loc[key] == w["text"], (key, loc[key], w["text"])
            del w["text"]
            w["bind_text"] = "loc:" + key
            replaced += 1
    return replaced


def main():
    src, dst = sys.argv[1], sys.argv[2]
    loc = json.load(open(sys.argv[3])) if len(sys.argv) > 3 else None
    man = json.load(open(src))
    out, replaced = convert(man, loc)
    with open(dst, "w") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print(f"wrote {dst}: {replaced} labels bound to loc:, areas -> template, atlas -> romfs")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode Pokemon Sword/Shield 1.3.2 wild encounter tables from the player's romfs, keyed by Town Map zone.
Numbers and ids only (species / form / rate / level / weather); names come from the game's own
message tables at run time, in the companion language.

Sources (all inside the romfs):
  bin/archive/field/resident/data_table.gfpak   (GFLXPACK, LZ4 members, gfpak.py)
      encount_k.bin         Sword "hidden" tables (grass "!" / shaking trees / fishing)
      encount_symbol_k.bin  Sword "symbol" tables (overworld wanderers: land / flying / surfing / special)
      encount_t.bin, encount_symbol_t.bin: the same for Shield (the variant's romfs_suffix,
                            sw_variants.py; decode(tables=...))
    FlatBuffers (pkNX FlatBuffers/SWSH/Schemas/EncounterArchive.fbs):
      EncounterArchive { Field_00:uint; EncounterTables:[EncounterTable] }
      EncounterTable   { ZoneID:ulong; SubTables:[EncounterSubTable] }
      EncounterSubTable{ LevelMin:ubyte; LevelMax:ubyte; Slots:[EncounterSlot] }
      EncounterSlot    { Probability:ubyte; Species:int; Form:ubyte }
    Hidden tables: ZoneID = FNV-1a 64 of the zone id ("z_r0101"), the hash the Town Map's b_z_*
      panes and sw_fly.h use. 11 sub-tables: 9 weathers + [9] shaking trees + [10] fishing.
    Symbol tables: ZoneID is a spawner-table id, NOT a zone (resolved through placement.gfpak).
  bin/archive/field/resident/placement.gfpak    (PlacementZoneArchive flatbuffers, pkNX Placement*.fbs)
      PlacementZone.Meta(0).ZoneID(1) = zone hash; PlacementZone.Symbols(20)[].Object(0) =
      { Field_01(1) = spawn kind (0 land, 1 flying, 2 surfing, 3 special/burrow), SymbolHash(7) }.
  bin/message/English/script/place_name.tbl     labels only (PLACENAME_Z_<ZONE>_01 -> zone ids)

Encounter sub-table weather order (pkNX SWSHEncounterType): 0 Normal, 1 Overcast, 2 Raining,
3 Thunderstorm, 4 Intense Sun, 5 Snowing, 6 Snowstorm, 7 Sandstorm, 8 Heavy Fog. NOT the live
weather enum's order (gen_route_encounters.py converts).
"""
import collections
import os
import struct
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gfpak import GFPak, fnv1a64  # noqa: E402
from fb import root  # noqa: E402

TREES, FISHING = 9, 10
SYMBOL_KIND = {0: "wander", 1: "flying", 2: "surf", 3: "special"}

# Zones whose tables exist but that have no PLACENAME_*_01 label: FNV-1a 64 is one-way, so these
# were recovered once by hashing every z_<1-2 letters><4 digits> candidate against the table ids.
UNLABELLED_ZONES = ["z_r0202", "z_d0103", "z_r1002"]

# Which weathers can occur where (encounter-table order): pkNX EncounterTableUtil.WeatherbyArea
# (FlatBuffers/SWSH/Gen8/Wild/EncounterTableUtil.cs, keyed there by met location; re-keyed here by
# the zone those locations name). Not in the romfs; the game's tables hold filler rows for weathers
# that never happen (Giant's Bed "Sandstorm" = one species at 100 %). An unlisted zone is Normal
# only, as in pkNX. Weather that "bleeds" in from a neighbouring area at its edge is not listed.
_ALL = list(range(9))
_IOA = [0, 1, 2, 3, 4, 7, 8]          # All_IoA
_IOA5 = [0, 1, 2, 3, 4, 8]            # Normal | Overcast | Stormy | Intense Sun | Heavy Fog
_CT = [0, 1, 2, 3, 4, 5, 6, 8]        # All_CT
_CT5 = [0, 1, 4, 5, 6, 8]             # Normal | Overcast | Intense Sun | Icy | Heavy Fog
WEATHER_BY_ZONE = {
    "z_r0601": [4],                   # Route 6
    "z_r0901": [5],                   # Route 8 (Steamdrift Way)
    "z_r0902": [5],                   # Route 9
    "z_r0903": [5],                   # Route 9 (Circhester Bay)
    "z_r0904": [1],                   # Route 9 (Outer Spikemuth)
    "z_r1001": [6], "z_r1002": [6],   # Route 10
    **{z: _ALL for z in ("z_wr0102", "z_wr0103", "z_wr0104", "z_wr0105", "z_wr0106", "z_wr0107",
                         "z_wr0108", "z_wr0109", "z_wr0110", "z_wr0121", "z_wr0122", "z_wr0131",
                         "z_wr0132", "z_wr0133", "z_wr0134", "z_wr0135", "z_wr0136")},  # Wild Area
    "z_wr0201": _IOA5, "z_wr0202": _IOA5, "z_wr0203": _IOA, "z_wr0204": _IOA5, "z_wr0206": _IOA,
    "z_wr0208": _IOA5, "z_wr0209": _IOA, "z_wr0211": [0, 1, 2, 4, 7, 8], "z_wr0221": _IOA5,
    "z_wr0222": _IOA5, "z_wr0223": _IOA5, "z_wr0224": _IOA5, "z_wr0231": _IOA5,  # Isle of Armor
    "z_wr0301": _CT5, "z_wr0303": _CT5, "z_wr0304": _CT, "z_wr0305": _CT, "z_wr0306": _CT5,
    "z_wr0307": [1], "z_wr0308": _CT5, "z_wr0310": _CT, "z_wr0311": [1],
    "z_wr0312": [0, 1, 2, 3, 5, 6, 8], "z_wr0313": _CT, "z_wr0321": [0, 1, 2, 3, 4, 5, 8],
    "z_wr0322": [1],                  # Crown Tundra
}


def read_tbl(path):
    """GameFreak message label table (AHTB): [(label, hash)]."""
    d = Path(path).read_bytes()
    n = struct.unpack_from("<I", d, 4)[0]
    off, out = 8, []
    for _ in range(n):
        h = struct.unpack_from("<Q", d, off)[0]
        ln = struct.unpack_from("<H", d, off + 8)[0]
        off += 10
        out.append((d[off:off + ln].rstrip(b"\0").decode(), h))
        off += ln
    return out


def zone_ids(romfs):
    """zone hash -> zone id, from the English place_name labels plus UNLABELLED_ZONES."""
    out = {}
    for label, _h in read_tbl(Path(romfs) / "bin/message/English/script/place_name.tbl"):
        if label.startswith("PLACENAME_Z_") and label.endswith("_01"):
            zid = label[len("PLACENAME_"):-3].lower()
            out[fnv1a64(zid.encode())] = zid
    for zid in UNLABELLED_ZONES:
        out[fnv1a64(zid.encode())] = zid
    return out


def sub_table(st):
    lo, hi = st.scalar(0, "B"), st.scalar(1, "B")
    slots = []
    for s in st.tables(2):
        sp = s.scalar(1, "i")
        if sp <= 0 or s.scalar(0, "B") == 0:
            continue
        slots.append({"species": sp, "form": s.scalar(2, "B"), "rate": s.scalar(0, "B")})
    return {"min": lo, "max": hi, "slots": slots} if slots and lo else None


def merge_slots(slots):
    """Same species+form listed twice in one table (Buneary 15+15) -> one entry, rates summed."""
    out = collections.OrderedDict()
    for s in slots:
        k = (s["species"], s["form"])
        if k in out:
            out[k]["rate"] += s["rate"]
        else:
            out[k] = dict(s)
    return list(out.values())


def weather_tables(subs, n=9):
    """9 weather sub-tables -> [{weathers:[table ids], min, max, slots}], identical tables merged."""
    groups = []
    for w in range(min(n, len(subs))):
        t = sub_table(subs[w])
        if not t:
            continue
        t["slots"] = merge_slots(t["slots"])
        for g in groups:
            if g["min"] == t["min"] and g["max"] == t["max"] and g["slots"] == t["slots"]:
                g["weathers"].append(w)
                break
        else:
            groups.append({"weathers": [w], **t})
    return groups


def decode(romfs, tables=("encount_k.bin", "encount_symbol_k.bin")):
    """{zone hash: {zone, weathers_possible, grass[], trees, fishing, symbol[]}} for every zone with data.
    tables: the version's hidden and symbol tables (sw_variants.Variant.encounter_tables)."""
    romfs = Path(romfs)
    ids = zone_ids(romfs)
    res = romfs / "bin/archive/field/resident"
    data = GFPak((res / "data_table.gfpak").read_bytes())
    hidden_name, symbol_name = tables
    hidden = {t.scalar(0, "Q"): t for t in root(data.member(data.index_of_name(hidden_name))).tables(1)}
    symbol = {t.scalar(0, "Q"): t for t in root(data.member(data.index_of_name(symbol_name))).tables(1)}
    place = GFPak((res / "placement.gfpak").read_bytes())
    spawners = collections.defaultdict(collections.Counter)  # zone -> (table, kind) -> count
    for i in range(len(place.files)):
        try:
            zones = root(place.member(i)).tables(0)
        except Exception:
            continue  # not a PlacementZoneArchive
        for z in zones:
            try:
                zid = z.table(0).scalar(1, "Q")
                syms = z.tables(20)
            except Exception:
                continue
            for s in syms:
                o = s.table(0)
                h = o.scalar(7, "Q")
                if h and h != 0xCBF29CE484222645:
                    spawners[zid][(h, o.scalar(1, "i"))] += 1
    out = {}
    for zh in sorted(set(hidden) | set(spawners)):
        zid = ids.get(zh)
        e = {"zone_hash": zh, "zone": zid, "weathers_possible": WEATHER_BY_ZONE.get(zid, [0])}
        if zh in hidden:
            subs = hidden[zh].tables(1)
            e["grass"] = weather_tables(subs)
            if len(subs) > TREES and (t := sub_table(subs[TREES])):
                t["slots"] = merge_slots(t["slots"])
                e["trees"] = t
            if len(subs) > FISHING and (t := sub_table(subs[FISHING])):
                t["slots"] = merge_slots(t["slots"])
                e["fishing"] = t
        sym = []
        for (h, kind), n in sorted(spawners.get(zh, {}).items(), key=lambda kv: (kv[0][1], -kv[1], kv[0][0])):
            if h in symbol:
                sym.append({"kind": SYMBOL_KIND.get(kind, "special"), "spawners": n,
                            "weathers": weather_tables(symbol[h].tables(1))})
        if sym:
            e["symbol"] = sym
        if any(e.get(x) for x in ("grass", "trees", "fishing", "symbol")):
            if zid is None:
                raise SystemExit("zone 0x%016X has encounter data but no known zone id" % zh)
            out[zh] = e
    return out

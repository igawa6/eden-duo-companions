// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Field map and overlay (p5r_map_reader.h, p5r_map_overlay.h).
//   - SampleMap: the field variant, the native map context and marker (supported assets only),
//     the overlay (fog, parts, POIs, radar enemies) and the player icon's rotation.
//   - PublishMap: map.ready, map.area, map.x / map.y, map.player.*; PublishOverlay: map.fog.*,
//     map.part.*, map.poi.*, map.radar.ready, map.enemy.*.

#include "p5r_reader.h"

namespace p5r_module {

// Field only: map variant, the native map + marker, overlay (fog/parts/POIs/radar), player icon.
void Reader::SampleMap(Snapshot& out, u64 field_context) const {
    out.field_variant_ready = ReadAt(field_context, 0x1fa, out.field_variant);
    out.map_ready =
        out.battle.state_known && !out.battle.active &&
        p5r_map::Sample([&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                        host.main_base, field_context, out.map);
    if (out.map_ready) {
        const u64 asset = (u64{out.map.asset_major} << 32) | (u64{out.map.asset_minor} << 16) |
                          static_cast<u64>(out.map.layer);
        out.map_ready = std::binary_search(p5r_map::SupportedAssets.begin(),
                                           p5r_map::SupportedAssets.end(), asset);
    }
    if (out.map_ready)
        out.overlay_ready = p5r_map_overlay::Sample(
            [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
            host.main_base, host.main_size, field_context, out.map.context, out.map.field_major,
            out.overlay);
#ifdef P5R_MAP_TRACE
    if (host.log && out.map.context) {
        int trace_poi28 = -1;
        if (out.overlay_ready) {
            trace_poi28 = 0;
            for (unsigned q = 0; q < out.overlay.poi_count; ++q)
                if (out.overlay.pois[q].type == 28)
                    trace_poi28 = 1;
        }
        static u64 last_ctx = 0;
        static int last_match = -1;
        static unsigned tick = 0;
        if (out.map.context != last_ctx || int(out.map.native_marker_matches) != last_match ||
            (++tick % 30) == 0) {
            char m[256];
            std::snprintf(m, sizeof m,
                          "P5R maptrace: ctx=%llx field=%d/%d asset=%d/%d layer=%d valid=%d "
                          "match=%d world=(%.1f,%.1f,%.1f) px=(%.2f,%.2f) poi28=%d",
                          (unsigned long long)out.map.context, out.map.field_major,
                          out.map.field_minor, out.map.asset_major, out.map.asset_minor,
                          out.map.layer, out.map.marker_valid ? 1 : 0,
                          out.map.native_marker_matches ? 1 : 0, out.map.world_x, out.map.world_y,
                          out.map.world_z, out.map.pixel_x, out.map.pixel_y, trace_poi28);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, m);
            last_ctx = out.map.context;
            last_match = out.map.native_marker_matches;
        }
    }
#endif
    // The rotated player triangle follows the same native draw gate as the marker (C36F4C).
    if (out.map_ready && out.map.marker_valid)
        out.player_rotation_ready = p5r_map_overlay::PlayerRotation(
            [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, field_context,
            out.player_rotation);
}
// map.* overlay outputs. Nothing is published for an area that is not ready, so the host's
// per-sample snapshot drops every stale part/POI/radar slot on its own.
void Reader::PublishOverlay(bool ready, const p5r_map_overlay::Frame& o) const {
    I64("map.overlay.ready", ready);
    if (!ready)
        return;
    char key[48];
    I64("map.fog.red", o.fog_red);
    I64("map.fog.dark", !o.fog_red);
    I64("map.parts.count", o.part_count);
    for (unsigned i = 0; i < o.part_count; ++i) {
        std::snprintf(key, sizeof(key), "map.part.%u", i);
        I64(key, o.part_visible[i]);
    }
    I64("map.poi.count", o.poi_count);
    for (unsigned i = 0; i < o.poi_count; ++i) {
        std::snprintf(key, sizeof(key), "map.poi.%u.kind", i);
        I64(key, o.pois[i].type);
        std::snprintf(key, sizeof(key), "map.poi.%u.x", i);
        F64(key, o.pois[i].x);
        std::snprintf(key, sizeof(key), "map.poi.%u.y", i);
        F64(key, -o.pois[i].y);
    }
    I64("map.radar.ready", o.radar_ready);
    I64("map.enemy.count", o.radar_ready ? o.enemy_count : 0);
    for (unsigned i = 0; o.radar_ready && i < o.enemy_count; ++i) {
        std::snprintf(key, sizeof(key), "map.enemy.%u.kind", i);
        I64(key, 1);
        std::snprintf(key, sizeof(key), "map.enemy.%u.x", i);
        F64(key, o.enemies[i].x);
        std::snprintf(key, sizeof(key), "map.enemy.%u.y", i);
        F64(key, -o.enemies[i].y);
    }
}
// map.ready / map.area / map.x,y, the overlay and the player icon (field only).
void Reader::PublishMap(const Snapshot& out, bool map_ready) const {
    I64("map.ready", map_ready);
    char map_area[64]{};
    if (map_ready)
        std::snprintf(map_area, sizeof(map_area), "rmap_%u_%u_%d", out.map.asset_major,
                      out.map.asset_minor, out.map.layer);
    Text("map.area", map_area);
    if (map_ready && out.map.marker_valid && host.publish_f64) {
        host.publish_f64(host.userdata, "map.x", out.map.pixel_x);
        host.publish_f64(host.userdata, "map.y", -out.map.pixel_y);
    }
    PublishOverlay(map_ready && out.overlay_ready, out.overlay);
    // Native player icon (C36EE0): rotated triangle at the marker; the package maps the
    // 64 rotation buckets (5.625 degrees, bucket 0 = apex up) to pre-rotated sprites.
    if (map_ready && out.map.marker_valid && out.player_rotation_ready) {
        I64("map.player.count", 1);
        I64("map.player.0.kind", static_cast<s64>(std::lround(out.player_rotation / 5.625f)) % 64);
        F64("map.player.0.x", out.map.pixel_x);
        F64("map.player.0.y", -out.map.pixel_y);
    }
}

} // namespace p5r_module

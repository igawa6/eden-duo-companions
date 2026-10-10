// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes 1.41 exploration reader (Field Home slice). Contract: research/chained-echoes/
// contracts/field-readers.md. Read-only: no guest write anywhere in this file.
//
// What it reads (all re-resolved by IL2CPP class name; the scene objects are validated through
// the native GameObject back-pointer):
//   GameManager statics   area code, current MapScene / map object / LvlManager, event, skit,
//                         (menuOpened / whichMenu are documented in the contract, not used yet)
//   Player statics        playerPos (world), playerDirection (1 up, 2 left, 3 down, 4 right)
//   Map (scene object)    mapName, the hierarchy nodes MapImage / Map / Map Grid / Masks /
//                         teleports (their local positions = native map coordinates)
//   SaveDataOwn           mapExplored (discovered crystals, explored map cells), quests and
//                         tasks, the Reward Board task list
// Player -> map: the game's own Map.OpenMap formula (k = 0.0492, 0.1205 on the world map):
//   x = mapPosition.x + (player.x - map.x) * k,  y = -(mapPosition.y + (map.y - player.y) * k)
// Interiors (MapScene.lockedMarkerPosition != 0) use the parent scene and the locked position.
//
// Outputs (the picture keys go through ce_reader's double buffer, see IdleKey()):
//   ce.fhome / ce.fsub / ce.fboard          tap-target gates of the field pages
//   ce.fmark, ce.fmx, ce.fmy, ce.fmd        the live player marker widget (canvas px, facing)
#pragma once

#include "ce_page_field.h"
#include "core/mods/dsmod_module_abi.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ce_draw {
class Art;
}

namespace ce_field {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

class Reader {
public:
    explicit Reader(ce_draw::Art& art);
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Timing thread, every sample. `in_game` = ce_reader is in its in-game (no battle) state.
    void Sample(const EdenDsmodHostApi& host, bool in_game);
    /// The picture for ce_reader's in-game state ("" = keep ce_reader's holding view).
    std::string IdleKey();
    /// Publish the field gates for the picture ce_reader actually shows (after its Sample).
    void PublishFor(const EdenDsmodHostApi& host, const std::string& shown, bool loading);
    /// Module actions; false = not a field action.
    bool Action(std::string_view name, s64 argument);
    /// Asset worker: the model behind a "module:ce:fh..." key.
    std::optional<ce_page_field::FieldView> Model(const std::string& key);

    /// Skills page (ce_skill_reader): the picture for page 2 ("" = keep the placeholder), the
    /// native Skills menu mirror (shows page 2 while it is open) and the Skills button alarm.
    std::function<std::string()> skills_key;
    std::function<bool()> skills_mirror;
    std::function<bool()> skills_alarm;
    /// Crystals page (ce_crystal_reader): its picture for page 1 ("" = keep the placeholder) and
    /// the native Equipment / smith fusion mirror (shows the page while that menu is open).
    std::function<std::string()> crystals_key;
    std::function<bool()> crystals_mirror;

private:
    struct Mem;
    struct MapCache {
        u64 map_object{};
        std::string map_name;
        bool ok{};
        ce_page_field::MapGeometry geo;
        struct Tele {
            float x{}, y{};
            std::string state;
        };
        std::vector<Tele> teleports;
        struct Mask {
            float cx{}, cy{}, half{};
            std::string state;
        };
        std::vector<Mask> masks;
    };

    bool Refresh(const EdenDsmodHostApi& host);
    void ReadSave(Mem& m, u64 sd);
    bool ReadMap(Mem& m, u64 map_object);
    std::optional<std::pair<float, float>> SceneWorldPos(Mem& m, u64 map_go);
    std::string Translate(std::string_view meta, int row, std::string fallback);
    std::string KeyFor(const ce_page_field::FieldView& v);

    ce_draw::Art& art;
    u64 sample{};
    bool refresh_now{};
    std::unordered_map<u64, std::string> class_names;

    // live model
    bool have{};
    ce_page_field::FieldView view; ///< last valid model (kept through a failed refresh)
    std::string area_code;
    float player_x{}, player_y{};  ///< map-local
    int facing{};
    bool interior{};
    float view_cx{}, view_cy{};    ///< map-local point drawn at the canvas anchor
    bool view_set{};

    MapCache map;
    std::map<u64, std::pair<float, float>> scene_world;
    std::set<std::string> explored;
    u64 save_stamp{};

    // UI state (module-local)
    int page{};         ///< 0 home, 1 crystals, 2 skills, 3 board
    std::string section; ///< board section (area code); "" = current area
    int sel_tile{-1};    ///< board: selected task id
    std::vector<int> win_ids; ///< board: task id under each visible cell (-1 empty)

    // picture keys
    std::mutex mutex;
    std::map<std::string, ce_page_field::FieldView> models;
    std::deque<std::string> order;
    std::map<std::string, std::pair<float, float>> key_view; ///< key -> view anchor (marker)
    // IdleKey memo: the key stays valid until the next refresh, page or alarm change
    u64 view_gen{};
    u64 idle_gen{~u64{}};
    int idle_pg{-1};
    bool idle_alarm{};
    std::string idle_key;
    std::string last_log;
};

} // namespace ce_field

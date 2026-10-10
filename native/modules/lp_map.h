// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
namespace lp_map {
// Native GetTownMapPos uses the saved outdoor position for rooms. A failed
// table lookup retains the previous cursor; scope that fallback to this save.
struct PlayerLocation {
    struct Location { int zone; std::pair<int, int> cell; };
    std::uint64_t owner = 0;
    std::optional<Location> last;
    std::optional<Location> Update(std::uint64_t save, bool acquired, bool room,
                                   std::optional<Location> current, std::optional<Location> outdoor) {
        if (save != owner || !save || !acquired) last.reset();
        owner = save;
        if (!save || !acquired) return last;
        const auto next = room ? outdoor : current ? current : outdoor;
        if (next) last = next;
        return last;
    }
};
struct Overlay { std::string_view name,sprite; int x,y,w,h; };
// Native ui resident Townmap prefab geometry, relative to the map center.
inline constexpr std::array<Overlay,29> overlays{{
    {"Image_Parts_01","map_img_map_01_parts_01",-217,205,82,72},
    {"Image_Parts_02","map_img_map_01_parts_02",-136,205,82,72},
    {"Image_Parts_03","map_img_map_01_parts_03",478,223,80,94},
    {"Image_Parts_Patch","map_ico_road_01_parts_03",321,142,186,194},
    {"Image_Parts_03","map_ico_road_01_parts_02",477,143,32,200},
    {"Image_Base","map_ico_road_01_base",109,-10,764,608},
    {"Image_Parts_01","map_ico_road_01_parts_01",-219,201,36,38},
    {"Image_Parts_02","map_ico_road_01_parts_01",-147,201,36,38},
    {"Image_Parts_05","map_ico_road_01_parts_04",364,-122,62,50},
    {"Image_c2","map_ico_symbol_01_02_01",-267,-145,34,60},
    {"Image_c1","map_ico_symbol_01_05_01",-159,-169,58,60},
    {"Image_t2","map_ico_symbol_02_01_01",-147,-253,34,36},
    {"Image_t1","map_ico_symbol_02_01_01",-219,-277,34,36},
    {"Image_c3","map_ico_symbol_01_04_01",-39,-169,58,60},
    {"Image_c4","map_ico_symbol_01_03_01",-15,-1,58,60},
    {"Image_t3","map_ico_symbol_02_02_01",-147,-73,34,60},
    {"Image_c9","map_ico_symbol_01_02_01",21,287,34,60},
    {"Image_c5","map_ico_symbol_01_05_01",105,-121,58,60},
    {"Image_c7","map_ico_symbol_01_05_01",321,-49,58,60},
    {"Image_c6","map_ico_symbol_01_05_01",249,-241,58,60},
    {"Image_c8","map_ico_symbol_01_05_01",441,-169,58,60},
    {"Image_c10","map_ico_symbol_01_01_01",429,11,34,36},
    {"Image_t5","map_ico_symbol_02_01_01",93,11,34,36},
    {"Image_t4","map_ico_symbol_02_01_01",201,-85,58,36},
    {"Image_t6","map_ico_symbol_02_01_01",261,155,34,36},
    {"Image_t7","map_ico_symbol_02_01_01",393,59,58,36},
    {"Image_c10_02","map_ico_symbol_03_01_01",429,-13,34,36},
    {"Image_c11","map_ico_symbol_01_01_01",249,83,58,36},
    {"Image_r221","map_ico_symbol_03_01_01",-51,-301,34,36},
}};
static_assert(overlays.size()<31);
inline bool Visible(int flag,const std::array<int32_t,500>& work) {
    return flag>=500 || (flag>=0 && work[flag]!=0);
}
inline bool Arrived(int view,int color,bool town,const std::array<int32_t,500>& work,
                    const std::array<std::uint8_t,1000>& sys) {
    return Visible(view,work) && (!town || (color>=0 && color<1000 && sys[color]==1));
}
}

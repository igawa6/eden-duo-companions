// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_screen.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
namespace {
struct RecordedMemory {
  uint64_t main{};
  std::map<uint64_t,uint8_t> bytes;
  explicit RecordedMemory(const char* path) {
    if(!path)return;
    std::ifstream f(path,std::ios::binary);char magic[8]{};uint32_t count{};
    f.read(magic,8);f.read(reinterpret_cast<char*>(&main),8);f.read(reinterpret_cast<char*>(&count),4);
    assert(f && (std::string(magic,8)=="OT0MAP01" || std::string(magic,8)=="OT0READ1") && count<=10000);
    for(uint32_t i=0;i<count;++i) {
      uint64_t at{};uint32_t n{};f.read(reinterpret_cast<char*>(&at),8);f.read(reinterpret_cast<char*>(&n),4);
      assert(f && n<=65536);std::vector<uint8_t> b(n);f.read(reinterpret_cast<char*>(b.data()),n);assert(f);
      for(size_t j=0;j<n;++j)bytes[at+j]=b[j];
    }
  }
  ot0::ReadMemory reader() const {return [this](uint64_t at,void* out,size_t n) {
    auto* b=static_cast<uint8_t*>(out);
    for(size_t i=0;i<n;++i) {const auto p=bytes.find(at+i);if(p==bytes.end())return false;b[i]=p->second;}
    return true;
  };}
};
void Mask() {
  const ot0::Image source{8, 8, std::vector<ot0::u8>(8 * 8 * 4, 255)};
  ot0::ExplorationSnapshot map;
  map.valid = true;
  map.block_width = 2;
  map.bounds_min = {0, 0, 0};
  map.bounds_max = {8, 8, 1};
  map.axis = {1, -1};
  map.offset = {0, 8};
  map.scale = {1, 1};
  map.revealed_cells = {{1, 2}};
  const auto one = ot0::MaskRevealedPaths(source, map);
  assert(one.width == 8 && one.height == 8);
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 8; ++x)
      assert((one.rgba[(y * 8 + x) * 4 + 3] != 0) ==
             (x >= 4 && x < 6 && y >= 4 && y < 6));
  // Noninteger neighboring cells must neither leak hidden texel centers nor
  // leave a fabricated grid seam. Check against inverse native coordinates.
  map.scale = {.7, .7};
  map.revealed_cells = {{1, 1}, {1, 2}, {2, 1}, {2, 2}};
  const auto joined = ot0::MaskRevealedPaths(source, map);
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 8; ++x) {
      const auto world = ot0::TextureToWorld(map, x + .5, y + .5);
      const auto cell = ot0::WorldCell(map, world.x, world.y);
      const bool allowed =
          world.x >= 0 && world.y >= 0 && world.x < 8 && world.y < 8 &&
          std::find(map.revealed_cells.begin(), map.revealed_cells.end(),
                    cell) != map.revealed_cells.end();
      assert((joined.rgba[(y * 8 + x) * 4 + 3] != 0) == allowed);
    }
  const auto empty = [&] {
    const auto im = ot0::MaskRevealedPaths(source, map);
    assert(std::none_of(im.rgba.begin(), im.rgba.end(),
                        [](auto v) { return v != 0; }));
  };
  map.revealed_cells.clear();
  empty();
  map.revealed_cells = {{100, 100}};
  empty();
  map.revealed_cells = {{1, 2}};
  map.valid = false;
  empty();
  map.valid = true;
  map.scale.x = std::numeric_limits<double>::quiet_NaN();
  empty();
  assert(source.rgba == std::vector<ot0::u8>(8 * 8 * 4, 255));
}
void Geometry() {
  for (unsigned bits = 0; bits < 256; ++bits) {
    ot0::Snapshot party;
    party.state = 23;
    party.valid = true;
    unsigned count = 0;
    for (unsigned i = 0; i < 8; ++i)
      if (bits & (1u << i)) {
        party.members[i] = {900 + i, 1, 300, 45, true};
        ++count;
      }
    const auto layout = ot0::ComputeFormationLayout(party);
    assert(layout.solo == (count == 1));
    for (unsigned i = 0; i < 8; ++i) {
      const auto r = layout.slots[i];
      assert(r.on == party.members[i].available);
      if (r.on)
        assert(r.w >= 112 && r.h >= 112 && r.x >= 0 && r.y >= 0 &&
               r.x + r.w <= 1240 && r.y + r.h <= 956);
      if (count > 4)
        assert(r.x == (i < 4 ? 590 : 912) && r.y == 84 + int(i % 4) * 222);
    }
    party.valid = false;
    const auto cleared = ot0::ComputeFormationLayout(party);
    for (auto r : cleared.slots)
      assert(!r.on);
  }
}
void NativeReveal() {
  const ot0::Image source{8, 8, std::vector<ot0::u8>(8 * 8 * 4, 255)};
  ot0::ExplorationSnapshot map;
  map.valid = true;
  map.block_width = 2;
  map.bounds_max = {8, 8, 1};
  map.axis = {1, -1};
  map.scale = {1, 1};
  map.reveal_texture_world_size = 8;
  map.reveal_stamp_world_size = 2;
  map.revealed_cells = {{1, 2}};
  ot0::Image stamp{1, 1, {0, 0, 0, 128}};
  auto revealed = ot0::MaskRevealedPaths(source, map, &stamp);
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 8; ++x)
      assert(revealed.rgba[(y * 8 + x) * 4 + 3] ==
             (x >= 4 && x < 6 && y >= 4 && y < 6 ? 128 : 0));
  // AlphaHoldout multiplies remaining alpha; it neither adds stamp alpha
  // nor substitutes the mask's black RGB for the native path colour.
  map.revealed_cells.push_back({1, 2});
  revealed = ot0::MaskRevealedPaths(source, map, &stamp);
  assert(revealed.rgba[(4 * 8 + 4) * 4 + 3] == 192);
  assert(revealed.rgba[(4 * 8 + 4) * 4] == 255);
  map.revealed_cells.resize(1);
  stamp = {2, 2, {0,0,0,0, 0,0,0,64, 0,0,0,128, 0,0,0,255}};
  revealed = ot0::MaskRevealedPaths(source, map, &stamp);
  assert(revealed.rgba[(4 * 8 + 4) * 4 + 3] == 0);
  assert(revealed.rgba[(4 * 8 + 5) * 4 + 3] == 64);
  assert(revealed.rgba[(5 * 8 + 4) * 4 + 3] == 128);
  assert(revealed.rgba[(5 * 8 + 5) * 4 + 3] == 255);
  map.revealed_cells.clear();
  revealed = ot0::MaskRevealedPaths(source, map, &stamp);
  assert(std::none_of(revealed.rgba.begin(), revealed.rgba.end(),
                      [](auto v) { return v != 0; }));
  map.filter_on = false;
  assert(ot0::MaskRevealedPaths(source, map, &stamp).rgba == source.rgba);
  map.filter_on = true;
  map.block_width = 8;
  map.reveal_stamp_world_size = 8;
  map.revealed_cells.assign(8192, {0, 0});
  stamp = {1, 1, {0, 0, 0, 255}};
  assert(ot0::MaskRevealedPaths(source, map, &stamp).rgba == source.rgba);
}
void ProtagonistAliases() {
  const char *path = std::getenv("OT0_PAK");
  if (!path)
    return;
  std::ifstream pak(path, std::ios::binary);
  assert(pak);
  const ot0::RangeReader read = [&](ot0::u64 at, void *out, std::size_t n) {
    pak.clear();
    pak.seekg(at);
    pak.read(static_cast<char *>(out), n);
    return std::size_t(pak.gcount()) == n;
  };
  ot0::ScreenComposer composer;
  ot0::Snapshot party;
  party.valid = true;
  party.state = 23;
  party.members[0] = {901, 1, 300, 45, true};
  const auto reference = composer.Compose(read, party, 0);
  assert(reference);
  // Party page (owner 2026-10-31): status pane on the left shows the selected member's native
  // name and values in the game font; every job alias renders the same page.
  unsigned ink = 0;
  for (unsigned y = 126; y < 190; ++y)
    for (unsigned x = 44; x < 560; ++x) {
      const auto at = (y * 1240 + x) * 4;
      ink += reference->rgba[at] > 200 && reference->rgba[at + 1] > 200 && reference->rgba[at + 2] > 200;
    }
  assert(ink > 300);
  // Footer settings square (1112..1224 x 956..1068): native Miscellaneous glyph drawn inside.
  unsigned glyph = 0;
  for (unsigned y = 986; y < 1038; ++y)
    for (unsigned x = 1142; x < 1194; ++x) {
      const auto at = (y * 1240 + x) * 4;
      glyph += reference->rgba[at] > 150 && reference->rgba[at + 1] > 150 && reference->rgba[at + 2] > 150;
    }
  assert(glyph > 60);
  if (const char *out = std::getenv("OT0_PARTY_PPM")) {
    std::ofstream f(out, std::ios::binary);
    f << "P6\n1240 1080\n255\n";
    for (std::size_t i = 0; i < reference->rgba.size(); i += 4) f.write(reinterpret_cast<const char *>(&reference->rgba[i]), 3);
  }
  for (unsigned id = 900; id <= 909; ++id) {
    party.members[0].id = id;
    const auto alias = composer.Compose(read, party, 0);
    assert(alias && alias->rgba == reference->rgba);
  }
}
void BattlePage() {
  const char *path = std::getenv("OT0_PAK");
  if (!path)
    return;
  std::ifstream pak(path, std::ios::binary);
  assert(pak);
  const ot0::RangeReader read = [&](ot0::u64 at, void *out, std::size_t n) {
    pak.clear();
    pak.seekg(at);
    pak.read(static_cast<char *>(out), n);
    return std::size_t(pak.gcount()) == n;
  };
  ot0::Snapshot party;
  party.valid = true;
  party.state = 26;
  const ot0::u32 ids[8] = {900, 10503, 10905, 10101, 10804, 40204, 11407, 0};
  for (int i = 0; i < 7; ++i) party.members[i] = {ids[i], 14, 1, 1, true};
  // Live 2026-10-25 Aristocrat fight: foe 0 at shield 1 after a dagger hit,
  // Polearm/Dagger/Tome/Fire revealed.
  ot0::BattleSnapshot b;
  b.valid = true;
  b.enemy_count = 2;
  for (int i = 0; i < 2; ++i) b.enemies[i] = {32312, 0x8c, 0x2, i == 0 ? 1 : 2, true, 226, 226, 73};
  const int hp[7] = {727, 558, 857, 791, 908, 850, 764}, sp[7] = {103, 68, 60, 60, 68, 119, 80};
  const int max_hp[7] = {765,747,857,908,908,850,764};
  b.member_count = 7;
  for (int i = 0; i < 7; ++i) {
    b.members[i] = {ids[i] == 900 ? 904u : ids[i], hp[i], sp[i], 1};
    b.members[i].max_hp=max_hp[i];b.members[i].max_sp=sp[i];
  }
  // Battle record keeps the job alias (904) while the formation has 900.
  b.now_count = 6;
  b.now = {{{false, 10503}, {false, 10101}, {false, 900}, {false, 10905}, {true, 0}, {true, 1}}};
  b.now_index = 1;
  b.command_actor_id=10101; // Explicit selected command actor; queue alone is insufficient.
  b.members[0].weapons = {108001, 106006};
  b.members[1].weapons = {103001, 101001};
  b.members[2].weapons = {105003, 104002};
  b.members[3].weapons = {101100, 102001};
  b.next_count = 6;
  b.next = {{{false, 10101}, {true, 0}, {false, 10503}, {false, 900}, {false, 10905}, {true, 1}}};
  ot0::ScreenComposer composer;
  std::string why;
  const auto image = composer.Compose(read, party, 0, &why, nullptr, false, nullptr, &b, 0);
  if (!image) std::cerr << "battle compose failed: " << why << "\n";
  assert(image && image->width == 1240 && image->height == 1080);
  // UI fixtures exercise selection transitions and pending-value geometry.
  // Supplied numbers here are renderer oracles, never claimed as live ACKs.
  {
    constexpr ot0::MemberId ids[]{ot0::MemberId::CharaStatusDb,ot0::MemberId::AilmentTypeDb,ot0::MemberId::TextUI,ot0::MemberId::TextSkill};
    std::array<std::optional<std::vector<ot0::u8>>,4> tables;
    for(std::size_t i=0;i<4;++i){tables[i]=ot0::ReadMember(read,ot0::Member(ids[i]),&why);assert(tables[i]);}
    const auto metadata=ot0::DecodeBattleMetadata(*tables[0],*tables[1],*tables[2],*tables[3]);assert(metadata.valid);
    ot0::BattleSelection selection;selection.valid=true;selection.phase=ot0::BattleSelectionPhase::Command;selection.actor_id=904;
    ot0::BattleStatsSnapshot stats;stats.valid=true;
    for(std::size_t i=0;i<7;++i) {
      stats.party[i].id=b.members[i].id;stats.party[i].bound=true;
      stats.party[i].weapon_context_valid=true;stats.party[i].equipment_weapon_slot=0;
      for(std::size_t p=0;p<8;++p)stats.party[i].parameters[p]={static_cast<int>(100+p),true};
    }
    for(std::size_t i=0;i<2;++i) {
      stats.enemies[i].id=b.enemies[i].enemy_id;stats.enemies[i].bound=true;
      for(std::size_t p=0;p<8;++p)stats.enemies[i].parameters[p]={static_cast<int>(200+p),true};
    }
    ot0::BattleStatusSnapshot statuses;statuses.valid=true;
    for(auto [type,suffix]:{std::pair{21u,"Down_Provoke"},std::pair{129u,"Down_FireUp"},std::pair{107u,"Up_PDefense"}}) {
      const auto* info=metadata.FindAilment(type);assert(info);
      const std::string name="UiTX_Battle_IconStatus_"+std::string(suffix)+"_png";
      ot0::VisibleBattleStatus status;status.type_id=type;status.name=info->name;status.counter=3;status.counter_valid=true;
      status.texture_path="/Game/UI/Battle/Texture/Frames/"+name+"."+name;
      statuses.party[0].entries.push_back(status);statuses.enemies[0].entries.push_back(status);
    }
    statuses.party[0].id=904;statuses.party[0].valid=true;statuses.enemies[0].id=32312;statuses.enemies[0].valid=true;
    ot0::BattleDetailView detail{&selection,&stats,&metadata,&statuses,true};
    auto detail_battle=b;detail_battle.command_actor_id=904;
    ot0::ProtagonistAppearance detail_appearance{1,4,1,13,1,0,0,0,1};detail_appearance.actor_chara_id=9004;
    const auto render=[&](const char* name) {
      ot0::ScreenVisuals fx;
      const auto frame=composer.Compose(read,party,0,&why,nullptr,false,&detail_appearance,&detail_battle,0,false,nullptr,&fx,nullptr,&detail);
      assert(frame);
      if(selection.phase==ot0::BattleSelectionPhase::EnemyTarget && detail.automatic)
        for(const auto& pulse:fx.pulses)assert(pulse.x<642 || pulse.y<492);
      if(const char* directory=std::getenv("OT0_DETAIL_PREVIEW_DIR")) {
        std::ofstream f(std::string(directory)+"/"+name+".ppm",std::ios::binary);assert(f);
        f<<"P6\n1240 1080\n255\n";for(std::size_t i=0;i<frame->rgba.size();i+=4)f.write(reinterpret_cast<const char*>(frame->rgba.data()+i),3);
      }
      return frame;
    };
    const auto command=render("command");assert(command->rgba!=image->rgba);
    selection.phase=ot0::BattleSelectionPhase::EnemyTarget;selection.enemy_index=0;
    const auto enemy=render("enemy-target");assert(enemy->rgba!=command->rgba);
    for(auto& parameter:stats.enemies[0].parameters)parameter.valid=false;
    const auto pending=render("enemy-pending");assert(pending->rgba!=enemy->rgba);
    // Long real native labels and large original frames must stay in-column.
    const auto bytes=[&](ot0::MemberId id){const auto data=ot0::ReadMember(read,ot0::Member(id),&why);assert(data);return *data;};
    const auto enemies=ot0::DecodeEnemies(bytes(ot0::MemberId::EnemyDb),bytes(ot0::MemberId::EnemyTypeDb),bytes(ot0::MemberId::EnemyAnimDb),bytes(ot0::MemberId::EnemyTexDb),bytes(ot0::MemberId::EnemyWeakDb),bytes(ot0::MemberId::TextEnemy));
    bool long_found=false;
    for(const auto& [id,info]:enemies.rows)if(info.name=="Bard Who Sings of a Dragon Slayer") {
      detail_battle.enemies[0].enemy_id=id;stats.enemies[0].id=id;statuses.enemies[0].id=id;
      render("long-enemy-name");long_found=true;break;
    }
    assert(long_found);detail_battle.enemies[0]=b.enemies[0];stats.enemies[0].id=32312;statuses.enemies[0].id=32312;
    selection.phase=ot0::BattleSelectionPhase::AllyTarget;selection.ally_id=904;
    render("ally-target");
    detail.automatic=false;render("overview");
    selection.phase=ot0::BattleSelectionPhase::GroupTargets;detail.automatic=true;
    const auto group=render("group");assert(group);
  }
  // The focused foe's two foe rows: focus 0 large, foe 1 as one compact row.
  const auto layout = ot0::ComputeBattleLayout(b, 0);
  assert(layout.focus == 0 && layout.other_count == 1 && layout.others[0] == 1);
  // The Leader's pair panel (642,154..) must not be empty: digits drawn right of x=700.
  unsigned ink = 0;
  for (unsigned y = 240; y < 300; ++y)
    for (unsigned x = 900; x < 1000; ++x) ink += image->rgba[(y * 1240 + x) * 4] > 200;
  assert(ink > 50);
  const auto gauge=(273*1240+822)*4;
  assert(image->rgba[gauge]==90 && image->rgba[gauge+1]==140 && image->rgba[gauge+2]==93);
  auto unknown=b;unknown.members[0].max_hp=0;
  const auto unknown_image=composer.Compose(read,party,0,&why,nullptr,false,nullptr,&unknown,0);
  assert(unknown_image && unknown_image->rgba[gauge+1]!=140);
  // A defeated focused foe moves focus to the next living foe.
  auto dead = b;
  dead.enemies[0].alive = false;
  assert(ot0::ComputeBattleLayout(dead, 0).focus == 1);
  for(auto& enemy:dead.enemies)enemy.alive=false;
  const auto idle=composer.Compose(read,party,0,&why,nullptr,false,nullptr,&dead,0);
  assert(idle && idle->rgba!=image->rgba);
  auto defeated=b;for(auto& member:defeated.members)member.hp=0;
  const auto defeat=composer.Compose(read,party,0,&why,nullptr,false,nullptr,&defeated,0);
  assert(defeat && defeat->rgba==idle->rgba);
  const auto modal=composer.Compose(read,party,0,&why,nullptr,false,nullptr,&b,0,true);
  assert(modal && modal->rgba==idle->rgba);
  if (const char *out = std::getenv("OT0_BATTLE_PPM")) {
    ot0::ProtagonistAppearance appearance{1,4,1,13,1,0,0,0,1};appearance.actor_chara_id=9004;
    const auto native=composer.Compose(read,party,0,&why,nullptr,false,&appearance,&b,0);
    assert(native);
    std::ofstream f(out, std::ios::binary);
    f << "P6\n1240 1080\n255\n";
    for (std::size_t i = 0; i < native->rgba.size(); i += 4) f.write(reinterpret_cast<const char *>(&native->rgba[i]), 3);
  }
  if(const char* out=std::getenv("OT0_CROWD_BATTLE_PPM")) {
    auto crowd=b;crowd.enemy_count=6;
    for(int i=2;i<6;++i)crowd.enemies[i]=b.enemies[i%2];
    const auto frame=composer.Compose(read,party,0,&why,nullptr,false,nullptr,&crowd,0);
    assert(frame);
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(std::size_t i=0;i<frame->rgba.size();i+=4)f.write(reinterpret_cast<const char*>(frame->rgba.data()+i),3);
  }
  if(const char* out=std::getenv("OT0_SOLO_BATTLE_PPM")) {
    auto solo=party;for(unsigned i=1;i<8;++i)solo.members[i]={};
    auto one=b;one.member_count=1;
    one.command_actor_id=900;
    one.now_count=3;one.now_index=0;one.now={{{false,900},{true,0},{true,1}}};
    one.next_count=3;one.next=one.now;
    const auto frame=composer.Compose(read,solo,0,&why,nullptr,false,nullptr,&one,0);
    assert(frame);
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(std::size_t i=0;i<frame->rgba.size();i+=4)f.write(reinterpret_cast<const char*>(frame->rgba.data()+i),3);
  }
  const auto settings_on=composer.ComposeSettings(read,true,true,false,&why);
  const auto settings_off=composer.ComposeSettings(read,false,false,true,&why);
  assert(settings_on && settings_off && settings_on->rgba!=settings_off->rgba);
  assert(settings_on->width==1240 && settings_on->height==1080);
  if (const char *out=std::getenv("OT0_SETTINGS_PPM")) {
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(std::size_t i=0;i<settings_on->rgba.size();i+=4)
      f.write(reinterpret_cast<const char*>(settings_on->rgba.data()+i),3);
  }
  if(const char* out=std::getenv("OT0_MAP_STRIP_PPM")) {
    auto stress=party;stress.state=23;
    stress.members[7]=stress.members[4]; // Explicit synthetic eight-column fit case.
    for(auto& member:stress.members) {member.available=true;member.hp=99999;member.sp=99999;}
    const auto frame=composer.Compose(read,stress,0,&why,nullptr,true);
    assert(frame);
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(std::size_t i=0;i<frame->rgba.size();i+=4)f.write(reinterpret_cast<const char*>(frame->rgba.data()+i),3);
  }
  if(const char* out=std::getenv("OT0_TOWN_PPM")) {
    const RecordedMemory m(std::getenv("OT0_TOWN_MEMORY"));
    const RecordedMemory mi(std::getenv("OT0_MINIMAP_MEMORY"));
    const auto map=ot0::ReadExploration(m.reader(),m.main);
    const auto icons=ot0::ReadMinimapIcons(mi.reader(),mi.main,0,&map);
    if(!map.valid || !icons.valid)std::cerr<<"Town mock map="<<map.valid<<"/"<<map.map_id<<" icons="<<icons.valid<<"/"<<icons.map_id<<"\n";
    assert(map.valid && map.map_id==46 && icons.valid && icons.map_id==46);
    auto field=party;field.state=23;
    for(int i=0;i<7;++i) {field.members[i].hp=hp[i];field.members[i].sp=sp[i];}
    // Captured owned seven-member male Scholar appearance, not a guessed face.
    ot0::ProtagonistAppearance appearance{1,4,1,13,1,0,0,0,1};appearance.actor_chara_id=9004;
    appearance.childhood_body=19;appearance.childhood_hair=21;appearance.childhood_face=5;
    ot0::ScreenVisuals map_visuals;
    const auto frame=composer.Compose(read,field,0,&why,&map,true,&appearance,nullptr,0,false,nullptr,&map_visuals,&icons);
    std::cerr<<"Map preview icons="<<icons.icons.size()
             <<" reader_complete="<<icons.complete<<" rendered_complete="<<map_visuals.map_complete<<"\n";
    if(!frame)std::cerr<<why<<"\n";assert(frame);
    if(const char* expected=std::getenv("OT0_EXPECT_MAP_COMPLETE")) {
      assert(map_visuals.map_complete==(std::string(expected)=="1"));
      if(map_visuals.map_complete && !icons.icons.empty()) {
        auto unsupported=icons;unsupported.icons.back().frame_path="/Unsupported/NativeGlyph";
        ot0::ScreenVisuals refused;
        assert(composer.Compose(read,field,0,&why,&map,true,&appearance,nullptr,0,false,nullptr,&refused,&unsupported));
        assert(!refused.map_complete); // Unknown art must not authorize top-map hiding.
      }
    }
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(size_t i=0;i<frame->rgba.size();i+=4)f.write(reinterpret_cast<const char*>(frame->rgba.data()+i),3);
  }
  party.state=25;
  ot0::ShopComparisonSnapshot shop;
  shop.valid=true;shop.candidate_item_id=101002;shop.candidate_name="Silver Sword";
  shop.selected_member_id=900;
  for(unsigned i=0;i<7;++i) {
    auto& member=shop.members[i];member.id=ids[i];member.valid=true;member.stat_count=4;
    member.stats={{{1,"Phys. Atk.",115+static_cast<int>(i),128},
                   {2,"Elem. Atk.",106,98}, {3,"Phys. Def.",72,72}, {4,"Elem. Def.",67,67}}};
  }
  const auto comparison=composer.Compose(read,party,1,&why,nullptr,false,nullptr,nullptr,0,false,&shop);
  assert(comparison);
  // Companion selection stays independent of the native shop cursor. Reading
  // another party member must change the image without changing the source.
  const auto other=composer.Compose(read,party,2,&why,nullptr,false,nullptr,nullptr,0,false,&shop);
  assert(other && comparison->rgba!=other->rgba && shop.selected_member_id==900);
  shop.valid=false;
  const auto closed=composer.Compose(read,party,1,&why,nullptr,false,nullptr,nullptr,0,false,&shop);
  const auto formation=composer.Compose(read,party,1);
  assert(closed && formation && closed->rgba==formation->rgba);
  if(const char* out=std::getenv("OT0_SHOP_PPM")) {
    std::ofstream f(out,std::ios::binary);f<<"P6\n1240 1080\n255\n";
    for(std::size_t i=0;i<comparison->rgba.size();i+=4)
      f.write(reinterpret_cast<const char*>(comparison->rgba.data()+i),3);
  }
}
} // namespace
int main() {
  Mask();
  Geometry();
  NativeReveal();
  ProtagonistAliases();
  BattlePage();
  std::cout << "Revealed texel boundaries, empty masks and 256 party layouts "
               "passed\n";
}

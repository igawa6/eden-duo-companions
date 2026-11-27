// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acnh_tex.h"
#include <chrono>
#include <thread>
#include "core/mods/dsmod_module_extensions.h"
#include "pla_assets.h"
#include "pla_dock.h"
#include "pla_inventory_page.h"
#include "pla_metadata.h"
#include "pla_map_art.h"
#include "pla_missions.h"
#include "pla_hud.h"
#include "pla_battle.h"
#include "pla_pokedex.h"
#include "pla_party.h"
#include "pla_packed_ui.h"
#include "pla_pokemon_page.h"
#include "pla_font.h"
#include "pla_reader.h"
#include "pla_select.h"
#include "pla_text.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <vector>

namespace {
struct Fake {
    std::map<uint64_t, std::vector<uint8_t>> memory;
    std::map<std::string, double> output;
    unsigned reads = 0;
    size_t romfs_reads = 0;
    bool fail = false;
    uint64_t mutate_address = 0;
    int64_t bag_filter = 0, bag_item = 0, bag_key = 0;
    int64_t area_index = 0, follow = 1, zoom = 1, tap_select = 1, aux_present = 1;
    int64_t pokemon_slot = 0, pokemon_tab = 0, pokemon_move = 0, missions_request = 0;
    std::map<std::string, std::string> texts;
    template <class T> void Put(uint64_t address, const T &value) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
        memory[address] = {bytes, bytes + sizeof value};
    }
};
EdenDsmodHostApi Host(Fake &f) {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.struct_size = sizeof h;
    h.title_id = pla::TitleId;
    h.userdata = &f;
    std::memcpy(h.build_id, pla::BuildId, 32);
    h.main_base = 0x8000000;
    h.main_size = 0x5000000;
    h.is_mapped = [](void *, uint64_t at, uint64_t size) -> EdenDsmodBool {
        return at >= 0x10000 && at < 0x10000000 && size <= 0x10000000 - at;
    };
    h.read_memory = [](void *u, uint64_t at, void *out, size_t size) -> EdenDsmodBool {
        auto &f = *static_cast<Fake *>(u);
        ++f.reads;
        auto it = f.memory.find(at);
        if (f.fail || it == f.memory.end() || it->second.size() != size)
            return false;
        std::memcpy(out, it->second.data(), size);
        if (at == f.mutate_address && size > 2) it->second[2] ^= 1;
        return true;
    };
    h.publish_i64 = [](void *u, const char *name, int64_t v) {
        static_cast<Fake *>(u)->output[name] = v;
    };
    h.get_i64 = [](void *u, const char *key, int64_t fallback) -> int64_t {
        const auto &f = *static_cast<Fake *>(u);
        if (std::string_view(key) == "@flag:pla.missions_request") return f.missions_request;
        if (std::string_view(key) == "@flag:pla.pokemon_slot") return f.pokemon_slot;
        if (std::string_view(key) == "@flag:pla.pokemon_tab") return f.pokemon_tab;
        if (std::string_view(key) == "@flag:pla.pokemon_move") return f.pokemon_move;
        if (std::string_view(key) == "@flag:pla.bag_key") return f.bag_key;
        if (std::string_view(key) == "@flag:pla.bag_filter") return f.bag_filter;
        if (std::string_view(key) == "@flag:pla.bag_item") return f.bag_item;
        if (std::string_view(key) == "@flag:pla.area_override")
            return f.area_index;
        if (std::string_view(key) == "__aux_present")
            return f.aux_present;
        if (std::string_view(key) == "@flag:pla.tap_select")
            return f.tap_select;
        if (std::string_view(key) == "@flag:pla.follow")
            return f.follow;
        return fallback;
    };
    h.get_text = [](void *u, const char *name) -> const char * {
        auto &texts=static_cast<Fake *>(u)->texts;
        const auto it=texts.find(name);
        return it==texts.end() ? nullptr : it->second.c_str();
    };
    h.publish_text = [](void *u, const char *name, const char *value) {
        static_cast<Fake *>(u)->texts[name] = value;
    };
    h.publish_f64 = [](void *u, const char *name, double v) {
        static_cast<Fake *>(u)->output[name] = v;
    };
    return h;
}
// Synthetic turn battle: roster chain, actors (own 0, foe 12), active slots and a 6-entry order.
// Throw selector view in the battle state (+5B4 = 0: field HUD off), same route as the
// blocks test's view traversal.
void BattleSelectorFixture(Fake &f, const EdenDsmodHostApi &h) {
    f.Put(h.main_base + 0x42a8b00, uint64_t(0x3300000));
    f.Put(uint64_t(0x3300050), uint64_t(0x3310000));
    f.Put(uint64_t(0x3310030), uint64_t(0x3320000));
    f.Put(uint64_t(0x33204c8), uint64_t(0x3330000));
    f.Put(uint64_t(0x3330070), uint64_t(0x3340000));
    f.Put(uint64_t(0x3330078), uint64_t(0x3340010));
    f.Put(uint64_t(0x3340000), uint64_t(0x3350000));
    f.Put(uint64_t(0x3350000), uint64_t(0));
    f.Put(uint64_t(0x3340008), uint64_t(0x3360000));
    f.Put(uint64_t(0x3360000), h.main_base + 0x415d618);
    const uint64_t v = 0x3360000, ib = 0x3370000, pb = 0x3380000;
    f.Put(v + 0x750, unsigned(0)); f.Put(v + 0x704, unsigned(0)); f.Put(v + 0x5b4, uint8_t(0));
    f.Put(v + 0x5ac, unsigned(1)); f.Put(v + 0x5c0, ib); f.Put(v + 0x5c8, ib + 12);
    f.Put(v + 0x630, pb); f.Put(v + 0x638, pb + 16); f.Put(v + 0x6f8, unsigned(1)); f.Put(v + 0x6fc, unsigned(0));
    const std::array<pla::Item, 2> items{{{1710, 294, 1}, {1711, 515, 1}}};
    f.Put(ib, items);
    std::array<uint64_t, 2> slots{5, 2};
    f.Put(pb, slots);
}

void BattleFixture(Fake &f, const EdenDsmodHostApi &h, unsigned own_species, unsigned foe_species) {
    const uint64_t root = 0x3200000, one = 0x3210000, ctx = 0x3220000, roster = 0x3230000;
    f.Put(h.main_base + 0x42A6F00, root);
    f.Put(root + 0xB0, one); f.Put(one + 0xE0, ctx); f.Put(ctx + 0xD0, roster);
    f.Put(one, h.main_base + 0x40C27E0); f.Put(ctx, h.main_base + 0x40B76B0); f.Put(roster, h.main_base + 0x40B6880);
    f.Put(roster + 0x1A0, uint32_t(2));
    const unsigned species[2] = {own_species, foe_species}, ids[2] = {0, 12};
    uint64_t bs[2], as[2];
    for (unsigned n = 0; n < 2; ++n) {
        const uint64_t a = 0x3240000 + n * 0x1000, b = 0x3260000 + n * 0x1000;
        as[n] = a; bs[n] = b;
        f.Put(roster + 0xB0 + 8 * n, a);
        f.Put(a, h.main_base + 0x40C2098);
        f.Put(a + 0x70, b);
        std::vector<uint8_t> raw(0x310);
        const auto w16 = [&](size_t at, unsigned v) { raw[at - 0x60] = uint8_t(v); raw[at - 0x5F] = uint8_t(v >> 8); };
        w16(0x70, species[n]); w16(0x72, 200); w16(0x74, n ? 50 : 180);
        raw[0x7C - 0x60] = n ? 68 : 100; raw[0x7D - 0x60] = uint8_t(ids[n]);
        raw[0x368 - 0x60] = 2;
        if (n) raw[0x90 + 8 * 4 - 0x60] = uint8_t(1 | 12 << 3); // foe burned (sick id 4)
        const unsigned moves[2] = {33, 52};
        for (unsigned m = 0; m < 2; ++m) {
            const size_t at = 0x300 + 0x1C * m - 0x60;
            raw[at] = uint8_t(moves[m]); raw[at + 4] = 10; raw[at + 5] = 15;
        }
        f.memory[b + 0x60] = raw;
    }
    const uint64_t z = 0x3280000, y = 0x3290000, fp = 0x32A0000, fa = 0x32B0000, tm = 0x32C0000;
    f.Put(roster + 0x68, z); f.Put(z + 0x98, y); f.Put(y + 0x370, fp); f.Put(fp + 8, fa);
    f.Put(fa + 0x08, bs[0]); f.Put(fa + 0x40, bs[1]);
    f.Put(ctx + 0x110, tm);
    for (unsigned n = 0; n < 6; ++n) f.Put(tm + 0x70 + 8 * n, as[n % 2]);
}

void Chain(Fake &f, const EdenDsmodHostApi &h) {
    uint64_t ptr = h.main_base;
    const uint64_t offsets[] = {0x42F18E8, 0x88, 0x90, 0x1F0, 0x18, 0x80};
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t next = 0x100000 + i * 0x1000;
        f.Put(ptr + offsets[i], next);
        ptr = next;
    }
    const float xyz[] = {214.604f, 303.753f, 81.316f};
    f.Put(ptr + 0x90, xyz);
}
std::vector<uint8_t> Read(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void NativePartyChain(Fake &f, const EdenDsmodHostApi &h, std::span<const uint8_t> bytes) {
    assert(bytes.size() >= 0x8d1 && bytes[0x8d0] <= 6);
    f.Put(h.main_base+0x42a7000,uint64_t(0x300000));
    f.Put(0x3000d0,uint64_t(0x310000));
    f.Put(0x310058,uint64_t(0x320000));
    f.Put(0x320000,h.main_base+0x418ad18);
    f.Put(0x320088,uint8_t(bytes[0x8d0]));
    std::array<uint64_t,6> slots{};
    for (unsigned j=0; j<bytes[0x8d0]; ++j) {
        const uint64_t pokemon=0x400000+j*0x1000, access=pokemon+0x200;
        const uint64_t core=0x500000+j*0x1000, tail=core+0x200;
        slots[j]=pokemon;
        f.Put(pokemon+0x98,access);
        f.Put(access+8,tail);f.Put(access+0x10,core);
        f.Put(access+0x18,uint8_t(1));f.Put(access+0x19,uint8_t(0));
        f.Put(access+0x30,uint64_t(0));
        const auto slot=bytes.subspan(j*0x178,0x178);
        f.memory[core]={slot.begin(),slot.begin()+0x168};
        f.memory[tail]={slot.begin()+0x168,slot.end()};
    }
    f.Put(0x320058,slots);
}
void ImageSink(void *u, uint32_t w, uint32_t h, const uint8_t *data, size_t size) {
    assert(data && size == size_t(w) * h * 4);
    *static_cast<size_t *>(u) = size;
}
void FontSink(void *u, uint32_t cap, uint32_t first, const EdenDsmodFontGlyph *glyphs,
              uint32_t count) {
    assert(cap == 25 && first == 32 && count == 0x203b - 32);
    assert(glyphs['H' - 32].h == 23);
    int level_width = 0;
    for (unsigned char cp : std::string_view("Lv. 100")) level_width += glyphs[cp - first].advance;
    std::cout << "Native Lv100 label width: " << level_width << "px\n";
    assert(level_width <= 100);
    for (unsigned cp : {0xe9u, 0xd7u, 0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201cu, 0x201du, 0x2026u,
                        0x152u, 0x153u})
        assert(glyphs[cp - 32].w && glyphs[cp - 32].advance);
    *static_cast<size_t *>(u) = count;
}
void TextureTest() {
    // A minimal 4x4 BC3 BNTX, linear tile, opaque red block. Exercises the reused
    // BNTX path.
    std::vector<uint8_t> b(0x240, 0);
    auto put = [&](size_t off, uint64_t v, unsigned n) {
        for (unsigned j = 0; j < n; ++j)
            b[off + j] = uint8_t(v >> (j * 8));
    };
    std::memcpy(b.data(), "BNTX", 4);
    put(0x1c, b.size(), 4);
    put(0x24, 1, 4);
    put(0x28, 0x40, 8);
    put(0x40, 0x80, 8);
    std::memcpy(b.data() + 0x80, "BRTI", 4);
    put(0x92, 1, 2);
    put(0x9c, 0x1c01, 4);
    put(0xa4, 4, 4);
    put(0xa8, 4, 4);
    put(0xb0, 1, 4);
    put(0xd0, 32, 4);
    put(0xd8, 0x05040302, 4);
    put(0xf0, 0x180, 8);
    put(0x180, 0x200, 8);
    b[0x200] = b[0x201] = 255;
    put(0x208, 0xf800, 2);
    acnh::Image image;
    assert(acnh::BntxDecode(b, {}, image));
    assert(image.w == 4 && image.h == 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            auto *p = image.At(x, y);
            assert(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
        }
    b.resize(0x100);
    assert(!acnh::BntxDecode(b, {}, image));
}
void MessageHashTests() {
    std::vector<uint8_t> dat(44,0), tbl(8,0);
    auto put=[](auto &bytes,size_t at,uint64_t value,unsigned n) {
        for (unsigned j=0;j<n;++j) bytes[at+j]=uint8_t(value>>(8*j));
    };
    put(dat,0,1,2);put(dat,2,2,2);put(dat,12,16,4);put(dat,16,28,4);
    for (unsigned row=0;row<2;++row) {
        put(dat,20+row*8,20+row*4,4);put(dat,24+row*8,2,2);
        const uint16_t key=uint16_t(0x7c89+row*0x2983);
        put(dat,36+row*4,('A'+row)^key,2);
        put(dat,38+row*4,uint16_t((key<<3)|(key>>13)),2);
    }
    put(tbl,0,0x42544841,4);put(tbl,4,2,4);
    constexpr uint64_t hashes[]{0x1111222233334444,0x5555666633334444};
    for (unsigned row=0;row<2;++row) {
        const size_t at=tbl.size();tbl.resize(at+12);
        put(tbl,at,hashes[row],8);put(tbl,at+8,2,2);tbl[at+10]='a'+row;
    }
    assert(pla::MessageByHash(dat,tbl,hashes[0])=="A");
    assert(pla::MessageByHash(dat,tbl,hashes[1])=="B");
    assert(pla::MessageByHash(dat,tbl,1).empty());
    assert(pla::MessageByHash(dat,std::span(tbl).first(tbl.size()-1),hashes[0]).empty());
    const auto duplicate=tbl;tbl.insert(tbl.end(),duplicate.begin()+8,duplicate.begin()+20);put(tbl,4,3,4);
    assert(pla::MessageByHash(dat,tbl,hashes[0]).empty());
}
#include "pla_blocks_test.h"
} // namespace
int main(int argc, char **argv) {
    MessageHashTests();
    BlocksTests();
    BlockDecodeTests();
    {
        assert(!pla::VillageMapVariant({}));
        // Independently compare every threshold-side value with the exact
        // native multiply/shift and branches, including the early zero case.
        for (uint32_t story = 0; story <= 70001; ++story) {
            const auto quotient = uint32_t((uint64_t(story) * 0xd1b71759) >> 45);
            const unsigned native = (story >> 4) < 0x271 ? 0 : story < 0x11170 ? quotient - 1 : 5;
            assert(pla::VillageMapVariant(story) == native);
        }
        assert(pla::VillageMapVariant(UINT32_MAX) == 5);
        assert(pla::VillageMapVariant(108000) == 5 && pla::VillageMapVariant(31600) == 2 &&
               pla::VillageMapVariant(60100) == 5);
        for (const auto *key : {"module:pla:map:5", "module:pla:map:5:6", "module:pla:map:6",
             "module:pla:map:0:1", "module:pla:map:5:00", "module:pla:map:5:-1",
             "module:pla:map:5:0suffix", "module:pla:map:", "module:pla:map:05"})
            assert(!pla::ParseMapArtKey(key));
        for (unsigned variant = 0; variant < 6; ++variant) {
            const auto key = "module:pla:map:5:" + std::to_string(variant);
            const auto parsed = pla::ParseMapArtKey(key);
            assert(parsed && parsed->area == 5 && parsed->village_variant == variant);
            assert(pla::MapArtPath(*parsed) == "bin/appli/map/map_texture/map_lmap_pic_l_05_0" +
                   std::to_string(variant) + ".bntx");
        }
        assert(pla::MapArtPath({5, {}}).empty() && pla::MapArtPath({0, 1}).empty());
        for (unsigned area = 0; area < 5; ++area) {
            const auto name = "ha_area0" + std::to_string(area + 1);
            const auto selected = pla::MapArtForAreaName(name, {});
            assert(selected && selected->area == area && !selected->village_variant);
        }
        assert(!pla::MapArtForAreaName("ha_area00", {}));
        const auto village = pla::MapArtForAreaName("ha_area00", 31600);
        assert(village && village->area == 5 && village->village_variant == 2);
        for (const auto *name : {"", "ha_area06", "ha_area07", "ha_area01_extra",
             "ha_area01/room", "HA_AREA01", " ha_area01", "ha_area01 "})
            assert(!pla::MapArtForAreaName(name, 108000));
    }
    {
        assert(pla::InventoryCapacity(39)==59 && pla::InventoryCapacity(40)==60 &&
               pla::InventoryCapacity(400)==420 && pla::InventoryCapacity(UINT32_MAX)==675);
        std::array<uint8_t, 400> pouch{};
        pouch[0] = 0xae; pouch[1] = 6; // item 1710
        pouch[2] = 0x26; pouch[3] = 1; // count 294
        auto items = pla::ParseInventory(pouch, 400);
        assert(items.valid && items.items.size() == 1 && items.items[0].id == 1710 &&
               items.items[0].count == 294);
        assert(!pla::ParseInventory(pouch, 2700).valid);
        pouch[4] = 1; // occupied slot with zero count is corrupt, not an empty row
        assert(!pla::ParseInventory(pouch, 400).valid);
        pouch.fill(0);
        assert(pla::ParseInventory(pouch, 400).valid &&
               pla::ParseInventory(pouch, 400).items.empty());
        pla::Inventory bag{true, 40, {{1710, 294, 0}, {1711, 515, 0}}};
        pla::Selector selector;
        selector.valid = true; selector.item_count = 1; selector.items[0] = {1711,515,0};
        Fake f; auto h = Host(f);
        f.bag_item = 1710;
        pla::PublishInventory(h, bag, selector);
        assert(f.output["pla.bag_count"] == 2 && f.output["pla.bag.0.id"] == 1710 &&
               f.output["pla.bag_capacity_valid"] && f.texts["pla.bag_capacity"]=="2/40");
        assert(f.output["pla.bag_cursor_valid"] && f.output["pla.bag_cursor_x"]==-18 &&
               f.output["pla.bag_cursor_y"]==128);
        f.bag_item=1711;pla::PublishInventory(h,bag,selector);
        assert(f.output["pla.bag_cursor_x"]==528 && f.output["pla.bag_cursor_y"]==128);
        f.bag_filter = 1;
        pla::PublishInventory(h, bag, selector);
        assert(f.output["pla.bag_count"] == 1 && f.output["pla.bag.0.id"] == 1711 &&
               f.output["pla.bag.1.id"] == 0);
        assert(f.texts["pla.bag_detail"] == "module:pla:bag_detail:1711:515:1");
        pla::PublishInventory(h, {}, selector);
        assert(!f.output["pla.bag_filter_available"] && !f.output["pla.bag_valid"] &&
               f.output["pla.bag_count"]==0 && f.texts["pla.bag_detail"].empty() &&
               f.texts["pla.bag.0.image"].empty() && !f.output["pla.bag_cursor_valid"] &&
               f.output["pla.bag_cursor_x"]==0 && f.output["pla.bag_cursor_y"]==0);
        pla::PublishInventory(h, bag, {});
        assert(f.output["pla.bag_valid"] == 0 && f.texts["pla.bag_detail"].empty());
        f.bag_key = 1;
        pla::PublishInventory(h, bag, {});
        assert(f.output["pla.bag_count"] == 2 && f.output["pla.bag_filter_available"] == 0 &&
               f.output["pla.bag_filter_on"] == 0 && !f.output["pla.bag_capacity_valid"] &&
               f.texts["pla.bag_capacity"].empty());
        f.bag_key=2;pla::PublishInventory(h,bag,selector);
        assert(f.output["pla.bag_recipes"] && !f.output["pla.bag_unavailable"] &&
               !f.output["pla.bag_valid"] && !f.output["pla.bag_empty"] &&
               !f.output["pla.bag_count"] && !f.output["pla.bag_cursor_valid"] &&
               !f.output["pla.bag_filter_available"] && !f.output["pla.bag_filter_on"] &&
               f.texts["pla.bag_capacity"].empty() && f.texts["pla.bag_detail"].empty() &&
               f.texts["pla.bag.0.image"].empty());
        pla::PublishInventory(h, {}, {});
        assert(f.output["pla.bag_valid"] == 0 && f.texts["pla.bag.0.image"].empty());
    }
    {
        // Pill height == 2*requested radius used to reverse clamp bounds.
        acnh::Image pill(160,48);
        pla::PokemonRound(pill,0,0,160,48,24,{51,77,103,255});
        assert(pill.At(80,0)[3]==255 && pill.At(0,0)[3]==0 &&
               pill.At(0,23)[3]==255 && pill.At(159,24)[3]==255);
        for (int y=0;y<48;++y)
            for (int x=0;x<160;++x)
                assert(pill.At(x,y)[3]==pill.At(x,47-y)[3]);
        const auto unchanged=pill.rgba;
        pla::PokemonRound(pill,0,0,0,48,24,{255,0,0,255});
        assert(pill.rgba==unchanged);
        acnh::Image pixel(1,1);
        pla::PokemonRound(pixel,0,0,1,1,99,{51,77,103,255});
        assert(pixel.At(0,0)[3]==255);
        pla::Party party;
        party.valid=true;party.count=2;
        party.slots[0].valid=true;party.slots[0].species=468;party.slots[0].hp=325;
        party.slots[1].valid=true;party.slots[1].species=571;party.slots[1].hp=262;
        const auto original=party;
        pla::PokemonPageSnapshot page;
        page.Update(party,0,0,0);
        const auto first=page.ImageKey();
        page.Update(party,0,0,0);assert(page.ImageKey()==first);
        page.Update(party,1,1,2);
        assert(page.ImageKey()!=first && page.slot==1 && page.tab==1 && page.move==2);
        assert(party==original); // inspection owns a copy, never edits game state
        Fake f;auto h=Host(f);
        pla::PublishPokemonPage(h,page);
        assert(f.output["pla.pokemon.valid"]==1 && f.output["pla.pokemon.moves"]==1 &&
               f.output["pla.pokemon.row.0"]==1 && f.output["pla.pokemon.row.2"]==0 && f.reads==0);
        party.slots[1].valid=false;page.Update(party,1,2,99);
        assert(page.slot==0 && page.move==3 && page.Valid());
        page.Update({},0,0,0);pla::PublishPokemonPage(h,page);
        assert(!page.Valid() && f.output["pla.pokemon.valid"]==0 &&
               f.output["pla.pokemon.row.0"]==0 && f.output["pla.pokemon.moves"]==0);
    }
    Pa8Tests();
    if (argc > 1 && std::string_view(argv[1]) == "--live-blocks") {
        const auto dir = std::getenv("PLA_LIVE_BLOCKS");
        if (!dir) return 77;
        Fake f;
        auto h = Host(f);
        const uint32_t keys[] = {0x02168706, 0x267dd9da, 0x2985fe5d, 0x717ddaa3, 0xf25c070e};
        constexpr uint64_t table = 0x300000;
        for (unsigned j = 0; j < 5; ++j) {
            char name[40];
            std::snprintf(name, sizeof name, "block-%08x.bin", keys[j]);
            auto bytes = Read(std::filesystem::path(dir) / name);
            const uint64_t address = 0x500000 + j * 0x40000;
            f.Put(table + j * 0x30, uint64_t(keys[j]));
            f.Put(table + j * 0x30 + 8, address);
            f.Put(table + j * 0x30 + 0x18, uint64_t(bytes.size()));
            f.memory[address] = bytes;
            const auto decoded = pla::DecodeBlock(bytes, keys[j]);
            assert(decoded.type == (j == 3 ? 11 : 4));
            if (j == 1 || j == 2) {
                const auto expected = Read(std::filesystem::path(dir) /
                                           (j == 1 ? "coords-payload.bin" : "party-payload.bin"));
                assert(!expected.empty() && decoded.payload == expected);
            }
        }
        const pla::Blocks store{table, table + 5 * 0x30};
        assert(pla::ReadArea(h, store) == 4);
        const auto party = pla::ReadParty(h, store);
        assert(party.valid && party.count == 6);
        constexpr unsigned expected[] = {468, 571, 503, 59, 901, 903};
        for (unsigned j = 0; j < 6; ++j)
            assert(party.slots[j].valid && party.slots[j].species == expected[j] &&
                   party.slots[j].level == 100);
        std::cout << "Private live blocks: exact decoded payloads, area04, six expected PA8; dex/status/quest headers checked\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--inventory-block") {
        const bool key_items = std::string_view(std::getenv("PLA_INVENTORY_KIND")) == "key";
        const auto bytes = Read(std::getenv("PLA_INVENTORY_BLOCK"));
        const auto inventory = pla::ParseInventory(bytes, key_items ? 400 : 2700);
        assert(inventory.valid && !inventory.items.empty());
        assert(inventory.items.size() == (key_items ? 42u : 15u));
        // Exercise the production encrypted-object/scalar reader too: F14's
        // upgrade value 40 must produce capacity60, not an unavailable label.
        Fake f; auto host = Host(f);
        const uint32_t key = key_items ? 0x59a4d0c3 : 0x9fe2790a;
        const uint64_t table = 0x300000;
        std::array<uint32_t,2> keys{key,0x75ce2cf6};
        std::sort(keys.begin(),keys.end());
        for (unsigned j=0;j<2;++j) {
            std::vector<uint8_t> blob;
            if (keys[j]==key) blob=ObjectBlob(bytes,key);
            else { blob={10,40,0,0,0}; pla::CryptBlock(blob,keys[j]); }
            const uint64_t addr=0x500000+j*0x10000;
            f.Put(table+j*0x30,uint64_t(keys[j]));
            f.Put(table+j*0x30+8,addr);
            f.Put(table+j*0x30+0x18,uint64_t(blob.size()));
            f.memory[addr]=blob;
        }
        if (!key_items) {
            f.Put(host.main_base+0x42a6ed8,uint64_t(0x210000));
            f.Put(0x210280,uint64_t(0x220000));
            f.memory[0x220068]=bytes;
            // Native value changes without rewriting the saved blob.
            --f.memory[0x220068][2];
        }
        const auto live=pla::ReadInventory(host,{table,table+2*0x30},key_items);
        assert(live.valid && live.items.size()==inventory.items.size());
        assert(live.capacity==(key_items ? 0u : 60u));
        if (!key_items) assert(live.items.front().count==293 && inventory.items.front().count==294);
        if (!key_items) {
            f.mutate_address=0x220068;
            assert(!pla::ReadInventory(host,{table,table+2*0x30},false).valid);
            f.mutate_address=0;
        }
        host.build_id[0]^=1; f.reads=0;
        assert(!pla::ReadInventory(host,{table,table+2*0x30},key_items).valid && f.reads==0);
        if (!key_items) {
            for (const auto &item : inventory.items)
                if (item.id == 1710) assert(item.count == 294);
        }
        std::cout << "Private PLA-F14 held inventory: valid " << (key_items ? "key" : "everyday")
                  << " pouch, no empty slots published\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--mission-defs") {
        const char *romfs=std::getenv("PLA_ROMFS");assert(romfs);
        std::cout << '[';
        bool comma=false;
        for (bool requests : {false,true}) {
            const auto root=std::filesystem::path(romfs)/"bin/event/event_progress";
            const auto bytes=Read(root/(requests ? "quest_list_sub.bin" : "quest_list_main.bin"));
            const auto defs=pla::ParseMissionDefinitions(bytes,requests);
            assert(defs.valid);
            assert(!pla::ParseMissionDefinitions(bytes,!requests).valid);
            for (size_t length : {size_t(0),size_t(7),bytes.size()/2})
                assert(!pla::ParseMissionDefinitions(std::span(bytes).first(length),requests).valid);
            auto corrupt=bytes;
            std::fill_n(corrupt.begin(),4,0xff);
            assert(!pla::ParseMissionDefinitions(corrupt,requests).valid);
            pla::FlatMetadata metadata(bytes);
            const auto tables=metadata.Tables(metadata.Read(0,4),0,128);
            const auto field=metadata.Field(tables.front(),8);
            const auto vector=field+metadata.Read(field,4);
            corrupt=bytes;
            std::fill_n(corrupt.begin()+vector,4,0xff);
            assert(!pla::ParseMissionDefinitions(corrupt,requests).valid);
            const auto messages=std::filesystem::path(romfs)/"bin/message/English/common";
            const std::string table=requests ? "questlist_sub" : "questlist_main";
            const auto dat=Read(messages/(table+".dat")), tbl=Read(messages/(table+".tbl"));
            for (const auto &q:defs.quests) {
                assert(!pla::MessageByHash(dat,tbl,q.title_hash).empty());
                assert(pla::ClassifyMission(q,false,255).status==pla::MissionStatus::Unavailable);
                assert(pla::ClassifyMission(q,true,0).status==pla::MissionStatus::NotReceived);
                assert(pla::ClassifyMission(q,true,1).status==pla::MissionStatus::NotReceived);
                assert(pla::ClassifyMission(q,true,255).status==pla::MissionStatus::Completed);
                for (const auto &step:q.steps) {
                    const auto state=pla::ClassifyMission(q,true,step.id);
                    if (step.id <= 1) {
                        assert(state.status==pla::MissionStatus::NotReceived);
                        assert(!state.purpose && !state.outline);
                    } else {
                        assert(state.status!=pla::MissionStatus::Unavailable);
                        assert(state.ordinal==unsigned(&step-q.steps.data()));
                        assert(state.outline==step.outline);
                        assert(state.purpose==(step.id==255?0:step.purpose));
                    }
                }
                unsigned missing=2;
                while (missing<255 && std::any_of(q.steps.begin(),q.steps.end(),[&](const auto &s){return s.id==missing;})) ++missing;
                assert(pla::ClassifyMission(q,true,uint8_t(missing)).status==pla::MissionStatus::Unavailable);
                if (comma) std::cout << ',';
                comma=true;
                std::cout << "{\"id\":" << q.id << ",\"request\":" << q.request <<
                    ",\"hash\":" << q.state_hash << ",\"title\":" << q.title_hash << ",\"steps\":[";
                for (size_t j=0;j<q.steps.size();++j) {
                    if (j) std::cout << ',';
                    const auto &s=q.steps[j];
                    std::cout << '[' << unsigned(s.id) << ',' << s.outline << ',' << s.purpose << ']';
                }
                std::cout << "]}";
            }
        }
        std::cout << "]\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--native-split") {
        const char *capture=std::getenv("PLA_NATIVE_CAPTURE"), *phase=std::getenv("PLA_NATIVE_PHASE");
        assert(capture && phase && (std::string_view(phase)=="before" || std::string_view(phase)=="after"));
        for (unsigned slot=0; slot<6; ++slot) {
            const auto suffix=std::string(phase)+"-"+std::to_string(slot)+".bin";
            const auto root=std::filesystem::path(capture);
            const auto core=Read(root/("native-core-"+suffix));
            const auto tail=Read(root/("native-tail-"+suffix));
            const auto flag=Read(root/("native-flag-"+suffix));
            assert(flag.size()==1);
            const auto p=pla::ParseNativePa8(core,tail,flag[0]);
            assert(p.valid);
            std::cout << slot << '\t' << p.species << '\t' << p.form << '\t' << p.hp << '\t'
                << p.max_hp << '\t' << p.level << '\t' << p.gender << '\t' << p.shiny << '\t'
                << p.status << '\n';
        }
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--party-block") {
        const auto bytes = Read(std::getenv("PLA_PARTY_BLOCK"));
        assert(bytes.size() > 0x8d0 && bytes[0x8d0] == 6);
        constexpr unsigned expected[] = {468, 571, 503, 59, 901, 903};
        constexpr unsigned genders[] = {0, 0, 0, 1, 1, 1};
        constexpr bool shiny[] = {false, true, false, false, true, false};
        for (unsigned j = 0; j < 6; ++j) {
            const auto p = pla::ParsePa8(std::span(bytes).subspan(j * 0x178, 0x178));
            assert(p.valid && p.species == expected[j] && p.level == 100 &&
                   p.gender == genders[j] && p.shiny == shiny[j]);
        }
        Fake native; auto nh=Host(native);
        NativePartyChain(native,nh,bytes);
        auto current=pla::ReadCurrentParty(nh);
        assert(current.valid && current.count == 6);
        // Read-only Pokemon inspection must not depend on slot39 position data.
        const auto *module =
            eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
        auto *inspection = module->create(&nh, "{}");
        assert(inspection);
        native.texts["__page"] = "pokemon";
        module->sample(inspection, &nh);
        assert(native.output["pla.position_valid"] == 0 && native.output["pla.map_valid"] == 0 &&
               native.output["pla.tap_ready"] == 0 && native.output["pla.pokemon.valid"] == 1);
        module->destroy(inspection);
        for (unsigned j=0; j<6; ++j) {
            const auto saved=pla::ParsePa8(std::span(bytes).subspan(j*0x178,0x178));
            assert(current.slots[j].valid && current.slots[j].species == expected[j] &&
                current.slots[j].hp == saved.hp && current.slots[j].max_hp == saved.max_hp &&
                current.slots[j].gender == genders[j] && current.slots[j].shiny == shiny[j]);
        }
        const auto saved_hp=current.slots[0].hp;
        std::array<uint8_t,0x178> changed;
        std::copy_n(bytes.begin(),changed.size(),changed.begin());
        const auto pv=pla::U32(changed,0);
        pla::Crypt(std::span(changed).subspan(8,0x160),pv);
        pla::Crypt(std::span(changed).subspan(0x168),pv);
        pla::UnshufflePa8(changed);
        // Create a distinct current HP without touching the original saved fixture.
        std::fill_n(changed.begin(),4,0); // native block order0, encryption seed0
        changed[0x92]=uint8_t(saved_hp-1); changed[0x93]=uint8_t((saved_hp-1)>>8);
        uint16_t checksum=0;
        for (unsigned at=8; at<0x168; at+=2) checksum+=pla::U16(changed,at);
        changed[6]=uint8_t(checksum);changed[7]=uint8_t(checksum>>8);
        pla::Crypt(std::span(changed).subspan(8,0x160),0);
        pla::Crypt(std::span(changed).subspan(0x168),0);
        native.memory[0x500000]={changed.begin(),changed.begin()+0x168};
        native.memory[0x500200]={changed.begin()+0x168,changed.end()};
        current=pla::ReadCurrentParty(nh);
        assert(current.valid && current.slots[0].valid && current.slots[0].hp == saved_hp-1);
        assert(pla::ParsePa8(std::span(bytes).first(0x178)).hp == saved_hp);
        pla::Crypt(std::span(changed).subspan(8,0x160),0);
        pla::Crypt(std::span(changed).subspan(0x168),0);
        native.memory[0x500000]={changed.begin(),changed.begin()+0x168};
        native.memory[0x500200]={changed.begin()+0x168,changed.end()};
        native.Put(0x400218,uint8_t(0));native.Put(0x400219,uint8_t(1));
        assert(pla::ReadCurrentParty(nh).slots[0].hp == saved_hp-1);
        native.Put(0x400230,uint64_t(1));
        current=pla::ReadCurrentParty(nh);
        assert(current.valid && !current.slots[0].valid && current.slots[1].valid);
        native.Put(0x400230,uint64_t(0));
        native.mutate_address=0x500000;
        assert(!pla::ReadCurrentParty(nh).slots[0].valid);
        native.mutate_address=0;
        native.Put(0x320088,uint8_t(7));
        assert(!pla::ReadCurrentParty(nh).valid);
        native.Put(0x320088,uint8_t(6));
        native.Put(0x320000,nh.main_base+0x418ad20);
        assert(!pla::ReadCurrentParty(nh).valid);
        native.Put(0x320000,nh.main_base+0x418ad18);
        nh.build_id[31]^=1;native.reads=0;
        assert(!pla::ReadCurrentParty(nh).valid && native.reads == 0);
        assert(!pla::ReadCurrentPokemon(nh,0x400000).valid && native.reads == 0);
        nh.build_id[31]^=1;nh.main_size=0x42a7007;native.reads=0;
        assert(!pla::ReadCurrentParty(nh).valid && native.reads == 0);
        nh.main_size=0x5000000;
        std::array<uint64_t,6> duplicate_slots{0x400000,0x400000,0x402000,0x403000,0x404000,0x405000};
        native.Put(0x320058,duplicate_slots);
        assert(!pla::ReadCurrentParty(nh).valid);
        native.reads=0;
        assert(!pla::ReadCurrentPokemon(nh,0x400001).valid && native.reads == 0);
        std::cout << "Private PLA-F14 party: six checked PA8, expected species, "
                     "all level100; split native buffers, newer HP, lock/torn/type/build refusal\n";
        return 0;
    }
    const auto *api =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api && api->supports_build(pla::BuildHex));
    assert(api->supports_build(std::string(pla::BuildHex, 40).c_str()));
    assert(!api->supports_build(nullptr) && !api->supports_build("AEE8F150DDA1B5A8"));
    Fake f;
    auto h = Host(f);
    Chain(f, h);
    if (argc > 1) {
        const char *romfs = std::getenv("PLA_ROMFS");
        const char *nso = std::getenv("PLA_NSO");
        if (!romfs || !nso) {
            std::cout << "SKIP: set PLA_ROMFS and PLA_NSO\n";
            return 77;
        }
        const auto main = Read(nso);
        assert(main.size() >= 0x60);
        assert(std::memcmp(main.data() + 0x40, pla::BuildId, 32) == 0);
        if (std::string_view(argv[1]) == "--romfs") {
            for (auto member : {pla::PackedUiMember::BattleTimeline, pla::PackedUiMember::BattleHeader}) {
                auto bytes = Read(std::filesystem::path(romfs) / pla::PackedUiPath(member));
                assert(bytes.size() == pla::PackedUiSize(member));
                const auto sarc = pla::DecodePackedUi(member, bytes);
                assert(!sarc.empty());
                const auto bntx = pla::SarcTexture(sarc);
                for (const auto *name : {"btl_tl_base_00_act^s", "btl_icon_state_00^t", "btl_icon_tl_00^t"}) {
                    if ((member == pla::PackedUiMember::BattleTimeline) !=
                        (std::string_view(name) == "btl_tl_base_00_act^s")) continue;
                    acnh::Image image;
                    assert(acnh::BntxDecode(bntx, name, image) && image.w > 0 && image.h > 0);
                }
                bytes[bytes.size() / 2] ^= 1;
                assert(pla::DecodePackedUi(member, bytes).empty());
                bytes.pop_back();
                assert(pla::DecodePackedUi(member, bytes).empty());
                assert(pla::DecodePackedUi(member, {}).empty());
            }
        }
        h.read_romfs = [](void *u, const char *path, uint64_t off, void *out,
                          size_t size) -> size_t {
            ++static_cast<Fake *>(u)->romfs_reads;
            auto bytes = Read(std::filesystem::path(std::getenv("PLA_ROMFS")) / path);
            if (!out && !size)
                return bytes.size();
            if (off >= bytes.size())
                return 0;
            size = std::min(size, bytes.size() - size_t(off));
            std::memcpy(out, bytes.data() + off, size);
            return size;
        };
        void *instance = api->create(&h, "{}");
        assert(instance);
        const auto *ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
        if (std::string_view(argv[1])=="--live-pokedex-preview") {
            // Live-dumped Dex block through the production Sample/Action/LoadImage path.
            const char *dir=std::getenv("PLA_LIVE_BLOCKS"),*preview=std::getenv("PLA_PREVIEW_DIR");
            if(!dir||!preview) return 77;
            std::filesystem::create_directories(preview);
            // Every block-<key>.bin in the directory becomes one entry of a sorted
            // block table; filler keys pad it to the >=1000 entries Resolve requires.
            std::vector<std::pair<uint32_t,std::vector<uint8_t>>> entries;
            for(const auto &file:std::filesystem::directory_iterator(dir)){
                const auto name=file.path().filename().string();
                if(name.size()!=18||!name.starts_with("block-"))continue;
                entries.push_back({uint32_t(std::stoul(name.substr(6,8),nullptr,16)),Read(file.path())});
            }
            for(uint32_t k=0xfff00000;entries.size()<1200;++k)entries.push_back({k,{0}});
            std::sort(entries.begin(),entries.end(),[](const auto &a,const auto &b){return a.first<b.first;});
            constexpr uint64_t table=0x2000000;
            uint64_t blob=0x3000000,dex_blob=0;
            for(size_t j=0;j<entries.size();++j){
                f.Put(table+j*0x30,uint64_t(entries[j].first));
                f.Put(table+j*0x30+8,blob);
                f.Put(table+j*0x30+0x18,uint64_t(entries[j].second.size()));
                if(entries[j].first==0x02168706)dex_blob=blob;
                f.memory[blob]=entries[j].second;
                blob+=(entries[j].second.size()+0x40)&~uint64_t(0xf);
            }
            assert(dex_blob);
            f.Put(h.main_base+0x42a6ed8,uint64_t(0x1f00000));
            f.Put(uint64_t(0x1f00100),uint64_t(0x1f10000));
            f.Put(uint64_t(0x1f10008),table);f.Put(uint64_t(0x1f10010),table+entries.size()*0x30);
            const auto blob_of_dex=dex_blob;
            if(std::getenv("PLA_WARM")){
                for(const char *k:{"module:pla:warm:dex","module:pla:warm:battle","module:pla:warm:missions"}){
                    const auto t0=std::chrono::steady_clock::now();
                    assert(ext->load_image(instance,&h,k,nullptr,[](void*,uint32_t,uint32_t,const uint8_t*,size_t){}));
                    std::cout<<"warm "<<k<<" "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count()<<" ms\n";
                }
            }
            f.texts["__page"]="pokedex";
            api->sample(instance,&h);
            assert(f.output["pla.pokedex.valid"]==1);
            auto sink=[](void*u,uint32_t w,uint32_t h,const uint8_t*data,size_t size) {
                auto path=*static_cast<std::filesystem::path*>(u);
                std::ofstream rgb(path,std::ios::binary);rgb<<"P6\n"<<w<<' '<<h<<"\n255\n";
                for(size_t n=0;n<size;n+=4)rgb.write(reinterpret_cast<const char*>(data+n),3);
            };
            const auto shot=[&](const char *name){
                api->sample(instance,&h);
                auto path=std::filesystem::path(preview)/(std::string(name)+".ppm");
                const auto t0=std::chrono::steady_clock::now();
                assert(ext->load_image(instance,&h,f.texts["pla.pokedex.image"].c_str(),nullptr,
                    [](void*,uint32_t,uint32_t,const uint8_t*,size_t){}));
                std::cout<<"render pokedex "<<name<<" "<<std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now()-t0).count()<<" ms\n";
                assert(ext->load_image(instance,&h,f.texts["pla.pokedex.image"].c_str(),&path,sink));
                if(f.output["pla.pokedex.body"]){
                    auto body=std::filesystem::path(preview)/(std::string(name)+"-body.ppm");
                    assert(ext->load_image(instance,&h,f.texts["pla.pokedex.body_image"].c_str(),&body,sink));
                }
            };
            for(const char *key:{"module:pla:party_cell:468:0:1:0:100:325:325:0:0:1:Tusi","module:pla:party_cell:468:0:1:0:100:325:325:0:1:1:Tusi","module:pla:party_cell:155:0:0:0:16:44:47:16:0:0:"}){
                static int n=0; auto path=std::filesystem::path(preview)/("card-"+std::to_string(n++)+".ppm");
                assert(ext->load_image(instance,&h,key,&path,sink));
            }
            shot("dex-first");
            assert(ext->on_action(instance,"dex_scroll",1));assert(ext->on_action(instance,"dex_row",3));shot("dex-scrolled-row3");
            assert(ext->on_action(instance,"dex_tab",1));shot("dex-research");
            assert(ext->on_action(instance,"dex_tab",2));shot("dex-weak");
            assert(ext->on_action(instance,"dex_tab",0));assert(ext->on_action(instance,"dex_area",1));shot("dex-area1");
            assert(ext->on_action(instance,"dex_area",0));assert(ext->on_action(instance,"dex_sort",0));shot("dex-sort-level");
            assert(f.output["pla.pokedex.sort"]==1);
            assert(ext->on_action(instance,"dex_sort",0));assert(ext->on_action(instance,"dex_unreported",0));shot("dex-unreported");
            assert(f.output["pla.pokedex.unreported"]==1);
            assert(ext->on_action(instance,"dex_unreported",0));
            for(const char *k:{"module:pla:control2:dex_sort:0","module:pla:control2:dex_sort:1","module:pla:control2:dex_unrep:1"}){
                static int n=0; auto path=std::filesystem::path(preview)/("control-"+std::to_string(n++)+".ppm");
                assert(ext->load_image(instance,&h,k,&path,sink));
            }
            // A stale key from an older generation is refused.
            assert(!ext->load_image(instance,&h,"module:pla:pokedex:1",nullptr,ImageSink));
            // Torn reads: the block changes between the two reads, so the last good copy stays.
            f.mutate_address=blob_of_dex;
            for(int n=0;n<3;++n){std::this_thread::sleep_for(std::chrono::milliseconds(520));api->sample(instance,&h);}
            assert(f.output["pla.pokedex.valid"]==1);
            for(int n=0;n<4;++n){std::this_thread::sleep_for(std::chrono::milliseconds(520));api->sample(instance,&h);}
            assert(f.output["pla.pokedex.valid"]==0);
            f.mutate_address=0;
            f.texts["__page"]="missions";
            api->sample(instance,&h);
            const auto mshot=[&](const char *name){
                api->sample(instance,&h);
                {const auto t0=std::chrono::steady_clock::now();
                 ext->load_image(instance,&h,f.texts["pla.missions.image"].c_str(),nullptr,[](void*,uint32_t,uint32_t,const uint8_t*,size_t){});
                 std::cout<<"render missions "<<name<<" "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count()<<" ms\n";}
                auto path=std::filesystem::path(preview)/(std::string(name)+".ppm");
                assert(ext->load_image(instance,&h,f.texts["pla.missions.image"].c_str(),&path,sink));
            };
            if(f.output["pla.missions.valid"]==1){
                mshot("missions-first");
                assert(ext->on_action(instance,"mis_row",2));mshot("missions-row2");
                assert(ext->on_action(instance,"mis_category",1));mshot("requests-first");
                assert(ext->on_action(instance,"mis_scroll",1));assert(ext->on_action(instance,"mis_row",0));mshot("requests-scrolled");
                assert(ext->on_action(instance,"mis_category",0));assert(ext->on_action(instance,"mis_active",0));mshot("missions-active-only");
                assert(f.output["pla.missions.active_only"]==1);
                {auto path=std::filesystem::path(preview)/"mis-active-control.ppm";assert(ext->load_image(instance,&h,"module:pla:control2:mis_active:1",&path,sink));}
                std::cout<<"Live missions rendered\n";
            } else std::cout<<"No quest blocks in this directory\n";
            {
                const auto bag=std::filesystem::path(dir)/"block-9fe2790a.bin";
                if(std::filesystem::exists(bag)){
                    const auto pouch=pla::DecodeBlock(Read(bag),0x9fe2790a);
                    if(pouch.payload.size()==2700){
                        f.Put(uint64_t(0x1f00280),uint64_t(0x1f20000));
                        f.memory[0x1f20068]=pouch.payload;
                    }
                }
                f.texts["__page"]="satchel";f.bag_key=2;
                const auto rshot=[&](const char *name){
                    api->sample(instance,&h);
                    auto path=std::filesystem::path(preview)/(std::string(name)+".ppm");
                    assert(ext->load_image(instance,&h,f.texts["pla.recipes.image"].c_str(),&path,sink));
                    if(f.output["pla.recipes.body"]){
                        auto body=std::filesystem::path(preview)/(std::string(name)+"-body.ppm");
                        assert(ext->load_image(instance,&h,f.texts["pla.recipes.body_image"].c_str(),&body,sink));
                    }
                };
                rshot("recipes-first");
                if(f.output["pla.recipes.body"]){
                    assert(ext->on_action(instance,"rec_scroll",1));assert(ext->on_action(instance,"rec_row",2));
                    rshot("recipes-scrolled-row2");
                }
                f.bag_key=0;
            }
            {
                BattleFixture(f,h,468,442);
                BattleSelectorFixture(f,h);
                f.texts["__page"]="battle";
                api->sample(instance,&h);
                assert(f.output["pla.battle.active"]==1 && f.output["pla.battle.moves"]==1);
                for(int n=0;n<2;++n){const auto t0=std::chrono::steady_clock::now();
                 ext->load_image(instance,&h,f.texts["pla.battle.image"].c_str(),nullptr,[](void*,uint32_t,uint32_t,const uint8_t*,size_t){});
                 std::cout<<"render battle "<<n<<" "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count()<<" ms\n";}
                auto path=std::filesystem::path(preview)/"battle.ppm";
                assert(ext->load_image(instance,&h,f.texts["pla.battle.image"].c_str(),&path,sink));
                auto lp=std::filesystem::path(preview)/"battle-legend.ppm";
                assert(ext->load_image(instance,&h,f.texts["pla.battle.legend"].c_str(),&lp,sink));
                f.Put(h.main_base+0x42A6F00,uint64_t(0)); // battle over
                for(int n=0;n<4;++n){std::this_thread::sleep_for(std::chrono::milliseconds(320));api->sample(instance,&h);}
                assert(f.output["pla.battle.active"]==0);
            }
            api->destroy(instance);
            std::cout<<"Live Pokedex block: sample, row/scroll/tab/area actions, bodies, torn-read hold then drop\n";
            return 0;
        }
        if (std::string_view(argv[1])=="--reading-unavailable-preview") {
            const char *preview=std::getenv("PLA_PREVIEW_DIR"); assert(preview);
            std::filesystem::create_directories(preview);
            const auto guest_before=f.reads;
            for (const auto &entry : {std::pair{"paper", "module:pla:paper"},
                    std::pair{"pokedex", "module:pla:pokedex_unavailable"},
                    std::pair{"missions", "module:pla:missions_unavailable:0"},
                    std::pair{"requests", "module:pla:missions_unavailable:1"},
                    std::pair{"nav-pokedex", "module:pla:tab_icon:pokedex"},
                    std::pair{"nav-missions", "module:pla:tab_icon:missions"},
                    std::pair{"nav-gold", "module:pla:tab:gold"},
                    std::pair{"nav-grey", "module:pla:tab:grey"},
                    std::pair{"nav-map", "module:pla:map_icon"},
                    std::pair{"nav-party", "module:pla:tab_icon:party"},
                    std::pair{"nav-bag", "module:pla:tab_icon:bag"},
                    std::pair{"nav-settings", "module:pla:tab_icon:settings"}}) {
                auto path=std::filesystem::path(preview)/(std::string(entry.first)+".ppm");
                auto sink=[](void*u,uint32_t w,uint32_t h,const uint8_t*data,size_t size) {
                    assert(size==size_t(w)*h*4);
                    auto path=*static_cast<std::filesystem::path*>(u);
                    std::ofstream rgb(path,std::ios::binary);rgb<<"P6\n"<<w<<' '<<h<<"\n255\n";
                    path.replace_extension(".alpha.pgm");std::ofstream a(path,std::ios::binary);a<<"P5\n"<<w<<' '<<h<<"\n255\n";
                    for(size_t n=0;n<size;n+=4){rgb.write(reinterpret_cast<const char*>(data+n),3);a.put(char(data[n+3]));}
                };
                assert(ext->load_image(instance,&h,entry.second,&path,sink));
            }
            assert(f.reads==guest_before);
            assert(!ext->load_image(instance,&h,"module:pla:missions_unavailable:2",nullptr,ImageSink));
            const auto romfs_before=f.romfs_reads;
            for(unsigned repeat=0;repeat<2;++repeat) for(const char *key:{
                    "module:pla:pokedex_unavailable","module:pla:missions_unavailable:0","module:pla:missions_unavailable:1"}) {
                size_t bytes=0;assert(ext->load_image(instance,&h,key,&bytes,ImageSink));
                assert(bytes==1104*1080*4);
            }
            std::cout<<"Unavailable reading-page warm ROMFS calls="<<f.romfs_reads-romfs_before<<"; guest reads="<<f.reads-guest_before<<"\n";
            assert(f.reads==guest_before);
            const auto romfs_after=f.romfs_reads;
            h.build_id[31]^=1;
            for(const char *key:{"module:pla:pokedex_unavailable","module:pla:missions_unavailable:0"})
                assert(!ext->load_image(instance,&h,key,nullptr,ImageSink));
            assert(f.romfs_reads==romfs_after && f.reads==guest_before);
            h.build_id[31]^=1;
            f.texts["__page"]="missions";
            for(int64_t request:{0,1,0}) {(void)request;f.texts["__page"]="missions";api->sample(instance,&h);
                assert(f.texts["pla.missions.image"].starts_with("module:pla:missions:"));}
            api->destroy(instance);
            std::cout<<"Six-tab unavailable image and local category binding checks pass; no current data supplied\n";
            return 0;
        }
        if (std::string_view(argv[1])=="--pokemon-preview" ||
            std::string_view(argv[1])=="--cross-page-cache") {
            const bool cache_only=std::string_view(argv[1])=="--cross-page-cache";
            const auto guest_reads_before=f.reads;
            const auto preload_start=std::chrono::steady_clock::now();
            size_t token_bytes=0;
            assert(ext->load_image(instance,&h,"module:pla:pokemon_static",&token_bytes,ImageSink));
            assert(token_bytes==4 && f.reads==guest_reads_before);
            std::cout<<"Pokemon static preload: "
                <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-preload_start).count()
                <<" ms, "<<f.romfs_reads<<" ROMFS calls, 0 guest reads\n";
            for (const char *key : {"module:pla:pokedex_unavailable",
                    "module:pla:missions_unavailable:0","module:pla:missions_unavailable:1",
                    "module:pla:settings_page","module:pla:toggle:0","module:pla:toggle:1","module:pla:settings_emblem",
                    "module:pla:bag_static","module:pla:bag_cursor","module:pla:bag_capacity",
                    "module:pla:bag_filter:0","module:pla:bag_filter:1",
                    "module:pla:bag_tabs:0","module:pla:bag_tabs:1","module:pla:bag_tabs:2"})
                assert(ext->load_image(instance,&h,key,&token_bytes,ImageSink));
            assert(f.reads==guest_reads_before);
            const char *party_path=std::getenv("PLA_PARTY_BLOCK");
            const char *preview=std::getenv("PLA_PREVIEW_DIR");
            assert(party_path && preview);
            const auto saved_party=Read(party_path);
            NativePartyChain(f,h,saved_party);
            if (const char *capture=std::getenv("PLA_NATIVE_CAPTURE")) {
                const char *phase=std::getenv("PLA_NATIVE_PHASE");assert(phase);
                for (unsigned slot=0;slot<6;++slot) {
                    const auto suffix=std::string(phase)+"-"+std::to_string(slot)+".bin";
                    const auto root=std::filesystem::path(capture);
                    const uint64_t core=0x500000+slot*0x1000,access=0x400200+slot*0x1000;
                    f.memory[core]=Read(root/("native-core-"+suffix));
                    f.memory[core+0x200]=Read(root/("native-tail-"+suffix));
                    const auto flag=Read(root/("native-flag-"+suffix));assert(flag.size()==1);
                    f.Put(access+0x18,flag[0]);
                }
            }
            if (const char *chart=std::getenv("PLA_TYPE_CHART"))
                f.memory[h.main_base+0x3984908]=Read(chart);
            f.texts["__page"]="pokemon";
            std::filesystem::create_directories(preview);
            auto save_image=[&](const std::string &name,const std::string &key) {
                if (cache_only) {
                    size_t bytes=0;assert(ext->load_image(instance,&h,key.c_str(),&bytes,ImageSink));return;
                }
                const auto path=std::filesystem::path(preview)/(name+".ppm");
                auto sink=[](void *u,uint32_t w,uint32_t h,const uint8_t *data,size_t size) {
                    assert(size==size_t(w)*h*4);
                    const auto &path=*static_cast<const std::filesystem::path *>(u);
                    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<w<<" "<<h<<"\n255\n";
                    for (size_t j=0;j<size;j+=4) out.write(reinterpret_cast<const char *>(data+j),3);
                    auto alpha=path;alpha.replace_extension(".alpha.pgm");
                    std::ofstream a(alpha,std::ios::binary);a<<"P5\n"<<w<<" "<<h<<"\n255\n";
                    for (size_t j=3;j<size;j+=4) a.put(char(data[j]));
                };
                assert(ext->load_image(instance,&h,key.c_str(),const_cast<std::filesystem::path *>(&path),sink));
            };
            api->sample(instance,&h);
            const auto first_start=std::chrono::steady_clock::now();
            const auto first_reads=f.romfs_reads;
            size_t first_size=0;
            assert(ext->load_image(instance,&h,f.texts["pla.pokemon.image"].c_str(),&first_size,ImageSink));
            assert(first_size==1104u*1080*4);
            std::cout<<"Pokemon first page after preload: "
                <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-first_start).count()
                <<" ms, "<<f.romfs_reads-first_reads<<" ROMFS calls\n";
            for (const auto &asset : {std::pair{"nav-gold","module:pla:tab:gold"},
                std::pair{"nav-grey","module:pla:tab:grey"},
                std::pair{"nav-map","module:pla:map_icon"},
                std::pair{"nav-party","module:pla:tab_icon:party"},
                std::pair{"nav-bag","module:pla:tab_icon:bag"},
                std::pair{"nav-settings","module:pla:tab_icon:settings"}})
                save_image(asset.first,asset.second);
            std::string old;
            for (unsigned slot:{0u,1u,4u}) {
                f.pokemon_slot=slot;
                for (unsigned tab=0;tab<3;++tab) {
                    f.pokemon_tab=tab;api->sample(instance,&h);
                    assert(f.output["pla.pokemon.valid"]==1);
                    const auto key=f.texts["pla.pokemon.image"];
                    if (!old.empty() && old!=key)
                        assert(!ext->load_image(instance,&h,old.c_str(),nullptr,ImageSink));
                    old=key;
                    const auto name="pokemon-"+std::to_string(slot)+"-"+std::to_string(tab);
                    save_image(name,key);
                    if (tab==2) {
                        save_image(name+"-body",f.texts["pla.pokemon.weak_image"]);
                        api->sample(instance,&h);
                        assert(f.texts["pla.pokemon.image"]==key);
                        assert(f.output["pla.pokemon.weak_height"]>=546 && f.output["pla.pokemon.weak_height"]<=1600);
                    }
                }
            }
            // Repeat already-primed member/topics through the real module image
            // callback: no ROMFS access or immutable decode should be repeated.
            for (unsigned slot=0;slot<6;++slot) {
                f.pokemon_slot=slot;
                for (unsigned tab=0;tab<3;++tab) {
                    f.pokemon_tab=tab;api->sample(instance,&h);
                    size_t bytes=0;
                    assert(ext->load_image(instance,&h,f.texts["pla.pokemon.image"].c_str(),&bytes,ImageSink));
                    if (tab==2)
                        assert(ext->load_image(instance,&h,f.texts["pla.pokemon.weak_image"].c_str(),&bytes,ImageSink));
                }
            }
            for (const char *key : {"module:pla:pokedex_unavailable",
                    "module:pla:missions_unavailable:0","module:pla:missions_unavailable:1",
                    "module:pla:settings_page","module:pla:toggle:0","module:pla:toggle:1","module:pla:settings_emblem",
                    "module:pla:bag_static","module:pla:bag_cursor","module:pla:bag_capacity",
                    "module:pla:bag_filter:0","module:pla:bag_filter:1",
                    "module:pla:bag_tabs:0","module:pla:bag_tabs:1","module:pla:bag_tabs:2"})
                assert(ext->load_image(instance,&h,key,&token_bytes,ImageSink));
            api->sample(instance,&h);
            const auto reads_before=f.romfs_reads;
            const auto bntx_before=f.output["pla.assets.bntx_builds"];
            const auto textures_before=f.output["pla.assets.texture_decodes"];
            const auto paper_before=f.output["pla.assets.paper_builds"];
            const auto warm_start=std::chrono::steady_clock::now();
            unsigned warm_images=0;
            for (unsigned slot=0;slot<6;++slot) {
                f.pokemon_slot=slot;
                for (unsigned tab=0;tab<3;++tab) {
                    f.pokemon_tab=tab;api->sample(instance,&h);
                    size_t bytes=0;
                    assert(ext->load_image(instance,&h,f.texts["pla.pokemon.image"].c_str(),&bytes,ImageSink));
                    assert(bytes==1104u*1080*4);++warm_images;
                    if (tab==2) {
                        assert(ext->load_image(instance,&h,f.texts["pla.pokemon.weak_image"].c_str(),&bytes,ImageSink));
                        assert(bytes==596u*1600*4);++warm_images;
                    }
                }
            }
            for (const char *key : {"module:pla:pokedex_unavailable",
                    "module:pla:missions_unavailable:0","module:pla:missions_unavailable:1",
                    "module:pla:settings_page","module:pla:toggle:0","module:pla:toggle:1","module:pla:settings_emblem",
                    "module:pla:bag_static","module:pla:bag_cursor","module:pla:bag_capacity",
                    "module:pla:bag_filter:0","module:pla:bag_filter:1",
                    "module:pla:bag_tabs:0","module:pla:bag_tabs:1","module:pla:bag_tabs:2"}) {
                assert(ext->load_image(instance,&h,key,&token_bytes,ImageSink));++warm_images;
            }
            api->sample(instance,&h);
            assert(f.romfs_reads==reads_before);
            assert(f.output["pla.assets.bntx_builds"]==bntx_before);
            assert(f.output["pla.assets.texture_decodes"]==textures_before);
            assert(f.output["pla.assets.paper_builds"]==paper_before);
            assert(f.output["pla.assets.byte_cache"]<=(16u<<20));
            assert(f.output["pla.assets.image_cache"]<=(32u<<20));
            std::cout<<"Cross-page cache warm: "<<warm_images<<" images, 0 ROMFS reads/decodes; "
                <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-warm_start).count()
                <<" ms; bytes "<<uint64_t(f.output["pla.assets.byte_cache"])<<" + "
                <<uint64_t(f.output["pla.assets.image_cache"])<<'\n';
            if (cache_only) {api->destroy(instance);return 0;}
            // Isolated synthetic display stress, never submitted to an emulator.
            for (unsigned slot : {1u,2u}) {
                std::array<uint8_t,0x178> mon{};
                std::copy_n(saved_party.begin()+slot*0x178,0x178,mon.begin());
                if (!pla::CheckPa8(mon)) {
                    const auto pv=pla::U32(mon,0);
                    pla::Crypt(std::span(mon).subspan(8,0x160),pv);
                    pla::Crypt(std::span(mon).subspan(0x168),pv);
                    pla::UnshufflePa8(mon);
                }
                assert(pla::CheckPa8(mon));
                const auto put=[&](size_t at,unsigned value,unsigned size=2) {
                    for (unsigned j=0;j<size;++j) mon[at+j]=uint8_t(value>>(j*8));
                };
                put(0,0,4);put(0x92,slot==1 ? 999 : 0);put(0x16a,999);
                if (slot==1) {
                    put(0x94,pla::U32(mon,0x94)|0x80000000u,4);
                    std::fill(mon.begin()+0x60,mon.begin()+0x7a,0);
                    const std::string name="Featherwings";
                    for (size_t j=0;j<name.size();++j) put(0x60+j*2,unsigned(name[j]));
                    for (size_t at=0x16c;at<0x176;at+=2) put(at,999);
                }
                uint16_t sum=0;
                for (size_t at=8;at<0x168;at+=2) sum+=pla::U16(mon,at);
                put(6,sum);
                const uint64_t core=0x500000+slot*0x1000,access=0x400200+slot*0x1000;
                f.memory[core]={mon.begin(),mon.begin()+0x168};
                f.memory[core+0x200]={mon.begin()+0x168,mon.end()};
                f.Put(access+0x18,uint8_t(0));
            }
            f.pokemon_slot=1;f.pokemon_tab=0;api->sample(instance,&h);
            save_image("pokemon-synthetic-stress",f.texts["pla.pokemon.image"]);
            // A legal Steel/Dragon type combination stresses the scroll extent.
            auto long_weak=f.memory[0x501000];assert(long_weak.size()==0x168);
            long_weak[8]=uint8_t(706);long_weak[9]=uint8_t(706>>8);long_weak[0x24]=1;
            uint16_t sum=0;
            for (size_t at=8;at<0x168;at+=2) sum+=pla::U16(long_weak,at);
            long_weak[6]=uint8_t(sum);long_weak[7]=uint8_t(sum>>8);
            f.memory[0x501000]=std::move(long_weak);
            f.pokemon_tab=2;api->sample(instance,&h);
            save_image("pokemon-synthetic-long-weak",f.texts["pla.pokemon.image"]);
            save_image("pokemon-synthetic-long-weak-body",f.texts["pla.pokemon.weak_image"]);
            api->sample(instance,&h);
            assert(f.output["pla.pokemon.weak_height"]>546 && f.output["pla.pokemon.weak_height"]<=1600);
            f.Put(0x320088,uint8_t(0));api->sample(instance,&h);
            assert(!f.output["pla.pokemon.valid"]);
            save_image("pokemon-empty",f.texts["pla.pokemon.image"]);
            api->destroy(instance);
            std::cout<<"Private Pokemon page previews: actual split captures; stale snapshot keys refused; empty clears\n";
            return 0;
        }
        const auto item_icons=Read(std::filesystem::path(romfs)/"bin/appli/res_item/list/item_info_list.bin");
        assert(pla::ItemIcon(item_icons,1710)=="bin/appli/res_item/icon_item/item_1710.bntx");
        for (unsigned id=1787;id<=1807;++id) {
            if (id==1791) continue;
            assert(pla::ItemIcon(item_icons,id)=="bin/appli/res_item/icon_item/item_1787.bntx");
        }
        assert(pla::ItemIcon(item_icons,1791)=="bin/appli/res_item/icon_item/item_dummy.bntx");
        assert(pla::ItemIcon(item_icons,0).empty() && pla::ItemIcon(item_icons,4096).empty());
        for (size_t n : {size_t(0),size_t(4),item_icons.size()/2})
            assert(pla::ItemIcon(std::span(item_icons).first(n),1807).empty());
        size_t bytes = 0;
        const auto icon_table=Read(std::filesystem::path(romfs) /
            "bin/appli/res_pokemon/list/pokemon_info_list.bin");
        for (const auto &entry : {std::pair{468u,"0468_000_000"}, {571u,"0571_000_041"},
                                 {503u,"0503_000_041"}, {59u,"0059_000_041"},
                                 {901u,"1007_000_000"}, {903u,"1003_000_041"}}) {
            const unsigned form=(entry.first == 59 || entry.first == 503 || entry.first == 571);
            for (unsigned shiny=0; shiny<2; ++shiny) {
                const auto path=pla::PokemonIcon(icon_table,entry.first,form,0,shiny);
                assert(path == std::string("bin/appli/res_pokemon/icon_pokemon_s/pokeicon_s_") +
                    entry.second + "_n_00000000_fn_" + (shiny ? "r" : "n") + ".bntx");
                assert(!Read(std::filesystem::path(romfs)/path).empty());
                const auto key="module:pla:pokemon:"+std::to_string(entry.first)+":"+
                    std::to_string(form)+":0:"+std::to_string(shiny);
                assert(ext->load_image(instance,&h,key.c_str(),&bytes,ImageSink));
                assert(bytes == 64*64*4);
            }
        }
        assert(pla::PokemonIcon(icon_table,59,0,0,false).empty());
        assert(pla::PokemonIcon(icon_table,468,255,0,false).empty());
        assert(pla::PokemonIcon(icon_table,468,0,3,false).empty());
        struct PortraitCase { unsigned species, form, gender; bool shiny; const char *asset; };
        for (const auto &entry : {
                PortraitCase{59,1,1,false,"0059_000_041_n_00000000_fn_n"},
                {901,0,1,true,"1007_000_000_n_00000000_fn_r"},
                {903,0,1,false,"1003_000_041_n_00000000_fn_n"},
                {25,0,0,false,"0025_000_000_n_00000000_fn_n"},
                {25,0,1,false,"0025_001_000_n_00000000_fn_n"},
                {81,0,2,false,"0081_000_000_n_00000000_fn_n"},
                {488,0,1,false,"0488_000_000_n_00000000_fn_n"}}) {
            const auto path=pla::PokemonIcon(icon_table,entry.species,entry.form,entry.gender,entry.shiny);
            assert(path == std::string("bin/appli/res_pokemon/icon_pokemon_s/pokeicon_s_")+
                entry.asset+".bntx");
            const auto key="module:pla:pokemon:"+std::to_string(entry.species)+":"+
                std::to_string(entry.form)+":"+std::to_string(entry.gender)+":"+
                std::to_string(entry.shiny);
            assert(ext->load_image(instance,&h,key.c_str(),&bytes,ImageSink));
            assert(bytes == 64*64*4);
        }
        assert(pla::PokemonIcon(icon_table,81,0,0,false).empty());
        assert(pla::PokemonIcon(icon_table,488,0,0,false).empty());
        // Every short prefix and representative internal truncations must refuse.
        for (size_t n=0; n<128; ++n)
            assert(pla::PokemonIcon(std::span(icon_table).first(n),468,0,0,false).empty());
        for (size_t n=128; n<icon_table.size(); n+=257)
            assert(pla::PokemonIcon(std::span(icon_table).first(n),468,0,0,false).empty());
        auto bad_icons=icon_table;
        std::fill_n(bad_icons.begin(),4,0xff);
        assert(pla::PokemonIcon(bad_icons,468,0,0,false).empty());
        assert(!ext->load_image(instance,&h,"module:pla:pokemon:468:0:0:0junk",&bytes,ImageSink));
        assert(ext->load_image(instance, &h, "module:pla:map:0", &bytes, ImageSink));
        assert(bytes == 2048 * 2048 * 4);
        auto font = Read(std::filesystem::path(romfs) / "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
        const auto *fonts =
            eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH);
        assert(fonts->decode_font(instance, font.data(), font.size(), &bytes, FontSink));
        assert(bytes == 0x203b - 32);
        assert(ext->load_image(instance, &h, "module:pla:font", &bytes, ImageSink));
        assert(bytes == 1024 * 1024 * 4);
        for (unsigned cap : {37u, 23u, 21u}) {
            pla::Font sized;
            assert(pla::BuildFont(font, sized, cap));
            assert(sized.glyphs['H' - 32].h == cap);
            auto image = pla::TextImage(sized, "HHEEFF", cap, 600, 100);
            int top = 100, bottom = -1;
            for (int y = 0; y < 100; ++y)
                for (int x = 0; x < 600; ++x)
                    if (image.At(x, y)[3]) {
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
            assert(bottom - top + 1 == int(cap));
            for (unsigned cp = 160; cp <= 255; ++cp)
                assert(sized.glyphs[cp - 32].advance); // includes accented Western European text
        }
        for (unsigned area : {0u, 1u, 2u, 3u, 4u, 5u}) {
            const auto key = "module:pla:map:" + std::to_string(area) + (area == 5 ? ":0" : "");
            assert(ext->load_image(instance, &h, key.c_str(), &bytes, ImageSink));
            const auto title = "module:pla:area_title:" + std::to_string(area);
            assert(ext->load_image(instance, &h, title.c_str(), &bytes, ImageSink));
        }
        assert(ext->load_image(instance, &h, "module:pla:tab:gold", &bytes, ImageSink));
        assert(ext->load_image(instance, &h, "module:pla:marker", &bytes, ImageSink));
        assert(ext->load_image(instance, &h, "module:pla:settings", &bytes, ImageSink));
        assert(bytes < 90 * 90 * 4); // texture padding was removed before 64px presentation
        assert(ext->load_image(instance, &h, "module:pla:control:off", &bytes, ImageSink));
        assert(bytes == 124 * 112 * 4);
        auto bar_sink = [](void *, uint32_t w, uint32_t h, const uint8_t *data, size_t size) {
            assert(w == 600 && h == 48 && size == 600 * 48 * 4);
            assert(data[3] == 0); // rounded corner
            const auto *fill = data + (24 * 600 + 8) * 4;
            assert(fill[0] == 78 && fill[1] == 80 && fill[2] == 81 && fill[3] == 255);
        };
        assert(ext->load_image(instance, &h, "module:pla:area_title:3", nullptr, bar_sink));
        const auto map_reads = f.romfs_reads;
        assert(!ext->load_image(instance, &h, "module:pla:map:5", &bytes, ImageSink));
        assert(!ext->load_image(instance, &h, "module:pla:map:5:6", &bytes, ImageSink));
        assert(f.romfs_reads == map_reads);
        for (unsigned variant = 0; variant < 6; ++variant) {
            const auto key = "module:pla:map:5:" + std::to_string(variant);
            assert(ext->load_image(instance, &h, key.c_str(), &bytes, ImageSink));
        }
        for (const char *key :
             {"module:pla:item:1710:294:1", "module:pla:hero_item:1710:294",
              "module:pla:name:468:0:", "module:pla:pokemon:468:0:0:0", "module:pla:plate:selected",
              "module:pla:hero_pokemon:468:333:333:0:", "module:pla:hero_slot:6", "module:pla:bag_row:1710:294:1:1",
              "module:pla:bag_detail:1711:515:1", "module:pla:paper", "module:pla:bag"})
            assert(ext->load_image(instance, &h, key, &bytes, ImageSink));
        for (const char *key : {"module:pla:item:1710:294:1", "module:pla:item:1711:515:0",
                                "module:pla:item:1711:515:2",
                                "module:pla:item:1711:0:0"}) {
            assert(ext->load_image(instance, &h, key, &bytes, ImageSink));
            assert(bytes == 122 * 124 * 4); // raised band and native left chevron.
        }
        // Repainting an inspected row/detail uses immutable art only: after
        // warming, removing ROMFS access must preserve the exact picture and
        // must never cause a guest read or retain an inventory snapshot.
        auto picture_sink=[](void *u,uint32_t,uint32_t,const uint8_t *data,size_t size) {
            auto &picture=*static_cast<std::vector<uint8_t> *>(u);
            picture.assign(data,data+size);
        };
        for (const char *key : {"module:pla:bag_row:1711:515:1:1", "module:pla:bag_detail:1711:515:1"}) {
            std::vector<uint8_t> cold,warm;
            assert(ext->load_image(instance,&h,key,&cold,picture_sink));
            const auto reader=h.read_romfs;
            const auto reads=f.reads;
            h.read_romfs=[](void *,const char *,uint64_t,void *,size_t)->size_t { return 0; };
            assert(ext->load_image(instance,&h,key,&warm,picture_sink));
            assert(cold==warm && f.reads==reads);
            h.read_romfs=reader;
        }
        for (const char *key : {"module:pla:party_cell:155:0:0:0:16:44:47:0:1:0:",
                "module:pla:party_cell:399:0:0:0:11:0:42:0:0:0:",
                "module:pla:party_cell:406:0:0:0:10:22:27:8:0:0:"}) {
            const auto reads=f.reads;
            assert(ext->load_image(instance,&h,key,&bytes,ImageSink));
            assert(bytes==170u*168*4 && f.reads==reads);
        }
        for (const char *key : {"module:pla:hero_item_card:1710:294",
                "module:pla:party_hero:571:1:0:0:100:262:262:0:1:1:Alkony"}) {
            assert(ext->load_image(instance,&h,key,&bytes,ImageSink));assert(bytes==324u*124*4);
        }
        std::vector<uint8_t> before_faint,after_faint;
        const char *healthy="module:pla:party_cell:399:0:0:0:11:42:42:0:0:0:";
        assert(ext->load_image(instance,&h,healthy,&before_faint,picture_sink));
        assert(ext->load_image(instance,&h,"module:pla:party_cell:399:0:0:0:11:0:42:0:0:0:",&bytes,ImageSink));
        assert(ext->load_image(instance,&h,healthy,&after_faint,picture_sink));
        assert(before_faint==after_faint); // local greying must not mutate shared portrait art.
        for (const char *key : {"module:pla:party_cell:155:0:0:0:16:48:47:0:1:0:",
                "module:pla:party_cell:155:0:0:0:0:44:47:0:1:0:",
                "module:pla:party_hero:571:255:0:0:100:262:262:0:1:1:Alkony",
                "module:pla:hero_item_card:1710:294junk"})
            assert(!ext->load_image(instance,&h,key,&bytes,ImageSink));
        if (const char *preview = std::getenv("PLA_PREVIEW_DIR")) {
            std::filesystem::create_directories(preview);
            for (const auto &entry : {std::pair{"bag-row", "module:pla:bag_row:1711:515:1:1"},
                                      std::pair{"bag-tabs", "module:pla:bag_tabs:0"},
                                      std::pair{"bag-tabs-key", "module:pla:bag_tabs:1"},
                                      std::pair{"bag-filter", "module:pla:bag_filter:0"},
                                      std::pair{"bag-filter-active", "module:pla:bag_filter:1"},
                                      std::pair{"item-selected", "module:pla:item:1710:294:1"},
                                      std::pair{"item-normal", "module:pla:item:1711:515:0"},
                                      std::pair{"item-remembered", "module:pla:item:1711:515:2"},
                                      std::pair{"item-zero", "module:pla:item:1711:0:0"},
                                      std::pair{"bag-detail", "module:pla:bag_detail:1711:515:1"},
                                      std::pair{"hero-slot", "module:pla:hero_slot:6"},
                                      std::pair{"paper", "module:pla:paper"},
                                      std::pair{"party-plate", "module:pla:plate:normal"}}) {
                const auto path = std::filesystem::path(preview) / (std::string(entry.first) + ".ppm");
                auto sink = [](void *u, uint32_t w, uint32_t h, const uint8_t *data, size_t size) {
                    assert(size == size_t(w) * h * 4);
                    std::ofstream out(*static_cast<const std::filesystem::path *>(u), std::ios::binary);
                    out << "P6\n" << w << " " << h << "\n255\n";
                    for (size_t j = 0; j < size; j += 4)
                        out.write(reinterpret_cast<const char *>(data + j), 3);
                    auto alpha_path = *static_cast<const std::filesystem::path *>(u);
                    alpha_path.replace_extension(".alpha.pgm");
                    std::ofstream alpha(alpha_path, std::ios::binary);
                    alpha << "P5\n" << w << " " << h << "\n255\n";
                    for (size_t j = 3; j < size; j += 4)
                        alpha.put(char(data[j]));
                };
                assert(ext->load_image(instance, &h, entry.second,
                                       const_cast<std::filesystem::path *>(&path), sink));
            }
        }
        api->destroy(instance);
        std::cout << "Private NSO, map and widened CFF atlas passed\n";
        return 0;
    }
    {
        // Battle roster reader: typed actors, active own/foe, order; torn and mistyped refuse.
        Fake bf; auto bh = Host(bf);
        assert(!pla::ReadBattle(bh).valid);
        BattleFixture(bf, bh, 468, 442);
        auto r = pla::ReadBattle(bh);
        assert(r.valid && r.actors.size() == 2 && r.actors[0].species == 468 && r.actors[1].id == 12);
        assert(r.actors[1].hp == 50 && r.actors[1].level == 68 && r.actors[0].move_count == 2);
        assert(r.actors[0].move[1] == 52 && r.actors[0].pp[0] == 10 && r.actors[0].max_pp[0] == 15);
        assert(r.ByB(r.own_b)->id == 0 && r.ByB(r.foe_b)->id == 12 && r.order_valid);
        assert(r.actors[1].status_id == 4 && r.actors[0].status_id == 0);
        {
            // Multi-foe: two foes in group 1, the first fainted -> the standing one is shown.
            bf.Put(uint64_t(0x32B0070), uint8_t(2));
            bf.Put(uint64_t(0x32B0048), uint64_t(0x3261000));
            auto mem = bf.memory[0x3261060]; bf.memory[0x3260060][0x74 - 0x60] = 0; bf.memory[0x3260060][0x75 - 0x60] = 0;
            bf.Put(uint64_t(0x32B0040), uint64_t(0x3260000)); // fainted own-side B reused as foe sub0
            auto m = pla::ReadBattle(bh);
            assert(m.valid && m.foe_b == 0x3261000);
            bf.memory[0x3260060][0x74 - 0x60] = 180;
            bf.Put(uint64_t(0x32B0040), uint64_t(0x3261000));
            bf.Put(uint64_t(0x32B0070), uint8_t(1));
            (void)mem;
        }
        bf.Put(uint64_t(0x32301A0), uint32_t(31)); // over the native 30-actor cap
        assert(!pla::ReadBattle(bh).valid);
        bf.Put(uint64_t(0x32301A0), uint32_t(2));
        bf.mutate_address = 0x32301A0; // roster count changes between the two reads
        assert(!pla::ReadBattle(bh).valid);
        bf.mutate_address = 0;
        bf.Put(uint64_t(0x3241000), uint64_t(0x1234)); // not a battle actor
        assert(!pla::ReadBattle(bh).valid);
    }
    // Position: the movement-tested chain publishes after two agreeing reads.
    f.reads = 0;
    {
        const auto p = pla::ReadPosition(h);
        assert(p.valid && std::abs(p.x - 214.604f) < 1e-3 && std::abs(p.z - 81.316f) < 1e-3);
        assert(f.reads == 14); // two chains of six links plus two xyz reads
    }
    // A chain that moves between the two reads (torn) publishes nothing.
    f.mutate_address = 0x104080;
    assert(!pla::ReadPosition(h).valid);
    f.mutate_address = 0;
    f.Put(uint64_t(0x104080), uint64_t(0x105000));
    {
        const float zero[] = {0.f, 0.f, 0.f};
        f.Put(0x105090, zero);
        assert(!pla::ReadPosition(h).valid);
        const float wild[] = {NAN, 0.f, 1.f};
        f.Put(0x105090, wild);
        assert(!pla::ReadPosition(h).valid);
    }
    void *instance = api->create(&h, "{}");
    assert(instance);
    const auto *actions = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(actions && actions->on_action);
    for (const float x : {214.604f, 700.f}) {
        const float xyz[] = {x, 303.753f, 81.316f};
        f.Put(0x105090, xyz);
        for (int follow : {0, 1}) {
            f.follow = follow;
            api->sample(instance, &h);
            assert(f.output["pla.position_valid"] == 1 && f.output["pla.readiness"] == 1);
            assert(std::abs(f.output["pla.x"] - x) < 1e-3);
            assert(f.texts["pla.raw.x"] != "—");
        }
    }
    // A failed sample clears the previous positive publication.
    f.fail = true;
    api->sample(instance, &h);
    f.fail = false;
    for (const char *key : {"pla.position_valid", "pla.map_valid", "pla.readiness",
                            "pla.area_title_visible",
                            "pla.tap_ready", "pla.x", "pla.height", "pla.z", "pla.map_x", "pla.map_y"})
        assert(f.output[key] == 0);
    assert(!actions->on_action(instance, "select_item", 0));
    // Position never becomes available for another title/build or missing API.
    h.build_id[31] = 1;
    f.reads = 0;
    api->sample(instance, &h);
    assert(f.reads == 0 && f.output["pla.position_valid"] == 0);
    h.title_id = 0;
    assert(!pla::ReadPosition(h).valid && f.reads == 0);
    h = Host(f);
    h.read_memory = nullptr;
    assert(!pla::ReadPosition(h).valid);
    for (unsigned i = 0; i < pla::Transforms.size(); ++i) {
        auto xy = pla::MapPixel(i, 12, -3);
        assert(xy[0] == 24 && xy[1] == -6);
    }
    pla::Font font;
    assert(!pla::BuildFont({}, font));
    TextureTest();
    api->destroy(instance);
    std::cout << "Double-read position, torn-read refusal, stale publication clearing, transforms, BC3 and "
                 "malformed font passed\n";
}

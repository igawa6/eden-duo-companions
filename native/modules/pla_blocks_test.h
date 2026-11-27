// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_dock.h"

// Included after Fake/Host in pla_test.cpp. Every fixture here is synthetic.
std::vector<uint8_t> ObjectBlob(std::span<const uint8_t> payload, uint32_t key) {
    std::vector<uint8_t> blob(5 + payload.size());
    blob[0] = 4;
    for (unsigned j = 0; j < 4; ++j)
        blob[j + 1] = payload.size() >> (8 * j);
    std::copy(payload.begin(), payload.end(), blob.begin() + 5);
    pla::CryptBlock(blob, key);
    return blob;
}
void PutBlob(Fake &f, uint64_t address, const std::vector<uint8_t> &blob) {
    f.memory[address] = blob;
}
void BlocksTests() {
    Fake f;
    auto h = Host(f);
    const uint64_t s = 0x200000, t = 0x201000, b = 0x300000, n = 1000;
    f.Put(h.main_base + 0x42a6ed8, s);
    f.Put(s + 0x100, t);
    f.Put(t + 8, b);
    f.Put(t + 16, b + n * 0x30);
    for (uint64_t j = 0; j < n; ++j)
        f.Put(b + j * 0x30, j * 10 + 1);
    f.Put(b + 543 * 0x30 + 8, uint64_t(0x500000));
    f.Put(b + 543 * 0x30 + 0x18, uint64_t(5));
    auto blocks = pla::Blocks::Resolve(h);
    assert(blocks.begin == b);
    assert(!blocks.Find(h, 5430).data);
    auto block = blocks.Find(h, 5431);
    assert(block.data == 0x500000 && block.size == 5);
    std::array<uint8_t, 5> payload{1, 2, 3, 4, 5}, out{};
    f.Put(block.data, payload);
    assert(block.Read(h, 0, out.data(), 5) && out == payload);
    const auto reads = f.reads;
    assert(!block.Read(h, 1, out.data(), 5));
    assert(!block.Read(h, UINT64_MAX, out.data(), 1));
    assert(f.reads == reads);
    for (uint64_t end : {b - 8, b + 1, b + 999 * 0x30, b + 4001 * 0x30}) {
        f.Put(t + 16, end);
        assert(!pla::Blocks::Resolve(h).begin);
    }
    f.Put(t + 16, b + n * 0x30);
    f.Put(b + 250 * 0x30, uint64_t(1));
    assert(!pla::Blocks::Resolve(h).begin);
    f.Put(b + 250 * 0x30, uint64_t(2501));
    f.Put(b + 500 * 0x30, uint64_t(UINT64_MAX));
    assert(!blocks.Find(h, 5431).data);
    f.Put(s + 0x100, uint64_t(1));
    assert(!pla::Blocks::Resolve(h).begin);
    h.build_id[0] ^= 1;
    f.reads = 0;
    assert(!pla::Blocks::Resolve(h).begin && !pla::ReadSelector(h).valid && f.reads == 0);
    // Map source accepts only a complete, terminated exact area code.
    h = Host(f);
    auto coords = pla::Blocks{b, b + n * 0x30};
    // Separate sorted table with the target key at its midpoint.
    for (uint64_t j = 0; j < n; ++j)
        f.Put(b + j * 0x30, uint64_t(0x267dd9da - 500 + j));
    f.Put(b + 500 * 0x30 + 8, uint64_t(0x600000));
    f.Put(b + 500 * 0x30 + 0x18, uint64_t(117));
    for (int area = 0; area <= 5; ++area) {
        char name[10] = "ha_area00";
        name[8] = '0' + area;
        std::array<uint8_t, 112> payload{};
        std::memcpy(payload.data() + 8, name, sizeof name);
        PutBlob(f, 0x600000, ObjectBlob(payload, 0x267dd9da));
        assert(pla::ReadArea(h, coords) == area);
    }
    char bad[10] = "ha_area06";
    std::array<uint8_t, 112> badCoords{};
    std::memcpy(badCoords.data() + 8, bad, sizeof bad);
    PutBlob(f, 0x600000, ObjectBlob(badCoords, 0x267dd9da));
    assert(pla::ReadArea(h, coords) == -1);
    for (uint64_t j = 0; j < n; ++j)
        f.Put(b + j * 0x30, uint64_t(0x2985fe5d - 500 + j));
    f.Put(b + 500 * 0x30 + 8, uint64_t(0x600000));
    f.Put(b + 500 * 0x30 + 0x18, uint64_t(2265));
    std::array<uint8_t, 2260> partyBytes{};
    partyBytes[0x8d0] = 7;
    PutBlob(f, 0x600000, ObjectBlob(partyBytes, 0x2985fe5d));
    assert(!pla::ReadParty(h, coords).valid);
    partyBytes[0x8d0] = 1;
    PutBlob(f, 0x600000, ObjectBlob(partyBytes, 0x2985fe5d));
    auto badParty = pla::ReadParty(h, coords);
    assert(badParty.valid && !badParty.slots[0].valid);
    f.Put(b + 500 * 0x30 + 0x18, uint64_t(0x8d0));
    assert(!pla::ReadParty(h, coords).valid);
    assert(!pla::VectorBounds(h, 0x700000, 0x700007, 6, 64));
    assert(!pla::VectorBounds(h, 0x700000, 0x700000 + 65 * 6, 6, 64));
    assert(!pla::VectorBounds(h, 0x700000, 0x700000 + 7 * 8, 8, 6));
    assert(pla::VectorBounds(h, 0, 0, 6, 64));
    // Actual view traversal; type is deliberately at a non-fixed list index.
    f.Put(h.main_base + 0x42a8b00, uint64_t(0x210000));
    f.Put(0x210050, uint64_t(0x220000));
    f.Put(0x220030, uint64_t(0x230000));
    f.Put(0x2304c8, uint64_t(0x240000));
    f.Put(0x240070, uint64_t(0x250000));
    f.Put(0x240078, uint64_t(0x250010));
    f.Put(0x250000, uint64_t(0x260000));
    f.Put(0x260000, uint64_t(0));
    f.Put(0x250008, uint64_t(0x270000));
    f.Put(0x270000, h.main_base + 0x415d618);
    const uint64_t v = 0x270000, ib = 0x280000, pb = 0x290000;
    f.Put(v + 0x750, unsigned(0));
    f.Put(v + 0x704, unsigned(0));
    f.Put(v + 0x5b4, uint8_t(1));
    f.Put(v + 0x5ac, unsigned(1));
    f.Put(v + 0x5c0, ib);
    f.Put(v + 0x5c8, ib + 12);
    f.Put(v + 0x630, pb);
    f.Put(v + 0x638, pb + 16);
    f.Put(v + 0x6f8, unsigned(1));
    f.Put(v + 0x6fc, unsigned(0));
    const std::array<pla::Item, 2> items{{{1710, 294, 1}, {1711, 515, 1}}};
    f.Put(ib, items);
    std::array<uint64_t, 2> slots{5, 2};
    f.Put(pb, slots);
    auto selector = pla::ReadSelector(h);
    assert(selector.valid && selector.items[1].count == 515 && selector.party[0] == 5);
    {
        // Throw selector HUD hide: view -> work+148/150 binding table -> hash entry +20 pane.
        const uint64_t work = 0x2a0000, table = 0x2b0000, pane = 0x2c0000;
        f.Put(v + 0x2c8, work);
        f.Put(work + 0x148, table);
        f.Put(work + 0x150, uint64_t(3));
        for (unsigned n = 0; n < 3; ++n) {
            f.Put(table + n * 0x30, n == 1 ? pla::SelectorAllHash : uint64_t(0x1111 + n));
            f.Put(table + n * 0x30 + 0x20, n == 1 ? pane : uint64_t(0x2d0000));
        }
        f.Put(pane + 0x58, uint8_t(131));
        auto hh = h;
        hh.write_memory = [](void *u, uint64_t at, const void *in, size_t size) -> EdenDsmodBool {
            auto &f = *static_cast<Fake *>(u);
            auto it = f.memory.find(at);
            if (it == f.memory.end() || it->second.size() != size) return false;
            std::memcpy(it->second.data(), in, size);
            return true;
        };
        pla::SelectorHud hud;
        assert(hud.Resolve(hh, selector.view) && hud.pane == pane);
        const auto flags = [&] { uint8_t b = 0; pla::ReadAt(hh, pane, 0x58, b); return b; };
        hud.Apply(hh, true);
        assert(flags() == 130 && hud.hidden_by_us);
        f.Put(pane + 0x58, uint8_t(131)); // native refresh re-shows it
        assert(hud.Resolve(hh, selector.view));
        hud.Apply(hh, true);
        assert(flags() == 130);
        hud.Apply(hh, false); // setting turned off / bottom screen gone: restore
        assert(flags() == 131 && !hud.hidden_by_us);
        f.Put(pane + 0x58, uint8_t(130)); // the game hid it itself
        hud.Apply(hh, false);
        assert(flags() == 130); // never re-shown by the companion
        // Hidden, then the pane's binding disappears (freed) before the restore: no write.
        f.Put(pane + 0x58, uint8_t(131));
        assert(hud.Resolve(hh, selector.view));
        hud.Apply(hh, true);
        assert(flags() == 130 && hud.hidden_by_us);
        f.Put(table + 0x30, uint64_t(0x9999));
        hud.Apply(hh, false);
        assert(flags() == 130 && !hud.hidden_by_us && !hud.pane);
        f.Put(table + 0x30, pla::SelectorAllHash);
        f.Put(table + 0x30, uint64_t(0x9999)); // binding gone: forget
        assert(!hud.Resolve(hh, selector.view) && !hud.pane);
        f.Put(table + 0x30, pla::SelectorAllHash);
        f.Put(pane + 0x58, uint8_t(131));
    }
    // Exercise the production configure/action/sample path with a real synthetic
    // pointer route, then invalidate the full build ID while a request is pending.
    Chain(f, h);
    const auto *api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto *ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    auto *instance = api->create(&h, "{}");
    std::array<uint32_t, 1024> mailbox{};
    mailbox[PLA_EPOCH / 4] = 9;
    EdenDsmodHostExtensions io{};
    io.version = EDEN_DSMOD_EXT_VERSION; io.struct_size = sizeof io;
    io.abi_hash = EDEN_DSMOD_EXT_HASH; io.userdata = mailbox.data();
    io.mailbox_address = 0x12340000; io.mailbox_size = sizeof mailbox;
    io.load_u32 = [](void *u, uint32_t off, uint32_t *out) -> EdenDsmodBool {
        *out = static_cast<uint32_t *>(u)[off/4]; return true;
    };
    io.store_u32 = [](void *u, uint32_t off, uint32_t value) -> EdenDsmodBool {
        static_cast<uint32_t *>(u)[off/4] = value; return true;
    };
    ext->configure(instance, &io);
    f.tap_select = 0; api->sample(instance, &h);
    assert(!ext->on_action(instance, "select_item", 0) && mailbox[PLA_REQ/4] == 0);
    f.tap_select = 1; api->sample(instance, &h);
    assert(f.output["pla.selector_valid"] == 1 && f.output["pla.position_valid"] == 1);
    assert(f.texts["pla.item.1.image"] == "module:pla:item:1711:515:2");
    assert(ext->on_action(instance, "select_item", 0));
    api->sample(instance, &h);
    assert(mailbox[PLA_REQ/4] != 0 && mailbox[PLA_OP/4] == PLA_TOGGLE);
    assert(f.output["pla.item.0.pending"] == 1);
    h.build_id[31] = 1; f.reads = 0;
    api->sample(instance, &h);
    assert(f.reads == 0 && f.output["pla.item.0.pending"] == 0);
    assert(mailbox[PLA_CANCEL/4] == mailbox[PLA_REQ/4]);
    assert(!ext->on_action(instance, "select_item", 0));
    h.build_id[31] = 0;
    ext->configure(instance, nullptr); api->sample(instance, &h);
    assert(!ext->on_action(instance, "select_item", 0));
    // Hidden-display path calls only Tick; it must cancel without sampling memory.
    mailbox.fill(0); mailbox[PLA_EPOCH / 4] = 10;
    ext->configure(instance, &io); api->sample(instance, &h);
    assert(ext->on_action(instance, "select_item", 0)); api->sample(instance, &h);
    f.aux_present = 0; f.reads = 0; api->tick(instance, &h);
    assert(f.reads == 0 && mailbox[PLA_CANCEL/4] == mailbox[PLA_REQ/4]);
    assert(!ext->on_action(instance, "select_item", 0));
    f.aux_present = 1;
    api->destroy(instance);
    f.Put(v + 0x6fc, unsigned(2));
    assert(!pla::ReadSelector(h).valid);
    f.Put(v + 0x6fc, unsigned(0));
    slots[1] = 5;
    f.Put(pb, slots);
    assert(!pla::ReadSelector(h).valid);
    slots[1] = 6;
    f.Put(pb, slots);
    assert(!pla::ReadSelector(h).valid);
    f.Put(v + 0x5ac, unsigned(2));
    assert(!pla::ReadSelector(h).valid);
    f.Put(0x220030, uint64_t(0));
    assert(!pla::ReadSelector(h).valid);
}
void Pa8Tests() {
    std::array<uint8_t, 0x178> plain{};
    auto put = [&](size_t off, uint32_t n, unsigned bytes = 2) {
        for (unsigned j = 0; j < bytes; ++j)
            plain[off + j] = n >> (8 * j);
    };
    put(8, 468);
    put(0x24, 0, 1);
    put(0x168, 100, 1);
    put(0x92, 111);
    put(0x16a, 333);
    put(0x16c, 101);
    put(0x16e, 202);
    put(0x170, 303);
    put(0x172, 404);
    put(0x174, 505);
    put(0x3e, 94);
    put(0x20, 24, 1);
    put(0x21, 3, 1);
    for (unsigned j = 0; j < 4; ++j) {
        put(0x54 + 2 * j, 100 + j);
        put(0x5c + j, 20 - j, 1);
        put(0x86 + j, j, 1);
    }
    put(0xa4, 0, 1);
    put(0xa5, 1, 1);
    put(0xa6, 2, 1);
    put(0xa7, 7, 1);
    put(0xa8, 4, 1);
    put(0xa9, 5, 1);
    for (unsigned j = 0; j < 8; ++j) put(0x15d + j, 0x80 + j, 1);
    put(0x9c, 8, 4);
    put(0x94, 0x80000000u | 19u | (20u << 5) | (25u << 10) | (31u << 15) |
                  (26u << 20) | (30u << 25), 4);
    put(0x60, 'H');
    put(0x62, 0xe9);
    uint16_t checksum = 0;
    for (unsigned j = 8; j < 0x168; j += 2)
        checksum += pla::U16(plain, j);
    put(6, checksum);
    auto p = pla::ParsePa8(plain);
    assert(p.valid && p.species == 468 && p.hp == 111 && p.max_hp == 333 && p.level == 100 &&
           p.status == 8 && p.nicknamed && p.nickname == "Hé" && p.gender == 0 && p.shiny);
    assert(p.detail_valid && p.effort_valid && p.attack == 101 && p.defense == 202 &&
           p.speed == 303 && p.sp_attack == 404 && p.sp_defense == 505 &&
           p.nature == 24 && p.stat_alignment == 3 && p.alpha_move == 94);
    assert((p.moves == std::array<unsigned, 4>{100, 101, 102, 103}));
    assert((p.pp == std::array<unsigned, 4>{20, 19, 18, 17}));
    assert((p.pp_ups == std::array<unsigned, 4>{0, 1, 2, 3}));
    assert((p.ganbaru_values == std::array<unsigned, 6>{0, 1, 2, 4, 5, 7}));
    assert((p.ivs == std::array<unsigned, 6>{19, 20, 25, 26, 30, 31}));
    assert((p.effort_levels == std::array<unsigned, 6>{0, 2, 3, 6, 7, 10}));
    assert((p.mastered_record == std::array<uint8_t, 8>{0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87}));
    const auto initial=plain;
    const auto expected_details=p;
    auto recheck = [&]() {
        uint16_t sum = 0;
        for (unsigned j = 8; j < 0x168; j += 2) sum += pla::U16(plain, j);
        put(6, sum);
        return pla::ParsePa8(plain);
    };
    // Invalid detail fields leave the dock identity and HP readable.
    for (const auto [offset, value] : {std::pair{0x20u, 25u}, std::pair{0x21u, 25u},
                                     std::pair{0xa4u, 11u}, std::pair{0x86u, 4u}}) {
        put(offset, value, 1);
        const auto refused = recheck();
        assert(refused.valid && !refused.detail_valid && refused.hp == 111);
        plain = initial;
    }
    put(0x3e, 851);
    assert(recheck().valid && !recheck().detail_valid);
    plain = initial;
    put(0x16c, 0);
    assert(recheck().valid && !recheck().detail_valid);
    plain = initial;
    put(0x13e, 1, 1);
    assert(recheck().valid && recheck().detail_valid && !recheck().effort_valid);
    plain = initial;
    // Base-IV effort bias at all boundaries; the total is capped at10.
    for (const auto [iv, bias] : {std::pair{0u, 0u}, std::pair{19u, 0u}, std::pair{20u, 1u},
                                std::pair{25u, 1u}, std::pair{26u, 2u}, std::pair{30u, 2u},
                                std::pair{31u, 3u}}) {
        put(0x94, 0x80000000u | iv, 4);
        put(0xa4, 0, 1);
        assert(recheck().effort_levels[0] == bias);
        put(0xa4, 10, 1);
        assert(recheck().effort_levels[0] == 10);
        plain = initial;
    }
    auto changed = expected_details;
    assert(changed == expected_details);
    changed.pp[0]--;
    assert(!(changed == expected_details));
    put(0x22,4,1); // female, PA8 gender bits 2..3
    put(0x1c,16); // shiny xor=16 is the first non-shiny value
    uint16_t gender_sum=0;
    for (unsigned j=8; j<0x168; j+=2) gender_sum += pla::U16(plain,j);
    put(6,gender_sum);
    p=pla::ParsePa8(plain);
    assert(p.valid && p.gender == 1 && !p.shiny);
    plain=initial;
    // Exercise every shuffle index using inverse permutations.
    // UTF-16 surrogate pairs round-trip to UTF-8; lone surrogates fail closed.
    auto original = plain;
    put(0x60, 0xd83d);
    put(0x62, 0xde00);
    uint16_t unicodeSum = 0;
    for (unsigned j = 8; j < 0x168; j += 2)
        unicodeSum += pla::U16(plain, j);
    put(6, unicodeSum);
    assert(pla::ParsePa8(plain).nickname == "\xf0\x9f\x98\x80");
    put(0x62, 'A');
    unicodeSum = 0;
    for (unsigned j = 8; j < 0x168; j += 2)
        unicodeSum += pla::U16(plain, j);
    put(6, unicodeSum);
    assert(!pla::ParsePa8(plain).valid);
    plain = original;
    constexpr uint8_t order[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
        {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
        {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0}};
    for (unsigned sv = 0; sv < 32; ++sv) {
        put(0, sv << 13, 4);
        auto encrypted = plain;
        for (unsigned j = 0; j < 4; ++j)
            std::copy_n(plain.begin() + 8 + j * 0x58, 0x58,
                        encrypted.begin() + 8 + order[sv % 24][j] * 0x58);
        // Independent LCG loop, including a separately seeded party tail.
        for (auto range : {std::pair{8u, 0x168u}, std::pair{0x168u, 0x178u}}) {
            uint32_t seed = sv << 13;
            for (unsigned j = range.first; j < range.second; j += 2) {
                seed = seed * 0x41c64e6d + 0x6073;
                encrypted[j] ^= seed >> 16;
                encrypted[j + 1] ^= seed >> 24;
            }
        }
        p = pla::ParsePa8(encrypted);
        assert(p.valid && p.species == 468 && p.hp == 111 && p.level == 100 && p.nickname == "Hé");
        // Native storage stays shuffled even in its temporarily decrypted state.
        const auto native_encrypted=pla::ParseNativePa8(std::span(encrypted).first(0x168),
            std::span(encrypted).subspan(0x168),1);
        assert(native_encrypted == expected_details);
        assert(native_encrypted.valid && native_encrypted.species == 468 &&
               native_encrypted.hp == 111 && native_encrypted.max_hp == 333 &&
               native_encrypted.level == 100 && native_encrypted.nickname == "Hé");
        auto shuffled=encrypted;
        pla::Crypt(std::span(shuffled).subspan(8,0x160),sv<<13);
        pla::Crypt(std::span(shuffled).subspan(0x168),sv<<13);
        assert(pla::CheckPa8(shuffled)); // Sum does not detect block order.
        const auto native_clear=pla::ParseNativePa8(std::span(shuffled).first(0x168),
            std::span(shuffled).subspan(0x168),0);
        assert(native_clear == expected_details);
        assert(native_clear.valid && native_clear.species == 468 && native_clear.hp == 111 &&
               native_clear.max_hp == 333 && native_clear.level == 100 &&
               native_clear.nickname == "Hé");
        assert(!pla::ParseNativePa8(std::span(shuffled).first(0x167),
            std::span(shuffled).subspan(0x168),0).valid);
        assert(!pla::ParseNativePa8(std::span(shuffled).first(0x168),
            std::span(shuffled).subspan(0x169),0).valid);
        assert(!pla::ParseNativePa8(std::span(shuffled).first(0x168),
            std::span(shuffled).subspan(0x168),2).valid);
        encrypted[8] ^= 1;
        assert(!pla::ParsePa8(encrypted).valid);
    }
    assert(!pla::ParsePa8(std::span(plain).first(0x177)).valid);
    plain[4] = 1;
    assert(!pla::ParsePa8(plain).valid);
    plain[4] = 0;
    plain[0x168] = 101;
    assert(!pla::ParsePa8(plain).valid);
    // Publication clearing and slot-indirection: list index 0 means physical
    // slot 5.
    Fake f;
    auto h = Host(f);
    pla::Party party;
    party.valid = true;
    party.count = 6;
    plain[0x168] = 100;
    party.slots[5] = pla::ParsePa8(plain);
    pla::Selector selector;
    selector.valid = true;
    selector.mode = 1;
    selector.party_count = 1;
    selector.party[0] = 5;
    pla::PublishDock(h, party, selector);
    assert(f.output["pla.party.5.selected"] == 1 && f.output["pla.party.0.selected"] == 0);
    pla::PublishDock(h, {}, selector);
    assert(f.texts["pla.hero"] == "module:pla:hero_slot:6");
    pla::PublishDock(h, {}, {});
    assert(f.output["pla.party.5.valid"] == 0 && f.output["pla.party.5.hp"] == 0 &&
           f.texts["pla.hero"].empty());
}

void BlockDecodeTests() {
    auto encrypted = [](std::vector<uint8_t> bytes) {
        pla::CryptBlock(bytes, 0x717ddaa3);
        return pla::DecodeBlock(bytes, 0x717ddaa3);
    };
    for (uint8_t type : {1, 2, 3}) {
        assert(encrypted({type}).type == type);
        assert(!encrypted({type, 0}).type);
    }
    assert(encrypted({11, 1, 2, 3, 4, 5, 6, 7, 8}).payload.size() == 8);
    assert(!encrypted({11, 1}).type);
    assert(!encrypted({0}).type && !encrypted({6}).type);
    assert(!encrypted({4, 255, 255, 255, 255}).type);
    assert(!encrypted({4, 0, 0, 0}).type);
    assert(!encrypted({4, 0, 0, 0, 0, 1}).type);
    assert(encrypted({4, 0, 0, 0, 0}).type == 4);
    assert(encrypted({5, 2, 0, 0, 0, 9, 1, 2, 3, 4}).payload.size() == 4);
    assert(!encrypted({5, 255, 255, 255, 255, 9}).type);
    assert(!encrypted({5, 0, 0, 0, 0, 6}).type);
    assert(!pla::DecodeBlock({}, 1).type);
}

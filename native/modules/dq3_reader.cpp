// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Field party reader for Dragon Quest III HD-2D Remake 1.1.0.0. Routes and rules: dq3_reader.h.

#include "dq3_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "dq3_font.h"
#include "dq3_layout.h"
#include "dq3_sprites.h"

namespace dq3 {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

constexpr u64 AccessorGd = 0x87437c;
constexpr u64 AccessorNames = 0x112d84c;
constexpr u32 AdrpX23 = 0x90000017, AddX23X23 = 0x910002f7;
constexpr u64 PoolBlocks = 0x28;
// UE4 reflection layout (verified live by class/object names, contracts/LOCATION.md).
using layout::ObjName, layout::ObjOuter, layout::WorldSubsystems;
constexpr u64 EngineViewport = 0x7a0, ViewportWorld = 0x70;
constexpr u64 GLocalPlayers = 0x38, PlayerController = 0x30, AcknowledgedPawn = 0x2a0;
constexpr u64 LlmLoading = 0x1b1;
constexpr u32 AdrpMask = 0x9f00001f, AdrpX8 = 0x90000008;
constexpr u32 AddImmMask = 0xffc003ff, AddX8X8 = 0x91000108;
constexpr u32 AdrpX9 = 0x90000009, AddX9X9 = 0x91000129;
constexpr u64 LooksCode = 0x8d0398, JobCode = 0xce274c;
constexpr u64 ClassTableOff = 0xe08; // ldr x9,[x9,#0xe08] / ldr w10,[x9,#0xe10]
constexpr u64 JobTable = 0x4909fba, JobTargetBase = 0xce2774, JobRoto = 0xce2784;
constexpr u64 CBag = 0x78, BagElems = 0x38, BagNum = 0x40, BagBits = 0x48, BagBitsHeap = 0x58,
              BagNumFree = 0x6c;
constexpr u64 PFlags = layout::PProgress, RotoFlag = 0x8b; // P+0x128+0xC8

// Field offsets, each pinned by the code above (see Pins()).
constexpr u64 GdOff = layout::GGd, POff = layout::GdP;
constexpr u64 PmOff = layout::PPartyManager, RosterData = 0x10, RosterNum = 0x18;
constexpr u64 CName = 0x28, CField = 0x58, CFlag9c = 0x9c, CMember = 0xc0, CSlot = 0xc4;
constexpr u64 FLevel = 0x1c, FMaxHp = 0x28, FHp = 0x2c, FMaxMp = 0x30, FMp = 0x34;
constexpr u64 FBonusHp = 0x68, FBonusMp = 0x6c, FStatus = 0x8c;
constexpr s32 Cap = 999; // GetMaxHP/MP clamp (mov w9,#0x3e7)
constexpr s32 MaxRoster = 64;
constexpr s32 MaxNameUnits = 33; // incl. NUL

using detail::Get;

// ReadFName results by (index, number) for one pool (dq3_reader.h).
struct FNameCache {
    std::mutex m;
    u64 pool{};
    std::unordered_map<u64, std::string> map;
};
FNameCache& Names() {
    static FNameCache cache;
    return cache;
}
constexpr std::size_t FNameCacheMax = 4096;

} // namespace

std::uint64_t AdrpPage(std::uint64_t pc, std::uint32_t adrp) {
    s64 imm = static_cast<s64>(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3));
    if (imm & (s64{1} << 20))
        imm -= s64{1} << 21;
    return (pc & ~u64{0xfff}) + static_cast<u64>(imm * 4096);
}

std::string CheckPins(const GuestRead& read, u64 main_base, u64 main_size, std::span<const CodePin> pins,
                      std::string_view label,
                      const std::function<void(const CodePin&, std::span<const u32>)>& on_match) {
    for (const CodePin& pin : pins) {
        if (pin.offset + pin.words.size() * 4 > main_size)
            return std::string{label} + " outside main";
        std::vector<u32> words(pin.words.size());
        if (!read(main_base + pin.offset, words.data(), words.size() * 4))
            return std::string{label} + " read";
        for (std::size_t i = 0; i < words.size(); ++i) {
            const u32 mask = pin.masks.empty() ? 0xffffffffu : pin.masks[i];
            if ((words[i] & mask) != pin.words[i]) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), " main+0x%llx word %zu = %08x",
                              static_cast<unsigned long long>(pin.offset), i, words[i]);
                return std::string{label} + buf;
            }
        }
        if (on_match)
            on_match(pin, words);
    }
    return {};
}

const std::vector<CodePin>& Pins() {
    static const std::vector<CodePin> pins{
        // GD accessor; ADRP page and ADD offset immediates vary (masked), the rest is exact.
        {AccessorGd,
         {AdrpX8, AddX8X8, 0xf9400108, 0xf9429508, 0x91006100, 0xd65f03c0},
         {AdrpMask, AddImmMask, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
        // FName::ToString: pool page/offset masked; blocks at +0x28, index >> 16 / & 0xffff.
        {AccessorNames,
         {AdrpX23, AddX23X23, 0xaa1503e1, 0xaa1703e0, 0xaa1403e2, 0x97ffe5ac, 0x53107c08,
          0x531f3c09, 0x8b284ee8, 0x2a0003f6, 0xaa1503e1, 0xf9401508},
         {AdrpMask, AddImmMask, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
          0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}},
        {0x112b938,
         {0xb9400288, 0x531f7909, 0xd34dfd08, 0x927d3d08, 0x927f3d29, 0x8b0802a8, 0xf9401508,
          0x8b090115, 0x794002a8, 0x37000548},
         {}},
        {0x112b970, {0x794002aa, 0xd346fd42}, {}},
        {0x8bc1c8, {0xf9411100, 0xf9411515}, {}},
        {0x8b10e0, {0xf8410edb, 0xb9800ac8}, {}},
        {0x8b114c, {0xf9400368, 0x39427109, 0x34000309}, {}},
        {0x8b11b4, {0x39430109, 0x51000529, 0x1ac926a9, 0x3607fc09}, {}},
        {0x8b11d4, {0xb940c519}, {}},
        {0x8d08d0,
         {0xf9402ea8, 0xb4000148, 0xb9408d08, 0x370000c8, 0x121e0109, 0xb9006be9, 0x360800a8,
          0x321f0128},
         {}},
        {0x89d30c,
         {0xb9402818, 0xaa0003f4, 0x36000121, 0xb9406a88, 0x52807ce9, 0x0b180108, 0x710f9d1f,
          0x1a89b108, 0x7100011f, 0x1a9fc100},
         {}},
        {0x89d5ac,
         {0xb9403018, 0xaa0003f4, 0x36000121, 0xb9406e88, 0x52807ce9, 0x0b180108, 0x710f9d1f,
          0x1a89b108, 0x7100011f, 0x1a9fc100},
         {}},
        {0x7240fc, {0xb841cd00}, {}},
        {0x716624, {0xb9402d08}, {}},
        {0x7165fc, {0xb9403508}, {}},
        // GetLooksId: vocation, class FString table (ADRP/ADD masked), coffin test, looks, +0xe08/+0xe10
        {LooksCode,
         {0x3942a808, 0x1280000a, 0x2a0203f5, 0xaa0003f4, AdrpX9, AddX9X9, 0x8b0a010a, 0x9001f548,
          0x9120e108, 0x360000a1, 0xf9402e8b, 0xb400006b, 0x3942316b, 0x370004cb, 0x8b2a5129,
          0xb940ba82, 0x9001f780, 0x91296800, 0xb94e112a, 0xf9470529, 0x7100015f, 0x9a890101},
         {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, AdrpMask, AddImmMask, 0xffffffff,
          0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
          0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
          0xffffffff}},
        // GetLooksId: equipped armour (bag C+0x78, map elements B+0x38, 32-byte stride, key 4)
        {0x8d03fc,
         {0xf9403e88, 0xb4001bc8, 0xb9404109, 0xb9406d0a, 0x6b0a013f, 0x54001b40, 0xf9403d0a,
          0x9101c109, 0xf100015f, 0x9a8a0129, 0xb940810a, 0x5100054a, 0x121e014a, 0xb86a5929,
          0x3100053f, 0x54001a00, 0xf9401d08, 0x93407d29, 0x8b091509, 0x3940012a, 0x7100115f},
         {}},
        // HP state thresholds: HP < 1, max < 1, r = HP / max, 0.5f (fmov), 0.3f (0x3e99999a)
        {0xce252c,
         {0xb9402e88, 0x7100051f, 0x5400036b, 0xaa1403e0, 0x2a1f03e1, 0x52800022, 0x97eeeb6b,
          0x7100041f, 0x540006ab, 0xbd402e80, 0xaa1403e0, 0x2a1f03e1, 0x52800022, 0x5e21d808,
          0x97eeeb63, 0x1e220000, 0x1e2c1001, 0x52933349, 0x72a7d329, 0x1e201900, 0x1e212000,
          0x1e270121, 0x52800069, 0x1a9f87e8, 0x531e7508, 0x1e212000, 0x1a898114},
         {}},
        // Erdrick title flag: GD accessor, flag 0x8b, bitset P+0x128; bit test with +0xc8
        {0x84a858, {0xa9bf7bfd, 0x910003fd, 0x9400a6c7, 0xf9400008, 0x52801161, 0x9104a100}, {}},
        {0x877570,
         {0x12003c28, 0x7102b11f, 0x54000108, 0x53037c28, 0x927d2508, 0x8b080008, 0xf9406508,
          0x9ac12508, 0x12000100},
         {}},
        // GetJobTextId (vocation 1..10 jump table) and its byte table in rodata
        {JobCode,
         {0x12001c08, 0x51000508, 0x7100251f, 0x54000428, 0xf001e129, 0x913ee929, 0x1000008a,
          0x3868692b, 0x8b0b094a, 0xd61f0140, 0x9001da73, 0x9127f273, 0x360000c1, 0x97eda036,
          0xb001d288, 0x9114d908, 0x7200001f, 0x9a931113},
         {}},
        {0x4909fb8, {0x1f000000, 0x22110e0b, 0x17281425}, {0xffff0000, 0xffffffff, 0xffffffff}},
        // LevelLoadingManager load-state byte +0x1B1: clear / set 1 / set 2 via the LLM getter 0x9181b4
        {0x91d5d4,
         {0xa9bf7bfd, 0x910003fd, 0x97ffeaf6, 0x3906c41f, 0xa8c17bfd, 0x140f4592, 0xa9bf7bfd,
          0x910003fd, 0x97ffeaf0, 0x52800028, 0x3906c408, 0xa8c17bfd, 0xd65f03c0, 0xa9bf7bfd,
          0x910003fd, 0x97ffeae9, 0x52800048, 0x3906c408, 0xa8c17bfd, 0xd65f03c0},
         {}},
    };
    return pins;
}

std::string Resolve(const GuestRead& read, u64 main_base, u64 main_size, Roots& roots) {
    roots = {};
    if (!read || main_base == 0 || main_size < 0x1000000)
        return "no main image";
    std::array<u32, 2> gd{}, names{}, classes{};
    const std::string bad =
        CheckPins(read, main_base, main_size, Pins(), "code pin", [&](const CodePin& pin, std::span<const u32> words) {
            if (pin.offset == AccessorGd)
                gd = {words[0], words[1]};
            if (pin.offset == AccessorNames)
                names = {words[0], words[1]};
            if (pin.offset == LooksCode)
                classes = {words[4], words[5]};
        });
    if (!bad.empty())
        return bad;
    // ADRP + ADD: the ADRP page plus the ADD's 12-bit immediate.
    const auto target = [main_base](u64 at, u32 adrp, u32 add) {
        return AdrpPage(main_base + at, adrp) + ((add >> 10) & 0xfff);
    };
    const u64 slot = target(AccessorGd, gd[0], gd[1]);
    const u64 pool = target(AccessorNames, names[0], names[1]);
    if (slot < main_base || slot + 8 > main_base + main_size)
        return "G slot outside main";
    if (pool < main_base || pool + PoolBlocks + 8 > main_base + main_size)
        return "name pool outside main";
    const u64 table = target(LooksCode + 16, classes[0], classes[1]) + ClassTableOff;
    if (table < main_base || table + 16 * 10 > main_base + main_size)
        return "class table outside main";
    roots.g_slot = slot;
    roots.name_pool = pool;
    roots.class_table = table;
    // The job text ids: each jump target loads its string with ADRP + ADD; read them back.
    roots.job_ids_ok = true;
    u8 jump[10]{};
    if (!read(main_base + JobTable, jump, sizeof(jump)))
        roots.job_ids_ok = false;
    const auto job_string = [&](u64 at) -> std::optional<std::string> {
        u32 w[2]{};
        if (!read(main_base + at, w, sizeof(w)) || (w[0] & AdrpMask & ~0x1fu) != 0x90000000 ||
            (w[1] & 0xffc00000) != 0x91000000)
            return std::nullopt;
        const u64 str = target(at, w[0], w[1]);
        std::uint16_t units[48]{};
        if (str < main_base || str + sizeof(units) > main_base + main_size ||
            !read(str, units, sizeof(units)))
            return std::nullopt;
        std::string out;
        for (const std::uint16_t c : units) {
            if (c == 0)
                return out;
            if (c >= 0x80)
                return std::nullopt;
            out += static_cast<char>(c);
        }
        return std::nullopt;
    };
    for (u8 v = 1; v <= 10 && roots.job_ids_ok; ++v) {
        const auto got = job_string(JobTargetBase + u64{jump[v - 1]} * 4);
        roots.job_ids_ok = got && *got == *JobTextId(v, false);
    }
    if (roots.job_ids_ok) {
        const auto roto = job_string(JobRoto);
        roots.job_ids_ok = roto && *roto == *JobTextId(1, true);
    }
    return {};
}

void ClearFNameCache() {
    auto& c = Names();
    std::scoped_lock lock{c.m};
    c.map.clear();
    c.pool = 0;
}

bool ReadGdP(const GuestRead& read, const Roots& roots, u64& gd, u64& p, u64* g_out) {
    u64 g = 0;
    gd = p = 0;
    if (!Get(read, roots.g_slot, g) || !Get(read, g + layout::GGd, gd) || !gd || !Get(read, gd + layout::GdP, p) ||
        !p)
        return false;
    if (g_out)
        *g_out = g;
    return true;
}

std::optional<std::string> ReadFName(const GuestRead& read, u64 name_pool, u32 index, u32 number) {
    const u64 key = u64{index} | u64{number} << 32;
    auto& cache = Names();
    {
        std::scoped_lock lock{cache.m};
        if (cache.pool == name_pool)
            if (const auto it = cache.map.find(key); it != cache.map.end())
                return it->second;
    }
    u64 block = 0;
    if ((index >> 16) >= 8192) // FNameMaxBlocks
        return std::nullopt;
    if (!Get(read, name_pool + PoolBlocks + 8 * u64{index >> 16}, block) || block == 0)
        return std::nullopt;
    const u64 entry = block + u64{index & 0xffff} * 2;
    std::uint16_t header = 0;
    if (!Get(read, entry, header))
        return std::nullopt;
    const u32 len = header >> 6;
    if (len == 0 || len > 1024)
        return std::nullopt;
    std::string out;
    if (header & 1) {
        std::vector<std::uint16_t> units(len);
        if (!read(entry + 2, units.data(), len * 2))
            return std::nullopt;
        const auto text = FromUtf16(units);
        if (!text)
            return std::nullopt;
        for (const char32_t c : *text)
            out += c < 0x80 ? static_cast<char>(c) : '?';
    } else {
        out.resize(len);
        if (!read(entry + 2, out.data(), len))
            return std::nullopt;
    }
    if (number)
        out += "_" + std::to_string(number - 1);
    std::scoped_lock lock{cache.m};
    if (cache.pool != name_pool) {
        cache.map.clear();
        cache.pool = name_pool;
    }
    if (cache.map.size() >= FNameCacheMax)
        cache.map.clear();
    cache.map.emplace(key, out);
    return out;
}

bool ReadScene(const GuestRead& read, const Roots& roots, SceneState& out, SceneCache* cache) {
    out = {};
    u64 g = 0, engine = 0, viewport = 0, world = 0, players = 0, player = 0, pc = 0, pawn = 0;
    if (!Get(read, roots.g_slot, g) || !Get(read, g + ObjOuter, engine) ||
        !Get(read, engine + EngineViewport, viewport) || !Get(read, viewport + ViewportWorld, world))
        return false;
    out.world_ptr = world;
    SceneCache local;
    SceneCache& c = cache ? *cache : local;
    u32 name[2]{};
    if (!read(world + ObjName, name, sizeof(name)))
        return false;
    if (c.world_ptr != world || c.world_name.empty() || c.world_fname[0] != name[0] ||
        c.world_fname[1] != name[1]) {
        const auto world_name = ReadFName(read, roots.name_pool, name[0], name[1]);
        if (!world_name)
            return false;
        c = {world, 0, *world_name, {name[0], name[1]}};
    }
    out.world = c.world_name;
    if (Get(read, g + GLocalPlayers, players) && Get(read, players, player) &&
        Get(read, player + PlayerController, pc) && Get(read, pc + AcknowledgedPawn, pawn))
        out.pawn = pawn != 0;
    out.pawn_ptr = pawn;
    if (out.world != "FieldTop")
        return true;
    // LevelLoadingManager: looked up by class name once per world object, re-checked by pointer.
    u64 subs = 0;
    s32 nsubs = 0;
    if (!Get(read, world + WorldSubsystems, subs) || !Get(read, world + WorldSubsystems + 8, nsubs) ||
        nsubs <= 0 || nsubs > 64 || subs == 0)
        return true;
    std::vector<u64> entries(static_cast<std::size_t>(nsubs) * 3);
    if (!read(subs, entries.data(), entries.size() * 8))
        return true;
    u64 llm = 0;
    for (s32 k = 0; k < nsubs && !llm; ++k) {
        const u64 cls = entries[static_cast<std::size_t>(k) * 3], obj = entries[static_cast<std::size_t>(k) * 3 + 1];
        if (c.llm) {
            if (obj == c.llm)
                llm = obj;
            continue;
        }
        u32 name[2]{};
        if (cls == 0 || obj == 0 || !read(cls + ObjName, name, sizeof(name)))
            continue;
        if (ReadFName(read, roots.name_pool, name[0], name[1]) == std::string{"LevelLoadingManager"})
            llm = obj;
    }
    c.llm = llm;
    out.llm = llm;
    if (llm && Get(read, llm + LlmLoading, out.loading))
        out.llm_found = true;
    return true;
}

namespace {
// The equipped armour (map key 4) from the bag's equipment TSparseArray (contracts/INVENTORY.md).
void ReadArmour(const GuestRead& read, u64 bag, PartyMember& m) {
    m.armour_read = false;
    m.armour[0] = m.armour[1] = 0;
    u8 h[BagNumFree + 4 - BagElems];
    if (bag == 0 || !read(bag + BagElems, h, sizeof(h)))
        return;
    const auto at = [&h](u64 off) { return h + (off - BagElems); };
    u64 elems = 0, heap_bits = 0;
    s32 num = 0, num_free = 0;
    std::memcpy(&elems, at(BagElems), 8);
    std::memcpy(&num, at(BagNum), 4);
    std::memcpy(&num_free, at(BagNumFree), 4);
    std::memcpy(&heap_bits, at(BagBitsHeap), 8);
    // guest values: bound both before adding them (a torn read must not overflow the sum)
    if (num < 0 || num > 32 || num_free < 0 || num_free > 32)
        return;
    const s32 span = num + num_free;
    if (span > 32 || (span > 0 && elems == 0))
        return;
    u32 bits = 0;
    if (heap_bits) {
        if (!Get(read, heap_bits, bits))
            return;
    } else {
        std::memcpy(&bits, at(BagBits), 4);
    }
    if (span == 0) {
        m.armour_read = true;
        return;
    }
    std::vector<u8> el(static_cast<std::size_t>(span) * 0x20);
    if (!read(elems, el.data(), el.size()))
        return;
    for (s32 i = 0; i < span; ++i) {
        if (!((bits >> i) & 1))
            continue;
        const u8* e = el.data() + static_cast<std::size_t>(i) * 0x20;
        if (e[0] != 4) // armour slot
            continue;
        u64 slot = 0, def = 0;
        std::memcpy(&slot, e + 8, 8);
        u32 name[2]{};
        if (!Get(read, slot + 8, def) || def == 0 || !read(def + 8, name, sizeof(name)))
            return;
        m.armour[0] = name[0];
        m.armour[1] = name[1];
        break;
    }
    m.armour_read = true;
}
} // namespace

int HpState(s32 hp, s32 max) {
    if (hp < 1)
        return 2;
    if (max < 1)
        return 0;
    const float r = static_cast<float>(hp) / static_cast<float>(max); // fdiv s0, s8, s0
    if (!(r > 0.3f)) // fcmp + csel hi: r <= 0.3f (or unordered) -> 3
        return 3;
    return r <= 0.5f ? 4 : 0; // cset ls
}

int HpState(const PartyMember& m) {
    return m.Dead() ? 2 : HpState(m.hp, m.hp_max);
}

std::optional<std::string> ReadClassName(const GuestRead& read, const Roots& roots, u8 vocation) {
    if (vocation < 1 || vocation > 10 || roots.class_table == 0)
        return std::nullopt;
    const u64 entry = roots.class_table + 16 * u64{static_cast<u32>(vocation - 1)};
    u64 data = 0;
    s32 num = 0;
    if (!Get(read, entry, data) || !Get(read, entry + 8, num))
        return std::nullopt;
    if (num == 0)
        return std::string{}; // the game formats L"" then
    if (num < 2 || num > 32 || data == 0)
        return std::nullopt;
    std::vector<std::uint16_t> units(static_cast<std::size_t>(num));
    if (!read(data, units.data(), units.size() * 2) || units.back() != 0)
        return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i + 1 < units.size(); ++i) {
        if (units[i] < 0x20 || units[i] >= 0x7f)
            return std::nullopt;
        out += static_cast<char>(units[i]);
    }
    return out;
}

bool IsCostumeArmour(std::string_view armour) {
    // GOP_Item rows of type EQUIP_ARMOR whose UseSpecId has EffectType COSUTUME_CHANGE.
    return armour == "ITEM_EQUIP_ARMOR_CAT_SUIT" || armour == "ITEM_EQUIP_ARMOR_SCANDALOUS_SWIMSUIT" ||
           armour == "ITEM_EQUIP_ARMOR_MAGIC_BIKINI" || armour == "ITEM_EQUIP_ARMOR_BLESSED_BIKINI";
}

std::string LooksId(const PartyMember& m, std::string_view class_name, std::string_view armour) {
    if (m.Dead())
        return "UNIT_LOOKS_PC_COFFIN_" + std::string{class_name};
    char looks[16];
    std::snprintf(looks, sizeof(looks), "%02d", static_cast<int>(m.looks));
    std::string id = "UNIT_LOOKS_PC_" + std::string{class_name} + "_" + looks;
    if (IsCostumeArmour(armour))
        return armour == "ITEM_EQUIP_ARMOR_CAT_SUIT" ? std::string{"UNIT_LOOKS_PC_NUIGURUMI_00"}
                                                     : id + "_MIZUGI";
    return id;
}

s32 DisplayedMax(s32 base, s32 bonus) {
    s32 v = static_cast<s32>(static_cast<u32>(base) + static_cast<u32>(bonus)); // add w8,w8,w24
    v = v < Cap ? v : Cap;
    return v > 0 ? v : 0;
}

s32 AddClamped(s32 a, s32 b) {
    const std::int64_t v = std::int64_t{a} + b;
    return static_cast<s32>(std::clamp<std::int64_t>(v, INT32_MIN, INT32_MAX));
}

PartyState ReadPartyOnce(const GuestRead& read, u64 g_slot, PartySnapshot& out, std::string* why) {
    const auto state = [why](PartyState s, const char* reason) {
        if (why)
            *why = reason;
        return s;
    };
    out = {};
    u64 g = 0, gd = 0, p = 0, pm = 0, data = 0;
    s32 n = 0;
    if (!Get(read, g_slot, g))
        return state(PartyState::Invalid, "G slot unreadable");
    if (g == 0)
        return state(PartyState::NoGame, "no game instance");
    if (!Get(read, g + GdOff, gd))
        return state(PartyState::Invalid, "G+0x528 unreadable");
    if (gd == 0)
        return state(PartyState::NoGame, "no game data");
    if (!Get(read, gd + POff, p))
        return state(PartyState::Invalid, "GD+0x18 unreadable");
    if (p == 0)
        return state(PartyState::NoGame, "no progress record");
    if (!Get(read, p + PmOff, pm))
        return state(PartyState::Invalid, "P+0x220 unreadable");
    if (pm == 0)
        return state(PartyState::NoGame, "no party manager");
    u64 flags = 0;
    if (!Get(read, p + PFlags + 8 * (RotoFlag >> 6), flags))
        return state(PartyState::Invalid, "progress flags unreadable");
    out.roto = ((flags >> (RotoFlag & 63)) & 1) != 0;
    if (!Get(read, pm + RosterData, data) || !Get(read, pm + RosterNum, n))
        return state(PartyState::Invalid, "roster header unreadable");
    if (n == 0)
        return state(PartyState::NoGame, "empty roster");
    if (n < 0 || n > MaxRoster || data == 0)
        return state(PartyState::Invalid, "roster size");
    out.roster_count = n;
    std::vector<u64> entries(static_cast<std::size_t>(n) * 2);
    if (!read(data, entries.data(), entries.size() * 8))
        return state(PartyState::Invalid, "roster unreadable");
    bool slot_used[4]{};
    for (s32 k = 0; k < n; ++k) {
        const u64 c = entries[static_cast<std::size_t>(k) * 2];
        if (c == 0)
            return state(PartyState::Invalid, "null roster entry");
        u8 head[0xc8];
        if (!read(c, head, sizeof(head)))
            return state(PartyState::Invalid, "character unreadable");
        if (head[CFlag9c] != 0)
            ++out.flag9c_entries;
        if (head[CMember] != 1)
            continue;
        u32 slot = 0;
        std::memcpy(&slot, head + CSlot, 4);
        if (slot > 3 || slot_used[slot] || out.members.size() >= 4)
            return state(PartyState::Invalid, "party slots");
        slot_used[slot] = true;
        u64 f = 0, name_ptr = 0;
        s32 name_num = 0;
        std::memcpy(&f, head + CField, 8);
        std::memcpy(&name_ptr, head + CName, 8);
        std::memcpy(&name_num, head + CName + 8, 4);
        if (f == 0)
            return state(PartyState::Invalid, "member without field record");
        u8 rec[FStatus + 4 - FLevel];
        if (!read(f + FLevel, rec, sizeof(rec)))
            return state(PartyState::Invalid, "field record unreadable");
        const auto i32 = [&rec](u64 off) {
            s32 v;
            std::memcpy(&v, rec + (off - FLevel), 4);
            return v;
        };
        PartyMember m;
        m.slot = slot;
        m.field = f;
        m.level = i32(FLevel);
        m.hp = i32(FHp);
        m.mp = i32(FMp);
        m.hp_base = i32(FMaxHp);
        m.hp_bonus = i32(FBonusHp);
        m.mp_base = i32(FMaxMp);
        m.mp_bonus = i32(FBonusMp);
        m.hp_max = DisplayedMax(m.hp_base, m.hp_bonus);
        m.mp_max = DisplayedMax(m.mp_base, m.mp_bonus);
        m.status = static_cast<u32>(i32(FStatus));
        if (m.level < 1 || m.level > 99 || m.hp < 0 || m.hp > Cap || m.mp < 0 || m.mp > Cap)
            return state(PartyState::Invalid, "member values");
        if (name_num < 1 || name_num > MaxNameUnits || name_ptr == 0)
            return state(PartyState::Invalid, "name length");
        std::vector<std::uint16_t> units(static_cast<std::size_t>(name_num));
        if (!read(name_ptr, units.data(), units.size() * 2) || units.back() != 0)
            return state(PartyState::Invalid, "name unreadable");
        units.pop_back();
        const auto name = FromUtf16(units);
        if (!name || name->empty())
            return state(PartyState::Invalid, "name encoding");
        m.name = *name;
        m.vocation = head[0xaa];
        std::memcpy(&m.looks, head + 0xb8, 4);
        u64 bag = 0;
        std::memcpy(&bag, head + CBag, 8);
        ReadArmour(read, bag, m);
        out.members.push_back(std::move(m));
    }
    std::sort(out.members.begin(), out.members.end(),
              [](const PartyMember& a, const PartyMember& b) { return a.slot < b.slot; });
    return state(PartyState::Ok, "");
}

PartyState ReadParty(const GuestRead& read, u64 g_slot, PartySnapshot& out, std::string* why) {
    PartySnapshot again;
    const PartyState first = ReadPartyOnce(read, g_slot, out, why);
    std::string why2;
    const PartyState second = ReadPartyOnce(read, g_slot, again, &why2);
    if (first != second || !(out == again)) {
        if (why)
            *why = "torn";
        out = {};
        return PartyState::Invalid;
    }
    return first;
}

} // namespace dq3

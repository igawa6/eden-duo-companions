// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R camp-menu extras (build D4B1), all reproduced from the native menu code:
//  STATS    81CBC4 Baton Pass RANK (detail page, every member but the navigator), 81D34C/81D43C
//           "Down Shots left/max" (protagonist only), 8180C4 "Technical RANK n" (list page) and
//           its datTechnicalHelp line (818A90 -> 987F40 kind 12 -> DATMSG index 18).
//  CALENDAR 7779C0 Day Job / Night Job lists of the cursor day (cmpArbeitTable + the arbeit
//           unlock records 882410 + cmpCalArbeitDrawOffTable 7EBF60 / 777778).
//  CALENDAR 778F00 past cursor day -> the "Daily Log" (77AF2C): two lines 87F8F0(month, day, 0/1)
//           = bytes of the u16 day record main+22867FC[day index], each a cmmNetReportTable row
//           (86B330) drawn by 77EE30 (row +0 == 0: red text + underline), plus stickers.
//  REQUEST  7FD56C grade sprite (0x202 + difficulty, "?" while state 1), 7FA610 tab lists:
//           Recent (qsort 7FF070), Progress (7FEDC0), Difficulty (7FEF40); NEW badge 8946F0.
// Every code region used is verified byte-for-byte (FNV-1a over the D4B1 instructions) and every
// global is decoded from the instructions that access it; reads are bounded and read-only, and
// anything unexpected clears the affected output instead of guessing.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_social_menu_reader.h"

namespace p5r_data2 {
using namespace dsmod_sdk::int_types;

constexpr unsigned MemberIds = 11;   // member ids 0..10 (baton table 15E6490)
constexpr unsigned MaxJobs = 3;      // 7779C0 stops each list at 3 rows
constexpr unsigned ArbeitSlots = 10; // 841250 accepts ids 0..9
constexpr u16 FuncDownShot = 0xa1, FuncCheapShot = 0xa6;
constexpr u32 FlagBaton = 0x30000130, FlagTechnical = 0x30000131;
constexpr u32 CountDownShotsUsed = 0x13b, CountTechnical = 0xd4;
constexpr unsigned LogDays = 365;           // 87F8F0 answers 0 past day index 0x16C
constexpr unsigned TechnicalHelpIndex = 18; // datTechnicalHelp.bmd in the DATMSG.PAK name table

namespace code {
struct Range {
    u32 at, size;
    u64 hash;
};
enum Id : unsigned {
    GetCount,
    CountOverride,
    Baton,
    DownShot,
    DownShotGate,
    FuncState,
    FuncOn,
    RowMap,
    Exists,
    Remap,
    CmmLookup,
    CmmGetters,
    OpenGetters,
    JobList,
    DrawOff,
    DrawOffMonth,
    ArbeitGetters,
    ArbeitRec,
    ArbeitBase,
    DayIndex,
    Weekday,
    Technical,
    TechHelp,
    ListBuild,
    ProgressSort,
    DifficultySort,
    ProgressKey,
    Grade,
    NewFlag,
    LogPast,
    LogBlock,
    LogRecord,
    LogBase,
    NetReportRow,
    LogLine,
    Count
};
constexpr std::array<Range, Count> Ranges{{
    {0x882310, 0x4c, 0x7c9020663645fa2eull},  // GET_COUNT (override only for 0xE/0xF)
    {0x899440, 0x50, 0x7c195288f4c90cf7ull},  // GET_COUNT override
    {0x81cbc4, 0x7c, 0xaf5d5ac9b5336d28ull},  // STATS detail: Baton Pass rank
    {0x81d43c, 0x44, 0x21c1e596e308c71eull},  // STATS detail: Down Shots left/max
    {0x81d34c, 0x1c, 0x439e17980ee917e5ull},  // STATS detail: Down Shots gate (member 1)
    {0x85a1e0, 0x3ac, 0xbd1260bc2316fefaull}, // confidant function state
    {0x85a590, 0x1c, 0xb9d4427f5e184bdaull},  // function unlocked = state > 0
    {0x86aa60, 0x90, 0xbe7b7c71ea621defull},  // function row -> confidant id
    {0x857c00, 0x1c, 0x9440afe809e54ebaull},  // confidant exists
    {0x857c20, 0x38, 0x9dff629092f5a27cull},  // confidant id remap
    {0x857990, 0x20, 0xdc92c72497b7b673ull},  // confidant record lookup
    {0x86b150, 0x6c, 0x08f7bf4b59be976dull},  // cmmFunctionTable / count / info getters
    {0x86b290, 0x38, 0x075db54717f36825ull},  // cmmFunctionOpenSPTable getters
    {0x7779c0, 0x594, 0xd3328e30135912f3ull}, // CALENDAR job list builder
    {0x7ebf60, 0xd0, 0xf622fd4e062c8079ull},  // cmpCalArbeitDrawOffTable day test
    {0x777778, 0x64, 0x6619c7b749b8c921ull},  // month draw-off bits
    {0x7ed630, 0x60, 0x219a0b5664d6597eull},  // cmpArbeitTable / cmpArbeitName getters
    {0x841250, 0x148, 0x7ea5c0eaf3625df6ull}, // arbeit unlock record accessors
    {0x882410, 0xc, 0x9e8dca02c805e55bull},   // arbeit record base
    {0x721690, 0x8c, 0x0435c41a99f3a74cull},  // month/day -> day index
    {0x720ed0, 0x48, 0xae0248b10caf1a2bull},  // day index -> weekday
    {0x8180c4, 0x54, 0xbde02140144fc151ull},  // STATS list: Technical rank
    {0x818a90, 0x2c, 0xdd0c91acd86f515full},  // STATS list: technical help line
    {0x7fa610, 0x258, 0x2db41722ea4df52full}, // REQUEST tab lists
    {0x7fedc0, 0x180, 0x3335dd358fc2ad21ull}, // Progress tab sort
    {0x7fef40, 0x130, 0x32a6d9c96bf634c5ull}, // Difficulty tab sort
    {0x894870, 0x38, 0xbd5699148adddd77ull},  // Progress sort key
    {0x7fd56c, 0x60, 0xf47b39254fb476beull},  // grade sprite
    {0x8946f0, 0x44, 0xfed8a94ccfa17102ull},  // NEW flag
    {0x778efc, 0x30, 0x593a577ba3e602e7ull},  // CALENDAR: cursor day < today -> Daily Log
    {0x77af2c, 0x678, 0x4e0a00a8096462c9ull}, // Daily Log: two lines + sticker rules
    {0x87f8f0, 0x80, 0xe97d7b11454717bcull},  // day record byte (0 = first line, 1 = second)
    {0x881540, 0xc, 0x47c2c4046615d605ull},   // day record array base
    {0x86b330, 0x34, 0xf02bef2df418dee0ull},  // cmmNetReportTable row getter
    {0x77ee30, 0x130, 0x442acc2b0212f601ull}, // Daily Log line: row +0 == 0 -> red + underline
}};
} // namespace code

struct Roots {
    bool stats{}, jobs{}, requests{};
    u64 counters{};                             // s32 GET_COUNT array (882340 adrp/add + ldr #imm)
    u64 baton{};                                // u32 counter index per member id (81CC18)
    u64 cmm{};                                  // slot of the confidant record table (857998)
    u64 func_table{}, func_info{}, func_open{}; // CTD slots
    std::array<u8, 0x23> remap_a{}, remap_b{};  // 857C40 / 85A388 jump bytes (ids 3..0x25)
    u64 arbeit{}, arbeit_names{}, drawoff{};    // CTD slots
    u64 arbeit_records{};                // 882410 base (+4 + 4*id: u8 flags, u8, u8 month, u8 day)
    std::array<s32, 12> month_lengths{}; // 7216D4 table (Jan first)
    std::array<u8, 7> prio{};            // Progress priority by state (7FEDFC)
    bool log{};
    u64 log_records{}; // u16 per day index 0..364 (881540 adrp/add)
    u64 net_report{};  // cmmNetReportTable CTD slot (86B330 adrp/ldr)
};

template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using p5r_social::detail::Global;
    using p5r_social::detail::InMain;
    std::array<bool, code::Count> good{};
    std::vector<u8> buf;
    for (unsigned i = 0; i < code::Count; ++i) {
        const auto& r = code::Ranges[i];
        buf.resize(r.size);
        good[i] = InMain(main_base, main_size, main_base + r.at, r.size) &&
                  read(main_base + r.at, buf.data(), r.size) &&
                  dsmod_sdk::Fnv1a64(buf.data(), r.size) == r.hash;
    }
    auto word = [&](u64 off) -> u32 {
        u32 w{};
        return read(main_base + off, &w, 4) ? w : 0;
    };
    auto slot = [&](u64 pc, u32 expected, unsigned scale, u64& dst) {
        return Global(main_base, main_size, main_base + pc, word(pc), word(pc + 4), expected, scale,
                      dst);
    };
    constexpr u32 LdrX8 = 0xf9400108, AddX8 = 0x91000108, AddX0 = 0x91000000, AddX9 = 0x91000129,
                  AddX10 = 0x9100014a, AddX12 = 0x9100018c, AddX25 = 0x91000339;
    Roots r;
    // Counters: adrp/add x8 at 882340, then "ldr w0, [x8, #imm*4]" at 88234C (idx*4 added between).
    u64 counter_page{};
    const bool counters =
        good[code::GetCount] && good[code::CountOverride] && slot(0x882340, AddX8, 0, counter_page);
    if (counters)
        r.counters = counter_page + (u64((word(0x88234c) >> 10) & 0xfff) << 2);
    // Confidant-function state (85A1E0) and its tables.
    bool func = counters && good[code::FuncState] && good[code::FuncOn] && good[code::RowMap] &&
                good[code::Exists] && good[code::Remap] && good[code::CmmLookup] &&
                good[code::CmmGetters] && good[code::OpenGetters] &&
                slot(0x857998, LdrX8, 3, r.cmm) && slot(0x86b150, LdrX8, 3, r.func_table) &&
                slot(0x86b1a0, LdrX8, 3, r.func_info) && slot(0x86b2b0, LdrX8, 3, r.func_open);
    u64 remap_a{}, remap_b{}, count_slot{};
    func = func && slot(0x86b180, LdrX8, 3, count_slot) && count_slot == r.func_table &&
           slot(0x857c34, AddX10, 0, remap_a) && slot(0x85a37c, AddX9, 0, remap_b) &&
           read(remap_a, r.remap_a.data(), r.remap_a.size()) &&
           read(remap_b, r.remap_b.data(), r.remap_b.size());
    r.stats = func && good[code::Baton] && good[code::DownShot] && good[code::DownShotGate] &&
              good[code::Technical] && good[code::TechHelp] && slot(0x81cc18, AddX8, 0, r.baton) &&
              InMain(main_base, main_size, r.baton, 4 * MemberIds) &&
              InMain(main_base, main_size, r.counters, 4 * 0x400);
    // Jobs.
    u64 lengths{}, arbeit_count{};
    r.jobs = good[code::JobList] && good[code::DrawOff] && good[code::DrawOffMonth] &&
             good[code::ArbeitGetters] && good[code::ArbeitRec] && good[code::ArbeitBase] &&
             good[code::DayIndex] && good[code::Weekday] && slot(0x7ed630, LdrX8, 3, r.arbeit) &&
             slot(0x7ed650, LdrX8, 3, arbeit_count) && arbeit_count == r.arbeit &&
             slot(0x7ed670, LdrX8, 3, r.arbeit_names) && slot(0x7ebf74, LdrX8, 3, r.drawoff) &&
             slot(0x882410, AddX0, 0, r.arbeit_records) && slot(0x7216d4, AddX12, 0, lengths) &&
             InMain(main_base, main_size, r.arbeit_records, 4 + 4 * ArbeitSlots) &&
             read(lengths, r.month_lengths.data(), sizeof(r.month_lengths));
    for (auto l : r.month_lengths)
        r.jobs = r.jobs && l >= 28 && l <= 31;
    // Requests.
    u64 prio{};
    r.requests = good[code::ListBuild] && good[code::ProgressSort] && good[code::DifficultySort] &&
                 good[code::ProgressKey] && good[code::Grade] && good[code::NewFlag] &&
                 slot(0x7fedfc, AddX25, 0, prio) && read(prio, r.prio.data(), r.prio.size());
    // Daily Log (needs the jobs' month table for the day index).
    r.log = r.jobs && good[code::LogPast] && good[code::LogBlock] && good[code::LogRecord] &&
            good[code::LogBase] && good[code::NetReportRow] && good[code::LogLine] &&
            slot(0x881540, AddX0, 0, r.log_records) && slot(0x86b330, LdrX8, 3, r.net_report) &&
            InMain(main_base, main_size, r.log_records, 2 * LogDays);
    out = r;
    return r.stats || r.jobs || r.requests || r.log;
}

namespace detail {
using p5r_social::detail::Get;
template <class Read>
std::optional<s32> Counter(Read& read, const Roots& r, u32 index) {
    s32 v{};
    // 882310 answers 0xE/0xF from another table (899440); never needed here.
    if (index == 0xe || index == 0xf || index >= 0x400 || !Get(read, r.counters + 4ull * index, v))
        return std::nullopt;
    return v;
}
struct Record {
    u16 flags{}, rank{};
};
// 857990: 24 records at table+2+16*k, id at +4 (flags +2, rank +6).
template <class Read>
std::optional<std::optional<Record>> Confidant(Read& read, const Roots& r, u16 id) {
    u64 table{};
    std::array<u8, 2 + 16 * 24> raw{};
    if (!Get(read, r.cmm, table) || !table || !read(table, raw.data(), raw.size()))
        return std::nullopt;
    for (unsigned k = 0; k < 24; ++k) {
        u16 rid{};
        std::memcpy(&rid, raw.data() + 2 + 16 * k + 4, 2);
        if (id && rid == id) {
            Record rec;
            std::memcpy(&rec.flags, raw.data() + 2 + 16 * k + 2, 2);
            std::memcpy(&rec.rank, raw.data() + 2 + 16 * k + 6, 2);
            return std::optional<Record>{rec};
        }
    }
    return std::optional<Record>{};
}
template <class Read>
bool CtdBlock(Read& read, u64 slot, u32 entry, u64& data, u32& count, u32 max_count) {
    u64 base{};
    u32 magic{}, blocks{}, off{}, size{};
    if (!Get(read, slot, base) || !base || !Get(read, base + 4, magic) || magic != 0x46544430 ||
        !Get(read, base + 0xc, blocks) || blocks < 1 || blocks > 16 ||
        !Get(read, base + 0x10, off) || off < 0x14 || off > 0x1000 ||
        !Get(read, base + off + 4, size) || !Get(read, base + off + 8, count) ||
        count > max_count || u64(count) * entry != size)
        return false;
    data = base + off + 0x10;
    return true;
}
} // namespace detail

// 85A1E0 for one function id: -3 not in any table row, 0 locked, 1 unlocked, 2 early (OpenSP),
// -1/-2 reversed/broken gate, -4 disabled by the info flag. nullopt = could not reproduce.
template <class Read>
std::optional<int> FunctionState(Read& read, const Roots& r, const p5r_social_menu::Roots& flags,
                                 u64 main_base, u64 main_size, s16 func) {
    using namespace detail;
    auto bit = [&](u32 f) {
        return p5r_social_menu::BitCheck(read, flags, main_base, main_size, f);
    };
    u64 table{}, info{}, open{};
    u32 rows{}, infos{}, opens{};
    if (!CtdBlock(read, r.func_table, 0x78, table, rows, 64) ||
        !CtdBlock(read, r.func_info, 8, info, infos, 1024) ||
        !CtdBlock(read, r.func_open, 8, open, opens, 256) || func < 0 || u32(func) >= infos)
        return std::nullopt;
    if (rows < 2)
        return -3;
    std::array<u8, 0x78> row{};
    unsigned found_row = 0, found_k = 0;
    for (unsigned id = 1; id < rows && !found_row; ++id) {
        if (!read(table + 0x78ull * id, row.data(), row.size()))
            return std::nullopt;
        for (unsigned k = 0; k < 10; ++k) {
            s16 rank{}, fn{};
            std::memcpy(&rank, row.data() + 12 * k + 2, 2);
            if (rank < 0)
                break;
            std::memcpy(&fn, row.data() + 12 * k + 4, 2);
            if (fn == func) {
                found_row = id;
                found_k = k;
                break;
            }
        }
    }
    if (!found_row)
        return -3;
    // 86AA60 / 857C20 / 85A36C remap confidant ids 3..0x25 through two jump tables and ids
    // 0x21..0x25 through 859E00; only identity mappings are reproduced (jump byte 2 = "keep").
    if (found_row >= 0x21)
        return std::nullopt;
    if (found_row >= 3 && (r.remap_a[found_row - 3] != 2 || r.remap_b[found_row - 3] != 2))
        return std::nullopt;
    const u16 cid = u16(found_row);
    const auto rec = Confidant(read, r, cid);
    if (!rec)
        return std::nullopt;
    if (!*rec)
        return 0; // 85A454: confidant not met
    u8 kind = row[12 * found_k];
    s16 need{};
    u32 entry_flag{};
    std::memcpy(&need, row.data() + 12 * found_k + 2, 2);
    std::memcpy(&entry_flag, row.data() + 12 * found_k + 8, 4);
    const u16 cflags = (*rec)->flags;
    if (cflags & 1) {
        if (kind & 2)
            return -1;
    } else if ((cflags & 2) && (kind & 2)) {
        return -2;
    }
    std::vector<u8> ob(8ull * opens);
    if (opens && !read(open, ob.data(), ob.size()))
        return std::nullopt;
    for (u32 i = 0; i < opens; ++i) {
        s16 f{};
        u32 fl{};
        std::memcpy(&f, ob.data() + 8ull * i, 2);
        std::memcpy(&fl, ob.data() + 8ull * i + 4, 4);
        if (f != func)
            continue;
        const auto b = bit(fl);
        if (!b)
            return std::nullopt;
        if (*b)
            return 2;
        break;
    }
    u32 info_flag{};
    if (!Get(read, info + 8ull * u32(func) + 4, info_flag))
        return std::nullopt;
    if (info_flag) {
        const auto b = bit(info_flag);
        if (!b)
            return std::nullopt;
        if (*b)
            return -4;
    }
    if (s32((*rec)->rank) < need)
        return 0;
    if (entry_flag) {
        const auto b = bit(entry_flag);
        if (!b)
            return std::nullopt;
        if (!*b)
            return 0;
    }
    if (kind & 4)
        return std::nullopt; // party-member gate (7E3AE0) not needed for the functions used here
    return 1;
}

// ---- STATS ----
struct Member {
    bool ready{};
    u8 baton{}; // Baton Pass RANK 1..3 (0 = the native page draws no baton block)
    bool baton_max{};
    bool downshot{}; // "Down Shots left/max" line drawn (protagonist with Down Shot)
    u8 ds_left{}, ds_max{};
};
struct Stats {
    bool ready{};
    u8 technical{}; // Technical RANK 1..4 on the STATS list page (0 = not drawn)
    bool technical_max{};
    std::string technical_help;
    std::array<Member, MemberIds> member{}; // by member id
};

template <class Read>
Stats SampleStats(Read& read, const Roots& r, const p5r_social_menu::Roots& flags,
                  const p5r_social_menu::request::Roots& help, u64 main_base, u64 main_size) {
    using namespace detail;
    Stats s;
    if (!r.stats)
        return s;
    auto bit = [&](u32 f) {
        return p5r_social_menu::BitCheck(read, flags, main_base, main_size, f);
    };
    const auto baton_on = bit(FlagBaton);
    const auto tech_on = bit(FlagTechnical);
    if (!baton_on || !tech_on)
        return s;
    std::array<u32, MemberIds> index{};
    if (!read(r.baton, index.data(), sizeof(index)))
        return s;
    for (unsigned id = 1; id < MemberIds; ++id) {
        auto& m = s.member[id];
        m.ready = true;
        if (id == 8 || !*baton_on)
            continue; // 81CBC4: the navigator has no baton block; flag 0x30000130 gates the rest
        const auto count = Counter(read, r, index[id]);
        if (!count)
            return {};
        if (*count < 0)
            continue;
        m.baton = u8(*count > 2 ? 3 : *count + 1);
        m.baton_max = m.baton > 2;
    }
    {
        auto& p = s.member[1];
        const auto shot = FunctionState(read, r, flags, main_base, main_size, s16(FuncDownShot));
        const auto cheap = FunctionState(read, r, flags, main_base, main_size, s16(FuncCheapShot));
        const auto used = Counter(read, r, CountDownShotsUsed);
        if (!shot || !cheap || !used)
            return {};
        p.downshot = *shot > 0;
        if (p.downshot) {
            p.ds_max = u8(*cheap > 0 ? 3 : 1);
            p.ds_left = u8(p.ds_max > *used ? p.ds_max - *used : 0);
        }
    }
    if (*tech_on) {
        const auto count = Counter(read, r, CountTechnical);
        if (!count)
            return {};
        const s32 rank = *count >= 4 ? 4 : *count + 1;
        if (rank >= 1) {
            s.technical = u8(rank);
            s.technical_max = rank >= 4;
            // datTechnicalHelp.bmd message = rank (registered while the camp menu is up).
            s32 handle{};
            u64 object{}, bmd{};
            if (help.help_handles && help.message_slots &&
                Get(read, help.help_handles + 4ull * TechnicalHelpIndex, handle) && handle >= 0 &&
                handle <= 0x400 &&
                Get(read, help.message_slots + u64(handle) * 0x40 + 0x48, object) && object &&
                Get(read, object + 8, bmd) && bmd)
                p5r_social_menu::HelpText(read, bmd, s16(rank), s.technical_help);
        }
    }
    s.ready = true;
    return s;
}

// ---- CALENDAR jobs ----
struct Job {
    u16 row{}, id{};
    std::string name;
};
struct Jobs {
    bool ready{};
    u8 month{}, day{}, weekday{};
    u16 index{};  // days since 4/1 (721690)
    u8 drawoff{}; // 777778 bits: 1 no day jobs, 2 no night jobs, 4 no jobs at all
    std::vector<Job> day_jobs, night_jobs;
};

inline std::optional<u16> DayIndex(const Roots& r, int month, int day) {
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return std::nullopt;
    if (month == 4)
        return u16(day - 1);
    int m = month - 1 == 0 ? 12 : month - 1;
    int acc = r.month_lengths[(m - 1) % 12];
    while (m != 4) {
        m = m - 1 == 0 ? 12 : m - 1;
        acc += r.month_lengths[(m - 1) % 12];
    }
    return u16(acc + day - 1);
}

template <class Read>
Jobs SampleJobs(Read& read, const Roots& r, const p5r_social_menu::Roots& flags, u64 main_base,
                u64 main_size, int month, int day) {
    using namespace detail;
    Jobs j;
    if (!r.jobs)
        return j;
    const auto index = DayIndex(r, month, day);
    if (!index)
        return j;
    j.month = u8(month);
    j.day = u8(day);
    j.index = *index;
    j.weekday = u8((*index % 7 + 5) % 7);
    auto bit = [&](u32 f) {
        return p5r_social_menu::BitCheck(read, flags, main_base, main_size, f);
    };
    // 7EBF60: the day's draw-off rows {u8 month, u8 day, u8 type, u8, u32 flag}.
    {
        u64 data{};
        u32 count{};
        if (!CtdBlock(read, r.drawoff, 8, data, count, 512))
            return j;
        std::vector<u8> rows(8ull * count);
        if (count && !read(data, rows.data(), rows.size()))
            return j;
        int result = 0xffff;
        for (u32 i = 0; i < count; ++i) {
            const u8* p = rows.data() + 8ull * i;
            if (p[0] != month || p[1] != day)
                continue;
            u32 f{};
            std::memcpy(&f, p + 4, 4);
            if (f) {
                const auto b = bit(f);
                if (!b)
                    return j;
                if (!*b)
                    continue;
            }
            const int t = p[2];
            if (t == 0 || (result == 2 && t == 1) || (result == 1 && t == 2)) {
                result = 0;
                break;
            }
            result = t;
        }
        if (result <= 2)
            j.drawoff = u8((0x20104u >> (result * 8)) & 0xff);
    }
    if (j.drawoff & 4) {
        j.ready = true;
        return j;
    }
    // Unlocked jobs (841250 bit 0, unlock date <= the day).
    std::array<u8, 4 + 4 * ArbeitSlots> rec{};
    if (!read(r.arbeit_records, rec.data(), rec.size()))
        return j;
    std::vector<u32> ids;
    for (u32 k = 0; k < ArbeitSlots; ++k) {
        const u8* p = rec.data() + 4 + 4 * k;
        if (!(p[0] & 1))
            continue;
        const auto since = DayIndex(r, p[2], p[3]);
        if (!since)
            return j;
        if (*since <= *index)
            ids.push_back(k);
    }
    u64 table{}, names{};
    u32 rows{}, name_count{};
    if (!CtdBlock(read, r.arbeit, 0x30, table, rows, 64) ||
        !CtdBlock(read, r.arbeit_names, 64, names, name_count, 64))
        return j;
    std::vector<u8> t(0x30ull * rows);
    if (rows && !read(table, t.data(), t.size()))
        return j;
    auto rd_u16 = [&](u32 row, unsigned off) {
        u16 v{};
        std::memcpy(&v, t.data() + 0x30ull * row + off, 2);
        return v;
    };
    std::vector<u32> list;
    for (u32 id : ids)
        for (u32 row = 0; row < rows; ++row)
            if (rd_u16(row, 0) == id && (rd_u16(row, 4) & (1u << j.weekday))) {
                list.push_back(row);
                break;
            }
    // Exchange sort, descending by s16 +0xC (777D88).
    for (size_t a = 0; a + 1 < list.size(); ++a)
        for (size_t b = a + 1; b < list.size(); ++b)
            if (s16(rd_u16(list[a], 0xc)) < s16(rd_u16(list[b], 0xc)))
                std::swap(list[a], list[b]);
    auto make = [&](u32 row) -> std::optional<Job> {
        Job job;
        job.row = u16(row);
        job.id = rd_u16(row, 0);
        const s16 name = s16(rd_u16(row, 8));
        if (name < 0 || u32(name) >= name_count ||
            !p5r_social_menu::detail::Label(read, names + 64ull * u32(name), job.name))
            return std::nullopt;
        return job;
    };
    if (!(j.drawoff & 1))
        for (u32 row : list) {
            if (j.day_jobs.size() > 2)
                break;
            if (t[0x30ull * row + 6] & 1) {
                auto job = make(row);
                if (!job)
                    return {};
                j.day_jobs.push_back(std::move(*job));
            }
        }
    if (!(j.drawoff & 2))
        for (u32 row : list) {
            if (j.night_jobs.size() > 2)
                break;
            if (t[0x30ull * row + 6] & 2) {
                auto job = make(row);
                if (!job)
                    return {};
                j.night_jobs.push_back(std::move(*job));
            }
        }
    j.ready = true;
    return j;
}

// Native L / R on the CALENDAR: the same day of the previous / next month, clamped to that
// month's length; the in-game year runs April .. March. Returns month*100+day.
inline std::optional<int> StepMonth(const Roots& r, int month, int day, int dir) {
    if (month < 1 || month > 12 || day < 1 || (dir != -1 && dir != 1))
        return std::nullopt;
    const int order = (month + 8) % 12 + dir; // April 0 .. March 11
    if (order < 0 || order > 11)
        return std::nullopt;
    const int m = (order + 3) % 12 + 1;
    return m * 100 + std::min(day, int(r.month_lengths[m - 1]));
}

// ---- CALENDAR Daily Log (past cursor days) ----
// 778F00: a cursor day before today shows the Daily Log instead of the plans' job panels.
// 77AF2C: line K (0, 1) = 87F8F0(month, day, K): the high (K 0) / low (K 1) byte of the u16 at
// records[day index] (0 past index 364); 0 = an empty line. A non-zero id names a
// cmmNetReportTable row (86B330: rows outside the table read row 1); 77EE30 draws the row text
// (+4) and, when the row's u32 +0 is 0 (Palace / Mementos / calling card / Treasure / request
// rows), draws it red over the camp underline sprite 0x33F. Stickers (camp sprites, red):
// a line 0x5B "Stole the Treasure!" -> 0x342 "Done!!" and, on even days except 12/24, 0x341;
// else a line 0x58 "Went to the Palace" -> 0x341 on even days; else a line 0x59 "Went to
// Mementos" -> 0x340 on odd days. (Month 9 days 8..11 add the Hawaii texture art; not drawn.)
struct LogLine {
    u8 id{};
    bool red{};
    std::string text;
};
struct DailyLog {
    bool ready{};
    std::array<LogLine, 2> line{};
    bool bus{}, morgana{}, done{}; // stickers 0x340 / 0x341 / 0x342
};
template <class Read>
DailyLog SampleDailyLog(Read& read, const Roots& r, int month, int day) {
    DailyLog log;
    const auto index = r.log ? DayIndex(r, month, day) : std::nullopt;
    if (!index)
        return log;
    u16 raw = 0;
    if (*index < LogDays && !detail::Get(read, r.log_records + 2ull * *index, raw))
        return log;
    const std::array<u8, 2> ids{u8(raw >> 8), u8(raw & 0xff)};
    u64 data{};
    u32 count{};
    for (unsigned k = 0; k < 2; ++k) {
        auto& l = log.line[k];
        l.id = ids[k];
        if (!l.id)
            continue;
        if (!data && !detail::CtdBlock(read, r.net_report, 0x34, data, count, 512))
            return {};
        const u64 row = data + 0x34ull * (l.id < count ? l.id : 1);
        u32 category{};
        if (!detail::Get(read, row, category) ||
            !p5r_social_menu::detail::Label(read, row + 4, l.text))
            return {};
        l.red = category == 0;
    }
    auto any = [&](u8 id) { return ids[0] == id || ids[1] == id; };
    const bool even = day % 2 == 0;
    if (any(0x5b)) {
        log.done = true;
        log.morgana = even && !(month == 12 && day == 24);
    } else if (any(0x58)) {
        log.morgana = even;
    } else if (any(0x59)) {
        log.bus = !even;
    }
    log.ready = true;
    return log;
}

// ---- REQUEST ----
struct RequestExtra {
    u8 grade{};   // native grade sprite = 0x202 + grade: 0 "?", 1 D, 2 C, 3 B, 4 A, 5 S (MAX)
    bool isnew{}; // NEW badge (8946F0 bit 0)
};
struct Requests {
    bool ready{};
    std::vector<RequestExtra> extra;      // per request::List entry (Recent order)
    std::vector<u8> progress, difficulty; // entry indices in the native tab order
};
inline const char* GradeLetter(u8 grade) {
    constexpr std::array<const char*, 6> Letters{"?", "D", "C", "B", "A", "S"};
    return grade < Letters.size() ? Letters[grade] : "";
}

template <class Read>
Requests SampleRequests(Read& read, const Roots& r, const p5r_social_menu::request::Roots& q,
                        const p5r_social_menu::request::List& list) {
    using namespace detail;
    Requests out;
    if (!r.requests || !list.ready)
        return out;
    // Base order = cmpQuestSortTable order (7FA610 copies it before each tab sort).
    u64 sort{}, rows{};
    u32 count{}, qcount{};
    if (!CtdBlock(read, q.sort, 4, sort, count, 128) ||
        !CtdBlock(read, q.data, 0x24, rows, qcount, 100))
        return out;
    std::vector<u8> ids(4ull * count);
    std::array<u8, 8 * 0x64 + 8> states{};
    if ((count && !read(sort, ids.data(), ids.size())) ||
        !read(q.states, states.data(), states.size()))
        return out;
    std::vector<u8> base; // entry indices
    for (u32 i = 0; i < count; ++i) {
        s16 id{};
        std::memcpy(&id, ids.data() + 4ull * i, 2);
        if (id < 0 || id > 0x63)
            continue;
        bool found = false;
        for (size_t e = 0; e < list.entries.size() && !found; ++e)
            if (list.entries[e].id == u16(id)) {
                base.push_back(u8(e));
                found = true;
            }
        // 7FA610 lists every sort-table id whose state is non-zero: the list must agree.
        if (!found && states[8ull * u64(id) + 5] != 0)
            return out;
    }
    if (base.size() != list.entries.size())
        return out;
    out.extra.resize(list.entries.size());
    for (size_t e = 0; e < list.entries.size(); ++e) {
        const auto& en = list.entries[e];
        out.extra[e].grade = en.state == 1 ? 0 : en.difficulty;
        out.extra[e].isnew = states[8ull * en.id + 4] & 1;
    }
    auto state = [&](u8 e) { return list.entries[e].state; };
    auto key2 = [&](u8 e) {
        s16 v{};
        std::memcpy(&v, states.data() + 8ull * list.entries[e].id + 6, 2);
        return v;
    };
    for (size_t e = 0; e < list.entries.size(); ++e)
        if (state(u8(e)) >= r.prio.size())
            return {};
    // 7FEDC0: exchange sort; state priority first, then the +6 key for two state-3 requests.
    out.progress = base;
    auto& p = out.progress;
    for (size_t a = 0; a + 1 < p.size(); ++a)
        for (size_t b = a + 1; b < p.size(); ++b) {
            bool swap = r.prio[state(p[a])] > r.prio[state(p[b])];
            if (!swap && state(p[a]) == 3 && state(p[b]) == 3) {
                const int ka = key2(p[a]), kb = key2(p[b]);
                swap = (ka < 0 && kb >= 0) || (ka > kb && (ka | kb) >= 0);
            }
            if (swap)
                std::swap(p[a], p[b]);
        }
    // 7FEF40: states < 2 last, otherwise ascending difficulty.
    out.difficulty = base;
    auto& d = out.difficulty;
    for (size_t a = 0; a + 1 < d.size(); ++a)
        for (size_t b = a + 1; b < d.size(); ++b) {
            if (state(d[b]) < 2)
                continue;
            if (state(d[a]) < 2 || list.entries[d[a]].difficulty > list.entries[d[b]].difficulty)
                std::swap(d[a], d[b]);
        }
    out.ready = true;
    return out;
}

} // namespace p5r_data2

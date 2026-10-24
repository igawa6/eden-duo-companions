// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side tests for the SMO save reader (ctest target dsmod-smo-save). Plain assert()-based,
// like smo_font_test.cpp. Two independent checks:
//   1. A small, hand-built synthetic BYML document (hash/array/string/int/bool), always runs.
//   2. The real save fixtures (SMO_SAVE_FIXTURES, same convention as SMO_BASE_ROMFS in
//      smo_font_test.cpp -- no private path is hard-coded here) joined against the package's own
//      shipped native-table TSVs (packages/SuperMarioOdyssey/dualscreen/data, committed source,
//      found relative to the working directory or via SMO_PACKAGE_DATA). Skipped cleanly (one
//      printed line, exit 0) when the env var is unset or the fixture/data is missing.

#include "smo_save.h"
#include "smo_romfs_tables.h"
#include "smo_test_romfs_host.h"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>

namespace SmoSave {
namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string ReadTextFile(const std::string& path) {
    const auto bytes = ReadFile(path);
    return std::string(bytes.begin(), bytes.end());
}

/// The package's native-table joins are no longer shipped as TSVs (steward ruling 2026-10-24):
/// they are built from the player's own romfs at runtime by SmoRomfsTables::Build(). This test
/// drives that builder from SMO_BASE_ROMFS (same convention as SMO_SAVE_FIXTURES above /
/// SMO_BASE_ROMFS elsewhere in this directory) and caches the result for the whole process,
/// since every table-dependent test in this file wants the same build. Returns nullopt --
/// callers skip cleanly -- when the env var is unset or the build fails.
const std::optional<SmoRomfsTables::Tables>& RomfsTables() {
    static const std::optional<SmoRomfsTables::Tables> cached = [] {
        const char* dir = std::getenv("SMO_BASE_ROMFS");
        if (!dir || !*dir) return std::optional<SmoRomfsTables::Tables>{};
        SmoTestHost::FakeHost host(dir);
        auto api = host.Api();
        return std::optional<SmoRomfsTables::Tables>(SmoRomfsTables::Build(api));
    }();
    return cached;
}

// ---------------------------------------------------------------------------------------------
// 1. Synthetic BYML: {"A": 42, "B": "hi", "C": [true, false]}
// ---------------------------------------------------------------------------------------------

void PutU8(Bytes& b, std::size_t at, std::uint8_t v) {
    if (b.size() < at + 1) b.resize(at + 1, 0);
    b[at] = v;
}
void PutU16(Bytes& b, std::size_t at, std::uint16_t v) {
    if (b.size() < at + 2) b.resize(at + 2, 0);
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void PutU32(Bytes& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4, 0);
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void PutStr(Bytes& b, std::size_t at, const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    if (b.size() < at + n) b.resize(at + n, 0);
    std::memcpy(b.data() + at, s, n);
}

Bytes BuildSyntheticByml() {
    Bytes b(0x70, 0);
    b[0] = 'Y';
    b[1] = 'B';
    PutU16(b, 2, 3);     // version (not checked by the parser beyond the "YB" magic)
    PutU32(b, 4, 0x10);  // hash_key_off
    PutU32(b, 8, 0x28);  // str_off
    PutU32(b, 0xC, 0x34); // root_off

    // Hash-key table: ["A", "B", "C"]
    PutU32(b, 0x10, (3u << 8) | 0xC2);
    PutU32(b, 0x14, 0x10); // "A" at table_off + 0x10 = 0x20
    PutU32(b, 0x18, 0x12); // "B" at 0x22
    PutU32(b, 0x1c, 0x14); // "C" at 0x24
    PutStr(b, 0x20, "A");
    PutStr(b, 0x22, "B");
    PutStr(b, 0x24, "C");

    // String table: ["hi"]
    PutU32(b, 0x28, (1u << 8) | 0xC2);
    PutU32(b, 0x2c, 0x08); // "hi" at table_off + 0x08 = 0x30
    PutStr(b, 0x30, "hi");

    // Root hash, 3 entries: header at 0x34, entries at 0x38.
    PutU32(b, 0x34, (3u << 8) | 0xC1);
    PutU32(b, 0x38, (0xD1u << 24) | 0); // "A": int-typed entry, name index 0
    PutU32(b, 0x3c, 42);
    PutU32(b, 0x40, (0xA0u << 24) | 1); // "B": string-typed entry, name index 1
    PutU32(b, 0x44, 0);                 // strings[0] == "hi"
    PutU32(b, 0x48, (0xC0u << 24) | 2); // "C": array-typed entry, name index 2
    PutU32(b, 0x4c, 0x50);               // absolute offset of the array's own node header

    // Array node (dereferenced via "C"'s value slot): [true, false].
    PutU32(b, 0x50, (2u << 8) | 0xC0);
    PutU8(b, 0x54, 0xFF); // element 0 type: bool
    PutU8(b, 0x55, 0xFF); // element 1 type: bool
    PutU32(b, 0x58, 1);   // true
    PutU32(b, 0x5c, 0);   // false
    return b;
}

void TestCheckpointTable() {
    const auto table = ParseCheckpointTable(
        "stage\tid\tx\ty\tz\tname\n"
        "SandWorldHomeStage\tobj655\t24200\t-2331.5\t-12850\tTostarena Town\n"
        "SandWorldHomeStage\tobj655\t24200\t-2331.5\t-12850\tDuplicate\n"
        "SandWorldHomeStage\tobj999\tnan\t0\t0\tInvalid\n");
    assert(table.size() == 1);
    assert(table[0].checkpoint.stage == "SandWorldHomeStage" && table[0].checkpoint.id == "obj655");
    assert(table[0].position[0] == 24200 && table[0].name == "Tostarena Town");
    assert(ParseCheckpointTable("bad\n").empty());
}

void TestSameSizeSaveUpdates() {
    // A real File1.bin envelope containing one CoinNum hash entry. Changing its value must
    // refresh progress even though both the BYML document and the file keep the same length.
    Bytes file(0x3c, 0);
    PutU32(file, 4, 1);
    PutU32(file, 0xc, 0x2c);
    file[0x10] = 'Y'; file[0x11] = 'B';
    PutU16(file, 0x12, 3);
    PutU32(file, 0x14, 0x10);
    PutU32(file, 0x1c, 0x20);
    PutU32(file, 0x20, (1u << 8) | 0xc2);
    PutU32(file, 0x24, 8);
    PutStr(file, 0x28, "CoinNum");
    PutU32(file, 0x30, (1u << 8) | 0xc1);
    PutU32(file, 0x34, 0xd1u << 24);
    PutU32(file, 0x38, 371);
    ProgressCache cache;
    SaveProgress progress;
    assert(cache.Update(file, progress));
    assert(progress.valid && progress.coins_held == 371);
    assert(!cache.Update(file, progress));
    PutU32(file, 0x38, 372);
    assert(cache.Update(file, progress));
    assert(progress.coins_held == 372);
    auto broken = file;
    broken[0x10] = 0;
    assert(!cache.Update(broken, progress));
    assert(progress.coins_held == 372);
    assert(!cache.Update(file, progress)); // failed reads did not replace the cache
    PutU32(file, 0x38, 373);
    assert(cache.Update(file, progress));
    assert(progress.coins_held == 373);
    std::puts("smo_save: same-size updates and malformed-read recovery passed");
}

void TestSyntheticByml() {
    const auto bytes = BuildSyntheticByml();
    const auto root = ParseByml(bytes);
    assert(root.has_value());
    assert(root->IsHash());
    const auto* a = root->Find("A");
    assert(a && a->AsInt() == 42);
    const auto* bv = root->Find("B");
    assert(bv && bv->AsStringView() == "hi");
    const auto* c = root->Find("C");
    assert(c && c->IsArray() && c->arr.size() == 2);
    assert(c->arr[0].AsBool() == true);
    assert(c->arr[1].AsBool() == false);
    assert(root->Find("NoSuchKey") == nullptr);

    // A truncated buffer (the array's dereferenced node is now out of range) fails closed.
    Bytes truncated(bytes.begin(), bytes.begin() + 0x50);
    const auto bad = ParseByml(truncated);
    assert(!bad.has_value());

    std::puts("smo_save: synthetic BYML hash/array/string/int/bool round-trip passed");
}

void TestItemNameColumn() {
    // Always runs (no fixture/package data dependency): basic shape of ParseItemNameColumn,
    // independent of the real-fixture join test in TestRealFixtures below.
    const auto ok = ParseItemNameColumn("item_name\tdisplay_name\nMario\tfoo\nMarioCaptain\tbar\n");
    assert(ok && ok->size() == 2 && ok->contains("Mario") && ok->contains("MarioCaptain"));
    // Blank item_name cells are skipped, not counted as an empty-string entry.
    const auto blank = ParseItemNameColumn("item_name\nMario\n\t\n");
    assert(blank && blank->size() == 1);
    assert(!ParseItemNameColumn("no_such_column\nx\n").has_value());
    assert(!ParseItemNameColumn("").has_value());
    std::puts("smo_save: ParseItemNameColumn passed");
}

// ---------------------------------------------------------------------------------------------
// 2. Real save fixtures + the package's own shipped native-table TSVs
// ---------------------------------------------------------------------------------------------

/// Private save fixtures root (SMO_SAVE_FIXTURES, same convention as SMO_BASE_ROMFS in
/// smo_font_test.cpp / DREAD_ROMFS etc. elsewhere in this directory): no private path is
/// hard-coded here, and the whole fixture section skips cleanly when it is unset.
/// nullopt only when SMO_BASE_ROMFS is unset/build failed (callers skip cleanly).
std::optional<TaskRules> LoadTaskRules() {
    const auto& tables = RomfsTables();
    if (!tables) return std::nullopt;
    const auto rules=ParseTaskRules(tables->task_shine_rules, tables->task_world_rules,
        tables->task_item_rules, tables->task_shop_rules, tables->moons);
    assert(rules && rules->worlds.size()==17 && rules->shines.size()==3033);
    return rules;
}
std::optional<SaveProgress> LoadFixture(const std::string& name) {
    const char* fixtures = std::getenv("SMO_SAVE_FIXTURES");
    if (!fixtures || !*fixtures) {
        return std::nullopt;
    }
    const auto bytes = ReadFile(std::string(fixtures) + "/" + name + "/File1.bin");
    if (bytes.empty()) {
        return std::nullopt;
    }
    const auto root = ParseSaveFile(bytes);
    if (!root) {
        return std::nullopt;
    }
    auto out=ExtractProgress(*root);
    if (const auto rules = LoadTaskRules()) ApplyTaskRules(*root,out,*rules);
    return out;
}

Node NInt(std::int64_t v){Node n;n.type=Node::Type::Int;n.i=v;return n;}
Node NString(const std::string& s){Node n;n.type=Node::Type::String;n.s=s;return n;}
Node NHash(std::initializer_list<std::pair<std::string,Node>> fields){Node n;n.type=Node::Type::Hash;n.hash=fields;return n;}
Node NArray(std::vector<Node> rows){Node n;n.type=Node::Type::Array;n.arr=std::move(rows);return n;}
Node TaskRoot(const TaskRules& rules){
    std::vector<Node> worlds,quizzes;
    for(int i=0;i<20;++i)worlds.push_back(NHash({{"ScenarioNo",NInt(1)},{"WorldWarpHoleThrough",NInt(0)},{"ShopShine",NInt(0)}}));
    for(int i=0;i<17;++i)quizzes.push_back(NHash({{"IsAnswerCorrectSphinxQuiz",NInt(0)},{"IsAnswerCorrectSphinxQuizAll",NInt(0)}}));
    return NHash({{"IsUnlockAchievement",NInt(1)},{"ShineData",NArray({})},
        {"WorldInfo",NArray(worlds)},{"SphinxQuizData",NArray(quizzes)},
        {"AchievementSaveData",NArray({})},{"ItemCap",NArray({})},{"ItemCloth",NArray({})},
        {"ItemGift",NArray({})},{"ItemSticker",NArray({})}});
}
Node& Field(Node& n,const std::string& name){for(auto& [k,v]:n.hash)if(k==name)return v;assert(false);return n;}
void TestNativeTaskRules(){
    const auto loaded=LoadTaskRules();
    if (!loaded) {
        std::puts("smo_save: SMO_BASE_ROMFS not set, skipping native task-rule join test");
        return;
    }
    const auto& rules=*loaded;
    assert(!ParseTaskRules("bad", "bad", "bad", "bad"));
    auto root=TaskRoot(rules);
    const auto evaluate=[&]{auto p=ExtractProgress(root);ApplyTaskRules(root,p,rules);return p;};
    auto p=evaluate();
    assert(p.achievement_counters.at("Scenario_Ending")==1);
    assert(ForTask(p,{"Scenario_Ending","","",{}}).ready); // native absent Num defaults to1
    assert(p.achievement_counters.at("Shine_Gather_1")==0);
    assert(p.achievement_counters.at("Shine_CollectCoinShop")==0);
    // Native Grand units and shop purchases are not unique-placement counts.
    const auto grand=std::find_if(rules.shines.begin(),rules.shines.end(),[](const auto& r){return r.label=="Shine_Gather_1" && r.weight==3;});
    assert(grand!=rules.shines.end());
    Field(root,"ShineData").arr.push_back(NHash({{"UniqueId",NInt(grand->id)},{"IsGet",NInt(1)}}));
    Field(Field(root,"WorldInfo").arr[0],"ShopShine")=NInt(7);
    Field(root,"AchievementSaveData").arr.push_back(NHash({{"Name",NString("Scenario_Ending")},{"IsGet",NInt(1)}}));
    p=evaluate();assert(p.achievement_counters.at("Shine_Gather_1")==11);
    // Multi-category filters retain native overlaps; Peach counts kingdoms, not moon actors.
    const auto peach=std::find_if(rules.shines.begin(),rules.shines.end(),[](const auto& r){return r.label=="Shine_TravelingPeach_1";});
    assert(peach!=rules.shines.end() && !peach->group.empty());
    for(const auto& r:rules.shines)if(r.label==peach->label && r.group==peach->group)
        Field(root,"ShineData").arr.push_back(NHash({{"UniqueId",NInt(r.id)},{"IsGet",NInt(1)}}));
    p=evaluate();assert(p.achievement_counters.at(peach->label)==1);
    // Story/MoonRock predicates use equality, so a later unrelated scenario is not completion.
    for(size_t i=0;i<14;++i)Field(Field(root,"WorldInfo").arr[i],"ScenarioNo")=NInt(rules.worlds[i].moon_rock);
    p=evaluate();assert(p.achievement_counters.at("Scenario_WorldAll")==14 && p.achievement_counters.at("Other_MoonStoneAll")==14);
    Field(Field(root,"WorldInfo").arr[0],"ScenarioNo")=NInt(99);
    p=evaluate();assert(p.achievement_counters.at("Scenario_WorldAll")==13 && p.achievement_counters.at("Other_MoonStoneAll")==13);
    // Sphinx Forest uses the first field; all other worlds use the All field.
    Field(Field(root,"SphinxQuizData").arr[3],"IsAnswerCorrectSphinxQuiz")=NInt(1);
    Field(Field(root,"SphinxQuizData").arr[0],"IsAnswerCorrectSphinxQuiz")=NInt(1);
    p=evaluate();assert(p.achievement_counters.at("MiniGame_SphinxQuiz")==1);
    Field(Field(root,"SphinxQuizData").arr[0],"IsAnswerCorrectSphinxQuizAll")=NInt(1);
    p=evaluate();assert(p.achievement_counters.at("MiniGame_SphinxQuiz")==2);
    // Read all20 warp slots, including positions outside the17world catalogue.
    Field(Field(root,"WorldInfo").arr[19],"WorldWarpHoleThrough")=NInt(1);
    p=evaluate();assert(p.achievement_counters.at("Other_WorldWarpHoleAll")==1);
    // Whitelists ignore debug/unknown owned strings; repeated items don't inflate native counts.
    const auto& item=rules.items.front();
    Field(root,item.array).arr={NString(item.item),NString(item.item),NString("UnknownItem")};
    p=evaluate();assert(p.achievement_counters.at("Souvenir_Count_1")==1);
    for(const auto& r:rules.shops)if(r.world==rules.shops.front().world)Field(root,r.array).arr.push_back(NString(r.item));
    p=evaluate();assert(p.achievement_counters.at("Shine_CollectCoinShop")==1);
    // Missing/malformed independent data becomes unknown, never a guessed zero or stale ready.
    Field(root,"SphinxQuizData").arr.pop_back();
    Field(root,"WorldInfo").arr.pop_back();
    Field(root,"ItemGift")=NString("not an array");
    Field(root,"ShineData").arr.push_back(NHash({{"UniqueId",NInt(0)},{"IsGet",NInt(1)}}));
    p=evaluate();
    assert(!p.achievement_counters.contains("MiniGame_SphinxQuiz"));
    assert(!p.achievement_counters.contains("Other_WorldWarpHoleAll"));
    assert(!p.achievement_counters.contains("Souvenir_Count_1"));
    assert(!p.achievement_counters.contains("Shine_Gather_1"));
    assert(!p.achievement_counters.contains("Shine_Shine2D_1"));
    assert(p.achievement_counters.contains("Scenario_WorldAll"));
    // A legacy save needs native resolution; do not silently undercount it as modern data.
    root=TaskRoot(rules);
    Field(root,"ShineData").arr.push_back(NHash({{"UniqueId",NInt(999999)},{"IsGet",NInt(1)}}));
    p=evaluate();assert(!p.achievement_counters.contains("Shine_Gather_1"));
    root=TaskRoot(rules);Field(root,"ShineData").arr.push_back(NHash({{"UniqueId",NInt(grand->id)},{"IsGet",NInt(1)}}));
    root.hash.emplace_back("Shine",NArray({NString("legacy")}));
    p=evaluate();assert(!p.achievement_counters.contains("Shine_Gather_1") && !p.achievement_counters.contains("Shine_Shine2D_1"));
    std::puts("smo_save: all native task joins, multi-moons, shop/award accounting, exact story/quiz boundaries, whitelists and missing-data refusals passed");
}

void TestTasks() {
    Node root; root.type = Node::Type::Hash;
    assert(!ExtractProgress(root).achievements_available.has_value());
    Node unlock; unlock.type = Node::Type::Int;
    root.hash.emplace_back("IsUnlockAchievement", unlock);
    assert(ExtractProgress(root).achievements_available == false);
    root.hash[0].second.i = 1;
    assert(ExtractProgress(root).achievements_available == true);
    root.hash[0].second.i = 99;
    assert(!ExtractProgress(root).achievements_available.has_value());
    root.hash[0].second.type = Node::Type::String;
    root.hash[0].second.s = "1";
    assert(!ExtractProgress(root).achievements_available.has_value());
    const auto tasks = ParseTaskTable("label\tname\thint\ttarget\nOther_Jump\tJump!\tJump.\\nAgain.\t10000\nUnknown\tOther\tHint\t\nOther_Jump\tDuplicate\tHint\t1\nBad\tBad\tHint\tNaN\n");
    assert(tasks.size() == 2 && tasks[0].hint == "Jump.\nAgain.");
    // RowHint: the row's one-line preview joins the native hint's own line break back into a
    // single sentence (so the row label's wrap_width/max_lines does real word-wrap + ellipsis
    // against the whole sentence, PACKAGE_FORMAT.md section 3.14), instead of hard-cutting at
    // the break point the way the old `hint.substr(0, hint.find('\n'))` did (reproduced here:
    // "Bring peace to all kingdoms in\nthe world." -- the owner-reported bug, Scenario_WorldAll).
    assert(RowHint(tasks[0]) == "Jump. Again.");
    assert(RowHint(Task{"Scenario_WorldAll", "Achieve World Peace",
                        "Bring peace to all kingdoms in\nthe world.", {}}) ==
          "Bring peace to all kingdoms in the world.");
    assert(RowHint(Task{"x", "y", "No break here.", {}}) == "No break here."); // passthrough
    assert(RowHint(Task{"x", "y", "  Leading and\n\ntrailing  \n ", {}}) ==
          "Leading and trailing"); // collapses runs of whitespace, trims both ends
    assert(RowHint(Task{"x", "y", "", {}}).empty());
    SaveProgress p;
    assert(!ForTask(p, tasks[0]).current);
    p.valid = true;
    assert(!ForTask(p, tasks[0]).current);
    p.achievement_counters["PlayerJumpCount"] = 9999;
    assert(ForTask(p, tasks[0]).current == 9999 && !ForTask(p, tasks[0]).ready);
    p.achievement_counters["PlayerJumpCount"] = 10000;
    assert(ForTask(p, tasks[0]).ready);
    p.achievements_claimed.push_back("Other_Jump");
    assert(ForTask(p, tasks[0]).claimed && !ForTask(p, tasks[0]).ready);
    assert(!ForTask(p, tasks[1]).current && !ForTask(p, tasks[1]).ready);
    assert(ParseTaskTable("bad\n").empty());
}

void TestRealFixtures() {
    const auto hundred = LoadFixture("100-percent-en");
    if (!hundred) {
        std::puts("smo_save: SMO_SAVE_FIXTURES not set (or fixture missing), skipping fixture "
                 "decode test");
        return;
    }
    // The save format notes' validated counts for this fixture.
    assert(hundred->achievements_available == true);
    assert(hundred->achievement_counters.at("PlayerJumpCount") == 27962);
    assert(hundred->achievement_counters.at("ItemCap") == 41);
    assert(hundred->moon_ids.size() == 775);
    assert(hundred->coin_ids.size() == 1000);
    assert(hundred->checkpoint_count == 85);
    assert(hundred->checkpoints.size() == 85);
    assert(hundred->achievements_claimed.size() == 61);
    assert(hundred->captures_seen.size() == 52);
    assert(hundred->world_id == 14 && hundred->stage_name == "PeachWorldHomeStage");
    // Stats-tab extension: raw owned item_name sets
    // and the live CollectBgmList music counter, cross-checked against the save format notes'
    // own validated "41/42/26/17" set-equal table and RE batch 9's live "82/82" finding.
    assert(hundred->owned_caps.size() == 41);
    assert(hundred->owned_cloth.size() == 42);
    assert(hundred->owned_gifts.size() == 26);
    assert(hundred->music_valid);
    assert(hundred->music_total == 82 && hundred->music_owned == 82);

    const auto sand = LoadFixture("sand-16-moons-en");
    assert(sand.has_value());
    // The fixture's own label claims 16 Sand moons; verified against save_read.py's live output
    // (not assumed): coins held 371 (the task's "coins still 0371" live-check value), 19 moons
    // total (not all in Sand -- see the per-kingdom join below), 0 coins total, 1 checkpoint, 0
    // achievements, 6 captures.
    assert(sand->achievements_available == false);
    assert(sand->achievement_counters.at("PlayerJumpCount") == 244);
    assert(sand->coins_held == 371);
    assert(sand->coin_ids.empty());
    assert(sand->moon_ids.size() == 19);
    assert(sand->checkpoint_count == 1);
    assert(sand->achievements_claimed.empty());
    assert(sand->captures_seen.size() == 6);
    assert(sand->world_id == 2 && sand->stage_name == "SandWorldHomeStage");
    // Early-game save: far fewer owned items/tracks than the 100% fixture, but never negative
    // or over the (still-unknown-here) static totals -- the live field, not a fabricated number.
    assert(sand->owned_caps.size() < 41 && sand->owned_cloth.size() < 42);
    // CollectBgmList is append-only: this fixture's array has only 8 entries (confirmed this
    // session by direct inspection, not assumed), all already collected -- total must still read
    // the verified native constant (82), never the array's own (much smaller, early-game) size.
    assert(sand->music_valid && sand->music_total == 82 && sand->music_owned == 8);

    const auto cascade = LoadFixture("cascade-native-common");
    assert(cascade.has_value());
    assert(cascade->moon_ids.empty() && cascade->coin_ids.empty());
    assert(cascade->checkpoint_count == 1);
    assert(cascade->achievements_claimed.empty());
    assert(cascade->captures_seen.size() == 2);
    // Save format notes: "the live `work` save's early-game state parsed... to 4 tracks, in
    // Cascade" -- confirmed directly against this exact fixture this session (4 CollectBgmList
    // entries, all collected).
    assert(cascade->music_valid && cascade->music_total == 82 && cascade->music_owned == 4);

    std::printf("smo_save: real fixtures passed (100%%: %zu moons/%zu coins/%zu cps/%zu ach/%zu "
               "cap; sand: %zu moons, coins held %lld)\n",
               hundred->moon_ids.size(), hundred->coin_ids.size(), hundred->checkpoint_count,
               hundred->achievements_claimed.size(), hundred->captures_seen.size(),
               sand->moon_ids.size(), static_cast<long long>(sand->coins_held));

    // --- Native-table join: built from the player's own romfs (SmoRomfsTables::Build(), see
    // RomfsTables() above), not shipped TSVs (steward ruling 2026-10-24). Skips cleanly without
    // SMO_BASE_ROMFS, same as the rest of this file's romfs-dependent checks.
    const auto& built = RomfsTables();
    if (!built) {
        std::puts("smo_save: SMO_BASE_ROMFS not set, skipping native-table join test");
        return;
    }
    const auto tasks = ParseTaskTable(built->task_details);
    assert(tasks.size() == 61);
    auto unclaimed = *hundred;
    unclaimed.achievements_claimed.clear();
    std::size_t counters = 0;
    for (const auto& task : tasks) {
        assert(ForTask(*hundred, task).claimed);
        const auto progress = ForTask(unclaimed, task);
        if (progress.current) { ++counters; assert(progress.ready); }
    }
    assert(counters == 61);
    const auto locations = ParseCheckpointTable(built->checkpoints);
    assert(locations.size() == 9);
    for (const auto& location : locations) {
        assert(std::find(hundred->checkpoints.begin(), hundred->checkpoints.end(), location.checkpoint) != hundred->checkpoints.end());
        assert(std::find(sand->checkpoints.begin(), sand->checkpoints.end(), location.checkpoint) == sand->checkpoints.end());
    }
    const auto& moons_tsv = built->moons;
    const auto& coins_tsv = built->purple_coins;
    const auto& kingdoms_tsv = built->kingdoms;
    if (moons_tsv.empty() || coins_tsv.empty() || kingdoms_tsv.empty()) {
        std::puts("smo_save: romfs native-table build failed, skipping kingdom join test");
        return;
    }
    const auto tables = ParseKingdomTables(moons_tsv, coins_tsv, kingdoms_tsv);
    assert(tables.has_value());
    assert(tables->kingdoms.size() == 17);
    const auto peach_info = std::find_if(tables->kingdoms.begin(), tables->kingdoms.end(),
        [](const auto& k) { return k.world == "Peach"; });
    assert(peach_info != tables->kingdoms.end() && peach_info->shine_type == 0);
    assert(sand->visited_world_ids == std::vector<int>({0,1,2}));
    assert(hundred->visited_world_ids.size() == 17);
    const auto ruined = ForKingdom(*hundred, *tables, "Attack");
    assert(ruined.moons == 10 && ruined.moon_total == 10);
    const auto peach = ForKingdom(*hundred, *tables, "Peach");
    assert(peach.moons == 104 && peach.moon_total == 104);
    int list_total = 0;
    for (const auto& kingdom : tables->kingdoms) {
        const auto list = ForKingdom(*hundred, *tables, kingdom.world);
        assert(list.moons == list.moon_total);
        list_total += list.moons;
    }
    assert(list_total == 836); // placement entries775 + Toadette rewards61


    const auto sand_100 = ForKingdom(*hundred, *tables, "Sand");
    assert(sand_100.known);
    assert(sand_100.moons == 89 && sand_100.moon_total == 89);
    assert(sand_100.coins == 100 && sand_100.coin_capacity == 100);

    const auto sand_kingdom = ForKingdom(*sand, *tables, "Sand");
    assert(sand_kingdom.known);
    // This is the number the task asks to verify rather than assume: how many of the fixture's
    // 19 total moons are actually in the Sand kingdom.
    assert(sand_kingdom.moons == 16);
    assert(sand_kingdom.moon_total == 89);
    assert(sand_kingdom.coins == 0 && sand_kingdom.coin_capacity == 100);

    const auto unknown = ForKingdom(*sand, *tables, "NotAKingdom");
    assert(!unknown.known);

    // Globe page "Collected, all visited kingdoms" pill ("Spoiler handling"): sum moons/
    // moon_total over VISITED
    // kingdoms only, the same rows the globe headline/list already read. sand-16-moons-en
    // visits only world_id 0/1/2 (Cap/Waterfall/Sand per kingdoms.tsv, asserted above as
    // {0,1,2}) -- the other 14 kingdoms, Peach (moon_total 104) included, must not contribute
    // to either side of the ratio.
    {
        int expect_moons = 0, expect_total = 0;
        for (const auto& info : tables->kingdoms) {
            if (info.world_id != 0 && info.world_id != 1 && info.world_id != 2) continue;
            const auto kingdom = ForKingdom(*sand, *tables, info.world);
            expect_moons += kingdom.moons;
            expect_total += kingdom.moon_total;
        }
        const auto visited_sum = SumVisitedKingdomMoons(*sand, *tables);
        assert(visited_sum.moons == expect_moons);
        assert(visited_sum.moon_total == expect_total);

        // Regression guard against the "excluded from both sides" requirement specifically:
        // summing ForKingdom() over every kingdom regardless of visited state (what a naive
        // implementation might do) gives a strictly larger denominator, since ForKingdom() itself
        // doesn't gate on visited state and several unvisited kingdoms have a nonzero moon_total.
        int sum_all_moons = 0, sum_all_total = 0;
        for (const auto& info : tables->kingdoms) {
            const auto kingdom = ForKingdom(*sand, *tables, info.world);
            sum_all_moons += kingdom.moons;
            sum_all_total += kingdom.moon_total;
        }
        assert(visited_sum.moon_total < sum_all_total);
        assert(visited_sum.moons <= sum_all_moons);

        // 100-percent-en: every kingdom visited (visited_world_ids.size() == 17, asserted above)
        // and every kingdom fully complete (moons == moon_total for all 17, the `list_total` loop
        // above) -- so the visited-only sum must equal the all-kingdom total, not a partial one.
        const auto hundred_sum = SumVisitedKingdomMoons(*hundred, *tables);
        assert(hundred_sum.moons == list_total && hundred_sum.moon_total == list_total);

        std::printf("smo_save: visited-only moon sum passed (sand %d/%d, 100%% %d/%d)\n",
                   visited_sum.moons, visited_sum.moon_total, hundred_sum.moons,
                   hundred_sum.moon_total);
    }

    const auto& achievements_tsv = built->achievements;
    const auto& captures_tsv = built->captures;
    if (!achievements_tsv.empty()) {
        const auto pairs = ParseNamePairsTsv(achievements_tsv);
        assert(pairs.size() == 61);
        bool found = false;
        for (const auto& [label, name] : pairs) {
            if (label == "Scenario_Ending") {
                assert(name == "Rescue Princess Peach");
                found = true;
            }
        }
        assert(found);
    }
    if (!captures_tsv.empty()) {
        assert(ParseNamePairsTsv(captures_tsv).size() == 52);
    }

    // Stats-tab extension: static costume-pair / souvenir name tables, joined against the real
    // 100% save's owned item_name sets above.
    const auto& caps_tsv = built->costumes_cap;
    const auto& cloth_tsv = built->costumes_cloth;
    const auto& gift_tsv = built->items_gift;
    if (!caps_tsv.empty() && !cloth_tsv.empty() && !gift_tsv.empty()) {
        const auto cap_names = ParseItemNameColumn(caps_tsv);
        const auto cloth_names = ParseItemNameColumn(cloth_tsv);
        const auto gift_names = ParseItemNameColumn(gift_tsv);
        assert(cap_names && cap_names->size() == 41);
        assert(cloth_names && cloth_names->size() == 42);
        assert(gift_names && gift_names->size() == 26);
        assert(!ParseItemNameColumn("no_item_name_column\tx\n1\t2\n").has_value());
        std::size_t pairs = 0;
        for (const auto& name : *cap_names)
            if (cloth_names->contains(name)) ++pairs;
        // Save format notes: 41 cap-only + 42 clothes-only
        // entries, 39 names present in both tables.
        assert(pairs == 39);
        const std::set<std::string> owned_caps{hundred->owned_caps.begin(), hundred->owned_caps.end()};
        const std::set<std::string> owned_cloth{hundred->owned_cloth.begin(), hundred->owned_cloth.end()};
        std::size_t owned_pairs = 0;
        for (const auto& name : *cap_names)
            if (cloth_names->contains(name) && owned_caps.contains(name) && owned_cloth.contains(name))
                ++owned_pairs;
        assert(owned_pairs == 39); // a 100%-complete save owns every costume, both halves
        const std::set<std::string> owned_gifts{hundred->owned_gifts.begin(), hundred->owned_gifts.end()};
        std::size_t owned_souvenirs = 0;
        for (const auto& name : *gift_names)
            if (owned_gifts.contains(name)) ++owned_souvenirs;
        assert(owned_souvenirs == 26);
    } else {
        std::puts("smo_save: costume/souvenir name TSVs absent, skipping collections-extension join test");
    }

    std::printf("smo_save: kingdom join passed (Sand: 100%% save %d/%d moons %d/%d coins; sand "
               "fixture %d/%d moons %d/%d coins)\n",
               sand_100.moons, sand_100.moon_total, sand_100.coins, sand_100.coin_capacity,
               sand_kingdom.moons, sand_kingdom.moon_total, sand_kingdom.coins,
               sand_kingdom.coin_capacity);
}

} // namespace
} // namespace SmoSave

int main() {
    SmoSave::TestTasks();
    SmoSave::TestNativeTaskRules();
    SmoSave::TestCheckpointTable();
    SmoSave::TestSameSizeSaveUpdates();
    SmoSave::TestSyntheticByml();
    SmoSave::TestItemNameColumn();
    SmoSave::TestRealFixtures();
    std::puts("smo_save: all tests passed");
    return 0;
}

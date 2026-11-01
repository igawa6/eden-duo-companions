// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// One table for every live-RAM lead this module knows about, and the one formula every domain
// reader uses to turn a lead into a guest address.
//
// Addressing -- SOLVED 2026-10-24 (LIVE-READERS-132.md, "step 8 live session 3"). SysBot.NET/
// CaptureSight publish *absolute* heap addresses for a real console, but under Eden the guest
// heap moves every boot. Two independent recovery paths were compared on 2 independent boots:
// (a) a harness `findi` scan for a known live value (Watt, capped at 9999999), back-solved
// against its SysBot literal, and (b) `ModRuntime::HeapLow()` (`mod_state.cpp:65`, reading the
// live page table via `process->GetPageTable().GetHeapRegionStart()`) -- the same function this
// ABI exposes to a module as `host->get_heap_begin(userdata)`
// (`mod_module_host.cpp:209`: `module_host.get_heap_begin = [](void* p){ return
// static_cast<ModRuntime*>(p)->HeapLow(); }`). Both boots: (a) and (b) are **bit-for-bit
// identical**. So the formula every anchor below uses,
//
//     guest_addr = host->get_heap_begin(userdata) + sysbot_offset
//
// is confirmed, not a hypothesis: a module can call `get_heap_begin` directly, every boot, with
// no scan. An anchor's `verified` bit below reflects whether *that specific lead* was read live
// and decoded correctly against a known-good value (a native screenshot, a save-file field, or a
// capped/constant value) on 2 independent boots -- not just whether the address formula resolves.
// A lead that is readable but whose semantics/value were never independently cross-checked stays
// `verified = false` even though the addressing formula itself is now proven
// (PORTING_A_GAME.md section 6: "No heuristics" -- an address resolving is not the same claim as
// a value being correct).
//
// To land a verified reader for a not-yet-verified lead: read it live with the harness, decode
// it, and cross-check the value (native screenshot, a capped/constant field, or a checksum) on at
// least 2 boots, then flip `verified` to true and wire its domain's Sample function in
// sw_reader.h. The table is the single place that changes.

#pragma once

#include <cstdint>
#include <string_view>

namespace sw_addresses {

using u64 = std::uint64_t;

enum class Confidence {
    kHigh,       // version-pinned, cross-validated, or confirmed live more than once
    kMedium,     // readable but semantics or exact live-address unconfirmed
    kLow,        // unlabeled / single-source / not independently checked
    kUnresolved, // no live address found publicly or this session; field layout only
};

enum class Domain {
    kTrainer,  // MyStatus8 (TID/SID/OT/Watt), Misc8 (money/badges)
    kParty,    // live party PK8 array
    kBag,      // MyItem8 pouches
    kDex,      // Zukan8 seen/caught (Galar/IoA/CT)
    kLocation, // Coordinates8 (position/zone), CurrentScreen
    kDens,     // Max Raid den tables
    kBattle,   // active/opposing mon HP/PP, single-vs-double
};

struct Anchor {
    const char* name;
    Domain domain;
    u64 sysbot_offset; // the SysBot/CaptureSight literal; see the file header for the formula
    u64 size;          // bytes read for the live round-trip/torn-read check
    Confidence confidence;
    // True only once this specific lead was read live and decoded correctly against a
    // known-good value on >=2 independent boots -- see the file header.
    bool verified;
    const char* note;
};

// clang-format off
inline constexpr Anchor kAnchors[]{
    // --- Trainer ---------------------------------------------------------------------------
    {"mystatus.watt",      Domain::kTrainer, 0x45068FE8, 4,   Confidence::kHigh,
     true, "TrainerDataOffset+0xD0; VERIFIED 2026-10-24, 2 independent boots (5 total across "
           "sessions), byte-identical: 9999999 (this fixture's Watt cap), heap_begin()+offset "
           "formula confirmed against ModRuntime::HeapLow() bit-for-bit both boots"},
    {"mystatus.tid_sid",   Domain::kTrainer, 0x45068FB8, 4,   Confidence::kHigh,
     true, "+0xA0 (TID16 u16, SID16 u16); VERIFIED 2026-10-24, 2 boots, byte-identical: 0/0 for "
           "the gb-635571 fixture (plausible for a GameBanana save, not cross-checked against a "
           "second known OT's nonzero TID/SID)"},
    {"mystatus.ot_name",   Domain::kTrainer, 0x45068FC8, 0x1A, Confidence::kHigh,
     true, "+0xB0; VERIFIED 2026-10-24, 2 boots, byte-identical: UTF-16LE \"Player\" decodes "
           "cleanly once addressed with the confirmed heap_begin()+offset formula (the prior "
           "session's garbage decode was a per-boot delta mistake, not a layout problem)"},
    {"mystatus.game",      Domain::kTrainer, 0x45068FBC, 1,   Confidence::kHigh,
     true, "+0xA4 (u8); VERIFIED readable and decodes correctly 2026-10-24 boot 1 (44 / 0x2C = "
           "PKHeX GameVersion.SW, matches Sword) -- boot 2 re-check skipped for time, but the "
           "value is a fixed per-title constant, not boot state, so a second independent decode "
           "adds little; re-confirm if this module ever ships for Shield too"},
    {"misc8.money_badges", Domain::kTrainer, 0x45072DF0, 0x120, Confidence::kHigh,
     true, "Misc8 (KMisc); VERIFIED 2026-10-25, 2 independent boots, byte-identical "
           "(live-pkg-test boot1 heap 0x2104C00000, reader-verify boot2 heap 0x2104E00000 -- a "
           "clean +0x200000 per-boot shift, same get_heap_begin()+sysbot_offset formula as every "
           "other anchor in this table): Badges u8+0x00 = 0xFF (all 8 Galar badges), Money "
           "u32+0x04 = 9999999 (the in-game cap), BP u16+0x11C = 9999 (the in-game cap) -- all "
           "three match LIVE-READERS-132.md's 2026-10-25 session exactly. Found via a `findi "
           "9999999` magic-value search (same trick as mystatus.watt)"},
    {"progress.hall_of_fame", Domain::kTrainer, 0x45072C80, 8, Confidence::kHigh,
     true, "SCBlock KEnteredHallOfFame (0xE2F6E456, PKHeX SaveBlockAccessor8SWSH): u64 unix time of "
           "the first Hall of Fame entry, 0 before the game is beaten. Found 2026-11-01 by searching "
           "the live save-block cluster heap+0x45000000..0x450E0000 for the fixture's own block value "
           "(0x5DD06865, read from the save with swsh_save.py): exactly one hit, next to misc8/repel. "
           "VERIFIED 2 independent g0 boots, same offset: heap 0x2104800000 and 0x2105000000 (the "
           "reader then published sw.progress.champion=1 live)"},
    // --- Party -----------------------------------------------------------------------------
    {"party.save_mirror",  Domain::kParty, 0x450C68B0, 0x158 * 6 + 1, Confidence::kHigh,
     true, "Save-snapshot party mirror (renamed from party.live 2026-10-26: it is NOT the live "
           "party, see party.array_root) + the count byte at +6*0x158 (5 for gb-635571, verified "
           "2026-10-24, 2 boots). The reader uses it only as a whole-party fallback when the live "
           "array cannot be read. CaptureSight Pk8Offset::Party; VERIFIED 2026-10-24, 2 boots, byte-identical: species/"
           "level/MaxHP/moves/PP all round-trip through sw_pk8::DecodeParty and checksum-validate; "
           "MaxHP matched the native 06-party.png screenshot's x/max values exactly. Current HP is "
           "NOT present in this mirror (searched exhaustively in the decrypted bytes, not found) "
           "-- do not publish a current-HP value from this anchor"},
    {"party.array_root",   Domain::kParty, 0x4C4294F8, 0x90, Confidence::kHigh,
     true, "Live party-slot array container (x21 at the item-notify breakpoint main+0x7A6E20, "
           "caller_main_off 0x7ba35c, inside the already-documented main+0x7A7458..0x7BE344 "
           "item-effect cluster); VERIFIED 2026-10-25, 2 independent cold boots, byte-identical "
           "heap-relative offset. +0x58..+0x80 (stride 8): 6 u64 pointers to the live party-slot "
           "objects, index 0..5; +0x88: u64 live party count (read 5, both boots). With 5 members "
           "the 6th pointer (+0x80) is a padding/'capacity' slot object (PID=0, species decodes "
           "to 0); the reader reads slots 0..count-1 only (count 1..6). 6-member read: see "
           "MODULE-SCAFFOLD.md 2026-10-26 review fixes. "
           "Each slot object's encrypted PK8 'stored' blob (species, LIVE current HP/status) "
           "starts at slot_ptr+0xD0, 0x148 bytes -- see sw_pk8::DecodeLiveStored(). The live "
           "array's per-index species order can differ from the save-snapshot mirror's order "
           "(this fixture: index 2/3 swapped, Zacian<->Zamazenta -- cosmetic, not a chain error). "
           "See LIVE-READERS-132.md's 'Live party objects FOUND' session for the full breakpoint/"
           "2-boot verification writeup, including verification (b) (a 2nd Potion, re-read with "
           "a plain memdump, no breakpoint/debugger at all, still matched the on-screen HP)."
           "\n\nREJECTED 2026-10-26 (game-session review round) as a title-exclusion signal: a "
           "live check (sw_reader.h's new AnyLivePartySlotValid(), 2 independent boots) found "
           "this anchor's live party array fully checksum-valid (sw.party.live_count=5, "
           "sw.party.0.ready=1) EVEN AT THE TITLE SCREEN -- the title screen's own cosmetic "
           "stadium render decrypts the real save's party to pose it. Still used (ANDed with "
           "location.zone_name's own validity) as extra insurance for the FIELD confirmation "
           "itself, just never alone and never for title/loading -- see that anchor's own note "
           "and sw_reader.h's SampleState"},
    // --- Bag -------------------------------------------------------------------------------
    {"field.repel",        Domain::kBag, 0x4507300C, 4, Confidence::kHigh,
     true, "Repel record: u16 item (76/77/79 or 0), u16 steps left. Game data (heap+0x45021770) "
           "+0x208 -> repel object heap+0x45072D90, fields +0x27C/+0x27E (the game's own UseRepel "
           "main+0x1439120 writes them, IsRepelActive main+0x1439170 reads them). VERIFIED "
           "2026-10-26 on 2 boots (pointer chain identical heap-relative: 0x214A072D90 at heap "
           "0x2105000000, 0x214A672D90 at 0x2105600000); Max Repel used via the field helper read "
           "back item 77 / 250 steps"},
    {"bag.item_pouch",     Domain::kBag, 0x45067a98, 4856, Confidence::kHigh,
     true, "KItem (MyItem8 pouch block), heap_begin()+offset (the same proven formula as every "
           "other anchor in this table); VERIFIED 2026-10-25, 2 independent boots, byte-exact "
           "match to the real save's decrypted KItem block at this heap-relative offset both "
           "boots (found via a wide memdump + offline byte-search against the real save's "
           "decrypted blocks, not findi/cluster -- LIVE-READERS-132.md's 'KItem (Bag) + KZukan... "
           "SOLVED' session). 1214 fixed u32 slots (4856/4), one per PlayerBag8 pouch in order "
           "(Medicine 60 @+0x0000, Balls 30 @+0x00F0, BattleItems 20 @+0x0168, Berries 80 "
           "@+0x01B8, Items 550 @+0x02F8, TMHMs 210 @+0x0B90, Treasure 100 @+0x0ED8, Candy 100 "
           "@+0x1068, KeyItems 64 @+0x11F8 -- kwsch/PKHeX PlayerBag8.cs GetPouches(), fetched "
           "live from GitHub and cross-checked: 60+30+20+80+550+210+100+100+64=1214 exactly). "
           "Per-slot u32 (InventoryItem8.cs GetValue): bits 0-10 item id (0x7FF), bits 15-24 "
           "count (0x3FF after >>15), bit 30 IsNew, bit 31 IsFavorite -- re-derived directly "
           "against kwsch/PKHeX's real C# source (not the prior session's 12-bit/15-bit/'sentinel' "
           "guess, which happened to decode the same id/count values for every real item in this "
           "fixture by coincidence of range, but mislabelled bit 30 as a list-start marker when it "
           "is really the game's own 'NEW' badge flag -- confirmed: every Medicine-pouch slot in "
           "this maxed/edited save has it set except slot 0, exactly matching 'everything is new "
           "except whatever the save editor touched last', not a sentinel). Spot-checked against "
           "file bytes: Potion(17)=995, Antidote(18)=995, ... Full Restore(23)=995, matching the "
           "native 08-08-bag.png screenshot exactly"},
    // --- Dex -------------------------------------------------------------------------------
    // Zukan8 (seen/caught), heap_begin()+offset, VERIFIED 2026-10-25, 2 independent boots, all
    // 3 blocks byte-exact to the real save's decrypted Zukan blocks at these heap-relative
    // offsets both boots (same wide-memdump-plus-offline-byte-search method as bag.item_pouch
    // above; LIVE-READERS-132.md's 'KItem + KZukan... SOLVED' session). Entry layout from
    // Zukan8.cs (fetched live from GitHub, sources/Zukan8.cs): EntrySize=0x30, first 0x20 bytes
    // = 4 QWORD seen-region bitfields (any nonzero = seen), OFS_CAUGHT=0x20 u32 bit0 = Owned
    // (caught) -- this per-block decode is correct and unit-tested (TestZukanDexCounts), but a
    // raw block's own N entries are NOT a region's full Pokédex DISPLAY list (see below).
    //
    // CORRECTED 2026-10-25 (game-session review, fresh native capture of this exact fixture,
    // 20261024T1752+0700-g0-g0-native): Galar 400/400, Isle of Armor 211/211, Crown Tundra
    // Caught 210 / Seen 210 -- matching this header's EARLIER note's own cited screenshot
    // exactly, confirming that screenshot was real and this exact fixture, not a different save
    // as a prior pass here wrongly concluded. The real bug: Zukan8's `GetDexLookup` assigns each
    // species to exactly ONE "home" block for its real flags, by priority Galar > Armor > Crown
    // -- but a region's own Pokédex screen DISPLAYS every species with that region's own
    // `<Region>DexIndex` field nonzero, REGARDLESS of home (Crown Tundra's own dex legitimately
    // lists many ordinary Galar-native species, not just DLC-exclusive legendaries). Reading a
    // region's raw block directly therefore only ever sees the species truly HOMED there -- for
    // this fixture, Crown Tundra: 210 species display, but only 74 are Crown-homed (the other
    // 136 are Galar/Armor-homed "visitors" whose real flags live in KZukan/KZukanR1 instead,
    // both of which are themselves 100% caught here, giving 74+135+1=210). `sw_dex_index.h`'s
    // `BuildReverseIndex`/`BuildReverseIndexFromBytes` now build both the per-region DISPLAY
    // list and the per-species HOME resolution; `sw_reader.h`'s `SampleDex` resolves every
    // displayed species through its real home before counting/publishing. Verified end to end
    // against real save + real personal-table bytes in `TestDexRegionCountsViaHome`
    // (0100ABF008968000_test.cpp): Galar 400/400, Isle of Armor 211/211, Crown Tundra 210/210,
    // matching the native screen exactly.
    {"dex.galar",          Domain::kDex, 0x45069120, 19200, Confidence::kHigh,
     true, "KZukan (Galar dex), 400 entries x 0x30; VERIFIED 2026-10-25, 2 boots, byte-exact; "
           "decodes to 400/400 seen/caught for the gb-635571 fixture (display == home for Galar, "
           "see this table's domain header comment)"},
    {"dex.isle_of_armor",  Domain::kDex, 0x4506DC20, 10128, Confidence::kHigh,
     true, "KZukanR1 (Isle of Armor dex), 211 entries x 0x30, starts exactly where KZukan ends; "
           "VERIFIED 2026-10-25, 2 boots, byte-exact; the region's real 211/211 DISPLAY total "
           "needs home-resolution (sw_dex_index.h), not a direct block read -- see this table's "
           "domain header comment"},
    {"dex.crown_tundra",   Domain::kDex, 0x450703B0, 10080, Confidence::kHigh,
     true, "KZukanR2 (Crown Tundra dex), 210 entries x 0x30, starts exactly where KZukanR1 ends; "
           "VERIFIED 2026-10-25, 2 boots, byte-exact; the raw block alone only has 74 of the "
           "region's real 210/210 DISPLAY total (136 members are Galar/Armor-homed) -- see this "
           "table's domain header comment and sw_dex_index.h"},
    // --- Location --------------------------------------------------------------------------
    {"location.zone_name", Domain::kLocation, 0x441C1EC0, 0x98, Confidence::kHigh,
     true, "Live place-name popup: 8-byte tag (FF FF FF FF 08 00 03 00, checked byte-for-byte --"
           " see sw_zone::DecodeZoneName) immediately followed by a null-padded UTF-16LE zone"
           " name buffer (+0x08, read 0x90 bytes here, bounded further by kMaxZoneNameChars);"
           " VERIFIED 2026-10-25, 2 independent cold boots, same heap-relative offset both times."
           " Live-ness proven IN-BOOT, not just cross-boot: flying Axewell->Wyndon overwrote this"
           " exact address in place on the same tick (old trailing bytes survived past the new"
           " name's terminator -- a reused mutable buffer, not a fresh allocation or a save-time"
           " snapshot, unlike dens.* below). A second, differently-labelled live location display"
           " (the X-menu corner HUD / Town Map pin) was also seen this session but resolved to no"
           " new address -- most likely the same underlying Lua zoneName property, per the RE"
           " batch, just formatted differently; this anchor remains the one concretely useful"
           " reader. Decode must stop at the first UTF-16 double-null, never read the full"
           " nominal buffer capacity, or stale trailing bytes from a shorter previous name leak in"
           "\n\nNEW USE 2026-10-26 (game-session review round): this anchor's live validity"
           " (sw_reader.h's zone_cache_ready_, i.e. a successful DecodeZoneName this tick) is now"
           " the sole signal deciding the title/loading boundary in SampleState, replacing"
           " location.current_screen for that one judgement (see its own anchor note)."
           " VERIFIED LIVE, 2 independent boots: sw.location.zone_ready read ABSENT for the"
           " entire ~45s title-idle wait on both, becoming 1 with the real destination name"
           " (\"Wyndon\") within ~2-3s of pressing Continue -- well before the load visually"
           " finishes (~8-9s). Chosen over party.array_root (see that anchor's own note) because"
           " this address reads invalid (not just absent-game-progress) at the title screen,"
           " where the live party array does not"},
    {"location.current_screen", Domain::kLocation, 0x6B30FA00, 4, Confidence::kHigh,
     true, "CurrentScreenOffset (u32, heap_begin()+offset, the same proven formula as every other "
           "anchor in this table); VERIFIED 2026-10-25, reproducible across 2 independent boots "
           "AND across 2 separate wild battles in the second (LIVE-READERS-132.md's 'WILD AREA "
           "battle reached' + battle-struct-readers sessions): outdoor field (incl. mid-dialogue) "
           "reads 0xFFFF5127 (-44761), the battle command/move-select menu reads 0xFFFFFFFF (-1) "
           "-- stable and reproducible both battles, both boots. Camp separately confirmed "
           "0xFF000000. The earlier 'boot-varying, no cross-boot constant' caution predates the "
           "get_heap_begin() addressing-formula proof and is superseded -- it was reading this "
           "anchor with a stale per-boot delta, not a layout problem. Only these two sentinels "
           "(0xFFFF5127 field / 0xFFFFFFFF battle) are classified; any other value (e.g. the "
           "field<->battle transition frame) is treated as unresolved for that tick, not guessed"
           "\n\nEXTENDED 2026-10-25 (companion worktree task 'title + loading state detection', 3 "
           "independent cold boots, harness label g0, fixture gb-635571, console `readi` polling, "
           "tools/poll_state_classifier.py in the research tree -- see sw_reader.h's SampleState "
           "for the classifier this built): the TITLE SCREEN does NOT read a stable sentinel at "
           "all -- this address aliases evidently unrelated, actively-changing animation/render "
           "data during the title screen's own animated demo scene (confirmed live: boot1 read a "
           "stable 0x0 for a full 60s sample at title; boot2, sampled at the 'same' nominal "
           "title-idle moment, read constantly-varying values for 25s and never settled). "
           "CurrentScreen is therefore NOT a usable title detector by itself. Separately, ORDINARY "
           "TOWN-FIELD camera (confirmed live, standing and walking, Wyndon) reads plain 0x0 too "
           "-- 0xFFFF5127 is specifically the Wild-Area-style free-camera value, not a general "
           "field sentinel; a plain town has no dedicated one. Also confirmed live this task: the "
           "boot-to-world LOADING screen (Continue -> world) reads a STABLE 0xFF000000 for its "
           "entire ~8-9s duration on 2/2 boots tested, immediately followed by the field sentinel "
           "(one single-tick garbage/tear read observed at the exact transition, both times). The "
           "X-menu (Rotom ring, cursor on Pokedex) and the Town Map ALSO read 0xFF000000 this "
           "session (2 independent live checks) -- the earlier-documented 0xFFFFCF63 X-menu / "
           "0xFF262626 EXP-screen sentinels were NOT reproduced here (same cursor position as the "
           "earlier X-menu capture); kept as historical/possible values in sw_reader.h's matcher, "
           "not re-verified. NOT resolved this task: whether a LATER reload (Fly, Battle Tower "
           "re-entry) after the world has already been entered once reads something distinguishable "
           "from this same 0xFF000000 bucket -- a live Fly capture was attempted but the harness "
           "crashed (Wayland disconnect, then a SIGSEGV on the retry boot) before a clean sample "
           "completed; see sw_reader.h's SampleState comment for the documented, accepted "
           "consequence (such a reload is classified 'menu', not 'loading').\n\nLIVE-CAUGHT "
           "REGRESSION (same task, g0 verification boot of the first cut of this classifier): "
           "a fresh boot's companion screenshot showed the Town Map WITH LIVE DATA while the "
           "device was still displaying the title screen -- the exact bug this task exists to "
           "fix, reproduced by the fix's own first draft. Root cause: the title screen's noise "
           "(see above) coincidentally read the EXACT field sentinel 0xFFFF5127 for at least one "
           "tick, and an un-gated classifier trusted field/battle sentinels instantly (no "
           "debounce), matching the pre-existing proven in-world toggle's own behaviour. A "
           "second live finding (boot2's title sample, this task) made it worse: title noise "
           "also held a STABLE, exactly-matching 0xFF000000 (the loading/menu bucket's own "
           "value) for ~2.7s before drifting into unrelated noise -- long enough that even a "
           "300ms debounce on the loading entry would not have caught it, and would have "
           "reproduced the same bug via a second path (a spurious loading commit, then a "
           "spurious field commit once the noise stopped matching). Fix (sw_reader.h's "
           "SampleState): no sentinel is trusted at all while phase_ is still kTitle except the "
           "loading bucket, and that specific edge's debounce was raised to 5s (not the task's "
           "literal 300ms floor) -- a pragmatic margin above the one 2.7s false-hold observed and "
           "below every genuine load's measured ~8.4-9.3s duration, not a proven bound. This "
           "residual risk is accepted and documented, not hidden: no stronger title-screen signal "
           "was found this task (see above -- the zone tag and party count were both tried and "
           "rejected as title detectors first).\n\nNet model used by "
           "sw_reader.h: battle (0xFFFFFFFF) and wild-area field (0xFFFF5127) stay exactly as "
           "before; 0xFF000000/0xFFFFCF63/0xFF262626 are one 'closed-world overlay' bucket, "
           "disambiguated into loading-vs-menu by whether the world has ever been entered yet this "
           "boot (a structural fact, not a heuristic: nothing but the boot load can happen before "
           "Continue, and title can never be revisited without a full restart); any other/"
           "unrecognized value is title before the world has been entered, ordinary field after"
           "\n\nSUPERSEDED 2026-10-26 (game-session review round): the whole "
           "CurrentScreen-only model above was rejected after it showed title.png as the loading "
           "spinner (not the title page) and could strand the companion on the loading page "
           "indefinitely for a town spawn -- see 'location.zone_name'/'party.array_root' anchors' "
           "own notes for the live-verified replacement signals (the zone tag's live validity for "
           "title, zone+party validity for field, both independent of this address). "
           "CurrentScreen is now used ONLY for the already-proven field (0xFFFF5127) and battle "
           "(0xFFFFFFFF) sentinels, the 'closed-world overlay' bucket's own 3 constants, and a "
           "CurrentScreen-off-the-field-sentinel reload timer; it is no longer consulted at all "
           "for the title/loading boundary. See sw_reader.h's SampleState for the current model"},
    // --- Dens --------------------------------------------------------------------------
    // All 3 regions: CaptureSight DenOffset is CORRECT AS-IS (no signature-hunt needed), struct
    // layout confirmed against sources/RaidSpawnList8.cs's RaidSpawnDetail (Hash/Seed/Stars/
    // RandRoll/DenType/Flags, 0x18 bytes). VERIFIED 2026-10-25, 2 independent boots: each
    // heap-relative address's full byte range matches `tools/swsh_save.py dump-block`'s decode of
    // the real save's own KRaidSpawnList/R1/R2 blocks EXACTLY (2664 bytes each, byte-for-byte,
    // both boots) -- the address/struct decode itself is high confidence. SAVE-SNAPSHOT CAVEAT
    // (same honesty bar as party.live's current_hp): this cluster (0x450C6xxx-0x450C9xxx) is the
    // literal tail of the struct this module's own party-current-HP gate (commit a9cfa55)
    // already proved only refreshes on an explicit in-game Save, not per-frame -- Den sits just
    // 0x88 bytes after SoftBanUnixTimespanOffset, contiguously. Not independently re-exercised
    // by an in-session mutation this cycle (the nearest active den is far from every fixture's
    // spawn); treat as high-but-not-certain for liveness specifically, high confidence for the
    // address/struct/decode. A page must show an "as of last save" marker, never imply this is
    // live-updating (sw_zone.h's file header; see sw_reader.h's SampleDens).
    {"dens.vanilla",       Domain::kDens, 0x450C8A70, 0x18 * 111, Confidence::kHigh,
     true, "CaptureSight DenOffset::Vanilla (KRaidSpawnList, 111-slot capacity, 100 CountUsed); "
           "see this table's domain header comment for the full verification + save-snapshot "
           "caveat"},
    {"dens.isle_of_armor", Domain::kDens, 0x450C94D8, 0x18 * 111, Confidence::kHigh,
     true, "CaptureSight DenOffset::IslandOfArmor (KRaidSpawnListR1, 111-slot capacity, 90 "
           "CountUsed); see this table's domain header comment"},
    {"dens.crown_tundra",  Domain::kDens, 0x450C9F40, 0x18 * 111, Confidence::kHigh,
     true, "CaptureSight DenOffset::CrownTundra (KRaidSpawnListR2, 111-slot capacity, 86 "
           "CountUsed); see this table's domain header comment"},
    // --- Battle ------------------------------------------------------------------------
    {"battle.own_mon",     Domain::kBattle, 0x8FE9D570, 0x350, Confidence::kHigh,
     true, "Pass-A own BattleMon*, heap_begin()+offset (the same proven formula as every other "
           "anchor); VERIFIED 2026-10-25, 2 independent cold boots + 2 separate wild battles "
           "(LIVE-READERS-132.md session 2, bp_capture.py gdbstub software breakpoints on the "
           "PP-decrement entry main+0x7E25A0 and the HP-commit entry main+0x7E37F0, then plain "
           "memdump/gdbstub reads at this heap-relative address with no breakpoint needed, "
           "byte-identical both boots): +0x70 u16 species (National Dex), +0x72 u16 MaxHP, +0x74 "
           "u16 CurHP, +0x7C u8 Level, per-move-slot i (stride 0x1C from the struct base) +0x2D8 "
           "u8 current PP (authoritative) / +0x2D9 u8 max PP. foe_mon = own_mon + 0x5B80 (same "
           "struct shape, verified identical both boots/both battles). Status condition, stat "
           "stages and the command/move cursor were searched for and NOT found that session -- do "
           "not invent them. Move id was NOT found that session but WAS found in a later task "
           "(2026-10-25, battle move-data session): u16 at +0x2D4 + i*0x1C, 4 bytes before the "
           "authoritative PP pair above, VERIFIED live on 2 independent boots with an "
           "offline-patched save carrying arbitrary move ids (see sw_battle.h's own header "
           "comment for the full cross-check, incl. the live-computed pp_max corroboration). A "
           "second 'pass B' copy exists at own_mon+0x18910 with unproven provenance; this module "
           "only reads pass A (own_mon, the earliest-written, unambiguous copy)"},
};
// clang-format on

/// `guest_addr = host->get_heap_begin(userdata) + anchor.sysbot_offset` -- confirmed formula,
/// see the file header (2026-10-24: bit-for-bit identical to a harness-recovered delta, 2 boots).
/// `heap_begin` is passed in rather than re-queried so a caller samples it once per tick
/// (MODULE_GUIDE.md: get_heap_begin reads the live page table).
inline u64 Resolve(u64 heap_begin, const Anchor& anchor) {
    return heap_begin + anchor.sysbot_offset;
}

/// A named anchor's offset at compile time (a name the table does not have does not compile).
consteval u64 OffsetOf(std::string_view name) {
    for (const auto& a : kAnchors)
        if (std::string_view(a.name) == name)
            return a.sysbot_offset;
    throw "sw_addresses::OffsetOf: no such anchor";
}

inline const Anchor* Find(const char* name) {
    for (const auto& a : kAnchors)
        if (name && a.name && std::string_view(a.name) == name)
            return &a;
    return nullptr;
}

} // namespace sw_addresses

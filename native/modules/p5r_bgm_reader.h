// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R "now playing" BGM (build D4B1). Read-only; every root comes from the game's own sound code.
//
// Sound module (main+EA7000..EAB000): script BGM(id) -> EA7130 -> EAA140(player 0, acb 0, cue).
// EAA140 scans a 256 x 0x60 slot table (ADRP/ADD at EAA14C) for {+0 active, +8 player,
// +0xC acb, +0x10 kind} and dispatches on kind; kind 0 (EAA1D0, the BGM kind) keeps
//   +0x24 state  0 steady, 1 fade-out to stop, 2 fade-in, 3 fade-out then switch, 4 switch gap
//   +0x3C cue    current cue (written on start, EAA248, and by the per-frame fader, EA8AC0)
//   +0x40 next   cue queued behind a state-3 fade
// The low-level voice array (pointer at the ADRP/LDR of 10A5A70, 0x20 per slot index) keeps
//   +0 byte  playing (set by start 10A6D0C, cleared by stop 10A59F0/10A5A30; 10A5A70 reads it)
//   +0x10    playback id (new id per start, 10A6D00)   +0x18 cue actually started (10A6D08)
// The BGM is the one active slot with player 0 / acb 0 / kind 0. Live D4B1: title
// 901, Leblanc 640, Mementos 480, Palace safe room 495 -> Palace 400 (state 3 fade, 4 gap), event
// 34, load screen = voice cleared (cue -1) while +0x3C keeps the stale id. The native 10..13 remap
// (EA7130, table main+1E3AFA0, only ever filled with -1 by EA77FC) happens before EAA140, so +0x3C
// is already the effective cue. The [main+2304A58] "second channel" is player 3 = SYSTEM.ACB
// (menu cursor/confirm sounds, live), never music.
// DLC costume BGM packs (sound/bgm_%02d.acb, loaded as ACB group 6 by EA6F.. / EA7030; loaded pack
// number stored at the ADRP/ADD of EA6FF0 +0) redefine exactly the BGM.ACB outside-link targets
// 60300, 60340 and 60907, i.e. cues 300 / 340 / 907. While a pack is loaded those cues are not
// the titled base-game audio: title unknown.
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_bgm {
using namespace dsmod_sdk::int_types;

constexpr unsigned SlotCount = 0x100;
constexpr u64 SlotStride = 0x60, VoiceStride = 0x20;
constexpr unsigned ThievesDenRows = 107;

// Titles = what the game's own Music Player shows, read from the game's romfs at run time
// (p5r_game_text.h: MYPTABLE.BIN mypSoundDataTable/NameTable + BGM.ACB waveform sharing +
// MUSIC_TITLE_001.SPD). Row R of mypSoundDataTable is drawn with sprite R of MUSIC_TITLE_001.SPD;
// rows >= 78 hold dev placeholders in the name table, so their title exists only as the sprite
// (art_only: `title` is empty, `index` = the row whose sprite the page draws). Untitled cues that
// resolve to the same BGM.ACB waveform inherit the title. No game text is embedded here.
// Cues that resolve to BGM.ACB waveform 41 (AWB 53): 60 s of digital silence (-91 dB peak).
inline constexpr u16 kSilent[] = {100, 200, 201, 202, 203, 204, 205, 206, 490, 910, 911, 912,
                                  916, 917, 921, 925, 926, 927, 928, 929, 930, 932, 937, 938};
inline bool Silent(s64 cue) {
    for (u16 c : kSilent)
        if (c == cue)
            return true;
    return false;
}
// Cues whose outside-link target is redefined by every sound/bgm_NN.acb.
inline bool PackOverrides(s64 cue) {
    return cue == 300 || cue == 340 || cue == 907;
}

struct Roots {
    u64 slots{};  // 256 x 0x60 slot table
    u64 voices{}; // main slot holding the voice array pointer
    u64 pack{};   // u32 loaded DLC BGM pack number (0 none)
};

namespace detail {
constexpr u64 PlayFn = 0xeaa140, BgmStart = 0xeaa244, Fader = 0xea8ab0, VoiceGetter = 0x10a5a70,
              VoiceStart = 0x10a6d00, PackCode = 0xea6ff0;
using dsmod_sdk::InMain;
// ADRP Xd + (ADD Xd,Xd,#imm | LDR Xd,[Xd,#imm*8]). Only page/offset immediates may vary.
inline bool Global(u64 main_base, u64 main_size, u64 pc, u32 adrp, u32 access, u32 expected,
                   unsigned scale, u64& out) {
    if ((adrp & 0x9f00001f) != (0x90000000u | ((access >> 5) & 31)) ||
        (access & ~0x003ffc00u) != expected)
        return false;
    const u32 imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3);
    const std::int64_t pages = (imm & 0x100000) ? std::int64_t(imm) - 0x200000 : imm;
    const std::int64_t result = std::int64_t(pc & ~u64{0xfff}) + pages * 4096 +
                                std::int64_t(u64((access >> 10) & 0xfff) << scale);
    if (result <= 0 || !InMain(main_base, main_size, u64(result), 8))
        return false;
    out = u64(result);
    return true;
}
} // namespace detail

template <class Read>
bool Resolve(Read&& read, u64 main_base, u64 main_size, Roots& out) {
    using namespace detail;
    std::array<u32, 20> play{};
    std::array<u32, 2> start{};
    std::array<u32, 6> fader{}, pack_use{};
    std::array<u32, 5> voice{};
    std::array<u32, 4> voice_start{};
    auto code = [&](u64 at, auto& words) {
        return InMain(main_base, main_size, main_base + at, sizeof(words)) &&
               read(main_base + at, words.data(), sizeof(words));
    };
    if (!code(PlayFn, play) || !code(BgmStart, start) || !code(Fader, fader) ||
        !code(VoiceGetter, voice) || !code(VoiceStart, voice_start) || !code(PackCode, pack_use))
        return false;
    Roots r;
    // EAA140: mov w8,w0; mov w0,wzr; mov x10,xzr; adrp x11; add x11,x11,#slots; b; add x10,#0x60;
    // add w0,#1; cmp x10,#0x6000; b.eq; ldrb w9,[x11,x10]; cbz; add x9,x11,x10; ldr w12,[x9,#8];
    // cmp w12,w8; b.ne; ldr w12,[x9,#0xc]; cmp w12,w1; b.ne; ldrsw x11,[x9,#0x10]
    constexpr std::array<u32, 20> PlayWords{
        0x2a0003e8, 0x2a1f03e0, 0xaa1f03ea, 0,          0,          0x14000005, 0x9101814a,
        0x11000400, 0xf140195f, 0x540002e0, 0x386a6969, 0x34ffff69, 0x8b0a0169, 0xb940092c,
        0x6b08019f, 0x54fffee1, 0xb9400d2c, 0x6b01019f, 0x54fffe81, 0xb980112b};
    for (size_t i = 0; i < play.size(); ++i)
        if (i != 3 && i != 4 && play[i] != PlayWords[i])
            return false;
    if (!Global(main_base, main_size, main_base + PlayFn + 12, play[3], play[4], 0x9100016b, 0,
                r.slots) ||
        !InMain(main_base, main_size, r.slots, SlotCount * SlotStride))
        return false;
    // BGM kind start: str wzr,[x19,#0x24]; str w20,[x19,#0x3c]. Fader switch: mov w8,#-1;
    // mov w0,w19; mov w3,#1; mov w4,wzr; stp w1,w8,[x26,#0x3c]; bl start.
    if (start[0] != 0xb900267f || start[1] != 0xb9003e74 || fader[0] != 0x12800008 ||
        fader[1] != 0x2a1303e0 || fader[2] != 0x52800023 || fader[3] != 0x2a1f03e4 ||
        fader[4] != 0x2907a341)
        return false;
    // 10A5A70: adrp x9; ldr x9,[x9,#voices]; sbfiz x8,x0,#5,#32; ldrb w0,[x9,x8]; ret.
    // 10A6D00: str w0,[x22,#0x10]; mov w0,#1; str w19,[x22,#0x18]; strb w0,[x22].
    if (voice[2] != 0x937b7c08 || voice[3] != 0x38686920 || voice[4] != 0xd65f03c0 ||
        voice_start[0] != 0xb90012c0 || voice_start[1] != 0x52800020 ||
        voice_start[2] != 0xb9001ad3 || voice_start[3] != 0x390002c0 ||
        !Global(main_base, main_size, main_base + VoiceGetter, voice[0], voice[1], 0xf9400129, 3,
                r.voices))
        return false;
    // EA6FF0: adrp x9; add x9,x9,#pack; ldr x10,[x9,#8]; ldrh w8,[x20]; str w8,[x10];
    // ldr w9,[x9] (loaded pack number).
    if (pack_use[2] != 0xf940052a || pack_use[3] != 0x79400288 || pack_use[4] != 0xb9000148 ||
        pack_use[5] != 0xb9400129 ||
        !Global(main_base, main_size, main_base + PackCode, pack_use[0], pack_use[1], 0x91000129, 0,
                r.pack))
        return false;
    out = r;
    return true;
}

// One published frame.
struct Frame {
    bool ready{}, playing{}, known{};
    s64 cue{-1}, index{-1}, change{}, fade{};
    const char* title{""};
};

// Per-sample state: the cached BGM slot index, the last playback id and the change counter.
class Tracker {
public:
    // `titles.Music(cue)` -> pointer to {row, text} (p5r_text::GameText) or null: unknown title.
    template <class Read, class Titles>
    Frame Update(Read&& read, const Roots& roots, bool live, const Titles& titles) {
        Frame f;
        f.change = change;
        if (!live) // outside gameplay: nothing published; the change counter keeps its baseline
            return f;
        Raw a{}, b{};
        if (!Find(read, roots) || !ReadRaw(read, roots, a) || !ReadRaw(read, roots, b) ||
            !(a == b)) {
            // torn or relocated: keep the previous frame for a few samples, then clear
            if (held < HoldSamples && have_last) {
                ++held;
                return last;
            }
            slot = -1;
            return f;
        }
        held = 0;
        f.ready = true;
        // the fader keeps +0x3C after a stop; only a started voice with the same cue is audible
        const bool started = a.voice_playing == 1 && a.voice_cue == a.cue && a.cue >= 0;
        if (started && Silent(a.cue)) {
            // a silent placeholder cue: the engine plays it, nothing is audible
            f.cue = a.cue;
            if (a.playback != last_playback || f.cue != last_cue)
                ++change;
            last_playback = a.playback;
            last_cue = f.cue;
        }
        f.playing = started && !Silent(a.cue);
        if (f.playing) {
            f.cue = a.cue;
            f.fade = (a.state == 1 || a.state == 3) ? -1 : a.state == 2 ? 1 : 0;
            if (a.playback != last_playback || f.cue != last_cue)
                ++change;
            last_playback = a.playback;
            last_cue = f.cue;
            const auto* t = titles.Music(f.cue);
            if (t && !(a.pack != 0 && PackOverrides(f.cue))) {
                f.known = true;
                f.title = t->text.c_str(); // empty for art-only rows (the sprite carries it)
                f.index = t->row;
            }
        }
        f.change = change;
        last = f;
        have_last = true;
        return f;
    }
    int Slot() const {
        return slot;
    }

private:
    static constexpr unsigned HoldSamples = 4;
    static constexpr unsigned ScanPeriod = 30;
    struct Raw {
        u8 active{}, voice_playing{};
        s32 player{}, acb{}, kind{}, state{}, cue{}, voice_cue{};
        u32 playback{}, pack{};
        bool operator==(const Raw& o) const {
            return active == o.active && voice_playing == o.voice_playing && player == o.player &&
                   acb == o.acb && kind == o.kind && state == o.state && cue == o.cue &&
                   voice_cue == o.voice_cue && playback == o.playback && pack == o.pack;
        }
    };
    static bool IsBgm(const u8* e) {
        s32 w[3];
        for (int i = 0; i < 3; ++i)
            w[i] = s32(u32(e[8 + 4 * i]) | u32(e[9 + 4 * i]) << 8 | u32(e[10 + 4 * i]) << 16 |
                       u32(e[11 + 4 * i]) << 24);
        return e[0] != 0 && w[0] == 0 && w[1] == 0 && w[2] == 0;
    }
    template <class Read>
    bool Find(Read& read, const Roots& roots) {
        if (slot >= 0) {
            std::array<u8, 0x14> e{};
            if (read(roots.slots + u64(slot) * SlotStride, e.data(), e.size()) && IsBgm(e.data()))
                return true;
            slot = -1;
            scan_age = ScanPeriod;
        }
        if (++scan_age < ScanPeriod)
            return false;
        scan_age = 0;
        table.resize(SlotCount * SlotStride);
        if (!read(roots.slots, table.data(), table.size()))
            return false;
        int found = -1;
        for (unsigned i = 0; i < SlotCount; ++i)
            if (IsBgm(table.data() + i * SlotStride)) {
                if (found >= 0)
                    return false; // ambiguous: never guess
                found = int(i);
            }
        slot = found;
        return slot >= 0;
    }
    template <class Read>
    bool ReadRaw(Read& read, const Roots& roots, Raw& r) const {
        std::array<u8, 0x44> e{};
        u64 voices{};
        std::array<u8, 0x1c> v{}; // up to the cue at +0x18
        if (slot < 0 || !read(roots.slots + u64(slot) * SlotStride, e.data(), e.size()) ||
            !read(roots.voices, &voices, sizeof(voices)) || !voices ||
            voices > std::numeric_limits<u64>::max() - SlotCount * VoiceStride ||
            !read(voices + u64(slot) * VoiceStride, v.data(), v.size()) ||
            !read(roots.pack, &r.pack, sizeof(r.pack)))
            return false;
        auto rd_s32 = [](const u8* p) { return s32(dsmod_sdk::Le32(p)); };
        r.active = e[0];
        r.player = rd_s32(&e[8]);
        r.acb = rd_s32(&e[0xc]);
        r.kind = rd_s32(&e[0x10]);
        r.state = rd_s32(&e[0x24]);
        r.cue = rd_s32(&e[0x3c]);
        r.voice_playing = v[0];
        r.playback = u32(rd_s32(&v[0x10]));
        r.voice_cue = rd_s32(&v[0x18]);
        return r.active && !r.player && !r.acb && !r.kind && r.state >= 0 && r.state <= 4 &&
               r.voice_playing <= 1 && r.pack <= 64;
    }
    std::vector<u8> table;
    int slot{-1};
    unsigned scan_age{ScanPeriod}, held{};
    bool have_last{};
    Frame last{};
    u32 last_playback{};
    s64 last_cue{-1}, change{};
};
} // namespace p5r_bgm

// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD story flags as the native map conditions read them (dev.31; the
// NPC map mark analysis, build 5441AF3C89F97E1D only). The authoritative getter
// (manager *(main+0x182E1F0), vfunc +0x50 = main+0xB7F8B8) decodes two bytes per flag id from its
// mapping table *(*(mgr+0x18)+8): byte 0 = u16 word index, byte 1 = shift << 4 | (width - 1);
// Story(id) = (u16(buffer + 2 * word) >> shift) & ((2 << (width - 1)) - 1). The buffer comes from
// vfunc +0x68 = main+0xB80BC0: the save manager *(main+0x2BF9CC0) + 0x10 (the file in play) +
// 0x8E4, except when the first group-1 object of the object tree has profile 0 and the manager's
// byte +0xCD06 is 0 (a special scene): then +0x53D0. That alternate state is refused here (fail
// closed). The static mirror main+0x182E1F8 is not this buffer and is not used for map conditions.
//
// Everything is resolved per boot through the pointers above; the table is checked against ids
// whose mapping the analysis pinned before any value is trusted.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace sshd {

inline constexpr uint64_t kStoryMgrPtr = 0x182E1F0;  // main+: the story-flag manager
inline constexpr uint64_t kSaveMgrPtr = 0x2BF9CC0;   // main+: the save manager
inline constexpr uint64_t kObjTreeRoot = 0x1688510;  // main+: the object connect tree's root node
inline constexpr uint32_t kFileOff = 0x10, kAltFileOff = 0x53D0, kAltSelect = 0xCD06;
inline constexpr uint32_t kStoryWordsOff = 0x8E4;    // file + 0x8E4: the story words
inline constexpr int kStoryWords = 255;              // word index 255 = reserved (no flag)
inline constexpr int kStoryIds = 2048;
inline constexpr uint32_t kStoryTableBytes = 2 * kStoryIds;

// Ids whose mapping the analysis pinned (its raw mapping table); a live table that disagrees is not
// the decoded one. {id, word, shift, width}
inline constexpr int kStoryAnchors[][4] = {
    {5, 0, 6, 1},     {15, 1, 0, 1},    {42, 7, 3, 1},    {58, 2, 13, 1},  {130, 6, 8, 1},
    {150, 4, 4, 1},   {180, 7, 11, 1},  {201, 11, 3, 1},  {274, 16, 12, 1}, {281, 17, 3, 1},
    {286, 17, 8, 1},  {400, 28, 8, 1},  {509, 30, 3, 1},  {0, 255, 0, 1}};

struct StoryBits {
    int word, shift, width;
};
// One table entry; nullopt for the reserved word 255.
inline std::optional<StoryBits> DecodeStoryEntry(uint8_t b0, uint8_t b1) {
    if (b0 >= kStoryWords)
        return std::nullopt;
    return StoryBits{b0, b1 >> 4, (b1 & 15) + 1};
}

struct StoryTable {
    bool valid = false;
    std::array<uint8_t, kStoryTableBytes> raw{};
    std::optional<StoryBits> Bits(int id) const {
        if (!valid || id < 0 || id >= kStoryIds)
            return std::nullopt;
        return DecodeStoryEntry(raw[2 * id], raw[2 * id + 1]);
    }
};
// Takes kStoryTableBytes read from the live mapping table; valid when every anchor matches.
inline StoryTable ParseStoryTable(const uint8_t* bytes) {
    StoryTable t;
    std::memcpy(t.raw.data(), bytes, kStoryTableBytes);
    for (const auto& a : kStoryAnchors) {
        const uint8_t b0 = t.raw[2 * a[0]], b1 = t.raw[2 * a[0] + 1];
        if (b0 != a[1] || (b1 >> 4) != a[2] || (b1 & 15) + 1 != a[3])
            return StoryTable{};
    }
    t.valid = true;
    return t;
}

// The story words of the file in play (kStoryWords u16).
struct StoryWords {
    bool valid = false;
    uint16_t w[kStoryWords]{};
};

// Story(id) from a table and the words; nullopt when either is invalid or the id is reserved.
inline std::optional<uint32_t> StoryValue(const StoryTable& t, const StoryWords& w, int id) {
    if (!w.valid)
        return std::nullopt;
    const auto b = t.Bits(id);
    if (!b)
        return std::nullopt;
    const uint32_t mask = (2u << (b->width - 1)) - 1u;
    return (static_cast<uint32_t>(w.w[b->word]) >> b->shift) & mask;
}

// Live resolution over a bounded reader `rd(addr, out, n) -> bool` (main = the main module base).
// The mapping table's address, or 0.
template <typename Read>
inline uint64_t StoryTableAddr(Read&& rd, uint64_t main) {
    uint64_t mgr = 0, meta = 0, map = 0;
    if (!rd(main + kStoryMgrPtr, &mgr, 8) || mgr == 0 || !rd(mgr + 0x18, &meta, 8) || meta == 0 ||
        !rd(meta + 8, &map, 8))
        return 0;
    return map;
}

// The game's object connect tree (node[0] parent, [1] first child, [3] next sibling, [4] the
// object), walked depth first by StoryWordsOf below and the module's live-object walk. One step
// from `node` (whose first child is `child`): the child, else the next sibling of the node or of
// its nearest ancestor that has one; *next = 0 at the end of the tree. The tree is read off the
// game thread while the game changes it, so the ancestor climb is capped (kTreeClimbMax steps): a
// transient cycle in the parent chain cannot hang a sample. False (the caller stops and keeps what
// it collected: fail closed) on a failed read or a climb past the cap; the node loops themselves
// are bounded by kTreeWalkMax nodes.
inline constexpr int kTreeWalkMax = 4096;
inline constexpr int kTreeClimbMax = 64;
template <typename Read>
inline bool NextTreeNode(Read&& rd, uint64_t node, uint64_t child, uint64_t* next) {
    *next = 0;
    if (child != 0) {
        *next = child;
        return true;
    }
    uint64_t cur = node;
    for (int climb = 0; cur != 0; ++climb) {
        if (climb >= kTreeClimbMax)
            return false;
        uint64_t sib = 0, parent = 0;
        if (!rd(cur + 0x18, &sib, 8))
            return false;
        if (sib != 0) {
            *next = sib;
            return true;
        }
        if (!rd(cur, &parent, 8))
            return false;
        cur = parent;
    }
    return true;
}

// Why the words could not be used (StoryWordsOf): 0 ok, 1 no save manager / read failed, 2 the
// native alternate (special-scene) buffer is selected, 3 the object tree could not be walked.
// The tree probe follows the getter main+0xB80BC0: depth first (child +8, sibling +0x18, parent
// +0), the first node whose object (+0x20) has group byte +0x16 == 1 decides; bounded
// (NextTreeNode: kTreeWalkMax nodes, kTreeClimbMax ancestor steps; a walk that hits either cap
// before a node decides is reason 3).
template <typename Read>
inline StoryWords StoryWordsOf(Read&& rd, uint64_t main, int* why = nullptr, int* probed = nullptr) {
    StoryWords out;
    int dummy = 0, n = 0;
    int& reason = why != nullptr ? *why : dummy;
    reason = 1;
    uint64_t save = 0, node = 0;
    if (!rd(main + kSaveMgrPtr, &save, 8) || save == 0)
        return out;
    if (!rd(main + kObjTreeRoot, &node, 8)) {
        reason = 3;
        return out;
    }
    bool alt = false;
    for (; node != 0 && n < kTreeWalkMax; ++n) {
        uint64_t nd[5]{};
        if (!rd(node, nd, sizeof(nd))) {
            reason = 3;
            return out;
        }
        if (nd[4] != 0) {
            uint8_t hdr[0x18]{};
            if (!rd(nd[4], hdr, sizeof(hdr))) {
                reason = 3;
                return out;
            }
            if (hdr[0x16] == 1) {
                uint16_t prof = 0;
                std::memcpy(&prof, hdr + 0x10, 2);
                uint8_t sel = 1;
                if (prof == 0 && (!rd(save + kAltSelect, &sel, 1) || sel == 0))
                    alt = true;
                break;
            }
        }
        uint64_t next = 0;
        if (!NextTreeNode(rd, node, nd[1], &next)) {
            reason = 3;
            return out;
        }
        node = next;
    }
    if (probed != nullptr)
        *probed = n;
    // dev.42: the walk stopped at the node cap before any node decided; the native
    // getter's answer is unknown, so the normal buffer is not trusted (fail closed, reason 3)
    if (node != 0 && n >= kTreeWalkMax) {
        reason = 3;
        return out;
    }
    if (alt) {
        reason = 2;
        return out;
    }
    if (!rd(save + kFileOff + kStoryWordsOff, out.w, sizeof(out.w)))
        return out;
    out.valid = true;
    reason = 0;
    return out;
}

// The manager's cached story buffer (vfunc +0x58 = main+0xB7F914, used by the Kikwi face callbacks):
// *(*(mgr+0x10)+8), same mapping table; 0 when unresolved.
template <typename Read>
inline uint64_t StoryCachedAddr(Read&& rd, uint64_t main) {
    uint64_t mgr = 0, obj = 0, data = 0;
    if (!rd(main + kStoryMgrPtr, &mgr, 8) || mgr == 0 || !rd(mgr + 0x10, &obj, 8) || obj == 0 || !rd(obj + 8, &data, 8))
        return 0;
    return data;
}

} // namespace sshd

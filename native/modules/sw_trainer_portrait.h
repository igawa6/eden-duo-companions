// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Read-only sampler for Sword1.3.2's own League Card offscreen texture. The game's
// renderer supplies the image through its native UI or the background guest helper.
// This sampler makes no guest call/write; its immutable CPU copy survives cleanup.
// Native texture: 900-square packed-float output containing the complete card.
// read_memory is essential: Eden's ReadBlock downloads GPU-cached pages. A direct
// pointer/ReadBlockUnsafe cannot replace it. Decode runs on the asset worker.

#include "sw_addresses.h"
#include "sw_build.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sw_trainer_portrait {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
// MyStatus8 from TID/SID on (sw_addresses.h mystatus.tid_sid): +0 ids, +4 game version, +16 OT.
inline constexpr u64 kOwnerOffset = sw_addresses::OffsetOf("mystatus.tid_sid");
inline constexpr u32 kTextureSize = 900;
inline constexpr std::size_t kStorageSize = 3735552;
// Native card face is 800:960 (trlicence_card_parts_00.bflyt). This verified
// 900-square render target places its rounded face at [117,50,783,850] after Y
// inversion; keep the original opaque native background and every card detail.
inline constexpr u32 kCropX = 117, kCropY = 50, kWidth = 666, kHeight = 800;

inline u64 Hash(const void* ptr, std::size_t n, u64 h = 0xcbf29ce484222325ULL) {
    const auto* p = static_cast<const u8*>(ptr);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}
inline u32 U32(const u8* p) {
    return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}
inline u64 U64(const u8* p) { return u64(U32(p)) | u64(U32(p + 4)) << 32; }
inline bool HeapPtr(u64 p) { return p >= 0x800000000ULL && p < 0x8000000000ULL && (p & 7) == 0; }

struct Owner {
    std::array<u8, 26> name{};
    u32 ids{};
    u8 game{};
    bool operator==(const Owner&) const = default;
};
inline bool NormalizeName(std::array<u8, 26>& name) {
    if (name[0] == 0 && name[1] == 0) return false;
    for (std::size_t i = 0; i < name.size(); i += 2) {
        if (!name[i] && !name[i + 1]) {
            std::fill(name.begin() + i, name.end(), 0);
            return true;
        }
    }
    return false;
}
struct Resource {
    u64 global{}, app{}, context{}, meta_hash{}, backing{}, pool{}, offset{}, pool_size{};
    Owner owner{};
    // Per-frame descriptor/impl addresses may rotate while naming the exact same
    // backing. They are validated at each read, not used to cause per-frame copies.
    bool operator==(const Resource&) const = default;
};
inline bool SameContent(const Resource& a, const Resource& b) {
    return a.global == b.global && a.app == b.app && a.meta_hash == b.meta_hash && a.owner == b.owner;
}
struct Capture {
    Resource resource{};
    u64 generation{};
    std::string source;
    std::vector<u8> tiled;
};

template<class Host>
bool Read(const Host& h, u64 p, void* out, std::size_t n) {
    return h.read_memory && n && p <= UINT64_MAX - n &&
           (!h.is_mapped || h.is_mapped(h.userdata, p, n)) && h.read_memory(h.userdata, p, out, n);
}
template<class Host>
bool Stable(const Host& h, u64 p, void* out, std::size_t n) {
    if (n > 512 || !Read(h, p, out, n)) return false;
    std::array<u8, 512> check{};
    return Read(h, p, check.data(), n) && std::memcmp(out, check.data(), n) == 0;
}
template<class Host>
bool Q(const Host& h, u64 p, u64& out) {
    std::array<u8, 8> b{};
    if (!Stable(h, p, b.data(), b.size())) return false;
    out = U64(b.data());
    return true;
}
template<class Host>
bool W(const Host& h, u64 p, u32& out) {
    std::array<u8, 4> b{};
    if (!Stable(h, p, b.data(), b.size())) return false;
    out = U32(b.data());
    return true;
}
template<class Host>
bool Pointer(const Host& h, u64 p, u64& out) { return Q(h, p, out) && HeapPtr(out); }
// GOT entries address MAIN's BSS pointer slots, not heap objects. Keep that
// storage address separate from the heap object subsequently loaded from it.
template<class Host>
bool MainSlot(const Host& h, u64 p, u64& out) {
    return Q(h,p,out) && (out&7)==0 && out>=h.main_base && out<h.main_base+sw_variant::kBuild.image_span;
}
template<class Host>
bool FirstFunction(const Host& h, u64 obj, u64 offset) {
    u64 vt{}, fn{};
    return Q(h, obj, vt) && vt >= h.main_base && vt < h.main_base + sw_variant::kBuild.image_span &&
           Q(h, vt, fn) && fn == h.main_base + offset;
}
template<class Host>
bool ReadOwner(const Host& h, Owner& out) {
    if (!h.get_heap_begin) return false;
    const u64 heap = h.get_heap_begin(h.userdata);
    std::array<u8, 42> b{};
    if (!heap || !Stable(h, heap + kOwnerOffset, b.data(), b.size())) return false;
    out.ids = U32(b.data());
    out.game = b[4];
    std::copy_n(b.data() + 16, 26, out.name.begin());
    return out.game == sw_variant::kActive.save_game_version && NormalizeName(out.name);
}

template<class Host>
bool CanonicalMetadata(const Host& h, const Owner& owner, u64& hash) {
    u64 slot{},save{},card{};
    std::array<u8,0x1c8> metadata{};
    if (!MainSlot(h,h.main_base+sw_variant::kMain.game_data_slot,slot) || !Pointer(h,slot,save) ||
        !Pointer(h,save+0x288,card) || !Stable(h,card+0x60,metadata.data(),metadata.size())) return false;
    Owner printed{}; std::copy_n(metadata.data(),26,printed.name.begin());
    if (!NormalizeName(printed.name) || printed.name != owner.name || metadata[0x24] != sw_variant::kActive.save_game_version - 44 /* TrainerCard8 game: SW 0, SH 1 */ ||
        U32(metadata.data()+0x1c) >= 1000000U) return false;
    hash=Hash(metadata.data(),metadata.size()); return true;
}

// All offsets are native main-relative, resolved again every capture. A dead
// weak reference, a different native application or another trainer fails closed.
template<class Host>
bool Resolve(const Host& h, const Owner& owner, Resource& r) {
    u64 slot{}, g{}, tasks{}, head{}, weak{}, iface{}, app{}, wrapper{}, ctx{}, meta{};
    u32 active{}, kind{}, pending{};
    if (!MainSlot(h, h.main_base + sw_variant::kMain.field_slot, slot) || !Pointer(h, slot, g) ||
        !Pointer(h, g + 0x68, tasks) || !Pointer(h, tasks + 0x78, head) ||
        !FirstFunction(h, head, sw_variant::kMain.portrait_task_head_fn) || !Pointer(h, head + 0x88, weak) ||
        !W(h, weak + 0x54, active) || !active || !Pointer(h, weak + 0x60, iface) || iface < 0x50)
        return false;
    app = iface - 0x50;
    if (!FirstFunction(h, app, sw_variant::kMain.portrait_app_fn) || !Pointer(h, app + 0x768, wrapper) ||
        !FirstFunction(h, wrapper, sw_variant::kMain.portrait_wrapper_fn) || !Pointer(h, wrapper + 0x58, ctx) ||
        !W(h, ctx + 0x730, kind) || kind != 2 || !W(h, ctx + 0x940, pending) || pending)
        return false;
    u8 phase{};
    if (!Stable(h, ctx + 0xb50, &phase, 1) || phase || !Pointer(h, ctx + 0x930, meta)) return false;
    std::array<u8, 0x1c8> metadata{};
    if (!Stable(h, meta, metadata.data(), metadata.size())) return false;
    Owner card{};
    std::copy_n(metadata.data(), 26, card.name.begin());
    // This exact OWN native application slot uses the player's printed-card
    // snapshot (live byte-equal to KTrainerCard874DA6FA). Its printed ID can
    // legitimately predate current MyStatus IDs; do not reject that native card.
    // Current owner IDs still form the cache identity and clear on owner change.
    if (!NormalizeName(card.name) || card.name != owner.name || metadata[0x24] != sw_variant::kActive.save_game_version - 44 /* TrainerCard8 game: SW 0, SH 1 */ ||
        U32(metadata.data() + 0x1c) >= 1000000U) return false;

    u64 descriptor{};
    if (!Q(h, ctx + 0x538, descriptor) || descriptor == UINT64_MAX || descriptor > 0x100000) return false;
    for (unsigned i = 0; i < 3; ++i) {
        u64 impl{}, id{}, view{}, nvn{}, pool{}, offset{}, size{}, storage{}, pool_size{};
        if (!Pointer(h, ctx + 0x740 + 0x150 + i * 8, impl) || !Q(h, impl + 0x58, id) ||
            id != descriptor || !Pointer(h, impl + 0x60, view) || !Pointer(h, view + 8, nvn)) continue;
        std::array<u8, 0xc0> texture{};
        if (!Stable(h, nvn, texture.data(), texture.size()) || U32(texture.data() + 0x28) != 900 ||
            U32(texture.data() + 0x2c) != 900 || U32(texture.data() + 0x30) != 1 ||
            U32(texture.data() + 0x34) != 1 || U32(texture.data() + 0x38) != 63 ||
            texture[0x64] != 4 || texture[0x6c] != 1 || texture[0x6d] != 1 ||
            U32(texture.data() + 0x94) != 0x400) continue;
        pool = U64(texture.data() + 0x10);
        offset = U64(texture.data() + 0x18);
        size = U32(texture.data() + 0x78);
        if (!HeapPtr(pool) || size != kStorageSize || !Q(h, pool + 0x30, pool_size) ||
            offset > pool_size || size > pool_size - offset || !Pointer(h, pool + 0x70, storage) ||
            storage > UINT64_MAX - offset || !HeapPtr(storage + offset) ||
            (h.is_mapped && !h.is_mapped(h.userdata, storage + offset, size))) continue;
        r = {g, app, ctx, Hash(metadata.data(), metadata.size()), storage + offset,
             pool, offset, pool_size, owner};
        return true;
    }
    return false;
}

// Output of the native background owner, published only after its backend has
// completed asynchronous shutdown. The helper retains both target and own-card
// references until the host acknowledges its immutable GPU-synchronized copy.
template<class Host>
bool ResolveAuto(const Host& h, const Owner& owner, u64 mailbox, u32 seq, Resource& r) {
    if (mailbox < 0x10000 || mailbox >= 0x8000000000ULL || (mailbox & 7) || !seq) return false;
    u32 published{}, phase{};
    u64 target{}, own{}, g{}, app{}, slot{}, current_g{}, save{}, current_own{};
    if (!W(h, mailbox + 0x710, published) || published != seq ||
        !W(h, mailbox + 0x714, phase) || phase != 2 ||
        !Pointer(h, mailbox + 0x720, target) || !FirstFunction(h, target, sw_variant::kMain.portrait_target_fn) ||
        !Pointer(h, mailbox + 0x728, own) || !Pointer(h, mailbox + 0x730, g) ||
        !Q(h, mailbox + 0x738, app) || !MainSlot(h, h.main_base + sw_variant::kMain.field_slot, slot) ||
        !Pointer(h, slot, current_g) || current_g != g ||
        !MainSlot(h, h.main_base + sw_variant::kMain.game_data_slot, slot) || !Pointer(h, slot, save) ||
        !Pointer(h, save + 0x288, current_own) || current_own != own) return false;
    std::array<u8, 0x1c8> metadata{};
    if (!Stable(h, own + 0x60, metadata.data(), metadata.size())) return false;
    Owner card{};
    std::copy_n(metadata.data(), 26, card.name.begin());
    if (!NormalizeName(card.name) || card.name != owner.name || metadata[0x24] != sw_variant::kActive.save_game_version - 44 /* TrainerCard8 game: SW 0, SH 1 */ ||
        U32(metadata.data() + 0x1c) >= 1000000U) return false;
    // RenderResource has two900-square aliases of the same native backing;
    // +D8 is its full-color NVNTexture, +220 another view, +368 an8-square alias.
    std::array<u8, 0xc0> t{};
    if (!Stable(h, target + 0xd8, t.data(), t.size()) || U32(t.data()+0x28) != 900 ||
        U32(t.data()+0x2c) != 900 || U32(t.data()+0x30) != 1 || U32(t.data()+0x34) != 1 ||
        U32(t.data()+0x38) != 63 || t[0x64] != 4 || t[0x6c] != 1 || t[0x6d] != 1 ||
        U32(t.data()+0x78) != kStorageSize || U32(t.data()+0x94) != 0x400) return false;
    const u64 pool=U64(t.data()+0x10), offset=U64(t.data()+0x18);
    u64 pool_size{}, storage{};
    if (!HeapPtr(pool) || !Q(h,pool+0x30,pool_size) || offset > pool_size ||
        kStorageSize > pool_size-offset || !Pointer(h,pool+0x70,storage) ||
        storage > UINT64_MAX-offset || !HeapPtr(storage+offset) ||
        (h.is_mapped && !h.is_mapped(h.userdata,storage+offset,kStorageSize))) return false;
    r={g,app,target,Hash(metadata.data(),metadata.size()),storage+offset,pool,offset,pool_size,owner};
    return W(h,mailbox+0x710,published) && published==seq &&
           W(h,mailbox+0x714,phase) && phase==2;
}

inline std::size_t TiledOffset(u32 x, u32 y) {
    constexpr std::size_t gobs = (900 * 4 + 63) / 64, bh = 16;
    const std::size_t xb = x * 4;
    return (y / (8 * bh)) * 512 * bh * gobs + (xb / 64) * 512 * bh +
           ((y % (8 * bh)) / 8) * 512 + ((xb % 64) / 32) * 256 +
           ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + xb % 16;
}
inline double UnsignedFloat(u32 bits, unsigned mantissa) {
    const u32 e = bits >> mantissa, m = bits & ((1U << mantissa) - 1);
    if (!e) return std::ldexp(double(m), -14 - int(mantissa));
    if (e == 31) return m ? 0.0 : 65504.0;
    return std::ldexp(1.0 + double(m) / double(1U << mantissa), int(e) - 15);
}
// This native capture pass stores display-encoded values in the UFfloat
// texture. Palette proof: raw quantized blue(78,159,207) exactly matches the
// game's displayed card; applying another sRGBtransfer washes it to150,207,233.
inline u8 DisplayByte(double encoded) {
    return static_cast<u8>(std::clamp(std::lround(encoded*255),0L,255L));
}
inline bool Decode(const Capture& c, std::vector<u8>& rgba) {
    if (c.tiled.size() != kStorageSize) return false;
    std::array<u8, 2048> f11{};
    std::array<u8, 1024> f10{};
    for (u32 i = 0; i < f11.size(); ++i) f11[i] = DisplayByte(UnsignedFloat(i, 6));
    for (u32 i = 0; i < f10.size(); ++i) f10[i] = DisplayByte(UnsignedFloat(i, 5));
    rgba.resize(kWidth * kHeight * 4);
    for (u32 y = 0; y < kHeight; ++y) for (u32 x = 0; x < kWidth; ++x) {
        const auto p = TiledOffset(x + kCropX, 899 - (y + kCropY));
        if (p + 4 > c.tiled.size()) return false;
        const u32 v = U32(c.tiled.data() + p);
        auto* dst = rgba.data() + (y * kWidth + x) * 4;
        dst[0] = f11[v & 2047]; dst[1] = f11[(v >> 11) & 2047];
        dst[2] = f10[(v >> 22) & 1023]; dst[3] = 255;
    }
    return true;
}

class Sampler {
public:
    void SetAutoHint(u64 mailbox, u32 seq) {
        std::lock_guard lock(mutex_);
        if (auto_mailbox_ != mailbox || auto_seq_ != seq) auto_captured_ = 0;
        auto_mailbox_ = mailbox; auto_seq_ = seq;
    }
    u32 AutoCapturedSeq() const { std::lock_guard lock(mutex_); return auto_captured_; }
    void Reset() {
        std::lock_guard lock(mutex_);
        cache_.reset(); owner_valid_ = false; global_ = main_ = 0; auto_mailbox_ = 0; auto_seq_ = auto_captured_ = 0; ++generation_;
    }
    template<class Host> void Publish(const Host& h) {
        const bool build = h.title_id == sw_build::kTitleId && h.main_base &&
                           sw_build::BytesEqual(h.build_id, sw_build::kBuildId, sw_build::kKnownBuildIdBytes);
        const bool title = h.get_i64 && h.get_i64(h.userdata, "sw.state.title", 0) == 1;
        if (!build || title) { Reset(); Emit(h); return; }
        Owner owner{};
        if (!ReadOwner(h, owner)) {
            { std::lock_guard lock(mutex_); owner_valid_ = false; }
            Emit(h); return;
        }
        // Validate a newly read owner before any later pointer-read early return.
        // Loading or a missing G cannot keep the previous owner's card visible.
        {
            std::lock_guard lock(mutex_);
            if ((main_ && main_ != h.main_base) || (cache_ && cache_->resource.owner != owner)) cache_.reset();
            owner_ = owner; owner_valid_ = true;
        }
        u64 slot{}, g{};
        if (!MainSlot(h, h.main_base + sw_variant::kMain.field_slot, slot) || !Pointer(h, slot, g)) { Emit(h); return; }
        {
            std::lock_guard lock(mutex_);
            if ((main_ && main_ != h.main_base) || (global_ && global_ != g) ||
                (cache_ && cache_->resource.owner != owner)) cache_.reset();
            main_ = h.main_base; global_ = g; owner_ = owner; owner_valid_ = true;
        }
        u64 auto_mailbox{}; u32 auto_seq{};
        { std::lock_guard lock(mutex_); auto_mailbox=auto_mailbox_; auto_seq=auto_seq_; }
        u64 canonical_hash{};
        if (!CanonicalMetadata(h,owner,canonical_hash)) {
            { std::lock_guard lock(mutex_); owner_valid_=false; }
            Emit(h); return;
        }
        {
            std::lock_guard lock(mutex_);
            if (cache_ && cache_->resource.meta_hash != canonical_hash) cache_.reset();
        }
        Resource before{};
        const bool automatic = ResolveAuto(h,owner,auto_mailbox,auto_seq,before);
        if ((automatic || Resolve(h, owner, before)) && before.meta_hash == canonical_hash) {
            bool changed{};
            {
                std::lock_guard lock(mutex_);
                changed = !cache_ || !SameContent(cache_->resource, before);
                if (changed) cache_.reset();
                else if (automatic) auto_captured_ = auto_seq;
            }
            if (changed) {
                auto next = std::make_shared<Capture>(); next->resource = before;
                next->tiled.resize(kStorageSize);
                Resource after{}; Owner owner_after{};
                if (Read(h, before.backing, next->tiled.data(), next->tiled.size()) &&
                    ReadOwner(h, owner_after) && owner_after == owner &&
                    CanonicalMetadata(h,owner,canonical_hash) && canonical_hash == before.meta_hash &&
                    (automatic ? ResolveAuto(h,owner,auto_mailbox,auto_seq,after) : Resolve(h,owner,after)) && after == before) {
                    // A not-yet-rendered/cleared target must not become the sole cached card.
                    const u32 first = U32(next->tiled.data()); bool nonuniform{};
                    for (std::size_t i = 1; i < 128; ++i) {
                        const auto p = (i * (kStorageSize / 128)) & ~std::size_t(3);
                        if (U32(next->tiled.data() + p) != first) { nonuniform = true; break; }
                    }
                    if (nonuniform) {
                        std::lock_guard lock(mutex_);
                        next->generation = ++generation_;
                        u64 owner_hash = Hash(owner.name.data(), owner.name.size());
                        owner_hash = Hash(&owner.ids, sizeof owner.ids, owner_hash);
                        owner_hash = Hash(&owner.game, sizeof owner.game, owner_hash);
                        char source[160]{};
                        std::snprintf(source, sizeof source, "module:trainer-card/%016llx-%016llx-%016llx-%llu",
                            static_cast<unsigned long long>(owner_hash),
                            static_cast<unsigned long long>(before.meta_hash),
                            static_cast<unsigned long long>(g), static_cast<unsigned long long>(generation_));
                        next->source = source;
                        cache_ = std::move(next);
                        if (automatic) auto_captured_ = auto_seq;
                    }
                }
            }
        }
        Emit(h);
    }
    template<class Sink> bool LoadImage(std::string_view key, void* receiver, Sink sink) const {
        std::shared_ptr<const Capture> capture;
        { std::lock_guard lock(mutex_); if (owner_valid_) capture = cache_; }
        if (!capture || capture->source != key || !sink) return false;
        std::vector<u8> rgba;
        if (!Decode(*capture, rgba)) return false;
        sink(receiver, kWidth, kHeight, rgba.data(), rgba.size());
        return true;
    }
private:
    template<class Host> void Emit(const Host& h) const {
        std::shared_ptr<const Capture> capture;
        { std::lock_guard lock(mutex_); if (owner_valid_) capture = cache_; }
        if (h.publish_i64) h.publish_i64(h.userdata, "sw.card.image.ready", capture ? 1 : 0);
        if (h.publish_text) h.publish_text(h.userdata, "sw.card.image.src", capture ? capture->source.c_str() : "");
    }
    mutable std::mutex mutex_;
    std::shared_ptr<const Capture> cache_;
    Owner owner_{};
    bool owner_valid_{};
    u64 main_{}, global_{}, generation_{}, auto_mailbox_{};
    u32 auto_seq_{}, auto_captured_{};
};
} // namespace sw_trainer_portrait

// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_trainer_portrait.h"
#include <cassert>
#include <map>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace sw_trainer_portrait;
struct Host {
    u64 title_id = sw_build::kSwordTitleId, main_base = 0x80000000;
    u8 build_id[32]{};
    void* userdata{};
    u64 (*get_heap_begin)(void*){};
    int (*read_memory)(void*, u64, void*, std::size_t){};
    int (*is_mapped)(void*, u64, u64){};
    std::int64_t (*get_i64)(void*, const char*, std::int64_t){};
    void (*publish_i64)(void*, const char*, std::int64_t){};
    void (*publish_text)(void*, const char*, const char*){};
};
struct Fixture {
    Host h{};
    static constexpr u64 heap = 0x2104000000ULL, slot = 0x82901000, g = heap + 0x2000,
        tasks = heap + 0x4000, head = heap + 0x5000, weak = heap + 0x6000,
        app = heap + 0x8000, wrapper = heap + 0xa000, ctx = heap + 0xc000,
        meta = heap + 0xf000, impl = heap + 0x11000, view = heap + 0x12000,
        nvn = heap + 0x13000, pool = heap + 0x14000,
        storage = heap + 0x1000000, backing = storage + 0x1000,
        identity = heap + 0x45068fb8, save = heap + 0x15000, save_slot = 0x82902000;
    std::map<u64, std::vector<u8>> memory;
    std::map<std::string, std::int64_t> integers;
    std::map<std::string, std::string> texts;
    unsigned copies{};
    bool title{}, missing_owner{}, mutate_metadata_during_copy{};
    void Add(u64 p, std::size_t n) { memory[p].resize(n); }
    u8* At(u64 p, std::size_t n = 1) {
        auto it = memory.upper_bound(p);
        if (it == memory.begin()) return nullptr;
        --it;
        if (p - it->first > it->second.size() || n > it->second.size() - (p - it->first)) return nullptr;
        return it->second.data() + p - it->first;
    }
    void Put32(u64 p, u32 v) {
        auto* b = At(p, 4); assert(b);
        for (unsigned i = 0; i < 4; ++i) b[i] = u8(v >> (i * 8));
    }
    void Put64(u64 p, u64 v) { Put32(p, u32(v)); Put32(p + 4, u32(v >> 32)); }
    void Vtable(u64 p, u64 vt_offset, u64 fn_offset) {
        const u64 vt = h.main_base + vt_offset;
        Add(vt, 8); Put64(vt, h.main_base + fn_offset); Put64(p, vt);
    }
    Fixture() {
        h.userdata = this;
        std::copy_n(sw_build::kBuildId, sw_build::kKnownBuildIdBytes, h.build_id);
        h.get_heap_begin = [](void*) { return heap; };
        h.is_mapped = [](void* v, u64 p, u64 n) -> int { return static_cast<Fixture*>(v)->At(p, n) != nullptr; };
        h.read_memory = [](void* v, u64 p, void* out, std::size_t n) {
            auto& f = *static_cast<Fixture*>(v);
            if (f.missing_owner && p == identity) return 0;
            auto* b = f.At(p, n); if (!b) return 0;
            std::memcpy(out, b, n);
            if (n == kStorageSize) {
                ++f.copies;
                if (f.mutate_metadata_during_copy) {
                    ++*f.At(meta + 0x1a4); f.mutate_metadata_during_copy = false;
                }
            }
            return 1;
        };
        h.get_i64 = [](void* v, const char* name, std::int64_t fallback) -> std::int64_t {
            auto& f = *static_cast<Fixture*>(v);
            return std::string_view(name) == "sw.state.title" ? f.title : fallback;
        };
        h.publish_i64 = [](void* v, const char* k, std::int64_t x) { static_cast<Fixture*>(v)->integers[k] = x; };
        h.publish_text = [](void* v, const char* k, const char* x) { static_cast<Fixture*>(v)->texts[k] = x; };
        Add(identity, 42); Put32(identity, 123456); *At(identity + 4) = 44;
        for (unsigned i = 0; i < 4; ++i) { *At(identity + 16 + i * 2) = u8("Test"[i]); }
        Add(h.main_base + 0x2610848, 8); Put64(h.main_base + 0x2610848, slot);
        Add(slot, 8); Put64(slot, g); Add(g, 0x90); Put64(g + 0x68, tasks);
        Add(tasks, 0x80); Put64(tasks + 0x78, head);
        Add(head, 0x90); Vtable(head, 0x2500000, 0x15306f0); Put64(head + 0x88, weak);
        Add(weak, 0x70); Put32(weak + 0x54, 1); Put64(weak + 0x60, app + 0x50);
        Add(app, 0x800); Vtable(app, 0x2500010, 0x15360c0); Put64(app + 0x768, wrapper);
        Add(wrapper, 0x80); Vtable(wrapper, 0x2500020, 0x159d610); Put64(wrapper + 0x58, ctx);
        Add(ctx, 0x1800); Put32(ctx + 0x730, 2); Put64(ctx + 0x930, meta); Put64(ctx + 0x538, 2078);
        Put64(ctx + 0x740 + 0x150, impl);
        Add(h.main_base+0x2610798,8); Put64(h.main_base+0x2610798,save_slot);
        Add(save_slot,8); Put64(save_slot,save); Add(save,0x300); Put64(save+0x288,meta-0x60);
        Add(meta, 0x1c8); std::memcpy(At(meta, 26), At(identity + 16, 26), 26);
        Put32(meta + 0x1c, 219218); // real printed snapshots can predate current trainer IDs
        Add(impl, 0x88); Put64(impl + 0x58, 2078); Put64(impl + 0x60, view);
        Add(view, 0x40); Put64(view + 8, nvn);
        Add(nvn, 0xc0); Put64(nvn + 0x10, pool); Put64(nvn + 0x18, 0x1000);
        Put32(nvn + 0x28, 900); Put32(nvn + 0x2c, 900); Put32(nvn + 0x30, 1); Put32(nvn + 0x34, 1);
        Put32(nvn + 0x38, 63); Put32(nvn + 0x78, kStorageSize); Put32(nvn + 0x94, 0x400);
        *At(nvn + 0x64) = 4; *At(nvn + 0x6c) = 1; *At(nvn + 0x6d) = 1;
        Add(pool, 0x80); Put64(pool + 0x30, kStorageSize + 0x1000); Put64(pool + 0x70, storage);
        Add(backing, kStorageSize);
        // Independent coordinates from the pre-existing BNTX decoder: native
        // (117,849), (782,50), (118,849). These check crop, orientation, packed
        // float channels and output pixels without carrying any game fixture.
        Put32(backing + 2864180, 0x000003c0); // red1, green0, blue0
        Put32(backing + 396648, 0x78000000);  // red0, green0, blue1
        Put32(backing + 2864188, 0x751c834e); // native paletteblue78,159,207,alreadydisplayencoded
        Put32(backing + 2864184, 0x001e0000);// red0, green1, blue0
        Put32(backing + kStorageSize / 128, 1); // rejects a cleared target cheaply
    }
    std::string Source() const { return texts.at("sw.card.image.src"); }
    bool Ready() const { return integers.at("sw.card.image.ready") != 0; }
};
struct Image {
    unsigned width{}, height{}, calls{};
    std::vector<u8> pixels;
    static void Sink(void* v, u32 w, u32 h, const u8* p, std::size_t n) {
        auto& x = *static_cast<Image*>(v); x.width = w; x.height = h; ++x.calls;
        x.pixels.assign(p, p + n);
    }
};
void Pipeline() {
    Fixture f; Sampler s; s.Publish(f.h); assert(f.Ready() && f.copies == 1);
    const auto key = f.Source(); Image image;
    assert(s.LoadImage(key, &image, &Image::Sink));
    assert(image.width == 666 && image.height == 800 && image.pixels.size() == 666 * 800 * 4);
    assert((std::array<u8,4>{image.pixels[0],image.pixels[1],image.pixels[2],image.pixels[3]} == std::array<u8,4>{255,0,0,255}));
    assert((std::array<u8,4>{image.pixels[4],image.pixels[5],image.pixels[6],image.pixels[7]} == std::array<u8,4>{0,255,0,255}));
    assert((std::array<u8,4>{image.pixels[8],image.pixels[9],image.pixels[10],image.pixels[11]} == std::array<u8,4>{78,159,207,255}));
    const auto n = image.pixels.size(); assert(image.pixels[n-4] == 0 && image.pixels[n-2] == 255);
    s.Publish(f.h); assert(f.copies == 1 && key == f.Source());
    assert(!s.LoadImage("module:trainer-card/9999", &image, &Image::Sink));
    f.Put64(Fixture::tasks + 0x78, 0); f.Put32(Fixture::weak + 0x54, 0); f.Put64(Fixture::weak + 0x60, 0);
    s.Publish(f.h); assert(f.Ready() && f.Source() == key && f.copies == 1);
    assert(s.LoadImage(key, &image, &Image::Sink)); // no read of freed native objects
    f.missing_owner = true; s.Publish(f.h); assert(!f.Ready());
    assert(!s.LoadImage(key, &image, &Image::Sink));
    f.missing_owner = false; s.Publish(f.h); assert(f.Ready() && f.Source() == key);
    f.Put32(Fixture::identity, 654321); s.Publish(f.h); assert(!f.Ready());
    assert(!s.LoadImage(key, &image, &Image::Sink));
}
void RejectionsAndTornCapture() {
    for (unsigned which = 0; which < 8; ++which) {
        Fixture f; Sampler s;
        if (which == 0) f.h.build_id[0] ^= 1;
        if (which == 1) f.Put32(Fixture::ctx + 0x730, 3); // native NPC table path
        if (which == 2) *f.At(Fixture::meta) = 'F';
        if (which == 3) f.Put32(Fixture::nvn + 0x38, 64);
        if (which == 4) f.Put32(Fixture::nvn + 0x94, 0x300);
        if (which == 5) f.Put64(Fixture::nvn + 0x18, 0x10000000);
        if (which == 6) f.Put64(Fixture::impl + 0x58, 2079);
        if (which == 7) f.Put32(Fixture::nvn + 0x28, 901);
        s.Publish(f.h); assert(!f.Ready() && f.copies == 0);
    }
    Fixture f; Sampler s; f.mutate_metadata_during_copy = true; s.Publish(f.h);
    assert(!f.Ready() && f.copies == 1); s.Publish(f.h); assert(f.Ready() && f.copies == 2);
    const auto old = f.Source(); f.Put32(Fixture::meta + 0x1a8, 2); f.mutate_metadata_during_copy = true;
    s.Publish(f.h); assert(!f.Ready()); s.Publish(f.h); assert(f.Ready() && f.Source() != old);
    f.title = true; s.Publish(f.h); assert(!f.Ready()); Image image;
    assert(!s.LoadImage(old, &image, &Image::Sink));
    f.title = false; f.Put64(Fixture::tasks + 0x78, 0); s.Publish(f.h); assert(!f.Ready());
}
void BufferRotationAndReloadIdentity() {
    Fixture f; Sampler s; s.Publish(f.h); const auto original = f.Source();
    // Another native frame may use another valid storage allocation for exactly
    // the same card. This must not cause a 3.7MB timing-thread copy every frame.
    const u64 next_backing = Fixture::storage + 0x500000;
    f.Add(next_backing, kStorageSize);
    std::memcpy(f.At(next_backing, kStorageSize), f.At(Fixture::backing, kStorageSize), kStorageSize);
    f.Put64(Fixture::pool + 0x30, 0x500000 + kStorageSize);
    f.Put64(Fixture::nvn + 0x18, 0x500000);
    s.Publish(f.h); assert(f.Ready() && f.copies == 1 && f.Source() == original);
    f.Put32(Fixture::meta + 0x1a8, 3); s.Publish(f.h);
    assert(f.copies == 2 && f.Source() != original);

    Fixture missing_global; Sampler old_owner; old_owner.Publish(missing_global.h);
    const auto old = missing_global.Source();
    missing_global.Put32(Fixture::identity, 987654);
    missing_global.Put64(missing_global.h.main_base + 0x2610848, 0);
    old_owner.Publish(missing_global.h); assert(!missing_global.Ready()); Image image;
    assert(!old_owner.LoadImage(old, &image, &Image::Sink));

    Fixture a, b; Sampler reload_a, reload_b;
    b.Put32(Fixture::identity, 654321);
    reload_a.Publish(a.h); reload_b.Publish(b.h);
    assert(a.Ready() && b.Ready() && a.Source() != b.Source());
}
void CanonicalInvalidationWithoutApp() {
    Fixture f; Sampler sampler; sampler.Publish(f.h); assert(f.Ready());
    f.Put32(Fixture::weak+0x54,0); f.Put64(Fixture::weak+0x60,0);
    sampler.Publish(f.h); assert(f.Ready());
    // Sameowner/G, changedprintedpose whilethe nativeapp/contextisabsent.
    ++*f.At(Fixture::meta+0x1a4); sampler.Publish(f.h); assert(!f.Ready());
    assert(sampler.AutoCapturedSeq()==0);
    Fixture missing; Sampler other; other.Publish(missing.h); assert(missing.Ready());
    missing.Put64(Fixture::save+0x288,0); other.Publish(missing.h); assert(!missing.Ready());
}

void AutomaticNativeOutput() {
    Fixture f;
    constexpr u64 mb=0x84c9a000, own=Fixture::heap+0x30000,
                  target=Fixture::heap+0x40000, save=Fixture::heap+0x50000,
                  saveslot=0x82903000;
    f.Add(mb,0x800); f.Add(own,0x300); f.Add(target,0x4a8); f.Add(save,0x300); f.Add(saveslot,8);
    f.Add(f.h.main_base+0x2610798,8); f.Put64(f.h.main_base+0x2610798,saveslot);
    f.Put64(saveslot,save); f.Put64(save+0x288,own);
    std::memcpy(f.At(own+0x60,0x1c8),f.At(Fixture::meta,0x1c8),0x1c8);
    std::memcpy(f.At(target+0xd8,0xc0),f.At(Fixture::nvn,0xc0),0xc0);
    f.Vtable(target,0x2500030,0x5fe5f0);
    f.Put32(mb+0x710,19); f.Put32(mb+0x714,2);
    f.Put64(mb+0x720,target); f.Put64(mb+0x728,own);
    f.Put64(mb+0x730,Fixture::g); f.Put64(mb+0x738,Fixture::app);
    // No native Card app exists at all. Only the retained background output may
    // supply these pixels; the ordinary app resolver must fail.
    f.Put64(Fixture::tasks+0x78,0);
    Owner owner{}; assert(ReadOwner(f.h,owner)); Resource r{};
    assert(!Resolve(f.h,owner,r));
    for (u32 phase : {0U,1U,3U,4U}) {
        f.Put32(mb+0x714,phase); assert(!ResolveAuto(f.h,owner,mb,19,r));
    }
    f.Put32(mb+0x714,2);
    assert(!ResolveAuto(f.h,owner,mb,18,r));
    f.Put64(save+0x288,own+8); assert(!ResolveAuto(f.h,owner,mb,19,r));
    f.Put64(save+0x288,own);
    f.Put32(target+0xd8+0x38,1); assert(!ResolveAuto(f.h,owner,mb,19,r));
    f.Put32(target+0xd8+0x38,63);
    assert(ResolveAuto(f.h,owner,mb,19,r));
    Sampler sampler; sampler.SetAutoHint(mb,19); sampler.Publish(f.h);
    assert(f.Ready() && f.copies==1 && sampler.AutoCapturedSeq()==19);
    const auto key=f.Source(); Image image;
    assert(sampler.LoadImage(key,&image,Image::Sink));
    assert(image.width==kWidth && image.height==kHeight);
    // The host may release all guest objects only after the synchronized copy
    // was confirmed. Worker decode still uses the immutable captured bytes.
    f.Put32(mb+0x714,0); f.Put64(mb+0x720,0); f.Put64(mb+0x728,0);
    sampler.Publish(f.h); assert(f.Ready() && f.copies==1);
    assert(sampler.LoadImage(key,&image,Image::Sink));
    sampler.SetAutoHint(mb,20); assert(sampler.AutoCapturedSeq()==0);
    f.title=true; sampler.Publish(f.h); assert(!f.Ready());
}

}
int main() {
    Pipeline(); RejectionsAndTornCapture(); BufferRotationAndReloadIdentity(); CanonicalInvalidationWithoutApp(); AutomaticNativeOutput();
    assert(DisplayByte(0) == 0 && DisplayByte(1) == 255 && DisplayByte(.5) == 128);
    assert(UnsignedFloat(1,6) == std::ldexp(1.,-20));
    std::cout << "trainer portrait native-format/crop/owner/cache/lifetime tests passed\n";
}

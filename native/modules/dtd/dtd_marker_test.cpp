// Marker regression tests. Synthetic guest memory, no game files or emulator changes.
// clang++ -std=c++17 -O1 -fno-builtin -I <eden>/src dtd_marker_test.cpp -o dtd_marker_test
// Include the implementation so the test exercises the actual refresh/classification/publish paths.
#include "010097F018538000.cpp"
#include <stdio.h>

namespace test {
constexpr uint64_t base = 0x80000000;
static uint8_t mem[0x100000];
static uint64_t bump = 0x1000, deny = 0;
static int failures, checks;
static int64_t ammo_on, chest_on;
uint64_t alloc(size_t n = 0x100) {
    uint64_t a = base + bump; bump += (n + 15) & ~size_t(15);
    if (bump >= sizeof mem) __builtin_trap();
    return a;
}
template<class T> void put(uint64_t a, T v) { memcpy(mem + a - base, &v, sizeof v); }
uint64_t cname(const char* s) {
    size_t n = 0; while (s[n]) ++n;
    uint64_t a = alloc(n + 1); memcpy(mem + a - base, s, n + 1); return a;
}
uint64_t klass(const char* name, uint64_t parent = 0) {
    uint64_t k = alloc(); put(k + 0x10, cname(name)); put(k + 0x58, parent); return k;
}
uint64_t object(uint64_t k) { uint64_t o = alloc(); put(o, k); return o; }
uint64_t str(const char* s) {
    int32_t n = 0; while (s[n]) ++n;
    uint64_t o = alloc(0x18 + 2 * n); put(o + 0x10, n);
    for (int i = 0; i < n; ++i) put<uint16_t>(o + 0x14 + 2 * i, s[i]);
    return o;
}
EdenDsmodBool mapped(void*, uint64_t a, uint64_t n) {
    return a >= base && a - base <= sizeof mem && n <= sizeof mem - (a - base) &&
        !(deny && a <= deny && n > deny - a);
}
EdenDsmodBool read(void*, uint64_t a, void* out, size_t n) {
    if (!mapped(nullptr, a, n)) return 0;
    memcpy(out, mem + a - base, n); return 1;
}
void publish_text(void*, const char*, const char*) {}
void publish(void*, const char* k, int64_t v) {
    if (eq(k, "am.on0")) ammo_on = v;
    if (eq(k, "mk.on0")) chest_on = v;
}
void check(bool ok, const char* label) {
    ++checks; if (!ok) ++failures;
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}
uint64_t spawner(const char* name, uint64_t item, uint64_t parent = 0) {
    uint64_t sp = object(klass(name, parent));
    put(sp + 0x10, alloc()); put(sp + 0x68, item); put(sp + 0x60, str("Chest_A")); return sp;
}
uint64_t position(uint64_t obj) {
    uint64_t nat = alloc(), go = alloc(), list = alloc(), tr = alloc(), h = alloc(), nodes = alloc(), parents = alloc();
    put(obj + 0x10, nat); put(nat + 0x20, go); put(go + 0x20, list); put(list + 8, tr); put(tr + 0x20, go);
    put(tr + 0x28, h); put<int32_t>(tr + 0x30, 0); put(h + 0x18, nodes); put(h + 0x20, parents);
    float trs[12] = {4, -10, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    memcpy(mem + nodes - base, trs, sizeof trs); put<int32_t>(parents, -1); return nodes;
}
}

int main() {
    using namespace test;
    EdenDsmodHostApi host{}; host.is_mapped = mapped; host.read_memory = read; host.publish_i64 = publish; host.publish_text = publish_text;
    Guest g{&host}; MarkerProbe& m = g_mp;
    g_mv_x0 = -50; g_mv_wid = 100; g_mv_top = 0; g_mv_hgt = 300; g_mk_ok = true;
    m.o1 = 0x20; m.o2 = 0x20; m.o3 = 8; m.o4 = 0x28; m.xoff = 0x18; m.poff = 0x20; m.idxoff = 8;
    m.nkeep = 1; m.ci = 1; m.keep[0] = object(klass("InstanceItemChest")); m.mhow[0] = 1;
    uint64_t chest = spawner("SpawnerChestItem", m.keep[0]); m.spw[0] = chest;
    MpRefreshChestIdentity(g, m, 0);
    check(MpChestVisible(m, 0) && m.kind[0] == 1, "verified generic chest is visible");
    MarkerPublish(&host); check(chest_on == 1, "verified chest reaches published marker slot");
    m.opened[0] = 1; check(!MpChestVisible(m, 0), "opened chest is hidden"); m.opened[0] = 0;
    m.mhow[0] = 0; check(!MpChestVisible(m, 0), "failed current position hides chest"); m.mhow[0] = 1;
    uint64_t o2 = spawner("SpawnerChestO2", m.keep[0], g.ptr(chest)); m.spw[0] = o2;
    MpRefreshChestIdentity(g, m, 0); check(MpChestVisible(m, 0) && m.kind[0] == 2, "owned O2 chest has O2 category");
    m.spw[0] = chest; MpRefreshChestIdentity(g, m, 0);
    check(MpChestVisible(m, 0) && m.kind[0] == 1, "same coordinates do not retain previous O2 category");
    m.spw[0] = spawner("SpawnerChestWeapon", m.keep[0], g.ptr(chest)); MpRefreshChestIdentity(g, m, 0);
    check(MpChestVisible(m, 0) && m.kind[0] == 3, "owned weapon chest has weapon category");
    m.spw[0] = spawner("SpawnerChestPot", m.keep[0], g.ptr(chest)); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "pot is not drawn as a chest");
    m.spw[0] = spawner("SpawnerUnknown", m.keep[0], g.ptr(chest)); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "unknown derived spawner is not guessed to be a chest");
    m.spw[0] = chest; put(chest + 0x60, str("Gizmo_MXM_Stone")); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0) && m.mat[0], "material pile is hidden");
    put(chest + 0x60, str("Pot_A")); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "non-chest artwork on generic spawner is hidden");
    put(chest + 0x60, str("Chest_A")); deny = chest + 0x60;
    for (int i = 0; i < 10; ++i) MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "repeated failed identity reads never enable fallback chest");
    deny = 0; MpRefreshChestIdentity(g, m, 0); check(MpChestVisible(m, 0), "chest recovers after successful identity read");
    put<uint64_t>(chest + 0x68, alloc()); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0) && !m.spw[0], "recycled spawner loses old chest ownership");
    for (int i = 0; i < 10; ++i) MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "unowned chest stays hidden");
    MarkerPublish(&host); check(chest_on == 0, "unverified chest clears published marker slot");
    put(chest + 0x68, m.keep[0]); m.spw[0] = chest; put<uint64_t>(chest + 0x10, 0); MpRefreshChestIdentity(g, m, 0);
    check(!MpChestVisible(m, 0), "destroyed spawner cannot verify chest");

    m.npk = 1; m.pick_k = klass("PickupInstanceItem"); m.pk[0] = object(m.pick_k);
    uint64_t nodes = position(m.pk[0]);
    m.npcmdk = 1; m.pcmdk[0] = klass("PickupObjectCommand_SO"); put(m.pk[0] + 0x20, object(m.pcmdk[0]));
    uint64_t data = object(klass("IntegratedItem")); put<int32_t>(data + 0x10, 3016001); put(m.pk[0] + 0x78, data);
    uint64_t ammo = spawner("SpawnerPickupItem", m.pk[0]);
    MpPickupSrtRebuild(m); MpPickupSpawnerHit(g, m, ammo + 0x68, m.pk[0]);
    check(m.pspw[0] == ammo, "pickup census links correct owning spawner");
    MpRefreshPickup(g, m, 0); m.pkind[0] = 2;
    check(MpAmmoVisible(m, 0) && m.pxx[0] == 4 && m.pyy[0] == -10, "current owned ammo is placed from transform");
    g_mk_ok = true; PodAmmoPublish(&host); check(ammo_on == 1, "verified ammo reaches published marker slot");
    deny = nodes; MpRefreshPickup(g, m, 0); PodAmmoPublish(&host);
    check(!m.ph[0] && !MpAmmoVisible(m, 0) && ammo_on == 0, "failed transform clears old published ammo marker");
    deny = 0; MpRefreshPickup(g, m, 0); check(MpAmmoVisible(m, 0), "valid position recovers ammo");
    put<int32_t>(data + 0x10, 3019999); MpRefreshPickup(g, m, 0);
    check(m.ptid[0] == 3019999 && !m.pkind[0] && !MpAmmoVisible(m, 0), "recycled pickup item ID clears cached ammo classification");
    put<int32_t>(data + 0x10, 0); m.pkind[0] = 2; MpRefreshPickup(g, m, 0);
    check(!m.ptid[0] && !m.pkind[0] && !m.ph[0], "zero item ID cannot retain old ammo identity");
    put<uint8_t>(m.pk[0] + 0x34, 1); put<int32_t>(m.pk[0] + 0x38, 3016001); put<uint64_t>(m.pk[0] + 0x78, 0);
    m.pkind[0] = 2; MpRefreshPickup(g, m, 0);
    check(m.ptid[0] == 3016001 && !m.pactual[0] && !MpAmmoVisible(m, 0), "preset-only object cannot become ammo marker");
    put(m.pk[0] + 0x78, data); put<int32_t>(data + 0x10, 3016001); MpRefreshPickup(g, m, 0); m.pkind[0] = 2;
    put<uint64_t>(ammo + 0x68, alloc()); MpRefreshPickup(g, m, 0);
    check(!m.powned[0] && !m.pspw[0], "lost reciprocal ownership clears the owner link (0.37.35: ammo no longer needs one)");
    uint64_t wrong = spawner("SpawnerChestItem", m.pk[0]); m.slow = 0;
    MpPickupSpawnerHit(g, m, wrong + 0x68, m.pk[0]); check(!m.pspw[0], "unrelated spawner reference cannot claim pickup");
    put(ammo + 0x68, m.pk[0]); m.slow = 0; MpPickupSpawnerHit(g, m, ammo + 0x68, m.pk[0]);
    MpRefreshPickup(g, m, 0); check(MpAmmoVisible(m, 0), "ammo recovers after valid owner is rediscovered");

    // Actual item dictionary + icon classification; no guessed hard-coded item category.
    Instance in{}; in.main_base = base + 0x100 - kDataManagerMethodSlot;
    uint64_t mi = alloc(), sk = klass("Singleton`1"), st = alloc(), dm = object(klass("DataManager"));
    put(base + 0x100, mi); put(mi + 0x20, sk); put(sk + kStatics, st); put(st, dm);
    uint64_t table = object(klass("Dictionary`2")), entries = alloc();
    put(dm + 0x10, table); put(table + 0x18, entries); put<int32_t>(table + 0x20, 1); put(entries + 0x30, data);
    m.pkind[0] = 0; PickupKindStep(in, g);
    check(!m.pkind[0], "missing item icon remains retryable");
    put(data + 0x38, str("Ammo_Thumbnail")); PickupKindStep(in, g);
    check(m.pkind[0] == 2 && MpAmmoVisible(m, 0), "verified Ammo_Thumbnail resolves on retry");
    m.pkind[0] = 0; put(data + 0x38, str("AmmoDecoration")); PickupKindStep(in, g);
    check(m.pkind[0] == 1 && !MpAmmoVisible(m, 0), "unrelated Ammo-prefixed name is not ammo");
    m.pkind[0] = 0; put<int32_t>(data + 0x10, 999); PickupKindStep(in, g);
    check(!m.pkind[0], "stale item-table cache cannot classify different ID");
    put<int32_t>(data + 0x10, 3016001); put(data + 0x38, str("Ammo_Thumbnail"));
    MpRefreshPickup(g, m, 0); PickupKindStep(in, g);
    put<uint64_t>(m.pk[0] + 0x10, 0); MpRefreshPickup(g, m, 0); PodAmmoPublish(&host);
    check(m.pdead[0] && !m.ph[0] && !m.ptid[0] && !m.pkind[0] && ammo_on == 0, "destroyed pickup clears identity and published marker");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic test of the Tears of the Kingdom reader and module: builds a fake GameData manager, pouch,
// scene and player actor in a fake guest address space with the layout documented in totk_gamedata.h.
#include "totk_gamedata.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t, uint64_t);

namespace {
std::map<uint64_t, uint8_t> g_b;
std::map<std::string, int64_t> g_i;
std::map<std::string, double> g_f;
std::map<std::string, std::string> g_t;
std::map<std::string, int64_t> g_flags;
template <class T> void put(uint64_t a, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  for (size_t i = 0; i < sizeof(T); ++i) g_b[a + i] = p[i];
}
void putn(uint64_t a, const void* v, size_t n) {
  for (size_t i = 0; i < n; ++i) g_b[a + i] = static_cast<const uint8_t*>(v)[i];
}
EdenDsmodBool ReadMem(void*, uint64_t a, void* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto it = g_b.find(a + i);
    if (it == g_b.end()) return 0;
    static_cast<uint8_t*>(out)[i] = it->second;
  }
  return 1;
}
void PubI(void*, const char* n, int64_t v) { g_i[n] = v; }
void PubF(void*, const char* n, double v) { g_f[n] = v; }
void PubT(void*, const char* n, const char* v) { g_t[n] = v; }
int64_t GetI(void*, const char* n, int64_t fb) { auto it = g_flags.find(n); return it == g_flags.end() ? fb : it->second; }

constexpr uint64_t kBase = 0x7100000000ull, kMgr = 0x7200000000ull, kHeap = 0x7300000000ull;
uint64_t g_heap = kHeap;
uint64_t Alloc(size_t n) { uint64_t a = g_heap; g_heap += (n + 0xff) & ~0xffull; return a; }

// One type block: count, entries (stride bytes each), index of (hash, entry) cells.
uint64_t Block(totk::Type t, const std::vector<uint32_t>& hashes, uint32_t cap) {
  const auto& ti = totk::kTypes[t];
  const uint64_t blk = kMgr + ti.base + 8, entries = Alloc(hashes.size() * ti.stride), index = Alloc(cap * 8ull);
  put(blk, int32_t(hashes.size()));
  put(blk + 4, uint32_t(0)); put(blk + 8, entries); put(blk + 0x10, index); put(blk + 0x18, uint32_t(hashes.size())); put(blk + 0x1c, cap);
  for (uint32_t i = 0; i < cap; ++i) put(index + i * 8ull, uint64_t(0));
  for (uint32_t k = 0; k < hashes.size(); ++k) {
    uint32_t s = hashes[k] % cap;
    for (;; s = (s + 1 >= cap) ? 0 : s + 1) { uint32_t cur; ReadMem(nullptr, index + s * 8ull, &cur, 4); if (!cur) break; }
    put(index + s * 8ull, uint64_t(hashes[k]) | (uint64_t(k) << 32));
  }
  return entries;
}
uint64_t Entry(uint64_t entries, totk::Type t, uint32_t k) { return entries + uint64_t(k) * totk::kTypes[t].stride; }
uint32_t H(const char* s) { return totk::hash(s); }

EdenDsmodHostApi Host() {
  EdenDsmodHostApi h{};
  h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.struct_size = sizeof(h); h.main_base = kBase;
  h.read_memory = ReadMem; h.publish_i64 = PubI; h.publish_f64 = PubF; h.publish_text = PubT; h.get_i64 = GetI;
  const uint8_t id[16] = {0x27, 0x71, 0x78, 0xb7, 0xdb, 0xa1, 0xb6, 0xd4, 0x94, 0x9a, 0x84, 0x77, 0x8d, 0x4a, 0xbc, 0x58};
  std::memcpy(h.build_id, id, 16);
  return h;
}
// String array element at idx: inline buffer form {self+0xc, cap 64, chars}.
void PutName(uint64_t arr, uint32_t idx, const char* s) {
  const uint64_t e = arr + uint64_t(idx) * totk::kStrElem;
  const uint8_t zero[totk::kStrElem] = {0}; putn(e, zero, sizeof zero);
  put(e, e + 0xc); put(e + 8, int32_t(64));
  putn(e + 0xc, s, std::strlen(s) + 1);
}
}  // namespace

int main() {
  // hash vectors from the game files: MurmurHash3 of the name equals the hash in Player.pack / GameDataList
  assert(H("PlayerStatus.Life") == 0x31AB5580 && H("PlayerStatus.MaxLife") == 0xFBE01DA1);
  assert(H("PlayerStatus.ExtraLife") == 0x8703CB91 && H("PlayerStatus.BreakLife") == 0xD6C8BCA9);

  // Int block: 5600 fillers (count sanity) + the flags the module reads.
  std::vector<uint32_t> ints = {H("PlayerStatus.Life"), H("PlayerStatus.MaxLife"), H("PlayerStatus.ExtraLife"),
                                H("PlayerStatus.BreakLife"), H("PlayerStatus.CurrentRupee")};
  for (uint32_t i = 0; i < 5600; ++i) ints.push_back(0x10000000u + i * 7919u);
  const uint64_t ie = Block(totk::T_INT, ints, 8191);
  put(Entry(ie, totk::T_INT, 0) + 0x1c, int32_t(25));
  put(Entry(ie, totk::T_INT, 1) + 0x1c, int32_t(40));
  put(Entry(ie, totk::T_INT, 2) + 0x1c, int32_t(8));
  put(Entry(ie, totk::T_INT, 3) + 0x1c, int32_t(4));
  put(Entry(ie, totk::T_INT, 4) + 0x1c, int32_t(1234));
  // Float: MaxStamina 2500. Enum: current layer Sky. Vec3 fallback unused.
  const uint64_t fe = Block(totk::T_FLOAT, {H("PlayerStatus.MaxStamina")}, 127);
  put(Entry(fe, totk::T_FLOAT, 0) + 0x1c, 2500.0f);
  const uint64_t ee = Block(totk::T_ENUM, {H("MapData.CurrentLayer")}, 127);
  put(Entry(ee, totk::T_ENUM, 0) + 0x1c, H("Sky"));
  // Pouch: Weapon names (2 partitions x 20 slots), EquipIndex, Life.
  const uint64_t se = Block(totk::T_STRARR, {H("Pouch.Weapon.Content.Name")}, 127);
  const uint64_t names = Alloc(40 * totk::kStrElem);
  for (uint32_t i = 0; i < 40; ++i) { const uint8_t z[totk::kStrElem] = {0}; putn(names + i * totk::kStrElem, z, sizeof z); }
  put(Entry(se, totk::T_STRARR, 0) + 0x70, uint32_t(40)); put(Entry(se, totk::T_STRARR, 0) + 0x78, names);
  PutName(names, 20 + 0, "Weapon_Sword_001"); PutName(names, 20 + 3, "Weapon_Sword_070");
  const uint64_t ae = Block(totk::T_INTARR, {H("Pouch.Weapon.EquipIndex"), H("Pouch.Weapon.Content.Life")}, 127);
  const uint64_t eqv = Alloc(8), lifev = Alloc(40 * 4);
  for (int i = 0; i < 40; ++i) put(lifev + 4 * i, int32_t(0));
  put(eqv, int32_t(0)); put(eqv + 4, int32_t(3));  // partition 1 equips slot 3
  put(Entry(ae, totk::T_INTARR, 0) + 0x20, uint32_t(2)); put(Entry(ae, totk::T_INTARR, 0) + 0x28, eqv);
  put(Entry(ae, totk::T_INTARR, 1) + 0x20, uint32_t(40)); put(Entry(ae, totk::T_INTARR, 1) + 0x28, lifev);
  put(lifev + 4 * 20, int32_t(70)); put(lifev + 4 * 23, int32_t(33));
  // Singleton, pouch (partition 1), scene -> resident actor "Player" at (100, 200, -300).
  put(kBase + totk::kMgrSlot, kMgr);
  const uint64_t pouch = Alloc(0x200); put(pouch + 0x188, uint32_t(1)); put(kBase + totk::kPouchSlot, pouch);
  const uint64_t module = Alloc(0x400), scene = Alloc(0x100), comps = Alloc(14 * 8), resident = Alloc(0x40),
                 links = Alloc(0x70), data = Alloc(0x100), actor = Alloc(0x400), nm = Alloc(16);
  put(kBase + totk::kSceneSlot, module); put(module + 0x1e8, scene);
  put(scene + 0x50, int32_t(30)); put(scene + 0x58, comps); put(comps + 13 * 8, resident);
  put(resident + 0x20, int32_t(1)); put(resident + 0x28, links); put(links + 8, data); put(data + 0x40, actor);
  put(actor + totk::kActorName, nm); putn(nm, "Player", 7);
  const float pos[3] = {100.f, 200.f, -300.f}; putn(actor + totk::kActorPos, pos, 12);
  float rot[9] = {0}; rot[2] = 1.f; rot[8] = 0.f;  // local +Z world +X: heading 270? atan2(1, -0)=90
  putn(actor + totk::kActorRot, rot, sizeof rot);

  EdenDsmodHostApi h = Host();
  totk::Reader r; r.h = &h;
  assert(r.bind());
  int32_t v = 0; assert(r.get_int("PlayerStatus.Life", v) && v == 25);
  assert(!r.get_int("Nope", v));
  assert(r.player_actor() == actor);

  const EdenDsmodModuleApi* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
  assert(api && api->title_id == 0x0100F2C0115B6000ull);
  assert(api->supports_build("277178B7DBA1B6D4949A84778D4ABC58"));
  assert(api->supports_build("277178B7DBA1B6D400000000000000000000000000000000"));
  assert(!api->supports_build("277178B7DBA1B6D4AAAAAAAAAAAAAAAA"));
  assert(!api->supports_build("8E9978D50BDD20B4C8395A106C27FFDE"));
  void* inst = api->create(&h, "{}");
  g_flags["@flag:inv_tab"] = 0;
  api->sample(inst, &h);
  assert(g_i["totk.build_ok"] == 1 && g_i["totk.ready"] == 1 && g_i["totk.tables_ok"] == 1);
  assert(g_i["totk.hearts.valid"] == 1 && g_i["totk.hearts.q"] == 25 && g_t["totk.hearts.text"] == "6\xC2\xBC");
  assert(g_t["totk.hearts.max_text"] == "/ 10" && g_t["totk.hearts.extra_text"] == "+2");
  assert(g_i["totk.h.5.q"] == 4 && g_i["totk.h.6.q"] == 1 && g_i["totk.h.6.n"] == 1 && g_i["totk.h.9.y"] == 0);
  assert(g_i["totk.h.10.y"] == 1 && g_i["totk.h.10.q"] == 4 && g_i["totk.h.11.on"] == 1 && g_i["totk.h.12.on"] == 0);
  assert(g_t["totk.rupees.text"] == "1,234");
  assert(g_i["totk.sw.0.n"] == 1 && g_i["totk.sw.2.q"] == 10 && g_i["totk.sw.3.on"] == 0);
  assert(g_i["totk.pos.valid"] == 1 && g_f["totk.pos.x"] == 100.0 && g_f["totk.map.y"] == 300.0);
  assert(std::fabs(g_f["totk.pos.heading"] - 90.0) < 0.01);
  assert(g_t["totk.layer.text"] == "Surface" && g_i["totk.layer"] == 1 && g_t["totk.map.area"] == "Ground");
  assert(g_t["totk.pos.text"] == "0100  0300  0094");
  assert(g_t["totk.eq.weapon.name"] == "Weapon_Sword_070" && g_t["totk.eq.weapon.dur"] == "33");
  assert(g_i["totk.inv.n"] == 2 && g_t["totk.inv.0.name"] == "Weapon_Sword_001" && g_t["totk.inv.0.info"] == "70");
  assert(g_t["totk.inv.1.name"] == "Weapon_Sword_070" && g_i["totk.inv.1.eq"] == 1 && g_i["totk.inv.0.eq"] == 0);
  assert(g_t["totk.inv.2.name"].empty() && g_i["totk.inv.2.on"] == 0 && g_t["totk.sel.name"] == "Weapon_Sword_001");
  api->destroy(inst);

  // Sky by height (the enum flag says Sky above, and is ignored), then Depths, wrong build, null singleton.
  { const float hi[3] = {1.f, 1000.f, 2.f}; putn(actor + totk::kActorPos, hi, 12);
    inst = api->create(&h, "{}"); api->sample(inst, &h);
    assert(g_t["totk.layer.text"] == "Sky" && g_i["totk.layer"] == 2 && g_t["totk.map.area"] == "Sky"); api->destroy(inst); }
  const float deep[3] = {1.f, -900.f, 2.f}; putn(actor + totk::kActorPos, deep, 12);
  inst = api->create(&h, "{}"); api->sample(inst, &h);
  assert(g_t["totk.layer.text"] == "Depths" && g_i["totk.layer"] == 0 && g_t["totk.map.area"] == "UnderGround");
  api->destroy(inst);
  EdenDsmodHostApi bad = h; bad.build_id[10] ^= 1; g_i.clear();
  inst = api->create(&bad, "{}"); api->sample(inst, &bad);
  assert(g_i["totk.build_ok"] == 0 && g_i["totk.hearts.valid"] == 0);
  api->destroy(inst);
  put(kBase + totk::kMgrSlot, uint64_t(0)); g_i.clear();
  inst = api->create(&h, "{}"); api->sample(inst, &h);
  assert(g_i["totk.mgr_ok"] == 0 && g_i["totk.hearts.valid"] == 0 && g_i["totk.ready"] == 0);
  api->destroy(inst);
  std::puts("totk module tests passed");
  return 0;
}

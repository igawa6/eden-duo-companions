// SPDX-License-Identifier: GPL-3.0-or-later
// The Legend of Zelda: Tears of the Kingdom (0100F2C0115B6000) companion module.
// Game version 1.4.3 (v720896), main NSO build id 277178b7dba1b6d4949a84778d4abc58 only.
// Read-only: publishes player status, position, equipment, sages and one inventory category.
// Flag names, table layout and the player actor walk come from totk_gamedata.h (TotK Native, MIT, see
// NOTICE-TotKNative.txt in the package).
#include "totk_gamedata.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unordered_map>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kTitleId = 0x0100F2C0115B6000ull;
constexpr char kBuildPrefix[] = "277178B7DBA1B6D4";
constexpr char kBuildTail[] = "949A84778D4ABC58";

struct Category { const char* cat; uint32_t slots; bool life, stock; const char* equip; };
// Same categories, slot counts and flag names as TotK Native's CATEGORIES table.
constexpr Category kCats[] = {
    {"Weapon", 20, true, false, "EquipIndex"}, {"Bow", 14, true, false, "EquipIndex"},
    {"Shield", 20, true, false, "EquipIndex"}, {"Arrow", 1, false, true, "EquipIndex"},
    {"Armor", 150, false, false, nullptr},     {"Material", 255, false, true, nullptr},
    {"Food", 60, false, true, nullptr},        {"SpecialParts", 30, false, true, nullptr},
    {"KeyItem", 100, false, true, nullptr},
};
// Companion tabs -> pouch categories (the bows tab also lists the arrows).
constexpr int kTabs = 8;
constexpr int kTabCats[kTabs][2] = {{0, -1}, {1, 3}, {2, -1}, {4, -1}, {5, -1}, {6, -1}, {7, -1}, {8, -1}};
constexpr int kRows = 12;       // items per inventory page (4 x 3)
constexpr int kMaxHearts = 40;  // heart slots published

struct Row { std::string id; int32_t life{-1}, count{-1}; bool equipped{}; };
struct Piece { std::string id; int32_t life{-1}; };
struct Sage { bool known{}, joined{}, summoned{}; int32_t cooldown{-1}; };

struct State {
  bool actor_ok{}, hearts_ok{}, pos_ok{}, pouch_ok{};
  int32_t life{}, max{}, extra{}, broken{};
  int32_t rupees{-1}, poes{-1};
  float stamina{-1}, stamina_extra{0}, energy{-1}, energy_extra{0};
  std::string ability;       // actor id of the ability ("Obj_UltraHand")
  float pos[3]{};
  double heading{-1};
  int layer{-1};             // 0 depths, 1 surface, 2 sky
  Piece weapon, bow, shield, armor[3];
  Sage sages[5];
  std::vector<Row> rows;     // the selected tab
};

struct Text {
  bool loaded{};
  std::unordered_map<std::string, std::pair<std::string, std::string>> items;
  std::unordered_map<std::string, int> icon;  // actor id -> icons/<n>.png (art/text/icons.tsv)
  std::string lang{"USen"};
};

struct Instance {
  totk::Reader reader;
  bool build_ok{};
  uint32_t tick{};
  Text text;
  State state;
  int cached_tab{-1};
  uint32_t refresh{};
};

bool BuildMatches(const char* hex) {
  if (!hex || strncasecmp(hex, kBuildPrefix, 16) != 0) return false;
  const char* tail = hex + 16;
  const size_t n = std::strlen(tail);
  bool zeros = true;
  for (size_t i = 0; i < n; ++i) zeros = zeros && tail[i] == '0';
  return n == 0 || zeros || (n >= 16 && strncasecmp(tail, kBuildTail, 16) == 0);
}
EdenDsmodBool SupportsBuild(const char* hex) { return BuildMatches(hex) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE; }

std::string ReadFile(const EdenDsmodHostApi* h, const char* path) {
  if (!h->read_romfs) return {};
  const size_t n = h->read_romfs(h->userdata, path, 0, nullptr, 0);
  if (n == 0 || n > (8u << 20)) return {};
  std::string s(n, '\0');
  if (h->read_romfs(h->userdata, path, 0, s.data(), n) != n) return {};
  return s;
}

// user:art/text/items.tsv (the art pack, see make_art.py): actor id <TAB> name <TAB> description (written by tools/totk/make_art.py).
void LoadText(const EdenDsmodHostApi* h, Text& t) {
  t.loaded = true;
  const std::string lang = ReadFile(h, "user:art/text/lang.txt");
  if (lang.size() >= 4) t.lang = lang.substr(0, 4);
  {
    const std::string ix = ReadFile(h, "user:art/text/icons.tsv");
    size_t pos = 0; int line = 0;
    while (pos < ix.size()) {
      size_t e = ix.find('\n', pos);
      if (e == std::string::npos) e = ix.size();
      const std::string l = ix.substr(pos, e - pos);
      pos = e + 1;
      const size_t tab = l.find('\t');
      if (tab == std::string::npos) t.icon[l] = line;
      else t.icon[l.substr(0, tab)] = std::atoi(l.c_str() + tab + 1);
      ++line;
    }
  }
  const std::string tsv = ReadFile(h, "user:art/text/items.tsv");
  size_t pos = 0;
  while (pos < tsv.size()) {
    size_t e = tsv.find('\n', pos);
    if (e == std::string::npos) e = tsv.size();
    const std::string line = tsv.substr(pos, e - pos);
    pos = e + 1;
    const size_t a = line.find('\t');
    if (a == std::string::npos) continue;
    const size_t b = line.find('\t', a + 1);
    t.items[line.substr(0, a)] = {line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1),
                                  b == std::string::npos ? "" : line.substr(b + 1)};
  }
}

bool Spanish(const Text& t) { return t.lang.size() >= 4 && t.lang.compare(2, 2, "es") == 0; }
struct Chrome { const char *status, *map, *items, *info, *sky, *ground, *depths, *recenter, *page, *waiting, *nopos, *rupees, *autom; };
constexpr Chrome kEs = {"Estado", "Mapa", "Objetos", "Info", "Cielo", "Superficie", "Subsuelo", "Centrar", "Página",
                        "Esperando datos del juego...", "Esperando la posición de Link...", "Rupias", "Auto"};
constexpr Chrome kEn = {"Status", "Map", "Items", "Info", "Sky", "Surface", "Depths", "Recenter", "Page",
                        "Waiting for game data...", "Waiting for Link's position...", "Rupees", "Auto"};

void* Create(const EdenDsmodHostApi* host, const char*) {
  auto* inst = new (std::nothrow) Instance();
  if (!inst) return nullptr;
  inst->reader.h = host;
  char hex[65] = {};
  for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", host->build_id[i]);
  inst->build_ok = BuildMatches(hex);
  LoadText(host, inst->text);
  return inst;
}
void Destroy(void* p) { delete static_cast<Instance*>(p); }

struct Pub {
  const EdenDsmodHostApi* h;
  void i(const char* k, int64_t v) const { h->publish_i64(h->userdata, k, v); }
  void f(const char* k, double v) const { if (std::isfinite(v)) h->publish_f64(h->userdata, k, v); }
  void t(const char* k, const char* v) const { h->publish_text(h->userdata, k, v); }
  void i(const std::string& k, int64_t v) const { i(k.c_str(), v); }
  void t(const std::string& k, const std::string& v) const { t(k.c_str(), v.c_str()); }
};

const char* EnumName(uint32_t hs, const char* const* names, unsigned n) {
  for (unsigned i = 0; i < n; ++i) if (totk::hash(names[i]) == hs) return names[i];
  return nullptr;
}
constexpr const char* kLayers[] = {"Ground", "Sky", "UnderGround", "Underground"};
constexpr const char* kPowers[] = {"UltraHand", "OneTouchBond", "CeilingClipper", "ReverseRecorder", "AutoBuilder"};
constexpr const char* kPowerActors[] = {"Obj_UltraHand", "Obj_OneTouchBond", "Obj_Tooreroof", "Obj_ReverseRecorder", "Obj_AutoBuilder"};
constexpr const char* kSageKeys[] = {"Wind", "Fire", "Water", "Electric", "Soul"};
constexpr const char* kSageNames[] = {"Tulin", "Yunobo", "Sidon", "Riju", "Mineru"};

std::string Group(int64_t v) {  // 12,345
  std::string s = std::to_string(v < 0 ? -v : v), o;
  for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) o += ','; o += s[i]; }
  return v < 0 ? "-" + o : o;
}

// ---- reading (all game memory access) ----
void ReadPlayer(const totk::Reader& r, State& st, uint64_t actor) {
  int32_t v = 0;
  st.actor_ok = actor != 0;
  st.hearts_ok = r.get_int("PlayerStatus.Life", st.life) && r.get_int("PlayerStatus.MaxLife", st.max) &&
                 st.max >= 4 && st.max <= 400 && st.life >= 0;
  if (st.hearts_ok) {
    r.get_int("PlayerStatus.ExtraLife", st.extra);
    r.get_int("PlayerStatus.BreakLife", st.broken);
  }
  if (r.get_int("PlayerStatus.CurrentRupee", v)) st.rupees = v;
  if (r.get_int("PlayerStatus.CurrentMamo", v)) st.poes = v;
  float f;
  if (r.get_float("PlayerStatus.MaxStamina", f)) st.stamina = f;
  if (r.get_float("PlayerStatus.ExtraStamina", f)) st.stamina_extra = f;
  if (r.get_float("PlayerStatus.MaxEnergy", f)) st.energy = f;
  if (r.get_float("PlayerStatus.ExtraEnergy", f)) st.energy_extra = f;
  uint32_t e;
  if (r.get_enum_hash("PlayerStatus.CurrentSpecialPower", e))
    for (int k = 0; k < 5; ++k) if (totk::hash(kPowers[k]) == e) st.ability = kPowerActors[k];
  float pos[3]; bool have = false;
  if (actor && r.rd(actor + totk::kActorPos, pos) && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2])) have = true;
  if (!have) have = r.get_vec3("World_PlayerPos", pos);
  st.pos_ok = have;
  std::string layer;
  if (r.get_enum_hash("MapData.CurrentLayer", e)) if (const char* n = EnumName(e, kLayers, 4)) layer = n;
  if (have) {
    std::memcpy(st.pos, pos, sizeof pos);
    if (pos[1] < -400) layer = "UnderGround";
    else if (layer == "UnderGround" && pos[1] > -100) layer = "Ground";
  }
  st.layer = layer == "Sky" ? 2 : layer == "UnderGround" ? 0 : layer.empty() ? -1 : 1;
  float rot[9];
  if (actor && r.rd(actor + totk::kActorRot, rot) && std::isfinite(rot[2]) && std::isfinite(rot[8]) &&
      std::fabs(rot[2]) <= 2 && std::fabs(rot[8]) <= 2 && (rot[2] != 0 || rot[8] != 0)) {
    double d = std::atan2(rot[2], -rot[8]) * 180.0 / M_PI;  // 0 = north (-Z), clockwise
    st.heading = d < 0 ? d + 360 : d;
  }
  for (int k = 0; k < 5; ++k) {
    char fl[96]; bool got = false, sm = false; int32_t mn = 0, sc = 0;
    std::snprintf(fl, sizeof fl, "PlayerStatus.Companion.%s.IsGet", kSageKeys[k]);
    if (!r.get_bool(fl, got)) continue;
    st.sages[k].known = true; st.sages[k].joined = got;
    std::snprintf(fl, sizeof fl, "PlayerStatus.Companion.%s.IsSummon", kSageKeys[k]);
    if (r.get_bool(fl, sm)) st.sages[k].summoned = sm;
    std::snprintf(fl, sizeof fl, "PlayerStatus.Companion.%s.ForUI.RemainingMinute", kSageKeys[k]);
    const bool a = r.get_int(fl, mn);
    std::snprintf(fl, sizeof fl, "PlayerStatus.Companion.%s.ForUI.RemainingSecond", kSageKeys[k]);
    if (a && r.get_int(fl, sc) && mn >= 0 && sc >= 0) st.sages[k].cooldown = mn * 60 + sc;
  }
}

// One category of the current pouch partition as compact rows.
void ReadCategory(const totk::Reader& r, int ci, uint32_t part, std::vector<Row>& rows) {
  const Category& c = kCats[ci];
  char flag[96];
  totk::Arr a, b;
  std::snprintf(flag, sizeof flag, "Pouch.%s.Content.Name", c.cat);
  if (!r.arr(totk::T_STRARR, flag, a) || a.count < 2 * c.slots) return;
  static thread_local char names[255][totk::kNameMax];
  if (!r.names(a, part * c.slots, c.slots, names)) return;
  int32_t life[255], cnt[255]; bool hl = false, hs = false;
  if (c.life) {
    std::snprintf(flag, sizeof flag, "Pouch.%s.Content.Life", c.cat);
    hl = r.arr(totk::T_INTARR, flag, b) && b.count >= 2 * c.slots && r.words(b, part * c.slots, c.slots, life);
  }
  if (c.stock) {
    std::snprintf(flag, sizeof flag, "Pouch.%s.Content.StockNum", c.cat);
    hs = r.arr(totk::T_INTARR, flag, b) && b.count >= 2 * c.slots && r.words(b, part * c.slots, c.slots, cnt);
  }
  int equipped = -1, armor_eq[3] = {-1, -1, -1};
  int32_t e[2];
  if (c.equip) {
    std::snprintf(flag, sizeof flag, "Pouch.%s.%s", c.cat, c.equip);
    if (r.arr(totk::T_INTARR, flag, b) && b.count >= 2 && r.words(b, 0, 2, e)) equipped = e[part];
  }
  if (std::strcmp(c.cat, "Armor") == 0) {
    static const char* const kA[] = {"Pouch.Armor.EquipIndexes.Head", "Pouch.Armor.EquipIndexes.Upper", "Pouch.Armor.EquipIndexes.Bottom"};
    for (int k = 0; k < 3; ++k) if (r.arr(totk::T_INTARR, kA[k], b) && b.count >= 2 && r.words(b, 0, 2, e)) armor_eq[k] = e[part];
  }
  for (uint32_t s = 0; s < c.slots; ++s) {
    if (!names[s][0]) continue;
    Row row;
    row.id = names[s];
    if (hl) row.life = life[s];
    if (hs) row.count = cnt[s];
    row.equipped = (c.equip && int(s) == equipped) || int(s) == armor_eq[0] || int(s) == armor_eq[1] || int(s) == armor_eq[2];
    rows.push_back(std::move(row));
  }
}

void ReadEquipment(const totk::Reader& r, State& st, uint32_t part) {
  auto one = [&](int ci, const char* equip_flag, Piece& out) {
    const Category& c = kCats[ci];
    char flag[96]; totk::Arr a, b; int32_t idx[2];
    if (!r.arr(totk::T_INTARR, equip_flag, b) || b.count < 2 || !r.words(b, 0, 2, idx)) return;
    const int32_t ix = idx[part];
    if (ix < 0 || uint32_t(ix) >= c.slots) return;
    std::snprintf(flag, sizeof flag, "Pouch.%s.Content.Name", c.cat);
    char name[1][totk::kNameMax];
    if (!r.arr(totk::T_STRARR, flag, a) || a.count < 2 * c.slots || !r.names(a, part * c.slots + uint32_t(ix), 1, name) || !name[0][0]) return;
    out.id = name[0];
    if (c.life) {
      std::snprintf(flag, sizeof flag, "Pouch.%s.Content.Life", c.cat);
      int32_t lf;
      if (r.arr(totk::T_INTARR, flag, b) && b.count >= 2 * c.slots && r.words(b, part * c.slots + uint32_t(ix), 1, &lf)) out.life = lf;
    }
  };
  one(0, "Pouch.Weapon.EquipIndex", st.weapon);
  one(1, "Pouch.Bow.EquipIndex", st.bow);
  one(2, "Pouch.Shield.EquipIndex", st.shield);
  one(4, "Pouch.Armor.EquipIndexes.Head", st.armor[0]);
  one(4, "Pouch.Armor.EquipIndexes.Upper", st.armor[1]);
  one(4, "Pouch.Armor.EquipIndexes.Bottom", st.armor[2]);
}

// ---- publishing (pure: State + flags -> values) ----
struct Flags { int tab{}, page{}, sel{}, layer{}; };

std::string Name(const Text& t, const std::string& id) {
  // Variants without an own entry (elixirs, fused weapons) fall back to the base actor's name.
  for (std::string key = id;;) {
    auto it = t.items.find(key);
    if (it != t.items.end() && !it->second.first.empty()) return it->second.first;
    const size_t u = key.rfind('_');
    if (u == std::string::npos || u < 4) return id;
    key.resize(u);
  }
}
int Icon(const Text& t, const std::string& id) {
  auto it = t.icon.find(id);
  return it == t.icon.end() ? -1 : it->second;
}

std::string HeartsText(int32_t q) {  // "7¾"
  static const char* const kFrac[] = {"", "\xC2\xBC", "\xC2\xBD", "\xC2\xBE"};
  return std::to_string(q / 4) + kFrac[q % 4];
}

void PublishState(const Pub& p, const Text& t, const State& st, const Flags& fl) {
  const Chrome& c = Spanish(t) ? kEs : kEn;
  p.t("ui.status", c.status); p.t("ui.map", c.map); p.t("ui.items", c.items); p.t("ui.info", c.info);
  p.t("ui.sky", c.sky); p.t("ui.ground", c.ground); p.t("ui.depths", c.depths); p.t("ui.recenter", c.recenter);
  p.t("ui.auto", c.autom); p.t("ui.waiting", c.waiting); p.t("ui.nopos", c.nopos); p.t("ui.rupees", c.rupees);
  p.i("totk.ready", st.actor_ok ? 1 : 0);
  p.i("totk.hearts.valid", st.hearts_ok ? 1 : 0);
  p.i("totk.pos.valid", st.pos_ok ? 1 : 0);
  p.i("totk.pouch_ok", st.pouch_ok ? 1 : 0);
  if (st.hearts_ok) {
    // Like TotK Native: one grid, bonus hearts after the maximum, gloom hearts at the end of the maximum.
    const int slots = std::min(kMaxHearts, (st.max + st.extra + 3) / 4);
    for (int i = 0; i < kMaxHearts; ++i) {
      const std::string b = "totk.h." + std::to_string(i);
      const bool on = i < slots, yellow = on && i * 4 >= st.max;
      p.i(b + ".on", on ? 1 : 0);
      p.i(b + ".n", on && !yellow ? 1 : 0);
      p.i(b + ".y", yellow ? 1 : 0);
      p.i(b + ".q", on ? std::clamp((yellow ? st.max + st.extra : st.life) - 4 * i, 0, 4) : 0);
      p.i(b + ".k", on && !yellow && i * 4 + 4 > st.max - st.broken && st.broken > 0 ? 1 : 0);
    }
    p.t("totk.hearts.text", HeartsText(st.life));
    p.t("totk.hearts.max_text", "/ " + HeartsText(st.max));
    p.t("totk.hearts.extra_text", st.extra ? "+" + HeartsText(st.extra) : "");
    p.i("totk.hearts.q", st.life); p.i("totk.hearts.max_q", st.max);
  }
  p.t("totk.rupees.text", st.rupees >= 0 ? Group(st.rupees) : "-");
  p.t("totk.poes.text", st.poes >= 0 ? Group(st.poes) : "-");
  // stamina wheels (1000 per wheel, extra wheels in gold) and battery cells
  const double wheels = st.stamina >= 0 ? (st.stamina + st.stamina_extra) / 1000.0 : 0;
  const int base_wheels = st.stamina >= 0 ? int(std::ceil(st.stamina / 1000.0 - 1e-6)) : 0;
  for (int i = 0; i < 10; ++i) {
    const std::string b = "totk.sw." + std::to_string(i);
    const bool on = wheels > i + 1e-6;
    const bool gold = i >= base_wheels;
    p.i(b + ".on", on ? 1 : 0);
    p.i(b + ".n", on && !gold ? 1 : 0);
    p.i(b + ".g", on && gold ? 1 : 0);
    p.i(b + ".q", on ? int(std::lround(std::clamp(wheels - i, 0.0, 1.0) * 20)) : 0);
  }
  p.t("totk.stamina.text", st.stamina >= 0 ? std::to_string(int(std::lround(wheels * 10)) / 10) + (int(std::lround(wheels * 10)) % 10 ? "." + std::to_string(int(std::lround(wheels * 10)) % 10) : "") : "-");
  const double cells = st.energy >= 0 ? (st.energy + st.energy_extra) / 1000.0 : 0;
  const int base_cells = st.energy >= 0 ? int(std::ceil(st.energy / 1000.0 - 1e-6)) : 0;
  for (int i = 0; i < 10; ++i) {
    const std::string b = "totk.bat." + std::to_string(i);
    const bool on = cells > i + 1e-6, gold = i >= base_cells;
    p.i(b + ".on", st.energy >= 0 && i < int(std::ceil(cells - 1e-6)) ? 1 : 0);
    p.i(b + ".n", on && !gold ? 1 : 0);
    p.i(b + ".g", on && gold ? 1 : 0);
    p.i(b + ".q", on ? int(std::lround(std::clamp(cells - i, 0.0, 1.0) * 20)) : 0);
  }
  p.i("totk.energy.on", st.energy >= 0 ? 1 : 0);
  p.t("totk.energy.text", st.energy >= 0 ? std::to_string(int(std::lround(cells * 10)) / 10) : "-");
  // position and layer
  p.i("totk.layer", st.layer);
  const int area = fl.layer ? (fl.layer == 1 ? 2 : fl.layer == 2 ? 1 : 0) : (st.layer < 0 ? 1 : st.layer);
  static const char* const kAreas[] = {"UnderGround", "Ground", "Sky"};
  p.t("totk.map.area", kAreas[area]);
  p.t("totk.layer.text", st.layer == 2 ? c.sky : st.layer == 0 ? c.depths : st.layer == 1 ? c.ground : "");
  p.i("totk.map.on_layer", st.pos_ok && area == (st.layer < 0 ? 1 : st.layer) ? 1 : 0);
  if (st.pos_ok) {
    p.f("totk.pos.x", st.pos[0]); p.f("totk.pos.y", st.pos[1]); p.f("totk.pos.z", st.pos[2]);
    p.f("totk.map.x", st.pos[0]); p.f("totk.map.y", -st.pos[2]);
    const double half = 1400;  // world units around Link of the default map view
    p.f("totk.map.vx0", st.pos[0] - half); p.f("totk.map.vx1", st.pos[0] + half);
    p.f("totk.map.vy0", -st.pos[2] - half * 0.88); p.f("totk.map.vy1", -st.pos[2] + half * 0.88);
    char b[64]; std::snprintf(b, sizeof b, "%d  %d  %d", int(st.pos[0]), int(-st.pos[2]), int(st.pos[1]));
    p.t("totk.pos.text", b);
  }
  if (st.heading >= 0) p.f("totk.pos.heading", st.heading);
  // equipment
  auto piece = [&](const char* key, const Piece& pc) {
    const std::string b = std::string("totk.eq.") + key;
    p.t(b + ".name", pc.id.empty() ? "-" : Name(t, pc.id));
    p.i(b + ".ix", Icon(t, pc.id));
    p.t(b + ".dur", pc.life >= 0 ? std::to_string(pc.life) : "");
    p.i(b + ".on", pc.id.empty() ? 0 : 1);
  };
  piece("weapon", st.weapon); piece("bow", st.bow); piece("shield", st.shield);
  piece("head", st.armor[0]); piece("upper", st.armor[1]); piece("lower", st.armor[2]);
  p.t("totk.ability.name", st.ability.empty() ? "" : Name(t, st.ability));
  p.i("totk.ability.ix", st.ability.empty() ? -1 : Icon(t, st.ability));
  // sages
  for (int k = 0; k < 5; ++k) {
    const std::string b = "totk.sage." + std::to_string(k);
    p.t(b + ".name", kSageNames[k]);
    p.i(b + ".joined", st.sages[k].joined ? 1 : 0);
    p.i(b + ".summoned", st.sages[k].summoned ? 1 : 0);
    p.t(b + ".cd", st.sages[k].cooldown > 0 ? std::to_string(st.sages[k].cooldown / 60) + ":" + (st.sages[k].cooldown % 60 < 10 ? "0" : "") + std::to_string(st.sages[k].cooldown % 60) : "");
  }
  // inventory page of the selected tab
  const int pages = std::max(1, int((st.rows.size() + kRows - 1) / kRows));
  const int page = fl.page % pages;
  p.i("totk.inv.tab", fl.tab);
  p.i("totk.inv.n", int64_t(st.rows.size()));
  p.t("totk.inv.page", std::string(c.page) + " " + std::to_string(page + 1) + "/" + std::to_string(pages));
  const int sel = fl.sel >= 0 && fl.sel < kRows ? fl.sel : 0;
  for (int i = 0; i < kRows; ++i) {
    const size_t k = size_t(page) * kRows + size_t(i);
    const std::string b = "totk.inv." + std::to_string(i);
    if (k < st.rows.size()) {
      const Row& r = st.rows[k];
      p.i(b + ".on", 1);
      p.i(b + ".ix", Icon(t, r.id));
      p.t(b + ".name", Name(t, r.id));
      p.t(b + ".info", r.count >= 0 ? "x" + std::to_string(r.count) : r.life >= 0 ? std::to_string(r.life) : "");
      p.i(b + ".eq", r.equipped ? 1 : 0);
      if (i == sel) {
        p.i("totk.sel.on", 1);
        p.t("totk.sel.name", Name(t, r.id));
        p.i("totk.sel.ix", Icon(t, r.id));
        auto it = t.items.find(r.id);
        p.t("totk.sel.desc", it != t.items.end() ? it->second.second : "");
        p.t("totk.sel.info", r.count >= 0 ? "x" + std::to_string(r.count) : r.life >= 0 ? std::to_string(r.life) : "");
        p.i("totk.sel.eq", r.equipped ? 1 : 0);
      }
    } else {
      p.i(b + ".on", 0);
      p.i(b + ".ix", -1); p.t(b + ".name", ""); p.t(b + ".info", ""); p.i(b + ".eq", 0);
      if (i == sel) { p.i("totk.sel.on", 0); p.t("totk.sel.name", ""); p.i("totk.sel.ix", -1); p.t("totk.sel.desc", ""); p.t("totk.sel.info", ""); p.i("totk.sel.eq", 0); }
    }
    p.i(b + ".sel", i == sel ? 1 : 0);
  }
}

Flags ReadFlags(const EdenDsmodHostApi* h) {
  Flags f;
  f.tab = std::clamp(int(h->get_i64(h->userdata, "@flag:inv_tab", 0)), 0, kTabs - 1);
  f.page = std::max(0, int(h->get_i64(h->userdata, "@flag:inv_page", 0)));
  f.sel = std::clamp(int(h->get_i64(h->userdata, "@flag:inv_sel", 0)), 0, kRows - 1);
  f.layer = std::clamp(int(h->get_i64(h->userdata, "@flag:map_layer", 0)), 0, 3);
  return f;
}


void Sample(void* pi, const EdenDsmodHostApi* h) {
  auto* inst = static_cast<Instance*>(pi);
  if (!inst || !h) return;
  ++inst->tick;
  const Pub p{h};
  p.i("totk.build_ok", inst->build_ok ? 1 : 0);
  inst->reader.h = h;
  const Flags fl = ReadFlags(h);
  State empty;
  PublishState(p, inst->text, empty, fl);  // chrome strings and zeros first; overwritten below
  if (!inst->build_ok) return;
  const totk::Reader& r = inst->reader;
  const bool bound = inst->reader.bind();
  p.i("totk.mgr_ok", bound ? 1 : 0);
  if (!bound) return;
  uint32_t n_int = 0;
  inst->reader.count(totk::T_INT, n_int);
  const bool sane = n_int >= totk::kIntCountMin && n_int <= totk::kIntCountMax;
  p.i("totk.int_count", n_int);
  p.i("totk.tables_ok", sane ? 1 : 0);
  if (!sane) return;
  const uint64_t actor = r.player_actor();
  p.i("totk.actor_ok", actor ? 1 : 0);
  State st;
  ReadPlayer(r, st, actor);
  uint32_t part = 0;
  const uint64_t pouch = r.rd64(h->main_base + totk::kPouchSlot);
  st.pouch_ok = pouch && r.rd(pouch + 0x188, part) && part < 2;
  if (st.pouch_ok) {
    ReadEquipment(r, st, part);
    // inventory rows refresh about 3 times a second or when the tab changes
    if (fl.tab != inst->cached_tab || inst->tick >= inst->refresh) {
      inst->state.rows.clear();
      for (int ci : kTabCats[fl.tab]) if (ci >= 0) ReadCategory(r, ci, part, inst->state.rows);
      inst->cached_tab = fl.tab;
      inst->refresh = inst->tick + 20;
    }
    st.rows = inst->state.rows;
  }
  PublishState(p, inst->text, st, fl);
  if (h->log && inst->tick % 300 == 1) {  // about every 5 s: the raw reads, for the run log
    char b[256];
    std::snprintf(b, sizeof b, "[totk] life=%d max=%d extra=%d broken=%d rupees=%d poes=%d stamina=%.0f(+%.0f) energy=%.0f(+%.0f) pos=%.0f,%.0f,%.0f layer=%d sages=%d%d%d%d%d rows=%zu actor=%d",
                  st.life, st.max, st.extra, st.broken, st.rupees, st.poes, st.stamina, st.stamina_extra, st.energy, st.energy_extra,
                  st.pos[0], st.pos[1], st.pos[2], st.layer, st.sages[0].joined, st.sages[1].joined, st.sages[2].joined,
                  st.sages[3].joined, st.sages[4].joined, st.rows.size(), actor ? 1 : 0);
    h->log(h->userdata, 1, b);
  }
}

void Tick(void*, const EdenDsmodHostApi*) {}

const EdenDsmodModuleApi kApi = {
    EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0, EDEN_DSMOD_MODULE_ABI_HASH, kTitleId,
    "Tears of the Kingdom", 0, SupportsBuild, Create, Destroy, Sample, Tick,
};

}  // namespace

extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi* eden_dsmod_get_module(
    uint32_t abi_version, uint64_t abi_hash) {
  if (abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || abi_hash != EDEN_DSMOD_MODULE_ABI_HASH) return nullptr;
  return &kApi;
}

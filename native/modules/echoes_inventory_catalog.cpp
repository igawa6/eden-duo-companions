// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_inventory_catalog.h"
#include "echoes_catalog.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <span>
#include <unordered_map>

namespace echoes {
namespace {
constexpr size_t Budget = 1024 * 1024;
using Bytes = std::vector<uint8_t>;
uint16_t U16(std::span<const uint8_t> b, size_t p) {
  return uint16_t(uint16_t(b[p]) | uint16_t(uint16_t(b[p + 1]) << 8));
}
uint32_t U32(std::span<const uint8_t> b, size_t p) {
  return uint32_t(U16(b, p)) | uint32_t(U16(b, p + 2)) << 16;
}
uint64_t U64(std::span<const uint8_t> b, size_t p) {
  return uint64_t(U32(b, p)) | uint64_t(U32(b, p + 4)) << 32;
}
bool Fits(std::span<const uint8_t> b, uint64_t p, uint64_t n) {
  return p <= b.size() && n <= b.size() - p;
}
bool Allowed(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '/' || c == ':';
  });
}
bool Text(std::span<const uint8_t> b, uint64_t at, uint64_t n, uint64_t pool,
          std::string &out) {
  if (at < pool || !n || n > 256 || !Fits(b, at, n + 1) ||
      b[size_t(at + n)] != 0)
    return false;
  out.assign(reinterpret_cast<const char *>(b.data() + at), size_t(n));
  return Allowed(out);
}
bool CString(std::span<const uint8_t> b, uint64_t at, uint64_t pool,
             std::string &out) {
  if (at < pool || at >= b.size())
    return false;
  size_t n = 0;
  while (n < 256 && Fits(b, at + n, 1) && b[size_t(at + n)])
    ++n;
  return Text(b, at, n, pool, out);
}
bool File(const InventoryReadAssetFn &read, std::string_view path, Bytes &out) {
  const size_t n = read(path, 0, nullptr, 0);
  if (!n || n > Budget)
    return false;
  out.resize(n);
  return read(path, 0, out.data(), n) == n && read(path, 0, nullptr, 0) == n;
}
bool Guest(const ReadFn &read, uint64_t base, uint64_t off, size_t n,
           Bytes &out) {
  if (base > UINT64_MAX - off || base + off > UINT64_MAX - n)
    return false;
  out.resize(n);
  return read(base + off, out.data(), n);
}
// SHA256 authenticates full immutable dispatch blocks without embedding their
// actor/CRC-to-mask constants. The64 round constants are the SHA256 standard.
std::array<uint8_t, 32> Sha256(Bytes b) {
  constexpr std::array<uint32_t, 64> k{
      {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
       0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
       0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
       0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
       0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
       0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
       0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
       0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
       0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
       0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
       0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2}};
  std::array<uint32_t, 8> h{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};
  const uint64_t bits = uint64_t(b.size()) * 8;
  b.push_back(0x80);
  while (b.size() % 64 != 56)
    b.push_back(0);
  for (int i = 7; i >= 0; --i)
    b.push_back(uint8_t(bits >> (8 * i)));
  for (size_t p = 0; p < b.size(); p += 64) {
    std::array<uint32_t, 64> w{};
    for (size_t i = 0; i < 16; ++i) {
      const auto a = p + i * 4;
      w[i] = uint32_t(b[a]) << 24 | uint32_t(b[a + 1]) << 16 |
             uint32_t(b[a + 2]) << 8 | b[a + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
      const auto x = w[i - 15], y = w[i - 2];
      w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) +
             w[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto v = h;
    for (size_t i = 0; i < 64; ++i) {
      const auto a = v[0], e = v[4];
      const uint32_t t1 =
          v[7] + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
          ((e & v[5]) ^ (~e & v[6])) + k[i] + w[i];
      const uint32_t t2 =
          (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
          ((a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]));
      for (size_t j = 7; j > 0; --j)
        v[j] = v[j - 1];
      v[4] += t1;
      v[0] = t1 + t2;
    }
    for (size_t i = 0; i < 8; ++i)
      h[i] += v[i];
  }
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < 32; ++i)
    out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
  return out;
}
bool Digest(const Bytes &b, std::string_view expected) {
  constexpr char hex[] = "0123456789abcdef";
  const auto hash = Sha256(b);
  std::string actual;
  for (auto c : hash) {
    actual.push_back(hex[c >> 4]);
    actual.push_back(hex[c & 15]);
  }
  return actual == expected;
}
struct FieldSpec {
  std::string_view name, type;
  uint16_t kind, offset;
};
struct Sheet {
  Bytes bytes;
  uint64_t pool{}, rows{};
  uint32_t count{}, stride{};
  bool Init(const InventoryReadAssetFn &read, std::string_view file,
            uint32_t expected_count, uint32_t expected_stride,
            std::span<const FieldSpec> fields) {
    if (!File(read, file, bytes) || bytes.size() < 0x30 ||
        std::memcmp(bytes.data(), "gsht", 4) || U32(bytes, 4) != 2)
      return false;
    pool = U64(bytes, 0x10);
    rows = U64(bytes, 0x20);
    count = U32(bytes, 0x28);
    stride = U32(bytes, 0x2c);
    const uint32_t n = U32(bytes, 0x1c);
    const uint32_t expected_roots = expected_stride == 0x130  ? 24U
                                    : expected_stride == 0x48 ? 8U
                                                              : 3U;
    const uint32_t expected_fields =
        expected_stride == 0x130 ? 47U : expected_roots;
    if (count != expected_count || stride != expected_stride || n > 64 ||
        U32(bytes, 0x18) != expected_roots || n != expected_fields ||
        rows != 0x30 + uint64_t(n) * 0x30 ||
        pool != rows + uint64_t(count) * stride || pool >= bytes.size() ||
        !Fits(bytes, rows, uint64_t(count) * stride))
      return false;
    for (const auto &f : fields) {
      size_t matches = 0;
      for (size_t i = 0; i < n; ++i) {
        const size_t p = 0x30 + i * 0x30;
        std::string name, type;
        if (!CString(bytes, U64(bytes, p), pool, name) ||
            !CString(bytes, U64(bytes, p + 8), pool, type))
          return false;
        if (name == f.name) {
          ++matches;
          if (type != f.type || U16(bytes, p + 16) != f.kind ||
              U16(bytes, p + 20) != f.offset)
            return false;
        }
      }
      if (matches != 1)
        return false;
    }
    return true;
  }
  size_t At(size_t i) const { return size_t(rows) + i * stride; }
  bool String(size_t row, size_t off, std::string &out) const {
    const auto p = At(row) + off;
    return Text(bytes, U64(bytes, p), U64(bytes, p + 8), pool, out);
  }
};
int64_t Signed(uint32_t x) {
  return x <= INT32_MAX ? int64_t(x) : int64_t(x) - (INT64_C(1) << 32);
}
int64_t Extend(uint32_t x, unsigned n) {
  const auto sign = uint32_t(1) << (n - 1);
  return (x & sign) ? int64_t(x) - (INT64_C(1) << n) : x;
}
bool Mask(const Bytes &code, uint64_t low, uint64_t start, uint64_t stop,
          uint32_t crc, uint32_t &mask) {
  std::array<uint32_t, 32> r{};
  r[10] = ~crc;
  uint64_t pc = start;
  int64_t a = 0, b = 0;
  for (size_t step = 0; step < 150; ++step) {
    if (pc == stop) {
      mask = r[8];
      return std::has_single_bit(mask);
    }
    if (pc < low || pc % 4 || pc - low > code.size() - 4)
      return false;
    const uint32_t v = U32(code, size_t(pc - low));
    uint64_t next = pc + 4;
    const auto rd = v & 31;
    if ((v & 0xff800000) == 0x52800000 || (v & 0xff800000) == 0x72800000) {
      const unsigned shift = ((v >> 21) & 3) * 16;
      if (shift > 16)
        return false;
      const uint32_t imm = ((v >> 5) & 65535) << shift;
      r[rd] = (v & 0xff800000) == 0x52800000
                  ? imm
                  : (r[rd] & ~(uint32_t(65535) << shift)) | imm;
    } else if ((v & 0xffe0ffe0) == 0x2a0003e0)
      r[rd] = r[(v >> 16) & 31];
    else if ((v & 0xffe0ffe0) == 0x2a2003e0)
      r[rd] = ~r[(v >> 16) & 31];
    else if ((v & 0xffe0fc1f) == 0x6b00001f) {
      a = Signed(r[(v >> 5) & 31]);
      b = Signed(r[(v >> 16) & 31]);
    } else if ((v & 0xfc000000) == 0x14000000) {
      const int64_t target = int64_t(pc) + Extend(v & 0x3ffffff, 26) * 4;
      if (target < 0)
        return false;
      next = uint64_t(target);
    } else if ((v & 0xff000010) == 0x54000000) {
      bool take = false;
      switch (v & 15) {
      case 0:
        take = a == b;
        break;
      case 1:
        take = a != b;
        break;
      case 10:
        take = a >= b;
        break;
      case 11:
        take = a < b;
        break;
      case 12:
        take = a > b;
        break;
      case 13:
        take = a <= b;
        break;
      default:
        return false;
      }
      if (take) {
        const int64_t target = int64_t(pc) + Extend((v >> 5) & 0x7ffff, 19) * 4;
        if (target < 0)
          return false;
        next = uint64_t(target);
      }
    } else
      return false;
    r[31] = 0;
    pc = next;
  }
  return false;
}
bool RuntimeString(const ReadFn &read, uint64_t base, uint64_t offset,
                   size_t length, std::string &out) {
  Bytes b;
  if (!Guest(read, base, offset, length + 1, b) || b.back() != 0)
    return false;
  out.assign(reinterpret_cast<const char *>(b.data()), length);
  return out.find('\0') == std::string::npos;
}
using Messages = std::unordered_map<std::string, std::string>;
bool Localize(std::string_view ref, std::string_view prefix,
              const Messages &messages, std::string &out) {
  if (!ref.starts_with(prefix))
    return false;
  const auto it = messages.find(std::string(ref.substr(prefix.size())));
  if (it == messages.end() || it->second.empty())
    return false;
  out = it->second;
  return true;
}
bool Build(const InventoryReadAssetFn &asset, const ReadFn &read, uint64_t base,
           std::string_view root, InventoryCatalog &out) {
  Bytes accessories_code, outfits_code, again;
  if (!Guest(read, base, 0x2501e20, 0x4e8, accessories_code) ||
      !Guest(read, base, 0x25028ec, 0x2d4, outfits_code) ||
      !Digest(
          accessories_code,
          "43357a69321195698ed526307aa5e1c95701b3e6e34dcacd6f6b4897171be074") ||
      !Digest(
          outfits_code,
          "7b8f072f9cff65abedf5fb0e94196c8dafe308025e198d9a82b2203c827fe55a") ||
      !Guest(read, base, 0x2501e20, 0x4e8, again) ||
      again != accessories_code ||
      !Guest(read, base, 0x25028ec, 0x2d4, again) || again != outfits_code)
    return false;
  Bytes indices, branches;
  if (!Guest(read, base, 0x29176bc, 25, indices) ||
      !Guest(read, base, 0x28da206, 80, branches))
    return false;
  std::string accessory_format, dress_format, ingredient_format, coupon_texture;
  if (!RuntimeString(read, base, 0x2877191, 13, accessory_format) ||
      accessory_format != "Accessory_{0}" ||
      !RuntimeString(read, base, 0x2897874, 9, dress_format) ||
      dress_format != "Dress_{0}" ||
      !RuntimeString(read, base, 0x2875093, 17, ingredient_format) ||
      ingredient_format != "Ingredients{0:D2}" ||
      !RuntimeString(read, base, 0x289b0ef, 17, coupon_texture) ||
      !Allowed(coupon_texture))
    return false;
  Bytes bytes;
  Messages names, gear_descriptions, item_descriptions;
  const std::string messages = std::string(root) + "/message/";
  if (!File(asset, messages + "glossary/Item.msbt", bytes) ||
      !DecodeNativeMessages(bytes, names) ||
      !File(asset, messages + "ui/DressUpMenu.msbt", bytes) ||
      !DecodeNativeMessages(bytes, gear_descriptions) ||
      !File(asset, messages + "ui/ItemMenu.msbt", bytes) ||
      !DecodeNativeMessages(bytes, item_descriptions))
    return false;
  constexpr std::array<FieldSpec, 3> gear_fields{
      {{"symbol", "string", 4, 0},
       {"nameLabel", "string", 4, 16},
       {"explanationLabel", "string", 4, 32}}};
  Sheet accessories, outfits, item_list, items;
  if (!accessories.Init(asset, "region_common/datasheets/ui/UiAccessory.gsheet",
                        28, 0x30, gear_fields) ||
      !outfits.Init(asset, "region_common/datasheets/ui/UiDressUp.gsheet", 11,
                    0x30, gear_fields))
    return false;
  for (unsigned type = 0; type < 2; ++type) {
    const auto &sheet = type == 0 ? accessories : outfits;
    const auto &code = type == 0 ? accessories_code : outfits_code;
    const uint64_t low = type == 0 ? 0x2501e20 : 0x25028ec,
                   start = type == 0 ? 0x2501fac : 0x2502a8c,
                   stop = type == 0 ? 0x2501e20 : 0x25028f0;
    uint32_t seen = 0;
    std::vector<std::string> symbols;
    for (size_t i = 0; i < sheet.count; ++i) {
      InventoryMetadata e;
      e.kind = type == 0 ? InventoryKind::Accessory : InventoryKind::Outfit;
      std::string name, description;
      if (!sheet.String(i, 0, e.symbol) || !sheet.String(i, 16, name) ||
          !sheet.String(i, 32, description) ||
          std::find(symbols.begin(), symbols.end(), e.symbol) !=
              symbols.end() ||
          !Mask(code, low, start, stop, ActorKey(e.symbol), e.mask) ||
          (seen & e.mask) || !Localize(name, "glossary/Item:", names, e.name) ||
          !Localize(description, "ui/DressUpMenu:", gear_descriptions,
                    e.description))
        return false;
      symbols.push_back(e.symbol);
      seen |= e.mask;
      const auto &format = type == 0 ? accessory_format : dress_format;
      e.texture = "romfs:/region_common/ui/IconItem.arc#timg/__Combined.bntx#" +
                  format.substr(0, format.find('{')) + e.symbol;
      out.entries.push_back(std::move(e));
    }
    if (seen != (type == 0 ? 0x0fffffffU : 0x7ffU))
      return false;
  }
  constexpr std::array<FieldSpec, 6> list_fields{
      {{"mSymbol", "string", 4, 0},
       {"itemId", "ItemId", 2, 16},
       {"mBottleContentEffectKind", "BottleContentEffectKind", 2, 28},
       {"mAmount", "float", 3, 32},
       {"mNameTextLabel", "string", 4, 40},
       {"mDescriptionTextLabel", "string", 4, 56}}};
  constexpr std::array<FieldSpec, 3> item_fields{
      {{"symbol", "string", 4, 0},
       {"itemID", "ItemId", 2, 16},
       {"actorSymbolName", "string", 4, 24}}};
  if (!item_list.Init(asset,
                      "region_common/datasheets/ui/UiItemMenuList.gsheet", 26,
                      0x48, list_fields) ||
      !items.Init(asset, "region_common/datasheets/Items.gsheet", 240, 0x130,
                  item_fields))
    return false;
  uint32_t seen_ingredients = 0;
  bool coupon = false;
  for (size_t i = 0; i < item_list.count; ++i) {
    const uint32_t id = U32(item_list.bytes, item_list.At(i) + 16);
    int index = -1;
    if (id >= 212 && id <= 236) {
      const auto value = indices[id - 212];
      if (value < 16)
        index = value;
    }
    if (index < 0 && id != 197)
      continue;
    InventoryMetadata e;
    e.item_id = id;
    float amount{};const auto bits=U32(item_list.bytes,item_list.At(i)+32);std::memcpy(&amount,&bits,4);
    if(!std::isfinite(amount) || amount<0)return false;
    e.edible=index>=0 && U32(item_list.bytes,item_list.At(i)+28)==1 && amount>0;
    e.drink=e.edible && id==0xda; // Original ItemMenu label branch at25216e0.
    e.heal=e.edible?static_cast<int32_t>(std::min(amount,4096.f)):0;

    e.quantity_index = index;
    e.kind = index < 0 ? InventoryKind::Coupon : InventoryKind::Ingredient;
    if (id < 157 || id > 236 ||
        0x251ff24 + uint64_t(branches[id - 157]) * 4 !=
            (index < 0 ? 0x2520164U : 0x251ff24U))
      return false;
    std::string name, description;
    if (!item_list.String(i, 0, e.symbol) || !item_list.String(i, 40, name) ||
        !item_list.String(i, 56, description) ||
        !Localize(name, "glossary/Item:", names, e.name) ||
        !Localize(description, "ui/ItemMenu:", item_descriptions,
                  e.description))
      return false;
    size_t matches = 0;
    for (size_t j = 0; j < items.count; ++j)
      if (U32(items.bytes, items.At(j) + 16) == id) {
        std::string symbol;
        if (!items.String(j, 0, symbol) || symbol != e.symbol ||
            !items.String(j, 24, e.actor))
          return false;
        ++matches;
      }
    if (matches != 1)
      return false;
    if (index >= 0) {
      const uint32_t bit = uint32_t(1) << unsigned(index);
      if (seen_ingredients & bit)
        return false;
      seen_ingredients |= bit;
      const std::string number =
          (index < 10 ? "0" : "") + std::to_string(index);
      e.texture =
          "romfs:/region_common/ui/IconSmoothie.arc#timg/__Combined.bntx#" +
          ingredient_format.substr(0, ingredient_format.find('{')) + number;
    } else {
      if (coupon || coupon_texture != e.symbol)
        return false;
      coupon = true;
      e.texture = "romfs:/region_common/ui/IconItem.arc#timg/__Combined.bntx#" +
                  coupon_texture;
    }
    out.entries.push_back(std::move(e));
  }
  return seen_ingredients == 0xffff && coupon && out.entries.size() == 56;
}
} // namespace
bool LoadInventoryCatalog(const InventoryReadAssetFn &asset, const ReadFn &read,
                          uint64_t base, bool build_matches,
                          std::string_view root, InventoryCatalog &out) {
  out = {};
  if (!asset || !read || !base || base % 4 || !build_matches || root.empty() ||
      root.size() > 32 || !Allowed(root) ||
      root.find('/') == std::string_view::npos)
    return false;
  InventoryCatalog candidate;
  if (!Build(asset, read, base, root, candidate))
    return false;
  out = std::move(candidate);
  return true;
}
} // namespace echoes

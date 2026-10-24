// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_inventory_catalog.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint64_t Base = 0x80400000;
size_t cases{};
void Check(bool ok, const char *message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
Bytes File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}
struct Fixture {
  std::filesystem::path romfs;
  Bytes text, rodata;
  std::map<std::string, Bytes> files;
  std::map<uint64_t, size_t> visits;
  size_t guest_reads{}, asset_reads{}, max_guest{}, max_asset{};
  std::function<void(uint64_t, size_t, void *, size_t)> guest_mutation;
  std::function<void(std::string_view, void *, size_t)> asset_mutation;
  Fixture(std::filesystem::path r, std::filesystem::path segments)
      : romfs(std::move(r)), text(File(segments / "text.bin")),
        rodata(File(segments / "rodata.bin")) {}
  bool Read(uint64_t address, void *out, size_t n) {
    ++guest_reads;
    max_guest = std::max(max_guest, n);
    const size_t visit = ++visits[address];
    if (address < Base)
      return false;
    uint64_t off = address - Base;
    const Bytes *b = &text;
    if (off >= 0x2837000) {
      off -= 0x2837000;
      b = &rodata;
    }
    if (off > b->size() || n > b->size() - off)
      return false;
    std::memcpy(out, b->data() + off, n);
    if (guest_mutation)
      guest_mutation(address, visit, out, n);
    return true;
  }
  size_t Asset(std::string_view path, uint64_t offset, void *out, size_t n) {
    ++asset_reads;
    max_asset = std::max(max_asset, n);
    auto &b = files[std::string(path)];
    if (b.empty())
      b = File(romfs / std::string(path));
    if (!out && !n)
      return b.size();
    if (offset > b.size() || n > b.size() - offset)
      return 0;
    std::memcpy(out, b.data() + offset, n);
    if (asset_mutation)
      asset_mutation(path, out, n);
    return n;
  }
  bool Load(echoes::InventoryCatalog &out, bool build = true) {
    visits.clear();
    return echoes::LoadInventoryCatalog(
        [&](auto p, auto o, auto b, auto n) { return Asset(p, o, b, n); },
        [&](auto p, auto b, auto n) { return Read(p, b, n); }, Base, build,
        "regionUS/USen", out);
  }
};
} // namespace
int main() {
  const char *romfs = std::getenv("ECHOES_ROMFS"),
             *segments = std::getenv("ECHOES_SEGMENTS");
  if (!romfs || !segments) {
    std::cout << "ECHOES_ROMFS/ECHOES_SEGMENTS absent: native inventory "
                 "catalog fixture skipped\n";
    return 77;
  }
  Fixture f(romfs, segments);
  echoes::InventoryCatalog c;
  Check(f.Load(c),
        "original runtime code + native sheets + native locale load");
  Check(c.entries.size() == 56,
        "28 accessories +11 outfits +16 ingredients +coupon");
  const auto entry = [&](std::string_view symbol) -> const auto & {
    auto it = std::find_if(c.entries.begin(), c.entries.end(),
                           [&](const auto &e) { return e.symbol == symbol; });
    Check(it != c.entries.end(), "symbol present");
    return *it;
  };
  Check(entry("FastSwimming").mask == 0x80000 &&
            entry("ZorasScales").mask == 1 && entry("HiJump").mask == 2,
        "native bit dispatch differs from sheet row order");
  Check(entry("GreenColor").mask == 32 &&
            entry("GreenColorAnother").mask == 1024,
        "native outfit mappings");
  Check(entry("FastSwimming").name == "Zora's Flippers" &&
            !entry("FastSwimming").description.empty(),
        "native gear localization");
  Check(entry("FastSwimming").texture.ends_with("#Accessory_FastSwimming") &&
            entry("FastSwimming").actor.empty(),
        "native gear UI texture without invented actor");
  Check(entry("Adventure1").texture.ends_with("#Dress_Adventure1"),
        "native dress texture format");
  Check(entry("SmoothieIngredient00").edible && !entry("SmoothieIngredient00").drink &&
        !entry("SmoothieIngredient01").edible && entry("SmoothieIngredient06").edible &&
        entry("SmoothieIngredient06").drink && entry("SmoothieIngredient15").edible,
        "native effect metadata distinguishes edible food, milk and inedible ingredients");
  Check(entry("SmoothieIngredient15").item_id == 236 &&
            entry("SmoothieIngredient15").quantity_index == 15 &&
            entry("SmoothieIngredient15").actor == "ItemSmoothieIngredient15" &&
            entry("SmoothieIngredient15").texture.ends_with("#Ingredients15"),
        "noncontiguous ingredient ID joins native actor and quantity index");
  Check(entry("ItemMachineCoupon").kind == echoes::InventoryKind::Coupon &&
            entry("ItemMachineCoupon").item_id == 197 &&
            entry("ItemMachineCoupon").texture.ends_with("#ItemMachineCoupon"),
        "coupon native source");
  Check(std::all_of(c.entries.begin(), c.entries.end(),
                    [](const auto &e) {
                      return !e.name.empty() && !e.description.empty() &&
                             !e.texture.empty();
                    }),
        "no partial or guessed metadata");
  Check(f.max_guest <= 0x4e8 && f.max_asset <= 1024 * 1024, "bounded reads");
  ++cases;
  const auto reject = [&]() {
    Check(!f.Load(c) && c.entries.empty(),
          "failure clears previously populated catalog");
    ++cases;
  };
  const auto reads = f.guest_reads;
  Check(!f.Load(c, false) && c.entries.empty() && f.guest_reads == reads,
        "wrong build performs no guest IO");
  ++cases;
  for (auto address : {Base + 0x2501e20, Base + 0x25028ec, Base + 0x2877191,
                       Base + 0x2897874, Base + 0x2875093}) {
    f.guest_mutation = [address](auto at, auto, void *out, auto n) {
      if (at == address && n)
        static_cast<uint8_t *>(out)[0] ^= 1;
    };
    reject();
  }
  f.guest_mutation = [](auto at, auto visit, void *out, auto n) {
    if (at == Base + 0x2501e20 && visit == 2 && n)
      static_cast<uint8_t *>(out)[0] ^= 1;
  };
  reject();
  f.guest_mutation = [](auto at, auto, void *out, auto n) {
    if (at == Base + 0x29176bc && n >= 25)
      static_cast<uint8_t *>(out)[24] = 14;
  };
  reject();
  f.guest_mutation = [](auto at, auto, void *out, auto n) {
    if (at == Base + 0x28da206 && n >= 80)
      static_cast<uint8_t *>(out)[212 - 157] = 1;
  };
  reject();
  f.guest_mutation = {};
  for (const auto suffix :
       {"UiAccessory.gsheet", "UiDressUp.gsheet", "UiItemMenuList.gsheet",
        "Items.gsheet", "Item.msbt", "DressUpMenu.msbt", "ItemMenu.msbt"}) {
    f.asset_mutation = [suffix](auto path, void *out, auto n) {
      if (path.ends_with(suffix) && n)
        static_cast<uint8_t *>(out)[0] ^= 1;
    };
    reject();
  }
  f.asset_mutation = [](auto path, void *out, auto n) {
    if (path.ends_with("UiAccessory.gsheet") && n > 0xf0)
      std::memcpy(static_cast<uint8_t *>(out) + 0xf0,
                  static_cast<uint8_t *>(out) + 0xc0, 16);
  };
  reject();
  f.asset_mutation = [](auto path, void *out, auto n) {
    if (path.ends_with("UiItemMenuList.gsheet") && n > 0x1c0)
      static_cast<uint8_t *>(out)[0x1c0] = 0xff;
  };
  reject();
  f.asset_mutation = [](auto path, void *out, auto n) {
    if (path.ends_with("UiDressUp.gsheet") && n > 0xd7)
      std::memset(static_cast<uint8_t *>(out) + 0xd0, 0xff, 8);
  };
  reject();
  f.asset_mutation = {};
  Check(f.Load(c) && c.entries.size() == 56,
        "recovery rebinds original inputs without cached failure");
  ++cases;
  f.asset_mutation = [](auto path, void *out, auto n) {
    if (path.ends_with("Items.gsheet") && n > 0x18)
      static_cast<uint8_t *>(out)[0x18] ^= 1;
  };
  reject();
  f.asset_mutation = {};
  const auto asset = [&](auto path, auto offset, auto buffer, auto n) {
    if (!buffer && !n)
      return size_t(1024 * 1024 + 1);
    return f.Asset(path, offset, buffer, n);
  };
  const auto guest = [&](auto address, auto buffer, auto n) {
    return f.Read(address, buffer, n);
  };
  Check(!echoes::LoadInventoryCatalog(asset, guest, Base, true, "regionUS/USen",
                                      c) &&
            c.entries.empty(),
        "oversized native asset fails without partial metadata");
  ++cases;
  const auto short_asset = [&](auto path, auto offset, auto buffer, auto n) {
    const auto got = f.Asset(path, offset, buffer, n);
    return buffer && n && got ? got - 1 : got;
  };
  Check(!echoes::LoadInventoryCatalog(short_asset, guest, Base, true,
                                      "regionUS/USen", c) &&
            c.entries.empty(),
        "short native asset reads fail closed");
  ++cases;
  std::cout << cases << " native inventory catalog cases passed\n";
}

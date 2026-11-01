// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../modules/ot0_data.h"
#include "../modules/ot0_reader.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace {
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &t) {
    const auto *b = reinterpret_cast<const uint8_t *>(&t);
    for (size_t i = 0; i < sizeof(T); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *dest, size_t n) {
    auto *b = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
  void zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = 0;
  }
};
constexpr uint64_t mainbase = 0x80000000, root = 0x1400000000,
                   manager = 0x1400001000, save = 0x1400002000,
                   parties = 0x1400003000, records = 0x1400004000,
                   ids = 0x1400005000;
void array(Memory &m, uint64_t p, uint64_t ptr, int count, int cap) {
  m.put(p, ptr);
  m.put(p + 8, int32_t(count));
  m.put(p + 12, int32_t(cap));
}
Memory Fixture() {
  Memory m;
  m.put(mainbase + 0x7cc8cc0, root);
  m.put(root, mainbase + 0x6207610);
  m.put(root + 0x2f0, std::array<uint8_t, 3>{23, 25, 0});
  m.put(mainbase + 0x7cc8540, manager);
  m.put(manager + 0x2c8, save);
  m.put(save + 0x44, uint32_t(0));
  array(m, save + 0x128, parties, 1, 1);
  array(m, save + 0x138, records, 2, 2);
  m.zero(parties, 0x20);
  array(m, parties + 8, ids, 36, 36);
  m.zero(records, 2 * 0x6c);
  m.put(records, uint32_t(900));
  m.put(records + 0xc, uint32_t(15));
  m.put(records + 0x10, uint32_t(727));
  m.put(records + 0x14, uint32_t(103));
  m.put(records + 0x6c, uint32_t(10503));
  m.put(records + 0x6c + 0xc, uint32_t(14));
  m.put(records + 0x6c + 0x10, uint32_t(558));
  m.put(records + 0x6c + 0x14, uint32_t(68));
  m.put(ids, std::array<uint32_t, 8>{901, 10503, 0, 0, 0, 0, 0, 0});
  return m;
}
std::vector<uint8_t> File(const char *p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
using Bytes = std::vector<uint8_t>;
void Append(Bytes &a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
Bytes Text(const std::string &s) {
  assert(s.size() < 32);
  Bytes b{uint8_t(0xa0 | s.size())};
  b.insert(b.end(), s.begin(), s.end());
  return b;
}
Bytes UInt(uint32_t n) {
  return {0xce, uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8),
          uint8_t(n)};
}
Bytes Map(std::initializer_list<std::pair<std::string, Bytes>> entries) {
  Bytes b{uint8_t(0x80 | entries.size())};
  for (auto &[k, v] : entries) {
    Append(b, Text(k));
    Append(b, v);
  }
  return b;
}
Bytes List(std::initializer_list<Bytes> entries) {
  Bytes b{uint8_t(0x90 | entries.size())};
  for (auto &v : entries)
    Append(b, v);
  return b;
}
Bytes Envelope(const Bytes &payload) {
  Bytes b{0, 0, 0, 0, 0, 3};
  uint32_t n = payload.size();
  for (int i = 0; i < 4; ++i)
    b.push_back(uint8_t(n >> (8 * i)));
  Append(b, payload);
  Append(b, {0, 0, 0, 0, 0xc1, 0x83, 0x2a, 0x9e});
  return b;
}
void ParserChecks() {
  auto player = Map({{"m_id", UInt(10503)},
                     {"m_Name", UInt(10503)},
                     {"m_CharaID", UInt(105)}});
  auto text =
      Map({{"m_id", UInt(10503)}, {"m_gametext", List({Text("Célsus")})}});
  auto p = Envelope(Map({{"m_DataList", List({player})}}));
  auto e = Envelope(Map({{"m_DataList", List({text})}}));
  auto names = ot0::DecodeNames(p, e);
  assert(names.valid && names.LookupName(10503) == "Célsus" &&
         names.LookupSprite(10503) == 105);
  auto duplicate = Envelope(Map({{"m_DataList", List({player, player})}}));
  assert(!ot0::DecodeNames(duplicate, e).valid);
  auto bad_text =
      Map({{"m_id", UInt(10503)},
           {"m_gametext", List({Text(std::string("\xc0\xaf", 2))})}});
  auto bad_utf8 = Envelope(Map({{"m_DataList", List({bad_text})}}));
  assert(!ot0::DecodeNames(p, bad_utf8).valid);
  auto payload = Map({{"m_DataList", List({player})}});
  payload.push_back(0xc0);
  assert(!ot0::DecodeNames(Envelope(payload), e).valid);
  auto broken = p;
  broken[6] ^= 1;
  assert(!ot0::DecodeNames(broken, e).valid);
  broken = p;
  broken.back() ^= 1;
  assert(!ot0::DecodeNames(broken, e).valid);
}
} // namespace
int main(int argc, char **argv) {
  ParserChecks();
  auto m = Fixture();
  auto read = [&](uint64_t p, void *out, size_t n) {
    return m.read(p, out, n);
  };
  auto snap = ot0::ReadSnapshot(read, mainbase);
  assert(snap.valid && snap.state == 23 && snap.members[0].id == 901 &&
         snap.members[0].hp == 727 && snap.members[1].level == 14 &&
         !snap.members[2].available);
  assert(snap == ot0::ReadSnapshot(read, mainbase));
  for (int64_t delta : {int64_t(0x10000000000), int64_t(-0x1000)}) {
    m = Fixture();
    m.put(root, mainbase + 0x6207610 + static_cast<uint64_t>(delta));
    assert(!ot0::ReadSnapshot(read, mainbase).valid);
    assert(ot0::ReadSnapshot(read, mainbase, delta) == snap);
    assert(!ot0::ReadSnapshot(read, mainbase, delta + 8).valid);
    m.put(root, mainbase + 0x6207610);
    assert(ot0::ReadSnapshot(read, mainbase, delta) == snap);
  }
  m = Fixture();
  for (uint8_t s : {10, 12, 14, 27, 0}) {
    m.put(root + 0x2f0, s);
    auto x = ot0::ReadSnapshot(read, mainbase);
    assert(!x.valid && x.state == s && !x.members[0].available &&
           x.members[0].hp == 0);
  }
  m = Fixture();
  m.put(root, uint64_t(0));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  m.put(save + 0x130, int32_t(-1));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  m.put(save + 0x144, int32_t(1));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  m.put(parties + 0x18, uint32_t(1));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  m.put(records + 0x6c, uint32_t(900));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  m.put(ids, uint64_t(0x1400000001));
  assert(!ot0::ReadSnapshot(read, mainbase).valid);
  m = Fixture();
  unsigned roots = 0;
  auto racing = [&](uint64_t p, void *out, size_t n) {
    if (p == mainbase + 0x7cc8cc0 && ++roots == 2)
      m.put(p, uint64_t(root + 8));
    return m.read(p, out, n);
  };
  assert(!ot0::ReadSnapshot(racing, mainbase).valid);
  m = Fixture();
  assert(!ot0::ReadSnapshot({}, mainbase).valid);
  assert(!ot0::ReadSnapshot(read, UINT64_MAX).valid);
  assert(!ot0::DecodeNames({}, {}).valid);
  if (argc == 3) {
    auto p = File(argv[1]), e = File(argv[2]);
    auto names = ot0::DecodeNames(p, e);
    assert(names.valid);
    assert(names.LookupName(10503) == "Celsus");
    assert(names.LookupName(901).empty());
    assert(names.LookupSprite(901) == 9001);
    assert(names.LookupSprite(902) == 9002);
    assert(names.LookupSprite(10503) == 105);
    assert(names.LookupName(0).empty());
    assert(names.LookupName(0xffffffff).empty());
    auto broken = p;
    broken[6] ^= 1;
    assert(!ot0::DecodeNames(broken, e).valid);
    broken = p;
    broken.back() ^= 1;
    assert(!ot0::DecodeNames(broken, e).valid);
    broken = p;
    broken.push_back(0);
    assert(!ot0::DecodeNames(broken, e).valid);
    for (size_t n : {size_t(0), size_t(9), p.size() - 1})
      assert(!ot0::DecodeNames(std::span(p).first(n), e).valid);
  }
  std::cout << "ot0 reader/data checks passed\n";
}

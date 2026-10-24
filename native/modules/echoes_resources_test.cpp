// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_resources.h"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <source_location>
#include <thread>
namespace {
size_t count = 0;
void Check(bool b,const std::source_location location=std::source_location::current()) {
  ++count;
  if (!b) {
    std::cerr << "Failed resource check " << count << " at line " << location.line() << '\n';
    std::abort();
  }
}
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {(std::istreambuf_iterator<char>(f)), {}};
}
std::shared_ptr<echoes::NativeFonts> Native(std::vector<uint8_t> bytes) {
  auto n = std::make_shared<echoes::NativeFonts>();
  n->compatible = n->locale_valid = n->body_font_supported =
      n->standard_font_valid = true;
  n->locale_flag = 2;
  n->message_root = "regionUS/USen";
  n->standard_font = std::move(bytes);
  return n;
}
uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
std::vector<uint32_t> Codepoints(std::string_view text) {
  std::vector<uint32_t> result;
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i++]);
    uint32_t cp = c;
    unsigned continuation = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      cp = c & 31;
      continuation = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
      cp = c & 15;
      continuation = 2;
    } else if (c >= 0xf0) {
      cp = c & 7;
      continuation = 3;
    }
    Check(continuation <= text.size() - i);
    while (continuation--)
      cp = (cp << 6) | (uint8_t(text[i++]) & 63);
    if (cp != '\n' && cp != '\r' && cp != '\t')
      result.push_back(cp);
  }
  return result;
}
std::vector<std::string> Words(std::string_view text) {
  std::istringstream input{std::string(text)};
  return {std::istream_iterator<std::string>(input), {}};
}
void ReplaceItemText(void *out, size_t n, bool invalid_utf8) {
  auto *b = static_cast<uint8_t *>(out);
  for (size_t p = 32; p + 16 <= n;) {
    const size_t size = U32(b + p + 4), body = p + 16;
    Check(size <= n - body);
    if (!std::memcmp(b + p, "TXT2", 4)) {
      const size_t text_count = U32(b + body);
      Check(text_count <= (size - 4) / 4);
      for (size_t i = 0; i < text_count; ++i) {
        const size_t start = U32(b + body + 4 + i * 4);
        const size_t end = i + 1 == text_count ? size : U32(b + body + 8 + i * 4);
        if (end - start < 4)
          continue;
        auto *s = b + body + start;
        if (invalid_utf8) {
          s[0] = 0xff;
          s[1] = 0;
        } else {
          s[0] = 0xef;
          s[1] = 0xbf;
          s[2] = 0xbf;
          s[3] = 0;
        }
      }
      return;
    }
    p = body + ((size + 15) & ~size_t(15));
  }
  Check(false);
}
} // namespace
int main() {
  echoes::Resources absent({});
  Check(absent.BuildAttempts()==0 && absent.LastBuildPhase()=="not-attempted");
  Check(!absent.EnsureReady() && !absent.Snapshot());
  Check(absent.BuildAttempts()==1 && absent.LastBuildPhase()=="fonts-unavailable");
  Check(!absent.EnsureReady() && absent.BuildAttempts()==2);
  size_t reads = 0;
  echoes::Resources unavailable(
      [&](std::string_view, uint64_t, void *, size_t) {
        ++reads;
        return size_t(0);
      });
  auto invalid = Native({1, 2, 3});
  invalid->standard_font_valid = false;
  unavailable.SetNativeFonts(invalid);
  Check(!unavailable.EnsureReady() && reads == 0);
  auto malformed = Native({1, 2, 3});
  malformed->message_root = "../../private";
  unavailable.SetNativeFonts(malformed);
  Check(!unavailable.EnsureReady() && reads == 0);
  malformed = Native({1, 2, 3});
  malformed->locale_flag = 0x1000;
  malformed->message_root = "regionCN/CNzh";
  unavailable.SetNativeFonts(malformed);
  Check(!unavailable.EnsureReady() && reads == 0);
  malformed = Native({1, 2, 3});
  malformed->locale_flag = 0x20;
  unavailable.SetNativeFonts(malformed);
  Check(!unavailable.EnsureReady() && reads == 0);
  const char *romfs = std::getenv("ECHOES_ROMFS");
  const char *ttf = std::getenv("ECHOES_FONT_TTF");
  if (!romfs || !ttf) {
    std::cout << count
              << " checks passed; real resource fixtures skipped "
                 "(ECHOES_ROMFS/ECHOES_FONT_TTF)\n";
    return 0;
  }
  auto native = Native(File(ttf));
  Check(!native->standard_font.empty());
  const std::filesystem::path root(romfs);
  size_t io = 0;
  bool fail = false, short_read = false, oversize = false;
  std::function<void()> on_query;
  auto read = [&](std::string_view path, uint64_t offset, void *out,
                  size_t n) -> size_t {
    ++io;
    if (on_query && !out) {
      auto action = std::move(on_query);
      on_query = {};
      action();
    }
    if (fail)
      return 0;
    if (oversize && !out)
      return 1024 * 1024 + 1;
    auto bytes = File(root / std::string(path));
    if (!out && !n)
      return bytes.size();
    if (offset > bytes.size() || n > bytes.size() - size_t(offset))
      return 0;
    std::memcpy(out, bytes.data() + size_t(offset), n);
    return short_read ? n - 1 : n;
  };
  echoes::Resources resources(read);
  resources.SetNativeFonts(native);
  Check(resources.EnsureReady());
  Check(resources.BuildAttempts()==1 && resources.LastBuildPhase()=="ready");
  auto ready = resources.Snapshot();
  Check(ready && ready->echoes.size() == 128 && ready->locale_flag == 2 &&
        ready->message_root == "regionUS/USen" && !ready->body.image.Empty() &&
        !ready->heading.sheet.Empty());
  Check(ready->dungeons.size()==8 && ready->dungeon_floor_labels.contains("Map_1F") &&
        ready->dungeon_floor_labels.at("Map_1F")=="1F");
  for(const auto& map:ready->dungeons)
    for(int32_t floor=map.floor_min;floor<map.floor_min+map.floor_count;++floor) {
      const auto key=std::string(echoes::DungeonFloorMessageKey(map,floor));
      Check(ready->dungeon_floor_labels.contains(key));
      acnh::Image label;
      Check(echoes::RenderBody(ready->body,ready->dungeon_floor_labels.at(key),
                              {48,46,61,255},236,label) && label.h<=48);
    }
  Check(!ready->hero_body.image.Empty() &&
        ready->hero_body.line_height == 36 &&
        !ready->inspection_echo_text.empty());
  const auto verify_inspection = [](const echoes::ReadyResources &resources,
                                    std::string_view name,
                                    std::string_view description,
                                    const echoes::InspectionTextLayout &layout, uint32_t width=echoes::InspectionWidth, uint32_t paragraph_width=0) {
    if(!paragraph_width)paragraph_width=width;
    constexpr std::array<uint8_t, 4> white{255, 255, 255, 255};
    acnh::Image image;
    if (layout.use_body) {
      Check(layout.heading_lines.empty() && layout.title_h == 180);
      Check(echoes::RenderBodyParagraph(width==580?resources.echo_name_body:resources.hero_body, name, white, width,
                                        layout.title_h, image));
    } else {
      Check(!layout.heading_lines.empty() && layout.heading_lines.size() <= 2);
      Check(layout.title_h == (layout.heading_lines.size() == 1 ? 64 : 136));
      std::string complete;
      for (const auto &line : layout.heading_lines) {
        Check(echoes::RenderCompactHeading(resources.heading, line, white, width, image));
        if (!complete.empty())
          complete += ' ';
        complete += line;
      }
      Check(complete == name);
    }
    Check(layout.title_right > 0 && layout.title_right <= width);
    Check(layout.title_bottom > 0 && layout.title_bottom <= layout.title_h);
    Check(layout.description_h >= 1 && layout.description_h <= 1024);
    if (description.empty()) {
      Check(layout.description_h == 1);
    } else {
      const auto prose = echoes::InspectionDescription(description);
      Check(Words(prose) == Words(description));
      Check(echoes::RenderBodyParagraph(resources.body, prose, white, paragraph_width,
                                        layout.description_h, image));
    }
  };
  // Original opening-menu descriptions contain controller pictograms absent
  // from the Standard TTF. Every original row must replay using its separate
  // native pictogram atlas without changing the raw localized metadata.
  Check(ready->prologue.size() == 5);
  Check(ready->prologue_description_display.size() == ready->prologue.size() &&
        ready->prologue_text.size() == ready->prologue.size() &&
        !ready->prologue_body.image.Empty());
  for (size_t i = 0; i < ready->prologue.size(); ++i) {
    const auto at = ready->prologue_text.find(uint32_t(i));
    Check(at != ready->prologue_text.end());
    const auto &raw = ready->prologue[i].description;
    const auto &display = ready->prologue_description_display[i];
    Check(!raw.empty() && !display.empty());
    if (i != 0) Check(raw != display);
    acnh::Image replay;
    Check(echoes::RenderBodyParagraph(ready->prologue_body,
          echoes::InspectionDescription(display), {255,255,255,255},
          echoes::InspectionWidth, at->second.description_h, replay));
  }
  echoes::Resources missing_pictograms(
      [&](std::string_view path, uint64_t offset, void *out, size_t n) {
        return path == "region_common/ui/Pictogram.bffnt" ? size_t(0)
                                                        : read(path, offset, out, n);
      });
  missing_pictograms.SetNativeFonts(native);
  Check(missing_pictograms.EnsureReady());
  const auto without_pictograms = missing_pictograms.Snapshot();
  Check(without_pictograms && without_pictograms->echoes.size() == 128 &&
        !without_pictograms->body.image.Empty() &&
        !without_pictograms->inspection_echo_text.empty() &&
        without_pictograms->prologue_text.empty() &&
        without_pictograms->prologue_description_display.empty());
  size_t wrapped_names = 0;
  bool native_soft_break = false;
  for (const auto &entry : ready->echoes) {
    const auto at = ready->inspection_echo_text.find(entry.crc_key);
    if (entry.name.empty()) {
      // The native metadata includes an unnamed, unlearned alternate actor
      // record. An empty localized name is deliberately not an inspection UI.
      Check(at == ready->inspection_echo_text.end());
      continue;
    }
    if(at == ready->inspection_echo_text.end())std::cerr<<"Missing inspection layout: "<<entry.name<<"\n";
    Check(at != ready->inspection_echo_text.end());
    verify_inspection(*ready, entry.name, entry.description, at->second,580,930);
    if (entry.description.find('\n') != std::string::npos &&
        entry.description.find("\n\n") == std::string::npos) {
      native_soft_break = true;
      Check(echoes::InspectionDescription(entry.description).find('\n') ==
            std::string::npos);
    }
    wrapped_names += at->second.heading_lines.size() == 2;
  }
  Check(ready->automata.size()==6 && ready->inspection_automaton_text.size()==6);
  for(const auto& entry:ready->automata) {
    const auto at=ready->inspection_automaton_text.find(entry.crc_key);
    Check(at!=ready->inspection_automaton_text.end());
    verify_inspection(*ready,entry.name,entry.description,at->second,580,930);
  }
  echoes::Resources missing_automata([&](std::string_view path,uint64_t offset,void* out,size_t n) {
    if(path=="region_common/datasheets/ui/UiMachineSelect.gsheet")return size_t(0);
    return read(path,offset,out,n);
  });
  missing_automata.SetNativeFonts(native);
  Check(missing_automata.EnsureReady());
  Check(missing_automata.Snapshot()->automata.empty() &&
        missing_automata.Snapshot()->inspection_automaton_text.empty() &&
        missing_automata.Snapshot()->echoes.size()==128);
  // The original English fixture includes names wider than the detail column;
  // verify real native wrapping rather than only the short initial Table name.
  Check(wrapped_names != 0);
  Check(native_soft_break);
  Check(echoes::InspectionDescription("One\r\ntwo\n\nthree\r\n\r\nfour") ==
        "One two\n\nthree\n\nfour");
  Check(!ready->inventory_ready && ready->inventory.entries.empty() &&
        ready->heading_inventory_indices.empty());
  auto find = [&](std::string_view actor) -> const echoes::EchoMetadata * {
    for (const auto &e : ready->echoes)
      if (e.actor == actor)
        return &e;
    return nullptr;
  };
  Check(find("Pitty") && find("Pitty")->name == "Holmill");
  Check(find("Octorok") &&
        find("Octorok")->description_label == "Dictionary_Test_Octorok");
  Check(ready->ui.contains("MapMenu") && !ready->ui.at("MapMenu").empty());
  if(!ready->map_labels.empty()) {
    Check(!ready->map_body.image.Empty());
    Check(ready->map_label_sizes.size()==ready->map_labels.size());
    Check(std::any_of(ready->map_label_sizes.begin(),ready->map_label_sizes.end(),[](const auto& size){return size[0]>0 && size[1]>0;}));
  }
  size_t cached = io;
  Check(resources.EnsureReady() && io == cached && resources.BuildAttempts()==1);
  resources.SetNativeFonts(native);
  Check(resources.Snapshot() == ready && resources.EnsureReady() &&
        io == cached);
  resources.SetNativeFonts(nullptr);
  Check(!resources.Snapshot() && !resources.EnsureReady());
  Check(ready->echoes.size() ==
        128); // Existing readers retain immutable ownership.
  resources.SetNativeFonts(native);
  fail = true;
  Check(!resources.EnsureReady() && !resources.Snapshot());
  fail = false;
  Check(resources.EnsureReady());
  resources.SetNativeFonts(nullptr);
  resources.SetNativeFonts(native);
  oversize = true;
  Check(!resources.EnsureReady());
  oversize = false;
  short_read = true;
  Check(!resources.EnsureReady());
  short_read = false;
  Check(resources.EnsureReady());
  auto rebound = std::make_shared<echoes::NativeFonts>(*native);
  rebound->standard_font_address = 0x1234;
  resources.SetNativeFonts(rebound);
  Check(!resources.Snapshot());
  on_query = [&] { resources.SetNativeFonts(nullptr); };
  Check(!resources.EnsureReady() && !resources.Snapshot());
  resources.SetNativeFonts(native);
  Check(resources.EnsureReady());
  // Pause a real build's first read while the sampling thread changes
  // generation.
  resources.SetNativeFonts(nullptr);
  resources.SetNativeFonts(native);
  std::mutex mutex;
  std::condition_variable cv;
  bool blocked = false, released = false, result = true;
  on_query = [&] {
    std::unique_lock lock(mutex);
    blocked = true;
    cv.notify_one();
    cv.wait(lock, [&] { return released; });
  };
  std::thread worker([&] { result = resources.EnsureReady(); });
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return blocked; });
  }
  Check(!resources.Snapshot());
  resources.SetNativeFonts(rebound);
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  cv.notify_one();
  worker.join();
  Check(!result && !resources.Snapshot());
  Check(resources.EnsureReady());
  const char *segments = std::getenv("ECHOES_SEGMENTS");
  if (segments) {
    const auto text = File(std::filesystem::path(segments) / "text.bin");
    const auto rodata = File(std::filesystem::path(segments) / "rodata.bin");
    constexpr uint64_t base = 0x80400000;
    bool guest_fail = false, inventory_fail = false, corrupt_text = false,
         invalid_utf8 = false;
    size_t guest_reads = 0;
    const auto guest = [&](uint64_t address, void *out, size_t n) {
      ++guest_reads;
      if (guest_fail || address < base)
        return false;
      uint64_t offset = address - base;
      const auto *source = &text;
      if (offset >= 0x2837000) {
        offset -= 0x2837000;
        source = &rodata;
      }
      if (offset > source->size() || n > source->size() - offset)
        return false;
      std::memcpy(out, source->data() + offset, n);
      return true;
    };
    const auto inventory_read = [&](std::string_view path, uint64_t offset,
                                    void *out, size_t n) -> size_t {
      if (inventory_fail && path.ends_with("UiAccessory.gsheet"))
        return 0;
      const size_t got = read(path, offset, out, n);
      if (corrupt_text && out && got == n &&
          path.ends_with("glossary/Item.msbt"))
        ReplaceItemText(out, n, invalid_utf8);
      return got;
    };
    echoes::Resources inventory_resources(inventory_read, guest, base);
    inventory_resources.SetNativeFonts(native);
    Check(inventory_resources.EnsureReady());
    auto inventory_ready = inventory_resources.Snapshot();
    Check(inventory_ready && inventory_ready->inventory_ready &&
          inventory_ready->inventory.entries.size() == 56 &&
          inventory_ready->echoes.size() == 128);
    for (size_t i = 0; i < inventory_ready->inventory.entries.size(); ++i) {
      const auto &entry = inventory_ready->inventory.entries[i];
      const auto inspection = inventory_ready->inventory_text.find(uint32_t(i));
      Check(inspection != inventory_ready->inventory_text.end());
      verify_inspection(*inventory_ready, entry.name, entry.description,
                         inspection->second);
      const auto name = Codepoints(entry.name);
      auto cp = name;
      const auto description = Codepoints(entry.description);
      cp.insert(cp.end(), description.begin(), description.end());
      for (const auto c : cp) {
        const auto &body = inventory_ready->body;
        Check(c >= body.first_codepoint &&
              c - body.first_codepoint < body.glyphs.size());
        const auto &glyph = body.glyphs[c - body.first_codepoint];
        Check(glyph.w || glyph.h || glyph.advance);
      }
      const bool heading = std::all_of(name.begin(), name.end(), [&](auto c) {
        return inventory_ready->heading.glyphs.contains(c);
      });
      Check(inventory_ready->heading_inventory_indices.contains(i) == heading);
    }
    const auto cached_io = io, cached_guest = guest_reads;
    Check(inventory_resources.EnsureReady() &&
          inventory_resources.Snapshot() == inventory_ready &&
          io == cached_io && guest_reads == cached_guest);
    const auto reset = [&] {
      inventory_resources.SetNativeFonts(nullptr);
      inventory_resources.SetNativeFonts(native);
    };
    const auto echo_only = [&] {
      Check(inventory_resources.EnsureReady());
      const auto snapshot = inventory_resources.Snapshot();
      Check(snapshot && snapshot->echoes.size() == 128 &&
            !snapshot->body.image.Empty() && !snapshot->inventory_ready &&
            snapshot->inventory.entries.empty() &&
            snapshot->heading_inventory_indices.empty());
    };
    reset();
    guest_fail = true;
    echo_only();
    guest_fail = false;
    reset();
    inventory_fail = true;
    echo_only();
    inventory_fail = false;
    reset();
    corrupt_text = true;
    invalid_utf8 = true;
    echo_only();
    reset();
    invalid_utf8 = false;
    echo_only();
    corrupt_text = false;
    reset();
    on_query = [&] { inventory_resources.SetNativeFonts(nullptr); };
    Check(!inventory_resources.EnsureReady() &&
          !inventory_resources.Snapshot());
    inventory_resources.SetNativeFonts(native);
    Check(inventory_resources.EnsureReady() &&
          inventory_resources.Snapshot()->inventory_ready);
    Check(inventory_ready->inventory.entries.size() == 56);
  } else {
    std::cout << "Inventory resource fixtures skipped (ECHOES_SEGMENTS)\n";
  }
  auto bad_font = Native({0, 1, 0, 0});
  resources.SetNativeFonts(bad_font);
  Check(!resources.Snapshot() && !resources.EnsureReady());
  std::cout << count
            << " resource checks passed with native metadata, fonts, retry and "
               "concurrent invalidation\n";
}

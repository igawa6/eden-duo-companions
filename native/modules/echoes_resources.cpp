// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_resources.h"
#include "echoes_prologue_fonts.h"
#include <algorithm>
#include <array>
namespace echoes {
namespace {
bool LocaleMatches(const NativeFonts &n) {
  constexpr std::array<std::string_view, 15> roots{
      "regionJP/JPja", "regionUS/USen", "regionUS/USfr", "regionUS/USes",
      "regionUS/USpt", "regionEU/EUen", "regionEU/EUfr", "regionEU/EUes",
      "regionEU/EUde", "regionEU/EUit", "regionEU/EUnl", "regionEU/EUru",
      "regionCN/CNzh", "regionTW/TWzh", "regionKR/KRko"};
  for (size_t i = 0; i < roots.size(); ++i)
    if (n.locale_flag == (uint32_t(1) << i))
      return i >= 1 && i <= 11 && n.message_root == roots[i];
  return false;
}
bool Usable(const std::shared_ptr<const NativeFonts> &n) {
  return n && n->compatible && n->locale_valid && n->body_font_supported &&
         n->standard_font_valid && !n->standard_font.empty() &&
         n->standard_font.size() <= MaxNativeFontBytes && LocaleMatches(*n);
}
bool ReadFile(const ResourceReadAssetFn &read, std::string_view path,
              std::vector<uint8_t> &bytes) {
  if (!read)
    return false;
  size_t size = read(path, 0, nullptr, 0);
  if (!size || size > 1024 * 1024)
    return false;
  bytes.resize(size);
  return read(path, 0, bytes.data(), bytes.size()) == size;
}
bool Codepoints(std::string_view text, std::vector<uint32_t> &result) {
  for (size_t i = 0; i < text.size();) {
    uint8_t c = uint8_t(text[i++]);
    uint32_t cp = c, minimum = 0;
    size_t n = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      cp = c & 31;
      n = 1;
      minimum = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      cp = c & 15;
      n = 2;
      minimum = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      cp = c & 7;
      n = 3;
      minimum = 65536;
    } else if (c >= 128)
      return false;
    if (n > text.size() - i)
      return false;
    while (n--) {
      uint8_t next = uint8_t(text[i++]);
      if ((next & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (next & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
    if (cp == '\n' || cp == '\r' || cp == '\t')
      continue;
    if (cp < 32 || cp == 127)
      return false;
    result.push_back(cp);
  }
  return true;
}
bool HeadingSupports(const HeadingFont &f, std::string_view value) {
  std::vector<uint32_t> cp;
  if (value.empty() || !Codepoints(value, cp))
    return false;
  return std::all_of(cp.begin(), cp.end(),
                     [&](uint32_t c) { return f.glyphs.contains(c); });
}
uint32_t InkHeight(const acnh::Image &image) {
  for (int y = image.h; y > 0; --y)
    for (int x = 0; x < image.w; ++x)
      if (image.rgba[(size_t(y - 1) * image.w + x) * 4 + 3])
        return uint32_t(y);
  return 0;
}
bool InspectionLayout(const ReadyResources &resources, std::string_view name,
                      std::string_view description, InspectionTextLayout &out,
                      const FontAtlas *paragraph_font = nullptr, uint32_t width = InspectionWidth, uint32_t paragraph_width = 0) {
  if(!paragraph_width)paragraph_width=width;
  const auto &body = paragraph_font ? *paragraph_font : resources.body;
  constexpr std::array<uint8_t, 4> white{255, 255, 255, 255};
  if (name.empty())
    return false;
  InspectionTextLayout layout;
  const auto heading_fits = [&](std::string_view line) {
    acnh::Image measure;
    return RenderCompactHeading(resources.heading, line, white, width, measure) &&
           InkHeight(measure) <= 64;
  };
  std::string line;
  bool heading_ok = true;
  for (size_t at = 0; at < name.size();) {
    if (name[at] == '\n' || name[at] == '\r' || name[at] == '\t') {
      heading_ok = false;
      break;
    }
    if (name[at] == ' ') {
      ++at;
      continue;
    }
    const auto end = name.find_first_of(" \r\n\t", at);
    const auto word = name.substr(at, end == std::string_view::npos
                                         ? name.size() - at : end - at);
    const std::string candidate = line.empty() ? std::string(word)
                                                : line + " " + std::string(word);
    if (heading_fits(candidate)) {
      line = candidate;
    } else {
      if (line.empty() || !layout.heading_lines.empty() ||
          !heading_fits(word)) {
        heading_ok = false;
        break;
      }
      layout.heading_lines.push_back(std::move(line));
      line = word;
    }
    at = end == std::string_view::npos ? name.size() : end;
  }
  if (heading_ok && !line.empty()) {
    layout.heading_lines.push_back(std::move(line));
    layout.title_h = layout.heading_lines.size() == 1 ? 64 : 136;
  } else {
    layout.heading_lines.clear();
    acnh::Image measure;
    const auto& name_body=width==580?resources.echo_name_body:resources.hero_body;
    if (name_body.image.Empty() ||
        !RenderBodyParagraph(name_body, name, white, width, 180,
                             measure))
      return false;
    layout.use_body = true;
    layout.title_h = 180;
  }
  // Measure immutable native title ink once; the Echo favorite follows the name.
  acnh::Image title_measure;
  if(layout.use_body) {
    const auto& font=width==580?resources.echo_name_body:resources.hero_body;
    if(!RenderBodyParagraph(font,name,white,width,layout.title_h,title_measure))return false;
  } else {
    title_measure=acnh::Image(width,layout.title_h);
    int y=0;
    for(const auto& heading:layout.heading_lines) {
      acnh::Image glyphs;
      if(!RenderCompactHeading(resources.heading,heading,white,width,glyphs))return false;
      for(int row=0;row<glyphs.h && row+y<title_measure.h;++row)
        std::copy_n(glyphs.At(0,row),size_t(glyphs.w)*4,title_measure.At(0,row+y));
      y+=72;
    }
  }
  for(int y=0;y<title_measure.h;++y)for(int x=0;x<title_measure.w;++x)
    if(title_measure.At(x,y)[3]) {
      layout.title_right=std::max(layout.title_right,uint32_t(x+1));
      layout.title_bottom=std::max(layout.title_bottom,uint32_t(y+1));
    }
  layout.description_h = 1;
  if (!description.empty()) {
    const auto prose = InspectionDescription(description);
    acnh::Image measure;
    if (!RenderBodyParagraph(body, prose, white, paragraph_width, 1024,
                             measure))
      return false;
    layout.description_h = std::min(uint32_t(1024), InkHeight(measure) + 8);
    // Confirm that the selected-image callback can render at the compact
    // height; never publish a height that only a clipped raster could satisfy.
    if (!RenderBodyParagraph(body, prose, white, paragraph_width,
                             layout.description_h, measure))
      return false;
  }
  out = std::move(layout);
  return true;
}
void PrepareInspectionText(ReadyResources &out, const NativeFonts &native) {
  std::vector<uint32_t> names;
  for(const auto& entry:out.locations)if(!Codepoints(entry.name,names))return;
  for(const auto& entry:out.map_labels)if(!Codepoints(entry.name,names))return;
  for(const auto& entry:out.prologue)if(!Codepoints(entry.name,names))return;
  for (const auto &entry : out.echoes)
    if (!Codepoints(entry.name, names))
      return;
  for(const auto& entry:out.automata)if(!Codepoints(entry.name,names))return;
  if (out.inventory_ready)
    for (const auto &entry : out.inventory.entries)
      if (!Codepoints(entry.name, names))
        return;
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  // This optional atlas contains names only; descriptions use the existing
  // body atlas. Failure merely leaves unsupported inspection entries absent.
  BodyAtlas(native, names, out.hero_body, 36);
  BodyAtlas(native, names, out.echo_name_body, 29);
  for (const auto &entry : out.echoes) {
    InspectionTextLayout layout;
    if (InspectionLayout(out, entry.name, entry.description, layout, nullptr, 580, 930))
      out.inspection_echo_text.emplace(entry.crc_key, std::move(layout));
  }
  for(const auto& entry:out.automata) {
    InspectionTextLayout layout;
    if(InspectionLayout(out,entry.name,entry.description,layout,nullptr,580,930))
      out.inspection_automaton_text.emplace(entry.crc_key,std::move(layout));
  }
  if (out.inventory_ready)
    for (size_t i = 0; i < out.inventory.entries.size(); ++i) {
      const auto &entry = out.inventory.entries[i];
      InspectionTextLayout layout;
      if (InspectionLayout(out, entry.name, entry.description, layout))
        out.inventory_text.emplace(uint32_t(i), std::move(layout));
    }
  for (size_t i=0;i<out.prologue_description_display.size();++i) {
    InspectionTextLayout layout;
    if(InspectionLayout(out,out.prologue[i].name,out.prologue_description_display[i],layout,
                        &out.prologue_body))
      out.prologue_text.emplace(uint32_t(i),std::move(layout));
  }
}
std::shared_ptr<ReadyResources> Build(const ResourceReadAssetFn &read,
                                      const ReadFn &read_memory,
                                      uint64_t main_base,
                                      const NativeFonts &native,
                                      const std::function<void(std::string)> &progress) {
  progress("optional-native-metadata");
  auto out = std::make_shared<ReadyResources>();
  out->locale_flag = native.locale_flag;
  out->message_root = native.message_root;
  progress("optional:prologue");
  LoadPrologueItems(read,native.message_root,out->prologue);
  progress("optional:dungeon-catalog");
  LoadDungeonCatalog(read,out->dungeons);
  {
    progress("optional:dungeon-art");
    DungeonMapAssets art(read);
    for(const auto& map:out->dungeons) {
      DungeonMapArt metadata;
      if(art.Inspect(map,metadata))out->dungeon_art.emplace(map.key,std::move(metadata));
    }
  }
  progress("optional:warp-catalog");
  LoadNativeWarpCatalog(read,out->warps);
  { // Optional dungeon floor words never withdraw the Echo picker.
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string,std::string> labels;
    if(ReadFile(read,native.message_root+"/message/ui/Map.msbt",bytes) &&
       DecodeNativeMessages(bytes,labels)) {
      if(const auto found=labels.find("MapWarp_Dialog_00");found!=labels.end())
        out->ui.insert_or_assign(found->first,found->second);
      for(const auto& dungeon:out->dungeons)
        for(int32_t floor=dungeon.floor_min;floor<dungeon.floor_min+dungeon.floor_count;++floor) {
          const auto key=DungeonFloorMessageKey(dungeon,floor);
          const auto found=labels.find(std::string(key));
          if(!key.empty() && found!=labels.end() && !found->second.empty())
            out->dungeon_floor_labels.insert_or_assign(found->first,found->second);
        }
    }
  }
  std::vector<uint8_t> location_data,rift_data;
  if(!ReadFile(read,"region_common/datasheets/ui/UiLocationInfo.gsheet",location_data) ||
      !ParseNativeLocations(location_data,out->locations) ||
      !LocalizeNativeLocations(read,native.message_root,out->locations))out->locations.clear();
  if(!ReadFile(read,"region_common/datasheets/CrackRegion.gsheet",rift_data) ||
      !ParseNativeRiftRegions(rift_data,out->rift_regions))out->rift_regions.clear();
  out->field_story = LoadFieldStoryQuery(read);
  if (out->field_story.valid) {
    const std::set<std::string> panes(out->field_story.pane_names.begin(),out->field_story.pane_names.end());
    if (panes!=std::set<std::string>{"L_Extend_00","L_Extend_01","L_Extend_02"}) out->field_story={};
  }
  progress("optional:field-map");
  FieldMapAssets field(read);
  FieldMapGeometry geometry;
  if (field.Inspect(geometry)) {
    out->player_ready=LoadFieldProjection(read,geometry,out->player);
    LoadNativeMapLabels(read,geometry,native.message_root,out->map_labels);
  }
  progress("optional:quest-marker-catalog");
  LoadMarkerQuestCatalog(read,out->quest_catalog);
  LoadNativeCheckpoints(read,"Field",out->checkpoints);
  progress("optional:landmark-catalog");
  LoadNativeLandmarks(read,out->landmarks);
  progress("optional:entrance-catalog");
  LoadNativeEntrances(read,out->entrances);
  std::vector<uint8_t> actors, aliases, names, descriptions, heading;
  const auto required_read=[&](std::string path,std::vector<uint8_t>& bytes) {
    progress("read:"+path);
    return ReadFile(read,path,bytes);
  };
  if(!required_read("region_common/datasheets/CopyableActor.gsheet",actors) ||
     !required_read("region_common/datasheets/ui/UiActorSelect.gsheet",aliases) ||
     !required_read(native.message_root+"/message/glossary/CopyableActor.msbt",names) ||
     !required_read(native.message_root+"/message/ui/DictionaryExplanation.msbt",descriptions))return {};
  progress("parse:CopyableActor");
  if(!ParseEchoMetadata(actors,out->echoes))return {};
  progress("parse:UiActorSelect");
  if(!AttachEchoUiMetadata(aliases,out->echoes))return {};
  progress("localize:EchoMetadata");
  if(!LocalizeEchoMetadata(out->echoes,names,descriptions))return {};
  {
    progress("optional:UiMachineSelect");
    std::vector<uint8_t> machines;
    if(!ReadFile(read,"region_common/datasheets/ui/UiMachineSelect.gsheet",machines) ||
       !ParseAutomatonMetadata(machines,out->automata) ||
       !LocalizeEchoMetadata(out->automata,names,descriptions))out->automata.clear();
  }
  for (std::string_view suffix :
       {"/message/glossary/Ui.msbt", "/message/ui/ActorSelect.msbt",
        "/message/ui/CopyDictionary.msbt"}) {
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string, std::string> labels;
    const auto path=native.message_root+std::string(suffix);
    if(!required_read(path,bytes))return {};
    progress("decode:"+path);
    if(!DecodeNativeMessages(bytes,labels))return {};
    // Native labels are file-local. For shared key names, later specialized UI
    // messages take precedence over glossary labels; no translated text is made
    // up.
    for (auto &[key, value] : labels)
      out->ui.insert_or_assign(std::move(key), std::move(value));
  }
  // Action labels are native messages without controller glyph substitutions.
  for (const auto& file : {"Operation", "DressUpMenu", "ItemMenu"}) {
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string,std::string> labels;
    const auto path=native.message_root+"/message/ui/"+file+".msbt";
    if(!required_read(path,bytes))return {};
    progress("decode:"+path);
    if(!DecodeNativeMessages(bytes,labels))return {};
    for(const auto key : {"Operation_Return","DressUpMenu_Dialog_Equipment","DressUpMenu_Dialog_Remove","ItemMenu_Dialog_Eat","ItemMenu_Dialog_Drink"}) {
      const auto found=labels.find(key);if(found!=labels.end())out->ui.insert_or_assign(found->first,found->second);
    }
  }
  const auto slash = native.message_root.find('/');
  progress("heading:locale-root");
  if(slash==std::string::npos)return {};
  if(!required_read(native.message_root.substr(0,slash)+"/common/ui/Heading.bffnt",heading))return {};
  progress("parse:Heading.bffnt");
  if(!ParseHeading(heading,out->heading))return {};
  progress("unicode:EchoMetadata");
  std::vector<uint32_t> cp = EnglishBodyCodepoints();
  for (const auto &e : out->echoes) {
    if (!Codepoints(e.name, cp) || !Codepoints(e.description, cp))
      return {};
    if (HeadingSupports(out->heading, e.name))
      out->heading_echo_keys.insert(e.crc_key);
  }
  progress("unicode:UI");
  for (const auto &[key, value] : out->ui) {
    if (!Codepoints(value, cp))
      return {};
    if (HeadingSupports(out->heading, value))
      out->heading_ui_keys.insert(key);
  }
  const auto echo_codepoints = cp;
  out->inventory_ready =
      LoadInventoryCatalog(read, read_memory, main_base, native.compatible,
                           native.message_root, out->inventory);
  if (out->inventory_ready) {
    for (size_t i = 0; i < out->inventory.entries.size(); ++i) {
      const auto &entry = out->inventory.entries[i];
      if (!Codepoints(entry.name, cp) || !Codepoints(entry.description, cp)) {
        out->inventory_ready = false;
        break;
      }
      if (HeadingSupports(out->heading, entry.name))
        out->heading_inventory_indices.insert(i);
    }
  }
  const auto withdraw_inventory = [&] {
    out->inventory_ready = false;
    out->inventory = {};
    out->heading_inventory_indices.clear();
    cp = echo_codepoints;
  };
  if (!out->inventory_ready)
    withdraw_inventory();
  std::sort(cp.begin(), cp.end());
  cp.erase(std::unique(cp.begin(), cp.end()), cp.end());
  progress("font:required-body-atlas");
  if (!BodyAtlas(native, cp, out->body)) {
    if (!out->inventory_ready)
      return {};
    // Optional inventory coverage must never withdraw otherwise valid Echo
    // resources. Retry only the independently required Echo/UI codepoints.
    withdraw_inventory();
    std::sort(cp.begin(), cp.end());
    cp.erase(std::unique(cp.begin(), cp.end()), cp.end());
    if (!BodyAtlas(native, cp, out->body))
      return {};
  }
  // Automaton coverage is optional and cannot withdraw the Echo picker.
  if(!out->automata.empty()) {
    auto automaton_cp=cp;bool valid=true;
    for(const auto& entry:out->automata)
      valid=Codepoints(entry.name,automaton_cp) && Codepoints(entry.description,automaton_cp) && valid;
    std::sort(automaton_cp.begin(),automaton_cp.end());
    automaton_cp.erase(std::unique(automaton_cp.begin(),automaton_cp.end()),automaton_cp.end());
    FontAtlas extended;
    if(valid && BodyAtlas(native,automaton_cp,extended)) {
      out->body=std::move(extended);cp=std::move(automaton_cp);
      for(const auto& entry:out->automata)if(HeadingSupports(out->heading,entry.name))
        out->heading_automaton_keys.insert(entry.crc_key);
    } else out->automata.clear();
  }
  // Optional map/prologue names cannot withdraw required Echo/UI coverage.
  // Native prologue descriptions use a different pictogram font; do not ask
  // the Standard TTF atlas to replace those native controller glyphs.
  auto optional_names=cp;
  bool optional_valid=true;
  for(const auto& entry:out->locations)
    optional_valid=Codepoints(entry.name,optional_names) && optional_valid;
  for(const auto& entry:out->map_labels)
    optional_valid=Codepoints(entry.name,optional_names) && optional_valid;
  for(const auto& entry:out->prologue)
    optional_valid=Codepoints(entry.name,optional_names) && optional_valid;
  for(const auto& [key,label]:out->dungeon_floor_labels)
    optional_valid=Codepoints(label,optional_names) && optional_valid;
  if(optional_valid) {
    std::sort(optional_names.begin(),optional_names.end());
    optional_names.erase(std::unique(optional_names.begin(),optional_names.end()),optional_names.end());
    FontAtlas extended;
    if(BodyAtlas(native,optional_names,extended))out->body=std::move(extended);
  }
  if (!out->prologue.empty()) {
    auto prologue_cp = EnglishBodyCodepoints();
    std::vector<std::string> display;
    bool valid = true;
    for (const auto &entry : out->prologue) {
      std::string description;
      if (!PrologueDisplayText(entry.description, description) ||
          !Codepoints(description, prologue_cp) ||
          !Codepoints(entry.name, prologue_cp)) {
        valid = false;
        break;
      }
      display.push_back(std::move(description));
    }
    if (valid) {
      std::sort(prologue_cp.begin(), prologue_cp.end());
      prologue_cp.erase(std::unique(prologue_cp.begin(), prologue_cp.end()), prologue_cp.end());
      if (PrologueBodyAtlas(native, read, prologue_cp, out->prologue_body))
        out->prologue_description_display = std::move(display);
    }
  }
  PrepareInspectionText(*out, native);
  if(!out->map_labels.empty()) {
    std::vector<uint32_t> label_cp;
    bool valid=true;
    for(const auto& label:out->map_labels)valid=Codepoints(label.name,label_cp) && valid;
    if(valid) {
      std::sort(label_cp.begin(),label_cp.end());
      label_cp.erase(std::unique(label_cp.begin(),label_cp.end()),label_cp.end());
      if(BodyAtlas(native,label_cp,out->map_body,22))for(const auto& label:out->map_labels) {
        acnh::Image text;
        if(RenderBody(out->map_body,label.name,{255,244,219,255},456,text) && text.h<=60)
          out->map_label_sizes.push_back({text.w+4,text.h+4});
        else out->map_label_sizes.push_back({0,0});
      }
    }
  }
  progress("ready");
  return out;
}
} // namespace
std::string InspectionDescription(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    if (text[at] != '\n' && text[at] != '\r') {
      result.push_back(text[at++]);
      continue;
    }
    size_t lines = 0;
    do {
      const char c = text[at++];
      if (c == '\r' && at < text.size() && text[at] == '\n')
        ++at;
      ++lines;
    } while (at < text.size() && (text[at] == '\n' || text[at] == '\r'));
    result += lines == 1 ? " " : "\n\n";
  }
  return result;
}
Resources::Resources(ResourceReadAssetFn read, ReadFn read_memory,
                     uint64_t main_base)
    : read_(std::move(read)), read_memory_(std::move(read_memory)),
      main_base_(main_base) {}
void Resources::SetNativeFonts(std::shared_ptr<const NativeFonts> fonts) {
  if (!Usable(fonts))
    fonts.reset();
  std::lock_guard lock(mutex_);
  if (fonts_ == fonts)
    return;
  fonts_ = std::move(fonts);
  ready_.reset();
  ++generation_;
}
std::shared_ptr<const ReadyResources> Resources::Snapshot() const {
  std::lock_guard lock(mutex_);
  return ready_;
}
std::string Resources::LastBuildPhase() const {
  std::lock_guard lock(mutex_);return build_phase_;
}
uint64_t Resources::BuildAttempts() const {
  std::lock_guard lock(mutex_);return build_attempts_;
}
bool Resources::EnsureReady() {
  std::lock_guard build_lock(build_mutex_);
  std::shared_ptr<const NativeFonts> fonts;
  uint64_t generation{};
  {
    std::lock_guard lock(mutex_);
    if (ready_)
      return true;
    ++build_attempts_;
    fonts = fonts_;
    generation = generation_;
  }
  if (!fonts) {
    std::lock_guard lock(mutex_);build_phase_="fonts-unavailable";return false;
  }
  const auto progress=[this](std::string phase){std::lock_guard lock(mutex_);build_phase_=std::move(phase);};
  auto built = Build(read_, read_memory_, main_base_, *fonts, progress);
  std::lock_guard lock(mutex_);
  if(generation_!=generation || fonts_!=fonts) {build_phase_="generation-changed";return false;}
  if(!built)return false;
  ready_ = std::move(built);
  return true;
}
} // namespace echoes

// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Legends Z-A (0100F43008C44000) dual-screen module.
//
// Verified build 2.0.2: read-only live party, box and coordinate readers, with save fallback.
// The Windows manifest can invoke a guarded, one-shot native box/party exchange.
// Bag/Pokedex/progression use the player's save; art and messages come from their romfs.
//
// Threading: configure()/sample()/tick() may run on different threads, so the save state sits
// behind one mutex and the file read happens outside it.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <limits>
#include <optional>
#include <set>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "za_guest_mailbox.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include "za_save.h"
#include "za_assets.h"
#include "za_live.h"
#include "za_travel.h"
#include "za_map.h"
#include "za_type_matchup.h"
#include "za_catalog.h"
#include "za_box_swap.h"
#include "za_native_box.h"
#include "za_move_menu.h"
#include "za_ui_catalog.h"
#include <memory>

namespace {

using namespace dsmod_sdk;
using namespace dsmod_sdk::int_types;

constexpr u64 kTitleId = 0x0100F43008C44000ull;
constexpr std::size_t kMaxSaveBytes = 16u << 20;
constexpr int kDexPageSize=12;
constexpr int kBagPageSize=8;
constexpr u64 kPollTicks = 300; // ~5 s at 60 fps

// The save data directory holds whichever name the title used; probe the plausible ones once and
// keep the one that answered.
constexpr const char* kSaveNames[] = {
    "main", "save", "save_data", "Main", "main.bin", "save.bin", "GameData.bin",
};

constexpr int kWideMapSize=840,kWideMapX=200,kWideMapY=94;
constexpr float kMapMaxZoom=12.f; // Match both map widgets in gen_manifest.py.
constexpr std::array<std::string_view,8> kNavPages{
    "team","dex","box","map","bag","missions","progress","notebook"};
constexpr std::array<const char*,9> kNavLabels{
    "Plus","Équipe","Pokédex","Boîte","Carte","Sac","Missions","Progression","Carnet"};
constexpr std::array<int,3> kDefaultShortcuts{1,4,5};
struct MapView {
    float zoom=1,x=0,y=0;
    bool wide=false;
    int Size() const { return wide?kWideMapSize:za::kMapViewSize; }
    float Scale() const { return float(Size())/za::kMapTextureSize; }
    int Left() const { return wide?kWideMapX:za::kMapViewX; }
    int Top() const { return wide?kWideMapY:za::kMapViewY; }
};

MapView ReadMapView(const EdenDsmodHostApi& host,float zoom,float cx,float cy,bool wide){
    const auto read=[&](const char* key,float fallback){
        const auto value=host.get_f64?host.get_f64(host.userdata,key,fallback):fallback;
        return std::isfinite(value)?float(value):fallback;
    };
    const float home_zoom=zoom;
    const MapView shape{1,0,0,wide};
    zoom=std::clamp(read(wide?"@view_zoom:lumiose_large":"@view_zoom:lumiose",home_zoom),1.f,kMapMaxZoom);
    const float limit=640-640/zoom;
    const auto pan = [&](const char* key, float center) {
        const float home = (640 * center - 320 / home_zoom) * shape.Scale();
        return std::clamp(read(key, home) / shape.Scale(), 0.f, limit);
    };
    return {zoom,pan(wide?"@view_pan_x:lumiose_large":"@view_pan_x:lumiose",cx),
                 pan(wide?"@view_pan_y:lumiose_large":"@view_pan_y:lumiose",cy),wide};
}

struct Instance {
    EdenDsmodHostApi host{};
    EdenDsmodHostSaveApi save_api{};
    za::Assets assets;
    za::LiveReader live_reader;
    za::LiveSnapshot live;
    std::unordered_map<std::uint64_t,za::Entity> confirmed_combat;
    int selected=0, detail=0, dex_open=0, quest_category=0;
    struct Objective {
        int species=0, form=0, category=-1;
        std::string mission;
        bool mega=false;
        bool operator==(const Objective&) const = default;
    };
    struct EncounterLocation {
        int zone=0;
        std::string dimension;
        float u=0,v=0;
    };
    struct MapPin {
        int type,index,key;
        float x,y;
        std::string binding,name,identity;
    };
    std::vector<std::vector<MapPin>> map_groups;
    std::vector<MapPin> map_choices;
    unsigned map_choices_revision=0;
    bool map_help=false;
    u64 map_sampled_reset=0;
    std::vector<Objective> objectives;
    std::array<s64,64> objective_keys{};
    std::array<std::optional<s64>,64> objective_writes{};
    std::vector<EncounterLocation> guide_locations;
    struct CaptureZone { EncounterLocation location; std::vector<int> species; };
    std::vector<CaptureZone> capture_zones;
    struct SearchResult {
        int generation=0,detail=0,icon=0;
        za::Save::BoxEntry box{};
        std::string name,context;
    };
    std::vector<SearchResult> search_results;
    std::vector<std::string> search_keys;
    std::string search_query;
    std::string search_error;
    int search_mode=0,search_keyboard=0;
    unsigned search_revision=0;
    int bag_guide_item=0;
    bool bag_return_evolution=false;
    enum class BagTool { None, Heal, Hold, Machine, Evolve };
    struct BagTarget {
        int party=-1,box=-1,slot=-1,evolution=0;
        za::Entity entity;
        std::string context;
        bool eligible=false;
    };
    BagTool bag_tool=BagTool::None;
    int bag_tool_item=0,bag_target_selected=-1;
    unsigned bag_tool_revision=0;
    std::vector<BagTarget> bag_targets;
    std::optional<BagTarget> bag_held_target;
    std::optional<za::Entity> team_heal_target;
    unsigned team_heal_revision=0;
    struct BagRequest { u32 sequence; BagTool tool; };
    std::optional<BagRequest> bag_request;
    std::string bag_status;
    std::array<int,4> bag_favorites{};
    std::array<std::optional<int>,4> bag_favorite_writes{};
    Objective guide_target;
    int guide_page=0;
    bool quest_show_map=false;
    std::string quest_selected_id;
    std::string send_status;
    int language_override=0; // 0 follows the save; optional module configuration override.
    za::TravelController travel;
    std::array<bool,5> map_commands{};
    za::BoxSwapController box_swap;
    int swap_slot=-1,swap_party=-1,swap_box=0;
    za::Entity swap_source;
    std::array<bool,9> swap_commands{};
    std::string swap_status;
    std::array<int,3> nav_shortcuts=kDefaultShortcuts;
    std::optional<int> nav_pending;
    bool nav_menu=false;
    int nav_edit_slot=-1,nav_destination=0;
    int box_detail_open=0;za::Entity box_detail_entity;
    int box_detail_slot=-1,box_detail_box=-1;
    za::NativeBoxExchange native_box;
    std::string native_box_status;
    za::MoveMenu move_menu;
    za::GuestMailbox guest_mailbox;
    bool guest_helper_alive = false;
    int map_center=-1,map_cafe=-1;
    int map_filters=511;
    int map_landmark=-1;
    bool placing_marker=false;
    struct MarkerRequest {
        float x,y;
        u64 deadline;
        u64 close_deadline=0,close_not_before=0;
        int close_attempts=0;
        bool clear=false;
        bool sent=false;
    };
    std::optional<MarkerRequest> marker_request;
    const char* marker_result="";
    u64 marker_notice_until=0;
    bool marker_open_pulse=false,map_close_pulse=false;
    std::string map_dimension;
    int map_quest=0;
    std::array<int, 128> map_quest_targets{};
    bool research_open=false;int research_selected=0;
    float map_home_zoom=1,map_home_x=.5f,map_home_y=.5f;
    u64 map_reset=0;
    MapView map_view{1,0,0};
    float heading_x=0,heading_y=0,heading=0;
    bool heading_origin=false,heading_valid=false;
    bool map_filter_menu=false;
    int send_target=-1;
    int attack_slot=-1,attack_move=-1,attack_id=0,attack_pulse=-1;
    enum class AttackPhase { Select, Deploy, Aim };
    AttackPhase attack_phase=AttackPhase::Select;
    u64 attack_identity=0,attack_due=0,attack_busy_until=0;
    u64 attack_deadline=0,attack_next_select=0;
    bool aim_pulse=false;
    std::uint64_t command_tick=0,command_next=0,command_deadline=0;
    bool command_left=false,command_right=false,command_send=false;
    std::shared_ptr<const std::vector<std::string>> species_names;
    std::shared_ptr<const std::vector<std::string>> item_names;
    std::shared_ptr<const std::vector<bool>> available_species;
    int dex_gen = 1, dex_filter = 0, dex_page = 0;
    int bag_category = -1, bag_page = 0, bag_selected=0, map_zone=0;
    bool map_missing_only=false;

    std::mutex mutex;
    std::mutex poll_mutex;
    za::Save save;
    std::string save_name;
    std::string status = "recherche de la sauvegarde";
    u64 next_poll = 0;
    bool polled_once = false;
    // Box browsing: the page shows one box at a time. Until the player browses, the view follows
    // the box the game itself has selected; after that it stays where the player put it.
    int box_index = 0;
    bool box_follow = true;
    bool box_picker = false;
    // Cheap change detection: the container's SHA256 trailer changes on every save, so hashing the
    // last 0x20 bytes tells us whether re-decrypting the whole file is worth it.
    u64 last_tail = 0;
    bool have_tail = false;
    std::size_t last_size = 0;
};

u64 TailHash(const u8* tail, std::size_t tail_size, std::size_t file_size) {
    // FNV-1a over the trailer and the size; enough to notice a rewritten save.
    u64 hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < tail_size; ++i) {
        hash ^= tail[i];
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<u64>(file_size);
    return hash * 1099511628211ull;
}

void Log(const EdenDsmodHostApi& host, u32 level, const std::string& message) {
    if (host.log)
        host.log(host.userdata, level, message.c_str());
}

// The save's size, or 0 when the name does not answer (or answers with something that cannot be
// a save). One stat-like call, no payload.
std::size_t SaveFileSize(const Instance& inst, const char* name) {
    if (!inst.save_api.read_save_file)
        return 0;
    const std::size_t size =
        inst.save_api.read_save_file(inst.save_api.userdata, name, 0, nullptr, 0);
    return (size < 0x1000 || size > kMaxSaveBytes) ? 0 : size;
}

// The container's last 0x20 bytes: the SHA256 the game writes over the whole file.
bool ReadSaveTail(const Instance& inst, const char* name, std::size_t size, u8* out) {
    if (!inst.save_api.read_save_file || size < 0x20)
        return false;
    return inst.save_api.read_save_file(inst.save_api.userdata, name, size - 0x20, out, 0x20) ==
           0x20;
}

bool ReadSaveBytes(const Instance& inst, const char* name, std::size_t size,
                   std::vector<u8>& out) {
    out.assign(size, 0);
    const std::size_t read =
        inst.save_api.read_save_file(inst.save_api.userdata, name, 0, out.data(), size);
    if (read != size)
        out.clear();
    return read == size;
}

void Poll(Instance& inst, const EdenDsmodHostApi& host, bool force) {
    std::scoped_lock poll_lock{inst.poll_mutex};
    bool follow;
    int box;
    bool save_valid;
    {
        std::scoped_lock lock{inst.mutex};
        follow = inst.box_follow; box = inst.box_index; save_valid = inst.save.valid;
    }
    const u64 now = host.get_tick ? host.get_tick(host.userdata) : 0;
    if (!force && inst.polled_once && now < inst.next_poll)
        return;
    inst.polled_once = true;
    inst.next_poll = now + kPollTicks;
    if (!inst.save_api.read_save_file)
        return;

    // Which file? The one that answered last time first, then the candidate list.
    std::string name;
    std::size_t size = 0;
    if (!inst.save_name.empty()) {
        size = SaveFileSize(inst, inst.save_name.c_str());
        if (size != 0)
            name = inst.save_name;
    }
    if (name.empty()) {
        for (const char* candidate : kSaveNames) {
            size = SaveFileSize(inst, candidate);
            if (size != 0) {
                name = candidate;
                break;
            }
        }
    }

    // Change detection without reading the payload: the container ends with the SHA256 of its own
    // contents, so the last 0x20 bytes plus the size say whether a save happened. An unchanged save
    // therefore costs two small reads instead of 3 MiB on the timing thread.
    u8 tail_bytes[0x20] = {};
    const bool probed = !name.empty() && ReadSaveTail(inst, name.c_str(), size, tail_bytes);
    const u64 tail = probed ? TailHash(tail_bytes, sizeof(tail_bytes), size) : 0;
    if (!force && probed && inst.have_tail && save_valid && size == inst.last_size &&
        tail == inst.last_tail) {
        std::scoped_lock lock{inst.mutex};
        inst.save_name = name;
        return;
    }

    std::vector<u8> bytes;
    const bool have_bytes = !name.empty() && ReadSaveBytes(inst, name.c_str(), size, bytes);

    za::Save parsed;
    std::string status;
    if (!have_bytes) {
        status = "sauvegarde introuvable";
        parsed.status = status;
    } else {
        parsed = za::Parse(bytes, follow ? -1 : box);
        if (!parsed.valid) {
            status = parsed.status;
        } else if (!parsed.hash_ok) {
            status = "somme de controle invalide";
        } else {
            status = "sauvegarde a jour";
        }
    }

    const bool locale_changed=parsed.valid&&inst.assets.SetLanguage(inst.language_override?inst.language_override:parsed.trainer.language);
    std::scoped_lock lock{inst.mutex};
    if(locale_changed){inst.species_names.reset();inst.item_names.reset();}
    const bool changed = status != inst.status || parsed.valid != inst.save.valid;
    if (parsed.valid)
        inst.box_index = parsed.box_view_index;
    inst.save = std::move(parsed);
    inst.save_name = name;
    inst.status = status;
    inst.have_tail = probed;
    inst.last_tail = tail;
    inst.last_size = size;
    if (changed && host.log) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "ZA save: %s (%s, %zu octets, %zu blocs, equipe %d, dex %d/%d)",
                      inst.status.c_str(), name.empty() ? "-" : name.c_str(), inst.save.file_size,
                      inst.save.block_count, inst.save.party_count, inst.save.dex_caught,
                      inst.save.dex_seen);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
    }
}

bool DexCaught(const za::Save& save,int species) {
    return species>0&&species<save.dex_state.size()&&(save.dex_state[species]==2 || save.owned[species]>0);
}

std::string SearchFold(std::string_view value) {
    std::string folded;
    constexpr std::string_view accents="àâäáãåèéêëìíîïòóôöõùúûüçñýÿ";
    constexpr std::string_view replacements="aaaaaaeeeeiiiiooooouuuucnyy";
    for(std::size_t i=0;i<value.size();) {
        const auto byte=static_cast<unsigned char>(value[i]);
        if(byte<128) {
            folded+=byte>='A'&&byte<='Z'?char(byte+32):char(byte);
            ++i;
            continue;
        }
        const auto start=i++;
        while(i<value.size()&&(static_cast<unsigned char>(value[i])&0xC0)==0x80) ++i;
        auto glyph=value.substr(start,i-start);
        std::string lower(glyph);
        if(glyph.size()==2&&byte==0xC3&&static_cast<unsigned char>(glyph[1])>=0x80&&
           static_cast<unsigned char>(glyph[1])<=0x9E&&glyph[1]!='\x97') lower[1]+=32;
        if(const auto index=accents.find(lower);lower.size()==2&&index!=std::string_view::npos&&index%2==0)
            folded+=replacements[index/2];
        else if(lower=="œ"||lower=="Œ") folded+="oe";
        else folded+=lower;
    }
    return folded;
}

void AddSearchKey(std::set<std::string>& keys,std::string_view remainder) {
    if(remainder.empty()) return;
    std::size_t end=1;
    while(end<remainder.size()&&(static_cast<unsigned char>(remainder[end])&0xC0)==0x80) ++end;
    if(static_cast<unsigned char>(remainder.front())>=128) keys.emplace(remainder.substr(0,end));
}

void OpenGuideMap(Instance& inst,const Instance::EncounterLocation& location) {
    constexpr float zoom=2.4f,half=.5f/zoom;
    inst.map_home_zoom=zoom;
    inst.map_home_x=std::clamp(location.u/640,half,1-half);
    inst.map_home_y=std::clamp(location.v/640,half,1-half);
    ++inst.map_reset;
    inst.map_quest=0;
    inst.map_zone=location.zone;
    inst.map_dimension=location.dimension;
    inst.map_filters|=location.zone?1:16;
    inst.map_center=inst.map_cafe=inst.map_landmark=-1;
    inst.map_filter_menu=false;
    inst.map_help=false;
    inst.map_choices.clear();
    inst.guest_mailbox.ClearTravelStatus();
    inst.quest_show_map=true;
}

void CloseMapPanel(Instance& inst) {
    inst.map_zone=inst.map_quest=0;
    inst.map_center=inst.map_cafe=inst.map_landmark=-1;
    inst.map_dimension.clear();
    inst.map_choices.clear();
    inst.map_filter_menu=inst.map_help=inst.placing_marker=false;
}

std::vector<int> DexRows(za::Assets& assets,const za::Save& save, int gen, int filter) {
    std::vector<int> rows;
    for (const auto& entry:assets.Pokedex(gen-1)) {
        const int sp=za::NationalSpecies(entry.species);
        const bool caught = DexCaught(save,sp%2048);
        const bool seen = save.dex_state[sp%2048] > 0 || caught;
        if ((filter == 1 && !caught) || (filter == 2 && caught) ||
            (filter == 3 && (!seen || caught)) || (filter == 4 && !save.owned[sp%2048])) continue;
        rows.push_back(sp+(gen==3?entry.form*2048:0));
    }
    return rows;
}

// Scroll values are published after module sampling on runtime 19. Publish the
// active collection in full so offscreen rows are present when a fast fling lands;
// the renderer clips images and repeated widgets to the actual viewport.
std::pair<int,int> VisibleRows(const EdenDsmodHostApi& host,const char* region,int count){
    const char* page=host.get_text?host.get_text(host.userdata,"__page"):nullptr;
    const char* expected=std::strcmp(region,"za_dex")==0?"dex":std::strcmp(region,"za_bag")==0?"bag":std::strcmp(region,"za_quests")==0?"missions":std::strcmp(region,"za_moves")==0?"moves":std::strcmp(region,"za_research")==0?"progress":"map";
    return {0,!page||std::strcmp(page,expected)==0?count:0};
}
int TMMove(int item){for(const auto& tm:za::kTMs)if(tm.item==item)return tm.move;return 0;}

std::vector<za::Save::Item> BagRows(const za::Save& save, int category) {
    std::vector<za::Save::Item> rows;
    for (const auto& item : save.items)
        if (category < 0 || item.pouch == static_cast<unsigned>(category)) rows.push_back(item);
    return rows;
}

int InternalId(int national) {
    static const auto ids = [] {
        std::array<int, 1026> table{};
        for (int i = 1; i <= 1025; ++i) {
            const auto n = za::NationalSpecies(static_cast<za::u16>(i));
            if (n <= 1025) table[n] = i;
        }
        return table;
    }();
    return national >= 1 && national <= 1025 ? ids[national] : 0;
}

std::uint64_t QuestHash(const std::string& value){std::uint64_t hash=14695981039346656837ull;for(unsigned char c:value){hash^=c;hash*=1099511628211ull;}return hash;}
int MissionState(const za::Assets::Mission& mission,const za::Save& save){
    auto pos=mission.id.find("_title_");if(pos==std::string::npos)return 0;
    auto suffix=mission.id.substr(pos+7);std::string work=mission.work;
    if(!work.empty()){}
    else if(mission.id.starts_with("msg_subquest_")){int id=0;char extra=0;if(std::sscanf(suffix.c_str(),"%d%c",&id,&extra)!=1)return 0;work="SYS_WORK_QUEST_"+std::to_string(id);}
    else if(mission.id.starts_with("msg_mainquest_")){int chapter=0,part=0;char extra=0;if(std::sscanf(suffix.c_str(),"%d_%d%c",&chapter,&part,&extra)!=2)return 0;char key[80];std::snprintf(key,sizeof(key),"SYS_WORK_MAINQUEST_%02d_%02d",chapter,part);work=key;}
    else return 0;
    auto value=save.quest_work.find(QuestHash(work));if(value==save.quest_work.end())return 0;
    if(value->second==255)return 3;if(value->second>=10&&value->second<255)return 2;if(value->second==1)return 1;return 4;
}
std::vector<za::Assets::Mission> MissionRows(Instance& inst,int category,const za::Save& save){
    if(category==3){
        std::vector<za::Assets::Mission> active;
        for(int group=0;group<3;++group)
            for(auto& mission:inst.assets.Missions(group))
                if(MissionState(mission,save)==2) active.push_back(std::move(mission));
        return active;
    }
    auto rows=inst.assets.Missions(category);
    std::erase_if(rows,[&](const auto& mission){const auto state=MissionState(mission,save);return state!=1&&state!=2&&state!=3;});
    auto rank=[&](const auto& m){switch(MissionState(m,save)){case 2:return 0;case 1:return 1;case 3:return 4;default:return 2;}};
    std::stable_sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});return rows;
}

// ---- publishing -------------------------------------------------------------------------------

void PublishText(const EdenDsmodHostApi& host, const char* name, const std::string& value) {
    if (host.publish_text)
        host.publish_text(host.userdata, name, value.c_str());
}

void PublishI64(const EdenDsmodHostApi& host, const char* name, s64 value) {
    if (host.publish_i64)
        host.publish_i64(host.userdata, name, value);
}

// Keep the stored values exact even on hosts that expose flags through numeric bindings.
constexpr s64 MissionKeyBase=1LL<<50;
constexpr s64 MegaKeyBit=1LL<<20;

s64 ObjectiveKey(const Instance::Objective& objective) {
    if(objective.category<0) return objective.species*256+objective.form+(objective.mega?MegaKeyBit:0);
    return (objective.category+1)*MissionKeyBase+(QuestHash(objective.mission)&(MissionKeyBase-1));
}

void SyncObjectives(Instance& inst,const EdenDsmodHostApi& host) {
    auto keys=inst.objective_keys;
    for(std::size_t i=0;i<keys.size();++i) {
        static const auto suffixes=[] {
            std::array<std::string,64> result;
            for(std::size_t j=0;j<result.size();++j) result[j]=std::to_string(j);
            return result;
        }();
        const auto& suffix=suffixes[i];
        const auto flag="@flag:za_notebook_"+suffix;
        const auto stored=host.get_i64?host.get_i64(host.userdata,flag.c_str(),0):0;
        auto& pending=inst.objective_writes[i];
        if(pending==stored) pending.reset();
        keys[i]=pending.value_or(stored);
        PublishI64(host,("za.note.pending"+suffix).c_str(),pending.has_value());
        if(pending) PublishI64(host,("za.note.value"+suffix).c_str(),*pending);
    }
    const auto count=std::ranges::count_if(keys,[](s64 key){return key>0;});
    if(keys==inst.objective_keys&&count==inst.objectives.size()) return;
    inst.objective_keys=keys;
    inst.objectives.clear();
    std::array<std::optional<std::vector<za::Assets::Mission>>,3> missions;
    for(const auto key:keys) {
        if(key<=0) continue;
        const auto category=key/MissionKeyBase-1;
        if(category<0) {
            const auto species=(key&~MegaKeyBit)/256;
            if(species>0&&species<=1025)
                inst.objectives.push_back({int(species),int(key%256),-1,{},bool(key&MegaKeyBit)});
        } else if(category<missions.size()) {
            auto& rows=missions[category];
            if(!rows) rows=inst.assets.Missions(category);
            const auto found=std::ranges::find_if(*rows,[&](const auto& mission) {
                return ObjectiveKey({0,0,int(category),mission.id})==key;
            });
            if(found!=rows->end()) inst.objectives.push_back({0,0,int(category),found->id});
        }
    }
}

bool WriteObjective(Instance& inst,const Instance::Objective& objective,bool remove_only=false) {
    const auto key=ObjectiveKey(objective);
    auto current=inst.objective_keys;
    for(std::size_t i=0;i<current.size();++i)
        current[i]=inst.objective_writes[i].value_or(current[i]);
    auto found=std::ranges::find(current,key);
    if(found!=current.end()) inst.objective_writes[found-current.begin()]=0;
    else if(!remove_only&&(found=std::ranges::find(current,0))!=current.end())
        inst.objective_writes[found-current.begin()]=key;
    else return false;
    return true;
}

void PublishF64(const EdenDsmodHostApi& host, const char* name, double value) {
    if (host.publish_f64)
        host.publish_f64(host.userdata, name, value);
}

std::string CanonicalUi(const std::string& text){
    std::string result;for(const auto c:text){if(c==' '||c=='\n'||c=='\r'||c=='\t'){if(!result.empty()&&result.back()!=' ')result+=' ';}else result+=c;}
    if(!result.empty()&&result.back()==' ')result.pop_back();return result;
}

std::string UiText(za::Assets& assets,int language,const std::string& original,int* missing=nullptr){
    const auto source=CanonicalUi(original);if(source.empty())return {};
    if(language<1||language>11||language==6)language=2;
    if(language==3)return source;
    for(const auto& phrase:za::ui::phrases)if(source==phrase.source)return phrase.text[language];
    auto translated=assets.UiText(source);if(!translated.empty())return translated;
    if(missing)++*missing;return source;
}

std::string EvolutionCondition(za::Assets& assets,int language,const za::Assets::Evolution& evo) {
    const auto tr=[&](const char* text){return UiText(assets,language,text);};
    const auto with_argument=[&](const char* caption,const std::string& argument) {
        return argument.empty()||argument=="-"?tr("Condition spéciale"):tr(caption)+" · "+argument;
    };
    const auto level=evo.level>0?tr("Niveau")+" "+std::to_string(evo.level):tr("Montée de niveau");
    const auto item=[&]{return evo.item>0?assets.ItemName(evo.item):std::string{};};
    switch(evo.method) {
    case 1: return tr("Montée de niveau avec forte amitié");
    case 2: return tr("Montée de niveau de jour avec forte amitié");
    case 3: return tr("Montée de niveau de nuit avec forte amitié");
    case 4: return level;
    case 5: return tr("Échange");
    case 6: return with_argument("Échange avec objet tenu",item());
    case 8: return with_argument("Utiliser",item());
    case 9: return level+" · "+tr("Attaque > Défense");
    case 10: return level+" · "+tr("Attaque = Défense");
    case 11: return level+" · "+tr("Attaque < Défense");
    case 17: return with_argument("Utiliser sur un mâle",item());
    case 18: return with_argument("Utiliser sur une femelle",item());
    case 19: return with_argument("Montée de niveau de jour avec objet tenu",item());
    case 20: return with_argument("Montée de niveau de nuit avec objet tenu",item());
    case 21: return with_argument("Montée de niveau en connaissant",assets.MoveName(evo.argument));
    case 23: return level+" · ♂";
    case 24: return level+" · ♀";
    case 28: return level+" · "+tr("Console à l’envers");
    case 29: return with_argument("Forte amitié et capacité de type",assets.TypeName(evo.argument));
    case 32: return level+" · "+tr("De jour");
    case 33: return level+" · "+tr("De nuit");
    default: return tr("Condition spéciale");
    }
}

unsigned ItemCount(const za::Save& save,int id) {
    unsigned count=0;
    for(const auto& item:save.items) if(item.id==id) count+=item.count;
    return count;
}

void SyncBagFavorites(Instance& inst,const EdenDsmodHostApi& host) {
    for(std::size_t i=0;i<inst.bag_favorites.size();++i){
        const auto suffix=std::to_string(i);
        const auto flag="@flag:za_bag_favorite_"+suffix;
        const auto stored=host.get_i64?host.get_i64(host.userdata,flag.c_str(),0):0;
        const int id=stored>0&&stored<3000?int(stored):0;
        auto& write=inst.bag_favorite_writes[i];
        if(write==id) write.reset();
        inst.bag_favorites[i]=write.value_or(id);
    }
}

void PublishNavigation(Instance& inst,const EdenDsmodHostApi& host,
                       std::string_view page,int language) {
    const auto stored=host.get_i64?host.get_i64(host.userdata,"@flag:za_nav_shortcuts",0):0;
    if(inst.nav_pending==stored) inst.nav_pending.reset();
    if(!inst.nav_pending&&stored>=111&&stored<=888){
        const std::array<int,3> decoded{int(stored/100),int(stored/10%10),int(stored%10)};
        if(std::ranges::all_of(decoded,[](int id){return id>=1&&id<=int(kNavPages.size());})&&
           decoded[0]!=decoded[1]&&decoded[0]!=decoded[2]&&decoded[1]!=decoded[2])
            inst.nav_shortcuts=decoded;
    }

    if(inst.nav_destination&&page==kNavPages[inst.nav_destination-1]) inst.nav_destination=0;
    const auto top=std::ranges::find(kNavPages,page);
    int active=top==kNavPages.end()?0:int(top-kNavPages.begin())+1;
    if(page=="box_detail"||page=="box_exchange") active=3;
    else if(page=="team_heal"||(page=="bag_tools"&&inst.team_heal_target)) active=1;
    else if(page=="bag_tools") active=5;
    else if(page=="capture") active=8;
    else if(page=="dex_detail"||page=="locations"||page=="combat"||page=="evolutions"||page=="search") active=2;
    else if(page=="moves") active=inst.move_menu.from_bag?5:inst.move_menu.from_box?3:1;
    PublishI64(host,"za.nav.destination",inst.nav_destination);
    PublishI64(host,"za.nav.menu",inst.nav_menu);
    PublishI64(host,"za.nav.editing",inst.nav_edit_slot>=0);
    PublishI64(host,"za.nav.edit_slot",inst.nav_edit_slot+1);
    PublishI64(host,"za.nav.active",active);
    PublishI64(host,"za.nav.pending",inst.nav_pending.has_value());
    PublishI64(host,"za.nav.encoded",inst.nav_pending.value_or(0));
    const bool secondary=active&&std::ranges::find(inst.nav_shortcuts,active)==inst.nav_shortcuts.end();
    const int more=secondary?active:0;
    PublishI64(host,"za.nav.more.id",more);
    PublishI64(host,"za.nav.more.active",secondary||inst.nav_menu);
    PublishText(host,"za.nav.more.name",UiText(inst.assets,language,kNavLabels[more]));
    for(std::size_t i=0;i<inst.nav_shortcuts.size();++i){
        const auto prefix="za.nav.slot"+std::to_string(i)+".";
        const int id=inst.nav_shortcuts[i];
        PublishI64(host,(prefix+"id").c_str(),id);
        PublishI64(host,(prefix+"active").c_str(),id==active);
        PublishText(host,(prefix+"name").c_str(),UiText(inst.assets,language,kNavLabels[id]));
    }
}

void OpenBagTool(Instance& inst,Instance::BagTool tool,int item,int target=-1,bool from_team_heal=false) {
    inst.bag_tool=tool;
    inst.bag_tool_item=item;
    inst.bag_target_selected=target;
    inst.bag_targets.clear();
    if(!inst.bag_request) inst.bag_status.clear();
    if(tool!=Instance::BagTool::Hold) inst.bag_held_target.reset();
    if(!from_team_heal) inst.team_heal_target.reset();
    ++inst.bag_tool_revision;
    inst.guide_page=9;
}

bool CanUseMedicine(const za::Assets::ItemUsage& usage,const za::Entity& entity) {
    return entity.present&&!entity.egg&&entity.hp_max&&
           (usage.HpAfterMedicine(entity.hp,entity.hp_max)>entity.hp||
            (entity.hp&&usage.Cures(entity.status_condition)));
}

const char* PartyStatusLabel(const za::Entity& entity) {
    if(!entity.hp) return "K.O.";
    switch(static_cast<za::PartyStatus>(entity.status_condition)){
    case za::PartyStatus::Paralysis: return "Paralysie";
    case za::PartyStatus::Sleep: return "Sommeil";
    case za::PartyStatus::Frozen: return "Gel";
    case za::PartyStatus::Burn: return "Brûlure";
    case za::PartyStatus::Poison: return "Empoisonnement";
    default: return "";
    }
}

void PublishMoveStats(const EdenDsmodHostApi& host,za::Assets& assets,int language,
                      const std::string& prefix,int move) {
    const auto info=assets.MoveParameters(move);
    constexpr const char* categories[]={"Statut","Physique","Spéciale"};
    PublishText(host,(prefix+"category").c_str(),info.category>=0&&info.category<3
        ?UiText(assets,language,categories[info.category]):"—");
    PublishText(host,(prefix+"power").c_str(),info.power>1?std::to_string(info.power):"—");
    PublishText(host,(prefix+"accuracy").c_str(),info.accuracy>0&&info.accuracy<=100
        ?std::to_string(info.accuracy)+" %":"—");
}

void PublishTeamHealing(Instance& inst,const EdenDsmodHostApi& host,const za::Save& save) {
    std::optional<za::Entity> target;
    unsigned revision;
    {
        std::scoped_lock lock{inst.mutex};
        target=inst.team_heal_target;
        revision=inst.team_heal_revision;
    }
    const auto member=std::ranges::find_if(save.party,[&](const auto& entry){
        return target&&za::NativeBoxExchange::Same(*target,entry);
    });
    const bool found=member!=std::end(save.party);
    const int language=save.trainer.language;
    PublishI64(host,"za.heal.target",found);
    PublishI64(host,"za.heal.reset",revision);
    std::vector<const za::Save::Item*> rows;
    if(found){
        PublishText(host,"za.heal.name",member->nickname.empty()?std::string(inst.assets.SpeciesName(member->internal_species)):member->nickname);
        PublishText(host,"za.heal.status",UiText(inst.assets,language,PartyStatusLabel(*member)));
        PublishI64(host,"za.heal.icon",member->internal_species*256+member->form+(member->shiny?524288:0));
        PublishI64(host,"za.heal.hp",member->hp);
        PublishI64(host,"za.heal.maxhp",member->hp_max);
        for(const auto& item:save.items)
            if(item.count&&CanUseMedicine(inst.assets.ItemUsageInfo(item.id),*member)) rows.push_back(&item);
    }
    PublishI64(host,"za.heal.rows",rows.size());
    PublishI64(host,"za.heal.empty",found&&rows.empty());
    for(std::size_t i=0;i<rows.size();++i){
        const auto& item=*rows[i];
        const auto key="za.hm"+std::to_string(i)+".";
        PublishI64(host,(key+"id").c_str(),item.id);
        PublishI64(host,(key+"stock").c_str(),item.count);
        PublishText(host,(key+"name").c_str(),inst.assets.ItemName(item.id));
        PublishText(host,(key+"description").c_str(),inst.assets.ItemDescription(item.id));
    }
}

int SelectedBagItem(const Instance& inst) {
    if(inst.bag_guide_item) return inst.bag_guide_item;
    const auto rows=BagRows(inst.save,inst.bag_category);
    const auto found=std::ranges::find(rows,inst.bag_selected,&za::Save::Item::id);
    return found!=rows.end()?found->id:rows.empty()?0:rows.front().id;
}

bool MatchesEvolution(const za::Assets::Evolution& row,const za::Entity& entity) {
    return entity.present&&!entity.egg&&row.source==entity.internal_species&&row.source_form==entity.form&&
           (row.method!=17||entity.gender==0)&&(row.method!=18||entity.gender==1);
}

bool MachineCompatible(za::Assets& assets,int move,const za::Entity& entity) {
    if(!move||!entity.present||entity.egg) return false;
    const auto moves=assets.MachineMoves(entity.internal_species,entity.form);
    return std::ranges::find(moves,move)!=moves.end();
}

void PublishBagTools(Instance& inst,const EdenDsmodHostApi& host,const za::Save& save,
                     const za::LiveSnapshot& live,bool nav_menu) {
    using Tool=Instance::BagTool;
    Tool tool;
    int item,selected;
    unsigned revision;
    std::optional<Instance::BagTarget> held_target;
    std::optional<za::Entity> heal_target;
    bool native_available,bag_pending,mailbox_idle;
    std::string bag_status;
    {
        std::scoped_lock lock{inst.mutex};
        tool=inst.bag_tool;
        item=inst.bag_tool_item;
        selected=inst.bag_target_selected;
        revision=inst.bag_tool_revision;
        held_target=inst.bag_held_target;
        heal_target=inst.team_heal_target;
        native_available=inst.guest_helper_alive;
        bag_pending=inst.bag_request.has_value();
        mailbox_idle=native_available&&inst.guest_mailbox.Idle();
        bag_status=inst.bag_status;
    }
    const int language=save.trainer.language,move=tool==Tool::Machine?TMMove(item):0;
    const auto usage=inst.assets.ItemUsageInfo(item);
    const auto evolutions=tool==Tool::Evolve?inst.assets.EvolutionSourcesForItem(item):std::vector<za::Assets::Evolution>{};
    const auto name=[&](const za::Entity& entity){
        return entity.nickname.empty()?std::string(inst.assets.SpeciesName(entity.internal_species)):entity.nickname;
    };
    std::vector<Instance::BagTarget> rows;
    const auto add=[&](const za::Entity& entity,int party,int box,int cell){
        if(!entity.present||entity.egg) return;
        auto context=party>=0?UiText(inst.assets,language,"ÉQUIPE")+" "+std::to_string(party+1):
            UiText(inst.assets,language,"BOÎTE")+" "+std::to_string(box+1)+" · "+UiText(inst.assets,language,"Case")+" "+std::to_string(cell+1);
        bool eligible=false;
        int evolution=0;
        switch(tool){
        case Tool::Heal: eligible=CanUseMedicine(usage,entity);break;
        case Tool::Hold: eligible=item==0?entity.held_item>0:usage.holdable;break;
        case Tool::Machine: eligible=MachineCompatible(inst.assets,move,entity);break;
        case Tool::Evolve:
            for(const auto& row:evolutions) if(MatchesEvolution(row,entity)) {
                if(evolution==0) evolution=row.target;
                else if(evolution!=row.target) evolution=-1;
            }
            if(!evolution) return;
            eligible=true;
            break;
        default: return;
        }
        rows.push_back({party,box,cell,evolution,entity,std::move(context),eligible});
    };
    if(tool==Tool::Heal&&heal_target){
        for(int i=0;i<6;++i)
            if(za::NativeBoxExchange::Same(*heal_target,save.party[i])) add(save.party[i],i,-1,-1);
    }else if(tool==Tool::Hold&&held_target){
        const auto& ref=*held_target;
        if(ref.party>=0){
            for(int i=0;i<6;++i)
                if(za::NativeBoxExchange::Same(ref.entity,save.party[i])) add(save.party[i],i,-1,-1);
        }else if(live.box_valid&&live.box_live&&live.box_view_index==ref.box&&ref.slot>=0&&ref.slot<30){
            const auto& entity=live.box_view[ref.slot];
            if(za::NativeBoxExchange::Same(ref.entity,entity)) add(entity,-1,ref.box,ref.slot);
        }else{
            add(ref.entity,-1,ref.box,ref.slot);
        }
    }else for(int i=0;i<6;++i) add(save.party[i],i,-1,-1);
    if(tool==Tool::Evolve){
        const bool live_box=live.box_valid&&live.box_live&&live.box_view_index>=0;
        if(save.box_entries) for(const auto& entry:*save.box_entries){
            if(live_box&&entry.box==live.box_view_index) continue;
            za::Entity entity;
            entity.present=true;entity.egg=entry.egg;entity.gender=entry.gender;
            entity.internal_species=entry.species;entity.form=entry.form;
            entity.pid=entry.pid;entity.encryption_constant=entry.ec;entity.nickname=entry.nickname;
            add(entity,-1,entry.box,entry.slot);
        }
        if(live_box) for(int i=0;i<30;++i) add(live.box_view[i],-1,live.box_view_index,i);
    }
    PublishI64(host,"za.bagtool.mode",static_cast<int>(tool));
    PublishI64(host,"za.bagtool.rows",rows.size());
    const bool any_eligible=std::ranges::any_of(rows,[](const auto& row){return row.eligible;});
    PublishI64(host,"za.bagtool.empty",rows.empty()||(tool==Tool::Machine&&!any_eligible));
    PublishI64(host,"za.bagtool.revision",revision);
    PublishI64(host,"za.bagtool.item",item);
    PublishI64(host,"za.bagtool.stock",ItemCount(save,item));
    PublishText(host,"za.bagtool.name",inst.assets.ItemName(item));
    PublishI64(host,"za.bagtool.heal",tool==Tool::Heal);
    PublishI64(host,"za.bagtool.from_team",tool==Tool::Heal&&heal_target.has_value());
    PublishI64(host,"za.bagtool.hold",tool==Tool::Hold);
    PublishI64(host,"za.bagtool.tm",tool==Tool::Machine);
    PublishI64(host,"za.bagtool.evolve",tool==Tool::Evolve);
    PublishI64(host,"za.bagtool.describe",tool!=Tool::Hold);
    PublishI64(host,"za.bagtool.describe.scroll",tool!=Tool::Hold&&!nav_menu);
    PublishI64(host,"za.bagtool.heal_empty",tool==Tool::Heal&&!rows.empty()&&!any_eligible);
    PublishI64(host,"za.bagtool.hp_live",tool==Tool::Heal&&std::ranges::any_of(live.hp_valid,[](bool v){return v;}));
    const auto detail=move?inst.assets.MoveDescription(move):inst.assets.ItemDescription(item);
    PublishText(host,"za.bagtool.description",detail);
    PublishI64(host,"za.bagtool.lines",std::max(1,int(detail.size()/38)+2));
    const auto move_info=inst.assets.MoveParameters(move);
    PublishText(host,"za.bagtool.move",inst.assets.MoveName(move));
    PublishI64(host,"za.bagtool.type",move?inst.assets.MoveType(move):-1);
    PublishI64(host,"za.bagtool.power",std::max(0,move_info.power));
    PublishI64(host,"za.bagtool.accuracy",std::max(0,move_info.accuracy));
    for(std::size_t i=0;i<rows.size();++i){
        const auto prefix="za.bt"+std::to_string(i)+".";
        const auto& row=rows[i];
        PublishText(host,(prefix+"name").c_str(),name(row.entity));
        PublishText(host,(prefix+"context").c_str(),row.context);
        PublishI64(host,(prefix+"icon").c_str(),row.entity.internal_species*256+row.entity.form+
                   (row.entity.shiny?524288:0));
        PublishI64(host,(prefix+"selected").c_str(),int(i)==selected);
        PublishI64(host,(prefix+"eligible").c_str(),row.eligible);
        PublishI64(host,(prefix+"hp").c_str(),row.entity.hp);
        PublishI64(host,(prefix+"hpmax").c_str(),row.entity.hp_max);
        std::string status;
        if(tool==Tool::Heal) status=UiText(inst.assets,language,PartyStatusLabel(row.entity));
        if(tool==Tool::Hold) status=row.entity.held_item?inst.assets.ItemName(row.entity.held_item):UiText(inst.assets,language,"Aucun objet");
        if(tool==Tool::Machine) status=UiText(inst.assets,language,!row.eligible?"Incompatible":
            std::ranges::find(row.entity.moves,move)!=std::end(row.entity.moves)?"Déjà équipée":"Compatible");
        if(tool==Tool::Evolve&&row.evolution>0) status="→ "+std::string(inst.assets.SpeciesName(row.evolution));
        PublishText(host,(prefix+"status").c_str(),status);
    }
    const auto* target=selected>=0&&selected<rows.size()?&rows[selected]:nullptr;
    PublishI64(host,"za.bagtool.target",target!=nullptr);
    PublishI64(host,"za.bagtool.choose",!target&&!rows.empty()&&(tool!=Tool::Machine||any_eligible));
    PublishI64(host,"za.bagtool.fiche",target&&tool==Tool::Evolve);
    PublishI64(host,"za.bagtool.moves",target&&tool==Tool::Machine&&target->eligible);
    PublishI64(host,"za.bagtool.compare",target&&tool==Tool::Hold);
    PublishI64(host,"za.bagtool.removal",target&&tool==Tool::Hold&&!item&&target->entity.held_item);
    PublishI64(host,"za.bagtool.give",target&&tool==Tool::Hold&&item&&item!=target->entity.held_item);
    PublishI64(host,"za.bagtool.change_preview",target&&tool==Tool::Hold&&item&&target->entity.held_item);
    const bool party_target=target&&target->party>=0&&target->eligible&&native_available;
    const bool held_action=party_target&&tool==Tool::Hold&&item!=target->entity.held_item;
    const bool heal_action=party_target&&tool==Tool::Heal;
    const bool can_apply=live.party_current&&live.scene_idle&&live.deployed_index==-1&&!bag_pending&&mailbox_idle;
    PublishI64(host,"za.bagtool.held_apply",held_action);
    PublishI64(host,"za.bagtool.held_ready",held_action&&can_apply);
    PublishI64(host,"za.bagtool.heal_apply",heal_action);
    PublishI64(host,"za.bagtool.heal_ready",heal_action&&can_apply&&live.hp_valid[target->party]);
    if(!bag_pending&&(held_action||heal_action)){
        if(!live.scene_idle) bag_status="Ferme les menus du jeu.";
        else if(live.deployed_index>=0) bag_status="Rappelle ton Pokémon dans sa Poké Ball.";
        else if(!live.party_current||live.deployed_index==-2||(heal_action&&!live.hp_valid[target->party]))
            bag_status="Lecture de l'équipe en cours…";
        else if(!mailbox_idle) bag_status="Changement en cours…";
    }
    if(bag_status.empty()&&tool==Tool::Hold&&target&&(!native_available||target->party<0))
        bag_status="Équipe l'objet dans le jeu.";
    if(bag_status.empty()&&tool==Tool::Heal&&target&&target->eligible&&!heal_action)
        bag_status="À utiliser dans le Sac du jeu.";
    PublishText(host,"za.bagtool.held_status",tool==Tool::Hold?UiText(inst.assets,language,bag_status):"");
    PublishText(host,"za.bagtool.heal_status",tool==Tool::Heal?UiText(inst.assets,language,bag_status):"");
    PublishText(host,"za.bagtool.target_name",target?name(target->entity):"");
    PublishI64(host,"za.bagtool.current_icon",target?target->entity.held_item:0);
    PublishText(host,"za.bagtool.current_item",target&&target->entity.held_item?
                inst.assets.ItemName(target->entity.held_item):UiText(inst.assets,language,"Aucun objet"));
    PublishText(host,"za.bagtool.wanted_item",item?inst.assets.ItemName(item):UiText(inst.assets,language,"Aucun objet"));
    std::scoped_lock lock{inst.mutex};
    if(inst.bag_tool_revision==revision) inst.bag_targets=std::move(rows);
}

std::string DexDisplayName(za::Assets& assets,int language,std::string name,
                           const za::Assets::DexEntry& entry,
                           const std::vector<za::Assets::DexEntry>& catalog,bool mega) {
    if(!mega||name.empty()) return name;
    name=UiText(assets,language,"Méga-")+name;
    const int national=za::NationalSpecies(entry.species);
    if(national==6||national==150)name+=entry.form==1?" X":" Y";
    else if(national==25)name+=entry.form==2?" X":" Y";
    else if((national==359||national==445||national==448)&&entry.form==2)name+=" Z";
    else if(national==678)name+=entry.form==2?" (M)":" (F)";
    else if(std::ranges::count(catalog,entry.species,&za::Assets::DexEntry::species)>1)
        name+=" · "+UiText(assets,language,"Forme")+" "+std::to_string(entry.form);
    return name;
}

void PublishSearch(Instance& inst,const EdenDsmodHostApi& host,const za::Save& save,
                   const za::LiveSnapshot& live) {
    int mode,keyboard;
    unsigned revision;
    std::string query,error;
    {
        std::scoped_lock lock{inst.mutex};
        mode=inst.search_mode;
        keyboard=inst.search_keyboard;
        revision=inst.search_revision;
        query=inst.search_query;
        error=inst.search_error;
    }
    const auto language=save.trainer.language;
    const auto tr=[&](const char* label){return UiText(inst.assets,language,label);};
    const auto folded=SearchFold(query);
    int number=-1;
    if(!folded.empty()&&folded.size()<=4&&std::ranges::all_of(folded,[](char c){return c>='0'&&c<='9';}))
        number=std::stoi(folded);
    std::set<std::string> native_keys;
    std::vector<Instance::SearchResult> results;
    const auto matches=[&](const std::string& name){
        const auto word=SearchFold(name);
        if(folded.empty()) {
            AddSearchKey(native_keys,word);
            return false;
        }
        const auto offset=word.find(folded);
        if(offset==std::string::npos) return false;
        AddSearchKey(native_keys,std::string_view(word).substr(offset+folded.size()));
        return true;
    };
    if(mode==0) {
        constexpr const char* titles[]={"ILLUMIS","MÉGA-DIMENSION","MÉGA-ÉVOLUTIONS"};
        for(int gen=1;gen<=3;++gen) {
            const auto catalog=inst.assets.Pokedex(gen-1);
            for(const auto& entry:catalog) {
                const std::string name=inst.assets.SpeciesName(entry.species);
                const int national=za::NationalSpecies(entry.species);
                const int regional=entry.number-(gen==2?232:0);
                const bool base_matches=matches(name);
                const auto display=DexDisplayName(inst.assets,language,name,entry,catalog,gen==3);
                if(!(base_matches||(gen==3&&matches(display))||number==regional||number==national)) continue;
                auto context=tr(titles[gen-1])+" · "+tr("N°")+" "+std::to_string(regional);
                if(number==national&&national!=regional) context+=" · "+tr("N° national")+" "+std::to_string(national);
                results.push_back({gen,national+(gen==3?2048*entry.form:0),entry.species*256+entry.form,{},
                                   display,std::move(context)});
            }
        }
    } else {
        const auto add=[&](const za::Save::BoxEntry& entry,bool shiny=false) {
            const std::string species=inst.assets.SpeciesName(entry.species);
            const bool name_matches=matches(species);
            const bool nickname_matches=!entry.nickname.empty()&&matches(entry.nickname);
            if(!(name_matches||nickname_matches||number==za::NationalSpecies(entry.species))) return;
            const auto name=entry.nickname.empty()||entry.nickname==species?species:entry.nickname+" · "+species;
            const auto place=tr("BOÎTE")+" "+std::to_string(entry.box+1)+" · "+tr("Case")+" "+std::to_string(entry.slot+1);
            results.push_back({0,0,entry.species*256+entry.form+(shiny?524288:0),entry,name,place});
        };
        const bool fresh_box=live.box_live&&live.box_valid&&live.box_view_index>=0;
        if(save.box_entries) for(const auto& entry:*save.box_entries) {
            if(!fresh_box||entry.box!=live.box_view_index) add(entry);
        }
        if(fresh_box) for(std::size_t i=0;i<live.box_view.size();++i) {
            const auto& entity=live.box_view[i];
            if(entity.present) add({za::u16(live.box_view_index),za::u16(i),entity.internal_species,entity.form,
                                   entity.pid,entity.encryption_constant,entity.nickname},entity.shiny);
        }
        std::sort(results.begin(),results.end(),[](const auto& a,const auto& b){
            return std::pair(a.box.box,a.box.slot)<std::pair(b.box.box,b.box.slot);
        });
    }
    std::vector<std::string> keys(native_keys.begin(),native_keys.end());
    for(const char letter:std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ")) keys.emplace_back(1,letter);
    constexpr int keys_per_page=30;
    const int last=std::max(0,int(keys.size()-1)/keys_per_page);
    keyboard=std::clamp(keyboard,0,last);
    PublishI64(host,"za.search.previous",keyboard>0);
    PublishI64(host,"za.search.next",keyboard<last);
    const auto first=keys.begin()+keyboard*keys_per_page;
    std::vector<std::string> visible(first,first+std::min<int>(keys_per_page,int(keys.end()-first)));
    for(std::size_t i=0;i<visible.size();++i)
        PublishText(host,("za.sk"+std::to_string(i)+".key").c_str(),visible[i]);
    PublishI64(host,"za.search.keys",visible.size());
    PublishI64(host,"za.search.mode",mode);
    PublishI64(host,"za.search.rows",results.size());
    PublishI64(host,"za.search.reset",revision);
    PublishI64(host,"za.search.empty",!query.empty()&&results.empty());
    PublishI64(host,"za.search.typing",!query.empty());
    PublishText(host,"za.search.query",query);
    PublishText(host,"za.search.error",UiText(inst.assets,language,error));
    for(std::size_t i=0;i<results.size();++i) {
        const auto prefix="za.sr"+std::to_string(i)+".";
        PublishText(host,(prefix+"name").c_str(),results[i].name);
        PublishText(host,(prefix+"context").c_str(),results[i].context);
        PublishI64(host,(prefix+"icon").c_str(),results[i].icon);
    }
    std::scoped_lock lock{inst.mutex};
    if(inst.search_revision==revision) {
        inst.search_keyboard=keyboard;
        inst.search_keys=std::move(visible);
        inst.search_results=std::move(results);
    }
}

void PublishCapturePlan(Instance& inst,const EdenDsmodHostApi& host,const za::Save& save,
                        const std::vector<Instance::Objective>& objectives,
                        const std::vector<za::Assets::Zone>& zones) {
    std::set<int> targets,covered;
    for(const auto& objective:objectives) {
        if(objective.category>=0||objective.mega) continue;
        const int national=za::NationalSpecies(objective.species);
        if(national>0&&!DexCaught(save,national)) targets.insert(national);
    }
    std::vector<Instance::CaptureZone> plan;
    for(const auto& zone:zones) {
        std::set<int> in_zone;
        for(const auto& encounter:za::kZoneEncounters)
            if(encounter.zone==zone.number&&targets.contains(encounter.species)) in_zone.insert(encounter.species);
        if(in_zone.empty()) continue;
        covered.insert(in_zone.begin(),in_zone.end());
        plan.push_back({{zone.number,{},(2048+zone.x)*640/4096,(2048-zone.y)*640/4096},
                        {in_zone.begin(),in_zone.end()}});
    }
    std::sort(plan.begin(),plan.end(),[](const auto& a,const auto& b){
        if(a.species.size()!=b.species.size()) return a.species.size()>b.species.size();
        return a.location.zone<b.location.zone;
    });
    PublishI64(host,"za.capture.rows",plan.size());
    PublishI64(host,"za.capture.targets",targets.size());
    PublishI64(host,"za.capture.unknown",targets.size()-covered.size());
    for(std::size_t i=0;i<plan.size();++i) {
        const auto prefix="za.cp"+std::to_string(i)+".";
        PublishI64(host,(prefix+"zone").c_str(),plan[i].location.zone);
        PublishI64(host,(prefix+"count").c_str(),plan[i].species.size());
        PublishI64(host,(prefix+"recommended").c_str(),i==0);
        std::string names;
        for(const int national:plan[i].species) {
            if(!names.empty()) names+=" · ";
            names+=inst.assets.SpeciesName(InternalId(national));
        }
        PublishText(host,(prefix+"names").c_str(),names);
    }
    std::scoped_lock lock{inst.mutex};
    inst.capture_zones=std::move(plan);
}

std::string IndexedPoint(std::string name,int index){const auto pos=name.find("{i}");if(pos!=std::string::npos)name.replace(pos,3,std::to_string(index));return name;}

bool MissionOnMap(int state) {
    return state==2;
}

bool TravelUnlocked(const za::Assets::TravelDestination& destination, const za::LiveSnapshot& live) {
    return std::ranges::any_of(destination.points, [&](const auto& candidate) {
        return candidate.row>=0 && candidate.key && std::ranges::any_of(live.unlocked_travel, [&](const auto& point) {
            return point.kind==destination.kind && point.row==unsigned(candidate.row) && point.key==candidate.key;
        });
    });
}

int DimensionFilter(int kind) {
    return kind==0 ? 16 : kind==1 ? 32 : 128;
}

void PublishMissionRows(const EdenDsmodHostApi& host, za::Assets& assets, int language,
                        const char* group,const za::Assets::Mission& mission,unsigned columns,bool summary_first=false) {
    using Kind=za::Assets::MissionReward::Kind;
    int row=0;
    const auto emit=[&](const std::string& text, int style, int icon=0){
        if(row>=128)return;
        const auto key=std::string(group)+"."+std::to_string(row++)+".";
        PublishText(host,(key+"text").c_str(),text);
        PublishI64(host,(key+"style").c_str(),style);
        PublishI64(host,(key+"icon").c_str(),icon);
    };
    const auto paragraph=[&](std::string_view text){
        while(!text.empty()&&row<128){
            std::size_t end=0,space=0;
            unsigned count=0;
            while(end<text.size()&&text[end]!='\n'&&count<columns){
                if(text[end]==' ')space=end;
                ++end;
                while(end<text.size()&&(static_cast<unsigned char>(text[end])&0xC0)==0x80)++end;
                ++count;
            }
            if(end<text.size()&&text[end]!='\n'&&space)end=space;
            emit(std::string(text.substr(0,end)),0);
            text.remove_prefix(end);
            if(!text.empty()&&(text.front()==' '||text.front()=='\n'))text.remove_prefix(1);
        }
    };
    const auto objectives=[&] {
        if(!mission.objectives.empty()&&(summary_first||mission.objectives!=mission.description)){
            emit(UiText(assets,language,"OBJECTIFS"),1);
            paragraph(mission.objectives);
        }
    };
    const auto rewards=[&] {
        if(mission.rewards.empty()) return;
        emit(UiText(assets,language,"RÉCOMPENSES"),1);
        for(const auto& reward:mission.rewards){
            const auto amount=std::to_string(reward.amount);
            switch(reward.kind){
            case Kind::Item:
                emit(amount+" × "+assets.ItemName(reward.item),2,reward.item);
                break;
            case Kind::Money:
                emit(amount+" "+UiText(assets,language,"Pokédollars"),3);
                break;
            case Kind::ResearchPoints:
                emit(amount+" "+UiText(assets,language,"points de recherche"),4);
                break;
            }
        }
    };
    if(summary_first) {
        rewards();
        objectives();
        if(!mission.description.empty()&&mission.description!=mission.objectives){
            emit(UiText(assets,language,"Description"),1);
            paragraph(mission.description);
        }
    } else {
        paragraph(mission.description);
        objectives();
        rewards();
    }
    PublishI64(host,(std::string(group)+".rows").c_str(),row);
}

void PublishUi(const EdenDsmodHostApi& host,za::Assets& assets,int language){
    int missing=0;constexpr auto none=std::numeric_limits<s64>::min();
    for(const auto& spec:za::ui::specs){
        if(!*spec.bind){PublishText(host,spec.point,UiText(assets,language,spec.source,&missing));continue;}
        if(!host.get_i64)continue;
        const bool indexed=std::strstr(spec.bind,"{i}")!=nullptr;
        int first=0,last=spec.count;
        if(indexed){const char* region=std::strncmp(spec.bind,"za.d",4)==0?"za_dex":std::strncmp(spec.bind,"za.q",4)==0?"za_quests":std::strncmp(spec.bind,"za.zp",5)==0?"za_zone":nullptr;
            if(region){const auto window=VisibleRows(host,region,spec.count);first=window.first;last=window.second;}}
        for(int i=first;i<last;++i){
            const auto bind=IndexedPoint(spec.bind,i);const auto number=host.get_i64(host.userdata,bind.c_str(),none);if(number==none)continue;
            std::string text;
            if(!spec.names.empty()){if(number>=0&&number<spec.names.size())text=UiText(assets,language,spec.names[number],&missing);}
            else{
                auto prefix=UiText(assets,language,spec.source,&missing);if(!prefix.empty())prefix+=' ';
                auto value=std::to_string(number);if(spec.pad>value.size()&&number>=0)value.insert(0,spec.pad-value.size(),'0');text=prefix+value;
                if(*spec.maximum){const auto maximum=host.get_i64(host.userdata,IndexedPoint(spec.maximum,i).c_str(),none);if(maximum!=none)text+=std::string(spec.separator)+std::to_string(maximum);}
            }
            const auto point=std::string(spec.point)+(indexed?std::to_string(i):"");PublishText(host,point.c_str(),text);
        }
    }
    PublishI64(host,"za.ui.missing",assets.NamesReady()?missing:0);
}

void PublishEntity(const EdenDsmodHostApi& host, const char* prefix, int slot,
                   const za::Entity& entity, const std::vector<std::string>& names) {
    char key[48];
    auto name = [&](const char* suffix) {
        std::snprintf(key, sizeof(key), "%s%d.%s", prefix, slot, suffix);
        return key;
    };
    // The game's own French species name, once the name table is in memory (it is decoded on the
    // asset worker, so the first frames show nothing rather than stalling).
    const std::string& sname =
        entity.internal_species >= 1 && static_cast<std::size_t>(entity.internal_species) <= names.size()
            ? names[static_cast<std::size_t>(entity.internal_species) - 1]
            : std::string{};
    PublishText(host, name("sname"), sname);
    PublishI64(host, name("v"), entity.present ? 1 : 0);
    if (!entity.present) {
        PublishText(host, name("name"), "");
        PublishI64(host, name("sp"), 0);
        PublishI64(host, name("lvl"), 0);
        PublishI64(host, name("hp"), 0);
        PublishI64(host, name("hpmax"), 0);
        PublishI64(host, name("g"), 0);
        PublishI64(host, name("shiny"), 0);
        return;
    }
    PublishText(host, name("name"), entity.nickname);
    PublishI64(host, name("sp"), entity.species);
    PublishI64(host, name("nat"), entity.internal_species);
    PublishI64(host, name("icon"), entity.internal_species * 256 + entity.form + (entity.shiny ? 524288 : 0));
    PublishI64(host, name("lvl"), entity.level);
    PublishI64(host, name("hp"), entity.hp);
    PublishI64(host, name("hpmax"), entity.hp_max);
    PublishI64(host, name("g"), entity.gender);
    PublishI64(host, name("shiny"), entity.shiny ? 1 : 0);
    PublishI64(host, name("item"), entity.held_item);
    PublishI64(host, name("form"), entity.form);
    PublishI64(host, name("ball"), entity.ball);
    PublishI64(host, name("nature"), entity.nature);
}

void PublishSlot(const EdenDsmodHostApi& host, int slot, const za::Entity& entity,
                 const std::vector<std::string>& names) {
    PublishEntity(host, "za.p", slot, entity, names);
}

void Sample(void* p, const EdenDsmodHostApi* host) {
    if (!p || !host)
        return;
    auto& inst = *static_cast<Instance*>(p);

    za::Save save;int box_detail_open=0;za::Entity box_detail;
    bool nav_menu=false;
    std::string status,send_status,travel_status;
    bool swap_active=false;int swap_slot,swap_party,swap_box;za::Entity swap_source;std::string swap_status;std::array<bool,9> swap_commands{};
    bool travel_active=false;
    std::array<bool,5> map_commands{};
    bool box_follow = true;
    bool box_picker = false;
    int dex_gen, dex_filter, dex_page, bag_category, bag_page, selected, detail, dex_open;
    za::LiveSnapshot live;
    za::NativeBoxExchange native_box;std::string native_box_status;
    za::MoveMenu move_menu;
    bool guest_helper_alive = false, guest_helper_idle = false;
    za::GuestMailbox::TravelResult travel_result{};
    int quest_category,map_center,bag_selected,map_zone,bag_guide_item,box_detail_box,box_detail_slot;
    std::vector<Instance::Objective> objectives;
    Instance::Objective guide_target;
    std::string current_page;
    bool quest_show_map,bag_return_evolution;
    bool command_left,command_right,command_send;
    int map_filters,map_cafe,attack_pulse,research_selected;
    int map_landmark;
    bool placing_marker,marker_open_pulse,map_close_pulse,marker_pending,clearing_marker,persistence_pending;
    const char* marker_result;
    float marker_u,marker_v;
    bool research_open;std::string map_dimension;
    int map_quest;
    bool map_help;
    std::vector<Instance::MapPin> map_choices;
    unsigned map_choices_revision;
    float map_home_zoom,map_home_x,map_home_y,heading;
    bool heading_valid;
    u64 map_reset;
    bool map_filter_menu,map_missing_only;
    bool aim_pulse;
    std::unordered_map<std::uint64_t,za::Entity> confirmed_combat;
    std::string quest_selected_id;
    std::shared_ptr<const std::vector<std::string>> item_names;
    std::shared_ptr<const std::vector<bool>> available;
    std::shared_ptr<const std::vector<std::string>> names;
    {
        std::scoped_lock lock{inst.mutex};
        swap_slot=inst.swap_slot;swap_party=inst.swap_party;swap_box=inst.swap_box;swap_source=inst.swap_source;swap_commands=inst.swap_commands;swap_active=inst.box_swap.Active();swap_status=swap_active||!inst.box_swap.Status().empty()?inst.box_swap.Status():inst.swap_status;
        box_detail_open=inst.box_detail_open;box_detail=inst.box_detail_entity;
        box_detail_box=inst.box_detail_box;box_detail_slot=inst.box_detail_slot;
        bag_guide_item=inst.bag_guide_item;
        bag_return_evolution=inst.bag_return_evolution;
        save = inst.save;bag_selected=inst.bag_selected;map_zone=inst.map_zone;map_center=inst.map_center;send_status=inst.send_status;travel_status=inst.travel.Status();travel_active=inst.travel.Active();map_commands=inst.map_commands;
        map_cafe=inst.map_cafe;map_filter_menu=inst.map_filter_menu;
        map_missing_only=inst.map_missing_only;
        map_landmark=inst.map_landmark;placing_marker=inst.placing_marker;
        marker_open_pulse=inst.marker_open_pulse;map_close_pulse=inst.map_close_pulse;
        marker_pending=inst.marker_request.has_value();
        clearing_marker=marker_pending&&inst.marker_request->clear;
        marker_result=inst.marker_result;
        map_dimension=inst.map_dimension;map_quest=inst.map_quest;
        map_help=inst.map_help;map_choices=inst.map_choices;map_choices_revision=inst.map_choices_revision;
        research_open=inst.research_open;research_selected=inst.research_selected;
        map_filters=inst.map_filters;map_reset=inst.map_reset;
        map_home_zoom=inst.map_home_zoom;map_home_x=inst.map_home_x;map_home_y=inst.map_home_y;
        heading=inst.heading;heading_valid=inst.heading_valid;
        if(inst.language_override)save.trainer.language=inst.language_override;
        aim_pulse=inst.aim_pulse;attack_pulse=inst.attack_pulse;
        command_left=inst.command_left;command_right=inst.command_right;command_send=inst.command_send;
        quest_category=inst.quest_category;quest_selected_id=inst.quest_selected_id;
        const auto* page=host->get_text?host->get_text(host->userdata,"__page"):nullptr;
        if(page) current_page=page;
        constexpr std::string_view guide_pages[]={"","locations","combat","missions","evolutions","capture","search","dex","bag","bag_tools","team","team_heal"};
        if(inst.guide_page>0&&current_page==guide_pages[inst.guide_page]) inst.guide_page=0;
        PublishI64(*host,"za.guide.page",inst.guide_page);
        SyncObjectives(inst,*host);
        SyncBagFavorites(inst,*host);
        const auto pending=[](const auto& write){return write.has_value();};
        persistence_pending=inst.nav_pending.has_value()||
            std::ranges::any_of(inst.objective_writes,pending)||
            std::ranges::any_of(inst.bag_favorite_writes,pending);
        objectives=inst.objectives;
        guide_target=inst.guide_target;
        if (page && std::strcmp(page,"map")==0) inst.quest_show_map=false;
        quest_show_map=inst.quest_show_map;
        live=inst.live; confirmed_combat=inst.confirmed_combat; selected=inst.selected; detail=inst.detail; dex_open=inst.dex_open;
        inst.native_box.Observe(live);native_box=inst.native_box;native_box_status=inst.native_box_status;
        if(inst.move_menu.opened)inst.move_menu.Refresh(live);
        move_menu=inst.move_menu;
        guest_helper_alive = inst.guest_helper_alive;
        guest_helper_idle = guest_helper_alive && inst.guest_mailbox.Idle();
        travel_result = inst.guest_mailbox.TravelStatus();
        status = inst.status;
        box_follow = inst.box_follow;
        if(current_page!="box") inst.box_picker=false;
        box_picker=inst.box_picker;
        names = inst.species_names;
        item_names = inst.item_names;
        available = inst.available_species;
        dex_gen = inst.dex_gen; dex_filter = inst.dex_filter; dex_page = inst.dex_page;
        bag_category = inst.bag_category; bag_page = inst.bag_page;
    }
    if(!live.quest_work.empty())save.quest_work=live.quest_work;
    if(live.party_valid){std::copy(live.party.begin(),live.party.end(),save.party);save.party_count=live.party_count;}
    if(live.box_valid&&live.box_view_index==save.box_view_index){std::copy(live.box_view.begin(),live.box_view.end(),save.box_view);save.boxes_used[save.box_view_index]=std::count_if(live.box_view.begin(),live.box_view.end(),[](const auto& e){return e.present;});}
    for(auto& entity:save.party){auto hp=confirmed_combat.find((std::uint64_t(entity.pid)<<32)|entity.encryption_constant);if(hp!=confirmed_combat.end()&&hp->second.hp<=entity.hp_max){entity.hp=hp->second.hp;entity.exp=hp->second.exp;}}
    if(live.coordinates_valid){save.map_name=live.area;save.x=live.x;save.y=live.y;save.z=live.z;save.rotation=live.rotation;}
    static const std::vector<std::string> kNoNames;
    const std::vector<std::string>& species_names = names ? *names : kNoNames;

    const auto dex_rows= DexRows(inst.assets,save,dex_gen,dex_filter);
    if(!dex_rows.empty()&&std::find(dex_rows.begin(),dex_rows.end(),detail)==dex_rows.end())detail=dex_rows.front();
    const auto dex_catalog=inst.assets.Pokedex(dex_gen-1);
    auto dex_entry=[&](int id){for(const auto& e:dex_catalog)if(za::NationalSpecies(e.species)==id%2048&&(dex_gen!=3||e.form==id/2048))return e;return za::Assets::DexEntry{};};
    const auto dex_name=[&](const za::Assets::DexEntry& e){
        if(e.species<=0||e.species>species_names.size())return std::string{};
        return DexDisplayName(inst.assets,save.trainer.language,species_names[e.species-1],e,dex_catalog,dex_gen==3);
    };
    if (host->begin_output)
        host->begin_output(host->userdata);

    {
        std::scoped_lock lock{inst.mutex};
        PublishNavigation(inst,*host,current_page,save.trainer.language);
        nav_menu=inst.nav_menu;
    }
    for(int i=0;i<6;++i){
        const auto prefix="za.p"+std::to_string(i)+".";
        PublishI64(*host,(prefix+"selected").c_str(),selected==i);
        PublishI64(*host,(prefix+"deployed").c_str(),live.deployed_index==i&&live.party_live);
    }
    if(current_page=="search") PublishSearch(inst,*host,save,live);
    if(current_page=="bag_tools") PublishBagTools(inst,*host,save,live,nav_menu);
    if(current_page=="team_heal") PublishTeamHealing(inst,*host,save);
    const auto xp_info=inst.assets.SpeciesInfo(save.party[selected].internal_species,save.party[selected].form);
    const auto xp_low=inst.assets.ExperienceAt(xp_info.growth,save.party[selected].level),xp_high=inst.assets.ExperienceAt(xp_info.growth,std::min(100,int(save.party[selected].level)+1));
    PublishI64(*host,"za.sel.xp",save.party[selected].level==100?1:save.party[selected].exp>=xp_low?std::min(save.party[selected].exp-xp_low,xp_high-xp_low):0);
    PublishI64(*host,"za.sel.xpmax",save.party[selected].level==100?1:std::max(1u,xp_high-xp_low));PublishI64(*host,"za.sel.exp",save.party[selected].exp);
    const int research_level=live.research_live?live.research_level:save.research_level;
    const int research_points=live.research_live?live.research_points:save.research_points;
    PublishI64(*host,"za.progress.mable_level",research_level);PublishI64(*host,"za.progress.mable_points",research_points);
    PublishI64(*host,"za.progress.research_live",live.research_live);
    const auto research=inst.assets.ResearchLevels();
    if(research_selected<1||research_selected>research.size())research_selected=std::max(1,research_level);
    PublishI64(*host,"za.research.open",research_open);
    PublishI64(*host,"za.research.open.scroll",research_open&&!nav_menu);
    PublishI64(*host,"za.research.rows",research.size());
    PublishI64(*host,"za.research.selected",research_selected);
    PublishI64(*host,"za.research.follow",research_selected-1);
    const auto research_window=VisibleRows(*host,"za_research",research.size());
    for(int i=research_window.first;i<research_window.second;++i){const auto& r=research[i];const auto p="za.rl"+std::to_string(i)+".";
        PublishI64(*host,(p+"v").c_str(),1);PublishI64(*host,(p+"selected").c_str(),r.level==research_selected);
        PublishText(*host,(p+"label").c_str(),UiText(inst.assets,save.trainer.language,"NIVEAU")+" "+std::to_string(r.level));
        PublishI64(*host,(p+"points").c_str(),r.points);
        const int move=TMMove(r.item);
        PublishI64(*host,(p+"icon").c_str(),move?328:r.item);
        PublishText(*host,(p+"reward").c_str(),r.item?std::to_string(r.count)+" × "+inst.assets.ItemName(r.item)+
            (move?" · "+inst.assets.MoveName(move):""):"—");
    }
    const auto reward=std::find_if(research.begin(),research.end(),[&](const auto& r){return r.level==research_selected;});
    int reward_type=-1,reward_icon=0;std::string reward_name="—",reward_description;
    if(reward!=research.end()&&reward->item){
        reward_icon=reward->item;reward_name=std::to_string(reward->count)+" × "+inst.assets.ItemName(reward->item);
        reward_description=inst.assets.ItemDescription(reward->item);
        for(const auto& tm:za::kTMs)if(tm.item==reward->item){reward_icon=328;reward_type=inst.assets.MoveType(tm.move);
            reward_name+=" · "+inst.assets.MoveName(tm.move);reward_description=inst.assets.MoveDescription(tm.move);break;}
    }
    PublishI64(*host,"za.research.icon",reward_icon);PublishI64(*host,"za.research.type",reward_type);
    PublishText(*host,"za.research.reward",reward_name);PublishText(*host,"za.research.description",reward_description);
    PublishI64(*host,"za.research.points",reward!=research.end()?reward->points:0);
    PublishI64(*host,"za.research.lines",std::max(1,int(reward_description.size()/44)+4));
    int low=0,high=0;
    for(const auto& row:research){if(row.level==research_level)low=row.points;if(row.level==research_level+1)high=row.points;}
    const bool max_research=!research.empty()&&research_level==research.back().level;
    PublishI64(*host,"za.progress.research_max",max_research);
    PublishI64(*host,"za.progress.research_xp",high>low?std::clamp(research_points-low,0,high-low):0);
    PublishI64(*host,"za.progress.research_xpmax",high>low?high-low:1);
    PublishI64(*host,"za.progress.research_missing",high>low?std::max(0,high-research_points):0);
    PublishText(*host,"za.progress.research_fraction",high>low?std::to_string(std::max(0,research_points-low))+" / "+std::to_string(high-low):"—");
    for(int i=0;i<3;++i){
        const auto prefix="za.progress.r"+std::to_string(i)+".";
        const auto found=std::find_if(research.begin(),research.end(),[&](const auto& r){return r.level==research_level+i-1;});
        const bool valid=found!=research.end();
        PublishI64(*host,(prefix+"valid").c_str(),valid);PublishI64(*host,(prefix+"level").c_str(),valid?found->level:0);
        PublishI64(*host,(prefix+"points").c_str(),valid?found->points:0);PublishI64(*host,(prefix+"item").c_str(),valid?found->item:0);
        const bool machine=valid&&std::any_of(std::begin(za::kTMs),std::end(za::kTMs),[&](const auto& tm){return tm.item==found->item;});
        PublishI64(*host,(prefix+"icon").c_str(),machine?328:valid?found->item:0);
        PublishText(*host,(prefix+"reward").c_str(),valid&&found->item?std::to_string(found->count)+" × "+inst.assets.ItemName(found->item):"—");
    }
    const int infinite_points=live.infinite_points>=0?live.infinite_points:save.infinite_points;
    // The infinite league's ticket repeats every 50,000 points (verified in the game's UI).
    constexpr int infinite_target=50000;
    PublishI64(*host,"za.progress.infinite_points",infinite_points);
    PublishI64(*host,"za.progress.infinite_target",infinite_target);
    PublishI64(*host,"za.progress.infinite_missing",infinite_points>=0?std::max(0,infinite_target-infinite_points):-1);
    PublishI64(*host,"za.progress.infinite_live",live.infinite_points>=0);
    PublishText(*host,"za.progress.infinite_fraction",infinite_points>=0?std::to_string(infinite_points)+" / "+std::to_string(infinite_target):UiText(inst.assets,save.trainer.language,"Indisponible"));
    PublishI64(*host,"za.progress.hyperspace",live.hyperspace_points>=0?live.hyperspace_points:save.hyperspace_points);
    PublishI64(*host,"za.progress.money",live.money>=0?live.money:save.money);
    PublishI64(*host,"za.progress.tickets",save.ticket_points);
    PublishI64(*host,"za.progress.wins",live.royale_live?live.royale_wins:save.royale_wins);
    PublishI64(*host,"za.progress.medals",live.royale_live?live.royale_medals:save.royale_medals);
    const auto ranks=inst.assets.TrainerRanks();const int rank=live.rank_live?live.trainer_rank:-1;
    const auto rank_row=std::find_if(ranks.begin(),ranks.end(),[&](const auto& r){return r.rank==rank;});
    const bool infinite=rank==27;
    int target=-1;for(const auto& r:ranks)if(r.rank==rank+1)target=r.points;
    const int points=infinite?infinite_points:live.ticket_points;
    if(infinite)target=infinite_target;
    PublishText(*host,"za.progress.rank",rank_row!=ranks.end()?rank_row->name:"—");
    PublishI64(*host,"za.progress.rank_known",rank>=1);PublishI64(*host,"za.progress.is_infinite",infinite);
    PublishI64(*host,"za.progress.rank_points",std::max(0,points));PublishI64(*host,"za.progress.rank_target",std::max(1,target));
    PublishI64(*host,"za.progress.rank_missing",points>=0&&target>=0?std::max(0,target-points):0);
    PublishText(*host,"za.progress.rank_fraction",points>=0&&target>=0?std::to_string(points)+" / "+std::to_string(target):"—");
    PublishI64(*host,"za.map.battle_zones",inst.assets.BattleZones().size());
    PublishI64(*host,"za.boxdetail.open",box_detail_open);PublishI64(*host,"za.boxdetail.icon",box_detail.internal_species*256+box_detail.form+(box_detail.shiny?524288:0));
    PublishI64(*host,"za.boxdetail.toggle",box_detail_open%2);
    PublishText(*host,"za.boxdetail.position",box_detail_box<0?"":UiText(inst.assets,save.trainer.language,"BOÎTE")+" "+
        std::to_string(box_detail_box+1)+" · "+UiText(inst.assets,save.trainer.language,"Case")+" "+std::to_string(box_detail_slot+1));
    PublishText(*host,"za.boxdetail.name",box_detail.nickname.empty()&&box_detail.internal_species>0&&box_detail.internal_species<=species_names.size()?species_names[box_detail.internal_species-1]:box_detail.nickname);
    PublishText(*host,"za.boxdetail.item_name",box_detail.held_item?inst.assets.ItemName(box_detail.held_item):UiText(inst.assets,save.trainer.language,"Aucun objet"));
    PublishText(*host,"za.boxdetail.nature_name",inst.assets.NatureName(box_detail.nature));
    for(int t=0;t<18;++t){const auto key="za.type"+std::to_string(t)+".name";PublishText(*host,key.c_str(),inst.assets.TypeName(t));}
    const auto bi=inst.assets.SpeciesInfo(box_detail.internal_species,box_detail.form);
    PublishI64(*host,"za.boxdetail.type1",bi.type1);PublishI64(*host,"za.boxdetail.type2",bi.type2==bi.type1?-1:bi.type2);
    PublishI64(*host,"za.boxdetail.lvl",box_detail.level);PublishI64(*host,"za.boxdetail.g",box_detail.gender);PublishI64(*host,"za.boxdetail.nature",box_detail.nature);PublishI64(*host,"za.boxdetail.sp",box_detail.species);PublishI64(*host,"za.boxdetail.ball",box_detail.ball);
    PublishI64(*host,"za.boxdetail.hp",box_detail.hp);PublishI64(*host,"za.boxdetail.hpmax",box_detail.hp_max);PublishI64(*host,"za.boxdetail.atk",box_detail.attack);PublishI64(*host,"za.boxdetail.def",box_detail.defense);PublishI64(*host,"za.boxdetail.spa",box_detail.sp_attack);PublishI64(*host,"za.boxdetail.spd",box_detail.sp_defense);PublishI64(*host,"za.boxdetail.spe",box_detail.speed);
    for(int i=0;i<4;++i){const auto prefix="za.boxdetail.m"+std::to_string(i)+".";PublishText(*host,(prefix+"name").c_str(),inst.assets.MoveName(box_detail.moves[i]));PublishI64(*host,(prefix+"type").c_str(),inst.assets.MoveType(box_detail.moves[i]));PublishI64(*host,(prefix+"pp").c_str(),box_detail.move_pp[i]);}
    const auto live_hp=std::count(live.hp_valid.begin(),live.hp_valid.end(),true);
    PublishI64(*host,"za.live.ready",live_hp==save.party_count&&live_hp>0);
    PublishI64(*host,"za.map.live",live.position_live?1:0);
    PublishI64(*host,"za.map.mode",live.map_mode);PublishI64(*host,"za.map.destination",live.map_destination);
    PublishText(*host,"za.travel.status",travel_status);PublishI64(*host,"za.travel.active",travel_active);
    constexpr const char* map_keys[]={"za.cmd.map_open","za.cmd.map_destinations","za.cmd.map_up","za.cmd.map_down","za.cmd.map_confirm"};
    for(int i=0;i<5;++i)PublishI64(*host,map_keys[i],map_commands[i]);
    PublishI64(*host,"za.cmd.map_close",map_close_pulse);
    PublishI64(*host,"za.cmd.marker_open",marker_open_pulse);
    // Released runtimes log every refused enforce action, even on idle ticks.
    PublishI64(*host,"za.enforce.active",persistence_pending||command_left||command_right||
        command_send||aim_pulse||attack_pulse>=0||marker_open_pulse||map_close_pulse);
    PublishText(*host,"za.map.status",live.position_live?"Position du joueur en direct":"Position de sauvegarde - suivi en direct indisponible");
    PublishI64(*host,"za.cmd.aim",aim_pulse);
    for(int i=0;i<4;++i)PublishI64(*host,("za.cmd.attack"+std::to_string(i)).c_str(),attack_pulse==i);
    PublishI64(*host,"za.cmd.left",command_left);PublishI64(*host,"za.cmd.right",command_right);PublishI64(*host,"za.cmd.send",command_send);
    PublishI64(*host,"za.game.selected",live.selected_index);
    PublishI64(*host,"za.game.deployed",live.deployed_index);
    PublishI64(*host,"za.party.direct",live.party_live);
    PublishI64(*host,"za.party.current",live.party_current);
    PublishI64(*host,"za.scene.idle",live.scene_idle);
    PublishI64(*host,"za.box.current",live.box_live);
    PublishText(*host,"za.send.status",UiText(inst.assets,save.trainer.language,send_status));
    std::string live_status=live.diagnostic,pending;
    if(live.party_valid)for(int i=0;i<save.party_count&&i<6;++i)if(save.party[i].present&&!live.hp_valid[i]){const auto internal=save.party[i].internal_species;
        if(!pending.empty())pending+=", ";pending+=internal>0&&internal<=species_names.size()?species_names[internal-1]:"Pokémon "+std::to_string(i+1);}
    if(!pending.empty())live_status+="\nPV à confirmer : "+pending;
    PublishText(*host,"za.live.status",live_status);
    PublishI64(*host,"za.live.problem",live.diagnostic.starts_with("Build chargé")||live.diagnostic.starts_with("Mémoire :"));
    PublishI64(*host,"za.swap.open",swap_slot>=0);PublishI64(*host,"za.swap.active",swap_active);
    PublishI64(*host,"za.swap.party",swap_party);PublishI64(*host,"za.swap.ready",swap_party>=0&&!swap_active);
    PublishText(*host,"za.swap.status",swap_status);
    PublishText(*host,"za.swap.name",swap_source.nickname);
    PublishI64(*host,"za.swap.icon",swap_source.internal_species*256+swap_source.form+(swap_source.shiny?524288:0));
    constexpr const char* swap_keys[]={"left","right","up","down","pick","confirm","back","next","previous"};
    for(int i=0;i<9;++i)PublishI64(*host,(std::string("za.cmd.swap_")+swap_keys[i]).c_str(),swap_commands[i]);
    PublishI64(*host,"za.box.live",live.box_valid&&live.box_view_index==save.box_view_index);
    bool native_box_available=false;
    constexpr std::uint8_t native_build[]={0xB1,0xF1,0x2F,0xD9,0x19,0xEA,0xE8,0x6A};
#ifdef _WIN32
    native_box_available=std::memcmp(host->build_id,native_build,8)==0;
#elif defined(__ANDROID__)
    native_box_available=guest_helper_alive&&std::memcmp(host->build_id,native_build,8)==0;
#endif
    const bool native_operation_idle = !guest_helper_alive || guest_helper_idle;
    PublishI64(*host,"za.nativebox.available",native_box_available);
    PublishI64(*host,"za.nativebox.ready",native_box_available&&native_operation_idle&&native_box.Ready(live));
    PublishI64(*host,"za.nativebox.box",native_box.box);PublishI64(*host,"za.nativebox.slot",native_box.slot);
    PublishI64(*host,"za.nativebox.party",native_box.party);
    PublishI64(*host,"za.nativebox.open",native_box.slot>=0);
    PublishI64(*host,"za.nativebox.done",native_box.completed);
    PublishI64(*host,"za.nativebox.icon",native_box.source.internal_species*256+native_box.source.form+(native_box.source.shiny?524288:0));
    PublishText(*host,"za.nativebox.name",native_box.source.nickname.empty()&&native_box.source.internal_species>0&&native_box.source.internal_species<=species_names.size()?species_names[native_box.source.internal_species-1]:native_box.source.nickname);
    if(native_box.completed)native_box_status="Échange confirmé dans le jeu";
    else if(native_box.party>=0&&!native_box.Ready(live))native_box_status="Revenez en ville et rappelez le Pokémon sorti. Vérifiez votre sélection.";
    else if(native_box.party>=0)native_box_status="Prêt pour l'échange direct.";
    PublishText(*host,"za.nativebox.status",UiText(inst.assets,save.trainer.language,native_box_status));
    PublishI64(*host,"za.moves.toggle",move_menu.opened%2);
    PublishI64(*host,"za.moves.frombox",move_menu.from_box);
    PublishI64(*host,"za.moves.frombag",move_menu.from_bag);
    PublishI64(*host,"za.moves.team_origin",!move_menu.from_box&&!move_menu.from_bag);
    if(move_menu.from_bag&&move_menu.slot>=0&&move_menu.slot<4){
        const auto current=move_menu.entity.moves[move_menu.slot];
        const auto power=inst.assets.MoveParameters(current).power;
        auto line=UiText(inst.assets,save.trainer.language,"Capacité actuelle")+" · "+inst.assets.MoveName(current);
        if(power>0) line+=" · "+std::to_string(power);
        PublishText(*host,"za.moves.compare",line);
    }
    PublishI64(*host,"za.moves.slot",move_menu.slot);
    PublishI64(*host,"za.moves.rows",move_menu.choices.size());
    PublishI64(*host,"za.moves.reset",move_menu.opened);
    PublishI64(*host,"za.moves.icon",move_menu.entity.internal_species*256+move_menu.entity.form+(move_menu.entity.shiny?524288:0));
    PublishText(*host,"za.moves.pokemon",move_menu.entity.nickname.empty()?inst.assets.SpeciesName(move_menu.entity.internal_species):move_menu.entity.nickname);
    PublishI64(*host,"za.moves.can_apply",native_box_available&&native_operation_idle&&move_menu.Ready(live));
    PublishI64(*host,"za.moves.target",move_menu.party>=0&&move_menu.party<6&&live.party_current?live.party_nodes[move_menu.party]:0);
    PublishI64(*host,"za.moves.desired",move_menu.desired);
    PublishI64(*host,"za.moves.detail_reset",static_cast<s64>(move_menu.opened)*65536+move_menu.desired);
    PublishI64(*host,"za.moves.type",inst.assets.MoveType(move_menu.desired));
    PublishText(*host,"za.moves.name",inst.assets.MoveName(move_menu.desired));
    PublishMoveStats(*host,inst.assets,save.trainer.language,"za.moves.",move_menu.desired);
    PublishText(*host,"za.moves.description",inst.assets.MoveDescription(move_menu.desired));
    const char* move_status="Choisissez une capacité à remplacer, puis une capacité disponible.";
    if(move_menu.from_box)move_status="Consultation en boîte. Modification directe non disponible.";
    else if(!native_box_available)move_status="Consultation des capacités sur cette plateforme.";
    else if(move_menu.slot>=0&&move_menu.slot<4&&move_menu.entity.moves[move_menu.slot]==move_menu.desired)move_status="Capacité équipée.";
    else if(move_menu.Ready(live))move_status="Prêt à remplacer la capacité sélectionnée.";
    else if(std::find(move_menu.entity.moves.begin(),move_menu.entity.moves.end(),move_menu.desired)!=move_menu.entity.moves.end())move_status="Cette capacité est déjà équipée.";
    else move_status="En ville, rappelez le Pokémon sorti.";
    PublishText(*host,"za.moves.status",UiText(inst.assets,save.trainer.language,move_status));
    for(int i=0;i<4;++i){const auto p="za.moves.e"+std::to_string(i)+".";
        PublishText(*host,(p+"name").c_str(),inst.assets.MoveName(move_menu.entity.moves[i]));
        PublishI64(*host,(p+"type").c_str(),inst.assets.MoveType(move_menu.entity.moves[i]));
        PublishI64(*host,(p+"selected").c_str(),move_menu.slot==i);}
    const auto move_window=VisibleRows(*host,"za_moves",move_menu.choices.size());
    for(int i=move_window.first;i<move_window.second;++i){const auto& row=move_menu.choices[i];const auto p="za.lm"+std::to_string(i)+".";
        PublishI64(*host,(p+"v").c_str(),1);PublishI64(*host,(p+"id").c_str(),row.id);
        PublishI64(*host,(p+"type").c_str(),inst.assets.MoveType(row.id));
        PublishI64(*host,(p+"selected").c_str(),row.id==move_menu.desired);
        PublishText(*host,(p+"name").c_str(),inst.assets.MoveName(row.id));
        std::string origin=row.item?inst.assets.ItemName(row.item):row.level?UiText(inst.assets,save.trainer.language,"NIVEAU")+" "+std::to_string(row.level):UiText(inst.assets,save.trainer.language,"Capacité équipée.");
        if(std::find(move_menu.entity.moves.begin(),move_menu.entity.moves.end(),row.id)!=move_menu.entity.moves.end())origin=UiText(inst.assets,save.trainer.language,"Capacité équipée.");
        PublishText(*host,(p+"origin").c_str(),origin);}
    PublishI64(*host,"za.box.input",live.boxes.valid);
    PublishI64(*host,"za.box.cursor",live.boxes.cursor);PublishI64(*host,"za.box.party_cursor",live.boxes.party_cursor);
    PublishI64(*host,"za.box.drag.source_box",live.boxes.source_box);PublishI64(*host,"za.box.drag.source_slot",live.boxes.source_slot);
    PublishI64(*host,"za.box.drag.target_box",live.boxes.target_box);PublishI64(*host,"za.box.drag.target_slot",live.boxes.target_slot);
    PublishI64(*host,"za.team.selected",selected);
    for(int slot=0;slot<6;++slot)for(int move=0;move<4;++move){
        const auto& timer=live.recharge[slot][move];
        const bool charging=timer.valid&&timer.remaining>0&&timer.initial>0;
        const std::string key="za.p"+std::to_string(slot)+".";
        PublishI64(*host,(key+"c"+std::to_string(move)).c_str(),charging);
        PublishI64(*host,(key+"r"+std::to_string(move)).c_str(),
                   charging?std::lround(1000.f*(1.f-timer.remaining/timer.initial)):0);
    }
    const auto& chosen=save.party[selected];
    PublishEntity(*host,"za.selected",0,chosen,species_names);
    // Selected panel uses a non-indexed binding namespace.
    for(int i=0;i<6;++i){const auto& e=save.party[i];const auto info=inst.assets.SpeciesInfo(e.internal_species,e.form);
        const auto low=inst.assets.ExperienceAt(info.growth,e.level),high=inst.assets.ExperienceAt(info.growth,std::min(100,int(e.level)+1));
        const auto base="za.p"+std::to_string(i)+".";
        PublishI64(*host,(base+"xp").c_str(),e.level==100?1:e.exp>=low?std::min(e.exp-low,high-low):0);
        PublishI64(*host,(base+"xpmax").c_str(),e.level==100?1:std::max(1u,high-low));
    }
    PublishI64(*host,"za.sel.v",chosen.present);
    PublishI64(*host,"za.sel.icon",chosen.internal_species*256+chosen.form+(chosen.shiny?524288:0));
    PublishText(*host,"za.sel.name",chosen.nickname);
    PublishText(*host,"za.sel.sname",chosen.internal_species>0 && chosen.internal_species<=species_names.size()?species_names[chosen.internal_species-1]:"");
    PublishI64(*host,"za.sel.lvl",chosen.level);PublishI64(*host,"za.sel.hp",chosen.hp);PublishI64(*host,"za.sel.hpmax",chosen.hp_max);
    PublishI64(*host,"za.sel.sp",chosen.species);PublishI64(*host,"za.sel.g",chosen.gender);PublishI64(*host,"za.sel.nature",chosen.nature);PublishI64(*host,"za.sel.item",chosen.held_item);
    PublishText(*host,"za.sel.item_name",chosen.held_item?inst.assets.ItemName(chosen.held_item):UiText(inst.assets,save.trainer.language,"Aucun objet"));
    const auto selected_info=inst.assets.SpeciesInfo(chosen.internal_species,chosen.form);
    PublishI64(*host,"za.sel.type1",selected_info.type1);PublishI64(*host,"za.sel.type2",selected_info.type2==selected_info.type1?-1:selected_info.type2);
    PublishI64(*host,"za.sel.moves_ready",chosen.present);
    for(int i=0;i<4;++i){const auto prefix="za.sel.m"+std::to_string(i)+".";
        PublishI64(*host,(prefix+"id").c_str(),chosen.moves[i]);
        PublishText(*host,(prefix+"name").c_str(),inst.assets.MoveName(chosen.moves[i]));
        PublishI64(*host,(prefix+"type").c_str(),inst.assets.MoveType(chosen.moves[i]));
        PublishI64(*host,(prefix+"pp").c_str(),chosen.move_pp[i]);
        PublishI64(*host,(prefix+"ppmax").c_str(),0);
        if(chosen.moves[i]) PublishMoveStats(*host,inst.assets,save.trainer.language,prefix,chosen.moves[i]);
        const auto& timer=live.recharge[selected][i];
        const bool active=timer.valid&&timer.initial>0&&timer.remaining>0;
        const auto fill=active?std::lround(1000.f*(1.f-timer.remaining/timer.initial)):timer.valid?1000:0;
        char seconds[24]={};
        if(active){
            const int tenths=static_cast<int>(std::ceil(timer.remaining*10));
            const char decimal=(save.trainer.language>=3&&save.trainer.language<=7)||
                               save.trainer.language==11?',':'.';
            std::snprintf(seconds,sizeof(seconds),"%d%c%d s",tenths/10,decimal,tenths%10);
        }else if(timer.valid&&live.party_live&&live.hp_valid[selected]&&chosen.hp>0&&chosen.move_pp[i]>0){
            const auto text=UiText(inst.assets,save.trainer.language,"Prête");
            PublishText(*host,(prefix+"recharge_text").c_str(),text);
        }else{
            std::snprintf(seconds,sizeof(seconds),"—");
        }
        if(*seconds) PublishText(*host,(prefix+"recharge_text").c_str(),seconds);
        PublishI64(*host,(prefix+"recharge").c_str(),fill);
        PublishI64(*host,(prefix+"charging").c_str(),active);
    }
    PublishI64(*host,"za.dex.open",dex_open);
    const auto detail_entry=dex_entry(detail);
    const int internal_detail=detail_entry.species;
    PublishI64(*host,"za.detail.number",detail_entry.number-(dex_gen==2?232:0));
    PublishI64(*host,"za.detail.sp",detail);PublishI64(*host,"za.detail.icon",internal_detail*256+detail_entry.form);
    PublishText(*host,"za.detail.name",internal_detail>0 && internal_detail<=species_names.size()?dex_name(detail_entry):"");
    PublishI64(*host,"za.detail.state",save.owned[detail%2048]?3:save.dex_state[detail%2048]);PublishI64(*host,"za.detail.owned",save.owned[detail%2048]);
    const auto dex_info=inst.assets.SpeciesInfo(internal_detail,detail_entry.form);
    PublishI64(*host,"za.detail.type1",dex_info.type1);PublishI64(*host,"za.detail.type2",dex_info.type1==dex_info.type2?-1:dex_info.type2);
    PublishText(*host,"za.detail.height","");PublishText(*host,"za.detail.weight","");
    const auto dex_description=inst.assets.SpeciesDescription(internal_detail);
    PublishText(*host,"za.detail.description",dex_description);PublishI64(*host,"za.detail.lines",std::max(1,int(dex_description.size()/48)+3));
    PublishI64(*host,"za.guide.dex_ready",internal_detail>0);
    const Instance::Objective dex_objective{internal_detail,detail_entry.form,-1,{},dex_gen==3};
    PublishI64(*host,"za.guide.dex_pin",std::ranges::find(objectives,dex_objective)!=objectives.end());
    const auto missions=MissionRows(inst,quest_category,save);
    PublishI64(*host,"za.quest.category",quest_category);
    PublishI64(*host,"za.quest.rows",missions.size());
    PublishI64(*host,"za.quest.active_empty",quest_category==3&&missions.empty());
    PublishI64(*host,"za.quest.show_map",quest_show_map);
    const auto found=std::ranges::find(missions,quest_selected_id,&za::Assets::Mission::id);
    const auto selected_mission=found==missions.end() ? missions.begin() : found;
    const auto quest_window=VisibleRows(*host,"za_quests",missions.size());
    for(int i=quest_window.first;i<quest_window.second;++i){
        const auto row=missions.begin()+i;
        const auto prefix="za.q"+std::to_string(i)+".";
        PublishI64(*host,(prefix+"v").c_str(),1);
        PublishI64(*host,(prefix+"state").c_str(),MissionState(*row,save));
        PublishI64(*host,(prefix+"selected").c_str(),row==selected_mission);
        PublishI64(*host,(prefix+"number").c_str(),row->number);
        PublishText(*host,(prefix+"title").c_str(),row->title);
        if(quest_category==3){
            constexpr const char* categories[]{"Principale","Extra","Secondaire"};
            PublishText(*host,(prefix+"category").c_str(),UiText(inst.assets,save.trainer.language,categories[row->category]));
        }
    }
    PublishI64(*host,"za.quest.detail.open",selected_mission!=missions.end());
    PublishI64(*host,"za.quest.detail.open.scroll",selected_mission!=missions.end()&&!nav_menu);
    bool has_pin=false;
    bool active_mission=false;
    if(selected_mission!=missions.end()){
        const auto& mission=*selected_mission;
        const int state=MissionState(mission,save);
        active_mission=MissionOnMap(state);
        PublishMissionRows(*host,inst.assets,save.trainer.language,"za.quest.body",mission,46,true);
        PublishText(*host,"za.quest.detail.title",mission.title);
        PublishI64(*host,"za.quest.detail.row",quest_category*1024+selected_mission-missions.begin()+1);
        PublishI64(*host,"za.quest.detail.state",state);
        PublishI64(*host,"za.quest.detail.number",mission.number);
        has_pin=MissionOnMap(state) && std::ranges::any_of(live.quest_pins,[&](const auto& pin) {
            return pin.category==mission.category && pin.id==mission.map_id;
        });
    }
    PublishI64(*host,"za.quest.detail.pin",has_pin);
    const bool needs_map=!live.quest_map_loaded;
    const bool can_open_map=needs_map&&guest_helper_idle&&live.scene_idle&&live.position_live&&
        live.area=="t1"&&(live.map_mode<0||live.map_mode>=3);
    PublishI64(*host,"za.map.quest_load",can_open_map);
    PublishI64(*host,"za.quest.detail.load",active_mission&&!has_pin&&can_open_map);
    PublishI64(*host,"za.quest.detail.no_location",selected_mission!=missions.end()&&!has_pin&&
        !(active_mission&&can_open_map));
    PublishText(*host,"za.guide.no_quest_location",UiText(inst.assets,save.trainer.language,
        active_mission&&needs_map?"Ouvre la carte du jeu (+)":"EMPLACEMENT INDISPONIBLE"));
    PublishI64(*host,"za.guide.quest_pin",selected_mission!=missions.end() &&
        std::ranges::find(objectives,Instance::Objective{0,0,selected_mission->category,selected_mission->id})!=objectives.end());
    PublishI64(*host, "za.assets", names ? 1 : 0);
    PublishI64(*host, "za.ready", save.valid ? 1 : 0);
    PublishI64(*host, "za.hash", save.hash_ok ? 1 : 0);
    PublishText(*host, "za.status", UiText(inst.assets,save.trainer.language,status));

    PublishText(*host, "za.ot", save.trainer.name);
    PublishI64(*host, "za.id", static_cast<s64>(save.trainer.id32));
    PublishI64(*host, "za.tid", save.trainer.tid);
    PublishI64(*host, "za.sid", save.trainer.sid);
    PublishI64(*host, "za.gender", save.trainer.gender);
    PublishI64(*host, "za.lang", save.trainer.language);
    PublishI64(*host, "za.money", static_cast<s64>(save.money));
    PublishI64(*host, "za.play", static_cast<s64>(save.play_seconds));
    PublishI64(*host, "za.play.h", static_cast<s64>(save.play_seconds / 3600));
    PublishI64(*host, "za.play.m", static_cast<s64>((save.play_seconds / 60) % 60));
    // The save's clock: seconds past midnight, and the same split into hours and minutes so the
    // page can print "14:07" with two padded values.
    PublishI64(*host, "za.day", static_cast<s64>(save.day_seconds));
    PublishI64(*host, "za.hour", static_cast<s64>(save.day_seconds) / 3600);
    PublishI64(*host, "za.minute", (static_cast<s64>(save.day_seconds) / 60) % 60);

    PublishI64(*host, "za.party.count", save.party_count);
    for (int i = 0; i < static_cast<int>(za::kPartySlots); ++i)
        PublishSlot(*host, i, save.party[i], species_names);

    PublishI64(*host, "za.dex.caught", save.dex_caught);
    PublishI64(*host, "za.dex.seen", save.dex_seen);
    PublishI64(*host, "za.dex.known", save.dex_caught + save.dex_seen);
    PublishI64(*host, "za.dex.total", 1025);
    {
        // Per-generation caught counts and their National Dex ceilings, so the Pokédex page can
        // draw one bar per generation with the plain `value` widget (max_bind + max_sep).
        static constexpr int kGenSize[9] = {151, 100, 135, 107, 156, 72, 88, 96, 120};
        char key[24];
        for (int gen = 0; gen < 9; ++gen) {
            std::snprintf(key, sizeof(key), "za.dex.g%d", gen);
            PublishI64(*host, key, save.dex_caught_by_gen[gen]);
            std::snprintf(key, sizeof(key), "za.dex.g%d.max", gen);
            PublishI64(*host, key, kGenSize[gen]);
        }
    }


    const int dex_pages = std::max(1, static_cast<int>((dex_rows.size() + kDexPageSize-1) / kDexPageSize));
    dex_page = std::clamp(dex_page, 0, dex_pages - 1);
    PublishI64(*host,"za.dex.reset",dex_gen*8+dex_filter);
    const auto followed=std::ranges::find(dex_rows,detail);
    PublishI64(*host,"za.dex.follow",followed==dex_rows.end()?-1:followed-dex_rows.begin());
    PublishI64(*host, "za.dex.gen", dex_gen);
    PublishI64(*host, "za.dex.filter", dex_filter);
    PublishI64(*host, "za.dex.rows", dex_rows.size());
    PublishI64(*host, "za.dex.page", dex_page + 1);
    PublishI64(*host, "za.dex.pages", dex_pages);
    const auto dex_window=VisibleRows(*host,"za_dex",dex_rows.size());
    for (int i=dex_window.first;i<dex_window.second;++i) {
        const auto row = static_cast<size_t>(i);
        const int sp = row < dex_rows.size() ? dex_rows[row] : 0;
        const auto entry=dex_entry(sp);const int internal=entry.species;
        char key[48];
        auto point = [&](const char* suffix) { std::snprintf(key,sizeof(key),"za.d%d.%s",i,suffix);return key; };
        PublishI64(*host,point("v"),sp ? 1 : 0);
        PublishI64(*host,point("sp"),sp);
        PublishI64(*host,point("icon"),internal * 256+entry.form);
        PublishI64(*host,point("number"),entry.number-(dex_gen==2?232:0));
        PublishI64(*host,point("selected"),sp==detail);
        PublishI64(*host,point("owned"),save.owned[sp%2048]);
        PublishI64(*host,point("state"),save.owned[sp%2048] ? 3 : save.dex_state[sp%2048]);
        PublishI64(*host,point("available"),available && internal < static_cast<int>(available->size()) && (*available)[internal]);
        const auto display = internal > 0 && internal <= static_cast<int>(species_names.size())
            ? dex_name(entry) : "Pokemon " + std::to_string(sp);
        PublishText(*host,point("name"),sp ? display : "");
    }
    const auto bag_rows = BagRows(save, bag_category);
    const int bag_pages = std::max(1, static_cast<int>((bag_rows.size() + kBagPageSize-1) / kBagPageSize));
    bag_page = std::clamp(bag_page, 0, bag_pages-1);
    PublishI64(*host,"za.bag.total",save.items.size());
    PublishI64(*host,"za.bag.rows",bag_rows.size());
    PublishI64(*host,"za.bag.category",bag_category + 1);
    PublishI64(*host,"za.bag.page",bag_page + 1);
    PublishI64(*host,"za.bag.pages",bag_pages);
    PublishI64(*host,"za.bag.reset",bag_category+1);
    auto picked=std::find_if(bag_rows.begin(),bag_rows.end(),[&](const auto& e){return e.id==bag_selected;});
    if(picked==bag_rows.end()&&!bag_rows.empty())picked=bag_rows.begin();
    auto picked_item=picked!=bag_rows.end()?*picked:za::Save::Item{};
    if(bag_guide_item) picked_item={za::u16(bag_guide_item),ItemCount(save,bag_guide_item),0,0};
    PublishI64(*host,"za.bag.guide",bag_return_evolution);
    const auto picked_usage=inst.assets.ItemUsageInfo(picked_item.id);
    PublishI64(*host,"za.item.hold",picked_item.count&&picked_usage.holdable);
    const int primary_tool=TMMove(picked_item.id)?3:!inst.assets.EvolutionSourcesForItem(picked_item.id).empty()?4:
        picked_item.count&&picked_usage.IsMedicine()?1:0;
    PublishI64(*host,"za.item.primary_tool",primary_tool);
    {
        std::scoped_lock lock{inst.mutex};
        const bool pinned=std::ranges::find(inst.bag_favorites,picked_item.id)!=inst.bag_favorites.end()&&picked_item.id;
        const bool room=std::ranges::find(inst.bag_favorites,0)!=inst.bag_favorites.end();
        PublishI64(*host,"za.bag.fav_add",picked_item.count&&!pinned&&room);
        PublishI64(*host,"za.bag.fav_remove",pinned);
        PublishI64(*host,"za.bag.fav_full",picked_item.count&&!pinned&&!room);
        PublishI64(*host,"za.bag.held_context",inst.bag_held_target.has_value());
        for(std::size_t i=0;i<inst.bag_favorites.size();++i){
            const auto suffix=std::to_string(i);
            const auto prefix="za.bf"+suffix+".";
            const int id=inst.bag_favorites[i];
            const auto& pending=inst.bag_favorite_writes[i];
            PublishI64(*host,(prefix+"id").c_str(),id);
            PublishI64(*host,(prefix+"count").c_str(),id?ItemCount(save,id):0);
            PublishText(*host,(prefix+"name").c_str(),id?inst.assets.ItemName(id):"");
            PublishI64(*host,(prefix+"pending").c_str(),pending.has_value());
            if(pending) PublishI64(*host,(prefix+"value").c_str(),*pending);
        }
    }
    const int tm_move=TMMove(picked_item.id);
    auto picked_name=inst.assets.ItemName(picked_item.id);
    if(tm_move)picked_name+=" - "+inst.assets.MoveName(tm_move);
    PublishText(*host,"za.item.name",picked_name);
    PublishI64(*host,"za.item.id",picked_item.id);PublishI64(*host,"za.item.count",picked_item.count);
    PublishI64(*host,"za.item.type",tm_move?inst.assets.MoveType(tm_move):-1);
    const auto description=tm_move?inst.assets.MoveDescription(tm_move):inst.assets.ItemDescription(picked_item.id);
    PublishText(*host,"za.item.description",description.empty()?UiText(inst.assets,save.trainer.language,"Description indisponible dans les ressources chargées."):description);
    PublishI64(*host,"za.item.lines",std::max(1,int(description.size()/44)+3));
    const auto bag_window=VisibleRows(*host,"za_bag",bag_rows.size());
    for (int i=bag_window.first;i<bag_window.second;++i) {
        const auto row=static_cast<size_t>(i);
        const za::Save::Item item=bag_rows[row];
        char key[48];
        auto point=[&](const char* suffix){std::snprintf(key,sizeof(key),"za.i%d.%s",i,suffix);return key;};
        PublishI64(*host,point("v"),item.id ? 1 : 0);
        PublishI64(*host,point("id"),item.id);
        PublishI64(*host,point("count"),item.count);
        PublishI64(*host,point("pouch"),item.pouch);
        PublishI64(*host,point("favorite"),(item.flags & 2) != 0);
        PublishI64(*host,point("held"),(item.flags & 16) != 0);
        std::string display;
        if (item_names && item.id < item_names->size()) display=(*item_names)[item.id];
        if (display.empty() && item.id) display="Objet " + std::to_string(item.id);
        if(const int move=TMMove(item.id))display+=" - "+inst.assets.MoveName(move);
        PublishI64(*host,point("selected"),item.id==picked_item.id);
        PublishText(*host,point("name"),display);
    }

    PublishI64(*host, "za.box.used", save.box_used);
    PublishI64(*host, "za.box.slots", static_cast<s64>(za::kBoxCount * za::kBoxSlotCount));
    PublishI64(*host, "za.box.unlocked", save.boxes_unlocked);
    PublishI64(*host, "za.box.current", save.current_box);
    PublishI64(*host, "za.box.current1", save.current_box + 1);
    PublishI64(*host, "za.box.count", save.boxes_available);
    PublishI64(*host, "za.box.view", save.box_view_index);
    PublishI64(*host, "za.box.view1", save.box_view_index + 1);
    PublishI64(*host, "za.box.view.used", save.boxes_used[save.box_view_index]);
    PublishI64(*host, "za.box.follow", box_follow ? 1 : 0);
    PublishI64(*host, "za.box.picker", box_picker);
    if(current_page=="box"){
        for(int i=0;i<save.boxes_available;++i){
            const auto prefix="za.box.tile"+std::to_string(i)+".";
            PublishI64(*host,(prefix+"number").c_str(),i+1);
            PublishI64(*host,(prefix+"selected").c_str(),box_picker&&i==save.box_view_index);
        }
    }
    for (int i = 0; i < static_cast<int>(za::kBoxSlotCount); ++i)
        PublishEntity(*host, "za.b", i, save.box_view[i], species_names);

    PublishText(*host, "za.map", save.map_name);
    PublishText(*host, "za.street", save.street);
    PublishF64(*host, "za.x", save.x);
    PublishF64(*host, "za.y", save.y);
    PublishF64(*host, "za.z", save.z);
    PublishF64(*host, "za.rot", save.rotation);
    // Whole metres, for the `value` widget (which shows integers only).
    PublishI64(*host, "za.xi", static_cast<s64>(std::lround(save.x)));
    PublishI64(*host, "za.yi", static_cast<s64>(std::lround(save.y)));
    PublishI64(*host, "za.zi", static_cast<s64>(std::lround(save.z)));

    auto zones=inst.assets.Zones();auto centers=inst.assets.Centers();
    if(current_page=="capture") PublishCapturePlan(inst,*host,save,objectives,zones);
    MapView view;
    {
        std::scoped_lock lock{inst.mutex};
        const bool wide=current_page=="map"
            ? !(map_help||map_zone>0||map_center>=0||map_cafe>=0||map_landmark>=0||map_quest||
                map_filter_menu||!map_dimension.empty()||!map_choices.empty())
            : inst.map_view.wide;
        const bool geometry_changed=wide!=inst.map_view.wide;
        if(geometry_changed&&inst.map_sampled_reset==map_reset) {
            const auto previous=inst.map_view;
            inst.map_home_zoom=map_home_zoom=previous.zoom;
            inst.map_home_x=map_home_x=(previous.x+320/previous.zoom)/640;
            inst.map_home_y=map_home_y=(previous.y+320/previous.zoom)/640;
            map_reset=++inst.map_reset;
        }
        if(geometry_changed)
            view={map_home_zoom,std::max(0.f,640*map_home_x-320/map_home_zoom),
                                std::max(0.f,640*map_home_y-320/map_home_zoom),wide};
        else view=ReadMapView(*host,map_home_zoom,map_home_x,map_home_y,wide);
        inst.map_view=view;
        inst.map_sampled_reset=map_reset;
    }
    PublishI64(*host,"za.map.wide",view.wide);
    unsigned battle_mask=0;
    if(live.battle_zones_live)for(const auto& zone:inst.assets.BattleZones())
        if(zone.texture>=20&&zone.texture<32&&std::find(live.active_battle_zones.begin(),live.active_battle_zones.end(),zone.active_hash)!=live.active_battle_zones.end())battle_mask|=1u<<(zone.texture-20);
    PublishI64(*host,"za.map.image",(map_filters&1)|((map_filters&2)?(battle_mask<<2)|2:0));
    PublishI64(*host,"za.map.active_battles",std::popcount(battle_mask));
    PublishF64(*host,"za.map.home.zoom",map_home_zoom);
    PublishF64(*host,"za.map.home.x",map_home_x);
    PublishF64(*host,"za.map.home.y",map_home_y);
    PublishI64(*host,"za.map.home.reset",map_reset);
    PublishI64(*host,"za.map.percent",std::lround(view.zoom*100));
    PublishI64(*host,"za.map.wild",bool(map_filters&1));
    PublishI64(*host,"za.map.battle",bool(map_filters&2));
    PublishI64(*host,"za.map.centers",bool(map_filters&4));
    PublishI64(*host,"za.map.zoomed",view.zoom>1.001f);
    PublishI64(*host,"za.map.cafes",bool(map_filters&8));
    PublishI64(*host,"za.map.dim_wild",bool(map_filters&16));PublishI64(*host,"za.map.dim_battle",bool(map_filters&32));
    PublishI64(*host,"za.map.dimensions_live",live.dimensions_live);
    PublishI64(*host,"za.map.filters_open",map_filter_menu);
    const auto map_x=[&](float u){return (u-view.x)*view.zoom;};
    const auto map_y=[&](float v){return (v-view.y)*view.zoom;};
    const auto canvas_x=[&](float u){return view.Left()+u*view.Scale();};
    const auto canvas_y=[&](float v){return view.Top()+v*view.Scale();};
    const auto visible=[](float u,float v){return u>=20&&u<=620&&v>=20&&v<=620;};
    std::vector<Instance::MapPin> map_pins;
    const auto record_pin=[&](int type,int index,float u,float v,const std::string& binding,
                              const std::string& name,int key=0,const std::string& identity={}) {
        if(current_page!="map") return;
        map_pins.push_back({type,index,key,canvas_x(u),canvas_y(v),binding,name,identity});
    };
    PublishI64(*host, "za.map.quests", bool(map_filters & 64));
    PublishI64(*host,"za.map.dim_unknown",bool(map_filters&128));
    PublishI64(*host,"za.map.landmarks",bool(map_filters&256));
    PublishI64(*host,"za.map.mark.placing",placing_marker);
    PublishI64(*host,"za.map.help",map_help);
    PublishI64(*host,"za.map.mark.start_compact",!view.wide&&!placing_marker);
    PublishI64(*host,"za.map.mark.start_wide",view.wide&&!placing_marker);
    PublishI64(*host,"za.map.mark.cancel_wide",view.wide&&placing_marker);
    const bool marker_city=guest_helper_alive&&live.position_live&&live.area=="t1";
    PublishI64(*host,"za.map.mark.can_start",marker_city&&!marker_pending);
    const char* marker_notice=marker_pending?(clearing_marker?"Effacement en cours…":"Pose en cours…"):
        placing_marker?"Touche la carte":marker_result;
    PublishI64(*host,"za.map.mark.notice",*marker_notice!=0);
    PublishText(*host,"za.map.mark.message",UiText(inst.assets,save.trainer.language,marker_notice));
    marker_u=live.map_marker.known&&live.map_marker.visible?(2048.f-live.map_marker.x)*640.f/4096.f:-1;
    marker_v=live.map_marker.known&&live.map_marker.visible?(2048.f+live.map_marker.y)*640.f/4096.f:-1;
    PublishI64(*host,"za.map.mark.set",marker_u>=0);
    PublishI64(*host,"za.map.mark.clear_wide",view.wide&&marker_u>=0);
    const float mark_x=map_x(marker_u),mark_y=map_y(marker_v);
    PublishI64(*host,"za.map.mark.visible",marker_u>=0&&visible(mark_x,mark_y));
    PublishF64(*host,"za.map.mark.x",canvas_x(mark_x));
    PublishF64(*host,"za.map.mark.y",canvas_y(mark_y));
    std::array<int, 128> quest_targets{};
    std::size_t quest_count = 0;
    int quest_category_index = -1;
    za::Assets::Mission map_mission;
    if ((map_filters & 64) && !live.quest_pins.empty()) {
        const auto main_missions = inst.assets.Missions(0);
        const auto side_missions = inst.assets.Missions(2);
        for (const auto& pin : live.quest_pins) {
            const auto& rows = pin.category == 0 ? main_missions : side_missions;
            const auto mission = std::ranges::find(rows, pin.id, &za::Assets::Mission::map_id);
            if (mission == rows.end() || !MissionOnMap(MissionState(*mission,save))) continue;
            const int key = pin.category * 1024 + pin.id;
            if (map_quest == key) {
                map_mission = *mission;
                quest_category_index = pin.category;
            }
            const float u = map_x((2048.f - pin.x) * 640.f / 4096.f);
            const float v = map_y((2048.f + pin.y) * 640.f / 4096.f);
            if (!visible(u,v) || quest_count >= quest_targets.size()) continue;
            const auto prefix = "za.mq" + std::to_string(quest_count) + ".";
            PublishI64(*host,(prefix+"v").c_str(),1);
            PublishI64(*host,(prefix+"shown").c_str(),1);
            PublishI64(*host,(prefix+"side").c_str(),pin.category == 2);
            PublishF64(*host,(prefix+"x").c_str(),canvas_x(u));
            PublishF64(*host,(prefix+"y").c_str(),canvas_y(v));
            const bool selected=map_quest==key;
            PublishI64(*host,(prefix+"selected").c_str(),selected);
            if(!selected) record_pin(4,int(quest_count),u,v,prefix,mission->title,key);
            quest_targets[quest_count++] = key;
        }
    }
    PublishI64(*host,"za.map.quest.pin_count",quest_count);
    {
        std::scoped_lock lock{inst.mutex};
        inst.map_quest_targets = quest_targets;
    }
    const bool quest_open_on_map = !map_mission.id.empty() && !map_filter_menu;
    PublishI64(*host,"za.map.quest.open",quest_open_on_map);
    PublishI64(*host,"za.map.quest.open.scroll",quest_open_on_map&&!nav_menu);
    PublishI64(*host,"za.map.quest.key",quest_open_on_map ? map_quest : 0);
    PublishI64(*host,"za.map.quest.side",quest_category_index == 2);
    if (quest_open_on_map) {
        PublishI64(*host,"za.map.quest.number",map_mission.number);
        PublishText(*host,"za.map.quest.title",map_mission.title);
        PublishMissionRows(*host,inst.assets,save.trainer.language,"za.map.body",map_mission,24);
    }
    const za::Dimension* picked_dimension=nullptr;int picked_dimension_index=-1;
    for(int i=0;i<50;++i){const auto p="za.dim"+std::to_string(i)+".";float u=0,v=0;
        const bool exists=i<live.dimensions.size()&&(map_filters&DimensionFilter(live.dimensions[i].kind))&&
            za::MapPoint("t1",live.dimensions[i].x,live.dimensions[i].z,u,v);
        if(i<live.dimensions.size()&&live.dimensions[i].gate==map_dimension&&exists){picked_dimension=&live.dimensions[i];picked_dimension_index=i;}
        u=map_x(u);v=map_y(v);PublishI64(*host,(p+"v").c_str(),exists&&visible(u,v));
        PublishI64(*host,(p+"shown").c_str(),exists&&visible(u,v));
        PublishI64(*host,(p+"kind").c_str(),exists?std::min(2,live.dimensions[i].kind):0);
        PublishI64(*host,(p+"selected").c_str(),exists&&live.dimensions[i].gate==map_dimension);
        PublishF64(*host,(p+"x").c_str(),canvas_x(u));PublishF64(*host,(p+"y").c_str(),canvas_y(v));
        if(current_page=="map"&&exists&&visible(u,v)){
            const auto& dimension=live.dimensions[i];
            const char* title=dimension.kind==0?"Dimension sauvage active":
                              dimension.kind==1?"DIMENSION DE COMBAT":"DIMENSION INCONNUE";
            record_pin(3,i,u,v,p,UiText(inst.assets,save.trainer.language,title),0,dimension.gate);
        }
    }
    for(int i=0;i<20;++i){auto prefix="za.zone"+std::to_string(i)+".";const bool exists=i<zones.size()&&(map_filters&1);
        const float x=exists?map_x((2048+zones[i].x)*640/4096):0;
        const float y=exists?map_y((2048-zones[i].y)*640/4096):0;
        PublishI64(*host,(prefix+"v").c_str(),exists&&visible(x,y));PublishI64(*host,(prefix+"number").c_str(),exists?zones[i].number:0);
        PublishI64(*host,(prefix+"shown").c_str(),exists&&visible(x,y));
        PublishF64(*host,(prefix+"x").c_str(),canvas_x(x));
        PublishF64(*host,(prefix+"y").c_str(),canvas_y(y));
        if(current_page=="map"&&exists&&visible(x,y))
            record_pin(0,i,x,y,prefix,IndexedPoint(UiText(inst.assets,save.trainer.language,"ZONE SAUVAGE {i}"),zones[i].number));
    }
    for(int i=0;i<12;++i){auto prefix="za.center"+std::to_string(i)+".";const bool exists=i<centers.size()&&(map_filters&4);float u=0,v=0;
        if(exists)za::MapPoint("t1",centers[i].x,centers[i].z,u,v);
        u=map_x(u);v=map_y(v);
        PublishI64(*host,(prefix+"v").c_str(),exists&&visible(u,v));PublishI64(*host,(prefix+"destination").c_str(),exists?centers[i].destination:0);PublishF64(*host,(prefix+"x").c_str(),canvas_x(u));PublishF64(*host,(prefix+"y").c_str(),canvas_y(v));
        PublishI64(*host,(prefix+"shown").c_str(),exists&&visible(u,v));
        if(exists&&visible(u,v)) record_pin(1,i,u,v,prefix,centers[i].name);
    }
    const auto cafes=inst.assets.Cafes();
    for(int i=0;i<24;++i){
        const auto prefix="za.cafe"+std::to_string(i)+".";
        float u=0,v=0;
        const bool exists=(map_filters&8)&&i<cafes.size()&&za::MapPoint("t1",cafes[i].x,cafes[i].z,u,v);
        u=map_x(u);v=map_y(v);
        PublishI64(*host,(prefix+"v").c_str(),exists&&visible(u,v));
        PublishI64(*host,(prefix+"shown").c_str(),exists&&visible(u,v));
        PublishF64(*host,(prefix+"x").c_str(),canvas_x(u));
        PublishF64(*host,(prefix+"y").c_str(),canvas_y(v));
        if(exists&&visible(u,v)) record_pin(2,i,u,v,prefix,cafes[i].name);
    }
    const auto landmarks=inst.assets.Landmarks();
    const za::Assets::Center* landmark=nullptr;
    for(int i=0;i<32;++i){
        const auto key="za.landmark"+std::to_string(i)+".";
        const auto pin=i<landmarks.size()?std::ranges::find(live.landmark_pins,
            landmarks[i].travel.points[0].row,&za::LandmarkPin::row):live.landmark_pins.end();
        const bool exists=(map_filters&256)&&pin!=live.landmark_pins.end();
        const float u=exists?map_x((2048.f-pin->x)*640.f/4096.f):0;
        const float v=exists?map_y((2048.f+pin->y)*640.f/4096.f):0;
        PublishI64(*host,(key+"v").c_str(),exists&&visible(u,v));
        PublishI64(*host,(key+"shown").c_str(),exists&&visible(u,v));
        PublishF64(*host,(key+"x").c_str(),canvas_x(u));PublishF64(*host,(key+"y").c_str(),canvas_y(v));
        if(exists&&visible(u,v)) record_pin(5,i,u,v,key,landmarks[i].name);
        if(exists&&map_landmark==i)landmark=&landmarks[i];
    }
    if(current_page=="map"){
        std::vector<std::vector<Instance::MapPin>> groups;
        const float detail=std::clamp((view.zoom-4.f)/(kMapMaxZoom-4.f),0.f,1.f);
        const float group_radius=std::lerp(48.f,40.f,detail);
        while(!map_pins.empty()){
            std::vector<Instance::MapPin> nearby;
            nearby.push_back(std::move(map_pins.back()));
            map_pins.pop_back();
            for(std::size_t i=0;i<nearby.size();++i) {
                for(auto it=map_pins.begin();it!=map_pins.end();) {
                    const float dx=nearby[i].x-it->x,dy=nearby[i].y-it->y;
                    if(dx*dx+dy*dy<group_radius*group_radius) {
                        nearby.push_back(std::move(*it));
                        it=map_pins.erase(it);
                    } else ++it;
                }
            }
            if(nearby.size()<2) continue;
            float x=0,y=0;
            for(const auto& pin:nearby) {
                x+=pin.x;
                y+=pin.y;
                PublishI64(*host,(pin.binding+"shown").c_str(),0);
            }
            const auto prefix="za.mg"+std::to_string(groups.size())+".";
            PublishF64(*host,(prefix+"x").c_str(),x/nearby.size());
            PublishF64(*host,(prefix+"y").c_str(),y/nearby.size());
            PublishI64(*host,(prefix+"count").c_str(),nearby.size());
            groups.push_back(std::move(nearby));
        }
        PublishI64(*host,"za.map.groups",groups.size());
        PublishI64(*host,"za.map.choices",map_choices.size());
        PublishI64(*host,"za.map.choices.scroll",!map_choices.empty()&&!nav_menu);
        PublishI64(*host,"za.map.choices_reset",map_choices_revision);
        for(std::size_t i=0;i<map_choices.size();++i){
            const auto prefix="za.mc"+std::to_string(i)+".";
            const auto& pin=map_choices[i];
            PublishI64(*host,(prefix+"type").c_str(),pin.type);
            PublishText(*host,(prefix+"name").c_str(),pin.name);
        }
        std::scoped_lock lock{inst.mutex};
        inst.map_groups=std::move(groups);
    }
    const bool cafe_open=map_cafe>=0&&map_cafe<cafes.size();
    const bool center_open=map_center>=0&&map_center<centers.size();
    const bool zone_open=map_zone>0&&!center_open&&!cafe_open&&!landmark&&!picked_dimension&&!map_filter_menu&&!quest_open_on_map;
    const auto selected_zone=std::ranges::find(zones,map_zone,&za::Assets::Zone::number);
    const bool can_travel=guest_helper_alive&&live.scene_idle&&live.position_live&&live.area=="t1";
    const bool place_travel=(landmark&&TravelUnlocked(landmark->travel,live))||
        (center_open&&TravelUnlocked(centers[map_center].travel,live))||
        (cafe_open&&TravelUnlocked(cafes[map_cafe].travel,live));
    const bool zone_travel=zone_open&&selected_zone!=zones.end()&&TravelUnlocked(selected_zone->travel,live);
    PublishI64(*host,"za.map.travel_ready",can_travel&&(place_travel||zone_travel)&&guest_helper_idle);
    PublishText(*host,"za.map.travel_caption",UiText(inst.assets,save.trainer.language,"VOYAGE RAPIDE"));
    const char* travel_message = "";
    switch(travel_result) {
    case za::GuestMailbox::TravelResult::Confirmation: travel_message="Confirmez dans le jeu."; break;
    case za::GuestMailbox::TravelResult::Expired: travel_message="Voyage indisponible pour le moment."; break;
    case za::GuestMailbox::TravelResult::Unavailable: travel_message="Destination indisponible."; break;
    default: break;
    }
    PublishText(*host,"za.map.travel_status",(place_travel||zone_travel)?
        UiText(inst.assets,save.trainer.language,travel_message):"");
    PublishI64(*host,"za.center.open",center_open||cafe_open||landmark||picked_dimension);
    PublishI64(*host,"za.center.basic",(center_open||cafe_open||landmark)&&!picked_dimension);
    PublishI64(*host,"za.center.cafe",landmark?2:cafe_open?1:0);
    PublishI64(*host,"za.dimension.open",picked_dimension!=nullptr);
    PublishI64(*host,"za.dimension.open.scroll",picked_dimension&&!nav_menu);
    PublishI64(*host,"za.dimension.type",picked_dimension?picked_dimension->type:-1);
    PublishI64(*host,"za.dimension.selected",picked_dimension_index+1);
    za::Assets::DimensionInfo dimension_info;
    std::string dimension_stars,dimension_stats;
    if(picked_dimension&&picked_dimension->kind<2){
        dimension_info=inst.assets.DimensionDetails(picked_dimension->content,picked_dimension->rank,picked_dimension->types,picked_dimension->patterns,picked_dimension->kind);
        for(int i=0;i<=picked_dimension->rank;++i)dimension_stars+="★";
        dimension_stats=UiText(inst.assets,save.trainer.language,"N.")+" "+(dimension_info.level?std::to_string(dimension_info.level):"—");
        static const char* sizes[]={"Petite taille","Taille moyenne","Grande taille"};
        if(dimension_info.size>=0&&dimension_info.size<3)dimension_stats+=" · "+UiText(inst.assets,save.trainer.language,sizes[dimension_info.size]);
    }
    PublishText(*host,"za.dimension.stars",dimension_stars);PublishText(*host,"za.dimension.stats",dimension_stats);
    PublishI64(*host,"za.dimension.battle",picked_dimension&&picked_dimension->kind==1);
    PublishI64(*host,"za.dimension.wild",picked_dimension&&picked_dimension->kind==0);
    PublishI64(*host,"za.dimension.unknown",picked_dimension&&picked_dimension->kind>=2);
    for(int i=0;i<3;++i)PublishI64(*host,("za.dimension.type"+std::to_string(i)).c_str(),picked_dimension?picked_dimension->types[i]:-1);
    PublishI64(*host,"za.dimension.rows",dimension_info.pokemon.size());
    const auto dimension_window=VisibleRows(*host,"za_dimension",dimension_info.pokemon.size());
    for(int i=dimension_window.first;i<dimension_window.second;++i){const auto& e=dimension_info.pokemon[i];const auto p="za.dp"+std::to_string(i)+".";
        PublishI64(*host,(p+"icon").c_str(),e.species*256+e.form);
        PublishText(*host,(p+"name").c_str(),inst.assets.SpeciesName(e.species));
    }
    std::string poi_name;
    if(landmark)poi_name=landmark->name;
    else if(cafe_open)poi_name=cafes[map_cafe].name;
    else if(picked_dimension)poi_name=UiText(inst.assets,save.trainer.language,picked_dimension->kind>=2?
        "DIMENSION INCONNUE":picked_dimension->kind==1?"DIMENSION DE COMBAT":"DIMENSION SAUVAGE");
    else if(center_open){
        poi_name=centers[map_center].name;
        if(poi_name.empty())poi_name=UiText(inst.assets,save.trainer.language,"CENTRE POKÉMON")+" "+std::to_string(map_center+1);
    }
    PublishText(*host,"za.center.name",poi_name);
    std::string dimension_description;
    if(picked_dimension){float a=0,b=0;const bool in_city=za::MapPoint(save.map_name,save.x,save.z,a,b);
        dimension_description=UiText(inst.assets,save.trainer.language,"Dimension active");
        if(in_city)dimension_description+="\n"+std::to_string(int(std::lround(std::hypot(save.x-picked_dimension->x,save.z-picked_dimension->z))))+" m";
    }
    PublishText(*host,"za.center.description",landmark?landmark->description:picked_dimension?dimension_description:cafe_open?cafes[map_cafe].description:
                center_open?centers[map_center].description:std::string{});
    PublishI64(*host,"za.zone.selected",map_zone);
    PublishI64(*host,"za.zone.list_visible",zone_open);
    PublishI64(*host,"za.zone.list_visible.scroll",zone_open&&!nav_menu);
    std::vector<za::ZoneEncounter> encounters;
    std::set<int> missing;
    for(const auto& e:za::kZoneEncounters) {
        if(e.zone!=map_zone) continue;
        const bool caught=DexCaught(save,e.species);
        if(!caught) missing.insert(e.species);
        if(!map_missing_only||!caught) encounters.push_back(e);
    }
    PublishI64(*host,"za.zone.missing",missing.size());
    PublishI64(*host,"za.zone.missing_only",map_missing_only);
    PublishI64(*host,"za.zone.scroll_reset",map_zone*2+map_missing_only);
    PublishI64(*host,"za.zone.complete",zone_open&&save.valid&&map_missing_only&&missing.empty());
    PublishI64(*host,"za.zone.rows",encounters.size());
    const auto zone_window=VisibleRows(*host,"za_zone",encounters.size());
    for(int i=zone_window.first;i<zone_window.second;++i){const auto& e=encounters[i];const int internal=InternalId(e.species);const auto prefix="za.zp"+std::to_string(i)+".";
        PublishI64(*host,(prefix+"v").c_str(),1);PublishI64(*host,(prefix+"icon").c_str(),internal*256+e.form);
        PublishText(*host,(prefix+"name").c_str(),internal>0&&internal<=species_names.size()?species_names[internal-1]:"");
        PublishText(*host,(prefix+"levels").c_str(),UiText(inst.assets,save.trainer.language,"N.")+" "+std::to_string(e.low)+"-"+std::to_string(e.high)+(e.alpha?" / "+UiText(inst.assets,save.trainer.language,"Baron possible"):""));
        PublishI64(*host,(prefix+"state").c_str(),save.owned[e.species]?3:save.dex_state[e.species]);
    }
    float map_u=0,map_v=0;
    const bool map_marker=live.position_live&&za::MapPoint(live.area,live.x,live.z,map_u,map_v);
    PublishI64(*host,"za.map.recenter",map_marker);
    const bool marker_visible=map_marker&&visible(map_x(map_u),map_y(map_v));
    PublishI64(*host,"za.map.marker",marker_visible&&!heading_valid);
    PublishI64(*host,"za.map.arrow",marker_visible&&heading_valid);
    PublishF64(*host,"za.map.heading",heading);
    PublishF64(*host,"za.nx",map_u/640.0f);PublishF64(*host,"za.ny",map_v/640.0f);
    PublishF64(*host,"za.pmx",canvas_x(map_x(map_u)));PublishF64(*host,"za.pmy",canvas_y(map_y(map_v)));

    if(current_page=="locations" || current_page=="combat" || current_page=="evolutions"){
        const auto name=inst.assets.SpeciesName(guide_target.species);
        PublishText(*host,"za.guide.name",guide_target.mega?UiText(inst.assets,save.trainer.language,"Méga-")+name:name);
        PublishI64(*host,"za.guide.icon",guide_target.species*256+guide_target.form);
    }
    if(current_page=="evolutions"){
        const auto rows=inst.assets.Evolutions(guide_target.species,guide_target.form);
        PublishI64(*host,"za.guide.evolution_rows",rows.size());
        for(std::size_t i=0;i<rows.size();++i) {
            const auto& evo=rows[i];
            const auto prefix="za.ge"+std::to_string(i)+".";
            PublishI64(*host,(prefix+"from_icon").c_str(),evo.source*256+evo.source_form);
            PublishI64(*host,(prefix+"to_icon").c_str(),evo.target*256+evo.target_form);
            PublishText(*host,(prefix+"from").c_str(),inst.assets.SpeciesName(evo.source));
            PublishText(*host,(prefix+"to").c_str(),inst.assets.SpeciesName(evo.target));
            PublishText(*host,(prefix+"condition").c_str(),EvolutionCondition(inst.assets,save.trainer.language,evo));
            const int item=evo.item;
            PublishI64(*host,(prefix+"item").c_str(),item);
            PublishI64(*host,(prefix+"stock").c_str(),item?ItemCount(save,item):0);
        }
    }
    if(current_page=="locations"){
        std::vector<Instance::EncounterLocation> locations;
        const int national=za::NationalSpecies(guide_target.species);
        // Live sightings come first, followed by the permanent wild-zone catalog.
        for(const auto& dimension:live.dimensions){
            if(dimension.kind!=0) continue;
            float u=0,v=0;
            if(!za::MapPoint("t1",dimension.x,dimension.z,u,v)) continue;
            const auto info=inst.assets.DimensionDetails(dimension.content,dimension.rank,dimension.types,dimension.patterns,dimension.kind);
            if(std::ranges::any_of(info.pokemon,[&](const auto& pokemon){return pokemon.species==guide_target.species;}))
                locations.push_back({0,dimension.gate,u,v});
        }
        for(const auto& zone:zones){
            if(std::ranges::any_of(za::kZoneEncounters,[&](const auto& entry){return entry.zone==zone.number&&entry.species==national;}))
                locations.push_back({zone.number,{},(2048+zone.x)*640/4096,(2048-zone.y)*640/4096});
        }
        if(locations.size()>80) locations.resize(80);
        PublishI64(*host,"za.guide.location_rows",locations.size());
        int active_index=0;
        for(std::size_t i=0;i<locations.size();++i){
            const auto& location=locations[i];
            const auto prefix="za.gl"+std::to_string(i)+".";
            const auto label=location.zone ? IndexedPoint(UiText(inst.assets,save.trainer.language,"ZONE SAUVAGE {i}"),location.zone)
                : UiText(inst.assets,save.trainer.language,"Dimension sauvage active")+" "+std::to_string(++active_index);
            PublishText(*host,(prefix+"label").c_str(),label);
            PublishI64(*host,(prefix+"live").c_str(),!location.zone);
        }
        std::scoped_lock lock{inst.mutex};
        if(inst.guide_target==guide_target) inst.guide_locations=std::move(locations);
    }
    if(current_page=="notebook"){
        PublishI64(*host,"za.guide.objective_rows",objectives.size());
        for(std::size_t i=0;i<objectives.size();++i){
            const auto& objective=objectives[i];
            const auto prefix="za.go"+std::to_string(i)+".";
            std::string title=inst.assets.SpeciesName(objective.species);
            if(objective.category>=0){
                const auto catalog=inst.assets.Missions(objective.category);
                const auto mission=std::ranges::find(catalog,objective.mission,&za::Assets::Mission::id);
                title=mission==catalog.end() ? objective.mission : mission->title;
            }
            PublishText(*host,(prefix+"label").c_str(),title);
            PublishI64(*host,(prefix+"icon").c_str(),objective.species*256+objective.form);
            PublishI64(*host,(prefix+"mission").c_str(),objective.category>=0);
        }
    }
    if(current_page=="combat"){
        const auto info=inst.assets.SpeciesInfo(guide_target.species,guide_target.form);
        PublishI64(*host,"za.guide.type1",info.type1);
        PublishI64(*host,"za.guide.type2",info.type1==info.type2?-1:info.type2);
        int weakness_count=0;
        for(int type=0;type<18;++type){
            const int matchup=za::TypeMatchup(type,info.type1,info.type2);
            if(matchup<=4) continue;
            const auto prefix="za.gw"+std::to_string(weakness_count++)+".";
            PublishI64(*host,(prefix+"type").c_str(),type);
            PublishText(*host,(prefix+"factor").c_str(),matchup==16?"×4":"×2");
        }
        PublishI64(*host,"za.guide.weaknesses",weakness_count);
        constexpr const char* multipliers[]={"×0","×¼","×½","","×1","","","","×2","","","","","","","","×4"};
        int ready_count=0,known_count=0;
        for(int slot=0;slot<6;++slot){
            const auto& member=save.party[slot];
            for(int index=0;index<4;++index){
                const int move=member.moves[index];
                const int matchup=move&&inst.assets.MoveParameters(move).power>0
                    ? za::TypeMatchup(inst.assets.MoveType(move),info.type1,info.type2) : -1;
                const auto prefix="za.gc"+std::to_string(slot*4+index)+".";
                const auto& timer=live.recharge[slot][index];
                const bool known=live.party_live&&member.present&&timer.valid&&move&&
                    live.party[slot].moves[index]==move&&live.party[slot].pid==member.pid&&
                    live.party[slot].encryption_constant==member.encryption_constant;
                known_count+=matchup>4&&known;
                const bool ready=matchup>4&&known&&std::isfinite(timer.remaining)&&timer.remaining<=0&&
                    live.hp_valid[slot]&&member.hp>0&&member.move_pp[index]>0;
                ready_count+=ready;
                PublishText(*host,(prefix+"name").c_str(),move?inst.assets.MoveName(move):"—");
                PublishText(*host,(prefix+"factor").c_str(),matchup>=0?multipliers[matchup]:"");
                PublishI64(*host,(prefix+"strong").c_str(),matchup>4);
                PublishI64(*host,(prefix+"ready").c_str(),ready);
            }
        }
        PublishI64(*host,"za.guide.ready_moves",ready_count);
        PublishI64(*host,"za.guide.ready_known",known_count);
    }
    PublishUi(*host,inst.assets,save.trainer.language);
    if (host->end_output)
        host->end_output(host->userdata);
}

void Tick(void* p, const EdenDsmodHostApi* host) {
    if (!p || !host)
        return;
    try {
        auto& inst=*static_cast<Instance*>(p);
        bool inspect_boxes=false;int requested_box=0;u64 marker_map=0;
        {
            std::scoped_lock lock{inst.mutex};
            inspect_boxes=inst.swap_slot>=0||inst.box_swap.Active();requested_box=inst.save.box_view_index;
            if(inst.marker_request&&inst.marker_request->sent) marker_map=inst.guest_mailbox.MarkerMapModel();
        }
        auto live=inst.live_reader.Poll(*host,inspect_boxes,requested_box,marker_map);
        {std::scoped_lock lock{inst.mutex};
            const bool helper_alive = inst.guest_mailbox.Poll(inst.command_tick + 1);
            if (helper_alive && !inst.guest_helper_alive)
                Log(*host, EDEN_DSMOD_LOG_INFO, "ZA: scene mailbox active");
            inst.guest_helper_alive = helper_alive;
            inst.marker_open_pulse=inst.map_close_pulse=false;
            const auto tick=inst.command_tick;
            if(tick>=inst.marker_notice_until) inst.marker_result="";
            if(inst.marker_request){
                using Result=za::GuestMailbox::TravelResult;
                auto& marker=*inst.marker_request;
                const auto result=marker.sent?inst.guest_mailbox.TravelStatus():Result::None;
                const bool in_city=live.position_live&&live.area=="t1";
                if(result==Result::MarkerApplied){
                    if(!marker.close_deadline){
                        marker.close_deadline=tick+100;
                        inst.marker_result=marker.clear?"Repère effacé":"Repère placé";
                        inst.marker_notice_until=tick+240;
                        Log(*host,EDEN_DSMOD_LOG_INFO,marker.clear?"ZA: marker removal confirmed":"ZA: marker placement confirmed");
                    }
                    if(marker.close_attempts&&live.map_mode==3){
                        Log(*host,EDEN_DSMOD_LOG_INFO,"ZA: map closed after marker operation");
                        inst.marker_request.reset();
                    }else if(live.map_mode==2||live.map_mode==4||tick>=marker.close_deadline){
                        inst.marker_request.reset();
                    }else if(live.map_mode==0&&tick>=marker.close_not_before&&marker.close_attempts<2){
                        inst.map_close_pulse=true;
                        ++marker.close_attempts;
                        marker.close_not_before=tick+32;
                    }
                }else if(result!=Result::None||!helper_alive||(!marker.sent&&tick>=marker.deadline)){
                    inst.marker_result=marker.clear?"Effacement à réessayer":"Pose à réessayer";
                    inst.marker_request.reset();
                    inst.marker_notice_until=tick+240;
                    Log(*host,EDEN_DSMOD_LOG_INFO,"ZA: marker placement not confirmed");
                }else if(!marker.sent&&in_city&&live.scene_idle){
                    const bool map_closed=live.map_mode<0||live.map_mode==3;
                    if((map_closed||live.map_mode==0)&&
                       inst.guest_mailbox.RequestMarker(marker.x,marker.y,marker.clear,false)){
                        marker.sent=true;
                        inst.marker_open_pulse=map_closed;
                        if(marker.clear)
                            Log(*host,EDEN_DSMOD_LOG_INFO,map_closed?"ZA: marker removal requested from the field":
                                "ZA: marker removal requested on the open map");
                        else
                            Log(*host,EDEN_DSMOD_LOG_INFO,map_closed?"ZA: marker requested from the field":
                                "ZA: marker requested on the open map");
                    }
                }
            }
            if(inst.bag_request){
                const auto result=inst.guest_mailbox.BagResult(inst.bag_request->sequence);
                if(result!=za::GuestMailbox::OperationResult::Pending){
                    const bool applied=result==za::GuestMailbox::OperationResult::Accepted;
                    const bool held=inst.bag_request->tool==Instance::BagTool::Hold;
                    inst.bag_status=applied?(held?"Objet tenu mis à jour.":"Soin appliqué."):
                        "Le jeu n'a pas confirmé le changement.";
                    Log(*host,EDEN_DSMOD_LOG_INFO,!applied?"ZA: bag action unconfirmed":
                        held?"ZA: held item and stock confirmed":"ZA: medicine and stock confirmed");
                    inst.bag_request.reset();
                }
            }
            for(int i=0;i<6;++i)if(live.hp_valid[i]){const auto& e=live.party[i];inst.confirmed_combat[(std::uint64_t(e.pid)<<32)|e.encryption_constant]=e;}
            inst.command_left=inst.command_right=inst.command_send=false;++inst.command_tick;
            float map_u=0,map_v=0;
            if(live.position_live&&za::MapPoint(live.area,live.x,live.z,map_u,map_v)){
                const float dx=map_u-inst.heading_x,dy=map_v-inst.heading_y;
                const float distance=std::hypot(dx,dy);
                if(!inst.heading_origin||distance>40){
                    inst.heading_origin=true;inst.heading_valid=false;
                    inst.heading_x=map_u;inst.heading_y=map_v;
                }else if(distance>=.6f){
                    inst.heading=std::atan2(dx,-dy)*57.2957795f;
                    inst.heading_valid=true;inst.heading_x=map_u;inst.heading_y=map_v;
                }
            }else if(live.position_live){
                inst.heading_origin=inst.heading_valid=false;
            }
            inst.aim_pulse=false;inst.attack_pulse=-1;
            if(inst.attack_slot>=0){
                const int slot=inst.attack_slot;
                const auto& entity=live.party[slot];
                const auto identity=(u64(entity.pid)<<32)|entity.encryption_constant;
                const auto* page=host->get_text?host->get_text(host->userdata,"__page"):nullptr;
                if(inst.command_tick>inst.attack_deadline||!live.party_valid||
                   (page&&std::strcmp(page,"team")!=0)||
                   !entity.present||identity!=inst.attack_identity||
                   entity.moves[inst.attack_move]!=inst.attack_id||
                   (live.hp_valid[slot]&&entity.hp==0)){
                    inst.attack_slot=-1;
                }else if(inst.attack_phase==Instance::AttackPhase::Select){
                    if(live.selected_index>=0&&inst.command_tick>=inst.attack_next_select){
                        if(live.selected_index!=slot){
                            inst.command_right=live.selected_index<slot;
                            inst.command_left=!inst.command_right;
                            inst.attack_next_select=inst.command_tick+18;
                        }else if(live.deployed_index==slot){
                            inst.attack_phase=Instance::AttackPhase::Aim;
                            inst.aim_pulse=true;
                            inst.attack_due=inst.command_tick+11;
                            inst.attack_busy_until=inst.command_tick+26;
                        }else if(live.deployed_index>=-1){
                            inst.command_send=true;
                            inst.attack_phase=Instance::AttackPhase::Deploy;
                            // The native field actor is checked again before aiming.
                            inst.attack_due=0;
                        }
                    }
                }else if(live.selected_index!=slot){
                    inst.attack_slot=-1;
                }else if(inst.attack_phase==Instance::AttackPhase::Deploy){
                    if(live.deployed_index!=slot){
                        inst.attack_due=0;
                    }else if(inst.attack_due==0){
                        // The actor flag precedes the end of the send-out animation.
                        inst.attack_due=inst.command_tick+45;
                    }else if(inst.command_tick>=inst.attack_due){
                        inst.attack_phase=Instance::AttackPhase::Aim;
                        inst.aim_pulse=true;
                        inst.attack_due=inst.command_tick+11;
                        inst.attack_busy_until=inst.command_tick+26;
                    }
                }else if(live.deployed_index!=slot){
                    inst.attack_slot=-1;
                }else if(inst.command_tick>=inst.attack_due){
                    inst.attack_pulse=inst.attack_move;
                    inst.attack_slot=-1;
                }
            }
            inst.swap_commands.fill(false);
            if(inst.box_swap.Active()){
                const auto command=inst.box_swap.Poll(inst.command_tick,live);
                if(command!=za::BoxSwapController::Command::None)inst.swap_commands[int(command)-1]=true;
                if(inst.box_swap.Completed())inst.swap_status="Échange confirmé dans le jeu";
            }
            if(inst.send_target>=0){
                if(inst.command_tick>inst.command_deadline){inst.send_target=-1;inst.send_status="Sélection impossible : fermez les menus du jeu.";}
                else if(inst.command_tick>=inst.command_next&&live.selected_index>=0){
                    if(live.selected_index==inst.send_target){inst.command_send=true;inst.send_target=-1;inst.send_status="Commande envoyée au jeu";}
                    else {inst.command_right=live.selected_index<inst.send_target;inst.command_left=!inst.command_right;}
                    inst.command_next=inst.command_tick+18;
                }
            }
            inst.map_commands.fill(false);
            const auto travel=inst.travel.Poll(inst.command_tick,live.map_mode,live.map_destination);
            if(inst.travel.Active()&&(inst.command_tick%120==0||travel!=za::TravelController::Command::None)){char trace[160];std::snprintf(trace,sizeof(trace),"ZA travel: mode=%d destination=%d command=%d",live.map_mode,live.map_destination,int(travel));Log(*host,EDEN_DSMOD_LOG_INFO,trace);}
            switch(travel){case za::TravelController::Command::OpenMap:inst.map_commands[0]=true;break;
                case za::TravelController::Command::Destinations:inst.map_commands[1]=true;break;
                case za::TravelController::Command::Up:inst.map_commands[2]=true;break;
                case za::TravelController::Command::Down:inst.map_commands[3]=true;break;
                case za::TravelController::Command::Confirm:inst.map_commands[4]=true;break;default:break;}
            inst.live=std::move(live);}
        Poll(inst, *host, false);
    } catch (...) {
        // A save the module cannot read is a state, never a crash.
    }
}

EdenDsmodBool SupportsBuild(const char*) {
    // The save container's block list and the PA9 layout are the same across the retail builds the
    // package is for; the manifest's build list decides which ones load it.
    return EDEN_DSMOD_TRUE;
}

void* Create(const EdenDsmodHostApi* host, const char* config) {
    if (!dsmod_sdk::HostAbiMatches(host))
        return nullptr;
    auto* inst = new (std::nothrow) Instance;
    if (!inst)
        return nullptr;
    inst->host = *host;
    if(config)if(const auto* key=std::strstr(config,"\"ui_language\""))if(const auto* colon=std::strchr(key,':')){
        int language=0;if(std::sscanf(colon+1,"%d",&language)==1&&language>=1&&language<=11&&language!=6)inst->language_override=language;
    }
    inst->assets.SetReader(host->userdata, host->read_romfs);
    return inst;
}

void Destroy(void* p) {
    delete static_cast<Instance*>(p);
}

void ConfigureSave(void* p, const EdenDsmodHostSaveApi* host) {
    if (!p || !host || host->version != EDEN_DSMOD_SAVE_EXT_VERSION)
        return;
    auto& inst = *static_cast<Instance*>(p);
    std::scoped_lock poll_lock{inst.poll_mutex};
    std::scoped_lock lock{inst.mutex};
    inst.save_api = *host;
    inst.polled_once = false;
    inst.next_poll = 0;
    inst.have_tail = false;
    inst.last_size = 0;
}

void ConfigureExtensions(void* p, const EdenDsmodHostExtensions* host) {
    if (p)
        static_cast<Instance*>(p)->guest_mailbox.Configure(host);
}

EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    if (!p || !action)
        return EDEN_DSMOD_FALSE;
    auto& inst = *static_cast<Instance*>(p);
    const std::string name = action;

    if (name == "moves_apply_guest" || name == "native_box_exchange_guest") {
        std::scoped_lock lock{inst.mutex};
        constexpr std::uint8_t build[] = {0xB1,0xF1,0x2F,0xD9,0x19,0xEA,0xE8,0x6A};
        if (!inst.guest_helper_alive || std::memcmp(inst.host.build_id, build, sizeof(build)) != 0)
            return EDEN_DSMOD_FALSE;
        const auto& live = inst.live;
        if (name == "moves_apply_guest") {
            const auto& move = inst.move_menu;
            return move.Ready(live) &&
                   inst.guest_mailbox.Request(za::GuestMailbox::Operation::ReplaceMove,
                                               move.party, move.slot, move.desired,
                                               live.party[move.party]);
        }
        const auto& swap = inst.native_box;
        return swap.Ready(live) &&
               inst.guest_mailbox.Request(za::GuestMailbox::Operation::Exchange,
                                           swap.party, swap.box, swap.slot,
                                           live.party[swap.party], &live.box_view[swap.slot]);
    }

    // These operations require in-game menu automation; intentionally unavailable.
    if(name=="box_pick"||name=="box_swap_confirm"||name=="center_travel")return EDEN_DSMOD_FALSE;
    if(name=="nav_more"||name=="nav_edit"||name=="nav_slot"||name=="nav_shortcut"||name=="nav_pick"){
        std::scoped_lock lock{inst.mutex};
        if(name=="nav_more"){
            inst.box_picker=false;
            inst.nav_menu=!inst.nav_menu;
            inst.nav_edit_slot=-1;
        }else if(name=="nav_edit"&&inst.nav_menu){
            inst.nav_edit_slot=inst.nav_edit_slot<0?0:-1;
        }else if(name=="nav_slot"&&inst.nav_menu&&inst.nav_edit_slot>=0&&argument>=0&&argument<3){
            inst.nav_edit_slot=int(argument);
        }else if(name=="nav_shortcut"&&argument>=0&&argument<3){
            inst.box_picker=false;
            inst.nav_destination=inst.nav_shortcuts[argument];
            inst.nav_menu=false;
            inst.nav_edit_slot=-1;
        }else if(name=="nav_pick"&&inst.nav_menu&&argument>0&&argument<=kNavPages.size()){
            const int id=int(argument);
            if(inst.nav_edit_slot>=0){
                auto& shortcuts=inst.nav_shortcuts;
                auto& slot=shortcuts[inst.nav_edit_slot];
                if(const auto duplicate=std::ranges::find(shortcuts,id);duplicate!=shortcuts.end())
                    std::swap(*duplicate,slot);
                else slot=id;
                inst.nav_pending=shortcuts[0]*100+shortcuts[1]*10+shortcuts[2];
            }else{
                inst.nav_destination=id;
                inst.nav_menu=false;
            }
        }else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="search_open"||name=="search_mode"||name=="search_key"||name=="search_digit"||
       name=="search_erase"||name=="search_clear"||name=="search_keyboard"){
        std::scoped_lock lock{inst.mutex};
        if(name=="search_open"||name=="search_mode"){
            inst.search_mode=argument==1;
            if(name=="search_open") inst.guide_page=6;
        }else if(name=="search_keyboard"){
            inst.search_keyboard=std::max(0,inst.search_keyboard+(argument>0?1:-1));
            return EDEN_DSMOD_TRUE;
        }else if(name=="search_clear"){
            inst.search_query.clear();
        }else if(name=="search_erase"){
            if(!inst.search_query.empty()){
                auto index=inst.search_query.size()-1;
                while(index>0&&(static_cast<unsigned char>(inst.search_query[index])&0xC0)==0x80) --index;
                inst.search_query.resize(index);
            }
        }else{
            if(inst.search_query.size()>=36) return EDEN_DSMOD_FALSE;
            if(name=="search_digit"){
                if(argument<0||argument>9) return EDEN_DSMOD_FALSE;
                inst.search_query+=char('0'+argument);
            }else{
                if(argument<0||argument>=inst.search_keys.size()) return EDEN_DSMOD_FALSE;
                inst.search_query+=inst.search_keys[argument];
            }
        }
        inst.search_keyboard=0;
        inst.search_error.clear();
        inst.search_results.clear();
        ++inst.search_revision;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="search_pick"||name=="bag_fiche"){
        Instance::SearchResult target;
        {
            std::scoped_lock lock{inst.mutex};
            if(name=="bag_fiche"){
                const int picked=inst.bag_target_selected;
                if(inst.bag_tool!=Instance::BagTool::Evolve||picked<0||picked>=inst.bag_targets.size()) return EDEN_DSMOD_FALSE;
                const auto& row=inst.bag_targets[picked];
                if(!row.eligible) return EDEN_DSMOD_FALSE;
                if(row.party>=0){
                    inst.selected=row.party;
                    inst.guide_page=10;
                    return EDEN_DSMOD_TRUE;
                }
                target.box={za::u16(row.box),za::u16(row.slot),row.entity.internal_species,row.entity.form,
                            row.entity.pid,row.entity.encryption_constant,row.entity.nickname};
            }else{
                if(argument<0||argument>=inst.search_results.size()) return EDEN_DSMOD_FALSE;
                target=inst.search_results[argument];
                if(target.generation){
                    inst.dex_gen=target.generation;
                    inst.dex_filter=0;
                    inst.detail=target.detail;
                    inst.guide_page=7;
                    return EDEN_DSMOD_TRUE;
                }
            }
        }
        if(!OnAction(p,"box_set",target.box.box)) return EDEN_DSMOD_FALSE;
        std::scoped_lock lock{inst.mutex};
        const bool live_box=inst.live.box_live&&inst.live.box_valid&&inst.live.box_view_index==target.box.box;
        const auto& entity=live_box?inst.live.box_view[target.box.slot]:inst.save.box_view[target.box.slot];
        if(!inst.save.valid||inst.save.box_view_index!=target.box.box||!entity.present||
           entity.pid!=target.box.pid||entity.encryption_constant!=target.box.ec){
            if(name=="search_pick") inst.search_error="Cette case a changé. Relance la recherche.";
            return name=="search_pick"?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;
        }
        inst.box_detail_entity=entity;
        inst.box_detail_slot=target.box.slot;
        inst.box_detail_box=target.box.box;
        ++inst.box_detail_open;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="box_details"){
        std::scoped_lock lock{inst.mutex};if(argument<0||argument>=30)return EDEN_DSMOD_FALSE;
        const auto& e=inst.live.box_valid&&inst.live.box_view_index==inst.save.box_view_index?inst.live.box_view[argument]:inst.save.box_view[argument];
        if(!e.present)return EDEN_DSMOD_FALSE;inst.box_detail_entity=e;inst.box_detail_slot=int(argument);inst.box_detail_box=inst.save.box_view_index;++inst.box_detail_open;return EDEN_DSMOD_TRUE;
    }
    if(name=="native_box_prepare"){
        std::scoped_lock lock{inst.mutex};
        if(!inst.native_box.Prepare(inst.live,inst.box_detail_box,inst.box_detail_slot,inst.box_detail_entity))return EDEN_DSMOD_FALSE;
        inst.native_box_status="Choisissez le Pokémon de l'équipe à échanger.";return EDEN_DSMOD_TRUE;
    }
    if(name=="native_box_party"){
        std::scoped_lock lock{inst.mutex};return inst.native_box.Pick(inst.live,int(argument))?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;
    }
    if(name=="native_box_cancel"){
        std::scoped_lock lock{inst.mutex};inst.native_box={};inst.native_box_status.clear();return EDEN_DSMOD_TRUE;
    }
    if(name=="moves_team"||name=="moves_box"||name=="moves_bag"){
        za::MoveMenu menu;std::vector<za::Save::Item> items;
        int desired=0;
        {std::scoped_lock lock{inst.mutex};
            menu.from_box=name=="moves_box";menu.opened=inst.move_menu.opened+1;
            menu.from_bag=name=="moves_bag";
            if(menu.from_box){menu.entity=inst.box_detail_entity;menu.box=inst.box_detail_box;menu.cell=inst.box_detail_slot;}
            else {
                menu.party=inst.selected;
                if(menu.from_bag){
                    const int picked=inst.bag_target_selected;
                    if(inst.bag_tool!=Instance::BagTool::Machine||picked<0||picked>=inst.bag_targets.size()) return EDEN_DSMOD_FALSE;
                    const auto& row=inst.bag_targets[picked];
                    if(!row.eligible) return EDEN_DSMOD_FALSE;
                    menu.party=row.party;
                    desired=TMMove(inst.bag_tool_item);
                }
                if(menu.party<0||menu.party>=6)return EDEN_DSMOD_FALSE;
                menu.entity=inst.live.party_valid?inst.live.party[menu.party]:inst.save.party[menu.party];}
            if(!menu.entity.present||!menu.entity.checksum_ok||menu.entity.egg)return EDEN_DSMOD_FALSE;
            items=inst.save.items;}
        menu.choices=za::MoveChoices(inst.assets,menu.entity,items);
        if(menu.from_bag&&std::ranges::find(menu.choices,desired,&za::MoveChoice::id)==menu.choices.end())
            return EDEN_DSMOD_FALSE;
        menu.desired=desired?desired:menu.entity.moves[0];
        {std::scoped_lock lock{inst.mutex};inst.move_menu=std::move(menu);}return EDEN_DSMOD_TRUE;
    }
    if(name=="moves_slot"){
        std::scoped_lock lock{inst.mutex};if(argument<0||argument>=4||!inst.move_menu.opened)return EDEN_DSMOD_FALSE;
        inst.move_menu.slot=int(argument);if(!inst.move_menu.from_bag)inst.move_menu.desired=inst.move_menu.entity.moves[argument];return EDEN_DSMOD_TRUE;
    }
    if(name=="moves_pick"){
        std::scoped_lock lock{inst.mutex};if(argument<0||argument>=inst.move_menu.choices.size())return EDEN_DSMOD_FALSE;
        inst.move_menu.desired=inst.move_menu.choices[argument].id;return EDEN_DSMOD_TRUE;
    }
    if(name=="box_pick"){
        std::scoped_lock lock{inst.mutex};if(inst.box_swap.Active()||argument<0||argument>=30)return EDEN_DSMOD_FALSE;
        const auto& source=inst.live.box_valid&&inst.live.box_view_index==inst.save.box_view_index?inst.live.box_view[argument]:inst.save.box_view[argument];
        if(!source.present)return EDEN_DSMOD_FALSE;
        inst.swap_slot=int(argument);inst.swap_box=inst.save.box_view_index;inst.swap_party=-1;inst.swap_source=source;inst.swap_status="Choisissez le Pokémon de l'équipe à échanger.";inst.box_swap=za::BoxSwapController{};return EDEN_DSMOD_TRUE;
    }
    if(name=="box_swap_pick"){std::scoped_lock lock{inst.mutex};if(inst.box_swap.Active()||argument<0||argument>=6||inst.swap_slot<0)return EDEN_DSMOD_FALSE;const auto& e=inst.live.party_valid?inst.live.party[argument]:inst.save.party[argument];if(!e.present)return EDEN_DSMOD_FALSE;inst.swap_party=int(argument);return EDEN_DSMOD_TRUE;}
    if(name=="box_swap_confirm"){
        std::scoped_lock lock{inst.mutex};if(inst.swap_slot<0||inst.swap_party<0||inst.box_swap.Active())return EDEN_DSMOD_FALSE;
        if(!inst.live.party_valid||!inst.live.box_valid||inst.live.box_view_index!=inst.swap_box){inst.swap_status="Données en direct indisponibles : Z-A 2.0.2 requis.";return EDEN_DSMOD_FALSE;}
        const auto& source=inst.live.box_view[inst.swap_slot];
        if(source.encryption_constant!=inst.swap_source.encryption_constant||source.pid!=inst.swap_source.pid){inst.swap_status="La boîte a changé. Sélectionnez à nouveau le Pokémon.";return EDEN_DSMOD_FALSE;}
        inst.travel.Cancel();inst.send_target=-1;
        inst.box_swap.Start(inst.swap_box,inst.swap_slot,inst.swap_party,source,inst.live.party[inst.swap_party],inst.command_tick);return EDEN_DSMOD_TRUE;
    }
    if(name=="box_swap_cancel"){std::scoped_lock lock{inst.mutex};if(inst.box_swap.Active())inst.box_swap.Cancel();else inst.swap_slot=-1;return EDEN_DSMOD_TRUE;}
    if(name=="bag_pick"){std::scoped_lock lock{inst.mutex};const auto rows=BagRows(inst.save,inst.bag_category);if(argument<0||argument>=rows.size())return EDEN_DSMOD_FALSE;inst.bag_guide_item=0;inst.bag_return_evolution=false;inst.bag_selected=rows[argument].id;return EDEN_DSMOD_TRUE;}
    if(name=="bag_favor"||name=="bag_favorite_pick"){
        std::scoped_lock lock{inst.mutex};
        if(name=="bag_favorite_pick"){
            if(argument<0||argument>=inst.bag_favorites.size()||!inst.bag_favorites[argument]) return EDEN_DSMOD_FALSE;
            inst.bag_category=-1;
            inst.bag_selected=inst.bag_guide_item=inst.bag_favorites[argument];
            inst.bag_return_evolution=false;
            return EDEN_DSMOD_TRUE;
        }
        const int item=SelectedBagItem(inst);
        if(!item) return EDEN_DSMOD_FALSE;
        auto ids=inst.bag_favorites;
        for(std::size_t i=0;i<ids.size();++i) ids[i]=inst.bag_favorite_writes[i].value_or(ids[i]);
        auto found=std::ranges::find(ids,item);
        if(found!=ids.end()) inst.bag_favorite_writes[found-ids.begin()]=0;
        else if(ItemCount(inst.save,item)&&(found=std::ranges::find(ids,0))!=ids.end())
            inst.bag_favorite_writes[found-ids.begin()]=item;
        else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="team_heal_open"||name=="team_heal_pick"||name=="team_heal_back"){
        std::scoped_lock lock{inst.mutex};
        const auto party=inst.live.party_valid?std::span<const za::Entity>(inst.live.party):
                                               std::span<const za::Entity>(inst.save.party);
        if(name=="team_heal_open"){
            const auto& member=party[inst.selected];
            if(!member.present||member.egg) return EDEN_DSMOD_FALSE;
            inst.team_heal_target=member;
            ++inst.team_heal_revision;
        }else{
            if(!inst.team_heal_target) return EDEN_DSMOD_FALSE;
            if(name=="team_heal_pick"){
                const auto target=std::ranges::find_if(party,[&](const auto& member){
                    return za::NativeBoxExchange::Same(*inst.team_heal_target,member);
                });
                if(argument<=0||argument>=3000||target==party.end()||!ItemCount(inst.save,int(argument)))
                    return EDEN_DSMOD_FALSE;
                auto member=*target;
                const auto cached=inst.confirmed_combat.find((std::uint64_t(member.pid)<<32)|member.encryption_constant);
                if(cached!=inst.confirmed_combat.end()&&cached->second.hp<=member.hp_max) member.hp=cached->second.hp;
                if(!CanUseMedicine(inst.assets.ItemUsageInfo(int(argument)),member)) return EDEN_DSMOD_FALSE;
                OpenBagTool(inst,Instance::BagTool::Heal,int(argument),0,true);
                return EDEN_DSMOD_TRUE;
            }
        }
        inst.guide_page=11;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="bag_hold_apply"||name=="bag_heal_apply"){
        using Tool=Instance::BagTool;
        const Tool tool=name=="bag_hold_apply"?Tool::Hold:Tool::Heal;
        std::scoped_lock lock{inst.mutex};
        const auto& live=inst.live;
        const int selected=inst.bag_target_selected,item=inst.bag_tool_item;
        if(inst.bag_tool!=tool||selected<0||selected>=inst.bag_targets.size()||
           inst.bag_request||!inst.guest_helper_alive||!live.party_current||
           !live.scene_idle||live.deployed_index!=-1) return EDEN_DSMOD_FALSE;
        const auto& row=inst.bag_targets[selected];
        if(!row.eligible||row.party<0||row.party>=live.party.size()) return EDEN_DSMOD_FALSE;
        const auto& member=live.party[row.party];
        if(!member.present||member.egg||!za::NativeBoxExchange::Same(row.entity,member)) return EDEN_DSMOD_FALSE;
        const auto usage=inst.assets.ItemUsageInfo(item);
        auto operation=za::GuestMailbox::Operation::HeldItem;
        u32 expected=member.held_item,effect=0;
        if(tool==Tool::Hold){
            if((item&&!usage.holdable)||item==member.held_item||row.entity.held_item!=member.held_item)
                return EDEN_DSMOD_FALSE;
        }else{
            if(!live.hp_valid[row.party]||!CanUseMedicine(usage,member)||member.status_condition>5||
               member.hp!=row.entity.hp||member.status_condition!=row.entity.status_condition)
                return EDEN_DSMOD_FALSE;
            const u32 next_hp=usage.HpAfterMedicine(member.hp,member.hp_max);
            const u32 next_status=member.hp&&usage.Cures(member.status_condition)?0:member.status_condition;
            expected=member.hp|(member.status_condition<<16);
            effect=next_hp|(next_status<<16);
            if(expected==effect) return EDEN_DSMOD_FALSE;
            operation=za::GuestMailbox::Operation::Medicine;
        }
        u32 sequence{};
        if(!inst.guest_mailbox.Request(operation,row.party,item,expected,member,nullptr,&sequence,effect))
            return EDEN_DSMOD_FALSE;
        inst.bag_request=Instance::BagRequest{sequence,tool};
        inst.bag_status="Changement en cours…";
        return EDEN_DSMOD_TRUE;
    }
    if(name=="bag_help"||name=="bag_target"||name=="bag_hold_team"||name=="bag_hold_box"||
       name=="bag_hold_browse"||name=="bag_hold_clear"||name=="bag_hold_end"){
        using Tool=Instance::BagTool;
        std::scoped_lock lock{inst.mutex};
        if(name=="bag_help"){
            const int item=SelectedBagItem(inst);
            if(argument<1||argument>4||!item) return EDEN_DSMOD_FALSE;
            const auto tool=static_cast<Tool>(argument);
            const auto info=inst.assets.ItemUsageInfo(item);
            if((tool==Tool::Heal&&(!info.IsMedicine()||!ItemCount(inst.save,item)))||
               (tool==Tool::Hold&&(!info.holdable||!ItemCount(inst.save,item)))||
               (tool==Tool::Machine&&!TMMove(item))||
               (tool==Tool::Evolve&&inst.assets.EvolutionSourcesForItem(item).empty())) return EDEN_DSMOD_FALSE;
            OpenBagTool(inst,tool,item,tool==Tool::Hold&&inst.bag_held_target?0:-1);
        }else if(name=="bag_target"){
            if(argument<0||argument>=inst.bag_targets.size()) return EDEN_DSMOD_FALSE;
            if(!inst.bag_request) inst.bag_status.clear();
            inst.bag_target_selected=int(argument);
        }else if(name=="bag_hold_team"||name=="bag_hold_box"){
            Instance::BagTarget row;
            if(name=="bag_hold_team"){
                row.party=inst.selected;
                row.entity=inst.live.party_valid?inst.live.party[row.party]:inst.save.party[row.party];
            }else{
                row.box=inst.box_detail_box;
                row.slot=inst.box_detail_slot;
                row.entity=inst.box_detail_entity;
            }
            if(!row.entity.present||row.entity.egg) return EDEN_DSMOD_FALSE;
            inst.bag_held_target=std::move(row);
            OpenBagTool(inst,Tool::Hold,0,0);
        }else if(name=="bag_hold_browse"){
            const int picked=inst.bag_target_selected;
            if(inst.bag_tool!=Tool::Hold||picked<0||picked>=inst.bag_targets.size()) return EDEN_DSMOD_FALSE;
            inst.bag_held_target=inst.bag_targets[picked];
            inst.bag_guide_item=0;
            inst.bag_return_evolution=false;
            inst.bag_category=-1;
            inst.guide_page=8;
        }else if(name=="bag_hold_clear"){
            if(inst.bag_tool!=Tool::Hold) return EDEN_DSMOD_FALSE;
            if(!inst.bag_request) inst.bag_status.clear();
            inst.bag_tool_item=0;
        }else{
            inst.bag_held_target.reset();
        }
        return EDEN_DSMOD_TRUE;
    }
    if(name=="research_open"||name=="research_pick"||name=="research_close"){
        std::scoped_lock lock{inst.mutex};
        if(name=="research_close")inst.research_open=false;
        else {if(name=="research_pick"&&(argument<0||argument>=50))return EDEN_DSMOD_FALSE;
            inst.research_open=true;inst.research_selected=name=="research_pick"?int(argument)+1:
                std::clamp((inst.live.research_live?inst.live.research_level:inst.save.research_level)+int(argument),1,50);}
        return EDEN_DSMOD_TRUE;
    }
    if (name == "map_quest_pick") {
        std::scoped_lock lock{inst.mutex};
        if (!(inst.map_filters & 64) || argument < 0 ||
            argument >= inst.map_quest_targets.size() || !inst.map_quest_targets[argument]) {
            return EDEN_DSMOD_FALSE;
        }
        CloseMapPanel(inst);
        inst.map_quest = inst.map_quest_targets[argument];
        inst.map_landmark=-1;
        inst.map_filter_menu = false;
        inst.map_center = inst.map_cafe = -1;
        inst.map_dimension.clear();
        inst.guest_mailbox.ClearTravelStatus();
        return EDEN_DSMOD_TRUE;
    }
    if(name=="dimension_pick"){
        std::scoped_lock lock{inst.mutex};if(argument<0||argument>=inst.live.dimensions.size())return EDEN_DSMOD_FALSE;
        const auto& d=inst.live.dimensions[argument];if(!(inst.map_filters&DimensionFilter(d.kind)))return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        inst.map_landmark=-1;
        inst.map_quest=0;inst.map_dimension=d.gate;inst.map_center=inst.map_cafe=-1;inst.map_filter_menu=false;return EDEN_DSMOD_TRUE;
    }
    if(name=="zone_pick"){
        const auto zones=inst.assets.Zones();
        if(argument<0||argument>=zones.size())return EDEN_DSMOD_FALSE;
        std::scoped_lock lock{inst.mutex};
        if(!(inst.map_filters&1))return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        inst.map_zone=zones[argument].number;
        inst.guest_mailbox.ClearTravelStatus();
        return EDEN_DSMOD_TRUE;
    }
    if(name=="zone_missing_toggle"){
        std::scoped_lock lock{inst.mutex};
        inst.map_missing_only=!inst.map_missing_only;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="map_panel"||name=="map_group"){
        std::scoped_lock lock{inst.mutex};
        if(name=="map_panel"){
            const bool was_wide=inst.map_view.wide;
            CloseMapPanel(inst);
            inst.map_help=was_wide;
        }else{
            if(argument<0||argument>=inst.map_groups.size()) return EDEN_DSMOD_FALSE;
            CloseMapPanel(inst);
            inst.map_choices=inst.map_groups[argument];
            ++inst.map_choices_revision;
        }
        return EDEN_DSMOD_TRUE;
    }
    if(name=="map_choice"){
        int type,index;
        {
            std::scoped_lock lock{inst.mutex};
            if(argument<0||argument>=inst.map_choices.size()) return EDEN_DSMOD_FALSE;
            const auto& pin=inst.map_choices[argument];
            type=pin.type;
            index=pin.index;
            if(type==3){
                const auto found=std::ranges::find(inst.live.dimensions,pin.identity,&za::Dimension::gate);
                if(found==inst.live.dimensions.end()) return EDEN_DSMOD_FALSE;
                index=found-inst.live.dimensions.begin();
            }else if(type==4){
                inst.map_quest=pin.key;
                inst.map_choices.clear();
                return EDEN_DSMOD_TRUE;
            }
        }
        constexpr const char* select[]={"zone_pick","center_pick","cafe_pick","dimension_pick","map_quest_pick","landmark_pick"};
        return OnAction(p,select[type],index);
    }
    if(name=="map_control"){
        std::scoped_lock lock{inst.mutex};
        if((argument>=0&&argument<3)||argument==8||(argument>=11&&argument<=15)){
            inst.map_filters^=1<<(argument==8?3:argument>=11?argument-7:argument);
            if(!(inst.map_filters&1))inst.map_zone=0;
            if(!(inst.map_filters&4))inst.map_center=-1;
            if(!(inst.map_filters&8))inst.map_cafe=-1;
            if(argument==11||argument==12||argument==14)inst.map_dimension.clear();
            if(!(inst.map_filters&256))inst.map_landmark=-1;
            if(!(inst.map_filters&64))inst.map_quest=0;
        }else if(argument==9){
            const bool opening=!inst.map_filter_menu;
            CloseMapPanel(inst);
            inst.map_filter_menu=opening;
        }else if(argument==3||(argument>=4&&argument<=7)||argument==10){
            const auto view=inst.map_view;
            float center_x=view.x+320/view.zoom,center_y=view.y+320/view.zoom;
            if(argument==4)center_x-=120/view.zoom;
            if(argument==5)center_x+=120/view.zoom;
            if(argument==6)center_y-=120/view.zoom;
            if(argument==7)center_y+=120/view.zoom;
            const float zoom=std::clamp(view.zoom*(argument==3?1.25f:argument==10?.8f:1.f),1.f,kMapMaxZoom);
            inst.map_home_zoom=zoom;
            inst.map_home_x=std::clamp(center_x/640,.5f/zoom,1-.5f/zoom);
            inst.map_home_y=std::clamp(center_y/640,.5f/zoom,1-.5f/zoom);
            ++inst.map_reset;
        }else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="map_recenter"){
        std::scoped_lock lock{inst.mutex};
        float x{},y{};
        if(!inst.live.position_live||!za::MapPoint(inst.live.area,inst.live.x,inst.live.z,x,y))
            return EDEN_DSMOD_FALSE;
        const float zoom=inst.map_view.zoom,half=.5f/zoom;
        inst.map_home_zoom=zoom;
        inst.map_home_x=std::clamp(x/640.f,half,1-half);
        inst.map_home_y=std::clamp(y/640.f,half,1-half);
        ++inst.map_reset;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="cafe_pick"){
        const auto cafes=inst.assets.Cafes();
        std::scoped_lock lock{inst.mutex};
        if(!(inst.map_filters&8)||argument<0||argument>=cafes.size())return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        inst.map_quest=0;inst.map_filter_menu=false;inst.map_dimension.clear();inst.map_center=-1;inst.map_cafe=int(argument);
        inst.map_landmark=-1;
        inst.guest_mailbox.ClearTravelStatus();
        return EDEN_DSMOD_TRUE;
    }
    if(name=="map_tap"){
        const auto x=int(std::uint64_t(argument)>>32),y=int(std::uint32_t(argument));
        std::scoped_lock lock{inst.mutex};
        const auto view=inst.map_view;
        if(x<0||y<0||x>=view.Size()||y>=view.Size())return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        if(inst.map_filters&1){
            const float scale=view.Scale()*view.zoom;
            const auto number=inst.assets.MapZoneAt(int(view.x+x/scale),int(view.y+y/scale));
            if(number>0){inst.map_quest=0;inst.map_filter_menu=false;inst.map_dimension.clear();inst.map_center=inst.map_cafe=inst.map_landmark=-1;inst.map_zone=number;inst.guest_mailbox.ClearTravelStatus();}
        }
        return EDEN_DSMOD_TRUE;
    }
    if(name=="center_pick"){auto centers=inst.assets.Centers();if(argument<0||argument>=centers.size())return EDEN_DSMOD_FALSE;std::scoped_lock lock{inst.mutex};CloseMapPanel(inst);inst.map_center=int(argument);inst.guest_mailbox.ClearTravelStatus();return EDEN_DSMOD_TRUE;}
    if(name=="landmark_pick"){
        const auto places=inst.assets.Landmarks();
        if(argument<0||argument>=places.size())return EDEN_DSMOD_FALSE;
        std::scoped_lock lock{inst.mutex};
        if(!(inst.map_filters&256)||std::ranges::find(inst.live.landmark_pins,
           places[argument].travel.points[0].row,&za::LandmarkPin::row)==inst.live.landmark_pins.end())
            return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        inst.map_landmark=int(argument);inst.map_quest=0;inst.map_filter_menu=false;
        inst.map_center=inst.map_cafe=-1;inst.map_zone=0;inst.map_dimension.clear();
        inst.guest_mailbox.ClearTravelStatus();
        return EDEN_DSMOD_TRUE;
    }
    if(name=="map_mark_start"||name=="map_mark_clear"||name=="map_mark_at"){
        std::scoped_lock lock{inst.mutex};
        const auto& live=inst.live;
        if(name=="map_mark_start") {
            if(!inst.placing_marker&&(!inst.guest_helper_alive||!live.position_live||
               live.area!="t1"||inst.marker_request))return EDEN_DSMOD_FALSE;
            inst.placing_marker=!inst.placing_marker;
            inst.map_filter_menu=false;
            inst.marker_result="";
        } else if(name=="map_mark_clear") {
            if(!inst.guest_helper_alive||inst.marker_request||
               !live.position_live||live.area!="t1")return EDEN_DSMOD_FALSE;
            inst.placing_marker=false;
            inst.marker_result="";
            inst.marker_request=Instance::MarkerRequest{.x=0,.y=0,.deadline=inst.command_tick+180,.clear=true};
        } else {
            if(!inst.placing_marker)return EDEN_DSMOD_FALSE;
            const auto x=int(std::uint64_t(argument)>>32),y=int(std::uint32_t(argument));
            const auto view=inst.map_view;
            if(x<0||y<0||x>=view.Size()||y>=view.Size())return EDEN_DSMOD_FALSE;
            const float u=view.x+x/(view.Scale()*view.zoom);
            const float v=view.y+y/(view.Scale()*view.zoom);
            if(std::hypot(u-320.f,v-320.f)>320.f)return EDEN_DSMOD_FALSE;
            const float native_x=2048.f-u*4096.f/640.f,native_y=v*4096.f/640.f-2048.f;
            inst.placing_marker=false;
            inst.marker_request=Instance::MarkerRequest{native_x,native_y,inst.command_tick+180};
        }
        return EDEN_DSMOD_TRUE;
    }
    if(name=="quest_load_map"){
        std::scoped_lock lock{inst.mutex};
        const auto& live=inst.live;
        if(!inst.guest_helper_alive||!live.scene_idle||!live.position_live||live.area!="t1"||
           live.quest_map_loaded||(live.map_mode>=0&&live.map_mode<3)) return EDEN_DSMOD_FALSE;
        return inst.guest_mailbox.RequestMapOpen()?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;
    }
    if(name=="native_map_travel"){
        const auto centers=inst.assets.Centers();
        const auto cafes=inst.assets.Cafes();
        const auto zones=inst.assets.Zones();
        std::scoped_lock lock{inst.mutex};
        const auto& live=inst.live;
        if(!inst.guest_helper_alive||!live.scene_idle||!live.position_live||live.area!="t1"||
           inst.map_filter_menu||!inst.map_dimension.empty()||inst.map_quest)return EDEN_DSMOD_FALSE;
        const auto landmarks=inst.assets.Landmarks();
        const za::Assets::TravelDestination* destination=nullptr;
        if(inst.map_center>=0&&inst.map_center<centers.size())destination=&centers[inst.map_center].travel;
        else if(inst.map_cafe>=0&&inst.map_cafe<cafes.size())destination=&cafes[inst.map_cafe].travel;
        else if((inst.map_filters&256)&&inst.map_landmark>=0&&inst.map_landmark<landmarks.size()){
            const auto& place=landmarks[inst.map_landmark];
            if(std::ranges::find(live.landmark_pins,place.travel.points[0].row,&za::LandmarkPin::row)!=
               live.landmark_pins.end())destination=&place.travel;
            else return EDEN_DSMOD_FALSE;
        }
        else if(inst.map_filters&1){
            const auto zone=std::ranges::find(zones,inst.map_zone,&za::Assets::Zone::number);
            if(zone!=zones.end())destination=&zone->travel;
        }
        if(!destination||!TravelUnlocked(*destination,live))return EDEN_DSMOD_FALSE;
        const bool open=live.map_mode<0||live.map_mode>=3;
        return inst.guest_mailbox.RequestTravel(*destination,open)?
            EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;
    }
    if(name=="travel_cancel"){std::scoped_lock lock{inst.mutex};inst.travel.Cancel();inst.map_commands.fill(false);return EDEN_DSMOD_TRUE;}
    if(name=="center_close"){std::scoped_lock lock{inst.mutex};CloseMapPanel(inst);inst.guest_mailbox.ClearTravelStatus();return EDEN_DSMOD_TRUE;}
    if(name=="attack"){
        std::scoped_lock lock{inst.mutex};
        if(argument<0||argument>=4||inst.send_target>=0||inst.attack_slot>=0||
           inst.command_tick<inst.attack_busy_until||!inst.live.party_valid||
           inst.live.selected_index<0||inst.live.deployed_index<-1)return EDEN_DSMOD_FALSE;
        const auto& entity=inst.live.party[inst.selected];
        if(!entity.present||!entity.moves[argument]||
           (inst.live.hp_valid[inst.selected]&&entity.hp==0))return EDEN_DSMOD_FALSE;
        inst.attack_slot=inst.selected;
        inst.attack_move=int(argument);
        inst.attack_id=entity.moves[argument];
        inst.attack_identity=(u64(entity.pid)<<32)|entity.encryption_constant;
        inst.attack_phase=Instance::AttackPhase::Select;
        inst.attack_deadline=inst.command_tick+300;
        inst.attack_next_select=inst.command_tick;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="send_out"){
        std::scoped_lock lock{inst.mutex};if(inst.box_swap.Active())return EDEN_DSMOD_FALSE;if(inst.live.selected_index<0){inst.send_status="Sélection en jeu indisponible. Vérifiez Z-A 2.0.2.";return EDEN_DSMOD_FALSE;}
        const auto& party=inst.live.party_valid?inst.live.party[inst.selected]:inst.save.party[inst.selected];
        if(!party.present)return EDEN_DSMOD_FALSE;
        inst.travel.Cancel();inst.send_status="Sélection du Pokémon en cours...";
        inst.attack_slot=-1;
        inst.send_target=inst.selected;inst.command_next=inst.command_tick;inst.command_deadline=inst.command_tick+480;
        return EDEN_DSMOD_TRUE;
    }
    if(name=="team_select"){std::scoped_lock lock{inst.mutex};inst.attack_slot=-1;inst.selected=static_cast<int>(std::clamp<int64_t>(argument,0,5));return EDEN_DSMOD_TRUE;}
    if(name=="dex_pick"){std::scoped_lock lock{inst.mutex};auto rows=DexRows(inst.assets,inst.save,inst.dex_gen,inst.dex_filter);auto row=argument;if(row>=0 && row<rows.size()){inst.detail=rows[row];}return EDEN_DSMOD_TRUE;}
    if(name.starts_with("guide_")){
        std::scoped_lock lock{inst.mutex};
        if(name=="guide_capture"){
            inst.guide_page=5;
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_capture_map"){
            if(argument<0||argument>=inst.capture_zones.size()) return EDEN_DSMOD_FALSE;
            OpenGuideMap(inst,inst.capture_zones[argument].location);
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_item"){
            const auto rows=inst.assets.Evolutions(inst.guide_target.species,inst.guide_target.form);
            if(argument<0||argument>=rows.size()) return EDEN_DSMOD_FALSE;
            const int item=rows[argument].item;
            if(item<=0||item>=3000) return EDEN_DSMOD_FALSE;
            inst.bag_category=-1;
            inst.bag_guide_item=item;
            inst.bag_return_evolution=true;
            inst.bag_held_target.reset();
            inst.bag_selected=item;
            inst.guide_page=8;
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_item_back"){
            inst.bag_guide_item=0;
            inst.bag_return_evolution=false;
            inst.guide_page=4;
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_dex_pin" || name=="guide_locations" || name=="guide_combat" || name=="guide_evolutions"){
            const auto rows=DexRows(inst.assets,inst.save,inst.dex_gen,inst.dex_filter);
            if(rows.empty()) return EDEN_DSMOD_FALSE;
            const int detail=std::ranges::find(rows,inst.detail)!=rows.end()?inst.detail:rows.front();
            const auto catalog=inst.assets.Pokedex(inst.dex_gen-1);
            const auto entry=std::ranges::find_if(catalog,[&](const auto& item){
                return za::NationalSpecies(item.species)==detail%2048 &&
                    (inst.dex_gen!=3 || item.form==detail/2048);
            });
            if(entry==catalog.end()) return EDEN_DSMOD_FALSE;
            const Instance::Objective target{entry->species,entry->form,-1,{},inst.dex_gen==3};
            if(name=="guide_dex_pin"){
                return WriteObjective(inst,target);
            }else{
                inst.guide_target=target;
                inst.guide_locations.clear();
                inst.guide_page=name=="guide_locations"?1:name=="guide_combat"?2:4;
            }
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_open"||name=="guide_remove"){
            if(argument<0||argument>=inst.objectives.size()) return EDEN_DSMOD_FALSE;
            const auto target=inst.objectives[argument];
            if(name=="guide_remove") return WriteObjective(inst,target,true);
            else if(target.category>=0){
                inst.quest_category=target.category;
                inst.quest_selected_id=target.mission;
                inst.guide_page=3;
            }else{
                inst.guide_target=target;
                inst.guide_locations.clear();
                inst.guide_page=1;
            }
            return EDEN_DSMOD_TRUE;
        }
        if(name=="guide_map"){
            if(argument<0||argument>=inst.guide_locations.size()) return EDEN_DSMOD_FALSE;
            const auto& location=inst.guide_locations[argument];
            if(!location.zone && std::ranges::find(inst.live.dimensions,location.dimension,&za::Dimension::gate)==inst.live.dimensions.end())
                return EDEN_DSMOD_FALSE;
            OpenGuideMap(inst,location);
            return EDEN_DSMOD_TRUE;
        }
        return EDEN_DSMOD_FALSE;
    }
    if(name.starts_with("quest_")){
        std::scoped_lock lock{inst.mutex};
        if(name=="quest_category"){
            const int category=static_cast<int>(std::clamp<int64_t>(argument,0,3));
            if (category!=inst.quest_category) inst.quest_selected_id.clear();
            inst.quest_category=category;
            return EDEN_DSMOD_TRUE;
        }
        auto state=inst.save;
        if(!inst.live.quest_work.empty()) state.quest_work=inst.live.quest_work;
        const auto rows=MissionRows(inst,inst.quest_category,state);
        if(name=="quest_pick"){
            if(argument<0||argument>=rows.size())return EDEN_DSMOD_FALSE;
            inst.quest_selected_id=rows[argument].id;
            return EDEN_DSMOD_TRUE;
        }
        if(rows.empty()) return EDEN_DSMOD_FALSE;
        const auto selected_row=std::ranges::find(rows,inst.quest_selected_id,&za::Assets::Mission::id);
        const auto& mission=selected_row==rows.end() ? rows.front() : *selected_row;
        if(name=="quest_pin"){
            const Instance::Objective target{0,0,mission.category,mission.id};
            return WriteObjective(inst,target);
        }
        if(name!="quest_map") return EDEN_DSMOD_FALSE;
        if (!MissionOnMap(MissionState(mission,state))) return EDEN_DSMOD_FALSE;
        const auto& pins=inst.live.quest_pins;
        const auto target=std::ranges::find_if(pins,[&](const auto& pin){
            return pin.category==mission.category && pin.id==mission.map_id;
        });
        if(target==pins.end()) return EDEN_DSMOD_FALSE;
        CloseMapPanel(inst);
        constexpr float zoom=2.4f, half=.5f/zoom;
        inst.map_home_zoom=zoom;
        inst.map_home_x=std::clamp((2048.f-target->x)/4096.f,half,1-half);
        inst.map_home_y=std::clamp((2048.f+target->y)/4096.f,half,1-half);
        ++inst.map_reset;
        inst.map_quest=target->category*1024+target->id;
        inst.map_filters|=64;
        inst.map_filter_menu=false;
        inst.map_dimension.clear();
        inst.map_center=inst.map_cafe=inst.map_landmark=-1;
        inst.map_zone=0;
        inst.quest_show_map=true;
        return EDEN_DSMOD_TRUE;
    }
    if (name.starts_with("dex_") || name.starts_with("bag_")) {
        std::scoped_lock lock{inst.mutex};
        if (name == "dex_gen") { inst.dex_gen=static_cast<int>(std::clamp<int64_t>(argument,1,3)); inst.dex_page=0; }
        else if (name == "dex_filter") { inst.dex_filter=static_cast<int>(std::clamp<int64_t>(argument,0,4)); inst.dex_page=0; }
        else if (name == "dex_prev" || name == "dex_next") {
            const int pages=std::max(1,static_cast<int>((DexRows(inst.assets,inst.save,inst.dex_gen,inst.dex_filter).size()+kDexPageSize-1)/kDexPageSize));
            inst.dex_page=std::clamp(inst.dex_page+(name=="dex_next"?1:-1),0,pages-1);
        } else if (name == "bag_category") { inst.bag_guide_item=0;inst.bag_return_evolution=false;inst.bag_category=static_cast<int>(std::clamp<int64_t>(argument,-1,8)); inst.bag_page=0; }
        else if (name == "bag_prev" || name == "bag_next") {
            const int pages=std::max(1,static_cast<int>((BagRows(inst.save,inst.bag_category).size()+kBagPageSize-1)/kBagPageSize));
            inst.bag_page=std::clamp(inst.bag_page+(name=="bag_next"?1:-1),0,pages-1);
        } else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    }

    int target = -1;
    bool follow = false;
    if (name == "box_picker") {
        std::scoped_lock lock{inst.mutex};
        if(!inst.save.valid||inst.save.boxes_available<=0) return EDEN_DSMOD_FALSE;
        inst.box_picker=!inst.box_picker;
        return EDEN_DSMOD_TRUE;
    } else if (name == "box_prev") {
        std::scoped_lock lock{inst.mutex};
        target = std::max(0, inst.save.box_view_index - 1);
    } else if (name == "box_next") {
        std::scoped_lock lock{inst.mutex};
        const int last = std::max(0, inst.save.boxes_available - 1);
        target = std::min(last, inst.save.box_view_index + 1);
    } else if (name == "box_set") {
        std::scoped_lock lock{inst.mutex};
        if(argument<0||argument>=inst.save.boxes_available) return EDEN_DSMOD_FALSE;
        inst.box_picker=false;
        target = static_cast<int>(argument);
    } else if (name == "box_follow") {
        follow = true;
    } else if (name == "refresh") {
        // A forced re-read, so the page picks up a save made a moment ago.
    } else {
        return EDEN_DSMOD_FALSE;
    }

    {
        std::scoped_lock lock{inst.mutex};
        if (target >= 0) {
            inst.box_index = target;
            inst.box_follow = false;
        }
        if (follow)
            inst.box_follow = true;
    }

    try {
        Poll(inst, inst.host, true);
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi* asset_host, const char* key,
                        void* receiver, EdenDsmodImageSink sink) {
    if (!p || !key || !sink) return EDEN_DSMOD_FALSE;
    auto& inst = *static_cast<Instance*>(p);
    try {
        za::Picture image;
        bool ok = false;
        if(std::strncmp(key,"module:radar/",13)==0){
            za::Entity e;{std::scoped_lock lock{inst.mutex};e=inst.box_detail_entity;}
            constexpr int side=280;image.width=image.height=side;image.rgba.assign(side*side*4,0);
            const float values[]={float(e.hp_max),float(e.attack),float(e.defense),float(e.speed),float(e.sp_defense),float(e.sp_attack)};
            float maximum=100;for(auto n:values)maximum=std::max(maximum,n);maximum*=1.1f;
            std::array<float,6> px{},py{};
            for(int i=0;i<6;++i){float a=-1.5707963f+i*1.04719755f;px[i]=140+110*std::cos(a)*values[i]/maximum;py[i]=140+110*std::sin(a)*values[i]/maximum;}
            auto pixel=[&](int x,int y,int r,int g,int b,int a){if(x<0||y<0||x>=side||y>=side)return;auto p=(y*side+x)*4;image.rgba[p]=r;image.rgba[p+1]=g;image.rgba[p+2]=b;image.rgba[p+3]=a;};
            for(int y=0;y<side;++y)for(int x=0;x<side;++x){bool inside=false;for(int i=0,j=5;i<6;j=i++)if((py[i]>y)!=(py[j]>y)&&x<(px[j]-px[i])*(y-py[i])/(py[j]-py[i])+px[i])inside=!inside;if(inside)pixel(x,y,183,212,112,155);}
            auto line=[&](float x1,float y1,float x2,float y2,int r,int g,int b,int alpha){int steps=int(std::max(std::abs(x2-x1),std::abs(y2-y1)))+1;for(int n=0;n<=steps;++n)pixel(int(std::lround(x1+(x2-x1)*n/steps)),int(std::lround(y1+(y2-y1)*n/steps)),r,g,b,alpha);};
            for(int ring=1;ring<=3;++ring)for(int i=0;i<6;++i){float a=-1.5707963f+i*1.04719755f,b=a+1.04719755f,t=110.f*ring/3;line(140+t*std::cos(a),140+t*std::sin(a),140+t*std::cos(b),140+t*std::sin(b),134,165,193,110);if(ring==3)line(140,140,140+t*std::cos(a),140+t*std::sin(a),134,165,193,110);}
            for(int i=0;i<6;++i)line(px[i],py[i],px[(i+1)%6],py[(i+1)%6],216,235,159,240);ok=true;
        } else
        if (std::strcmp(key, "module:ready") == 0) {
            ok = inst.assets.EnsureNames();
            image.width=1; image.height=1; image.rgba.assign(4,0);
        } else if(std::strncmp(key,"module:menu/",12)==0){
            ok=inst.assets.MenuIcon(std::atoi(key+12),image);
        } else if(std::strncmp(key,"module:progress/",16)==0){
            ok=inst.assets.ProgressArt(std::atoi(key+16),image);
        } else if (std::strcmp(key, "module:map") == 0) {
            ok = inst.assets.Map(640, image);
        } else if(std::strncmp(key,"module:map/",11)==0){
            const int index=std::atoi(key+11);
            if(index<0||index>16383)return EDEN_DSMOD_FALSE;
            ok=inst.assets.Map(640,image,index);
        } else if(std::strncmp(key,"module:zone/",12)==0){
            ok=inst.assets.ZoneIcon(std::atoi(key+12),image);
        } else if(std::strcmp(key,"module:center")==0){
            ok=inst.assets.CenterIcon(image);
        } else if(std::strcmp(key,"module:cafe")==0){
            ok=inst.assets.CafeIcon(image);
        } else if(std::strcmp(key,"module:map-player")==0){
            ok=inst.assets.MapMarker(0,image);
        } else if(std::strcmp(key,"module:map-portal-wild")==0){
            ok=inst.assets.MapMarker(1,image);
        } else if(std::strcmp(key,"module:map-portal-battle")==0){
            ok=inst.assets.MapMarker(2,image);
        } else if(std::strcmp(key,"module:map-quest-main")==0){
            ok=inst.assets.MapMarker(3,image);
        } else if(std::strcmp(key,"module:map-quest-side")==0){
            ok=inst.assets.MapMarker(4,image);
        } else if(std::strcmp(key,"module:map-portal-unknown")==0){
            ok=inst.assets.MapMarker(5,image);
        } else if(std::strcmp(key,"module:map-pin")==0){
            ok=inst.assets.MapMarker(6,image);
        } else if(std::strcmp(key,"module:map-landmark")==0){
            ok=inst.assets.MapMarker(7,image);
        } else if(std::strcmp(key,"module:map-dimension-base")==0){
            ok=inst.assets.MapMarker(8,image);
        } else if(std::strcmp(key,"module:reward-money")==0){
            ok=inst.assets.MoneyIcon(image);
        } else if(std::strncmp(key,"module:type/",12)==0){
            ok=inst.assets.TypeIcon(std::atoi(key+12),image);
        } else if(std::strncmp(key,"module:item/",12)==0){
            int item=0;char extra=0;if(std::sscanf(key+12,"%d%c",&item,&extra)!=1 || item<1 || item>=3000)return EDEN_DSMOD_FALSE;
            ok=inst.assets.ItemIcon(item,image);
        } else if (std::strncmp(key, "module:icon/", 12) == 0) {
            int species = 0;
            char extra = 0;
            if (std::sscanf(key + 12, "%d%c", &species, &extra) != 1 || species < 1 || species > 524288 + 1025 * 256 + 255)
                return EDEN_DSMOD_FALSE;
            ok = inst.assets.Icon((species & 524287) / 256, species % 256, image, (species & 524288)!=0);
        } else return EDEN_DSMOD_FALSE;
        if (inst.assets.NamesReady()) {
            bool need_names;
            { std::scoped_lock lock{inst.mutex}; need_names = !inst.species_names; }
            if (need_names) {
                auto names = std::make_shared<std::vector<std::string>>();
                names->reserve(1025);
                for (int i = 1; i <= 1025; ++i) names->emplace_back(inst.assets.SpeciesName(i));
                auto items = std::make_shared<std::vector<std::string>>();
                for (int i=0;i<3000;++i) items->push_back(inst.assets.ItemName(i));
                auto available = std::make_shared<std::vector<bool>>(1026,false);
                for (int i=1;i<=1025;++i) (*available)[i]=inst.assets.HasSpecies(i);
                if (asset_host && asset_host->log)
                    asset_host->log(asset_host->userdata,EDEN_DSMOD_LOG_INFO,"ZA assets: archives, noms et catalogue charges");
                std::scoped_lock lock{inst.mutex};
                inst.item_names=std::move(items);
                inst.available_species=std::move(available);
                inst.species_names = std::move(names);
            }
        }
        if (!ok || !image.Valid()) return EDEN_DSMOD_FALSE;
        sink(receiver, image.width, image.height, image.rgba.data(), image.rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) { return EDEN_DSMOD_FALSE; }
}

const EdenDsmodModuleExtensions Extensions{
    EDEN_DSMOD_EXT_VERSION,
    sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,
    &ConfigureExtensions,
    &OnAction,
    &LoadImage,
};

const EdenDsmodModuleApi ModuleApi{
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitleId,
    "Pokemon Legends Z-A DSMod",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN,
    &SupportsBuild,
    &Create,
    &Destroy,
    &Sample,
    &Tick,
};

const EdenDsmodModuleSaveExtensions SaveExtensions{
    EDEN_DSMOD_SAVE_EXT_VERSION,
    sizeof(EdenDsmodModuleSaveExtensions),
    EDEN_DSMOD_SAVE_EXT_HASH,
    &ConfigureSave,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
// No DSMOD_SDK_EXPORT_SAVE_EXTENSIONS macro exists; the generic getter is the same expansion.
DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_save_extensions, EdenDsmodModuleSaveExtensions,
                        EDEN_DSMOD_SAVE_EXT_VERSION, EDEN_DSMOD_SAVE_EXT_HASH, SaveExtensions)

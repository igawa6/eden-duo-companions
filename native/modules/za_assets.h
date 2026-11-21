// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Legends Z-A's own art and text, decoded at runtime from the player's romfs.
//
// The game stores everything in one Trinity archive: `data.trpfd` is a FlatBuffer listing
// 9,892 pack paths with their sizes, `data.trpfs` is the container itself (a 3.3 GB file with
// a FlatBuffer index at its end mapping an FNV-1a hash of each path to a byte offset). Each
// pack is a small "trpak": a header table, a hash table, and one or more inner files, each
// either stored raw or compressed with Oodle Kraken. Inside are ordinary Switch containers
// (BNTX textures, SARC archives).
//
// Nothing here ships a byte of the game: every call reads the player's own installation
// through the host's read_romfs, so a package stays asset-free (the same rule the other
// companions follow). Only offsets and hashes are pinned, and those are structure, not art.
//
// What this file exposes:
//   * SpeciesName(): the French species names out of the game's encrypted message table.
//   * Icon(): the 160x160 party/box thumbnail of a species (BC7, block-linear).
//   * Map(): the full Lumiose plan (BC1 4096x4096) box-filtered down for the map page.

#pragma once

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace za {

/// An RGBA8 picture, tightly packed, ready for the host's image sink.
struct Picture {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool Valid() const {
        return width != 0 && height != 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
    void Clear() {
        width = 0;
        height = 0;
        rgba.clear();
    }
};

class Assets {
public:
    /// The host's read_romfs: a bare path is a romfs path; returns the bytes copied, or the
    /// file's size when `output` is null.
    using ReadFn = std::size_t (*)(void* userdata, const char* path, std::uint64_t offset,
                                   void* output, std::size_t size);

    /// Called once, from the module's create(). Safe to call from any thread afterwards.
    void SetReader(void* userdata, ReadFn read);
    /// Save language IDs (including LATAM=11). Changing language reloads text, never the save.
    bool SetLanguage(int language);

    /// 1-based species id -> French name, UTF-8 (gendered species read "NidoranF"/"NidoranM").
    /// Returns "" before Load() has succeeded and for ids outside the table.
    const char* SpeciesName(int species);
    /// True once the pack index and the name table are in memory.
    [[nodiscard]] bool NamesReady() const;
    /// Builds the pack index and the name table if it has not been tried yet. Safe to call
    /// from any thread; the first caller does the work (a few hundred milliseconds) and the
    /// rest wait. Returns whether names are available.
    bool EnsureNames();
    std::string ItemName(int item);
    std::string ItemDescription(int item);
    struct ItemUsage {
        bool holdable=false, heals_hp=false, revives=false;
        unsigned cured_statuses=0;
        unsigned hp_points=0, hp_percent=0, revival_points=0, revival_percent=0;
        unsigned HpAfterMedicine(unsigned hp,unsigned maximum) const;
        bool IsMedicine() const { return heals_hp || revives || cured_statuses; }
        bool Cures(unsigned status) const { return status>0&&status<=5&&(cured_statuses&(1u<<status)); }
        bool CanHeal(unsigned hp,unsigned maximum,unsigned status) const {
            return maximum&&(hp==0?revives:(heals_hp&&hp<maximum)||Cures(status));
        }
    };
    ItemUsage ItemUsageInfo(int item);
    struct DexEntry {int species=0,form=0,number=0;};
    std::vector<DexEntry> Pokedex(int category);
    struct MissionReward {
        enum class Kind { Money, Item, ResearchPoints };
        Kind kind;
        int item, amount;
    };
    struct Mission {
        std::string id,title,description,objectives,work;
        int map_id=0,number=0,category=0;
        std::vector<MissionReward> rewards;
    };
    std::vector<Mission> Missions(int category);
    struct Personal {int type1=-1,type2=-1,growth=-1;};
    Personal SpeciesInfo(int internal_species,int form);
    struct Evolution {
        int source,source_form,target,target_form;
        int level,method,argument;
        int item=0;
    };
    std::vector<Evolution> Evolutions(int species,int form);
    std::vector<Evolution> EvolutionSourcesForItem(int item);
    int MoveType(int move);
    struct LearnMove {int id=0,level=0;};
    struct MoveInfo {int category=-1,power=-1,accuracy=-1;};
    std::vector<LearnMove> Learnset(int species,int form);
    std::vector<int> MachineMoves(int species,int form);
    MoveInfo MoveParameters(int move);
    std::string TypeName(int type);
    std::string NatureName(int nature);
    /// Uses matching rows of the game's French and selected-language UI tables.
    std::string UiText(const std::string& french);
    std::uint32_t ExperienceAt(int growth,int level);
    struct ResearchLevel {int level=0,points=0,item=0,count=0;};
    std::vector<ResearchLevel> ResearchLevels();
    struct TrainerRank {int rank=0,points=0;std::string name;};
    std::vector<TrainerRank> TrainerRanks();
    struct DimensionInfo {int level=0,size=-1;std::vector<DexEntry> pokemon;};
    DimensionInfo DimensionDetails(const std::string& content,int rank,
                                  const std::array<int,3>& types,const std::array<int,3>& patterns,int kind);
    struct TravelPoint {
        int row=-1;
        std::uint64_t key=0;
    };
    struct TravelDestination {
        unsigned kind=0;
        std::array<TravelPoint,2> points{};
        bool Available() const { return points.front().row>=0; }
        void Add(int row,std::uint64_t key) {
            for(auto& point:points)if(point.row<0){point={row,key};return;}
        }
    };
    struct Zone {
        int number=0,texture=0;
        float x=0,y=0,w=0,h=0;
        std::uint64_t active_hash=0;
        TravelDestination travel;
    };
    struct Center {
        std::string id;
        float x=0,z=0;
        int destination=0;
        std::string name,description;
        TravelDestination travel;
    };
    std::vector<Zone> Zones();
    std::vector<Zone> BattleZones();
    int MapZoneAt(int x,int y);
    std::vector<Center> Centers();
    std::vector<Center> Cafes();
    std::vector<Center> Landmarks();
    bool CenterIcon(Picture& out);
    bool CafeIcon(Picture& out);
    bool MapMarker(int index, Picture& out);
    bool MoneyIcon(Picture& out);
    bool ZoneIcon(int index,Picture& out);
    bool TypeIcon(int type,Picture& out);
    bool ProgressArt(int index,Picture& out);
    bool MenuIcon(int index,Picture& out);
    std::string MoveName(int move);
    std::string MoveDescription(int move);
    std::string SpeciesDescription(int internal_species);
    bool HasSpecies(int species) const;

    /// The party/box icon of a species (160x160). `form` is the save's form id; 0 is the
    /// ordinary sprite and is what the pages show. False when the pack is missing.
    bool Icon(int species, int form, Picture& out, bool shiny=false);
    /// Bag artwork, indexed by each BNTX's own item_<id> name, not by pack order.
    bool ItemIcon(int item, Picture& out);
    /// The whole Lumiose plan, box-filtered down to `size` x `size` (the page draws it 1:1).
    bool Map(unsigned size, Picture& out, unsigned layers = 3);

private:
    struct PackRef {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
    };

    /// One icon pack found while indexing: the save's form selects among them.
    struct IconRef {
        int form = 0;
        std::string path;
    };

    bool LoadLocked(); ///< builds packs_ and names_; the caller holds mutex_
    bool ReadRomfs(const char* path, std::uint64_t offset, void* out, std::size_t size);
    std::uint64_t RomfsSize(const char* path);
    bool Fetch(const char* path, std::vector<std::uint8_t>& out);
    bool FetchInner(const char* path, std::size_t index, std::vector<std::uint8_t>& out);
    bool FetchMessage(const char* path,std::vector<std::uint8_t>& text,std::vector<std::uint8_t>* labels=nullptr);
    bool IconLocked(int species, int form, Picture& out, bool shiny);
    bool IndexItemsLocked();
    void LoadDimensionsLocked();

    mutable std::mutex mutex_;
    void* userdata_ = nullptr;
    ReadFn read_ = nullptr;
    std::string archive_root_;
    std::vector<std::string> item_names_;
    std::unordered_map<int,ItemUsage> item_usage_;
    std::vector<std::string> item_descriptions_;
    std::vector<DexEntry> pokedex_,mega_forms_;
    std::vector<Mission> missions_[3];
    std::vector<std::string> move_names_;
    std::vector<std::string> type_names_,nature_names_;
    std::vector<std::string> move_descriptions_;
    std::vector<std::string> species_descriptions_;
    bool item_index_tried_ = false;
    std::vector<std::uint8_t> item_pack_;
    std::unordered_map<int, std::size_t> item_indices_;
    std::unordered_map<int, Picture> item_icons_;
    std::vector<int> item_icon_order_;
    std::chrono::steady_clock::time_point retry_at_{};
    bool tried_ = false;
    bool ready_ = false;
    std::unordered_map<std::uint64_t, PackRef> packs_;
    std::vector<Zone> zones_,battle_zones_;
    std::vector<Center> centers_,cafes_,landmarks_;
    std::vector<std::uint8_t> map_ui_bntx_;
    Picture money_icon_;
    std::unordered_map<int, std::vector<IconRef>> icon_packs_;
    std::vector<std::string> names_;
    std::unordered_map<int, Picture> icons_;
    std::vector<int> icon_order_;
    std::unordered_map<unsigned,Picture> maps_;
    std::vector<std::uint8_t> map_zone_hits_;
    std::unordered_map<int,Personal> personal_;
    std::unordered_map<int,std::vector<Evolution>> evolutions_;
    std::unordered_map<int,std::vector<Evolution>> evolutions_by_item_;
    std::vector<int> move_types_;
    std::vector<MoveInfo> move_info_;
    std::unordered_map<int,std::vector<LearnMove>> learnsets_;
    std::unordered_map<int,std::vector<int>> machine_moves_;
    std::vector<std::uint32_t> experience_;
    std::vector<ResearchLevel> research_levels_;
    std::vector<TrainerRank> trainer_ranks_;
    bool research_tried_=false;
    std::string research_pack_path_;
    std::array<std::string,5> dimension_pack_paths_{};
    bool dimensions_tried_=false;
    std::array<int,5> dimension_levels_{};
    std::unordered_map<std::string,int> dimension_sizes_;
    std::unordered_map<int,std::vector<DexEntry>> dimension_pokemon_;
    std::unordered_map<int,Picture> type_icons_;
    std::vector<std::uint8_t> type_bntx_;
    std::vector<std::uint8_t> progress_bntx_;
    std::unordered_map<int,Picture> progress_art_;
    int language_=3;
    std::unordered_map<std::string,std::string> ui_translations_;
};

} // namespace za

// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Art, text and data tables for the Luminescent Platinum companion, all decoded from the player's
// own romfs through lp_unity:
//   module:lp:ui/<bundle>/<sprite>        a UI sprite (bundle = short name, see Bundles below)
//   module:lp:icon/<species>/<form>/<gender>/<shiny>   a party icon
//   module:lp:item/<id>                   an item icon
//   module:lp:font/<name>:<px>[/<page>]   the game text font atlas, whole or one page (decode_font
//                                         gives the metrics; the face follows the language)
//   module:lp:tex/logo[/<sfx>], tex/pressa[/<sfx>]  the title screen's logo and "Press the A Button"
//   module:lp:mask/<any key>              that image in white with its alpha (a silhouette to tint)
//   ...@<sfx> on a poketch key            drawn with that language's text art
// Language-dependent art carries the UI suffix in its key (ui/common_lang.<sfx>/..., tex/logo/<sfx>),
// so the runtime's image cache never serves another language's picture.
// Names (species, dex_descriptions, moves, items) come from the message tables of the game's
// language (lp_lang.h; English fills labels a translation leaves empty) and move/species data from
// Pml/personal_masterdatas — the Luminescent Platinum copies when the mod is installed, since
// romfs: is the game's own LayeredFS chain (mod folders layered by name, as the game sees them).

#pragma once

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "lp_dex.h"
#include "lp_lang.h"
#include "lp_poketch.h"
#include "lp_unity.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lp_assets {

struct Img {
    std::uint32_t w{}, h{};
    std::vector<std::uint8_t> rgba;
};

struct Font {
    std::uint32_t line_height{};
    std::uint32_t first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs;
};

struct ItemData {
    int pocket = -1, type = 0, field_func = 0, sort = 0, hp_rcv = 0, icon_id = -1, pp_rcv = 0;
    std::uint32_t flags = 0;
    std::array<int, 3> friend_change{};
};

struct WazaInfo {
    int type = -1, category = 0, power = 0, accuracy = 0, base_pp = 0; // category: 0 status, 1 physical, 2 special
};

// Type effectiveness of an attack type on a (type1, type2) defender, in quarters: 0 immune,
// 1 / 2 resisted, 4 neutral, 8 / 16 super effective. Types in the game's order (0 Normal .. 17 Fairy).
int Effectiveness(int attack, int def1, int def2);

// How a wild Pokémon is met (the module names it in the game's language).
enum class Method : int { Grass, GrassDay, GrassNight, Surf, OldRod, GoodRod, SuperRod, Swarm, Radar };

struct Encounter {
    int species = 0, min_level = 0, max_level = 0;
    Method method = Method::Grass;
};

// The decoded tables; built once, off the tick thread.
struct Catalog {
    bool pearl = false; // native save cartridge version; controls dex/encounter tables
    std::vector<std::string> species, dex_descriptions, moves, move_descriptions, items, item_plurals, item_descriptions;
    std::vector<std::string> ability_names, ability_descriptions;
    std::map<int, WazaInfo> waza;
    std::map<int, int> sinnoh_numbers; // PokemonInfo.Catalog, keyed by national species
    std::map<int, ItemData> item_data;
    std::map<int, std::vector<Encounter>> encounters;
    bool encounters_ready = false;
    struct MapRow { int width = 0; std::array<int, 3> indices{}; int x = 0, y = 0; };
    std::map<int, std::vector<MapRow>> town_map;
    std::map<int, int> map_types; // MapInfo.ZoneData.MapType; 3 is MAPTYPE_ROOM
    struct MapGuide { int id = 0, x = -1, y = -1; std::string table, label; };
    std::vector<MapGuide> map_guides;
    std::map<int, std::pair<int, int>> marking_pos; // TownMapTable MarkingMapPosXZ (last row wins)
    std::vector<int> roamer_zones;                  // FieldEncountTable mvpoke[].zoneID
    struct MapGate { int view=500,color=1000; bool town=false; };
    std::map<std::string, MapGate, std::less<>> map_gates;
    // Town Map places Fly can take you to (TownMapTable.FlyingAvailablePlace)
    struct FlySpot { int zone = 0, locator = 0, x = 0, y = 0; MapGate gate; };
    std::vector<FlySpot> fly_spots;
    std::map<int, std::string> area_names;
    std::map<int, std::string> zone_labels; // MapInfo ZoneID -> MSLabel (dp_fld_areaname_display)
    std::vector<std::string> type_names;    // ss_typename by labelIndex
    // The game's own order of the species' names in this language (UIs/searchdatas/indexdata.<sfx>
    // SearchIndexData.MonsterName: pinyin in Simplified Chinese, 가나다 in Korean ...): rank by
    // species, -1 for a species the index leaves out (it has the 493 of Brilliant Diamond).
    std::vector<int> name_rank;
    struct Personal {
        int type1 = -1, type2 = -1, form_index = 0, form_max = 1;
        std::array<int, 6> base{}; // HP, Attack, Defense, Sp. Atk, Sp. Def, Speed
        std::array<int, 3> abilities{}; // tokusei1, tokusei2, tokusei3 (hidden)
        lp_dex::Info dex;               // the Pokédex page's fields (profile, training, breeding)
    };
    std::vector<Personal> personal; // index = PersonalTable id (species, then alternate forms)
    lp_dex::Table dex_table;        // the same rows with their EvolveTable entries (evolution chains)

    // Language of the text above.
    lp_lang::Info lang = lp_lang::Lookup(lp_lang::English);
    lp_lang::Detected detected;
    std::string logo_sfx = "en";               // the slot's own title logo, else en (LP's placeholder)
    lp_lang::LabelTables labels;               // lp_lang::LabelTableList, by label name
    std::vector<std::uint32_t> codepoints;     // every code point of the text (the font's glyph set)
    std::uint64_t generation = 0;              // counts installed catalogs
};

// The language of the catalog in effect.
struct LangState {
    int id = lp_lang::English;
    bool kanji = false;
    lp_lang::Variant variant = lp_lang::Variant::En;
    bool modded = false;
    std::string sfx = "en", logo_sfx = "en";
    std::uint64_t generation = 0; // changes whenever a catalog (and so every text) is replaced
};

// Rows per font atlas page (the manifest's font_page_h).
inline constexpr std::uint32_t FontPageH = 1024;

lp_unity::RangeReader MakeRomfsReader(const EdenDsmodHostApi& api);

std::string UiKey(std::string_view bundle, std::string_view sprite);
std::string IconKey(int species, int form, int gender, bool shiny);
std::string ItemKey(int item);
std::string TypeTagKey(int type, std::string_view sfx = "en"); // the TYPE pill (common_lang.<sfx>/cmn_ico_type_01_XX)
std::string WazaBodyKey(int type); // the battle move button body (sharedui/btl_bt_waza_01_body_XX)

class Assets {
public:
    explicit Assets(lp_unity::RangeReader read, bool pearl = false);
    ~Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    void SetAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode);
    void SetAstc(lp_unity::AstcDecoder decode) { unity.SetAstcDecoder(std::move(decode)); } // dev tool
    // Tick thread: the game's language (msg_lang_id, is_kanji). A change rebuilds the catalog on
    // the worker; the previous one keeps serving until the new one is installed.
    void SetLanguage(int msg_lang_id, bool kanji);
    // Follow native PlayerWork.cassetVersion when a cross-title save supplies a different version.
    void SetDataVersion(bool pearl);
    // Starts (or retries, at most every 5 s and 5 times per language) the catalog build for the
    // wanted language on a worker thread.
    void StartCatalog(std::uint64_t tick);
    bool CatalogReady() const;
    // The build gave up (every retry failed): lookups stay empty for the session.
    // `running` is read before the catalog: the worker publishes the catalog before clearing it.
    bool CatalogFailed() const { return attempts >= 5 && !running && !CatalogReady(); }
    void Stop();

    // Asset worker thread.
    std::optional<Img> Image(std::string_view key);
    // Font metrics for the manifest's font file ("<font name>:<px>"); the atlas is the
    // matching module:lp:font/<name>:<px> image.
    std::optional<Font> FontMetrics(std::string_view spec);

    // Font atlas pages and metrics follow the language: the face (lp_lang::Info::face) and a glyph
    // set made of the catalog's text plus every code point the module published (NoteText). A new
    // set is built on a worker; FontEpoch() then changes (publish it as __font_epoch) and the next
    // decode_font (FontMetrics) switches to it. Atlas pages always come from the font FontMetrics
    // returned last, so the runtime's metrics, its atlas and lp_text's measuring agree.
    std::int64_t FontEpoch() const { return font_epoch; }
    // Tick thread: code points of published text the font lacks are requested (rate-limited).
    void NoteText(std::string_view text) const;
    // Tick thread: starts a font build when the catalog's language or the requested glyphs changed.
    void PumpFont(std::uint64_t tick);

    // Tick thread: lookups into the built catalog (empty / nullopt until it is ready).
    LangState Language() const;
    // The text of `label` in message table `table` (without the language prefix, one of
    // lp_lang::LabelTableList) in the current language, else English, else "".
    std::string Label(std::string_view table, std::string_view label) const;
    std::string TypeName(int type) const; // ss_typename by labelIndex (0 Normal .. 17 Fairy)
    std::vector<int> NameRank() const;    // Catalog::name_rank
    std::vector<std::uint32_t> Codepoints() const; // every code point of the catalog's text
    std::vector<std::uint32_t> FontMissing();      // code points the served font's face lacks
    std::string SpeciesName(int species) const;
    std::string DexDescription(int species) const;
    std::string AreaName(int zone) const;
    std::pair<int,int> MapMasks(const std::array<int32_t,500>& work,const std::array<std::uint8_t,1000>& sys) const;
    bool MapRoom(int zone) const;
    std::optional<std::pair<int, int>> MapCell(int zone, int grid_x, int grid_y) const;
    std::optional<Catalog::MapGuide> MapGuide(int sequence) const;
    // Pokétch Marking Map cell of a roamer in zone-index `index` (mvpoke table), or nullopt.
    std::optional<std::pair<int, int>> RoamerCell(int index) const;
    // Zones whose town-map cell (NowPosXZ) is (x, y), in table order.
    std::vector<int> ZonesAtCell(int x, int y) const;
    std::string AbilityName(int ability) const;
    std::string AbilityDescription(int ability) const;
    // The Fly destination at a Town Map cell, if any.
    std::optional<Catalog::FlySpot> FlySpotAt(int x, int y) const;
    std::optional<std::array<int, 6>> BaseStats(int species, int form) const;
    std::array<int, 3> SpeciesAbilities(int species, int form) const; // 0 = none
    std::vector<Encounter> Encounters(int zone) const;
    bool EncounterDataReady() const;
    std::string WazaName(int waza) const;
    std::string WazaDescription(int waza) const;
    std::string ItemName(int item) const;
    std::string ItemPlural(int item) const;
    std::string ItemDescription(int item) const;
    std::optional<WazaInfo> Waza(int waza) const;
    std::optional<ItemData> Item(int item) const;
    std::optional<std::pair<int, int>> SpeciesTypes(int species, int form) const;
    // The Pokédex page: a species' form's PersonalTable fields, its valid forms (0 first; {0} for a
    // species without alternate forms), its whole evolution chain (lp_dex::Chain), where it is met
    // in the wild (zone, encounter; zones in id order) and a zone's Town Map cell.
    std::optional<lp_dex::Info> DexInfo(int species, int form) const;
    std::map<int, int> SinnohNumbers() const;
    std::vector<int> ValidForms(int species) const;
    std::vector<lp_dex::Member> EvolutionChain(int species, int form) const;
    std::vector<std::pair<int, Encounter>> SpeciesEncounters(int species) const;
    std::optional<std::pair<int, int>> ZoneCell(int zone) const;

private:
    struct BuiltFont {
        std::string face;
        std::uint32_t px = 0;
        std::vector<std::uint32_t> codepoints;
        lp_unity::FontAtlas atlas;
    };
    std::shared_ptr<const BuiltFont> BuildFont(const lp_lang::Info& lang, std::uint32_t px,
                                               std::vector<std::uint32_t> codepoints);
    // a game face's font file, cached for the last two faces (nullptr: cannot be read)
    std::shared_ptr<const std::vector<std::uint8_t>> FaceData(std::string_view face, std::string_view font_bundle);
    bool IsLumi() const;
    void BuildData(Catalog& c);
    void BuildText(Catalog& c, const lp_lang::Info& lang);
    std::shared_ptr<const lp_unity::Image> NurseryTitle();
    // catalog->*list[i] for 0 < i < size, else "" (no catalog yet: "")
    std::string CatalogText(std::vector<std::string> Catalog::*list, int i) const;
    // PersonalTable row of a species' form (the base row when the form has none); mutex held.
    const Catalog::Personal* PersonalLocked(int species, int form) const;

    const bool pearl_title;
    std::atomic<bool> wanted_pearl{false};
    lp_unity::RangeReader read;
    lp_unity::Reader unity;
    lp_poketch::SpriteCache poketch_cache;
    std::mutex poketch_text_mutex;
    std::map<std::string, std::shared_ptr<const lp_unity::Image>> poketch_text;
    mutable std::mutex mutex;
    std::shared_ptr<const Catalog> catalog;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<int> attempts{0}; // tick thread writes, the asset worker reads (CatalogFailed)
    std::uint64_t retry_tick = 0;
    std::atomic<int> wanted_lang{lp_lang::English * 2}; // msg_lang_id * 2 + kanji
    std::uint64_t generations = 0;                      // mutex

    // Font. font_mutex guards served / prepared / face_cache.
    std::mutex font_mutex;
    std::uint32_t font_px = 0;
    std::shared_ptr<const BuiltFont> served;   // what the runtime decoded last (its atlas pages)
    std::shared_ptr<const BuiltFont> prepared; // the newest build, served at the next decode
    std::array<std::pair<std::string, std::shared_ptr<const std::vector<std::uint8_t>>>, 2> face_cache; // newest first
    std::atomic<std::int64_t> font_epoch{0};
    std::thread font_worker;
    std::atomic<bool> font_running{false};
    // tick thread only
    std::shared_ptr<const Catalog> font_catalog;           // the catalog the glyph set was made for
    mutable lp_lang::CodepointSet font_known;              // requested code points (present or not)
    mutable std::vector<std::uint32_t> font_extra;         // from NoteText, kept across languages
    mutable bool font_more = false;                        // NoteText found new code points
    bool font_due = false;
    std::uint64_t font_next_tick = 0;
};

} // namespace lp_assets

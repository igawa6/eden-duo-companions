// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kirby_assets.h"
#include "kirby_files.h"
#include "kirby_font.h"
#include "kirby_layout.h"
#include "kirby_paint.h"

// The mode select page: the main mode's story, extra and Magolor Epilogue (TitleMenu ModeSelectBtn*,
// the pause screen's Lor) over Merry Magoland's attractions (their tickets and shots).
namespace KirbyModes {
using KirbyAssets::Image;

struct Sources {
    KirbyFiles::Sources files;
    std::function<std::shared_ptr<const Image>(const std::string&)> texture; // "lyt/<archive>/<texture>"
    std::function<std::optional<KirbyLayout::Layout>(const std::string&, const std::string&)> layout; // archive, name
};

struct Mode {
    bool locked{};
    int percent{}, lives{}, stars{}, spheres{}, parts{};
};

struct Epilogue {
    bool locked{}, complete{};
    int magic{};
    int level{}; // Magolor's level, 10 = MAX
};

// the ten games in the game's own order, then the extra and the final extra missions
constexpr int Attractions = 12;
constexpr std::array<int, Attractions> Missions{8, 7, 9, 9, 8, 9, 9, 8, 9, 9, 15, 20};

struct Magoland {
    bool locked{};
    bool finals{}; // the final extra missions are open
    std::array<int, Attractions> done{};
};

struct Texts {
    std::u32string story, extra, magolor;
    std::array<std::u32string, Attractions> titles; // lines split by '\n'
};

// Left clear for Magolor's picture, which the page shows from the player's own file (844, 246, 200 x 200
// in the top panel, centred in the hexagon, shown 3.25% larger), and his level drawn over its bottom right.
constexpr std::array<int, 4> MagolorBox{851, 252, 206, 206}, LevelBox{961, 395, 114, 54};

/// Magolor's level ("Lv N" on one line, or "MAX" for 10, its letters in the complete files' rainbow
/// at `phase` 0..1) as a w x h picture.
std::optional<Image> Level(const Sources& src, int level, std::uint32_t w, std::uint32_t h, float phase = 0);

/// The main mode's own menu: `checker` (1240 x 1080) under ModeSelectBg's dots, framed by CmnMenuFrame's
/// mode select bars (no title, no back button).
std::optional<Image> MainMenuPage(const Sources& src, const Image& checker);

/// Past Adventures', the Jukebox's and the Theater's page: the Theater's stage with its curtain closed (1240 x 1080).
std::optional<Image> CurtainPage(const Sources& src);

// The story or the extra mode, as the main mode's menu shows it under the cursor.
struct World {
    std::u32string number;                  // "World 1"
    std::vector<KirbyAssets::MsbtRun> name; // with its coloured initials
    int spheres{}, max{};
    bool beaten{}; // its boss: the Lor's part (worlds 1-5), or Metal General's and Landia's mask
};

constexpr int Worlds = 7, Challenges = 9;

struct Adventure {
    bool extra{};
    std::u32string title;
    int lives{}, stars{}, parts{};
    int open{}; // worlds open so far
    std::array<World, Worlds> worlds;
    // the Lor's ability challenges in the game's order (Sword, Whip, Water, Wing, Bomb, Hi-Jump, Sand,
    // Mecha, then Carry): -1 not played, else the medal (0 none, 1 copper .. 4 platinum)
    std::array<int, Challenges> medals{-1, -1, -1, -1, -1, -1, -1, -1, -1};
};

// The menu's mode page comes in two pictures, each slid in and out on its own: the mode's window
// (on the page at PanelBox: x, y, w, h) and its name for the title bar (at TitleBox).
constexpr std::array<int, 4> PanelBox{28, 184, 1184, 828}, TitleBox{390, 8, 480, 66};

/// The mode's window: Kirby's lives and stars, the ability challenges and their medals, the Lor and
/// each open world with its energy spheres and its boss's prize.
std::optional<Image> AdventurePanel(const Sources& src, const Adventure& a);
/// The mode's name as the mode buttons write it, for the title bar.
std::optional<Image> AdventureTitle(const Sources& src, bool extra, const std::u32string& title);

// The Magolor Epilogue under the cursor. Medals: -2 hidden (stage still locked), -1 its empty socket,
// 0 copper .. 3 platinum. Techniques in the game's own order (Mahoroa.msbt SkillName_*: Magic Ball ..
// Vitality, Craftiness).
constexpr int Techniques = 12, Areas = 4;

struct EpilogueArea {
    std::u32string number, name;
    std::vector<int> medals; // the stages (and, in area 4, the extra stage)
    int boss{-1};
    std::array<int, 3> trials{-1, -1, -1};  // their technique
    std::array<int, 3> trial_state{};       // 0 locked, 1 open, 2 cleared
};

struct EpiloguePage {
    std::u32string title;
    int magic{};
    int sprout{}; // 0 none, 1 silver, 2 complete
    std::array<int, Techniques> level{}; // 0: not learnt yet
    std::array<int, Techniques> max{};
    int open{}; // areas open
    std::array<EpilogueArea, Areas> areas;
};

// Magolor's picture and his level, which the page leaves to their own widgets, in page pixels.
constexpr std::array<int, 4> EpilogueMagolor{187, 322, 300, 300}, EpilogueLevel{357, 560, 130, 62};

/// The Magolor Epilogue's window: magic points, the sprout, Magolor's techniques and each open area
/// with its medals, its trials and its boss.
std::optional<Image> EpiloguePanel(const Sources& src, const EpiloguePage& e);
/// The Epilogue's name for the title bar.
std::optional<Image> EpilogueTitle(const Sources& src, const std::u32string& title);

// The Arena or the True Arena under the cursor: its best times, alone and together. A record holds the
// rounds reached, the time in frames (-1 none) and up to four characters (KirbyPink, KirbyYellow,
// KirbyBlue, KirbyGreen, Meta, Dedede, Wado; others none).
struct ArenaRecord {
    int frames{-1}, rounds{};
    std::array<int, 4> players{-1, -1, -1, -1};
    bool gift{}, amiibo{}; // a souvenir or an amiibo used along the run
};

struct ArenaPage {
    bool final{};   // the True Arena
    bool cleared{}; // beaten at least once: the frame's stars
    int rounds{};   // a whole run
    std::array<ArenaRecord, 6> records; // three alone, three together
    std::u32string alone, together, ok, none; // the boards' titles, the clear badge, an empty time
    std::u32string round;                     // "Round " for a run that fell short
};

std::optional<Image> ArenaPanel(const Sources& src, const ArenaPage& a);
/// The Arena's name for the title bar.
std::optional<Image> ArenaTitle(const Sources& src, bool final, const std::u32string& title);

// The menu's small buttons (cursor ids 3 theater, 4 jukebox, 5 Merry Magoland, 6 previous adventures),
// each shown alone in the window as the menu draws it under the cursor, centred on MenuButtonAt (page
// pixels) at MenuButtonScale of its 1920-wide size; its cursor outline apart, to pulse.
constexpr std::array<float, 2> MenuButtonAt{620, 598};
constexpr float MenuButtonScale = 1.6f;

std::optional<Image> MenuButton(const Sources& src, int button, const std::u32string& text);
std::optional<Image> MenuButtonCursor(const Sources& src, int button);
/// The menu's own title ("Mode select") as the game writes it in the bar.
std::optional<Image> MenuTitle(const Sources& src, const std::u32string& title);

/// Magolor's portrait: the Magolor Epilogue pause screen's picture (MahoroaPause PauseAbilityPic39) with its
/// background masked out, cropped square around him.
std::optional<Image> MagolorPortrait(const Sources& src);

/// The world map's sky behind Pop Star, as a w x h picture: WMap/Map/Popstar/Bg's space dome, the Milky
/// Way, its haze, coloured stars, sparkles, its faint ring and the planet's warm glow below.
std::optional<Image> WorldSky(const Sources& src, std::uint32_t w, std::uint32_t h);

// The world map's world selection (out of the worlds' hubs), as the sketch lays it out: the sky, the
// planet Kirby is on, ChallengeTutorialFrameGuideT01's bar widened to the page (its blues turned red in the
// extra mode) with the lives, stars and energy spheres and WorldName's planet name. The icons are widgets
// of their own (MapSlots), to bounce and push.
struct WorldMap {
    bool halcandra{}, extra{};
    int lives{}, stars{}, spheres{};
    std::u32string name;
};

/// `sky` is the page's 1240 x 1080 sky, `planet` the planet's 1000 x 1000 render.
std::optional<Image> WorldMapPage(const Sources& src, const Image& sky, const Image& planet, const WorldMap& w);

// The page's icons (keep in sync with WORLD_SLOTS in gen_manifest.py): Pop Star's worlds 1-5 and the Lor,
// Halcandra's worlds 6-7 and the Lor, then the other planet's icon; world -1 is the Lor, 7 the other
// planet. Centre and size in page pixels; `disc` is the round icon's own diameter inside its picture.
struct MapSlot {
    int world;
    bool halcandra;
    float x, y, size, disc;
};
constexpr std::array<MapSlot, 10> MapSlots{{{0, false, 618, 427, 108, 97.2f},
                                            {1, false, 919, 565, 108, 97.2f},
                                            {2, false, 835, 857, 108, 97.2f},
                                            {3, false, 379, 867, 108, 97.2f},
                                            {4, false, 303, 577, 108, 97.2f},
                                            {-1, false, 622, 645, 171, 153.9f},
                                            {5, true, 672, 820, 126, 113.4f},
                                            {6, true, 628, 557, 126, 113.4f},
                                            {-1, true, 362, 921, 126, 113.4f},
                                            {7, false, 1154, 1000, 124, 124}}};
// the cursor's ring, CursorMargin px around the disc
constexpr float CursorMargin = 10;

/// The pink ring the file select's cursor draws, round, around a disc `disc` px across.
std::optional<Image> WorldCursor(float disc);

/// A planet's icon (WorldIcon*) in a thin white ring and a yellow one, OtherPlanetSize px square.
constexpr float OtherPlanetSize = 128;
std::optional<Image> OtherPlanetIcon(const Sources& src, bool halcandra);

// The stage page's round buttons: a disc in a lighter (or darker) rim, a glyph drawn from scratch over
// its upper half and the label below. Pause: the pause menu's Kirby cut out in navy (Continue: its standing
// Kirby on a yellow button); Discard: the ability
// star (its mask) with three motion lines.
enum class RoundIcon { Pause, Continue, Discard, MagolorPause, MagolorContinue };
std::optional<Image> RoundButton(const Sources& src, RoundIcon icon, const std::u32string& label, std::uint32_t size,
                                 bool disabled = false);

// The stage page (the "Game" tab): the world map's bar, its stars taken out, with Kirby's lives and point
// stars, the device's charge (its level; the user's battery frame is a widget at BatteryBox) and, in a stage,
// the Lor with the stage's energy spheres (`got`: one bit per sphere, as the game's HUD shows them), or in a
// boss's stage with the Lor's part (a mask in worlds 6 and 7) its boss gives.
constexpr std::uint32_t PlayBarHeight = 260;
constexpr std::array<int, 4> BatteryBox{280, 10, 42, 66};
struct PlayBar {
    bool extra{};
    int lives{}, stars{};
    int charge{100}; // percent, -1 none (no battery shown)
    bool charging{};
    int spheres{};          // 0: not in a stage
    unsigned got{};
    int boss{-1};           // a boss's stage: its world (0..6), else -1
    bool beaten{};
};
std::optional<Image> PlayBarPicture(const Sources& src, const PlayBar& p);

// Magolor Epilogue's page. Its bar: the challenge result's header (2000 x 316) at the page's width in teal,
// Magolor's emblem in its star slot, his level in its long slot, the magic points and the device's charge
// (the user's battery frame is a widget at EpiBatteryBox).
inline constexpr std::array<std::uint32_t, 2> EpilogueBarSize{1240, 196};
constexpr std::array<int, 4> EpiBatteryBox{1084, 8, 42, 66};
struct EpilogueTop {
    std::u32string level; // "Level 1 | -8 to Level 2"
    int magic{};
    int charge{100};
    bool charging{};
};
std::optional<Image> EpilogueBar(const Sources& src, const EpilogueTop& t);

// The Arenas' page: the Epilogue's bar in the header's own yellow, the character's ability star in its slot (the
// star's colours as CmnAbilityStarIcon_<name> sets them, linear) and the time so far in the middle of the long slot;
// the name plates under the player's figure (pink, slanted on the right) and the boss's (red with yellow edges,
// slanted on the left).
// (the run's time is a label of the page over it, in the game's font: it changes every frame)
struct ArenaTop {
    std::string star; // AbilityStarIcon<star>^t
    KirbyPaint::Rgb black, white;
    int charge{100};
    bool charging{};
};
std::optional<Image> ArenaBar(const Sources& src, const ArenaTop& t);
enum class Plate { Player, Boss };
// the Arenas' cards (panels and plates) run on past their outer side by this much, off the page
constexpr std::uint32_t ArenaCardExtend = 160;
constexpr std::array<std::uint32_t, 2> PlayerPlateSize{480, 104}, BossPlateSize{660, 100};
constexpr std::uint32_t ArenaBossSize = 520; // the boss's picture (KirbyChara), in its card cut to the panel
// Kirby drawn in a square this big, his body framed alone (the same for every ability) at this fill, its centre at
// ArenaKirbyCentre of his panel; hats and weapons may reach out, cut by the panel's rim
constexpr std::uint32_t ArenaKirbyPicture = 760;
constexpr float ArenaKirbyFill = 3.6f;
constexpr std::array<float, 2> ArenaKirbyCentre{225, 240};
// (the player's plate takes the character's colour: Kirby, Metaknight, Dedede, Waddledee)
// (Kirby's in his spray's colours with `tint`: its skin and its name's ink, RGB)
std::optional<Image> NamePlate(const Sources& src, Plate kind, const std::u32string& name, const std::string& character = "Kirby",
                               std::optional<std::array<std::uint32_t, 2>> tint = {});
constexpr std::array<std::uint32_t, 2> PlayerPanelSize{640, 420}, BossPanelSize{600, 400};
std::optional<Image> FigurePanel(const Sources& src, Plate kind, std::uint32_t width, std::uint32_t height);
// the "VS" burst shown before a boss fight (styles 0 to 2), `size` square, and the sparks round the frames then:
// the player's panel (0), the boss's (1), the burst (2), frames 0 to VsSparkFrames - 1
std::optional<Image> VsBurst(const Sources& src, int style, std::uint32_t size);
constexpr int VsSparkFrames = 4;
// the plates' places from their panels' (the page's ARENA_PLATES less ARENA_PANELS), and the sparks' pictures from the
// panels: x, y, width, height
constexpr std::array<std::array<float, 2>, 2> PlateAt{{{0, 332}, {-60, 340}}};
constexpr std::array<std::array<int, 4>, 2> VsSparksBox{{{0, 0, 640, 456}, {-80, 0, 680, 456}}};
constexpr std::uint32_t VsSize = 400;
std::optional<Image> VsSparks(int which, int frame, std::uint32_t width, std::uint32_t height);
// The VS scene's moves at tick t (60 a second) of it: the cards' offsets from their places (the player's draws back
// left and down, the boss's right and up, then they rush at each other and clash in the middle, shaking), the burst's
// scale (0 = not shown) bumping up from the clash, the sparks on from a little later
struct VsPose {
    std::array<float, 2> player{}, boss{};
    float burst{};
    bool sparks{};
};
constexpr int VsWindTicks = 14, VsDashTicks = 10, VsFadeTicks = 18;
VsPose VsScene(float t);
// the burst's place on the page (VsSize square), between the clashed cards
constexpr std::array<int, 2> VsBurstAt{420, 340};
// a figure (its square picture shown `size` wide at x, y) inside a panel of that size, cut along the panel's rim
Image PanelPicture(Plate kind, std::uint32_t width, std::uint32_t height, const Image& figure, float x, float y, float size);
/// The gauge's buttons, Magolor Cannon (0), Black Hole (1), Heal (2) and Magolor Blast (3): locked ("???", greyed),
/// waiting for a full gauge (darkened) or ready.
enum class SpecialState { Locked, Waiting, Ready };
constexpr std::uint32_t SpecialSize = 231, EpiRoundSize = 330; // the specials; Pause and Continue
std::optional<Image> SpecialButton(const Sources& src, int which, const std::u32string& label, std::uint32_t size, SpecialState state);
/// The special gauge as a ring (MahoroaSpecialSkill's bar bent round), in layers: the fill (charging or
/// full), two sparkle layers, the glow of a full gauge, the empty track over each of its segments
/// (shown past the gauge's level, cropped to GaugeSegmentBox), the edge (one segment's track, turned to the
/// exact level to cover the rest of the segment it is in) and the front (its rims and the flask's
/// plate); a locked gauge is one greyed picture with "???".
constexpr std::uint32_t GaugeSize = 600;
constexpr float GaugeInner = 192, GaugeOuter = 288, GaugePlate = 104;
constexpr int GaugeSegments = 60;
enum class GaugePart { Charging, Full, Particles1, Particles2, Glow, Empty, Edge, Front, Locked, Rainbow };
std::array<int, 4> GaugeSegmentBox(int segment); // x, y, w, h in the gauge's picture
std::optional<Image> GaugePiece(const Sources& src, GaugePart part, int segment = 0);
/// The overflowing magic round a full gauge before the final blow: flames of every colour licking out of the ring
/// (`layer` 0 and 1, shaped apart, to turn against each other) and sparks in the halo (`layer` 2), on a bigger
/// square sharing the gauge's centre
constexpr std::uint32_t GaugeAuraSize = 760;
Image GaugeAura(int layer);
/// The Epilogue's tutorial window (MahoroaTutorialDialogWin) for a magic just learnt: the title, the magic's
/// button (`icon`) in the help picture's frame and the message, [bracketed] words in the game's highlights
/// (yellow in the title, orange in the message). Its button (MahoroaCmnBtnS, lit) is a picture of its own, at
/// PopupButtonAt in the window.
constexpr std::array<std::uint32_t, 2> PopupSize{1160, 1000}, PopupButtonSize{500, 122};
constexpr std::array<int, 2> PopupButtonAt{330, 772};
std::optional<Image> MagicPopup(const Sources& src, const std::u32string& title, const std::u32string& message, const Image& icon);
/// Kirby's colours just unlocked, in the story's help window (Cmn/Help): the title, their spray cans side by side on a
/// panel, the message ([bracketed] words red); its button (Cmn/Btn's, lit) a picture of its own, PopupSize and
/// PopupButtonAt as the Epilogue's.
std::optional<Image> SprayPopup(const Sources& src, const std::u32string& title, const std::u32string& message, std::span<const Image> sprays);
std::optional<Image> SprayButton(const Sources& src, const std::u32string& label);
/// The popup's title and message in the game's language (Italian, else English): the spheres got (red), then the colours'
/// names, run on to a third line when long.
std::pair<std::u32string, std::u32string> SprayWords(std::string_view dir, int spheres, std::span<const std::u32string> names);
std::optional<Image> PopupButton(const Sources& src, const std::u32string& label);
/// The amiibo button: white under the bar's checker band, out to its diagonal, with the amiibo logo (at
/// AmiiboBox on the page). Its face shows from AmiiboFace[0] to [1]; the rest of the box is tucked
/// under the bar and past the screen's left edge.
constexpr std::array<int, 4> AmiiboBox{-40, 75, 740, 149};
constexpr std::array<int, 2> AmiiboFace{99, 212};
constexpr int AmiiboTuck = 60;
std::optional<Image> AmiiboButton(const Sources& src, bool disabled = false);
/// The souvenir slot (StockItemInfo, its ring blue in the extra mode) with the item in it (1..5, the save's
/// order; 0 none).
std::optional<Image> SouvenirButton(const Sources& src, int item, bool extra, bool disabled, std::uint32_t size);
/// A tab at the page's bottom, a glossy box with rounded top corners, open at the bottom (0 red, 1 blue,
/// 2 green, 3 teal, 4 purple), with its label.
std::optional<Image> TabButton(const Sources& src, int colour, const std::u32string& label, std::uint32_t w, std::uint32_t h);
// the page's controls (keep in sync with PLAY_* in gen_manifest.py): 0 pause, 1 drop ability, 2 souvenir,
// 3 amiibo, 4..6 the tabs
constexpr int PlayButtons = 7;
constexpr std::uint32_t RoundSize = 300, SouvenirSize = 780;
// the stage page's are narrower, leaving room for the colour menu's ribbon (ColourRibbon)
constexpr std::array<std::uint32_t, 2> TabSize{400, 110}, StageTabSize{358, 110};

// The stage page's "Abilities" tab, drawn from the pause screen's ability pages: the description
// (PauseWinCopyInfo: PauseWinBase05 widened by repeating its checker, the frame, the ability's picture and
// its text in at most three lines), the page counter (W_PageNum) and the moves (PauseWinCommand, rows of
// PauseCommandText). Texts are msbt runs with the button font's glyphs (KirbyAssets::MsbtRuns glyphs).
struct MoveRow {
    enum class Kind { Normal, Long, Wide } kind{};
    std::vector<KirbyAssets::MsbtRun> command, name;
};
constexpr std::array<std::uint32_t, 2> AbilityInfoSize{800, 196}, AbilityMovesSize{996, 542}, AbilityPillSize{190, 58},
                                       AbilityArrowSize{110, 142};
constexpr int AbilityRows = 4; // a page's height in normal rows (a wide one takes two)
/// An ability's pause pages in the game's language (msg/Ark/<dir>): its description, its moves in the
/// pages yaml/Cmn/Pause.bin lists (Content.Abl.<name>) and its picture (PauseAbilityPicNN). `ability` is
/// the copy-ability id (0 none .. 38 Magolor).
struct AbilityPages {
    std::vector<KirbyAssets::MsbtRun> info;
    std::vector<std::vector<MoveRow>> pages;
    std::string picture; // texture key
};
/// The copy ability's name in the game's files (Pause.msbt Abl<name>, AbilityStarIcon<name>), "" past the table.
std::string AbilityKey(int ability);
std::optional<AbilityPages> AbilityContent(const KirbyAssets::RomfsReader& read, const std::string& dir, int ability);
/// The Epilogue's Magic tab: Magolor's moves as his pause screen lists them for his technique levels (msbt
/// order, Magic Ball .. Hover; 0 not learnt), with the description and picture of a Magolor whose powers are
/// all back (`restored`) or not.
constexpr std::size_t MagolorTechniques = 10;
std::optional<AbilityPages> MagolorContent(const KirbyAssets::RomfsReader& read, const std::string& dir,
                                           const std::array<int, MagolorTechniques>& levels, bool restored);
/// The Magic tab's strip of learnt techniques, one cell each in MahoroaTechniqueTop's order (TechniqueMenuOrder):
/// the technique in its socket with its level or MAX, as the technique menu shows it.
constexpr std::array<int, Techniques> TechniqueMenuOrder{0, 1, 9, 10, 8, 11, 2, 3, 6, 4, 5, 7};
constexpr std::array<std::uint32_t, 2> TechniqueCellSize{150, 88};
std::optional<Image> TechniqueCell(const Sources& src, int technique, int level);
/// Whose pause screen the pictures take their look from: Kirby's (Pause) or Magolor's (MahoroaPause).
enum class PauseLook { Kirby, Magolor };
std::optional<Image> AbilityInfo(const Sources& src, const KirbyFont::IconFont* icons,
                                 const std::vector<KirbyAssets::MsbtRun>& text, const std::string& picture,
                                 PauseLook look = PauseLook::Kirby);
std::optional<Image> AbilityMoves(const Sources& src, const KirbyFont::IconFont* icons, const std::vector<MoveRow>& rows,
                                  std::array<std::uint32_t, 2> size = AbilityMovesSize, PauseLook look = PauseLook::Kirby);
std::optional<Image> AbilityPill(const Sources& src, int page, int pages, std::array<std::uint32_t, 2> size = AbilityPillSize,
                                 PauseLook look = PauseLook::Kirby);
std::optional<Image> AbilityArrow(const Sources& src, bool left, bool disabled, PauseLook look = PauseLook::Kirby);
/// The Epilogue page's background icon: the gear of the Epilogue's wipe (WipeBIconGear, its left half
/// mirrored) in white, a little blurred, on PauseBg's 270 px square.
std::optional<Image> BackgroundGear(const Sources& src);
/// Two of Kirby's arrows one over the other, for a bigger step (DoubleArrowSize).
constexpr std::array<std::uint32_t, 2> DoubleArrowSize{160, 142};
std::optional<Image> DoubleArrow(const Sources& src, bool left);

// The stage page's "Options" tab: green panels in the stage page's button style. A row of the list (its
// value on the right, or an arrow when `value` is empty), a sub-page's panel (its title in a darker band,
// left clear for the back arrow at OptionBack), a setting's line ("label   < value >", on the panel), the
// cheat's ability card (the ability's star, its name below) and the red Apply button.
constexpr std::array<std::uint32_t, 2> OptionRowSize{1180, 104}, OptionPanelSize{1180, 700}, OptionLineSize{1100, 64},
                                       CheatCardSize{180, 180};
constexpr std::array<int, 4> OptionBack{24, 14, 72, 72}; // in the panel
constexpr int OptionHeader = 100;
struct OptionWords {
    std::u32string background, yes, no, hud, cheats, note, show, hide, ability, apply;
    std::array<std::u32string, 4> counters; // spheres, lives, point stars, souvenir
    std::u32string magic, energy, fill, edit; // the Epilogue's counters and cheats
};
constexpr std::array<std::uint32_t, 2> EpiApplySize{200, 100}; // the Epilogue's cheats' Apply, in a row
/// The Options tab's own texts in the game's language (msg/Ark/<dir>), English when there are none.
OptionWords OptionTexts(const std::string& dir);
std::optional<Image> OptionRow(const Sources& src, const std::u32string& label, const std::u32string& value,
                               std::string_view font = {});
/// `rules`: light lines across the panel at these heights (between a page's rows).
std::optional<Image> OptionPanel(const Sources& src, const std::u32string& title, const std::u32string& note,
                                 std::span<const float> rules = {});
std::optional<Image> OptionLine(const Sources& src, const std::u32string& label, const std::u32string& value);
/// `black` / `white`: the star's material colours (CmnAbilityStarIcon_<ability>.bflan, linear).
std::optional<Image> CheatCard(const Sources& src, const std::string& icon, const std::u32string& name, KirbyPaint::Rgb black,
                               KirbyPaint::Rgb white);
std::optional<Image> CheatApply(const Sources& src, const std::u32string& label, bool disabled,
                                std::array<std::uint32_t, 2> size = CheatCardSize);
/// The Epilogue's magic points cheat: the card with the magic points' icon and the amount it gives (or takes).
std::optional<Image> MagicPointsCard(const Sources& src, int amount);

std::optional<Image> Page(const Sources& src, const Image& backdrop, const Texts& text, const Mode& story,
                          const Mode& extra, const Epilogue& epilogue, const Magoland& magoland);

// Kirby's colour menu (the user's sketch, 2026-10-28), a popup in Merry Magoland's look: its window's blue checked band
// edged in gold round its yellow paper. The frame leaves the slots' window open (ColourWindow, the white under it being
// a widget of its own); over it the name bar's contents, Apply, the slots, the scroll thumb and the back button are
// pictures of their own. Boxes in the popup's pixels: x0, y0, x1, y1.
constexpr std::array<std::uint32_t, 2> ColourPopupSize{1160, 1000};
constexpr std::array<float, 4> ColourBar{36, 36, 888, 140}, ColourApplyBox{906, 34, 1126, 148}, ColourPanel{36, 164, 1124, 964};
constexpr float ColourSeparator = 1062; // the slots' window ends there; the scroll track runs on to the panel's edge
constexpr std::array<float, 4> ColourWindow{36, 164, ColourSeparator, 964}, ColourTrack{1070, 180, 1118, 948};
constexpr int ColourColumns = 7;
constexpr std::array<float, 2> ColourFirst{73, 186}, ColourPitch{139, 136};
constexpr std::array<std::uint32_t, 2> ColourSlotSize{118, 120}, ColourThumbSize{48, 160};
constexpr std::uint32_t ColourSlotRing = 7; // a slot's picture runs this far past its box for the selection's ring
// a slot reaching past the popup's bottom is drawn cut, its picture kept down to a multiple of ColourCutStep rows that ends
// within the frame's band under the window (ColourWindow's bottom .. ColourCutLimit)
constexpr float ColourCutStep = 32, ColourCutLimit = 996;
constexpr std::array<float, 3> ColourBackCircle{1112, 958, 66}; // centre and radius, over the frame's corner
constexpr std::uint32_t ColourBackSize = 148;
std::optional<Image> ColourFrame(const Sources& src);
/// The name bar's contents: the colour's name outlined in its ink over a depth in the shoes' colour, between two Kirbys in
/// that colour.
std::optional<Image> ColourHeader(const Sources& src, const std::u32string& name, KirbyPaint::Rgb ink, KirbyPaint::Rgb depth,
                                  const Image* kirby, std::string_view font = {});
std::optional<Image> ColourApply(const Sources& src, const std::u32string& label, bool disabled);
/// A slot of the grid with the colour's spray can, ringed in gold when `selected`.
Image ColourSlot(const Image& spray, bool selected);
/// A slot for a colour still to come: a "?" in place of the can.
std::optional<Image> ColourUnknown(const Sources& src);
Image ColourThumb();
std::optional<Image> ColourBack(const Sources& src);
/// The pages' button that opens the menu: a yellow ribbon rising from the screen's bottom edge, notched at its top, with
/// the current colour's spray can.
constexpr std::array<std::uint32_t, 2> ColourRibbonSize{112, 168};
std::optional<Image> ColourRibbon(const Sources& src, const Image& spray);
// Magolor Soul's words on the Arena page (Merry Magoland's count style: VDL-LogoG-Ultra blue, a white edge, a yellow
// ring), a picture just big enough for `text` at `cap` px
std::optional<Image> SoulWords(const Sources& src, const std::u32string& text, float cap);
// a word in the name plates' lettering (Kurokane white, a dark edge), a picture just big enough
std::optional<Image> PlateWords(const Sources& src, const std::u32string& text, float cap);

// The DSMod's own credits (credits.txt, user 2026-10-25), shown during the game's staff roll: cards of a title and
// entries ("name | role"), or of text ("[]": lines wrapped, a "—" line as the signature)
struct CreditCard {
    std::u32string title;
    std::vector<std::pair<std::u32string, std::u32string>> entries;
    bool text{};
};
std::vector<CreditCard> ParseCredits(std::string_view utf8);
constexpr std::uint32_t CreditCardWidth = 1000;
std::optional<Image> CreditCardImage(const Sources& src, const CreditCard& card);
// The cards' tops on the page `t` seconds into a staff roll of `length` seconds: they rise from below the page and
// leave at its top, spread over the roll; the last stops in the middle until the end
constexpr float CreditsLead = 6, CreditsSpeed = 24, CreditsHold = 40;
std::vector<float> CreditsTops(const std::vector<float>& heights, float t, float length);

} // namespace KirbyModes

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kirby_assets.h"

// The game's characters (gfx/Step/Chara/<name>/Model.bfres) drawn in software in a pose of their own motions
// (Motion.bfres): skinned shapes, the outline hulls, Kirby's face patterns and the hats and weapons on his bones.
namespace KirbyChara {
using KirbyAssets::Image;

// a texture of a model's own BNTX by its full name (hash suffix included)
using Textures = std::function<std::shared_ptr<const Image>(const std::string&)>;

// a bone's local transform: scale, Euler rotation (Z * Y * X), translation
struct Srt {
    std::array<float, 3> s{1, 1, 1}, r{}, t{};
};
using Pose = std::map<std::string, Srt>;

struct Skeleton;

class Model {
public:
    // (the file's model of that name, else its first, or its "TopL" when it has several)
    explicit Model(std::span<const std::uint8_t> bfres, std::string_view name = {});
    ~Model();
    bool Valid() const;
    const Skeleton& Data() const { return *data; }

private:
    std::unique_ptr<Skeleton> data;
};

class Motion {
public:
    explicit Motion(std::span<const std::uint8_t> bfres);
    bool Has(const std::string& anim) const;
    int Frames(const std::string& anim) const;
    bool Moves(const std::string& anim) const; // some bone has a curve
    Pose Sample(const std::string& anim, float frame) const;
    // the textures a texture-pattern animation (<anim>_tp) shows (Kirby's eye, its normal map, the mouth)
    std::vector<std::string> Patterns(const std::string& anim) const;

private:
    std::vector<std::uint8_t> bytes;
    std::map<std::string, std::uint64_t> skeletal, material;
};

// a see-through part (its shapes by a piece of their name): Mecha's visor, Ice's crystals, Tornado's swirl
struct Glass {
    std::string piece;
    std::uint32_t rgb{};
    float opacity{0.6f};
    float stripes{0.6f}; // how much its texture's light lines show white
    std::uint32_t rim{0xFFFFFF}; // its colour towards its edges
};

// One model of a figure: Kirby's body, or a part on one of the first part's bones (a hat on HatL, a sword on LHaveL).
struct Part {
    const Model* model{};
    Pose pose;
    Textures textures;
    std::string bone;               // empty for the first part
    std::vector<std::string> face;  // the anim's pattern textures (Kirby's KirbyEye.05, KirbyMouth.05; a boss's)
    std::vector<std::string> hide;  // shapes left out by a piece of their name (a hat's "Big" variants)
    std::vector<std::pair<std::string, std::uint32_t>> tint;  // a shape's colour (by a piece of its name) times this RGB
    std::vector<std::pair<std::string, std::uint32_t>> paint; // or this RGB instead of its textures
    std::vector<std::string> cut;                              // shapes cut out by their colour map's alpha
    std::vector<std::array<std::uint32_t, 2>> recolour;        // texels near the first RGB shown as the second, lit
    std::vector<std::string> slit;                             // shapes darkened by their vertex colour's red
    std::string base;                                          // the colour map, by a piece of its name, when not the usual
    bool grey_marble{};                                        // a marble texture fills the colour map's greys
    std::string gold;                                          // a mask whose alpha marks gold parts
    std::uint32_t zone{};                                      // RGB for the vertex colour's black zone (0: none)
    std::pair<std::string, std::uint32_t> lens;                // a shape's bluish texels lit in this RGB (eyes)
    int zone_channel{-1};                                      // zone: the vertex colour's black (-1) or R G B (0 1 2)
    std::string zone_shape;                                    // zone: only on this shape
    std::vector<Glass> glass;                                  // see-through shapes
    Srt turn;                                                  // on its bone, turned and moved by this first
    std::vector<std::pair<std::string, std::string>> matcap;   // shapes coloured by a texture of where they face
    // an effect's model (a particle's primitive): see-through, its texture's light (moved by `scroll`) its alpha, kept
    // inside `effect_mask`'s shape, from `effect_dark` to `effect_light` by `effect_colour`'s light
    std::shared_ptr<const Image> effect, effect_mask, effect_colour;
    std::array<float, 2> scroll{}, effect_tile{1, 1}; // the texture moved, and repeated across (u) and along (v)
    std::uint32_t effect_dark{}, effect_light{0xFFFFFF};
    float effect_opacity{1};
    // Kirby's own (KbBodyC) RGB: skin, feet, blush, outline, the feet's outline, the eye's dark, its iris, highlight, rim
    std::array<std::uint32_t, 9> kirby{0xFFA8CC, 0xE4153E, 0xFF5C8A, 0x8A2452, 0x6A0718, 0x0C0A2E, 0x2B63FF, 0xFFFFFF, 0x0C0A2E};
};

struct View {
    float yaw{-28}, pitch{8}; // degrees, from the model's front
    float outline{0.02f};     // the outline hull's thickness, model units
    bool mirror{};            // drawn facing the other way
    float fill{1.25f};        // the picture's half size over the framing's radius
    float dx{};               // the framing's centre moved right, a share of the picture's width
    float dy{};               // and up, a share of its height
};

// where a figure sits in its picture: the centre and the radius to fit, kept for every frame of a loop
struct Framing {
    std::array<float, 3> centre{};
    float radius{};
};
std::optional<Framing> Bounds(const std::vector<Part>& parts, const View& view);
Framing Union(const Framing& a, const Framing& b);
// how the Arena page shows a boss (its gfx/Step/Chara folder, Ex models alike): the view, shapes left out (attack
// props) and material colours the game's shaders give
struct Look {
    View view;
    std::vector<std::string> hide;
    std::vector<std::pair<std::string, std::uint32_t>> tint;
    std::vector<std::string> face; // pattern frames shown instead of the idle's own (Water Galboros' open eye)
    std::vector<std::pair<std::string, std::uint32_t>> paint;
    std::vector<std::string> cut;
    std::vector<std::array<std::uint32_t, 2>> recolour;
    std::vector<std::string> slit;
    std::string base;
    bool grey_marble{};
    std::string gold;
    std::uint32_t zone{};
    std::pair<std::string, std::uint32_t> lens;
    int zone_channel{-1};
    std::string zone_shape;
};
Look BossLook(std::string_view folder);
void Apply(Part& part, const Look& look); // the look's shape rules on a part (its view aside)

// Kirby with an ability: the hat (a file in gfx/Step/Chara/Kirby/<ability>/, empty for none) and how it shows, and the
// weapons held (each a file there, on one of Kirby's bones)
struct Gear {
    std::string file, bone;
    std::vector<std::string> hide;
    std::string anim;  // its own pose (Parasol's open canopy), else its bind pose
    Srt turn;          // in the hand, turned (Euler degrees), grown and moved
    std::vector<Glass> glass;
    std::vector<std::pair<std::string, std::string>> matcap;
    std::vector<std::pair<std::string, std::uint32_t>> paint;
};
// a particle the game draws as a model (an effect file's primitive), on Kirby's hat: its model and textures, placed by
// `turn` (scale, Euler degrees, move) on HatL, spun about its own axis and its texture moved each of the loop's frames
struct EffectModel {
    std::string effect, primitive, texture, mask, colour;
    Srt turn;
    std::uint32_t dark{}, light{0xFFFFFF};
    float opacity{1};
    float spin{};                  // degrees a frame
    std::array<float, 2> scroll{};  // texture moved a frame
    std::array<float, 2> tile{1, 1}; // texture repeated across and along
    float phase{};                 // its turn at the loop's start, degrees
};

struct KirbyKit {
    std::string hat;
    std::vector<std::string> hat_hide;
    std::vector<std::pair<std::string, std::uint32_t>> hat_tint, hat_paint;
    std::vector<Glass> hat_glass;
    std::string hat_anim;                        // its pose, when not its GearAnim (Wing's folded wings)
    std::pair<std::string, Srt> hat_bend;        // bones (by a piece of their name) turned by more (Ultra Sword's tail)
    std::vector<Gear> gear;
    std::vector<EffectModel> effects;
};
// a hat's pose in its kit at `t` of the loop: its anim's, its bones bent
Pose HatPose(const KirbyKit& kit, const Model& hat, const Motion& motion, float t);
KirbyKit KirbyAbility(std::string_view ability);
// the Arenas' other characters (abilities Meta, Dedede, Dee): their folder in gfx/Step/Chara (empty for Kirby's own) and
// how their figure shows in its Wait
std::string_view CharacterFolder(std::string_view ability);
View CharacterView(std::string_view folder);
// the pose a hat rests in: its own Wait, Loop or Main; empty for none
std::string GearAnim(const Motion& motion);
// its pose in that anim (the hat's GearAnim when empty) at `t` of its loop (0 to 1, Kirby's loop stretched over it) on
// Kirby's bone: its own bones, not the root's move (the anims carry it around the body)
Pose GearPose(const Motion& motion, float t, const std::string& anim = {});
Pose BindPose(const Model& model); // every bone's own transform
Pose EffectPose(const Model& model, float spin); // its bind pose, its first bone turned about Y by `spin` degrees

std::optional<Image> Draw(const std::vector<Part>& parts, const View& view, const Framing& framing, std::uint32_t size, int ss);

} // namespace KirbyChara

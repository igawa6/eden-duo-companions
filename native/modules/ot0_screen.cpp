// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_screen.h"
#include "ot0_image.h"
#include "ot0_data.h"
#include "ot0_map_assets.h"
#include "ot0_enemy_assets.h"
#include "ot0_status_sprite_pins.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <utility>
namespace ot0 {
namespace {
constexpr u32 Width=1240,Height=1080;
constexpr std::array<u8,4> White{255,255,255,255},Soft{214,208,196,255},Dim{150,146,140,255};
Image Solid(u32 w,u32 h,std::array<u8,4> color) {
    Image out{w,h,std::vector<u8>(static_cast<std::size_t>(w)*h*4)};
    for (std::size_t i=0;i<out.rgba.size();i+=4) std::copy(color.begin(),color.end(),out.rgba.begin()+static_cast<std::ptrdiff_t>(i));
    return out;
}
Image Alpha(Image im,double factor) {
    for (std::size_t i=3;i<im.rgba.size();i+=4) im.rgba[i]=static_cast<u8>(std::lround(im.rgba[i]*factor));
    return im;
}
Image Scale(const Image& im,u32 w,u32 h) { return Resize(im,0,0,im.width,im.height,w,h); }
std::optional<Image> ReadImage(const RangeReader& read,MemberId id,std::string* why) {
    const auto bytes=ReadMember(read,Member(id),why);
    const auto pin=TextureLayout(id);
    return bytes && pin ? DecodeTexture(*bytes,*pin,why) : std::nullopt;
}
std::optional<MemberId> Art(u32 sprite) {
    if(sprite>=900 && sprite<=909)return MemberId::ArtLeader;
    switch(sprite) {
    case 10101:return MemberId::ArtViator;
    case 10503:return MemberId::ArtCelsus;
    case 10804:return MemberId::ArtMacy;
    case 10905:return MemberId::ArtPhenn;
    case 11407:return MemberId::ArtAlexia;
    case 40204:return MemberId::ArtStia;
    default:return std::nullopt;
    }
}
std::optional<MemberId> Sprite(u32 sprite) {
    switch(sprite) {
    case 101:return MemberId::SpriteViator;
    case 105:return MemberId::SpriteCelsus;
    case 108:return MemberId::SpriteMacy;
    case 109:return MemberId::SpritePhenn;
    case 114:return MemberId::SpriteAlexia;
    case 401:return MemberId::SpriteStia;
    default:return std::nullopt;
    }
}
Image Trim(const Image& src) {
    u32 x0=src.width,y0=src.height,x1=0,y1=0;
    for(u32 y=0;y<src.height;++y)for(u32 x=0;x<src.width;++x)
        if(src.rgba[(static_cast<std::size_t>(y)*src.width+x)*4+3]) {
            x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);
        }
    return x1>x0 && y1>y0 ? Resize(src,x0,y0,x1-x0,y1-y0,x1-x0,y1-y0):Image{};
}
Image NearestFrame(const Image& sheet) {
    constexpr u32 Cell=44,ScaleBy=3;
    if(sheet.width<Cell || sheet.height<Cell) return {};
    Image out{Cell*ScaleBy,Cell*ScaleBy,std::vector<u8>(Cell*Cell*ScaleBy*ScaleBy*4)};
    for(u32 y=0;y<out.height;++y)for(u32 x=0;x<out.width;++x) {
        const auto s=(static_cast<std::size_t>(y/ScaleBy)*sheet.width+x/ScaleBy)*4;
        auto d=(static_cast<std::size_t>(y)*out.width+x)*4;
        std::copy_n(sheet.rgba.data()+s,4,out.rgba.data()+d);
        if(out.rgba[d]==255 && out.rgba[d+1]==0 && out.rgba[d+2]==0)out.rgba[d+3]=0;
    }
    return out;
}
}
Image MaskRevealedPaths(const Image& source,const ExplorationSnapshot& map,const Image* reveal_mask) {
    if(!source.width || !source.height || source.width>4096 || source.height>4096 ||
       source.rgba.size()!=static_cast<std::size_t>(source.width)*source.height*4)return {};
    Image out{source.width,source.height,std::vector<u8>(source.rgba.size())};
    if(!map.valid || !map.block_width || map.revealed_cells.size()>65536 ||
       !std::isfinite(map.scale.x) || !std::isfinite(map.scale.y) || map.scale.x<=0 || map.scale.y<=0 ||
       std::abs(map.axis.x)!=1 || std::abs(map.axis.y)!=1)return out;
    if(!map.filter_on)return source;
    if(reveal_mask && reveal_mask->width && reveal_mask->height &&
       reveal_mask->width<=1024 && reveal_mask->height<=1024 &&
       reveal_mask->rgba.size()==std::size_t(reveal_mask->width)*reveal_mask->height*4 &&
       map.reveal_texture_world_size && std::isfinite(map.reveal_stamp_world_size) &&
       map.reveal_stamp_world_size>0 && map.reveal_stamp_world_size<=100000000) {
        // Native UCanvas BLEND_AlphaHoldout removes alpha from an initially
        // opaque mask. Its inverse reveals the original section texture.
        const double diameter=map.reveal_stamp_world_size*source.width/map.reveal_texture_world_size;
        if(!std::isfinite(diameter) || diameter<=0 || diameter>100000000)return out;
        std::vector<u8> remaining(std::size_t(source.width)*source.height,255);
        constexpr unsigned Tile=16;
        const unsigned columns=(source.width+Tile-1)/Tile,rows=(source.height+Tile-1)/Tile;
        std::vector<bool> done(std::size_t(columns)*rows,false);
        const auto saturated=[&](unsigned tx,unsigned ty) {
            for(unsigned y=ty*Tile;y<std::min(source.height,(ty+1)*Tile);++y)
                for(unsigned x=tx*Tile;x<std::min(source.width,(tx+1)*Tile);++x) {
                    const auto i=std::size_t(y)*source.width+x;
                    if(source.rgba[i*4+3] && remaining[i])return false;
                }
            return true;
        };
        for(unsigned ty=0;ty<rows;++ty)for(unsigned tx=0;tx<columns;++tx)
            done[std::size_t(ty)*columns+tx]=saturated(tx,ty);
        const auto sample=[&](double u,double v) {
            const double x=std::clamp(u*reveal_mask->width-.5,0.,double(reveal_mask->width-1));
            const double y=std::clamp(v*reveal_mask->height-.5,0.,double(reveal_mask->height-1));
            const unsigned x0=static_cast<unsigned>(x),y0=static_cast<unsigned>(y);
            const unsigned x1=std::min(x0+1,reveal_mask->width-1),y1=std::min(y0+1,reveal_mask->height-1);
            const auto alpha=[&](unsigned sx,unsigned sy){return reveal_mask->rgba[(std::size_t(sy)*reveal_mask->width+sx)*4+3]/255.;};
            const double fx=x-x0,fy=y-y0;
            return (alpha(x0,y0)*(1-fx)+alpha(x1,y0)*fx)*(1-fy)+
                   (alpha(x0,y1)*(1-fx)+alpha(x1,y1)*fx)*fy;
        };
        for(const auto cell:map.revealed_cells) {
            if(cell.x<0 || cell.y<0 || cell.x>65535 || cell.y>65535)continue;
            const double wx=map.bounds_min[0]+(cell.x+.5)*map.block_width;
            const double wy=map.bounds_min[1]+(cell.y+.5)*map.block_width;
            const auto center=NativeTexturePoint(map,wx,wy,source.width,source.height);
            if(!std::isfinite(center.x) || !std::isfinite(center.y))continue;
            const double left=center.x-diameter/2,top=center.y-diameter/2;
            const unsigned x0=static_cast<unsigned>(std::ceil(std::clamp(left-.5,0.,double(source.width))));
            const unsigned y0=static_cast<unsigned>(std::ceil(std::clamp(top-.5,0.,double(source.height))));
            const unsigned x1=static_cast<unsigned>(std::ceil(std::clamp(left+diameter-.5,0.,double(source.width))));
            const unsigned y1=static_cast<unsigned>(std::ceil(std::clamp(top+diameter-.5,0.,double(source.height))));
            if(x1<=x0 || y1<=y0)continue;
            for(unsigned ty=y0/Tile;ty<=(y1-1)/Tile;++ty)for(unsigned tx=x0/Tile;tx<=(x1-1)/Tile;++tx) {
                const auto tile=std::size_t(ty)*columns+tx;
                if(done[tile])continue;
                for(unsigned y=std::max(y0,ty*Tile);y<std::min(y1,(ty+1)*Tile);++y)
                    for(unsigned x=std::max(x0,tx*Tile);x<std::min(x1,(tx+1)*Tile);++x) {
                        const auto i=std::size_t(y)*source.width+x;
                        if(!source.rgba[i*4+3] || !remaining[i])continue;
                        const double a=sample((x+.5-left)/diameter,(y+.5-top)/diameter);
                        remaining[i]=static_cast<u8>(std::lround(remaining[i]*(1-a)));
                    }
                done[tile]=saturated(tx,ty);
            }
        }
        for(std::size_t i=0;i<remaining.size();++i) {
            const unsigned alpha=(unsigned(source.rgba[i*4+3])*(255-remaining[i])+127)/255;
            if(!alpha)continue;
            std::copy_n(source.rgba.data()+i*4,3,out.rgba.data()+i*4);
            out.rgba[i*4+3]=static_cast<u8>(alpha);
        }
        return out;
    }
    for(const auto cell:map.revealed_cells) {
        if(cell.x<0 || cell.y<0)continue;
        const auto bounds=CellTextureBounds(map,cell);
        if(!std::isfinite(bounds[0].x) || !std::isfinite(bounds[0].y) ||
           !std::isfinite(bounds[1].x) || !std::isfinite(bounds[1].y))continue;
        // A texel belongs to the cell containing its center. Adjacent cells share the
        // same integer boundary, so their union has neither gaps nor filter bleed.
        const int x0=static_cast<int>(std::ceil(std::clamp(bounds[0].x,0.,double(source.width))-.5));
        const int y0=static_cast<int>(std::ceil(std::clamp(bounds[0].y,0.,double(source.height))-.5));
        const int x1=static_cast<int>(std::ceil(std::clamp(bounds[1].x,0.,double(source.width))-.5));
        const int y1=static_cast<int>(std::ceil(std::clamp(bounds[1].y,0.,double(source.height))-.5));
        if(x1<=x0 || y1<=y0)continue;
        for(int y=y0;y<y1;++y) {
            const auto at=(static_cast<std::size_t>(y)*source.width+static_cast<unsigned>(x0))*4;
            std::copy_n(source.rgba.data()+at,static_cast<std::size_t>(x1-x0)*4,out.rgba.data()+at);
        }
    }
    return out;
}
FormationLayout ComputeFormationLayout(const Snapshot& snapshot) {
    FormationLayout layout;
    const bool ready=snapshot.valid && snapshot.state>=23 && snapshot.state<=26;
    int count=0;
    for(const auto& member:snapshot.members)count+=member.available?1:0;
    layout.solo=count==1;
    int compact_row=0;
    for(int pair=0;pair<4;++pair) {
        const bool occupied=snapshot.members[static_cast<std::size_t>(pair)].available ||
                            snapshot.members[static_cast<std::size_t>(pair+4)].available;
        const int row=count<=4?compact_row:pair;
        for(int column=0;column<2;++column) {
            const int slot=pair+column*4;
            auto& rect=layout.slots[static_cast<std::size_t>(slot)];
            rect.x=layout.solo?724:590+column*322;
            rect.y=layout.solo?224:84+row*222;
            rect.on=ready && snapshot.members[static_cast<std::size_t>(slot)].available;
        }
        if(occupied)++compact_row;
    }
    return layout;
}
BattleLayout ComputeBattleLayout(const BattleSnapshot& battle,int focus) {
    BattleLayout out;
    if(!battle.valid)return out;
    const int n=std::clamp(battle.enemy_count,0,6);
    if(focus>=0 && focus<n && battle.enemies[static_cast<std::size_t>(focus)].alive)out.focus=focus;
    for(int i=0;i<n && out.focus<0;++i)if(battle.enemies[static_cast<std::size_t>(i)].alive)out.focus=i;
    for(int i=0;i<n;++i)
        if(i!=out.focus && battle.enemies[static_cast<std::size_t>(i)].alive && out.other_count<5)
            out.others[static_cast<std::size_t>(out.other_count++)]=i;
    return out;
}
struct ScreenComposer::Impl {
    ScreenVisuals* visuals{};
    FontLibrary font;
    Names names;
    MapNames map_names;
    MapRegions map_regions;
    std::map<std::string,std::optional<MemberId>> emblem_members; // emblem texture -> pinned member
    unsigned map_regions_attempts{};
    // The current map's region line and native region emblem (MapListTable).
    void MapHeader(Image& out,const RangeReader& read,const ExplorationSnapshot* map,const std::string& title,int right) {
        const MapRegion* region=nullptr;
        if(map && map->valid) {
            if(!map_regions.valid && map_regions_attempts<3) {
                ++map_regions_attempts;
                auto list=ReadMember(read,Member(MemberId::MapListDb),nullptr);
                auto textures=ReadMember(read,Member(MemberId::TextureDb),nullptr);
                if(list && textures)map_regions=DecodeMapRegions(*list,*textures);
            }
            region=map_regions.Find(map->map_id);
        }
        int x=24;
        if(region && !region->emblem.empty()) {
            auto cached=emblem_members.find(region->emblem);
            if(cached==emblem_members.end()) {
                std::optional<MemberId> found;
                for(u32 i=0;i<static_cast<u32>(MemberId::Count) && !found;++i)
                    if(std::string_view(Member(static_cast<MemberId>(i)).path).ends_with("/"+region->emblem+".uexp"))
                        found=static_cast<MemberId>(i);
                cached=emblem_members.emplace(region->emblem,found).first;
            }
            for(const auto id:cached->second?std::vector<MemberId>{*cached->second}:std::vector<MemberId>{}) {
                if(!Ensure(read,id,nullptr))continue;
                const auto& emblem=images.at(id);
                if(!emblem.width || !emblem.height)break;
                const u32 h=78,w=std::max<u32>(1,emblem.width*h/emblem.height);
                Over(out,Scale(emblem,w,h),20,8);
                x=20+static_cast<int>(w)+12;
                break;
            }
        }
        const std::string region_name=region && region->region_text?map_names.LookupName(region->region_text):"";
        const int width=right-x-12;
        if(!region_name.empty()) {
            Text(out,x,8,FitName(region_name,FontFace::Rocio,28,width),FontFace::Rocio,28,Soft);
            Text(out,x,36,FitName(title,FontFace::FrancoBold,38,width),FontFace::FrancoBold,38);
        } else {
            Text(out,x,18,FitName(title,FontFace::FrancoBold,40,width),FontFace::FrancoBold,40);
        }
    }
    bool map_names_loaded{};
    std::optional<ProtagonistAppearance> appearance_key;
    Image appearance_sprite;
    bool appearance_attempted{};
    const ProtagonistAppearance* current_appearance{};
    std::optional<ExplorationSnapshot> map_key;
    std::vector<MapPoint> map_frame_key;  // world points the picture must also frame
    std::string map_texture_key;
    Image map_source,map_picture,player_marker;
    std::map<std::string,Image> minimap_glyphs;
    int map_crop_x{},map_crop_y{},map_draw_x{},map_draw_y{};
    double map_zoom{};
    uint64_t map_revision{};
    bool map_load_failed{};
    unsigned map_load_attempts{};
    unsigned map_mask_attempts{};
    bool data_ready{};
    std::map<MemberId,Image> images;
    std::map<u32,Image> sprites;
    Image backdrop;
    bool Ensure(const RangeReader& read,MemberId id,std::string* why) {
        if(images.contains(id))return true;
        auto im=ReadImage(read,id,why);if(!im)return false;
        // BP material uses its black canvas as zero contribution. Preserve
        // the native disc ink when compositing over a blue active-row wash.
        if(id==MemberId::BpDisc)for(std::size_t i=0;i<im->rgba.size();i+=4) {
            const auto ink=std::max({im->rgba[i],im->rgba[i+1],im->rgba[i+2]});
            im->rgba[i+3]=static_cast<u8>(im->rgba[i+3]*ink/255);
            // Owner-requested red BP treatment, preserving the native disc/shading.
            im->rgba[i+1]=static_cast<u8>(im->rgba[i+1]*.45);
            im->rgba[i+2]=static_cast<u8>(im->rgba[i+2]*.45);
        }
        images.emplace(id,std::move(*im));return true;
    }
    bool Base(const RangeReader& read,std::string* why) {
        if(!font.Load(read,why))return false;
        for(auto id:{MemberId::InBox,MemberId::CharaFrame,MemberId::CharaLight,MemberId::Hand,MemberId::Logo,MemberId::IconParty})
            if(!Ensure(read,id,why))return false;
        return true;
    }
    bool PartyAssets(const RangeReader& read,std::string* why) {
        if(!data_ready) {
            auto cp=ReadMember(read,Member(MemberId::CharaPlayer),why);
            auto en=cp?ReadMember(read,Member(MemberId::TextCharacter),why):std::nullopt;
            if(!cp || !en)return false;
            names=DecodeNames(*cp,*en);
            if(!names.valid){if(why)*why="Native character table validation failed";return false;}
            data_ready=true;
        }
        if(backdrop.rgba.empty()) {
            auto bytes=ReadMember(read,Member(MemberId::WorldMap),why);
            const auto pin=TextureLayout(MemberId::WorldMap);
            if(!bytes || !pin)return false;
            // Approved D crop, not a live location indicator: center1450,1650 at zoom .9.
            auto crop=DecodeTextureRect(*bytes,*pin,{892,1164,1116,972},why);
            if(!crop)return false;
            backdrop=DarkenBlur(Scale(*crop,Width,Height),.10,8);
        }
        return !backdrop.rgba.empty();
    }
    void Text(Image& dst,int x,int y,std::string_view text,FontFace face,float size,
              std::array<u8,4> color=White,char h='l',char v='a') const {
        // Native fonts only; an unavailable glyph stays unavailable rather than altering a name.
        font.DrawGameText(dst,text,face,size,x+2,y+2,{0,0,0,200},h,v);
        font.DrawGameText(dst,text,face,size,x,y,color,h,v);
    }
    // Panel layers depend only on (w,h): resized once per size instead of every compose. The
    // same layers are blended in the same order, so pixels are unchanged.
    struct PanelLayers { Image fill,box; std::optional<Image> light,frame; };
    mutable std::map<std::pair<u32,u32>,PanelLayers> panel_layers;
    void Panel(Image& dst,int x,int y,u32 w,u32 h,bool selected=false) const {
        auto it=panel_layers.find({w,h});
        if(it==panel_layers.end())
            it=panel_layers.emplace(std::pair{w,h},PanelLayers{Solid(w-6,h-6,{7,8,11,225}),
                Alpha(Nine(images.at(MemberId::InBox),w,h),.92),{},{}}).first;
        auto& layers=it->second;
        Over(dst,layers.fill,x+3,y+3);
        Over(dst,layers.box,x,y);
        if(selected) {
            // Selection art is only needed (and only guaranteed loaded) for selected panels.
            if(!layers.light)layers.light=Alpha(Scale(images.at(MemberId::CharaLight),w,h),.55);
            if(!layers.frame)layers.frame=Nine(images.at(MemberId::CharaFrame),w,h);
            Over(dst,*layers.light,x,y);
            Over(dst,*layers.frame,x,y);
        }
    }
    static std::string ResourceText(u32 value,u32 maximum) {
        return std::to_string(value)+(maximum && value<=maximum?"/"+std::to_string(maximum):"");
    }
    void Gauge(Image& dst,int x,int y,u32 width,u32 height,u32 value,u32 maximum,bool hp) const {
        if(!width || !height || !maximum || value>maximum)return;
        Over(dst,Solid(width,height,{18,18,18,220}),x,y);
        const auto fill=static_cast<u32>(std::lround(double(width)*value/maximum));
        const std::array<int,3> hi=hp?std::array<int,3>{90,140,93}:std::array<int,3>{88,128,140};
        const std::array<int,3> lo=hp?std::array<int,3>{72,112,73}:std::array<int,3>{66,96,108};
        if(!fill)return;
        for(u32 row=0;row<height;++row) {
            std::array<u8,4> color{0,0,0,255};
            const int bottom=static_cast<int>(std::max(1u,height-1));
            for(int c=0;c<3;++c)color[static_cast<std::size_t>(c)]=static_cast<u8>(
                (hi[static_cast<std::size_t>(c)]*(bottom-static_cast<int>(row))+
                 lo[static_cast<std::size_t>(c)]*static_cast<int>(row)+bottom/2)/bottom);
            Over(dst,Solid(fill,1,color),x,y+static_cast<int>(row));
        }
    }
    std::string Name(u32 id) const {
        if(id>=900 && id<=909)return "Leader"; // Native player name is unreadable; no invented name.
        return names.LookupName(id);
    }
    std::string FitName(std::string_view text,FontFace face,float size,int max_width) const {
        const auto width=font.Measure(text,face,size);
        if(width && *width<=max_width)return std::string(text);
        auto decoded=DecodeUtf8(text);if(!decoded)return {};
        while(!decoded->empty()) {
            decoded->pop_back();
            const auto candidate=*decoded+U"...";
            const auto w=font.Measure(candidate,face,size);
            if(!w || *w>max_width)continue;
            std::string out;
            for(const auto c:candidate) {
                const auto cp=static_cast<u32>(c);
                if(cp<0x80)out+=static_cast<char>(cp);
                else if(cp<0x800) {out+=static_cast<char>(0xc0|(cp>>6));out+=static_cast<char>(0x80|(cp&63));}
                else if(cp<0x10000) {out+=static_cast<char>(0xe0|(cp>>12));out+=static_cast<char>(0x80|((cp>>6)&63));out+=static_cast<char>(0x80|(cp&63));}
                else {out+=static_cast<char>(0xf0|(cp>>18));out+=static_cast<char>(0x80|((cp>>12)&63));out+=static_cast<char>(0x80|((cp>>6)&63));out+=static_cast<char>(0x80|(cp&63));}
            }
            return out;
        }
        return {};
    }
    std::map<u32,std::optional<MemberId>> chara_sprites;
    u32 SpriteId(u32 id) const {
        if(id>=900 && id<=909)return 901;
        return names.LookupSprite(id);
    }
    const Image* GetSprite(const RangeReader& read,u32 id) {
        if(id>=900 && id<=909 && current_appearance) {
            if(!appearance_key || *appearance_key!=*current_appearance) {
                appearance_key=*current_appearance;appearance_sprite={};appearance_attempted=false;
            }
            if(!appearance_attempted) {
                appearance_attempted=true;
                auto atlas=LoadProtagonistAtlas(read,*current_appearance,nullptr);
                if(atlas) {
                    appearance_sprite=NearestFrame(*atlas);
                    for(std::size_t i=0;i<appearance_sprite.rgba.size();i+=4) {
                        const bool key=appearance_sprite.rgba[i]>=251 && appearance_sprite.rgba[i+1]<=4 && appearance_sprite.rgba[i+2]<5;
                        appearance_sprite.rgba[i+3]=key?0:255;
                    }
                }
            }
            if(!appearance_sprite.rgba.empty())return &appearance_sprite;
        }
        // Any playable character: its own sheet, found by party id (ChrTX_Chd_Ply_<id>_J..).
        if(id<900 || id>909) {
            auto found=chara_sprites.find(id);
            if(found==chara_sprites.end()) {
                std::optional<MemberId> member;
                const std::string needle="/ChrTX_Chd_Ply_"+std::to_string(id)+"_J";
                for(u32 i=0;i<static_cast<u32>(MemberId::Count) && !member;++i) {
                    const std::string_view path=Member(static_cast<MemberId>(i)).path;
                    if(path.find(needle)!=std::string_view::npos && path.ends_with("_B.uexp"))member=static_cast<MemberId>(i);
                }
                found=chara_sprites.emplace(id,member).first;
            }
            if(found->second) {
                const u32 key=0x80000000u|id;
                if(const auto it=sprites.find(key);it!=sprites.end())return &it->second;
                if(Ensure(read,*found->second,nullptr)) {
                    auto im=NearestFrame(images.at(*found->second));
                    images.erase(*found->second);
                    if(!im.rgba.empty())return &sprites.emplace(key,std::move(im)).first->second;
                }
            }
        }
        const u32 sprite=SpriteId(id);
        if(const auto it=sprites.find(sprite);it!=sprites.end())return &it->second;
        if(const auto member=Sprite(sprite)) {
            if(!Ensure(read,*member,nullptr))return nullptr;
            auto im=NearestFrame(images.at(*member));
            images.erase(*member); // Keep just the displayed 44px frame, not the whole sheet.
            auto [it,inserted]=sprites.emplace(sprite,std::move(im));(void)inserted;
            return &it->second;
        }
        if(sprite==901) {
            if(!Ensure(read,MemberId::ArtLeader,nullptr))return nullptr;
            const auto trimmed=Trim(images.at(MemberId::ArtLeader));
            if(trimmed.rgba.empty())return nullptr;
            auto medallion=Scale(trimmed,92,92);
            for(u32 y=0;y<92;++y)for(u32 x=0;x<92;++x) {
                const double dx=x-45.5,dy=y-45.5;
                if(dx*dx+dy*dy>45.5*45.5)medallion.rgba[(static_cast<std::size_t>(y)*92+x)*4+3]=0;
            }
            auto [it,inserted]=sprites.emplace(sprite,std::move(medallion));(void)inserted;
            return &it->second;
        }
        return nullptr;
    }
    // Map | Party | Settings. The square settings tab (native main-menu "Miscellaneous" glyph)
    // opens the host's companion settings page; the manifest tap rects mirror these rectangles.
    // Map | Party | Items (without a local map: Party | Items). FooterTabs gives the rectangles;
    // the manifest's tap rects mirror them.
    static std::array<int,2> FooterTab(bool map_available,int index) {
        if(map_available)return {16+index*274,262};
        return {16+(index-1)*364,352};
    }
    void Footer(Image& dst,bool map_available,int active,const RangeReader& read) {
        constexpr int SettingsX=1112;
        for(int page=map_available?0:1;page<4;++page) {
            const auto [x,w]=FooterTab(map_available,page);
            Panel(dst,x,956,static_cast<u32>(w),112,page==active);
            const char* label=page==0?"Map":page==1?"Party":page==2?"Items":"Quests";
            const int text_w=static_cast<int>(font.Measure(label,FontFace::Rocio,38).value_or(100));
            const int left=x+(w-(44+16+text_w))/2;
            if(page<2) {
                const auto icon=page==0?MemberId::IconMap:MemberId::IconParty;
                if(images.contains(icon))Over(dst,Scale(images.at(icon),44,44),left,990);
            } else Glyph(dst,CommonGlyph(read,page==2?"UiTX_Common_TabIcon_02_CS_png":"UiTX_Common_MenuListIcon_Mask_Quest_png"),left,990,44);
            Text(dst,left+60,1012,label,FontFace::Rocio,38,White,'l','m');
        }
        Panel(dst,SettingsX,956,112,112,false);
        if(images.contains(MemberId::IconMisc))
            Over(dst,Scale(images.at(MemberId::IconMisc),52,52),SettingsX+30,986);
    }
    static Image Nearest(const Image& src,u32 x0,u32 y0,u32 w,u32 h,u32 ow,u32 oh) {
        if(!w || !h || !ow || !oh || x0+w>src.width || y0+h>src.height)return {};
        Image out{ow,oh,std::vector<u8>(static_cast<std::size_t>(ow)*oh*4)};
        for(u32 y=0;y<oh;++y)for(u32 x=0;x<ow;++x) {
            const auto at=(static_cast<std::size_t>(y0+u64{y}*h/oh)*src.width+x0+u64{x}*w/ow)*4;
            std::copy_n(src.rgba.data()+at,4,out.rgba.data()+(static_cast<std::size_t>(y)*ow+x)*4);
        }
        return out;
    }
    void MapBase(const RangeReader& read,const ExplorationSnapshot& map,const std::vector<MapPoint>& frame={}) {
        auto key=map;key.world_xyz={};
        // Marker movement within a native world cell only redraws the marker. Crossing a
        // cell may extend the viewport, but never adds that cell to the geometry mask.
        const auto current_cell=WorldCell(map,map.world_xyz[0],map.world_xyz[1]);
        // Not keyed on the player's cell: the picture is the revealed area (it follows reveals).
        key.texture_position={};key.heading_valid=false;
        key.native_direction=0;key.state=0;key.map_name_id=0;
        const bool needs_mask=map.filter_on && map.reveal_texture_world_size && map.reveal_stamp_world_size>0;
        if(map_key && *map_key==key && map_frame_key==frame && !(map_source.rgba.empty() && map_load_attempts<3) &&
           !(needs_mask && !images.contains(MemberId::MinimapRevealMask) && map_mask_attempts<3))return;
        map_key=key;map_frame_key=frame;map_picture={};map_zoom=0;
        if(map_texture_key!=map.texture_path) {
            map_source={};map_texture_key=map.texture_path;map_load_attempts=0;
        }
        if(map_source.rgba.empty() && map_load_attempts<3) {
            ++map_load_attempts;
            auto image=LoadMapTexture(read,map.texture_path,nullptr);
            map_load_failed=!image;
            if(image)map_source=std::move(*image);
        }
        if(map_source.rgba.empty())return;
        map_load_failed=false;
        const Image* mask=nullptr;
        if(needs_mask) {
            if(!images.contains(MemberId::MinimapRevealMask) && map_mask_attempts<3) {
                ++map_mask_attempts;
                Ensure(read,MemberId::MinimapRevealMask,nullptr);
            }
            if(!images.contains(MemberId::MinimapRevealMask)) {map_load_failed=true;return;}
            mask=&images.at(MemberId::MinimapRevealMask);
        }
        auto masked=MaskRevealedPaths(map_source,map,mask);
        double left=masked.width,top=masked.height,right=0,bottom=0;
        // The native filter extends beyond the visited grid cell. Frame the
        // visible path pixels as well as transparent cells around the player.
        for(u32 y=0;y<masked.height;++y)for(u32 x=0;x<masked.width;++x)
            if(masked.rgba[(std::size_t(y)*masked.width+x)*4+3]) {
                left=std::min(left,double(x));top=std::min(top,double(y));
                right=std::max(right,double(x+1));bottom=std::max(bottom,double(y+1));
            }
        const auto include=[&](MapCell cell) {
            if(cell.x<0 || cell.y<0 || cell.x>65535 || cell.y>65535)return;
            auto box=CellTextureBounds(map,cell);
            if(map.reveal_texture_world_size) {
                const double wx=map.bounds_min[0]+double(cell.x)*map.block_width;
                const double wy=map.bounds_min[1]+double(cell.y)*map.block_width;
                const auto a=NativeTexturePoint(map,wx,wy,masked.width,masked.height);
                const auto b=NativeTexturePoint(map,wx+map.block_width,wy+map.block_width,masked.width,masked.height);
                box={MapPoint{std::min(a.x,b.x),std::min(a.y,b.y)},MapPoint{std::max(a.x,b.x),std::max(a.y,b.y)}};
            }
            if(!std::isfinite(box[0].x) || !std::isfinite(box[0].y) ||
               !std::isfinite(box[1].x) || !std::isfinite(box[1].y))return;
            const double l=std::clamp(box[0].x,0.,double(masked.width));
            const double t=std::clamp(box[0].y,0.,double(masked.height));
            const double r=std::clamp(box[1].x,0.,double(masked.width));
            const double b=std::clamp(box[1].y,0.,double(masked.height));
            if(r<=l || b<=t)return;
            left=std::min(left,l);top=std::min(top,t);right=std::max(right,r);bottom=std::max(bottom,b);
        };
        for(const auto cell:map.revealed_cells)include(cell);
        const auto point=map.reveal_texture_world_size ? NativeTexturePoint(map,map.world_xyz[0],map.world_xyz[1],masked.width,masked.height) : map.texture_position;
        const double px=point.x,py=point.y;
        if(std::isfinite(px) && std::isfinite(py) && px>=0 && py>=0 &&
           px<masked.width && py<masked.height)include(current_cell);
        // Revealed POIs (a chest in an alcove) can sit outside the drawn floor; frame them too.
        for(const auto& w:frame) {
            const auto t=NativeTexturePoint(map,w.x,w.y,masked.width,masked.height);
            if(!std::isfinite(t.x) || !std::isfinite(t.y) || t.x<0 || t.y<0 ||
               t.x>masked.width || t.y>masked.height)continue;
            left=std::min(left,t.x);top=std::min(top,t.y);right=std::max(right,t.x);bottom=std::max(bottom,t.y);
        }
        if(right<=left || bottom<=top)return;
        // Transparent portions of revealed cells and a native-plane margin keep a portal
        // marker in frame. Only already-masked pixels are resampled into this viewport.
        constexpr double Margin=32;
        const u32 x0=static_cast<u32>(std::floor(std::max(0.,left-Margin)));
        const u32 y0=static_cast<u32>(std::floor(std::max(0.,top-Margin)));
        const u32 x1=static_cast<u32>(std::ceil(std::min(double(masked.width),right+Margin)));
        const u32 y1=static_cast<u32>(std::ceil(std::min(double(masked.height),bottom+Margin)));
        // Native resolution (no resample): the runtime pans/zooms it. Pad the revealed crop,
        // centred, to the map pane's aspect so the host's stretch keeps texels square.
        // At least MapViewTexels across so zoom 1 (the runtime's minimum) never magnifies a small
        // revealed area: the home view keeps the same native scale everywhere, like the minimap.
        constexpr u32 MapViewTexels=512;
        u32 w=std::max(x1-x0,MapViewTexels),h=y1-y0;
        const double aspect=double(TownMapLayout::InnerW)/TownMapLayout::InnerH;
        if(double(w)/h<aspect)w=static_cast<u32>(std::ceil(h*aspect));else h=static_cast<u32>(std::ceil(w/aspect));
        const int ox=static_cast<int>(x0)-static_cast<int>((w-(x1-x0))/2);
        const int oy=static_cast<int>(y0)-static_cast<int>((h-(y1-y0))/2);
        map_picture=Image{w,h,std::vector<u8>(std::size_t(w)*h*4)};
        for(u32 y=y0;y<y1;++y)
            std::memcpy(&map_picture.rgba[(std::size_t(int(y)-oy)*w+std::size_t(int(x0)-ox))*4],
                        &masked.rgba[(std::size_t(y)*masked.width+x0)*4],std::size_t(x1-x0)*4);
        map_crop_x=ox;map_crop_y=oy;map_zoom=1;
        ++map_revision;
    }
    // Character art trimmed once per member (a full-size illustration scan per compose otherwise).
    std::map<MemberId,Image> trimmed_art;
    const Image& TrimmedArt(MemberId id) {
        auto it=trimmed_art.find(id);
        if(it==trimmed_art.end())it=trimmed_art.emplace(id,Trim(images.at(id))).first;
        return it->second;
    }
    // ---------------- Party page sub-tabs: Stats | Equipment ----------------
    void PartyTabs(Image& out,int tab) {
        for(int i=0;i<2;++i) {
            const int x=16+i*256;
            Panel(out,x,12,244,74,i==tab);
            Text(out,x+122,49,i==0?"Stats":"Equipment",FontFace::Rocio,34,i==tab?White:Soft,'m','m');
        }
    }
    Items items;
    unsigned items_attempts{};
    const Items& ItemCatalog(const RangeReader& read) {
        if(!items.valid && items_attempts<3) {
            ++items_attempts;
            auto list=ReadMember(read,Member(MemberId::ItemDb),nullptr);
            auto types=ReadMember(read,Member(MemberId::ItemTypeDb),nullptr);
            auto textures=ReadMember(read,Member(MemberId::TextureDb),nullptr);
            auto text=ReadMember(read,Member(MemberId::TextItem),nullptr);
            if(list && types && textures && text)items=DecodeItems(*list,*types,*textures,*text);
        }
        return items;
    }
    // A native common UI frame (PaperSprite on the CommonIcon atlas), e.g. "UiTX_Common_ListIcon_Sword_png".
    std::map<std::string,Image> common_glyphs;
    std::set<std::string> missing_glyphs; // stems with no pinned frame (some item icons are absent)
    const Image* CommonGlyph(const RangeReader& read,const std::string& stem) {
        if(const auto found=common_glyphs.find(stem);found!=common_glyphs.end())return &found->second;
        if(missing_glyphs.contains(stem))return nullptr;
        if(!stem.starts_with("UiTX_Common_") || !Ensure(read,MemberId::CommonIcon,nullptr))return nullptr;
        const std::string tail="UI/Common/Texture/Frames/"+stem+".uexp";
        for(u32 i=0;i<static_cast<u32>(MemberId::Count);++i) {
            const auto id=static_cast<MemberId>(i);
            const std::string_view path=Member(id).path;
            if(!path.ends_with(tail) || (path.size()>tail.size() && path[path.size()-tail.size()-1]!='/'))continue;
            const auto bytes=ReadMember(read,Member(id),nullptr);
            if(!bytes || bytes->size()!=259)return nullptr;
            std::array<double,4> rect{};
            for(int k=0;k<4;++k)std::memcpy(&rect[k],bytes->data()+11+k*8,8);
            const auto& atlas=images.at(MemberId::CommonIcon);
            if(!std::all_of(rect.begin(),rect.end(),[](double v){return std::isfinite(v) && v>=0 && std::floor(v)==v;}) ||
               rect[2]<1 || rect[3]<1 || rect[0]+rect[2]>atlas.width || rect[1]+rect[3]>atlas.height)return nullptr;
            auto [it,inserted]=common_glyphs.emplace(stem,Nearest(atlas,static_cast<u32>(rect[0]),static_cast<u32>(rect[1]),
                static_cast<u32>(rect[2]),static_cast<u32>(rect[3]),static_cast<u32>(rect[2]),static_cast<u32>(rect[3])));
            (void)inserted;return &it->second;
        }
        missing_glyphs.insert(stem); // not pinned at all: never scan for it again
        return nullptr;
    }
    void Glyph(Image& out,const Image* glyph,int x,int y,u32 size) {
        if(!glyph || !glyph->width || !glyph->height)return;
        const u32 w=glyph->width>=glyph->height?size:std::max<u32>(1,glyph->width*size/glyph->height);
        const u32 h=glyph->height>=glyph->width?size:std::max<u32>(1,glyph->height*size/glyph->width);
        Over(out,Scale(*glyph,w,h),x+int(size-w)/2,y+int(size-h)/2);
    }
    // Native Equipment screen order: weapons, shield, head, body, accessories. Armour is placed
    // by each item's own type (15 shield, 9 head, 10 body), not by its array index.
    void EquipmentPane(Image& out,const RangeReader& read,const PartyMember& m,const Gear* gear) {
        Panel(out,TownMapLayout::X,TownMapLayout::Y,TownMapLayout::W,TownMapLayout::H);
        if(!m.available)return;
        const int l=TownMapLayout::InnerX,r=TownMapLayout::InnerX+TownMapLayout::InnerW;
        if(const auto* source=GetSprite(read,m.id)) {
            const auto ink=Trim(*source);
            if(!ink.rgba.empty()) {
                const u32 w=source->width==132?ink.width:96,h=source->height==132?ink.height:96;
                Over(out,Nearest(ink,0,0,ink.width,ink.height,w,h),r-static_cast<int>(w),TownMapLayout::InnerY);
            }
        }
        Text(out,l,TownMapLayout::InnerY+4,FitName(Name(m.id),FontFace::FrancoBold,52,520),FontFace::FrancoBold,52);
        Text(out,l,TownMapLayout::InnerY+72,"Lv. "+std::to_string(m.level),FontFace::ClearTone,38,Soft);
        const auto& catalog=ItemCatalog(read);
        const bool known=gear && gear->valid && gear->member==m.id && catalog.valid;
        std::array<uint32_t,3> armor{};
        if(known)for(const auto item:gear->armor) {
            const auto* info=catalog.Find(item);
            const int slot=!info?-1:info->type==15?0:info->type==9?1:info->type==10?2:-1;
            if(slot>=0 && !armor[static_cast<std::size_t>(slot)])armor[static_cast<std::size_t>(slot)]=item;
        }
        // Weapons in the native screen's weapon-type order (Sword .. Fan), empty slots last.
        std::array<uint32_t,2> weapons{known?gear->weapons[0]:0,known?gear->weapons[1]:0};
        const auto weapon_rank=[&](uint32_t item) {
            const auto* info=item?catalog.Find(item):nullptr;
            return info && info->weapon_type?info->weapon_type:99u;
        };
        if(weapon_rank(weapons[1])<weapon_rank(weapons[0]))std::swap(weapons[0],weapons[1]);
        struct Row { const char* label; const char* slot_icon; uint32_t item; };
        const std::array<Row,7> rows{{
            {"Weapon","UiTX_Common_ListIcon_Sword_png",weapons[0]},
            {"Weapon","UiTX_Common_ListIcon_Sword_png",weapons[1]},
            {"Shield","UiTX_Common_ListIcon_Shield_png",armor[0]},
            {"Head","UiTX_Common_ListIcon_HeadArmor_png",armor[1]},
            {"Body","UiTX_Common_ListIcon_BodyArmor_png",armor[2]},
            {"Accessory","UiTX_Common_ListIcon_Accessory_png",known?gear->accessories[0]:0},
            {"Accessory","UiTX_Common_ListIcon_Accessory_png",known?gear->accessories[1]:0}}};
        Over(out,Solid(static_cast<u32>(r-l),2,{150,150,160,90}),l,TownMapLayout::InnerY+128);
        for(std::size_t i=0;i<rows.size();++i) {
            const int y=TownMapLayout::InnerY+146+static_cast<int>(i)*86;
            const auto& row=rows[i];
            const auto* info=row.item?catalog.Find(row.item):nullptr;
            // The equipped item's own native icon; an empty slot shows its slot icon, dimmed text.
            Glyph(out,CommonGlyph(read,info && !info->icon.empty()?info->icon:row.slot_icon),l,y+6,56);
            Text(out,l+72,y+4,row.label,FontFace::Rocio,28,Soft);
            const std::string name=!known?"—":info && !info->name.empty()?info->name:row.item?"—":"Empty";
            Text(out,l+72,y+32,FitName(name,FontFace::Franco,36,r-l-80),FontFace::Franco,36,
                 known && row.item?White:Dim);
        }
    }
    // ---------------- Items page (native start-menu Items: tab column + Name/Held list) -------
    struct ItemTab { const char* title; const char* icon; };
    static constexpr std::array<ItemTab,ItemTabs> ItemTabList{{
        {"Obtained","UiTX_Common_TabIcon_01_CS_png"},{"Items","UiTX_Common_TabIcon_02_CS_png"},
        {"Dishes","UiTX_Common_TabIcon_29_png"},{"Weapons","UiTX_Common_TabIcon_03_CS_png"},
        {"Shields","UiTX_Common_TabIcon_04_CS_png"},{"Armor (Head)","UiTX_Common_TabIcon_05_CS_png"},
        {"Armor (Body)","UiTX_Common_TabIcon_06_CS_png"},{"Accessories","UiTX_Common_TabIcon_07_CS_png"},
        {"Action Skills","UiTX_Common_TabIcon_27_png"},{"Building Materials","UiTX_Common_TabIcon_28_png"},
        {"Valuables","UiTX_Common_TabIcon_12_CS_png"},{"Info","UiTX_Common_TabIcon_13_CS_png"}}};
    // Tab membership (live-matched against the native menu 2026-10-31): category 1 weapons,
    // 4 shields, 2 head, 3 body, 5 accessories, 6 action skills, 12 building materials,
    // 11 valuables, 0 info; category-9 types labelled "Dishes" (94444) are Dishes, the rest of
    // 7..10/13 is Items. Rows with show order 0 are never listed; held 0 is not shown.
    static int ItemTabOf(const ItemInfo& info) {
        switch(info.category) {
        case 1: return 3; case 4: return 4; case 2: return 5; case 3: return 6; case 5: return 7;
        case 6: return 8; case 12: return 9; case 11: return 10; case 0: return 11;
        case 9: return info.type_text==94444?2:1;
        case 7: case 8: case 10: case 13: return 1;
        default: return -1;
        }
    }
    struct ItemRow { const ItemInfo* info; int32_t count; };
    std::vector<ItemRow> ItemList(const RangeReader& read,const Inventory& inventory,int tab) {
        const auto& catalog=ItemCatalog(read);
        std::vector<ItemRow> rows;
        if(!catalog.valid || !inventory.valid)return rows;
        for(const auto& e:inventory.entries) {
            const auto* info=catalog.Find(e.item);
            if(!info || !info->show_order || info->not_display || e.count<=0)continue;
            if(tab==0 || ItemTabOf(*info)==tab)rows.push_back({info,e.count});
        }
        if(tab==0)std::reverse(rows.begin(),rows.end()); // newest first
        else std::stable_sort(rows.begin(),rows.end(),[](const ItemRow& a,const ItemRow& b) {
            // Native order: weapon type (else item type) ascending, then show order descending.
            const auto ka=a.info->weapon_type?a.info->weapon_type:a.info->type;
            const auto kb=b.info->weapon_type?b.info->weapon_type:b.info->type;
            return ka!=kb?ka<kb:a.info->show_order>b.info->show_order;
        });
        return rows;
    }
    Image ItemsPage(const RangeReader& read,const Inventory* inventory,int tab,int first,int* last_first) {
        auto out=backdrop;
        Text(out,24,18,"Items",FontFace::FrancoBold,40);
        tab=std::clamp(tab,0,ItemTabs-1);
        // Native tab column.
        Panel(out,16,94,248,828);
        for(int i=0;i<ItemTabs;++i) {
            const int x=22+(i%2)*124, y=106+(i/2)*124;
            if(i==tab)Panel(out,x,y,112,112,true);
            Glyph(out,CommonGlyph(read,ItemTabList[static_cast<std::size_t>(i)].icon),x+24,y+24,64);
        }
        Panel(out,276,94,948,828);
        Text(out,304,112,ItemTabList[static_cast<std::size_t>(tab)].title,FontFace::FrancoBold,40);
        Text(out,304,170,"Name",FontFace::Rocio,28,Soft);
        Text(out,1196,170,"Held",FontFace::Rocio,28,Soft,'r');
        Over(out,Solid(892,2,{150,150,160,90}),304,204);
        const auto rows=inventory?ItemList(read,*inventory,tab):std::vector<ItemRow>{};
        const int pages=std::max<int>(1,(static_cast<int>(rows.size())+ItemRows-1)/ItemRows);
        first=std::clamp(first,0,(pages-1)*ItemRows);
        if(last_first)*last_first=(pages-1)*ItemRows;
        if(!inventory || !inventory->valid || !ItemCatalog(read).valid) {
            Text(out,670,500,"Items unavailable",FontFace::Rocio,38,Soft,'m','m');return out;
        }
        if(rows.empty())Text(out,670,500,"None",FontFace::Rocio,38,Soft,'m','m');
        for(int i=0;i<ItemRows && first+i<static_cast<int>(rows.size());++i) {
            const auto& row=rows[static_cast<std::size_t>(first+i)];
            const int y=216+i*58;
            Glyph(out,CommonGlyph(read,row.info->icon),304,y+6,44);
            // The favorite dish is named by the player (save string), like the native list.
            const std::string name=row.info->favorite_dish?inventory->favorite_dish+row.info->name:row.info->name;
            Text(out,364,y+8,FitName(name.empty()?"—":name,FontFace::Franco,36,680),FontFace::Franco,36);
            Text(out,1196,y+8,std::to_string(row.count),FontFace::ClearTone,38,White,'r');
        }
        if(pages>1) {
            const int page=first/ItemRows;
            Panel(out,860,12,104,74,page>0);
            Text(out,912,49,"Prev",FontFace::Rocio,30,page>0?White:Dim,'m','m');
            Text(out,1028,49,std::to_string(page+1)+"/"+std::to_string(pages),FontFace::ClearTone,32,Soft,'m','m');
            Panel(out,1092,12,132,74,page+1<pages);
            Text(out,1158,49,"Next",FontFace::Rocio,30,page+1<pages?White:Dim,'m','m');
        }
        return out;
    }
    // ---------------- Quests page (native start-menu Quests: "Available Quests") ------------
    Quests quests;
    unsigned quests_attempts{};
    void EnsureQuests(const RangeReader& read) {
        if(!quests.valid && quests_attempts<3) {
            ++quests_attempts;
            auto list=ReadMember(read,Member(MemberId::QuestDb),nullptr);
            auto text=ReadMember(read,Member(MemberId::TextQuest),nullptr);
            if(list && text)quests=DecodeQuests(*list,*text);
        }
    }
    // Page frame; the rows are a separate tall image the runtime scrolls (QuestListImage).
    Image QuestsPage(const RangeReader& read,const QuestStates* states) {
        auto out=backdrop;
        Text(out,24,18,"Quests",FontFace::FrancoBold,40);
        Panel(out,16,94,1208,828);
        Text(out,44,112,"Available Quests",FontFace::FrancoBold,40);
        Text(out,44,170,"Name",FontFace::Rocio,28,Soft);
        Text(out,1196,170,"Rec. Level",FontFace::Rocio,28,Soft,'r');
        Over(out,Solid(1152,2,{150,150,160,90}),44,204);
        EnsureQuests(read);
        if(!states || !states->valid || !quests.valid)
            Text(out,620,500,"Quests unavailable",FontFace::Rocio,38,Soft,'m','m');
        else if(QuestRows(*states).empty())Text(out,620,500,"None",FontFace::Rocio,38,Soft,'m','m');
        return out;
    }
    // Rows the native list shows (its own gate), in-progress first, then category and the
    // quest table's sort priority (matches the native order on the reference save).
    struct QuestRow { const QuestInfo* info; uint32_t state, destination; };
    std::vector<QuestRow> QuestRows(const QuestStates& states) {
        std::vector<QuestRow> rows;
        if(!states.valid || !quests.valid)return rows;
        for(const auto& q:states.entries)
            if(q.listed && (q.state==1 || q.state==2))
                if(const auto* info=quests.Find(q.quest);info && !info->hide_list)
                    rows.push_back({info,q.state,q.destination});
        std::stable_sort(rows.begin(),rows.end(),[](const QuestRow& a,const QuestRow& b) {
            if(a.state!=b.state)return a.state>b.state;
            if(a.info->category!=b.info->category)return a.info->category<b.info->category;
            return a.info->sort<b.info->sort;
        });
        return rows;
    }
    static constexpr int QuestRowH=128,QuestListW=1152;
    std::optional<Image> QuestListImage(const RangeReader& read,const QuestStates& states,int index) {
        EnsureQuests(read);
        if(!quests.valid)return std::nullopt;
        const auto rows=QuestRows(states);
        if(index<0 || index>=static_cast<int>(rows.size()))return std::nullopt;
        Image out{static_cast<u32>(QuestListW),static_cast<u32>(QuestRowH),std::vector<u8>(std::size_t(QuestListW)*QuestRowH*4)};
        const auto* icon=CommonGlyph(read,"UiTX_Common_MenuListIcon_Mask_Quest_png");
        for(std::size_t i=static_cast<std::size_t>(index);i==static_cast<std::size_t>(index);++i) {
            const auto& row=rows[i];
            const int y=8;
            Glyph(out,icon,0,y+8,48);
            if(row.info->hide_info) { // native "???" row: only the recommended level
                Text(out,68,y+24,"???",FontFace::Franco,36);
            } else {
                Text(out,68,y,FitName(row.info->title,FontFace::Franco,36,860),FontFace::Franco,36);
                if(!row.info->subtitle.empty())
                    Text(out,84,y+42,FitName(row.info->subtitle,FontFace::Rocio,34,880),FontFace::Rocio,34,Soft);
                if(const auto d=quests.texts.find(row.destination);row.destination && d!=quests.texts.end() && !d->second.empty())
                    Text(out,84,y+78,FitName("Next destination: "+d->second,FontFace::Rocio,34,1060),FontFace::Rocio,34,Soft);
            }
            if(row.info->level)
                Text(out,QuestListW,y+4,"Lv. "+std::to_string(row.info->level),FontFace::ClearTone,34,White,'r');
            if(i+1<rows.size())Over(out,Solid(QuestListW,1,{150,150,160,60}),0,QuestRowH-2);
        }
        return out;
    }
    std::optional<Image> MapGlyph(const RangeReader& read,const std::string& path) {
        if(const auto found=minimap_glyphs.find(path);found!=minimap_glyphs.end())return found->second;
        constexpr std::string_view folder="UI/Menu/Texture/Frames/";
        for(u32 i=0;i<static_cast<u32>(MemberId::Count);++i) {
            const auto id=static_cast<MemberId>(i);const auto rel=Member(id).path;
            if(!rel.starts_with(folder) || !rel.ends_with("_png.uexp"))continue;
            const auto name=rel.substr(folder.size(),rel.size()-folder.size()-5);
            if(!name.starts_with("UiTX_MainMenu_MiniMapIcon_"))continue;
            if(path!="/Game/"+std::string(folder)+std::string(name)+"."+std::string(name))continue;
            const auto bytes=ReadMember(read,Member(id),nullptr);
            if(!bytes || bytes->size()!=259 || !images.contains(MemberId::MinimapIcons))return std::nullopt;
            // Pinned PaperSprite SourceUV/SourceDimension, native double FVector2D.
            // Import is MiniMapIcon; no guessed atlas grid or semantic icon index.
            std::array<double,4> rect{};
            for(int i=0;i<4;++i)std::memcpy(&rect[i],bytes->data()+11+i*8,8);
            const auto& atlas=images.at(MemberId::MinimapIcons);
            if(!std::all_of(rect.begin(),rect.end(),[](double v){return std::isfinite(v) && v>=0 && std::floor(v)==v;}) ||
               rect[2]<1 || rect[3]<1 || rect[0]+rect[2]>atlas.width || rect[1]+rect[3]>atlas.height)return std::nullopt;
            auto glyph=Nearest(atlas,static_cast<u32>(rect[0]),static_cast<u32>(rect[1]),
                static_cast<u32>(rect[2]),static_cast<u32>(rect[3]),96,96);
            minimap_glyphs[path]=glyph;return glyph;
        }
        return std::nullopt;
    }
    static Image Tint(Image image,const std::array<float,4>& color,float opacity) {
        const auto srgb=[](float v) {return v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f;};
        const std::array<float,4> factors{srgb(color[0]),srgb(color[1]),srgb(color[2]),color[3]*opacity};
        for(std::size_t i=0;i<image.rgba.size();i+=4)
            for(int c=0;c<4;++c)image.rgba[i+c]=static_cast<u8>(std::lround(image.rgba[i+c]*factors[c]));
        return image;
    }
    void MapParty(Image& out,const RangeReader& read,const Snapshot& party,int selected=-1) {
        // Title on its own line; the column labels sit just above the pairs (no overlap).
        Text(out,TownMapLayout::PartyX+12,6,"Party",FontFace::Franco,36,Soft);
        Text(out,TownMapLayout::PartyX+108,78,"Front",FontFace::Rocio,28,Soft,'m','m');
        Text(out,TownMapLayout::PartyX+324,78,"Back",FontFace::Rocio,28,Soft,'m','m');
        int row=0;u32 art_id{};
        for(int pair=0;pair<4;++pair) {
            const auto& front=party.members[static_cast<std::size_t>(pair)];
            const auto& back=party.members[static_cast<std::size_t>(pair+4)];
            if(!front.available && !back.available)continue;
            if(!art_id)art_id=front.available?front.id:back.id;
            const int top=TownMapLayout::PairTop+row*TownMapLayout::PairPitch;
            Panel(out,TownMapLayout::PartyX,top,TownMapLayout::PartyW,TownMapLayout::PairH);
            Over(out,Solid(2,TownMapLayout::PairH-24,{150,150,160,90}),TownMapLayout::PartyX+215,top+12);
            for(int side=0;side<2;++side) {
                const auto& member=side?back:front;
                const int left=TownMapLayout::PartyX+side*216;
                if(member.available && (side?pair+4:pair)==selected)
                    Panel(out,left+2,top+2,212,TownMapLayout::PairH-4,true);
                if(!member.available) {
                    Text(out,left+108,top+100,"Empty",FontFace::Rocio,30,Dim,'m','m');continue;
                }
                Text(out,left+108,top+12,FitName(Name(member.id),FontFace::Franco,36,192),FontFace::Franco,36,side?Soft:White,'m','t');
                if(const auto* source=GetSprite(read,member.id)) {
                    const auto ink=Trim(*source);
                    if(!ink.rgba.empty()) {
                        const u32 w=source->width==132?ink.width*2/3:64;
                        const u32 h=source->height==132?ink.height*2/3:64;
                        Over(out,Nearest(ink,0,0,ink.width,ink.height,w,h),left+108-static_cast<int>(w)/2,top+44);
                    }
                }
                Text(out,left+12,top+128,"HP",FontFace::ClearTone,26,Soft,'l','t');
                Text(out,left+202,top+124,ResourceText(member.hp,member.max_hp),FontFace::ClearTone,32,White,'r','t');
                Text(out,left+12,top+168,"SP",FontFace::ClearTone,26,Soft,'l','t');
                Text(out,left+202,top+164,ResourceText(member.sp,member.max_sp),FontFace::ClearTone,32,White,'r','t');
            }
            ++row;
        }
        (void)art_id; // owner 2026-10-31: no large character art beside the party list
    }
    // Gains the native results added on dismissal: one line under the map pane until the next
    // battle.
    void LastBattleLine(Image& dst,const LastBattle& last) {
        std::string line="Last battle:";
        if(last.exp)line+="  +"+std::to_string(last.exp)+" EXP";
        if(last.money)line+="   +"+std::to_string(last.money)+" Leaves";
        if(last.levelup_count) {
            line+="   Level up: ";
            for(int i=0;i<last.levelup_count;++i)line+=(i?", ":"")+Name(last.levelups[static_cast<std::size_t>(i)]);
        }
        // Between the map pane and the footer (the header holds region and area names).
        Text(dst,24,924,FitName(line,FontFace::Rocio,28,740),FontFace::Rocio,28,Soft);
    }
    void StatusPane(Image& out,const RangeReader& read,const PartyMember& m,const MemberStats* stats) {
        Panel(out,TownMapLayout::X,TownMapLayout::Y,TownMapLayout::W,TownMapLayout::H);
        if(!m.available)return;
        const int l=TownMapLayout::InnerX,r=TownMapLayout::InnerX+TownMapLayout::InnerW;
        if(const auto* source=GetSprite(read,m.id)) {
            const auto ink=Trim(*source);
            if(!ink.rgba.empty()) {
                const u32 w=source->width==132?ink.width:96,h=source->height==132?ink.height:96;
                Over(out,Nearest(ink,0,0,ink.width,ink.height,w,h),r-static_cast<int>(w),TownMapLayout::InnerY);
            }
        }
        Text(out,l,TownMapLayout::InnerY+4,FitName(Name(m.id),FontFace::FrancoBold,52,520),FontFace::FrancoBold,52);
        Text(out,l,TownMapLayout::InnerY+72,"Lv. "+std::to_string(m.level),FontFace::ClearTone,38,Soft);
        const auto resource=[&](int y,const char* label,u32 value,u32 maximum,bool hp) {
            Text(out,l,y,label,FontFace::ClearTone,32,Soft);
            Text(out,r,y-6,ResourceText(value,maximum),FontFace::ClearTone,44,White,'r');
            Gauge(out,l,y+50,static_cast<u32>(r-l),8,value,maximum,hp);
        };
        resource(TownMapLayout::InnerY+150,"HP",m.hp,m.max_hp,true);
        resource(TownMapLayout::InnerY+240,"SP",m.sp,m.max_sp,false);
        Text(out,l,TownMapLayout::InnerY+330,"EXP",FontFace::ClearTone,32,Soft);
        Text(out,r,TownMapLayout::InnerY+324,std::to_string(m.exp),FontFace::ClearTone,40,White,'r');
        Over(out,Solid(static_cast<u32>(r-l),2,{150,150,160,90}),l,TownMapLayout::InnerY+396);
        // Native Status page order; values only from the game (no estimates).
        constexpr std::array<const char*,8> labels{"Phys. Atk","Elem. Atk","Phys. Def","Elem. Def",
                                                   "Accuracy","Speed","Critical","Evasion"};
        for(int i=0;i<8;++i) {
            const int col=i%2,row=i/2;
            const int x=l+col*(TownMapLayout::InnerW/2+8),y=TownMapLayout::InnerY+430+row*86;
            const int w=TownMapLayout::InnerW/2-24;
            Text(out,x,y,labels[static_cast<std::size_t>(i)],FontFace::Rocio,30,Soft);
            // All eight match the native Status page (live 2026-10-31: Eden, Leader, Celsus); the
            // helper leaves out the weapon's own bonus as that page does.
            const bool known=stats && stats->valid && stats->id==m.id;
            Text(out,x+w,y-4,known?std::to_string(stats->values[static_cast<std::size_t>(i)]):"—",
                 FontFace::ClearTone,40,known?White:Dim,'r');
        }
    }
    Image MapPage(const RangeReader& read,const Snapshot& party,const ExplorationSnapshot* map,const MinimapIcons* icons,int selected=-1,bool heal_shown=false) {
        auto out=backdrop;
        Ensure(read,MemberId::IconMap,nullptr);Ensure(read,MemberId::MinimapIcons,nullptr);
        std::string title="Local Map";
        if(map && map->valid) {
            if(!map_names_loaded) {
                auto bytes=ReadMember(read,Member(MemberId::TextMap),nullptr);
                if(bytes) {map_names=DecodeMapNames(*bytes);map_names_loaded=map_names.valid;}
            }
            const auto name=map_names.LookupName(map->map_name_id);if(!name.empty())title=name;
        }
        MapHeader(out,read,map,title,heal_shown?636:776);
        MapParty(out,read,party,selected);
        Panel(out,TownMapLayout::X,TownMapLayout::Y,TownMapLayout::W,TownMapLayout::H);
        if(!map || !map->valid) {
            Text(out,396,508,"Local Map Unavailable",FontFace::Rocio,38,Soft,'m','m');return out;
        }
        MapBase(read,*map);
        if(map_load_failed || map_source.rgba.empty()) {
            Text(out,396,508,"Local Map Unavailable",FontFace::Rocio,38,Soft,'m','m');return out;
        }
        if(map_picture.rgba.empty()) {
            Text(out,396,508,"No revealed paths",FontFace::Rocio,38,Soft,'m','m');return out;
        }
        // The picture, the player and the icons are separate runtime widgets now (see
        // MapGeometry); this page only frames them. Completeness still requires every native
        // icon glyph and the player glyph to be drawable, as before.
        bool complete=icons && icons->valid && icons->complete && icons->map_id==map->map_id &&
                      MapGlyph(read,"/Game/UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_01_png.UiTX_MainMenu_MiniMapIcon_01_png").has_value();
        if(complete)for(const auto& icon:icons->icons)
            if(!MapGlyph(read,icon.frame_path) ||
               (!icon.background_frame_path.empty() && !MapGlyph(read,icon.background_frame_path))) {complete=false;break;}
        if(visuals) {
            visuals->map_complete=complete;
            visuals->map={true,map_source.width,map_source.height,map_picture.width,map_picture.height,
                          double(map_crop_x),double(map_crop_y),map_revision};
        }
        return out;
    }
    void HealWindow(Image& out,const HealScreenView& heal) {
        Panel(out,16,486,760,436);
        Text(out,44,508,"Healing Skills",FontFace::FrancoBold,42);
        Text(out,44,568,FitName(Name(heal.target),FontFace::Franco,36,360),FontFace::Franco,36);
        if(heal.max_hp>0)
            Text(out,744,568,"HP  "+std::to_string(heal.hp)+"/"+std::to_string(heal.max_hp),FontFace::ClearTone,36,White,'r');
        if(heal.ready) {
            Text(out,44,626,FitName(heal.skill,FontFace::Franco,40,688),FontFace::Franco,40);
            Text(out,44,686,FitName(Name(heal.caster_id),FontFace::Rocio,34,440),FontFace::Rocio,34,Soft);
            Text(out,744,686,std::to_string(heal.cost)+" SP",FontFace::ClearTone,38,White,'r');
        } else {
            Text(out,396,652,heal.checking?"Checking healing skills…":heal.applying?"Healing…":heal.message,
                 FontFace::Rocio,34,Soft,'m','m');
        }
        Panel(out,44,782,328,112,false);
        Text(out,208,838,heal.done?"Close":"Cancel",FontFace::Rocio,38,White,'m','m');
        Panel(out,400,782,348,112,heal.ready);
        Text(out,574,838,heal.ready?"Heal":heal.done?"Healed":"Heal",FontFace::Rocio,38,
             heal.ready?White:Dim,'m','m');
        // GPU map pulses are behind this modal window, rather than floating
        // over its native text/buttons. The map's completeness flag survives.
        if(visuals)visuals->pulses.clear();
    }
    // ---------------- battle page (owner-approved C content + A native rail/panels) ----------
    Enemies enemies;
    WeaponTypes weapon_types;
    bool enemies_loaded{};
    unsigned enemies_attempts{};
    std::map<std::string,Image> enemy_frames;
    Image battle_backdrop;
    static constexpr std::array<std::array<u32,4>,15> TileBox{{
        {2,2,29,29},{2,33,29,29},{2,64,29,29},{2,157,29,29},{2,188,29,29},{2,219,29,29},   // dark fire light thunder ice wind
        {405,33,26,26},{405,61,26,26},{405,89,26,26},{405,117,26,26},{405,145,26,26},      // bow axe tome staff dagger
        {405,173,26,26},{405,341,26,26},{405,369,26,26},{405,425,26,26}}};                  // fan polearm sword unknown
    // EWEAPON 1..8 / EMAGIC 1..6 -> TileBox index and label.
    static constexpr std::array<int,9> WeaponTile{-1,13,12,10,7,6,9,8,11};
    static constexpr std::array<int,7> MagicTile{-1,1,4,3,5,2,0};
    static constexpr std::array<const char*,9> WeaponName{"","Sword","Polearm","Dagger","Axe","Bow","Staff","Tome","Fan"};
    static constexpr std::array<const char*,7> MagicName{"","Fire","Ice","Thunder","Wind","Light","Dark"};
    Image Tile(int index,u32 size) const {
        const auto& b=TileBox[static_cast<std::size_t>(index)];
        return Resize(images.at(MemberId::BattleIcon),b[0],b[1],b[2],b[3],size,size);
    }
    bool battle_assets_warm{};
    bool BattleAssets(const RangeReader& read,std::string* why) {
        for(auto id:{MemberId::FocusLight,MemberId::BattleIcon,MemberId::ShieldIcon,MemberId::ShieldCrack,MemberId::BpDisc,
                     MemberId::ListBase,MemberId::IconMisc})
            if(!Ensure(read,id,why))return false;
        if(!enemies_loaded && enemies_attempts<3) {
            ++enemies_attempts;
            std::array<std::optional<std::vector<u8>>,6> t;
            const std::array<MemberId,6> ids{MemberId::EnemyDb,MemberId::EnemyTypeDb,MemberId::EnemyAnimDb,
                                             MemberId::EnemyTexDb,MemberId::EnemyWeakDb,MemberId::TextEnemy};
            for(std::size_t i=0;i<6;++i){t[i]=ReadMember(read,Member(ids[i]),why);if(!t[i])return false;}
            enemies=DecodeEnemies(*t[0],*t[1],*t[2],*t[3],*t[4],*t[5]);
            auto items=ReadMember(read,Member(MemberId::ItemDb),why);if(!items)return false;
            weapon_types=DecodeWeaponTypes(*items);
            enemies_loaded=enemies.valid && weapon_types.valid;
            if(!enemies_loaded){if(why)*why="Native enemy table validation failed";return false;}
        }
        if(battle_backdrop.rgba.empty()) {
            battle_backdrop=Solid(Width,Height,{6,7,10,255});
            Over(battle_backdrop,Alpha(Scale(images.at(MemberId::ListBase),Width,Height),.9),0,0);
        }
        return enemies_loaded;
    }
    // First animation frame of an enemy sheet: rows split on transparent gaps, then columns.
    const Image* EnemyFrame(const RangeReader& read,const std::string& texture) {
        if(texture.empty())return nullptr;
        if(auto it=enemy_frames.find(texture);it!=enemy_frames.end())return it->second.rgba.empty()?nullptr:&it->second;
        auto sheet=LoadEnemyTexture(read,texture,nullptr);
        Image frame;
        if(sheet) {
            const auto alpha_at=[&](u32 x,u32 y){return sheet->rgba[(static_cast<std::size_t>(y)*sheet->width+x)*4+3]>8;};
            u32 y0=0,y1=0;bool in=false;
            for(u32 y=0;y<=sheet->height;++y) {
                bool any=false;
                if(y<sheet->height)for(u32 x=0;x<sheet->width && !any;++x)any=alpha_at(x,y);
                if(any && !in){in=true;y0=y;}
                if(!any && in){y1=y;break;}
            }
            u32 x0=0,x1=0;in=false;
            for(u32 x=0;x<=sheet->width && y1>y0;++x) {
                bool any=false;
                if(x<sheet->width)for(u32 y=y0;y<y1 && !any;++y)any=alpha_at(x,y);
                if(any && !in){in=true;x0=x;}
                if(!any && in){x1=x;break;}
            }
            if(y1>y0 && x1>x0)frame=Trim(Resize(*sheet,x0,y0,x1-x0,y1-y0,x1-x0,y1-y0));
        }
        auto [it,inserted]=enemy_frames.emplace(texture,std::move(frame));(void)inserted;
        return it->second.rgba.empty()?nullptr:&it->second;
    }
    void ShieldBadge(Image& dst,int x,int y,int shields,bool broken,u32 size) {
        if(broken) {
            const auto& c=images.at(MemberId::ShieldCrack);const u32 fw=c.width/3;
            Over(dst,Resize(c,2*fw,0,fw,c.height,size,size),x,y);
            Text(dst,x+static_cast<int>(size)/2,y+static_cast<int>(size)+6,"Break",FontFace::ClearTone,28,{240,200,120,255},'m','a');
            return;
        }
        Over(dst,Scale(images.at(MemberId::ShieldIcon),size,size),x,y);
        Text(dst,x+static_cast<int>(size)/2,y+static_cast<int>(size)/2+2,std::to_string(shields),FontFace::ClearTone,
             size>90?54.f:40.f,White,'m','m');
    }
    struct Slot { int tile; const char* name; bool revealed; };
    std::vector<Slot> WeakSlots(const EnemyInfo* info,const BattleEnemy& live) const {
        std::vector<Slot> out;
        if(!info)return out;
        for(int t=1;t<=8;++t)if(info->weapon_weak>>t&1)
            out.push_back({WeaponTile[static_cast<std::size_t>(t)],WeaponName[static_cast<std::size_t>(t)],bool(live.weapon_mask>>t&1)});
        for(int t=1;t<=6;++t)if(info->magic_weak>>t&1)
            out.push_back({MagicTile[static_cast<std::size_t>(t)],MagicName[static_cast<std::size_t>(t)],bool(live.magic_mask>>t&1)});
        return out;
    }
    void ActingWash(Image& dst,int x,int y,u32 w,u32 h) const {
        auto light=Scale(images.at(MemberId::FocusLight),w,h);
        for(std::size_t i=0;i<light.rgba.size();i+=4) {
            const auto ink=std::max({light.rgba[i],light.rgba[i+1],light.rgba[i+2]});
            light.rgba[i]=45;light.rgba[i+1]=112;light.rgba[i+2]=202;
            light.rgba[i+3]=static_cast<u8>(light.rgba[i+3]*ink/255*.75);
        }
        Over(dst,light,x,y);
        Over(dst,Solid(w-12,3,{117,187,246,230}),x+6,y+5);
    }
    void FocusPanel(Image& dst,const RangeReader& read,const BattleEnemy& live,const EnemyInfo* info,int top,int bottom,bool acting=false,
                    const ActorBattleStatuses* statuses=nullptr) {
        const int x=12,w=BattleLayout::FoeWidth,h=bottom-top;
        if(h<600) { OtherFoe(dst,read,top,live,info,acting,true,statuses); return; }
        Panel(dst,x,top,static_cast<u32>(w),static_cast<u32>(h),true);
        if(acting)ActingWash(dst,x,top,static_cast<u32>(w),static_cast<u32>(h));
        Text(dst,x+22,top+14,FitName(info?info->name:"",FontFace::FrancoBold,40,400),FontFace::FrancoBold,40);
        if(info && !info->level.empty()) {
            Text(dst,x+w-20,top+18,info->level,FontFace::ClearTone,36,White,'r');
            const auto lw=font.Measure(info->level,FontFace::ClearTone,36).value_or(30);
            Text(dst,x+w-28-lw,top+26,"Lv.",FontFace::ClearTone,26,Soft,'r');
        }
        const bool broken=live.alive && live.shields==0;
        ShieldBadge(dst,x+26,top+84,live.shields,broken,110);
        if(!broken && info)Text(dst,x+81,top+206,std::to_string(live.shields)+" / "+std::to_string(info->max_shields),FontFace::ClearTone,30,Soft,'m');
        Text(dst,x+26,top+248,"HP",FontFace::ClearTone,28,Soft);
        const auto hp_text=live.hp>=0?std::to_string(live.hp):"?";
        float hp_size=48;
        for(float candidate:{48.f,44.f,40.f,36.f,32.f,28.f}) {
            hp_size=candidate;if(font.Measure(hp_text,FontFace::ClearTone,hp_size).value_or(999)<=190)break;
        }
        Text(dst,x+26,top+274,hp_text,FontFace::ClearTone,hp_size);
        if(live.max_hp>0 && live.hp>=0 && live.hp<=live.max_hp) {
            Text(dst,x+26,top+330,"/"+std::to_string(live.max_hp),FontFace::ClearTone,30,Soft);
            Gauge(dst,x+26,top+374,190,8,static_cast<u32>(live.hp),static_cast<u32>(live.max_hp),true);
        }
        if(live.speed>=0) {
            Text(dst,x+26,top+406,"Speed",FontFace::ClearTone,26,Soft,'l','t');
            Text(dst,x+216,top+404,std::to_string(live.speed),FontFace::ClearTone,30,White,'r','t');
        }
        StatusStrip(dst,read,x+26,top+452,statuses,4,36);
        const auto slots=WeakSlots(info,live);
        const int by=bottom-160;
        if(const auto* frame=info?EnemyFrame(read,info->texture):nullptr) {
            u32 scale=7;
            while(scale>1 && (frame->height*scale>static_cast<u32>(std::max(1,by-top-90)) || frame->width*scale>300))--scale;
            auto spr=Nearest(*frame,0,0,frame->width,frame->height,frame->width*scale,frame->height*scale);
            if(broken)for(std::size_t i=0;i<spr.rgba.size();i+=4){spr.rgba[i]=static_cast<u8>(std::min(255,spr.rgba[i]+60));spr.rgba[i+1]/=2;spr.rgba[i+2]/=2;}
            Over(dst,spr,x+w-50-static_cast<int>(spr.width),by-50-static_cast<int>(spr.height));
        }
        if(slots.empty())return;
        Text(dst,x+22,by-40,"VULNERABLE",FontFace::Rocio,28,Soft);
        const int n=static_cast<int>(slots.size());const u32 size=n>7?56:68;
        const int gap=n>1?(w-44-n*static_cast<int>(size))/(n-1):0;
        int revealed=0;
        for(int i=0;i<n;++i) {
            const auto& sl=slots[static_cast<std::size_t>(i)];revealed+=sl.revealed;
            const int tx=x+22+i*(static_cast<int>(size)+gap);
            Over(dst,Tile(sl.revealed?sl.tile:14,size),tx,by);
            Text(dst,tx+static_cast<int>(size)/2,by+static_cast<int>(size)+12,
                 FitName(sl.revealed?sl.name:"?",FontFace::Rocio,28,n>1?static_cast<int>(size)+gap-6:128),
                 FontFace::Rocio,28,sl.revealed?White:Soft,'m');
        }
        Text(dst,x+22,by+static_cast<int>(size)+56,std::to_string(revealed)+" of "+std::to_string(n)+" weaknesses revealed",FontFace::Rocio,28,Soft);
    }
    void OtherFoe(Image& dst,const RangeReader& read,int y,const BattleEnemy& live,const EnemyInfo* info,bool acting=false,bool selected=false,
                  const ActorBattleStatuses* statuses=nullptr) {
        const int x=12,w=BattleLayout::FoeWidth,h=130;
        Panel(dst,x,y,static_cast<u32>(w),static_cast<u32>(h),selected);
        if(acting)ActingWash(dst,x,y,static_cast<u32>(w),static_cast<u32>(h));
        if(const auto* frame=info?EnemyFrame(read,info->texture):nullptr) {
            u32 scale=2;if(frame->height*scale>120)scale=1;
            auto spr=Nearest(*frame,0,0,frame->width,frame->height,frame->width*scale,frame->height*scale);
            Over(dst,spr,x+14,y+h-6-static_cast<int>(spr.height));
        }
        Text(dst,x+122,y+6,FitName(info?info->name:"",FontFace::Franco,34,340),FontFace::Franco,34,White,'l','t');
        Text(dst,x+122,y+44,"HP",FontFace::ClearTone,26,Soft,'l','t');
        Text(dst,x+166,y+42,live.hp>=0?ResourceText(live.hp,live.max_hp):"?",FontFace::ClearTone,30,White,'l','t');
        ShieldBadge(dst,x+w-86,y+14,live.shields,live.alive && live.shields==0,72);
        StatusStrip(dst,read,x+w-86-4*34,y+44,statuses,3,30);
        const auto slots=WeakSlots(info,live);
        for(std::size_t i=0;i<slots.size() && i<8;++i)
            Over(dst,Tile(slots[i].revealed?slots[i].tile:14,42),x+122+static_cast<int>(i)*54,y+82);
    }
    // A spent (boost-forecast) BP pip: the native disc desaturated and dimmed.
    static Image Grey(const Image& src) {
        Image out=src;
        for(std::size_t i=0;i+3<out.rgba.size();i+=4) {
            const u8 l=static_cast<u8>((out.rgba[i]*30+out.rgba[i+1]*59+out.rgba[i+2]*11)/100);
            out.rgba[i]=out.rgba[i+1]=out.rgba[i+2]=l;out.rgba[i+3]=static_cast<u8>(out.rgba[i+3]*3/4);
        }
        return out;
    }
    static void WeakTick(Image& dst,int x,int y,int size) {
        constexpr std::array<u8,4> red{230,70,60,255};
        for(int k=0;k<12;++k)Over(dst,Solid(static_cast<u32>(12-k),1,red),x+size-12+k,y+k);
    }
    static void Ring(Image& dst,int x,int y,int size) {
        constexpr std::array<u8,4> gold{226,196,128,255};
        for(int k=0;k<3;++k){
            Over(dst,Solid(static_cast<u32>(size+2*k),1,gold),x-k,y-k);Over(dst,Solid(static_cast<u32>(size+2*k),1,gold),x-k,y+size+k-1);
            Over(dst,Solid(1,static_cast<u32>(size+2*k),gold),x-k,y-k);Over(dst,Solid(1,static_cast<u32>(size+2*k),gold),x+size+k-1,y-k);
        }
    }
    void PairPanel(Image& dst,const RangeReader& read,int x,int y,u32 w,u32 h,const BattleMember* front,const BattleMember* back,
                   u32 focus_weak_weapons,u32 acting_id=0) {
        Panel(dst,x,y,w,h);
        const auto same=[](u32 a,u32 b){return a==b || (a>=900 && a<=909 && b>=900 && b<=909);};
        if(front && same(front->id,acting_id))ActingWash(dst,x+2,y+2,370,h-4);
        if(back && same(back->id,acting_id))ActingWash(dst,x+376,y+2,w-378,h-4);
        const auto draw_bp=[&](int bx,int by,int bp,u32 size,int boost=0) {
            for(int i=0;i<5;++i) {
                if(i<bp) {
                    const int px=bx+i*static_cast<int>(size-4);
                    const bool spent=i>=bp-boost;
                    Over(dst,Scale(spent?Grey(images.at(MemberId::BpDisc)):images.at(MemberId::BpDisc),size,size),px,by);
                    if(visuals)visuals->pulses.push_back({px+static_cast<int>(size)/2,by+static_cast<int>(size)/2,static_cast<int>(size),false,{255,255,255,255},spent});
                }
                else Over(dst,Solid(8,8,{120,118,116,220}),bx+i*static_cast<int>(size-4)+static_cast<int>(size)/2-4,by+static_cast<int>(size)/2-4);
            }
        };
        const auto resource=[&](int left,int right,int top,int value,int maximum,bool hp) {
            const bool known=maximum>0 && value>=0 && value<=maximum;
            const std::string text=std::to_string(value)+(known?"/"+std::to_string(maximum):"");
            float size=known?36.f:42.f;
            for(float candidate:{size,32.f,30.f,28.f}) {
                size=candidate;
                const auto width=font.Measure(text,FontFace::ClearTone,size);
                if(width && *width<=right-left)break;
            }
            Text(dst,right,top,text,FontFace::ClearTone,size,White,'r');
            if(!known)return;
            // Native gauge geometry and colours from the accepted battle
            // reference. No fill is drawn when the computed limit is unknown.
            Gauge(dst,left,top+45,static_cast<u32>(right-left),8,
                  static_cast<u32>(value),static_cast<u32>(maximum),hp);
        };
        if(front) {
            if(const auto* spr=GetSprite(read,front->id)) {
                auto s=spr->width>60?*spr:Nearest(*spr,0,0,spr->width,spr->height,spr->width*3,spr->height*3);
                Over(dst,s,x+2,y+static_cast<int>(h)-static_cast<int>(s.height)-6);
            }
            // Equipped weapon types; gold ring = hits a REVEALED weakness of the focused foe.
            // Gold ring = the current command weapon (follows the D-pad switch, as in the detail
            // panel); a red corner tick = hits a REVEALED weakness of the focused foe.
            int wx=x+12;
            for(std::size_t slot=0;slot<front->weapons.size();++slot) {
                const u32 type=weapon_types.Of(front->weapons[slot]);
                if(!type || type>8)continue;
                Over(dst,Tile(WeaponTile[type],44),wx,y+14);
                if(front->weapon_slot==static_cast<int>(slot))Ring(dst,wx,y+14,44);
                if(focus_weak_weapons>>type&1)WeakTick(dst,wx,y+14,44);
                wx+=54;
            }
            Text(dst,x+132,y+10,FitName(Name(front->id),FontFace::Franco,36,170),FontFace::Franco,36);
            StatusStrip(dst,read,x+8,y+64,PartyStatuses(front->id),3,30);
            Text(dst,x+132,y+64,"BP",FontFace::ClearTone,28,Soft);draw_bp(x+182,y+60,front->bp,28,front->boost);
            Text(dst,x+132,y+112,"HP",FontFace::ClearTone,28,Soft);
            resource(x+180,x+352,y+96,front->hp,front->max_hp,true);
            Text(dst,x+132,y+164,"SP",FontFace::ClearTone,28,Soft);
            resource(x+180,x+352,y+150,front->sp,front->max_sp,false);
        }
        Over(dst,Solid(2,static_cast<u32>(h-40),{150,150,160,90}),x+372,y+20);
        if(back) {
            const int bx=x+388,bx1=x+static_cast<int>(w)-14;
            Text(dst,bx,y+12,FitName(Name(back->id),FontFace::Franco,36,bx1-bx),FontFace::Franco,36,Soft);
            StatusStrip(dst,read,bx1-3*34,y+60,PartyStatuses(back->id),3,28);
            draw_bp(bx,y+60,back->bp,22,back->boost);
            resource(bx,bx1,y+96,back->hp,back->max_hp,true);
            resource(bx,bx1,y+150,back->sp,back->max_sp,false);
        } else Text(dst,x+388+(static_cast<int>(w)-402)/2,y+static_cast<int>(h)/2,"Empty",FontFace::Rocio,30,Dim,'m','m');
    }
    // Battle rows: the current battle's native statuses (set per BattlePage compose).
    const BattleStatusSnapshot* row_statuses{};
    const ActorBattleStatuses* PartyStatuses(u32 id) const {
        if(!row_statuses || !row_statuses->valid)return nullptr;
        for(const auto& s:row_statuses->party)
            if(s.valid && (s.id==id || (s.id>=900 && s.id<=909 && id>=900 && id<=909)))return &s;
        return nullptr;
    }
    // Up to `max` native status icons in a row (size px); "+N" when more are active.
    void StatusStrip(Image& dst,const RangeReader& read,int x,int y,const ActorBattleStatuses* s,int max,u32 size) {
        if(!s || !s->valid || s->entries.empty())return;
        int drawn=0;
        for(const auto& state:s->entries) {
            if(drawn>=max)break;
            const auto* icon=StatusGlyph(read,state.texture_path);
            if(!icon)continue;
            Over(dst,Scale(*icon,size,size),x+drawn*static_cast<int>(size+4),y);
            ++drawn;
        }
        const int more=static_cast<int>(s->entries.size())-drawn+static_cast<int>(s->unknown_count);
        if(more>0)Text(dst,x+drawn*static_cast<int>(size+4)+4,y+static_cast<int>(size)/2,"+"+std::to_string(more),
                       FontFace::ClearTone,24,Soft,'l','m');
    }
    std::map<std::string,Image> status_glyphs;
    const Image* StatusGlyph(const RangeReader& read,const std::string& path) {
        if(const auto found=status_glyphs.find(path);found!=status_glyphs.end())return &found->second;
        for(const auto& pin:StatusSpritePins) {
            if(pin.object_path!=path)continue;
            const auto frame=ReadMember(read,pin.frame,nullptr);
            const auto package=ReadMember(read,pin.package_header,nullptr);
            if(!frame || !package)return nullptr;
            const auto atlas=pin.atlas_path=="UI/Battle/Texture/Textures/Icon.uexp"?MemberId::BattleIcon:
                pin.atlas_path=="UI/Battle/Texture/Textures/Icon2.uexp"?MemberId::BattleIcon2:MemberId::Count;
            if(atlas==MemberId::Count || !Ensure(read,atlas,nullptr))return nullptr;
            const auto& picture=images.at(atlas);const auto& r=pin.rectangle;
            if(!r[2] || !r[3] || r[0]+r[2]>picture.width || r[1]+r[3]>picture.height)return nullptr;
            auto icon=Nearest(picture,r[0],r[1],r[2],r[3],r[2]*2,r[3]*2);
            return &status_glyphs.emplace(path,std::move(icon)).first->second;
        }
        return nullptr;
    }
    static bool SameHero(u32 a,u32 b) {
        return a==b || (a>=900 && a<=909 && b>=900 && b<=909);
    }
    void DetailStatuses(Image& out,const RangeReader& read,int x,const ActorBattleStatuses* statuses) {
        Text(out,x+26,802,"Status",FontFace::Rocio,34,Soft);
        if(!statuses || !statuses->valid) {
            Text(out,x+26,858,"—",FontFace::ClearTone,36,Dim);return;
        }
        if(statuses->entries.empty()) {
            Text(out,x+26,858,"—",FontFace::Rocio,30,Soft);return;
        }
        const std::size_t count=std::min<std::size_t>(3,statuses->entries.size());
        for(std::size_t i=0;i<count;++i) {
            const auto& state=statuses->entries[i];const int y=846+static_cast<int>(i)*68;
            if(const auto* icon=StatusGlyph(read,state.texture_path))Over(out,*icon,x+26,y);
            Text(out,x+96,y+9,FitName(state.name,FontFace::Rocio,34,350),FontFace::Rocio,34,White);
            Text(out,x+548,y+7,state.counter_valid?std::to_string(state.counter):"—",FontFace::ClearTone,34,
                 state.counter_valid?White:Dim,'r');
        }
        if(statuses->entries.size()>count || statuses->unknown_count)
            Text(out,x+548,1040,"+"+std::to_string(statuses->entries.size()-count+statuses->unknown_count),FontFace::ClearTone,32,Soft,'r');
    }
    void ActorDetail(Image& out,const RangeReader& read,const BattleSnapshot& battle,
                     const BattleDetailView& view,int index,bool enemy) {
        const int x=enemy?642:12;
        // The replaced column no longer owns its overview BP overlays.
        if(visuals)std::erase_if(visuals->pulses,[&](const auto& pulse){return !pulse.player && pulse.x>=x && pulse.x<x+588;});
        // Clear only the replaced column, keeping native roster/foe overview opposite.
        Over(out,Resize(battle_backdrop,x,132,588,936,588,936),x,132);
        Panel(out,x,132,576,936,true);
        const BattleMember* hero=enemy?nullptr:&battle.members[static_cast<std::size_t>(index)];
        const BattleEnemy* foe=enemy?&battle.enemies[static_cast<std::size_t>(index)]:nullptr;
        const EnemyInfo* info=foe?enemies.Find(foe->enemy_id):nullptr;
        const auto name=hero?Name(hero->id):info?info->name:"";
        Text(out,x+26,151,FitName(name,FontFace::FrancoBold,40,520),FontFace::FrancoBold,40);
        if(hero) {
            if(const auto* sprite=GetSprite(read,hero->id)) {
                const u32 scale=sprite->width>60?1:3;
                const auto picture=Nearest(*sprite,0,0,sprite->width,sprite->height,sprite->width*scale,sprite->height*scale);
                Over(out,picture,x+480-static_cast<int>(picture.width)/2,344-static_cast<int>(picture.height));
            }
            Text(out,x+26,222,"HP",FontFace::ClearTone,28,Soft);
            Text(out,x+330,217,ResourceText(hero->hp,hero->max_hp),FontFace::ClearTone,36,White,'r');
            Text(out,x+26,272,"SP",FontFace::ClearTone,28,Soft);
            Text(out,x+330,267,ResourceText(hero->sp,hero->max_sp),FontFace::ClearTone,36,White,'r');
            Text(out,x+26,322,"BP",FontFace::ClearTone,28,Soft);
            for(int bp=0;bp<5;++bp) {
                if(bp<hero->bp) {
                    const bool spent=bp>=hero->bp-hero->boost;
                    Over(out,Scale(spent?Grey(images.at(MemberId::BpDisc)):images.at(MemberId::BpDisc),28,28),x+100+bp*34,322);
                    if(visuals)visuals->pulses.push_back({x+114+bp*34,336,28,false,{255,255,255,255},spent});
                }
                else Over(out,Solid(8,8,{120,118,116,220}),x+110+bp*34,332);
            }
        } else {
            if(const auto* sprite=info?EnemyFrame(read,info->texture):nullptr) {
                u32 scale=3;while(scale>1 && (sprite->width*scale>190 || sprite->height*scale>150))--scale;
                // Large native frames keep1x pixels and crop inside the portrait
                // window rather than painting over identity/resources.
                const auto w=std::min<u32>(sprite->width,190/scale),h=std::min<u32>(sprite->height,150/scale);
                auto picture=Nearest(*sprite,(sprite->width-w)/2,0,w,h,w*scale,h*scale);
                Over(out,picture,x+466-static_cast<int>(picture.width)/2,352-static_cast<int>(picture.height));
            }
            Text(out,x+26,222,"HP",FontFace::ClearTone,28,Soft);
            Text(out,x+330,217,foe->hp>=0?ResourceText(foe->hp,foe->max_hp):"—",FontFace::ClearTone,36,White,'r');
            ShieldBadge(out,x+26,274,foe->shields,foe->alive && foe->shields==0,72);
            // Keep current caster resources visible while the native party HUD is hidden.
            for(int i=0;i<battle.member_count;++i) {
                const auto& caster=battle.members[static_cast<std::size_t>(i)];
                if(!SameHero(caster.id,battle.command_actor_id))continue;
                Text(out,x+26,364,"Acting",FontFace::Rocio,34,Soft);
                Text(out,x+150,358,FitName(Name(caster.id),FontFace::Franco,38,398),FontFace::Franco,38,Soft);
                Text(out,x+26,410,"HP",FontFace::ClearTone,30,Soft);
                Text(out,x+280,410,ResourceText(caster.hp,caster.max_hp),FontFace::ClearTone,32,White,'r');
                Text(out,x+310,410,"SP",FontFace::ClearTone,30,Soft);
                Text(out,x+548,410,ResourceText(caster.sp,caster.max_sp),FontFace::ClearTone,32,White,'r');
                Text(out,x+26,450,"BP",FontFace::ClearTone,30,Soft);
                for(int bp=0;bp<5;++bp) {
                    if(bp<caster.bp) {
                        const bool spent=bp>=caster.bp-caster.boost;
                        Over(out,Scale(spent?Grey(images.at(MemberId::BpDisc)):images.at(MemberId::BpDisc),28,28),x+100+bp*34,450);
                        if(visuals)visuals->pulses.push_back({x+114+bp*34,464,28,false,{255,255,255,255},spent});
                    } else Over(out,Solid(8,8,{120,118,116,220}),x+110+bp*34,460);
                }
                break;
            }
        }
        const ActorCombatStats* stats=nullptr;
        if(view.stats && view.stats->valid) {
            const auto& row=enemy?view.stats->enemies[static_cast<std::size_t>(index)]:view.stats->party[static_cast<std::size_t>(index)];
            if(row.bound && (enemy?row.id==foe->enemy_id:SameHero(row.id,hero->id)))stats=&row;
        }
        if(hero) {
            const int current=stats && stats->weapon_context_valid && stats->parameters[0].valid?
                stats->equipment_weapon_slot:-1;
            const auto current_type=current>=0 && current<static_cast<int>(hero->weapons.size())?
                weapon_types.Of(hero->weapons[static_cast<std::size_t>(current)]):0u;
            const bool known=current_type>0 && current_type<WeaponName.size();
            int wx=x+382;
            for(std::size_t slot=0;slot<hero->weapons.size();++slot) {
                const auto type=weapon_types.Of(hero->weapons[slot]);
                if(type>0 && type<WeaponTile.size()) {
                    Over(out,Tile(WeaponTile[type],42),wx,350);
                    if(known && current==static_cast<int>(slot))Ring(out,wx,350,42);
                }
                wx+=66;
            }
            // Here gold identifies the current calculation weapon. The explicit
            // caption separates it from revealed-weakness rings in the roster.
            Text(out,x+382,410,"Current",FontFace::Rocio,34,Soft);
            Text(out,x+382,450,known?WeaponName[current_type]:"—",FontFace::Rocio,34,known?White:Dim);
        }
        Over(out,Solid(520,2,{150,150,160,90}),x+26,492);
        // Native Status order: physical/accuracy/critical, elemental/speed/evasion.
        constexpr std::array<std::size_t,8> order{0,1,6,5,2,3,4,7};
        for(std::size_t cell=0;cell<order.size();++cell) {
            const auto i=order[cell];const int column=static_cast<int>(cell/4);
            const int y=516+static_cast<int>(cell%4)*62,left=x+26+column*270;
            const auto label=view.metadata && view.metadata->valid?view.metadata->parameters[i].name:std::string{};
            Text(out,left,y,FitName(label,FontFace::Rocio,34,182),FontFace::Rocio,34,Soft);
            const bool known=stats && stats->parameters[i].valid;
            Text(out,left+250,y,known?std::to_string(stats->parameters[i].value):"—",FontFace::ClearTone,36,known?White:Dim,'r');
        }
        const ActorBattleStatuses* statuses=nullptr;
        if(view.statuses && view.statuses->valid) {
            const auto& row=enemy?view.statuses->enemies[static_cast<std::size_t>(index)]:view.statuses->party[static_cast<std::size_t>(index)];
            if(enemy?row.id==foe->enemy_id:SameHero(row.id,hero->id))statuses=&row;
        }
        DetailStatuses(out,read,x,statuses);
    }
    Image BattlePage(const RangeReader& read,const Snapshot& party,const BattleSnapshot& b,int focus,const BattleDetailView* detail) {
        auto out=battle_backdrop;
        row_statuses=detail?detail->statuses:nullptr;
        // Native top keeps both turn rails; use this screen for foe/party detail.
        const TurnEntry command_actor{false,b.command_actor_id};
        const TurnEntry* acting=b.command_actor_id?&command_actor:
            b.now_index>=0 && b.now_index<b.now_count && b.now[static_cast<std::size_t>(b.now_index)].enemy?
            &b.now[static_cast<std::size_t>(b.now_index)]:nullptr;
        // A party queue entry names the pair's original front member even
        // after ZR. Without a visible native binding, leave its wash off.
        Text(out,24,42,"Enemies",FontFace::Franco,40,Soft);
        Text(out,660,42,"Party",FontFace::Franco,40,Soft);
        const auto divider=Nine(images.at(MemberId::InBox),8,936);
        Over(out,Alpha(divider,.65),611,132);
        Panel(out,1112,16,112,112);
        Over(out,Scale(images.at(MemberId::IconMisc),52,52),1142,46);
        const auto layout=ComputeBattleLayout(b,focus);
        const int oth_h=layout.other_count*140;
        if(layout.focus>=0) {
            const auto& live=b.enemies[static_cast<std::size_t>(layout.focus)];
            const auto fi=static_cast<std::size_t>(layout.focus);
            const ActorBattleStatuses* fs=row_statuses && row_statuses->valid && fi<row_statuses->enemies.size() &&
                row_statuses->enemies[fi].valid && row_statuses->enemies[fi].id==live.enemy_id?&row_statuses->enemies[fi]:nullptr;
            FocusPanel(out,read,live,enemies.Find(live.enemy_id),132,1080-12-oth_h,acting && acting->enemy && acting->ref==static_cast<u32>(layout.focus),fs);
        }
        for(int i=0;i<layout.other_count;++i) {
            const auto& live=b.enemies[static_cast<std::size_t>(layout.others[static_cast<std::size_t>(i)])];
            const auto foe_index=static_cast<std::size_t>(layout.others[static_cast<std::size_t>(i)]);
            const ActorBattleStatuses* foe_statuses=row_statuses && row_statuses->valid && foe_index<row_statuses->enemies.size() &&
                row_statuses->enemies[foe_index].valid && row_statuses->enemies[foe_index].id==live.enemy_id?&row_statuses->enemies[foe_index]:nullptr;
            OtherFoe(out,read,BattleLayout::RowTop(layout.other_count,i),live,enemies.Find(live.enemy_id),acting && acting->enemy && acting->ref==static_cast<u32>(layout.others[static_cast<std::size_t>(i)]),false,foe_statuses);
        }
        // Party: native front/back pairs (formation slots i / i+4) with battle values by id.
        const auto find=[&](u32 id)->const BattleMember*{
            const auto leader=[](u32 v){return v>=900 && v<=909;}; // protagonist job aliases
            for(int i=0;i<b.member_count;++i) {
                const u32 m=b.members[static_cast<std::size_t>(i)].id;
                if(m==id || (leader(m) && leader(id)))return &b.members[static_cast<std::size_t>(i)];
            }
            return nullptr;};
        const int ph=(1080-132-8)/4;
        int row=0;
        u32 illustration_id{};
        for(int i=0;i<4;++i) {
            const auto& f=party.members[static_cast<std::size_t>(i)];const auto& k=party.members[static_cast<std::size_t>(i+4)];
            if(!f.available && !k.available)continue;
            if(!illustration_id)illustration_id=f.available?f.id:k.id;
            PairPanel(out,read,642,132+row*ph,588,static_cast<u32>(ph-6),f.available?find(f.id):nullptr,k.available?find(k.id):nullptr,
                      layout.focus>=0?b.enemies[static_cast<std::size_t>(layout.focus)].weapon_mask:0u,
                      acting && !acting->enemy?acting->ref:0u);
            ++row;
        }
        // Keep native pair proportions for a solo/small party. The accepted
        // Formation illustration fills the freed area instead of vacant rows.
        if(row>0 && row<=2) {
            if(const auto id=Art(illustration_id);id && Ensure(read,*id,nullptr)) {
                auto art=TrimmedArt(*id);
                if(!art.rgba.empty()) {
                    const int top=132+row*ph+24;
                    const double factor=std::min(540.0/art.width,double(1060-top)/art.height);
                    auto picture=Scale(art,static_cast<u32>(std::lround(art.width*factor)),
                                           static_cast<u32>(std::lround(art.height*factor)));
                    Over(out,picture,936-static_cast<int>(picture.width)/2,top);
                }
            }
        }
        if(detail) {
            Panel(out,864,16,232,96,true);
            Text(out,980,64,detail->automatic?"Auto":"Overview",FontFace::Rocio,34,White,'m','m');
            const auto* selection=detail->selection;
            if(detail->automatic && selection && selection->valid) {
                if(selection->phase==BattleSelectionPhase::EnemyTarget && selection->enemy_index>=0 && selection->enemy_index<b.enemy_count) {
                    ActorDetail(out,read,b,*detail,selection->enemy_index,true);
                    Over(out,Resize(battle_backdrop,642,0,218,112,218,112),642,0);
                    Text(out,660,42,"Target",FontFace::Franco,40,Soft);
                } else if(selection->phase==BattleSelectionPhase::Command || selection->phase==BattleSelectionPhase::AllyTarget) {
                    const auto id=selection->phase==BattleSelectionPhase::AllyTarget?selection->ally_id:selection->actor_id;
                    for(int i=0;i<b.member_count;++i)if(SameHero(b.members[static_cast<std::size_t>(i)].id,id)) {
                        ActorDetail(out,read,b,*detail,i,false);
                        Over(out,Resize(battle_backdrop,12,0,576,112,576,112),12,0);
                        Text(out,24,42,selection->phase==BattleSelectionPhase::AllyTarget?"Ally Target":"Acting Party",FontFace::Franco,40,Soft);
                        break;
                    }
                }
            }
        }
        return out;
    }
    void Solo(Image& dst,const RangeReader& read,const Snapshot& snapshot,int selected) {
        Panel(dst,96,120,1048,760,true);
        const auto& member=snapshot.members[static_cast<std::size_t>(selected)];
        if(const auto id=Art(member.id);id && Ensure(read,*id,nullptr)) {
            auto art=TrimmedArt(*id);
            if(!art.rgba.empty()) {
                const double scale=520.0/std::max(art.width,art.height);
                auto scaled=Scale(art,static_cast<u32>(std::lround(art.width*scale)),
                                       static_cast<u32>(std::lround(art.height*scale)));
                Over(dst,scaled,390-static_cast<int>(scaled.width)/2,184);
            }
        }
        if(const auto* sprite=GetSprite(read,member.id))Over(dst,*sprite,956,176);
        Text(dst,724,180,selected<4?"Front Row":"Back Row",FontFace::Rocio,36,Soft);
        Text(dst,724,248,FitName(Name(member.id),FontFace::FrancoBold,52,376),FontFace::FrancoBold,52);
        Text(dst,1100,330,"Lv. "+std::to_string(member.level),FontFace::ClearTone,38,White,'r');
        Text(dst,724,444,"HP",FontFace::ClearTone,36,Soft);
        Text(dst,1100,425,ResourceText(member.hp,member.max_hp),FontFace::ClearTone,52,White,'r');
        Gauge(dst,724,494,376,8,member.hp,member.max_hp,true);
        Text(dst,724,564,"SP",FontFace::ClearTone,36,Soft);
        Text(dst,1100,545,ResourceText(member.sp,member.max_sp),FontFace::ClearTone,52,White,'r');
        Gauge(dst,724,614,376,8,member.sp,member.max_sp,false);
    }
    Image Intermission(const RangeReader& read) {
        auto out=backdrop;
        if(Ensure(read,MemberId::ArtLeader,nullptr)) {
            auto art=TrimmedArt(MemberId::ArtLeader);
            if(!art.rgba.empty()) {
                const double factor=760.0/std::max(art.width,art.height);
                auto image=Scale(art,static_cast<u32>(std::lround(art.width*factor)),
                                     static_cast<u32>(std::lround(art.height*factor)));
                Over(out,image,620-static_cast<int>(image.width)/2,100);
            }
        }
        const auto& logo=images.at(MemberId::Logo);
        const u32 height=static_cast<u32>(std::lround(480.0*logo.height/logo.width));
        Over(out,Alpha(Scale(logo,480,height),.9),380,920-static_cast<int>(height)/2);
        return out;
    }
    void Card(Image& dst,const RangeReader& read,const Snapshot& snapshot,int slot,bool selected,const FormationSlotRect& rect) {
        constexpr u32 w=312,h=160;
        Image card=Solid(w,h,{0,0,0,0});Panel(card,0,0,w,h,selected);
        const auto& member=snapshot.members[static_cast<std::size_t>(slot)];
        if(!member.available) {
            Text(card,156,80,"Empty",FontFace::Rocio,36,Dim,'m','m');
        } else {
            if(const auto* spr=GetSprite(read,member.id))Over(card,*spr,-7,26);
            const auto name=Name(member.id);
            float size=40;
            for(float candidate:{40.f,38.f,36.f}) {
                size=candidate;
                const auto width=font.Measure(name,FontFace::Franco,candidate);
                if(width && *width<=198)break;
            }
            Text(card,104,8,FitName(name,FontFace::Franco,size,198),FontFace::Franco,size);
            Text(card,298,53,"Lv. "+std::to_string(member.level),FontFace::ClearTone,32,Soft,'r');
            Text(card,118,88,"HP",FontFace::ClearTone,32,Soft);
            Text(card,298,85,std::to_string(member.hp),FontFace::ClearTone,36,White,'r');
            Text(card,118,125,"SP",FontFace::ClearTone,32,Soft);
            Text(card,298,122,std::to_string(member.sp),FontFace::ClearTone,36,White,'r');
        }
        const int x=rect.x,y=rect.y;
        Over(dst,card,x,y);
        if(selected)Over(dst,Scale(images.at(MemberId::Hand),48,48),x-22,y+54);
    }
};
ScreenComposer::ScreenComposer():impl_(std::make_unique<Impl>()){}
ScreenComposer::~ScreenComposer()=default;
ScreenComposer::ScreenComposer(ScreenComposer&&) noexcept=default;
ScreenComposer& ScreenComposer::operator=(ScreenComposer&&) noexcept=default;
std::optional<Image> ScreenComposer::MapPicture(const RangeReader& read,const ExplorationSnapshot& map,MapGeometry* geometry,
                                               const std::vector<MapPoint>& frame) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    if(!map.valid || !p.Base(read,nullptr))return std::nullopt;
    p.MapBase(read,map,frame);
    if(p.map_picture.rgba.empty())return std::nullopt;
    if(geometry)*geometry={true,p.map_source.width,p.map_source.height,p.map_picture.width,p.map_picture.height,
                           double(p.map_crop_x),double(p.map_crop_y),p.map_revision};
    return p.map_picture;
}
std::optional<Image> ScreenComposer::RingGlyph(const RangeReader& read,std::array<u8,4> color) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    if(!p.Ensure(read,MemberId::NotifyRing,nullptr))return std::nullopt;
    const auto& src=p.images.at(MemberId::NotifyRing);
    const u32 half=src.width/2;
    if(!half || src.height<half)return std::nullopt;
    Image ring{half,half,std::vector<u8>(std::size_t(half)*half*4)};
    for(u32 y=0;y<half;++y)for(u32 x=0;x<half;++x) {
        const auto* s=&src.rgba[(std::size_t(y)*src.width+x)*4];
        // Black background: brightness is coverage (doubled to match the minimap's ring strength).
        const u8 a=static_cast<u8>(std::min(255,2*std::max({s[0],s[1],s[2]})));
        auto* d=&ring.rgba[(std::size_t(y)*half+x)*4];
        for(int c=0;c<3;++c)d[c]=color[static_cast<std::size_t>(c)];
        d[3]=static_cast<u8>(a*color[3]/255);
    }
    return Scale(ring,96,96);
}
std::optional<Image> ScreenComposer::QuestList(const RangeReader& read,const QuestStates& states,int index) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    if(!p.Base(read,nullptr))return std::nullopt;
    return p.QuestListImage(read,states,index);
}
int ScreenComposer::QuestCount(const RangeReader& read,const QuestStates& states) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    p.EnsureQuests(read);
    return static_cast<int>(p.QuestRows(states).size());
}
std::optional<Image> ScreenComposer::RingFrame(const RangeReader& read,u32 frame) {
    if(frame>=RingFrames)return std::nullopt;
    const auto base=RingGlyph(read,{255,255,255,255});
    if(!base)return std::nullopt;
    Image out{RingFrameSize,RingFrameSize,std::vector<u8>(std::size_t(RingFrameSize)*RingFrameSize*4)};
    for(int k=0;k<2;++k) {
        const double t=std::fmod(double(frame)/RingFrames+0.5*k,1.0);
        const u32 size=std::max<u32>(1,static_cast<u32>(std::lround(96*(0.5+1.7*t))));
        const auto ring=Scale(*base,size,size);
        const int o=(int(RingFrameSize)-int(size))/2;
        for(u32 y=0;y<size;++y)for(u32 x=0;x<size;++x) {
            const int dx=o+int(x),dy=o+int(y);
            if(dx<0||dy<0||dx>=int(RingFrameSize)||dy>=int(RingFrameSize))continue;
            const double a=ring.rgba[(std::size_t(y)*size+x)*4+3]/255.0*(1-t);
            auto* d=&out.rgba[(std::size_t(dy)*RingFrameSize+dx)*4];
            const double da=d[3]/255.0,na=a+da*(1-a);
            for(int c=0;c<3;++c)d[c]=255;
            d[3]=static_cast<u8>(std::lround(na*255));
        }
    }
    return out;
}
std::optional<Image> ScreenComposer::IconGlyph(const RangeReader& read,const MinimapIcon& icon) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    auto glyph=p.MapGlyph(read,icon.frame_path);
    if(!glyph)return std::nullopt;
    Image out{96,96,std::vector<u8>(96*96*4)};
    if(!icon.background_frame_path.empty()) {
        auto background=p.MapGlyph(read,icon.background_frame_path);
        if(!background)return std::nullopt;
        Over(out,Impl::Tint(std::move(*background),icon.background_color,icon.opacity),0,0);
    }
    Over(out,Impl::Tint(std::move(*glyph),icon.color,icon.opacity),0,0);
    return out;
}
std::optional<Image> ScreenComposer::Glyph(const RangeReader& read,bool player,std::array<u8,4> color) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    const auto id=player?MemberId::MinimapIcons:MemberId::BpDisc;
    if(!p.Ensure(read,id,nullptr))return std::nullopt;
    if(!player) {
        auto disc=Scale(p.images.at(id),64,64);
        if(color!=std::array<u8,4>{255,255,255,255}) // spent pip: grey, dimmed
            for(std::size_t i=0;i+3<disc.rgba.size();i+=4) {
                const u8 l=static_cast<u8>((disc.rgba[i]*30+disc.rgba[i+1]*59+disc.rgba[i+2]*11)/100);
                disc.rgba[i]=disc.rgba[i+1]=disc.rgba[i+2]=static_cast<u8>(l*color[0]/255);
                disc.rgba[i+3]=static_cast<u8>(disc.rgba[i+3]*color[3]/255);
            }
        return disc;
    }
    auto source=p.MapGlyph(read,"/Game/UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_01_png.UiTX_MainMenu_MiniMapIcon_01_png");
    if(!source)return std::nullopt;
    auto glyph=Trim(*source);
    if(glyph.rgba.empty())return std::nullopt;
    auto out=p.Nearest(glyph,0,0,glyph.width,glyph.height,64,64);
    for(std::size_t i=0;i<out.rgba.size();i+=4)
        for(int c=0;c<4;++c)out.rgba[i+c]=static_cast<u8>(out.rgba[i+c]*color[c]/255);
    return out;
}
std::optional<Image> ScreenComposer::ComposeSettings(const RangeReader& read,bool hide_hud,
                                                     bool haptics,bool reduced_motion,
                                                     std::string* why) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    if(!p.Base(read,why) || !p.PartyAssets(read,why))return std::nullopt;
    auto out=p.backdrop;
    p.Text(out,24,18,"Settings",FontFace::FrancoBold,44);
    const std::array<const char*,3> labels{"Hide Top HUD","Touch Feedback","Reduced Motion"};
    const std::array<bool,3> values{hide_hud,haptics,reduced_motion};
    for(int i=0;i<3;++i) {
        const int y=228+i*190;
        p.Panel(out,48,y,1144,148,values[static_cast<std::size_t>(i)]);
        p.Text(out,80,y+68,labels[static_cast<std::size_t>(i)],FontFace::Franco,36,Soft,'l','m');
        p.Text(out,1154,y+68,values[static_cast<std::size_t>(i)]?"On":"Off",FontFace::FrancoBold,40,White,'r','m');
    }
    p.Panel(out,16,956,1208,112,true);
    p.Text(out,620,1012,"Back",FontFace::Rocio,38,White,'m','m');
    if(why)why->clear();
    return out;
}
std::optional<Image> ScreenComposer::Compose(const RangeReader& read,const Snapshot& snapshot,
                                            int selected,std::string* why,const ExplorationSnapshot* map,
                                            bool map_page,const ProtagonistAppearance* appearance,
                                            const BattleSnapshot* battle,int focus,bool modal_active,
                                            const ShopComparisonSnapshot* shop,ScreenVisuals* visuals,const MinimapIcons* icons,const BattleDetailView* detail,const HealScreenView* heal,const LastBattle* last,const MemberStats* stats,const PartyView* party_view) {
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& p=*impl_;
    p.visuals=visuals;
    struct VisualReset { Impl& impl; ~VisualReset(){impl.visuals=nullptr;} } reset{p};
    if(visuals)*visuals={};
    p.current_appearance=appearance;
    if(!p.Base(read,why))return std::nullopt;
    const bool world=snapshot.valid && snapshot.state>=23 && snapshot.state<=26;
    if(!world) {
        auto out=Solid(Width,Height,{0,0,0,255});
        const auto& logo=p.images.at(MemberId::Logo);
        const u32 h=static_cast<u32>(std::lround(520.0*logo.height/logo.width));
        Over(out,Alpha(Scale(logo,520,h),snapshot.state==10?.9:.55),360,300);
        p.Text(out,620,snapshot.state==10?760:740,
               snapshot.state==10?"Your companion opens once you continue a journey.":"Now Loading",
               FontFace::Rocio,snapshot.state==10?32.f:36.f,Soft,'m','m');
        if(why)why->clear();
        return out;
    }
    if(!p.PartyAssets(read,why))return std::nullopt;
    if(modal_active) {
        if(why)why->clear();
        return p.Intermission(read);
    }
    if(snapshot.state==26) {
        if(!battle || GetBattleFlow(*battle)!=BattleFlow::Combat) {
            if(why)why->clear();
            return p.Intermission(read);
        }
        if(!p.BattleAssets(read,why))return std::nullopt;
        auto out=p.BattlePage(read,snapshot,*battle,focus,detail);
        if(why)why->clear();
        return out;
    }
    const bool has_map=map && map->valid;
    if(has_map)p.Ensure(read,MemberId::IconMap,nullptr);
    p.Ensure(read,MemberId::IconMisc,nullptr);
    // Warm the battle assets once while exploring (decode on the field render, not at the first
    // battle frame): the battle page then appears without a multi-second delay on the Thor.
    if(!p.battle_assets_warm)p.battle_assets_warm=p.BattleAssets(read,nullptr);
    if(map_page) {
        auto out=p.MapPage(read,snapshot,map,icons,heal && heal->available?selected:-1,heal && heal->available);
        if(last && last->valid)p.LastBattleLine(out,*last);
        if(heal && heal->available) { // inline with the area name, top right of the map pane
            p.Panel(out,636,12,140,74,heal->open);
            p.Text(out,706,49,heal->open?"Close":"Heal",FontFace::Rocio,34,White,'m','m');
        }
        p.Footer(out,has_map,0,read);
        if(heal && heal->open)p.HealWindow(out,*heal);
        if(why)why->clear();
        return out;
    }
    if(party_view && party_view->quests_page) {
        auto out=p.QuestsPage(read,party_view->quests);
        p.Footer(out,has_map,3,read);
        if(why)why->clear();
        return out;
    }
    if(party_view && party_view->items_page) {
        auto out=p.ItemsPage(read,party_view->inventory,party_view->items_tab,party_view->items_first,
                             party_view->items_last_first);
        p.Footer(out,has_map,2,read);
        if(why)why->clear();
        return out;
    }
    // Party page (owner 2026-10-31): same frame as the map page; the left pane is the selected
    // member's status (the native Status page's values), the right pane the party list.
    selected=std::clamp(selected,0,7);
    if(!snapshot.members[static_cast<std::size_t>(selected)].available)
        for(int slot=0;slot<8;++slot)if(snapshot.members[static_cast<std::size_t>(slot)].available){selected=slot;break;}
    auto out=p.backdrop;
    const int tab=party_view?std::clamp(party_view->tab,0,1):0;
    p.PartyTabs(out,tab);
    p.MapParty(out,read,snapshot,selected);
    if(tab==1)p.EquipmentPane(out,read,snapshot.members[static_cast<std::size_t>(selected)],
                              party_view?party_view->gear:nullptr);
    else p.StatusPane(out,read,snapshot.members[static_cast<std::size_t>(selected)],stats);
    p.Footer(out,has_map,1,read);
    if(heal && heal->open)p.HealWindow(out,*heal);
    if(why)why->clear();
    return out;
}
} // namespace ot0

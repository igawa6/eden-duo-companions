// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_assets.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#ifdef ECHOES_TEST_DIAGNOSTICS
#include <cstdio>
#endif
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace echoes {
namespace {
constexpr size_t MaxLayout=128u<<10, MaxSource=32u<<20, MaxMetadata=1u<<20;
struct Bytes {
    std::span<const uint8_t> b;
    void Fits(size_t at,size_t n) const {if(at>b.size() || n>b.size()-at)throw std::runtime_error("range");}
    uint8_t U8(size_t at) const {Fits(at,1);return b[at];}
    uint16_t U16(size_t at) const {Fits(at,2);return uint16_t(b[at]|uint16_t(b[at+1])<<8);}
    uint32_t U32(size_t at) const {Fits(at,4);return b[at]|uint32_t(b[at+1])<<8|uint32_t(b[at+2])<<16|uint32_t(b[at+3])<<24;}
    uint64_t U64(size_t at) const {return U32(at)|(uint64_t(U32(at+4))<<32);}
    float F32(size_t at) const {const auto bits=U32(at);float f;std::memcpy(&f,&bits,4);if(!std::isfinite(f)||std::abs(f)>100000)throw std::runtime_error("float");return f;}
    std::string Str(size_t at,size_t capacity) const {
        Fits(at,capacity);size_t n=0;while(n<capacity && b[at+n])++n;
        if(n==capacity)throw std::runtime_error("string");
        for(size_t i=0;i<n;++i)if(b[at+i]<32 || b[at+i]>126)throw std::runtime_error("string encoding");
        return std::string(reinterpret_cast<const char*>(b.data()+at),n);
    }
    Bytes Sub(size_t at,size_t n) const {Fits(at,n);return {b.subspan(at,n)};}
    bool Tag(std::string_view s) const {return b.size()>=4 && !std::memcmp(b.data(),s.data(),4);}
};
struct Layout {
    std::vector<std::string> textures;
    std::vector<MapMaterial> materials;
    std::vector<MapPane> panes;
    std::vector<std::pair<int,Bytes>> pictures, parts;
};
MapPane Pane(Bytes s,int parent) {
    s.Fits(0,0x54);
    MapPane p;
    p.name=s.Str(12,24);p.parent=parent;p.flags=s.U8(8);p.origin=s.U8(9);p.alpha=s.U8(10);
    p.x=s.F32(0x2c);p.y=s.F32(0x30);p.z=s.F32(0x34);
    p.rotation_x=s.F32(0x38);p.rotation_y=s.F32(0x3c);p.rotation_z=s.F32(0x40);
    p.scale_x=s.F32(0x44);p.scale_y=s.F32(0x48);p.width=s.F32(0x4c);p.height=s.F32(0x50);
    if(p.name.empty() || p.width<0 || p.height<0 || p.origin>8)throw std::runtime_error("pane");
    return p;
}
Layout Parse(Bytes bytes) {
    if(bytes.b.size()>MaxLayout || !bytes.Tag("FLYT") || bytes.U16(4)!=0xfeff ||
       bytes.U32(8)!=0x09000000 || bytes.U32(12)!=bytes.b.size())throw std::runtime_error("layout header");
    const size_t start=bytes.U16(6);
    if(start<20)throw std::runtime_error("header size");
    Layout l;std::vector<int> stack{-1};int last=-1;size_t sections=0;
    bool tx_seen=false,mat_seen=false;
    for(size_t o=start;o<bytes.b.size();) {
        const size_t size=bytes.U32(o+4);if(size<8)throw std::runtime_error("section size");
        const auto s=bytes.Sub(o,size);++sections;
        if(s.Tag("txl1")) {
            if(tx_seen)throw std::runtime_error("duplicate texture list");
            tx_seen=true;
            const size_t n=s.U16(8);if(n>1024)throw std::runtime_error("texture count");
            s.Fits(12,n*4);
            for(size_t i=0;i<n;++i) {
                const size_t at=12+uint64_t(s.U32(12+4*i));
                if(at>=size)throw std::runtime_error("texture offset");
                l.textures.push_back(s.Str(at,std::min(size-at,size_t(256))));
            }
        } else if(s.Tag("mat1")) {
            if(mat_seen || !tx_seen)throw std::runtime_error("material order");
            mat_seen=true;
            const size_t n=s.U16(8);if(n>1024)throw std::runtime_error("material count");
            s.Fits(12,n*4);
            for(size_t i=0;i<n;++i) {
                const size_t at=s.U32(12+4*i), end=i+1<n?s.U32(12+4*(i+1)):size;
                if(at<12+4*n || end<at || end-at>4096)throw std::runtime_error("material offset");
                const auto m=s.Sub(at,end-at);m.Fits(0,44);
                MapMaterial v;v.name=m.Str(0,28);v.flags=m.U32(28);
                for(int c=0;c<4;++c){v.black[c]=m.U8(36+c);v.white[c]=m.U8(40+c);}
                for(size_t j=0;j<(v.flags&3);++j) {
                    const size_t ti=m.U16(44+4*j);if(ti>=l.textures.size())throw std::runtime_error("texture index");
                    v.textures.push_back(l.textures[ti]);
                }
                v.native_record.assign(m.b.begin(),m.b.end());l.materials.push_back(std::move(v));
            }
        } else if(s.Tag("pas1")) {
            if(last<0 || stack.size()>32)throw std::runtime_error("pane depth");
            stack.push_back(last);
        } else if(s.Tag("pae1")) {
            if(stack.size()<2)throw std::runtime_error("pane stack");
            stack.pop_back();
        } else if(s.Tag("pan1")||s.Tag("pic1")||s.Tag("prt1")||s.Tag("bnd1")||s.Tag("wnd1")||s.Tag("txt1")) {
            if(l.panes.size()>=1024)throw std::runtime_error("pane count");
            auto pane=Pane(s,stack.back());
            if(s.Tag("prt1")) {
                const size_t n=s.U32(0x54);if(n>64)throw std::runtime_error("part property count");
                const size_t at=0x60+40*n;if(at>=size)throw std::runtime_error("part layout offset");
                pane.part_layout=s.Str(at,std::min(size_t(256),size-at));
            }
            l.panes.push_back(std::move(pane));last=int(l.panes.size()-1);
            if(s.Tag("pic1"))l.pictures.emplace_back(last,s);
            if(s.Tag("prt1"))l.parts.emplace_back(last,s);
        }
        o+=size;
    }
    if(sections!=bytes.U16(16) || stack.size()!=1 || l.panes.empty())
        throw std::runtime_error("layout structure");
    std::set<std::string> names;for(const auto& p:l.panes)if(!names.insert(p.name).second)throw std::runtime_error("duplicate pane");
    return l;
}
MapPicture Picture(Bytes s,const Layout& l,int parent) {
    if(!s.Tag("pic1") || s.U32(4)!=s.b.size())throw std::runtime_error("picture section");
    MapPicture p;p.pane=Pane(s,parent);
    const size_t mi=s.U16(0x64),n=s.U8(0x66);
    if(mi>=l.materials.size() || n<1 || n>2)throw std::runtime_error("picture material");
    p.material=l.materials[mi];
    for(size_t i=0;i<4;++i)for(size_t c=0;c<4;++c)p.vertex_colors[i][c]=s.U8(0x54+4*i+c);
    for(size_t i=0;i<n;++i){std::array<float,8> uv{};for(size_t j=0;j<8;++j)uv[j]=s.F32(0x68+32*i+4*j);p.uv.push_back(uv);}
    return p;
}
FieldMapGeometry Geometry(Bytes field,Bytes traverse) {
    const auto f=Parse(field),t=Parse(traverse);FieldMapGeometry g;
    g.field_panes=f.panes;g.traverse_panes=t.panes;
    for(const auto& [id,s]:f.pictures)
        if(f.panes[id].name=="P_Terrain_00" || f.panes[id].name=="P_Terrain_01")g.terrain.push_back(Picture(s,f,f.panes[id].parent));
    if(g.terrain.size()!=2 || t.parts.size()!=108)throw std::runtime_error("Field topology");
    std::set<uint16_t> indices;
    for(const auto& [id,s]:t.parts) {
        TraverseShape shape;shape.pane=t.panes[id];std::string_view name=shape.pane.name;
        if(!name.starts_with("Traverse_"))throw std::runtime_error("traversal name");
        name.remove_prefix(9);
        unsigned index=512;const auto r=std::from_chars(name.data(),name.data()+name.size(),index);
        if(r.ec!=std::errc{} || r.ptr!=name.data()+name.size() || index>=512 || !indices.insert(uint16_t(index)).second)
            throw std::runtime_error("traversal index");
        shape.index=uint16_t(index);const size_t n=s.U32(0x54);
        if(n!=2)throw std::runtime_error("override count");
        if(s.Str(0x60+40*n,std::min(size_t(256),s.b.size()-(0x60+40*n)))!="L_MapTraverseItem")throw std::runtime_error("part name");
        for(size_t i=0;i<2;++i) {
            const size_t prop=0x60+40*i;const auto property=s.Str(prop,24);
            const size_t at=s.U32(prop+0x1c);s.Fits(at,8);
            auto picture=Picture(s.Sub(at,s.U32(at+4)),t,id);
            if(property!=picture.pane.name || property!=(i==0?"P_MapGrid_01":"P_MapGrid_00") ||
               picture.material.textures.size()!=2 || picture.material.textures[0]!="FieldTraverse_00 ^_H" ||
               picture.material.textures[1]!="FieldBreakThrough_"+std::to_string(index)+"^_H")throw std::runtime_error("mask binding");
            shape.pictures[i]=std::move(picture);
        }
        g.shapes.push_back(std::move(shape));
    }
    return g;
}
struct Transform {float x=0,y=0,sx=1,sy=1;};
Transform Then(Transform a,const MapPane& p) {
    if(p.origin!=0 || p.rotation_x!=0 || p.rotation_y!=0 || p.rotation_z!=0 || p.z!=0 ||
       p.scale_x<=0 || p.scale_y<=0)throw std::runtime_error("unsupported native transform");
    return {a.x+p.x*a.sx,a.y+p.y*a.sy,a.sx*p.scale_x,a.sy*p.scale_y};
}
Transform World(const std::vector<MapPane>& panes,int id) {
    if(id<0)return {};
    std::vector<int> chain;
    for(int i=id;i>=0;) {
        if(size_t(i)>=panes.size() || chain.size()>32)throw std::runtime_error("ancestry");
        chain.push_back(i);i=panes[i].parent;
    }
    Transform t;for(auto it=chain.rbegin();it!=chain.rend();++it)t=Then(t,panes[*it]);return t;
}
int Find(const std::vector<MapPane>& panes,std::string_view name) {
    for(size_t i=0;i<panes.size();++i)if(panes[i].name==name)return int(i);
    throw std::runtime_error("missing authored pane");
}
MapBounds Quad(const MapPane& pane,Transform t) {
    return {t.x-pane.width*t.sx/2,t.y+pane.height*t.sy/2,pane.width*t.sx,pane.height*t.sy};
}
struct SourceReader {
    const ReadAssetFn& fn;std::string_view path;size_t size{},used{};
    explicit SourceReader(const ReadAssetFn& r,std::string_view p,size_t maximum):fn(r),path(p) {
        size=fn?fn(path,0,nullptr,0):0;if(!size || size>maximum)throw std::runtime_error("source size");
    }
    std::vector<uint8_t> Read(uint64_t at,size_t n) {
        if(at>size || n>size-at || n>MaxMetadata-used)throw std::runtime_error("metadata budget");
        std::vector<uint8_t>b(n);used+=n;if(fn(path,at,b.data(),n)!=n)throw std::runtime_error("short read");return b;
    }
};
std::vector<MapTextureInfo> TextureIndex(const ReadAssetFn& fn) {
    SourceReader r(fn,"region_common/ui/Field.arc#timg/__Combined.bntx",MaxSource);
    const auto header=r.Read(0,64);const Bytes h{header};
    if(!h.Tag("BNTX") || h.U16(12)!=0xfeff || !h.Sub(32,4).Tag("NX  "))throw std::runtime_error("BNTX header");
    const size_t n=h.U32(36);if(!n || n>512)throw std::runtime_error("BNTX count");
    const auto pointers=r.Read(h.U64(40),n*8);const Bytes ptr{pointers};
    std::vector<MapTextureInfo> out;std::set<std::string> names;
    for(size_t i=0;i<n;++i) {
        const auto record=r.Read(ptr.U64(i*8),0xa0);const Bytes b{record};
        if(!b.Tag("BRTI"))throw std::runtime_error("texture header");
        const auto name_length=r.Read(b.U64(0x60),2);const size_t length=Bytes{name_length}.U16(0);
        if(!length || length>255)throw std::runtime_error("texture name size");
        auto name=r.Read(b.U64(0x60)+2,length+1);const auto s=Bytes{name}.Str(0,length+1);
        MapTextureInfo info{s,b.U32(0x24),b.U32(0x28),b.U32(0x1c),b.U32(0x58),b.U32(0x34),b.U32(0x50),b.U16(0x12),0};
        if(!info.width || !info.height || info.width>8192 || info.height>8192 || !names.insert(s).second)
            throw std::runtime_error("texture dimensions");
        const auto mip=r.Read(b.U64(0x70),8);const uint64_t data=Bytes{mip}.U64(0);
        if(data>r.size || b.U32(0x50)>r.size-data)throw std::runtime_error("texture data range");
        info.data_offset=data;
        out.push_back(std::move(info));
    }
    return out;
}
} // namespace
bool ParseFieldMapLayouts(std::span<const uint8_t> field,std::span<const uint8_t> traverse,FieldMapGeometry& out) {
    out={};try {out=Geometry({field},{traverse});return true;}catch(...){out={};return false;}
}
struct FieldMapAssets::Impl {
    explicit Impl(ReadAssetFn fn,void* user,EdenDsmodAstcDecoder decoder)
        :decode_user(user),decode(decoder),read(std::move(fn)){}
    void* decode_user{};
    EdenDsmodAstcDecoder decode{};
    std::vector<uint8_t> source;
    std::map<std::string,Image,std::less<>> tiles;
    size_t tile_bytes{};
    std::vector<std::string> tile_order;
    struct TileMemo {const Image* image{};int x{},y{};};
    std::map<const MapTextureInfo*,TileMemo> tile_memo;
    const MapTextureInfo* last_info{};const Image* last_tile{};int last_x{},last_y{};
    Image rendered;std::array<uint8_t,64> last_bits{};bool rendered_valid=false,last_extensions_hidden=false,last_sandstorm_hidden=false;

    bool Source() {
        if(!source.empty())return true;
        SourceReader r(read,"region_common/ui/Field.arc#timg/__Combined.bntx",MaxSource);
        std::vector<uint8_t> bytes(r.size);
        if(read(r.path,0,bytes.data(),bytes.size())!=bytes.size())return false;
        source=std::move(bytes);return true;
    }
    const Image* Tile(const MapTextureInfo& info,int x,int y,int& start_x,int& start_y) {
        // The host limits individual ASTC calls to 2048 per side. Keeping tiles at1024
        // also bounds temporary linear-block/output allocations independently of atlas size.
        int bw=0,bh=0;
        if(info.format==0x3806){bw=10;bh=10;}
        else if(info.format==0x3106){bw=6;bh=6;}
        else return nullptr;
        if(!decode || info.selectors!=0x05040302 || info.tile_mode>1 || (info.layout&7)>5 ||
           x<0 || y<0 || uint32_t(x)>=info.width || uint32_t(y)>=info.height)return nullptr;
        const int tile_w=(1024/bw)*bw,tile_h=(1024/bh)*bh;
        if(last_info==&info && last_tile && x>=last_x && y>=last_y &&
           x<last_x+last_tile->w && y<last_y+last_tile->h) {
            start_x=last_x;start_y=last_y;return last_tile;
        }
        if(auto it=tile_memo.find(&info);it!=tile_memo.end()) {
            const auto& m=it->second;
            if(x>=m.x && y>=m.y && x<m.x+m.image->w && y<m.y+m.image->h) {
                start_x=m.x;start_y=m.y;last_info=&info;last_tile=m.image;last_x=m.x;last_y=m.y;return m.image;
            }
        }
        start_x=x/tile_w*tile_w;start_y=y/tile_h*tile_h;
        const std::string key=info.name+"/"+std::to_string(start_x)+"/"+std::to_string(start_y);
        if(auto it=tiles.find(key);it!=tiles.end()) {
            last_info=&info;last_tile=&it->second;last_x=start_x;last_y=start_y;
            tile_memo[&info]={last_tile,start_x,start_y};return last_tile;
        }
        if(!Source())return nullptr;
        const int w=std::min<int>(tile_w,int(info.width)-start_x),h=std::min<int>(tile_h,int(info.height)-start_y);
        const int cols=(w+bw-1)/bw,rows=(h+bh-1)/bh;
        std::vector<uint8_t> linear(size_t(cols)*rows*16);
        const uint64_t block_height=uint64_t(1)<<(info.layout&7);
        const uint64_t native_cols=(uint64_t(info.width)+bw-1)/bw;
        const uint64_t gobs_x=(native_cols*16+63)/64;
        for(int by=0;by<rows;++by)for(int bx=0;bx<cols;++bx) {
            const uint64_t block_x=uint64_t(start_x/bw+bx),block_y=uint64_t(start_y/bh+by),byte_x=block_x*16;
            const uint64_t address=info.tile_mode==1?(block_y*native_cols+block_x)*16:
                (block_y/(8*block_height))*512*block_height*gobs_x+(byte_x/64)*512*block_height+
                ((block_y%(8*block_height))/8)*512+((byte_x%64)/32)*256+
                ((block_y%8)/2)*64+((byte_x%32)/16)*32+(block_y%2)*16+byte_x%16;
            if(address>info.data_size || 16>info.data_size-address ||
               info.data_offset>source.size() || address+16>source.size()-info.data_offset)return nullptr;
            std::memcpy(linear.data()+(size_t(by)*cols+bx)*16,source.data()+info.data_offset+address,16);
        }
        Image image(w,h);
        if(!decode(decode_user,uint32_t(w),uint32_t(h),uint32_t(bw),uint32_t(bh),
                   linear.data(),linear.size(),image.rgba.data(),image.rgba.size()))return nullptr;
        constexpr size_t MaxTiles=58u<<20;
        last_info=nullptr;last_tile=nullptr;tile_memo.clear();
        while(tile_bytes+image.rgba.size()>MaxTiles && !tile_order.empty()) {
            const auto it=tiles.find(tile_order.front());
            if(it!=tiles.end()){tile_bytes-=it->second.rgba.size();tiles.erase(it);}
            tile_order.erase(tile_order.begin());
        }
        if(tile_bytes+image.rgba.size()>MaxTiles)return nullptr;
        tile_bytes+=image.rgba.size();tile_order.push_back(key);
        last_info=&info;last_tile=&tiles.emplace(key,std::move(image)).first->second;
        last_x=start_x;last_y=start_y;tile_memo[&info]={last_tile,start_x,start_y};return last_tile;
    }
    bool Pixel(const MapTextureInfo& info,int x,int y,std::array<uint8_t,4>& out) {
        x=std::clamp(x,0,int(info.width)-1);y=std::clamp(y,0,int(info.height)-1);
        int sx=0,sy=0;const auto* tile=Tile(info,x,y,sx,sy);if(!tile)return false;
        std::memcpy(out.data(),tile->At(x-sx,y-sy),4);return true;
    }
    const MapTextureInfo& Texture(std::string_view name) const {
        for(const auto& info:geometry.textures)if(info.name==name)return info;
        throw std::runtime_error("texture binding");
    }
    static std::pair<float,float> Uv(const std::array<float,8>& uv,float x,float y) {
        const float u=(1-y)*((1-x)*uv[0]+x*uv[2])+y*((1-x)*uv[4]+x*uv[6]);
        const float v=(1-y)*((1-x)*uv[1]+x*uv[3])+y*((1-x)*uv[5]+x*uv[7]);
        return {u,v};
    }
    bool MaskInterior(const MapTextureInfo& info,float u,float v,bool any=false) {
        const float x=u*info.width-.5f,y=v*info.height-.5f;
        const int ix=int(std::floor(x)),iy=int(std::floor(y));
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
            std::array<uint8_t,4> pixel{};
            if(!Pixel(info,ix+dx,iy+dy,pixel))throw std::runtime_error("mask tile decode");
            if(any && pixel[0]!=0)return true;
            if(!any && pixel[0]!=255)return false;
        }
        return !any;
    }
    std::array<float,4> Sample(const MapTextureInfo& info,float u,float v) {
        const float x=u*info.width-.5f,y=v*info.height-.5f;
        const int ix=int(std::floor(x)),iy=int(std::floor(y));
        const float fx=x-ix,fy=y-iy;std::array<float,4> result{};
        static const auto linear=[] {
            std::array<float,256> lut{};
            for(int i=0;i<256;++i) {
                const float a=float(i)/255;
                lut[size_t(i)]=a<=.04045f?a/12.92f:std::pow((a+.055f)/1.055f,2.4f);
            }
            return lut;
        }();
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
            std::array<uint8_t,4> pixel{};
            if(!Pixel(info,ix+dx,iy+dy,pixel))throw std::runtime_error("terrain tile decode");
            const float weight=(dx?fx:1-fx)*(dy?fy:1-fy);
            for(int c=0;c<3;++c)result[c]+=linear[pixel[c]]*weight;
            result[3]+=float(pixel[3])/255*weight;
        }
        return result;
    }
    MapBounds NativeBounds() const {
        const auto& p=geometry.terrain.front().pane;
        const int id=Find(geometry.field_panes,p.name);
        return Quad(p,World(geometry.field_panes,id));
    }
    Transform ShapeTransform(const TraverseShape& shape,size_t picture=1) const {
        auto a=World(geometry.field_panes,Find(geometry.field_panes,"L_Attach_00"));
        const auto b=World(geometry.attach_panes,Find(geometry.attach_panes,"N_Traverse_00"));
        a={a.x+b.x*a.sx,a.y+b.y*a.sy,a.sx*b.sx,a.sy*b.sy};
        const auto c=World(geometry.traverse_panes,Find(geometry.traverse_panes,shape.pane.name));
        a={a.x+c.x*a.sx,a.y+c.y*a.sy,a.sx*c.sx,a.sy*c.sy};
        const auto& pic=shape.pictures[picture];
        const int id=Find(geometry.part_panes,pic.pane.name);
        const auto d=World(geometry.part_panes,geometry.part_panes[id].parent);
        a={a.x+d.x*a.sx,a.y+d.y*a.sy,a.sx*d.sx,a.sy*d.sy};
        return Then(a,pic.pane);
    }
    void ValidateRasterSubset() const {
        if(geometry.terrain.size()!=2 || geometry.excluded_quads.empty())throw std::runtime_error("native layers");
        const auto first=NativeBounds();
        if(first.width<1 || first.height<1 || first.width>2048 || first.height>2048 ||
           uint64_t(std::ceil(first.width))*uint64_t(std::ceil(first.height))>1240u*1080u)
            throw std::runtime_error("map output budget");
        for(const auto& p:geometry.terrain) {
            const auto bounds=Quad(p.pane,World(geometry.field_panes,Find(geometry.field_panes,p.pane.name)));
            if(std::abs(bounds.left-first.left)>.0001f || std::abs(bounds.top-first.top)>.0001f ||
               std::abs(bounds.width-first.width)>.0001f || std::abs(bounds.height-first.height)>.0001f ||
               p.material.textures.size()!=1 || p.uv.size()!=1 || p.material.flags!=0x15 ||
               p.material.black!=std::array<uint8_t,4>{0,0,0,0} ||
               p.material.white!=std::array<uint8_t,4>{255,255,255,255})throw std::runtime_error("terrain material subset");
            for(const auto& color:p.vertex_colors)if(color!=std::array<uint8_t,4>{255,255,255,255})
                throw std::runtime_error("terrain vertex color");
            const auto& record=p.material.native_record;Bytes r{record};
            if(r.F32(48)!=0 || r.F32(52)!=0 || r.F32(56)!=0 || r.F32(60)!=1 || r.F32(64)!=1)
                throw std::runtime_error("terrain texture transform");
        }
        for(const auto& shape:geometry.shapes) {
            const auto& baseline=shape.pictures[1];
            const auto expected=Quad(baseline.pane,ShapeTransform(shape));
            for(size_t i=0;i<2;++i) {
                const auto& p=shape.pictures[i];const auto& record=p.material.native_record;
                const auto& original=geometry.shapes.front().pictures[i].material.native_record;
                if(p.uv.empty() || p.material.flags!=0x800aa || p.pane.width<=0 || p.pane.height<=0 ||
                   record.size()!=original.size() || record.size()<192 ||
                   !std::equal(record.begin()+28,record.begin()+44,original.begin()+28) ||
                   !std::equal(record.begin()+52,record.end(),original.begin()+52))
                    throw std::runtime_error("traversal material subset");
                for(const auto& color:p.vertex_colors)if(color!=std::array<uint8_t,4>{255,255,255,255})
                    throw std::runtime_error("traversal vertex color");
                // Both native pictures retain original texture-coordinate selectors.
                // Grid00 uses its full UV1 mask; Grid01 has only UV0.
                const auto q=Quad(p.pane,ShapeTransform(shape,i));
                if(q.left!=expected.left || q.top!=expected.top || q.width!=expected.width ||
                   q.height!=expected.height)
                    throw std::runtime_error("different traversal picture coverage");
                const size_t source=record[109];
                if(source>=p.uv.size() || (i==1 && source!=1))throw std::runtime_error("native mask UV source");
                for(float v:p.uv[source])if(v<0 || v>1)throw std::runtime_error("mask UV range");
            }
        }
    }
    void Raster(std::span<const uint8_t,64> discovered,Image& out,MapBounds& bounds,bool field_extensions_hidden,bool sandstorm_hidden) {
        ValidateRasterSubset();bounds=NativeBounds();
        std::array<bool,512> authored{};for(const auto& s:geometry.shapes)authored[s.index]=true;
        for(size_t i=0;i<512;++i)if((discovered[i/8]>>(i%8)&1) && !authored[i])
            throw std::runtime_error("unsupported discovery index");
        if(rendered_valid && last_extensions_hidden==field_extensions_hidden && last_sandstorm_hidden==sandstorm_hidden && std::equal(discovered.begin(),discovered.end(),last_bits.begin())) {out=rendered;return;}
        if(!decode)throw std::runtime_error("missing ASTC decoder");
        const int w=int(std::ceil(bounds.width)),h=int(std::ceil(bounds.height));
        std::vector<uint8_t> coverage(size_t(w)*h,0);
        auto visit=[&](MapBounds q,int margin,auto&& fn) {
            const int left=std::max(0,int(std::floor((q.left-bounds.left)/bounds.width*w))-margin);
            const int right=std::min(w,int(std::ceil((q.left+q.width-bounds.left)/bounds.width*w))+margin);
            const int top=std::max(0,int(std::floor((bounds.top-q.top)/bounds.height*h))-margin);
            const int bottom=std::min(h,int(std::ceil((bounds.top-q.top+q.height)/bounds.height*h))+margin);
            for(int y=top;y<bottom;++y)for(int x=left;x<right;++x)fn(x,y);
        };
        for(const auto& shape:geometry.shapes)if(discovered[shape.index/8]>>(shape.index%8)&1) {
            const auto& pic=shape.pictures[1];const auto q=Quad(pic.pane,ShapeTransform(shape));
            const auto& mask=Texture(pic.material.textures[1]);
            visit(q,0,[&](int x,int y) {
                const float wx=bounds.left+(x+.5f)/w*bounds.width,wy=bounds.top-(y+.5f)/h*bounds.height;
                const float fx=(wx-q.left)/q.width,fy=(q.top-wy)/q.height;
                if(fx<0 || fx>1 || fy<0 || fy>1)return;
                const auto [u,v]=Uv(pic.uv[1],fx,fy);
                if(MaskInterior(mask,u,v))coverage[size_t(y)*w+x]=1;
            });
        }
        // Cover every nonzero unknown fog contributor from either original
        // picture, even the normally transparent highlight picture. Each retains
        // its own original texture-coordinate source and an opaque outer margin.
        for(const auto& shape:geometry.shapes)if(!(discovered[shape.index/8]>>(shape.index%8)&1)) {
            for(size_t i=0;i<2;++i) {
                const auto& pic=shape.pictures[i];const auto q=Quad(pic.pane,ShapeTransform(shape,i));
                const auto& mask=Texture(pic.material.textures[1]);
                const auto& uv=pic.uv[pic.material.native_record[109]];
                visit(q,4,[&](int x,int y){
                    if(!coverage[size_t(y)*w+x])return;
                    const float wx=bounds.left+(x+.5f)/w*bounds.width,wy=bounds.top-(y+.5f)/h*bounds.height;
                    const float fx=(wx-q.left)/q.width,fy=(q.top-wy)/q.height;
                    if(fx<0 || fx>1 || fy<0 || fy>1){coverage[size_t(y)*w+x]=0;return;}
                    const auto [u,v]=Uv(uv,fx,fy);
                    if(MaskInterior(mask,u,v,true))coverage[size_t(y)*w+x]=0;
                });
            }
        }
        const auto excluded=[&](size_t index) {
            if(field_extensions_hidden && std::find(geometry.field_extension_quads.begin(),geometry.field_extension_quads.end(),index)!=geometry.field_extension_quads.end())return false;
            if(sandstorm_hidden && std::find(geometry.sandstorm_quads.begin(),geometry.sandstorm_quads.end(),index)!=geometry.sandstorm_quads.end())return false;
            return true;
        };
        for(size_t i=0;i<geometry.excluded_quads.size();++i)if(excluded(i))visit(geometry.excluded_quads[i],4,[&](int x,int y){coverage[size_t(y)*w+x]=0;});
        const auto original=coverage;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(coverage[size_t(y)*w+x]) {
            if(x<2 || y<2 || x>=w-2 || y>=h-2){coverage[size_t(y)*w+x]=0;continue;}
            for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)
                if(!original[size_t(y+dy)*w+x+dx])coverage[size_t(y)*w+x]=0;
        }
        Image image(w,h);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(coverage[size_t(y)*w+x]) {
            std::array<float,4> pixel{};
            for(const auto& layer:geometry.terrain) {
                const auto [u,v]=Uv(layer.uv[0],(x+.5f)/w,(y+.5f)/h);
                const auto src=Sample(Texture(layer.material.textures[0]),u,v);
                const float alpha=src[3]+pixel[3]*(1-src[3]);
                for(int c=0;c<3;++c)pixel[c]=alpha>0?(src[c]*src[3]+pixel[c]*pixel[3]*(1-src[3]))/alpha:0;
                pixel[3]=alpha;
            }
            auto* dst=image.At(x,y);
            for(int c=0;c<3;++c) {
                const float a=std::clamp(pixel[c],0.f,1.f);
                const float encoded=a<=.0031308f?a*12.92f:1.055f*std::pow(a,1.f/2.4f)-.055f;
                dst[c]=uint8_t(std::clamp(std::lround(encoded*255),0l,255l));
            }
            dst[3]=uint8_t(std::clamp(std::lround(pixel[3]*255),0l,255l));
        }
        // Apply the exclusions again after all image filtering and compositing.
        for(size_t i=0;i<geometry.excluded_quads.size();++i)if(excluded(i))visit(geometry.excluded_quads[i],4,[&](int x,int y){std::memset(image.At(x,y),0,4);});
        std::copy(discovered.begin(),discovered.end(),last_bits.begin());rendered_valid=true;
        last_extensions_hidden=field_extensions_hidden;last_sandstorm_hidden=sandstorm_hidden;
        rendered=image;out=std::move(image);
    }
    ReadAssetFn read;FieldMapGeometry geometry;bool ready=false;
    bool Inspect(FieldMapGeometry& out) {
        if(ready){out=geometry;return true;}
        SourceReader f(read,"region_common/ui/Field.arc#blyt/Field.bflyt",MaxLayout);
        SourceReader t(read,"region_common/ui/Field.arc#blyt/Field_Traverse.bflyt",MaxLayout);
        const auto fb=f.Read(0,f.size),tb=t.Read(0,t.size);
        auto g=Geometry({fb},{tb});
        SourceReader p(read,"region_common/ui/Field.arc#blyt/L_MapTraverseItem.bflyt",MaxLayout);
        const auto pb=p.Read(0,p.size);
        g.part_panes=Parse({pb}).panes;
        auto load_layout=[&](std::string_view name) {
            if(name.empty() || name.size()>100 || !std::all_of(name.begin(),name.end(),[](char c){
                return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
            }))throw std::runtime_error("part source name");
            const std::string path="region_common/ui/Field.arc#blyt/"+std::string(name)+".bflyt";
            SourceReader r(read,path,MaxLayout);return r.Read(0,r.size);
        };
        const auto attach_bytes=load_layout("L_MapAttachPanes");g.attach_panes=Parse({attach_bytes}).panes;
        const auto field=Parse({fb});
        for(const auto role:{"L_Extend_00","L_Extend_01","L_Extend_02","L_SandStorm_00"}) {
            const int id=Find(g.field_panes,role);const auto& part=g.field_panes[id];
            const auto child_bytes=load_layout(part.part_layout);auto child=Parse({child_bytes});
            const auto parent_world=World(g.field_panes,id);
            std::map<std::string,MapPane,std::less<>> overrides;
            for(const auto& [part_id,s]:field.parts)if(part_id==id) {
                const size_t n=s.U32(0x54);
                for(size_t i=0;i<n;++i) {
                    const size_t prop=0x60+40*i;const auto name=s.Str(prop,24);
                    if(name.empty())continue;
                    const size_t data=s.U32(prop+28);s.Fits(data,8);
                    auto pic=Picture(s.Sub(data,s.U32(data+4)),field,-1);
                    if(pic.pane.name!=name)throw std::runtime_error("extension override identity");
                    overrides.emplace(name,std::move(pic.pane));
                }
            }
            for(auto& pane:child.panes)if(auto it=overrides.find(pane.name);it!=overrides.end()) {
                const int parent=pane.parent;pane=it->second;pane.parent=parent;
            }
            bool found=false;MapBounds merged{};
            if(!child.parts.empty())throw std::runtime_error("nested extension layout");
            for(const auto& [pic_id,section]:child.pictures) {
                (void)section;const auto& pane=child.panes[pic_id];
                const auto local=World(child.panes,pic_id);
                const Transform t{parent_world.x+local.x*parent_world.sx,parent_world.y+local.y*parent_world.sy,
                                  parent_world.sx*local.sx,parent_world.sy*local.sy};
                const auto q=Quad(pane,t);
                if(!found)merged=q;
                else {
                    const float left=std::min(merged.left,q.left),top=std::max(merged.top,q.top);
                    const float right=std::max(merged.left+merged.width,q.left+q.width);
                    const float bottom=std::min(merged.top-merged.height,q.top-q.height);
                    merged={left,top,right-left,top-bottom};
                }
                found=true;
            }
            if(!found)throw std::runtime_error("extension quad missing");
            if(std::string_view(role).starts_with("L_Extend_"))g.field_extension_quads.push_back(g.excluded_quads.size());
            else g.sandstorm_quads.push_back(g.excluded_quads.size());
            g.excluded_quads.push_back(merged);
        }
        g.textures=TextureIndex(read);
        std::set<std::string> names;for(const auto& tex:g.textures)names.insert(tex.name);
        for(const auto& shape:g.shapes)for(const auto& pic:shape.pictures)for(const auto& tex:pic.material.textures)
            if(!names.contains(tex))throw std::runtime_error("missing native texture");
        for(const auto& pic:g.terrain)for(const auto& tex:pic.material.textures)
            if(!names.contains(tex))throw std::runtime_error("missing terrain texture");
        geometry=std::move(g);ready=true;out=geometry;return true;
    }
};
FieldMapAssets::FieldMapAssets(ReadAssetFn read,void* user,EdenDsmodAstcDecoder decode)
    :impl_(std::make_unique<Impl>(std::move(read),user,decode)){}
FieldMapAssets::~FieldMapAssets()=default;
bool FieldMapAssets::Inspect(FieldMapGeometry& out) {
    out={};try{return impl_->Inspect(out);}
    catch(const std::exception& e){
#ifdef ECHOES_TEST_DIAGNOSTICS
        std::fprintf(stderr,"Field metadata: %s\n",e.what());
#else
        (void)e;
#endif
        out={};return false;
    }catch(...){out={};return false;}
}
bool FieldMapAssets::Compose(std::span<const uint8_t,64> discovered,Image& out,MapBounds& bounds,MapComposeStatus* status,bool field_extensions_hidden,bool sandstorm_hidden) {
    out={};bounds={};FieldMapGeometry geometry;
    if(!Inspect(geometry)) {
        if(status)*status=MapComposeStatus::MissingOrInvalidSource;
        return false;
    }
    try {
        impl_->Raster(discovered,out,bounds,field_extensions_hidden,sandstorm_hidden);
        if(status)*status=MapComposeStatus::Ready;
        return true;
    }catch(const std::exception& e) {
#ifdef ECHOES_TEST_DIAGNOSTICS
        std::fprintf(stderr,"Field composition: %s\n",e.what());
#else
        (void)e;
#endif
        out={};bounds={};if(status)*status=MapComposeStatus::UnsupportedDecoderOrDiscovery;return false;
    }catch(...) {out={};bounds={};if(status)*status=MapComposeStatus::UnsupportedMaterialAndStoryVisibility;return false;}
}
} // namespace echoes

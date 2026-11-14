// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_geometry.h"
#include "re4_map_art_tables.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <stdexcept>
namespace re4 {
namespace {
using Bytes=std::span<const uint8_t>;
uint64_t Word(Bytes b,size_t p,unsigned n){
    if(p>b.size()||n>b.size()-p)throw std::runtime_error("hit mesh bounds");
    uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[p+i])<<(8*i);return v;
}
double Float(Bytes b,size_t p){
    const double v=std::bit_cast<float>(uint32_t(Word(b,p,4)));
    if(!std::isfinite(v)||std::abs(v)>2000000)throw std::runtime_error("hit mesh coordinate");return v;
}
struct Entry{uint32_t tag;Bytes bytes;};
std::vector<Entry> Container(Bytes b){
    const auto n=Word(b,0,4);if(!n||n>512||16+n*8>b.size())throw std::runtime_error("hit mesh container");
    std::vector<Entry> entries;
    for(size_t i=0;i<n;++i){const size_t at=Word(b,16+i*4,4),end=i+1<n?Word(b,20+i*4,4):b.size();
        if(at<16+n*8||end<at||end>b.size())throw std::runtime_error("hit mesh entry");
        entries.push_back({uint32_t(Word(b,16+n*4+i*4,4)),b.subspan(at,end-at)});
    }return entries;
}
struct Sat {std::vector<std::array<double,3>> vertices;std::vector<MapTriangle> triangles;};
Sat ParseSat(Bytes b){
    const size_t base=Word(b,4,4),count=Word(b,base+2,2);
    const size_t normals=Word(b,base+4,2),edges=Word(b,base+6,2),polys=Word(b,base+10,2);
    const size_t floors=Word(b,base+12,2)+Word(b,base+14,2);
    if(!count||count>4096||floors>8192||polys>16384||floors>polys)throw std::runtime_error("hit mesh counts");
    size_t at=base+20;Sat result;result.vertices.reserve(count);
    for(size_t i=0;i<count;++i,at+=12)result.vertices.push_back({Float(b,at),Float(b,at+4),Float(b,at+8)});
    at+=(normals+edges)*12;
    if(at>b.size()||polys>(b.size()-at)/20)throw std::runtime_error("hit mesh polygons");
    for(size_t i=0;i<floors;++i,at+=20){MapTriangle t{uint16_t(Word(b,at,2)),uint16_t(Word(b,at+2,2)),uint16_t(Word(b,at+4,2))};
        if(std::max({t.a,t.b,t.c})>=count)throw std::runtime_error("hit mesh index");result.triangles.push_back(t);
    }return result;
}
MapMesh ParseRoom(Bytes b,const assets::MapArtRoom& row){
    auto entries=Container(b);std::vector<Bytes> models,sats;
    for(const auto& e:entries){if(e.tag==0x004e4942)models.push_back(e.bytes);if(e.tag==0x0054484d)sats.push_back(e.bytes);}
    if(models.empty()||sats.size()!=2)throw std::runtime_error("hit mesh pair");
    const auto model=models[0];const size_t head=Word(model,0,8);
    if(Word(model,0x29,1)!=1||Word(model,head,1)!=0||Word(model,head+1,1)!=255)throw std::runtime_error("hit mesh root");
    const double tx=Float(model,head+4),ty=Float(model,head+8),tz=Float(model,head+12);
    const auto local=ParseSat(sats[0]),world=ParseSat(sats[1]);
    if(world.triangles.empty())throw std::runtime_error("hit mesh correspondence");
    MapMesh result;result.area=row.key;std::replace(result.area.begin(),result.area.end(),'/','-');
    const unsigned room=std::stoul(result.area.substr(result.area.find('-')+1),nullptr,16);
    result.narrow=models.size()>1||room==0x107||room==0x10f||room==0x217||room==0x320;
    switch(room){case 0x10d:result.heading=-40;break;case 0x117:case 0x503:result.heading=-70;break;
    case 0x222:case 0x226:case 0x227:case 0x228:case 0x22a:case 0x21a:case 0x21b:result.heading=-90;break;}
    result.affine=result.area.starts_with("stage5-");
    for(size_t i=0;i<std::min(local.vertices.size(),world.vertices.size());++i)result.affine=result.affine&&
        std::abs(local.vertices[i][0]-world.vertices[i][0]/10)<1&&std::abs(local.vertices[i][2]-world.vertices[i][2]/10)<1;
    result.ox=row.scale*tx;result.oz=row.scale*tz;
    size_t count=0;for(const auto& t:world.triangles)count=std::max(count,size_t(std::max({t.a,t.b,t.c}))+1);
    if(count>local.vertices.size()||count>world.vertices.size())throw std::runtime_error("hit mesh correspondence");
    result.vertices.reserve(count);
    for(size_t i=0;i<count;++i){const auto& w=world.vertices[i];const auto& l=local.vertices[i];
        result.vertices.push_back({w[0],w[1],w[2],row.scale*(l[0]+tx),-row.scale*(l[2]+tz),row.scale*(l[1]+ty)});
    }
    result.triangles=world.triangles;return result;
}
}
bool GeometryCache::Warm(const EdenDsmodHostApi* host,unsigned stage,const std::string& directory){
    if(stage!=1&&stage!=2&&stage!=3&&stage!=5)return false;
    std::lock_guard lock(worker);if(Get(stage))return true;
    try{
        auto result=std::make_shared<GeometryStage>();result->stage=stage;
        size_t room_count=0;
        for(const auto& row:assets::MapArtRooms){const std::string_view key=row.key;
            const unsigned s=key.starts_with("village/")?1:key.starts_with("castle/")?2:key.starts_with("island/")?3:key.starts_with("stage5/")?5:0;
            room_count+=s==stage;
        }
        result->meshes.reserve(room_count);
        std::string filename;std::vector<uint8_t> data;std::vector<Entry> entries;
        for(const auto& row:assets::MapArtRooms){
            const std::string_view key=row.key;
            const unsigned s=key.starts_with("village/")?1:key.starts_with("castle/")?2:key.starts_with("island/")?3:key.starts_with("stage5/")?5:0;
            if(s!=stage)continue;
            if(filename!=row.file){filename=row.file;
                const std::string path="bio4/ss/cmn/"+filename;
                const size_t size=directory.empty()?(host&&host->read_romfs?host->read_romfs(host->userdata,path.c_str(),0,nullptr,0):0):
                    std::filesystem::file_size(directory+"/"+path);
                if(!size||size>2u<<20)return false;
                if(!assets::Read(host,path,data,directory))return false;
                if(data.size()>2u<<20)return false;
                entries=Container(data);
            }
            if(row.entry>=entries.size())return false;
            auto mesh=ParseRoom(entries[row.entry].bytes,row);
            result->bytes+=mesh.vertices.capacity()*sizeof(MapVertex)+mesh.triangles.capacity()*sizeof(MapTriangle)+sizeof(MapMesh)+mesh.area.capacity();
            if(result->bytes>Limit)return false;result->meshes.push_back(std::move(mesh));
        }
        if(result->meshes.empty())return false;
        std::shared_ptr<const GeometryStage> published=std::move(result);
        std::atomic_store_explicit(&ready,std::move(published),std::memory_order_release);return true;
    }catch(...){return false;}
}
}

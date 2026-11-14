// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_assets.h"
#include "re4_map_assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <zlib.h>
#include "bc_decoder.h"
namespace re4::assets {
bool UiImage(const EdenDsmodHostApi*,std::string_view,Image&,const std::string&);
bool InventoryImage(const EdenDsmodHostApi*,std::string_view,Image&,const std::string&);
namespace {
thread_local Cache* active_cache=nullptr;
thread_local ProfileScope* active_scope=nullptr;
thread_local std::string_view active_source;
thread_local std::span<const unsigned> batch_indices;
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
bool Allocate(Image& out,unsigned w,unsigned h){
    out={};if(!w||!h||w>8192||h>8192||uint64_t(w)*h*4>MaxBytes)return false;
    out={w,h,ByteBuffer(size_t(w)*h*4)};return true;
}
}
ProfileScope::ProfileScope(Phase p):profile(active_cache?active_cache->profile:nullptr),phase(p),parent(active_scope){
    if(profile){start=std::chrono::steady_clock::now();active_scope=this;}
}
ProfileScope::~ProfileScope(){
    if(profile){const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        profile->ms[size_t(phase)]+=ms-children;if(parent)parent->children+=ms;active_scope=parent;}
}
template<class Buffer> bool ReadInto(const EdenDsmodHostApi* host,std::string_view path,Buffer& out,const std::string& directory){
    ProfileScope timing(Phase::Read);
    if(active_cache&&active_cache->profile)++active_cache->profile->sources[std::string(path)].reads;
    out.clear();
    if(path.empty()||path.front()=='/'||path.find("..")!=path.npos||path.find('\\')!=path.npos)return false;
    const std::string name(path);
    if(host && host->read_romfs){
        const auto size=host->read_romfs(host->userdata,name.c_str(),0,nullptr,0);
        if(!size||size>MaxBytes)return false;
        out.resize(size);
        if(host->read_romfs(host->userdata,name.c_str(),0,out.data(),size)==size)return true;
    }else if(!directory.empty()){
        std::ifstream f(directory+"/"+name,std::ios::binary|std::ios::ate);
        const auto size=f.tellg();
        if(size<=0||size>MaxBytes)return false;
        out.resize(size);f.seekg(0);
        if(f.read(reinterpret_cast<char*>(out.data()),size))return true;
    }
    out.clear();return false;
}
template<class Buffer> bool InflateInto(std::span<const uint8_t> in,Buffer& out){
    ProfileScope timing(Phase::Inflate);
    if(active_cache&&active_cache->profile)++active_cache->profile->sources[std::string(active_source)].inflates;
    out.clear();if(in.size()<10||in.size()>MaxBytes)return false;
    const auto size=U32(in.data());if(!size||size>MaxBytes)return false;
    out.resize(size);z_stream z{};
    z.next_in=const_cast<Bytef*>(in.data()+4);z.avail_in=in.size()-4;
    z.next_out=out.data();z.avail_out=size;
    if(inflateInit(&z)!=Z_OK){out.clear();return false;}
    const int status=inflate(&z,Z_FINISH);
    // Some shipped packs have a trailer after the zlib stream; match the Python reader.
    const bool ok=status==Z_STREAM_END&&z.total_out==size;
    inflateEnd(&z);if(!ok)out.clear();return ok;
}
bool Read(const EdenDsmodHostApi* h,std::string_view p,std::vector<uint8_t>& out,const std::string& d){return ReadInto(h,p,out,d);}
bool Read(const EdenDsmodHostApi* h,std::string_view p,ByteBuffer& out,const std::string& d){return ReadInto(h,p,out,d);}
bool InflateFen(std::span<const uint8_t> in,std::vector<uint8_t>& out){return InflateInto(in,out);}
bool InflateFen(std::span<const uint8_t> in,ByteBuffer& out){return InflateInto(in,out);}
namespace {
bool DecodeRegion(std::span<const uint8_t> in,Image& out,int rx,int ry,unsigned rw,unsigned rh,bool region){
    ProfileScope timing(Phase::Decode);
    if(active_cache&&active_cache->profile)++active_cache->profile->sources[std::string(active_source)].decodes;
    out={};if(in.size()<128||std::memcmp(in.data(),"DDS ",4)||U32(in.data()+4)!=124||U32(in.data()+76)!=32)return false;
    const unsigned h=U32(in.data()+12),w=U32(in.data()+16);
    unsigned block=16,offset=128,format=0;
    const auto* cc=in.data()+84;
    if(!std::memcmp(cc,"DXT1",4)){format=1;block=8;}
    else if(!std::memcmp(cc,"DXT3",4))format=2;
    else if(!std::memcmp(cc,"DXT5",4))format=3;
    else if(!std::memcmp(cc,"DX10",4)){
        if(in.size()<148||U32(in.data()+132)!=3||U32(in.data()+136)!=0||U32(in.data()+140)!=1)return false;
        const auto dxgi=U32(in.data()+128);if(dxgi!=98&&dxgi!=99)return false;
        format=7;offset=148;
    }else return false;
    // Only 2D, single-face mip-zero images; reject cubemaps and volume textures.
    if(U32(in.data()+112)||U32(in.data()+24)>1||!w||!h||w>8192||h>8192)return false;
    const size_t bytes=size_t((w+3)/4)*((h+3)/4)*block;
    if(bytes>in.size()-offset)return false;
    if(!region){rx=ry=0;rw=w;rh=h;}
    if(!Allocate(out,rw,rh))return false;
    const int64_t x0=std::clamp<int64_t>(rx,0,w),y0=std::clamp<int64_t>(ry,0,h);
    const int64_t x1=std::clamp<int64_t>(int64_t(rx)+rw,0,w),y1=std::clamp<int64_t>(int64_t(ry)+rh,0,h);
    for(unsigned y=y0/4*4;y<y1;y+=4)for(unsigned x=x0/4*4;x<x1;x+=4){
        const auto* src=in.data()+offset+(size_t(y/4)*((w+3)/4)+x/4)*block;
        std::array<uint8_t,64> tile{};
        switch(format){
        case 1:bcn::DecodeBc1(src,tile.data(),0,0,4,4);break;
        case 2:bcn::DecodeBc2(src,tile.data(),0,0,4,4);break;
        case 3:bcn::DecodeBc3(src,tile.data(),0,0,4,4);break;
        case 7:
            if(!src[0]){out={};return false;} // No BC7 mode selected.
            bcn::DecodeBc7(src,tile.data(),0,0,4,4);break;
        }
        const unsigned left=std::max<unsigned>(x,x0),right=std::min<unsigned>(x+4,x1);
        for(unsigned row=std::max<unsigned>(y,y0);row<std::min<unsigned>(y+4,y1);++row)
            std::memcpy(out.pixels.data()+(size_t(int64_t(row)-ry)*rw+int64_t(left)-rx)*4,
                        tile.data()+(row-y)*16+(left-x)*4,(right-left)*4);
    }
    return true;
}
}
bool DecodeDds(std::span<const uint8_t> in,Image& out){return DecodeRegion(in,out,0,0,0,0,false);}
bool DecodeDdsRegion(std::span<const uint8_t> in,Image& out,int x,int y,unsigned w,unsigned h){return DecodeRegion(in,out,x,y,w,h,true);}
void Cache::Reset(){
    ui_materials.clear();ui_bytes=0;
    textures.clear();used.clear();sources.clear();source_used.clear();
    frame_geometry={};ByteBuffer().swap(frame_mask);
    ByteBuffer().swap(case_pack);case_offsets.clear();bytes=source_bytes=0;
}
void Cache::ReleaseIdle(){
    std::unique_lock lock(mutex,std::try_to_lock);
    if(lock.owns_lock()&&last.time_since_epoch().count()&&std::chrono::steady_clock::now()-last>std::chrono::seconds(2)){
        ui_materials.clear();ui_bytes=0;
    }
}
void Cache::Trim(size_t incoming){
    while(bytes+incoming>TextureLimit()&&!textures.empty()){
        auto oldest=std::min_element(textures.begin(),textures.end(),[&](const auto& a,const auto& b){const auto age=[&](const auto& key){const auto i=used.find(key);return i==used.end()?0:i->second;};return age(a.first)<age(b.first);});
        bytes-=oldest->second.pixels.size();used.erase(oldest->first);textures.erase(oldest);
    }
}
void Cache::TrimSources(size_t incoming){
    while(source_bytes+incoming>DdsLimit()&&!sources.empty()){
        auto oldest=std::min_element(source_used.begin(),source_used.end(),[](const auto& a,const auto& b){return a.second<b.second;});
        source_bytes-=sources.at(oldest->first).size();sources.erase(oldest->first);source_used.erase(oldest);
    }
}
void Cache::StoreSource(const std::string& key,std::span<const uint8_t> data){
    if(data.size()>DdsLimit()||sources.contains(key))return;
    TrimSources(data.size());
    source_used.emplace(key,++clock);
    try{sources.emplace(key,ByteBuffer(data.begin(),data.end()));}
    catch(...){source_used.erase(key);throw;}
    source_bytes+=data.size();
}
namespace {
bool TextureImpl(const EdenDsmodHostApi* host,const char* pack,unsigned index,Image& out,const std::string& directory,
                 int x=0,int y=0,unsigned w=0,unsigned h=0,bool region=false){
    struct SourceScope{std::string_view previous;SourceScope(const char* p):previous(active_source){active_source=p?p:"";}~SourceScope(){active_source=previous;}} source_scope(pack);
    out={};if(!pack||std::strlen(pack)!=8)return false;
    uint32_t pack_id=0;const auto parsed=std::from_chars(pack,pack+8,pack_id,16);
    if(parsed.ec!=std::errc{}||parsed.ptr!=pack+8)return false;
    const std::string source_key=std::string(pack)+"/"+std::to_string(index);
    const std::string cache_key=source_key+(region?"/"+std::to_string(x)+"/"+std::to_string(y)+"/"+std::to_string(w)+"/"+std::to_string(h):"");
    if(active_cache){
        if(const auto found=active_cache->textures.find(cache_key);found!=active_cache->textures.end()){
            active_cache->used[cache_key]=++active_cache->clock;out=found->second;return true;
        }
    }
    auto decode=[&](std::span<const uint8_t> dds){
        // Make room before allocating the new atlas, rather than briefly exceeding the LRU budget.
        if(active_cache&&dds.size()>=20){const uint64_t size=region?uint64_t(w)*h*4:uint64_t(U32(dds.data()+12))*U32(dds.data()+16)*4;
            active_cache->Trim(std::min<uint64_t>(size,active_cache->TextureLimit()));}
        return region?DecodeDdsRegion(dds,out,x,y,w,h):DecodeDds(dds,out);
    };
    bool decoded=false;
    if(active_cache){
        if(auto found=active_cache->sources.find(source_key);found!=active_cache->sources.end()){
            active_cache->source_used[source_key]=++active_cache->clock;decoded=decode(found->second);
        }
    }
    if(!decoded){
        ByteBuffer bytes,plain;std::span<const uint8_t> source;std::vector<size_t> offsets;
        const bool case_source=active_cache&&std::string_view(pack)=="3b000001";
        if(case_source&&!active_cache->case_pack.empty()){
            source=active_cache->case_pack;offsets=active_cache->case_offsets;
        }else{
            if(case_source){
                // Case's large container replaces the small DDS source working set.
                active_cache->sources.clear();active_cache->source_used.clear();active_cache->source_bytes=0;
            }
            if(!Read(host,std::string("bio4/imagepackhd/")+pack+".pack.fen",bytes,directory))return false;
            // Reserve the working set before inflating a large container. Retaining
            // all three small caches across a 26-28 MiB inflate exceeds the peak
            // budget even though each individual cache is bounded.
            if(active_cache&&bytes.size()>=4&&U32(bytes.data())>(16u<<20)){
                active_cache->ui_materials.clear();active_cache->ui_bytes=0;
                active_cache->textures.clear();active_cache->used.clear();active_cache->bytes=0;
                active_cache->sources.clear();active_cache->source_used.clear();active_cache->source_bytes=0;
                active_cache->frame_geometry={};ByteBuffer().swap(active_cache->frame_mask);
            }
            if(!InflateFen(bytes,plain)||plain.size()<32||U32(plain.data())!=pack_id)return false;
            ByteBuffer().swap(bytes);
            // The pack table has one u64 record offset per texture. Each record
            // starts with a 16-byte wrapper, followed by DDS. Never scan BC bytes
            // for magic: a valid compressed block can contain the same four bytes.
            const unsigned count=U32(plain.data()+4);
            if(!count||count>4096||8+size_t(count)*8>plain.size())return false;
            for(unsigned n=0;n<count;++n){
                const auto* entry=plain.data()+8+size_t(n)*8;
                const uint64_t at=uint64_t(U32(entry))+(uint64_t(U32(entry+4))<<32)+16;
                if(at<8+size_t(count)*8||at>plain.size()||plain.size()-at<128||
                   (!offsets.empty()&&at<=offsets.back())||std::memcmp(plain.data()+at,"DDS ",4))return false;
                offsets.push_back(at);
            }
            if(case_source&&plain.size()<=Cache::CaseLimit){active_cache->case_pack=std::move(plain);active_cache->case_offsets=offsets;source=active_cache->case_pack;}
            else source=plain;
        }
        if(index>=offsets.size())return false;
        auto dds=[&](unsigned n){const size_t end=n+1<offsets.size()?offsets[n+1]:source.size();return source.subspan(offsets[n],end-offsets[n]);};
        if(active_cache&&!case_source){
            // Small packs (map kit, digits, key icons) fit as encoded DDS sets.
            // Large packs retain only the requested batch, e.g. goal effects.
            if(source.size()<=active_cache->DdsLimit())for(unsigned n=0;n<offsets.size();++n)active_cache->StoreSource(std::string(pack)+"/"+std::to_string(n),dds(n));
            else for(unsigned n:batch_indices)if(n<offsets.size())active_cache->StoreSource(std::string(pack)+"/"+std::to_string(n),dds(n));
            active_cache->StoreSource(source_key,dds(index));
        }
        if(!decode(dds(index)))return false;
    }
    if(active_cache&&out.pixels.size()<=active_cache->TextureLimit()){
        active_cache->Trim(out.pixels.size());active_cache->used.emplace(cache_key,++active_cache->clock);
        try{active_cache->textures.emplace(cache_key,out);}
        catch(...){active_cache->used.erase(cache_key);throw;}
        active_cache->bytes+=out.pixels.size();
    }return true;
}
}
bool Texture(const EdenDsmodHostApi* host,const char* pack,unsigned index,Image& out,const std::string& directory){return TextureImpl(host,pack,index,out,directory);}
bool TextureRegion(const EdenDsmodHostApi* host,const char* pack,unsigned index,Image& out,int x,int y,unsigned w,unsigned h,const std::string& directory){return TextureImpl(host,pack,index,out,directory,x,y,w,h,true);}
bool Textures(const EdenDsmodHostApi* host,std::span<const unsigned> indices,std::vector<Image>& out,const std::string& directory,const char* pack){
    struct Batch {std::span<const unsigned> previous;Batch(std::span<const unsigned> s):previous(batch_indices){batch_indices=s;}~Batch(){batch_indices=previous;}} batch(indices);
    out.clear();size_t bytes=0;
    for(auto index:indices){Image im;if(!Texture(host,pack,index,im,directory)){out.clear();return false;}bytes+=im.pixels.size();if(bytes>(16u<<20)){out.clear();return false;}out.push_back(std::move(im));}
    return true;
}
bool HasCachedCaseImage(unsigned id){return active_cache&&active_cache->case_images.contains(id);}
bool HasUiMaterial(unsigned node){return active_cache&&active_cache->ui_materials.contains(node);}
void ClearUiMaterials(){
    if(!active_cache)return;
    active_cache->ui_materials.clear();active_cache->ui_bytes=0;
}
std::shared_ptr<const Image> UiMaterial(unsigned node){
    return HasUiMaterial(node)?active_cache->ui_materials.at(node):nullptr;
}
std::shared_ptr<const Image> StoreUiMaterial(unsigned node,Image&& image){
    if(!active_cache||!active_cache->ui_burst||image.pixels.size()>Cache::UiLimit)return nullptr;
    if(auto found=UiMaterial(node))return found;
    if(active_cache->ui_bytes+image.pixels.size()>Cache::UiLimit){active_cache->ui_materials.clear();active_cache->ui_bytes=0;}
    auto stored=std::make_shared<const Image>(std::move(image));
    active_cache->ui_materials.emplace(node,stored);active_cache->ui_bytes+=stored->pixels.size();return stored;
}
bool CachedCaseImage(unsigned id,Image& out){
    if(!active_cache)return false;
    const auto it=active_cache->case_images.find(id);if(it==active_cache->case_images.end())return false;
    ProfileScope timing(Phase::Inflate);
    if(!Allocate(out,it->second.w,it->second.h))return false;
    uLongf size=out.pixels.size();
    if(uncompress(out.pixels.data(),&size,it->second.encoded.data(),it->second.encoded.size())!=Z_OK||size!=out.pixels.size()){out={};return false;}
    return true;
}
bool StoreCaseImage(unsigned id,const Image& image){
    if(!active_cache||!image)return false;
    if(active_cache->case_images.contains(id))return true;
    ProfileScope timing(Phase::Compose);
    ByteBuffer encoded(compressBound(image.pixels.size()));uLongf size=encoded.size();
    if(compress2(encoded.data(),&size,image.pixels.data(),image.pixels.size(),Z_DEFAULT_COMPRESSION)!=Z_OK||
       size>Cache::CaseImageLimit-active_cache->case_image_bytes)return false;
    encoded.resize(size);encoded.shrink_to_fit();
    active_cache->case_images.emplace(id,Cache::CaseImage{image.w,image.h,std::move(encoded)});active_cache->case_image_bytes+=size;
    return true;
}
bool GetFrameGeometry(Image& frame,ByteBuffer& mask){
    if(!active_cache||!active_cache->frame_geometry)return false;
    frame=active_cache->frame_geometry;mask=active_cache->frame_mask;return true;
}
void StoreFrameGeometry(const Image& frame,const ByteBuffer& mask){
    if(!active_cache||frame.pixels.size()+mask.size()>Cache::FrameLimit)return;
    // Build both copies first: allocation failure must not leave half a geometry.
    Image copy=frame;ByteBuffer alpha=mask;
    active_cache->frame_geometry=std::move(copy);active_cache->frame_mask=std::move(alpha);
}
Image Crop(const Image& in,int x,int y,unsigned w,unsigned h){
    ProfileScope timing(Phase::Raster);
    Image out;if(!in||!Allocate(out,w,h))return {};
    for(unsigned j=0;j<h;++j)for(unsigned i=0;i<w;++i){
        const int64_t xx=int64_t(x)+i,yy=int64_t(y)+j;
        if(xx>=0&&yy>=0&&xx<int(in.w)&&yy<int(in.h))
            std::memcpy(out.pixels.data()+(size_t(j)*w+i)*4,in.pixels.data()+(size_t(yy)*in.w+xx)*4,4);
    }return out;
}
Image Trim(const Image& in){
    if(!in)return {};unsigned x0=in.w,y0=in.h,x1=0,y1=0;
    for(unsigned y=0;y<in.h;++y)for(unsigned x=0;x<in.w;++x)if(in.pixels[(size_t(y)*in.w+x)*4+3]){
        x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);
    }return x1?Crop(in,x0,y0,x1-x0,y1-y0):Image{};
}
// Separable Lanczos-3, Pillow-style 22-bit coefficients, 8-bit premultiplied passes.
Image Resize(const Image& in,unsigned w,unsigned h,unsigned filter){
    ProfileScope timing(Phase::Raster);
    if(!in||!w||!h||w>8192||h>8192||uint64_t(w)*h*4>MaxBytes)return {};
    if(w==in.w&&h==in.h)return in;
    Image work=in;
    for(size_t p=0;p<work.pixels.size();p+=4)for(int c=0;c<3;++c)
        work.pixels[p+c]=(unsigned(work.pixels[p+c])*work.pixels[p+3]+127)/255;
    auto pass=[filter](const Image& src,unsigned size,bool horizontal){
        Image dst;if(!Allocate(dst,horizontal?size:src.w,horizontal?src.h:size))return Image{};
        const unsigned old=horizontal?src.w:src.h;
        const double scale=double(old)/size,factor=std::max(1.,scale),support=(filter==3?2:filter==2?1:3)*factor;
        constexpr double pi=3.14159265358979323846;
        for(unsigned i=0;i<size;++i){
            const double centre=(i+.5)*scale;
            const int start=std::max(0,int(centre-support+.5)),end=std::min(int(old),int(centre+support+.5));
            std::vector<double> weights;double sum=0;
            for(int k=start;k<end;++k){double t=(k-centre+.5)/factor;
                double v;
                if(filter==3){t=std::abs(t);v=t<1?((1.5*t-2.5)*t)*t+1:t<2?((-.5*t+2.5)*t-4)*t+2:0;}
                else if(filter==2)v=std::max(0.,1-std::abs(t));
                else v=t==0?1:std::abs(t)>=3?0:std::sin(pi*t)*std::sin(pi*t/3)/(pi*pi*t*t/3);
                weights.push_back(v);sum+=v;}
            std::vector<int> coeff;for(double v:weights)coeff.push_back(int(std::round(v/sum*(1<<22))));
            for(unsigned j=0;j<(horizontal?src.h:src.w);++j)for(int c=0;c<4;++c){
                int64_t v=1<<21;
                for(int k=start;k<end;++k){size_t p=horizontal?size_t(j)*src.w+k:size_t(k)*src.w+j;v+=int64_t(src.pixels[p*4+c])*coeff[k-start];}
                const size_t p=horizontal?size_t(j)*size+i:size_t(i)*src.w+j;
                dst.pixels[p*4+c]=std::clamp<int64_t>(v>>22,0,255);
            }
        }return dst;
    };
    const bool vertical_first=int(in.h)-int(h)>0&&int(in.h)-int(h)>(int(in.w)-int(w))*2;
    if(vertical_first){if(h!=in.h)work=pass(work,h,false);if(w!=in.w)work=pass(work,w,true);}
    else{if(w!=in.w)work=pass(work,w,true);if(h!=in.h)work=pass(work,h,false);}
    for(size_t p=0;p<work.pixels.size();p+=4)for(int c=0;c<3;++c)
        work.pixels[p+c]=work.pixels[p+3]?std::min(255u,unsigned(work.pixels[p+c])*255/work.pixels[p+3]):work.pixels[p+c];
    return work;
}
void Composite(Image& dst,const Image& src,int x,int y){
    ProfileScope timing(Phase::Compose);
    if(!dst||!src)return;
    for(unsigned j=0;j<src.h;++j)for(unsigned i=0;i<src.w;++i){
        const int64_t xx=int64_t(x)+i,yy=int64_t(y)+j;if(xx<0||yy<0||xx>=int(dst.w)||yy>=int(dst.h))continue;
        const auto* s=src.pixels.data()+(size_t(j)*src.w+i)*4;
        auto* d=dst.pixels.data()+(size_t(yy)*dst.w+xx)*4;
        if(!s[3])continue;if(s[3]==255){std::memcpy(d,s,4);continue;}
        const unsigned a=s[3]*255+d[3]*(255-s[3]);
        for(int c=0;c<3;++c)d[c]=(s[c]*s[3]*255+d[c]*d[3]*(255-s[3])+a/2)/a;
        d[3]=(a+127)/255;
    }
}
bool Load(const EdenDsmodHostApi* host,std::string_view key,Image& out,const std::string& directory){
    out={};Image src;
    const bool inventory=key.starts_with("module:re4/case/prewarm/")||key.starts_with("module:re4/weapon/")||key.starts_with("module:re4/slot/")||key.starts_with("module:re4/icon/")||key.starts_with("module:re4/digit/")||(key.starts_with("module:re4/case/")&&key.ends_with("/120"));
    if(active_cache&&!inventory){ByteBuffer().swap(active_cache->case_pack);active_cache->case_offsets.clear();}
    if(active_cache&&(key.starts_with("module:re4/ui/")||key.starts_with("module:re4/case/background/"))){
        active_cache->frame_geometry={};ByteBuffer().swap(active_cache->frame_mask);
    }
    if(key=="module:re4/font/atlas"||key=="module:re4/font/atlas/0"){
        if(!Texture(host,"0f000001",0,src,directory)||src.w!=2048||src.h!=768)return false;
        out=Resize(src,1536,576);
    }else if(key=="module:re4/r5/castle-label"||key=="module:re4/icon/3b"){
        const bool stage=key=="module:re4/r5/castle-label";
        if(!Texture(host,stage?"3a000009":"39000009",stage?16:2,src,directory))return false;
        src=Trim(src);if(!src)return false;
        const unsigned w=stage?227:245,h=stage?46:125;
        const double factor=std::min({1.,double(w-unsigned(stage)*16)/src.w,double(h-unsigned(stage)*16)/src.h});
        // Pillow thumbnail rounds the dependent dimension to minimize aspect error.
        unsigned sw=src.w,sh=src.h;
        if(factor<1){
            const double aspect=double(sw)/sh;
            if(double(w-unsigned(stage)*16)/(h-unsigned(stage)*16)>=aspect){sh=h-unsigned(stage)*16;double ideal=sh*aspect;auto lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sw=std::abs(aspect-lo/sh)<std::abs(aspect-hi/sh)?lo:hi;}
            else{sw=w-unsigned(stage)*16;double ideal=sw/aspect;auto lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sh=std::abs(aspect-sw/lo)<std::abs(aspect-sw/hi)?lo:hi;}
        }
        src=Resize(src,sw,sh);if(!Allocate(out,w,h))return false;
        Composite(out,src,(w-sw)/2,(h-sh)/2);
    }else if(InventoryImage(host,key,out,directory))return true;
    else if(UiImage(host,key,out,directory))return true;
    else return MapImage(host,key,out,directory);
    return bool(out);
}
EdenDsmodBool LoadCached(Cache& cache,const EdenDsmodHostApi* host,const char* key,void* receiver,EdenDsmodImageSink sink){
    std::lock_guard lock(cache.mutex);
    const auto now=std::chrono::steady_clock::now();const unsigned stage=cache.stage.load(std::memory_order_relaxed);
    if(stage!=cache.decoded_stage||(cache.last.time_since_epoch().count()&&now-cache.last>std::chrono::seconds(2)))cache.Reset();
    cache.decoded_stage=stage;cache.last=now;
    const std::string_view source=key?key:"";
    cache.ui_burst=source.starts_with("module:re4/bg/");
    if(!cache.ui_burst){cache.ui_materials.clear();cache.ui_bytes=0;}
    cache.composition=source.starts_with("module:re4/bg/")||source.starts_with("module:re4/hp/")||source.starts_with("module:re4/ui/")||source.starts_with("module:re4/case/background/")||source.starts_with("module:re4/case/name/");
    cache.Trim();cache.TrimSources();
    struct Scope{Cache* previous;Scope(Cache& c):previous(active_cache){active_cache=&c;}~Scope(){active_cache=previous;}} scope(cache);
    ProfileScope timing(Phase::Other);
    return LoadImage(&cache,host,key,receiver,sink);
}
EdenDsmodBool LoadImage(void* instance,const EdenDsmodHostApi* host,const char* key,void* receiver,EdenDsmodImageSink sink){
    if(!instance||!host||!key||!sink)return EDEN_DSMOD_FALSE;
    try{Image out;if(!Load(host,key,out))return EDEN_DSMOD_FALSE;
        // A transient font failure must not fail the unrelated requested image.
        try{if(active_cache)active_cache->font.Warm(host,
            std::string_view(key).starts_with("module:re4/font/atlas")?&out:nullptr);}catch(...){}
        ProfileScope copy(Phase::Sink);
        sink(receiver,out.w,out.h,out.pixels.data(),out.pixels.size());
        return EDEN_DSMOD_TRUE;
    }catch(...){return EDEN_DSMOD_FALSE;}
}
}

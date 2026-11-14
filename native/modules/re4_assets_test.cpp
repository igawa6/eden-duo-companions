// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_assets.h"
#include "re4_map.h"
#include <iomanip>
#include "re4_map_art_tables.h"
#include "re4_map_state_tables.h"
#include "re4_names.h"
#include "re4_slots.h"
#include "re4_case_art.h"
#include <map>
#include <set>
#include <thread>
#include <new>
#include "re4_sha256.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <zlib.h>
#include <sys/resource.h>
namespace re4::assets {bool UiRecipeImage(const EdenDsmodHostApi*,unsigned,Image&,const std::string&);}
using namespace re4::assets;
namespace {
void Put(std::vector<uint8_t>& b,size_t p,uint32_t n){for(int i=0;i<4;++i)b[p+i]=n>>(i*8);}
size_t DirectoryRead(void* user,const char* path,uint64_t offset,void* output,size_t size){
    std::ifstream f(*static_cast<std::string*>(user)+"/"+path,std::ios::binary|std::ios::ate);
    if(!f)return 0;const auto total=f.tellg();if(!output)return total;
    if(offset>uint64_t(total))return 0;f.seekg(offset);f.read(static_cast<char*>(output),size);return f.gcount();
}
void Receive(void* user,uint32_t w,uint32_t h,const uint8_t* p,size_t n){
    auto& out=*static_cast<Image*>(user);assert(!out);out={w,h,ByteBuffer(p,p+n)};
}
}
struct ProfileImage {
    unsigned w=0,h=0;std::vector<uint8_t> pixels;
    explicit operator bool()const{return w&&h&&pixels.size()==size_t(w)*h*4;}
};
void ProfileReceive(void* user,uint32_t w,uint32_t h,const uint8_t* p,size_t n){
    auto& out=*static_cast<ProfileImage*>(user);assert(!out);
    out={w,h,std::vector<uint8_t>(p,p+n)};
}
int ProfileAssets(int argc,char** argv){
    const char* rom=std::getenv("RE4_TEST_ROMFS");if(!rom)return 77;
    std::string directory=rom;EdenDsmodHostApi host{};host.userdata=&directory;host.read_romfs=DirectoryRead;ProfileImage im;

    assert(argc==3);std::ifstream list(argv[2]);assert(list);std::string key;Cache cache;std::set<std::string> warmed;
    double total=0;
    while(std::getline(list,key)){
        if(key=="idle"){cache.last-=std::chrono::seconds(3);continue;}
        if(key.starts_with("stage ")){cache.stage.store(std::stoul(key.substr(6)));continue;}
        const bool warm=key.starts_with("warm ");if(warm)key.erase(0,5);
        if(!warm&&warmed.contains(key)){std::cout<<"hit "<<key<<"\n";continue;}
        Profile profile;cache.profile=&profile;im={};
        assert(LoadCached(cache,&host,key.c_str(),&im,ProfileReceive)&&im);
        warmed.insert(key);
        std::cout<<(warm?"warm ":"key ")<<key;
        for(double ms:profile.ms){std::cout<<" "<<ms;total+=ms;}std::cout<<" "<<re4::Sha256(im.pixels)<<"\n";
        assert(cache.bytes<=Cache::Limit&&cache.source_bytes<=Cache::SourceLimit&&cache.case_pack.size()<=Cache::CaseLimit);
        assert(cache.frame_geometry.pixels.size()+cache.frame_mask.size()<=Cache::FrameLimit);
        assert(cache.case_image_bytes<=Cache::CaseImageLimit);
        assert(cache.ui_bytes<=Cache::UiLimit);
        struct rusage per_key{};getrusage(RUSAGE_SELF,&per_key);std::cout<<"hwm_kib "<<per_key.ru_maxrss<<"\n";
        std::cout<<"image_bytes "<<im.pixels.size()<<"\n";
        std::cout<<"retained "<<cache.bytes<<" "<<cache.source_bytes<<" "<<cache.case_pack.size()<<" "<<cache.case_image_bytes<<" "<<cache.case_images.size()<<"\n";
        std::cout<<"ui_retained "<<cache.ui_bytes<<"\n";
        for(const auto& [name,c]:profile.sources)std::cout<<"source "<<name<<" "<<c.reads<<" "<<c.inflates<<" "<<c.decodes<<"\n";
    }
    cache.profile=nullptr;
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::cout<<"total_ms "<<total<<"\npeak_rss_kib "<<usage.ru_maxrss<<"\n";return 0;

}
int main(int argc,char** argv){
    if(argc>1&&std::string_view(argv[1])=="--profile")return ProfileAssets(argc,argv);
    if(argc==2&&std::string_view(argv[1])=="--geometry"){
        const char* rom=std::getenv("RE4_TEST_ROMFS");
        if(!rom||!std::filesystem::is_regular_file(std::string(rom)+"/bio4/ss/cmn/map_obj1.dat"))return 77;
        re4::GeometryCache geometry;
        std::shared_ptr<const re4::GeometryStage> previous;
        std::string root=rom;std::vector<uint8_t> original;
        assert(Read(nullptr,"bio4/ss/cmn/map_obj1.dat",original,root));
        for(size_t n:{size_t(1),size_t(4),size_t(15),size_t(16),size_t(128),original.size()/2}){
            auto truncated=std::vector<uint8_t>(original.begin(),original.begin()+n);
            EdenDsmodHostApi bad{};bad.userdata=&truncated;
            bad.read_romfs=[](void* p,const char*,uint64_t offset,void* out,size_t size)->size_t{
                const auto& b=*static_cast<std::vector<uint8_t>*>(p);if(!out)return b.size();
                if(offset>b.size()||size>b.size()-offset)return 0;std::memcpy(out,b.data()+offset,size);return size;
            };
            re4::GeometryCache refused;assert(!refused.Warm(&bad,1)&&!refused.Get(1));
        }
        bool first=true;std::cout<<std::setprecision(17)<<"[";
        for(unsigned stage:{1,2,3,5}){
            const auto start=std::chrono::steady_clock::now();assert(geometry.Warm(nullptr,stage,rom));
            const auto data=geometry.Get(stage);assert(data&&data->bytes<=re4::GeometryCache::Limit);
            if(previous){assert(previous->stage!=stage&&!previous->meshes.empty());assert(!geometry.Get(previous->stage));}
            previous=data;
            std::jthread reader([&]{for(unsigned n=0;n<1000;++n){const auto p=geometry.Get(stage);assert(p&&p->stage==stage);}});
            assert(geometry.Warm(nullptr,stage,rom));
            std::cerr<<"stage "<<stage<<" bytes "<<data->bytes<<" ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
            for(const auto& m:data->meshes){
                if(!first)std::cout<<",";first=false;
                std::cout<<"{\"area\":\""<<m.area<<"\",\"vertices\":[";bool comma=false;
                for(const auto& v:m.vertices){if(comma)std::cout<<",";comma=true;std::cout<<"["<<v.x<<","<<v.y<<","<<v.z<<","<<v.mx<<","<<v.my<<","<<v.mh<<"]";}
                std::cout<<"],\"triangles\":[";comma=false;
                for(const auto& t:m.triangles){if(comma)std::cout<<",";comma=true;std::cout<<"["<<t.a<<","<<t.b<<","<<t.c<<"]";}
                std::cout<<"],\"narrow\":"<<(m.narrow?"true":"false")<<",\"heading\":"<<m.heading<<",\"affine\":"<<(m.affine?"true":"false")<<",\"ox\":"<<m.ox<<",\"oz\":"<<m.oz<<"}";
            }
        }std::cout<<"]\n";return 0;
    }
    if(argc==2&&std::string_view(argv[1])=="--names"){
        const char* rom=std::getenv("RE4_TEST_ROMFS");
        if(!rom||!std::filesystem::is_regular_file(std::string(rom)+"/bio4/etc/core.udas"))return 77;
        re4::RuntimeNames names;names.Load(nullptr,rom);assert(names.Ready());
        for(unsigned i=0;i<272;++i)std::cout<<names.Get(i)<<"\n";
        assert(!*names.Get(999));return 0;
    }
    assert(re4::Sha256({})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc="abc";assert(re4::Sha256(std::span(reinterpret_cast<const uint8_t*>(abc.data()),abc.size()))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::vector<uint8_t> out,fen(64);Put(fen,0,3);uLongf count=60;
    assert(compress(fen.data()+4,&count,reinterpret_cast<const Bytef*>("abc"),3)==Z_OK);fen.resize(count+4);
    assert(InflateFen(fen,out)&&std::string(out.begin(),out.end())=="abc");
    auto bad=fen;bad.back()^=1;assert(!InflateFen(bad,out)&&out.empty());
    bad=fen;Put(bad,0,4);assert(!InflateFen(bad,out));
    bad=fen;Put(bad,0,MaxBytes+1);assert(!InflateFen(bad,out));
    bad=fen;bad.push_back(0);assert(InflateFen(bad,out));
    for(unsigned n=0;n<10;++n)assert(!InflateFen(std::span(fen).first(n),out));
    Image im;
    for(const char* cc:{"DXT1","DXT3","DXT5","DX10"}){
        const bool dx10=!std::strcmp(cc,"DX10"),bc1=!std::strcmp(cc,"DXT1");
        std::vector<uint8_t> dds((dx10?148:128)+(bc1?8:16));
        std::memcpy(dds.data(),"DDS ",4);Put(dds,4,124);Put(dds,12,3);Put(dds,16,1);Put(dds,76,32);std::memcpy(dds.data()+84,cc,4);
        if(dx10){Put(dds,128,98);Put(dds,132,3);Put(dds,140,1);dds[148]=0x40;}
        else {const unsigned p=bc1?128:136;dds[p]=0;dds[p+1]=0xf8;if(!bc1)for(unsigned i=128;i<136;++i)dds[i]=255;}
        assert(DecodeDds(dds,im)&&im.w==1&&im.h==3);
        if(!dx10)assert(im.pixels[0]==255&&im.pixels[1]==0&&im.pixels[2]==0&&im.pixels[3]==255);
        auto invalid=dds;Put(invalid,16,0xffffffff);assert(!DecodeDds(invalid,im));
        invalid=dds;invalid[84]='?';assert(!DecodeDds(invalid,im));
        if(dx10){invalid=dds;Put(invalid,128,97);assert(!DecodeDds(invalid,im));
            invalid=dds;invalid[148]=0;assert(!DecodeDds(invalid,im));}
        for(const auto box:{std::array<int,4>{-1,-2,4,8},std::array<int,4>{0,1,1,2},std::array<int,4>{5,6,2,3}}){
            Image full,region;assert(DecodeDds(dds,full));
            assert(DecodeDdsRegion(dds,region,box[0],box[1],box[2],box[3]));
            assert(region.pixels==Crop(full,box[0],box[1],box[2],box[3]).pixels);
        }
        // An unaligned crop spans several BC blocks and the partial image edge.
        Put(dds,12,7);Put(dds,16,9);dds.resize((dx10?148:128)+(bc1?8:16)*6);
        if(dx10)for(unsigned i=0;i<6;++i)dds[148+i*16]=0x40;
        Image full,region;assert(DecodeDds(dds,full));
        assert(DecodeDdsRegion(dds,region,3,2,7,6));assert(region.pixels==Crop(full,3,2,7,6).pixels);
        dds.pop_back();assert(!DecodeDds(dds,im)&&!im);
    }
    std::vector<uint8_t> empty;assert(!DecodeDds(empty,im));
    assert(!Read(nullptr,"../x",out,"."));
    Image red{1,1,{255,0,0,128}},blue{1,1,{0,0,255,255}};Composite(blue,red,0,0);
    assert((blue.pixels==ByteBuffer{128,0,127,255}));
    assert(Crop(red,-1,0,2,1).pixels[3]==0);assert(Resize(red,3,2).w==3);
    {
        Cache c;const size_t half=Cache::Limit/2;
        c.textures.emplace("old",Image{1,1,ByteBuffer(half)});c.used["old"]=1;
        c.textures.emplace("recent",Image{1,1,ByteBuffer(half)});c.used["recent"]=2;c.bytes=Cache::Limit;
        c.Trim(half);assert(c.bytes==half&&!c.textures.contains("old")&&c.textures.contains("recent"));
        const std::vector<uint8_t> data(Cache::SourceLimit/2);
        c.StoreSource("old",data);c.StoreSource("recent",data);c.source_used["old"]=++c.clock;
        c.StoreSource("new",data);assert(c.source_bytes==Cache::SourceLimit);
        assert(c.sources.contains("old")&&!c.sources.contains("recent"));
        c.case_pack.resize(1024);c.frame_geometry={1,1,{1,2,3,4}};c.frame_mask.resize(1);c.stage.store(2);EdenDsmodHostApi host{};Image im;
        assert(!LoadCached(c,&host,"module:re4/absent",&im,Receive));
        assert(c.bytes==0&&c.source_bytes==0&&c.case_pack.empty()&&!c.frame_geometry&&c.frame_mask.empty()&&c.decoded_stage==2);
        c.StoreSource("idle",data);c.last-=std::chrono::seconds(3);
        assert(!LoadCached(c,&host,"module:re4/absent",&im,Receive)&&c.sources.empty());
    }
    re4::GeometryCache geometry;
    assert(!geometry.Get(1));
    for(unsigned stage:{0,1,2,3,4,5,6})assert(!geometry.Warm(nullptr,stage)&&!geometry.Get(stage));
    EdenDsmodHostApi oversized{};oversized.read_romfs=[](void*,const char*,uint64_t,void* out,size_t)->size_t{
        assert(!out);return (2u<<20)+1;
    };
    assert(!geometry.Warm(&oversized,1)&&!geometry.Get(1));
    assert(!re4::MapPose("village-100",{0,0,0},{0,1}));
    unsigned calls=0;EdenDsmodHostApi corrupt{};corrupt.userdata=&calls;
    corrupt.read_romfs=[](void* u,const char*,uint64_t,void* output,size_t)->size_t{
        ++*static_cast<unsigned*>(u);if(output)std::memset(output,0,4);return 4;};
    assert(!geometry.Warm(&corrupt,1)&&!geometry.Get(1));calls=0;
    re4::RuntimeNames unsupported;unsupported.StageChanged(1);unsupported.Load(&corrupt);unsupported.Load(&corrupt);
    assert(calls==2&&!unsupported.Ready()&&!*unsupported.Get(0x23));
    unsupported.StageChanged(1);unsupported.Load(&corrupt);assert(calls==2);
    unsupported.StageChanged(0);unsupported.Load(&corrupt);assert(calls==2);
    unsupported.StageChanged(2);unsupported.Load(&corrupt);assert(calls==4&&!unsupported.Ready());
    re4::RuntimeNames missing;missing.Load(nullptr);assert(!missing.Ready()&&!*missing.Get(0x23));
    const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
    assert(ext&&ext->load_image&&!eden_dsmod_get_extensions(0,0));
    EdenDsmodHostApi host{};int instance=0;
    // Refusal contract of the decoder, without an allocated module instance.

    assert(!LoadImage(&instance,&host,"module:re4/unknown",&im,Receive));
    assert(!LoadImage(&instance,&host,"module:re4/font/atlas",&im,Receive));
    Cache cached;cached.textures.emplace("3a000009/2",Image{1,1,{255,0,0,255}});cached.bytes=4;
    im={};assert(LoadCached(cached,&host,"module:re4/mark/star",&im,Receive)&&im);
    cached.case_pack.assign(256,0);cached.case_offsets={0};
    cached.stage.store(1);im={};assert(!LoadCached(cached,&host,"module:re4/mark/star",&im,Receive)&&!im&&cached.textures.empty()&&cached.bytes==0&&cached.case_pack.empty()&&cached.case_offsets.empty());
    cached.case_pack.assign(256,0);cached.case_offsets={0};im={};
    assert(!LoadCached(cached,&host,"module:re4/ui/field",&im,Receive)&&!im);
    assert(cached.case_pack.empty()&&cached.case_offsets.empty());
    assert(!ext->load_image(nullptr,&host,"module:re4/mark/star",&im,Receive));
    host.abi_version=EDEN_DSMOD_MODULE_ABI_VERSION;host.abi_hash=EDEN_DSMOD_MODULE_ABI_HASH;host.struct_size=sizeof host;host.title_id=re4::Title;
    for(unsigned i=0;i<16;++i){auto nib=[](char c){return c<='9'?c-'0':c-'A'+10;};host.build_id[i]=nib(re4::Build[i*2])*16+nib(re4::Build[i*2+1]);}
    host.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{return false;};host.is_mapped=[](void*,uint64_t,uint64_t)->EdenDsmodBool{return false;};
    host.publish_i64=[](void*,const char*,int64_t){};host.publish_f64=[](void*,const char*,double){};host.publish_text=[](void*,const char*,const char*){};
    const auto* api=eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,EDEN_DSMOD_MODULE_ABI_HASH);assert(api);
    void* module_instance=api->create(&host,nullptr);assert(module_instance);
    struct Lifetime{const EdenDsmodModuleApi* api;void* p;~Lifetime(){api->destroy(p);}} lifetime{api,module_instance};
    const auto* font_ext=eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION,EDEN_DSMOD_FONT_EXT_HASH);
    assert(font_ext&&font_ext->decode_font&&!eden_dsmod_get_font_extensions(0,0));
    unsigned font_calls=0;
    const auto count_font=+[](void* p,uint32_t,uint32_t,const EdenDsmodFontGlyph*,uint32_t){++*static_cast<unsigned*>(p);};
    std::array<uint8_t,40> sentinel{};sentinel[32]=0x78;sentinel[33]=0x56;sentinel[34]=0x34;sentinel[35]=0x12;
    assert(!font_ext->decode_font(module_instance,sentinel.data(),sentinel.size(),&font_calls,count_font)&&!font_calls);
    Font failed_font;failed_font.Warm(nullptr);
    assert(!failed_font.attempted&&!failed_font.Decode(sentinel.data(),sentinel.size(),&font_calls,count_font));
    assert(!font_calls&&!failed_font.installed.load());
    Cache transient;transient.textures.emplace("3a000009/2",Image{1,1,{255,0,0,255}});transient.bytes=4;
    auto throwing_host=host;
    throwing_host.read_romfs=[](void*,const char*,uint64_t,void*,size_t)->size_t{throw std::bad_alloc{};};
    im={};assert(LoadCached(transient,&throwing_host,"module:re4/mark/star",&im,Receive)&&im);
    assert(!transient.font.attempted&&!transient.font.ready.load());
    Font exhausted_font;
    for(unsigned n=0;n<31;++n)exhausted_font.Warm(nullptr);
    assert(exhausted_font.attempted&&exhausted_font.attempts==30&&!exhausted_font.ready.load());
    assert(!exhausted_font.Decode(sentinel.data(),sentinel.size(),&font_calls,count_font)&&!font_calls);
    Font malformed_font;Image wrong_atlas{1,1,{255,255,255,255}};
    malformed_font.Warm(nullptr,&wrong_atlas);assert(malformed_font.attempted&&!malformed_font.ready.load());
    assert(!ext->load_image(module_instance,&host,"module:re4/mark/star",&im,nullptr));
    struct CorruptMap{std::vector<uint8_t> bytes;unsigned calls=0;} malformed{{0,0,0,0}};
    EdenDsmodHostApi fake{};fake.userdata=&malformed;
    fake.read_romfs=[](void* u,const char*,uint64_t offset,void* output,size_t n)->size_t{
        auto& data=*static_cast<CorruptMap*>(u);++data.calls;if(!output)return data.bytes.size();
        if(offset>data.bytes.size()||n>data.bytes.size()-offset)return 0;
        std::memcpy(output,data.bytes.data()+offset,n);return n;
    };
    for(const char* key:{"module:re4/door/090/1","module:re4/door/+90/1","module:re4/door/90/4","module:re4/door/999/1",
                        "module:re4/weapon/zz","module:re4/weapon/ff","module:re4/weapon/23/missing","module:re4/icon/ffff","module:re4/digit/10","module:re4/ui/absent","module:re4/bg/99999","module:re4/map/frame/4","module:re4/map/frame/+0",
                        "module:re4/room/village/100/4/f6","module:re4/room/village/100/5","module:re4/room/stage4/400/1"}){
        malformed.calls=0;im={};assert(!ext->load_image(module_instance,&fake,key,&im,Receive)&&!im&&malformed.calls==0);
    }
    malformed.calls=0;im={};assert(!ext->load_image(module_instance,&fake,"module:re4/door/90/1",&im,Receive)&&malformed.calls>0&&!im);
    for(const char* key:{"module:re4/case/23/120","module:re4/weapon/23","module:re4/slot/23","module:re4/slot/23/missing","module:re4/icon/3b","module:re4/digit/0","module:re4/ui/field","module:re4/ui/standby","module:re4/map/backing","module:re4/map/unmapped","module:re4/bg/0","module:re4/hp/base","module:re4/map/frame/0"}){
        malformed.calls=0;im={};assert(!ext->load_image(module_instance,&fake,key,&im,Receive)&&!im&&malformed.calls>0);
    }
    malformed.calls=0;
    Cache malformed_cache;malformed_cache.textures.emplace("3a000009/27",Image{1,1,{255,255,255,255}});malformed_cache.bytes=4;
    im={};assert(!LoadCached(malformed_cache,&fake,"module:re4/room/village/100/1",&im,Receive)&&!im);
    // Valid nested container, invalid BIN version: failure must not invoke the sink.
    malformed.bytes.assign(80+24+0x98,0);Put(malformed.bytes,0,8);
    for(unsigned n=0;n<8;++n)Put(malformed.bytes,16+n*4,80);
    Put(malformed.bytes,80,1);Put(malformed.bytes,96,24);std::memcpy(malformed.bytes.data()+100,"BIN",3);
    assert(!LoadCached(malformed_cache,&fake,"module:re4/room/village/100/1",&im,Receive)&&!im&&malformed.calls==8);
    std::cout<<"synthetic inflate/DDS/BC/composition/SHA/refusal PASS\n";
    if(argc<2)return 0;
    std::string directory=std::getenv("RE4_TEST_ROMFS")?std::getenv("RE4_TEST_ROMFS"):"";
    if(directory.empty()||!std::filesystem::exists(directory+"/bio4/etc/core.udas")){
        std::cout<<"SKIP: set RE4_TEST_ROMFS to the extracted RomFS root\n";return 77;
    }
    host.userdata=&directory;host.read_romfs=DirectoryRead;
    if(std::string_view(argv[1])=="--font"){
        assert(argc==3);
        const auto* fonts=eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION,EDEN_DSMOD_FONT_EXT_HASH);
        assert(fonts&&fonts->decode_font&&!eden_dsmod_get_font_extensions(0,0));
        std::vector<uint8_t> sentinel;assert(Read(&host,"bio4/font/common_p.fnt",sentinel));
        struct Metrics{unsigned height=0,first=0;std::vector<EdenDsmodFontGlyph> glyphs;} received;
        const auto sink=+[](void* p,uint32_t h,uint32_t first,const EdenDsmodFontGlyph* g,uint32_t n){
            auto& m=*static_cast<Metrics*>(p);m.height=h;m.first=first;m.glyphs.assign(g,g+n);
        };
        assert(!fonts->decode_font(nullptr,sentinel.data(),sentinel.size(),&received,sink));
        assert(!fonts->decode_font(module_instance,sentinel.data(),sentinel.size(),&received,sink)&&received.glyphs.empty());
        static std::map<std::string,int64_t> font_publications;
        auto font_host=host;
        font_host.publish_i64=[](void*,const char* k,int64_t v){font_publications[k]=v;};
        api->sample(module_instance,&font_host);
        assert(font_publications["__font_epoch"]==0&&font_publications["re4.font.installed"]==0);
        // First ordinary image request warms the font even if no text has drawn yet.
        const auto start=std::chrono::steady_clock::now();im={};
        assert(ext->load_image(module_instance,&host,"module:re4/mark/arrow",&im,Receive));
        std::cout<<"first_image_and_font_ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
        assert(!fonts->decode_font(module_instance,nullptr,0,&received,sink));
        assert(!fonts->decode_font(module_instance,sentinel.data(),sentinel.size(),&received,nullptr));
        auto bad=sentinel;bad[32]^=1;
        assert(!fonts->decode_font(module_instance,bad.data(),bad.size(),&received,sink)&&received.glyphs.empty());
        api->sample(module_instance,&font_host);
        assert(font_publications["__font_epoch"]==1&&font_publications["re4.font.installed"]==0);
        const auto decode_start=std::chrono::steady_clock::now();
        assert(fonts->decode_font(module_instance,sentinel.data(),sentinel.size(),&received,sink));
        std::cout<<"cached_decode_ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decode_start).count()<<"\n";
        assert(received.first==32&&received.glyphs.size()==95);
        api->sample(module_instance,&font_host);
        assert(font_publications["__font_epoch"]==1&&font_publications["re4.font.installed"]==1);
        // Fast installation before Sample must not trigger a second decode/drop of live pages.
        void* fast=api->create(&host,nullptr);assert(fast);
        im={};assert(ext->load_image(fast,&host,"module:re4/font/atlas/0",&im,Receive));
        assert(fonts->decode_font(fast,sentinel.data(),sentinel.size(),&received,sink));
        api->sample(fast,&font_host);
        assert(font_publications["__font_epoch"]==0&&font_publications["re4.font.installed"]==1);
        api->destroy(fast);
        {std::ofstream f(std::string(argv[2])+"/glyphs.bin",std::ios::binary);
         f.write(reinterpret_cast<const char*>(received.glyphs.data()),received.glyphs.size()*sizeof(EdenDsmodFontGlyph));assert(f);}
        std::cout<<"line_height "<<received.height<<"\n";
        // Isolate cold atlas+metrics generation, without any retained decoded textures.
        Cache font_cache;const auto warm_start=std::chrono::steady_clock::now();font_cache.font.Warm(&host);
        std::cout<<"metrics_and_atlas_ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warm_start).count()<<"\n";
        assert(font_cache.font.ascent==received.height);
        assert(!std::memcmp(font_cache.font.glyphs.data(),received.glyphs.data(),received.glyphs.size()*sizeof(EdenDsmodFontGlyph)));
        {std::ofstream f(std::string(argv[2])+"/font.bin",std::ios::binary);
         f.write(reinterpret_cast<const char*>(font_cache.font.packed.data()),font_cache.font.packed.size());assert(f);}
        const auto before=font_cache.font.packed;const auto* storage=font_cache.font.packed.data();
        font_cache.stage.store(5);font_cache.font.Warm(nullptr);
        assert(font_cache.font.packed==before&&font_cache.font.packed.data()==storage);
        Font failed;failed.Warm(nullptr);failed.Warm(&host);
        assert(!failed.installed.load()&&failed.ready.load());
        assert(failed.Decode(sentinel.data(),sentinel.size(),&received,sink)&&failed.installed.load());
        im={};assert(ext->load_image(module_instance,&host,"module:re4/font/atlas/0",&im,Receive));
        {std::ofstream f(std::string(argv[2])+"/font.rgba",std::ios::binary);
         f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);}
        im={};assert(!ext->load_image(module_instance,&host,"module:re4/font/atlas/1",&im,Receive)&&!im);
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        std::cout<<"immutable_metrics_bytes "<<font_cache.font.packed.size()+sizeof font_cache.font.glyphs
                 <<"\npeak_rss_kib "<<usage.ru_maxrss<<"\n";return 0;
    }
    if(std::string_view(argv[1])=="--fixture"){
        {
            Cache burst;
            for(const char* key:{"module:re4/bg/0","module:re4/bg/1","module:re4/bg/31","module:re4/bg/32","module:re4/bg/62","module:re4/bg/98","module:re4/bg/0"}){
                Image expected,actual;assert(Load(&host,key,expected));
                assert(LoadCached(burst,&host,key,&actual,Receive));
                assert(expected.w==actual.w&&expected.h==actual.h&&expected.pixels==actual.pixels);
                assert(burst.ui_bytes<=Cache::UiLimit);
            }
            assert(burst.ui_bytes>0);
            burst.last-=std::chrono::seconds(3);burst.ReleaseIdle();
            assert(burst.ui_materials.empty()&&burst.ui_bytes==0);
            Image result;assert(LoadCached(burst,&host,"module:re4/bg/0",&result,Receive));
            assert(burst.ui_bytes>0);burst.Reset();assert(burst.ui_materials.empty()&&burst.ui_bytes==0);
        }
        im={};assert(!ext->load_image(module_instance,&host,"module:re4/room/village/102/4/f0",&im,Receive)&&!im);
        {
            Cache shared;shared.stage.store(1);
            Image expected;assert(LoadCached(shared,&host,"module:re4/mark/arrow",&expected,Receive));
            const auto hash=re4::Sha256(expected.pixels);
            std::vector<std::jthread> workers;
            for(unsigned t=0;t<4;++t)workers.emplace_back([&,t]{
                for(unsigned n=0;n<8;++n){
                    shared.stage.store(1+(n+t)%2);Image result;
                    assert(LoadCached(shared,&host,"module:re4/mark/arrow",&result,Receive));
                    assert(re4::Sha256(result.pixels)==hash);
                }
            });
            workers.clear();assert(shared.bytes<=Cache::Limit&&shared.source_bytes<=Cache::SourceLimit);
        }
        {
            Cache cold,warm;Image result;EdenDsmodHostApi unavailable{};
            assert(!LoadCached(warm,&unavailable,"module:re4/case/prewarm/0",&result,Receive));
            for(unsigned group=0;group<8;++group){
                result={};const auto key="module:re4/case/prewarm/"+std::to_string(group);
                assert(LoadCached(warm,&host,key.c_str(),&result,Receive)&&result.w==1&&result.h==1);
            }
            assert(warm.case_images.size()==64&&warm.case_image_bytes<=Cache::CaseImageLimit);
            unsigned reads=0;EdenDsmodHostApi no_reads{};no_reads.userdata=&reads;
            no_reads.read_romfs=[](void* user,const char*,uint64_t,void*,size_t)->size_t{++*static_cast<unsigned*>(user);return 0;};
            warm.stage.store(3);warm.last-=std::chrono::seconds(3);
            for(unsigned id=0;id<256;++id)if(re4::CaseArtSupported(id)){
                const auto key=re4::ItemArt(id,"case-art-");Image expected,actual;
                assert(LoadCached(cold,&host,key.c_str(),&expected,Receive));
                assert(LoadCached(warm,&no_reads,key.c_str(),&actual,Receive));
                const auto cells=re4::CaseSize(id);
                assert(expected.w==cells[0]*120&&expected.h==cells[1]*120);
                assert(expected.w==actual.w&&expected.h==actual.h&&expected.pixels==actual.pixels);
            }
            // Re-request after host eviction must not decompress the whole material set.
            Profile profile;warm.profile=&profile;result={};
            assert(LoadCached(warm,&no_reads,"module:re4/case/prewarm/0",&result,Receive));
            assert(profile.ms[size_t(Phase::Inflate)]==0&&reads==0);
        }
        // Every present slot has the same native art, dimmed even for the range shotgun.
        for(unsigned id:{3u,33u,64u,35u,37u,39u,41u,42u,44u,45u,46u,47u,48u,52u,53u,54u,55u,56u,148u,23u,109u,62u,82u,1u,2u,14u,65u,16u,71u,83u}){
            Cache slots;Image normal,dim;
            const auto key=re4::ItemArt(id,"slot-art-");
            assert(LoadCached(slots,&host,key.c_str(),&normal,Receive));
            assert(LoadCached(slots,&host,(key+"/missing").c_str(),&dim,Receive));
            assert(normal.w==dim.w&&normal.h==dim.h);bool visible=false;
            for(size_t p=0;p<normal.pixels.size();p+=4){
                assert(normal.pixels[p+3]==dim.pixels[p+3]);
                for(unsigned c=0;c<3;++c){assert(dim.pixels[p+c]==int(normal.pixels[p+c]*.35f));visible|=normal.pixels[p+3]&&normal.pixels[p+c]>dim.pixels[p+c];}
            }assert(visible);
        }
        struct MissingColour{std::string directory;std::vector<uint8_t> common;} no_colour{directory,{}};
        assert(Read(&host,"bio4/ss/eng/ss_map.dat",no_colour.common));
        auto u32=[&](size_t p){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(no_colour.common[p+i])<<(8*i);return v;};
        const size_t colour_blob=u32(20);bool changed=false;
        for(unsigned n=0;n<no_colour.common[colour_blob+5];++n)if(no_colour.common[colour_blob+8+n*0xa8+4]==0){no_colour.common[colour_blob+8+n*0xa8+4]=254;changed=true;}
        assert(changed);EdenDsmodHostApi missing_colour=host;missing_colour.userdata=&no_colour;
        missing_colour.read_romfs=[](void* u,const char* path,uint64_t off,void* output,size_t n)->size_t{
            auto& data=*static_cast<MissingColour*>(u);if(std::string_view(path)!="bio4/ss/eng/ss_map.dat")return DirectoryRead(&data.directory,path,off,output,n);
            if(!output)return data.common.size();if(off>data.common.size()||n>data.common.size()-off)return 0;
            std::memcpy(output,data.common.data()+off,n);return n;
        };
        im={};assert(!ext->load_image(module_instance,&missing_colour,"module:re4/room/village/102/4",&im,Receive)&&!im);
        Cache bad_font;bad_font.textures.emplace("0f000001/0",Image{1,1,{255,255,255,255}});bad_font.bytes=4;
        im={};assert(!LoadCached(bad_font,&host,"module:re4/map/label/village",&im,Receive)&&!im);
        EdenDsmodHostApi no_font=host;
        no_font.read_romfs=[](void* u,const char* path,uint64_t off,void* output,size_t n)->size_t{
            return std::string_view(path).starts_with("bio4/font/")?0:DirectoryRead(u,path,off,output,n);
        };
        im={};assert(ext->load_image(module_instance,&no_font,"module:re4/map/backing",&im,Receive)&&im);
        im={};assert(!ext->load_image(module_instance,&no_font,"module:re4/map/unmapped",&im,Receive)&&!im);
    }

    if(std::string_view(argv[1])=="--ui-node") {
        assert(argc==4);assert(UiRecipeImage(&host,std::stoul(argv[2]),im,{}));
        std::ofstream f(argv[3],std::ios::binary);f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);
        std::cout<<im.w<<" "<<im.h<<"\n";return 0;
    }
    if(std::string_view(argv[1])=="--texture") {
        assert(argc==5);assert(Texture(&host,argv[2],std::stoul(argv[3]),im));
        std::ofstream f(argv[4],std::ios::binary);f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);
        std::cout<<im.w<<" "<<im.h<<"\n";return 0;
    }
    if(std::string_view(argv[1])=="--asset-set") {
        assert(argc==3);std::ifstream list(argv[2]);assert(list);std::string key;double total=0;
        while(std::getline(list,key)){
            im={};const auto start=std::chrono::steady_clock::now();
            assert(ext->load_image(module_instance,&host,key.c_str(),&im,Receive)&&im);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            total+=ms;std::cout<<key<<" "<<ms<<" "<<im.pixels.size()<<"\n";
        }
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        std::cout<<"total_ms "<<total<<"\npeak_rss_kib "<<usage.ru_maxrss<<"\n";return 0;
    }
    if(std::string_view(argv[1])=="--image") {
        assert(argc==4);im={};const auto start=std::chrono::steady_clock::now();assert(ext->load_image(module_instance,&host,argv[2],&im,Receive));
        std::cout<<"image_ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
        std::ofstream f(argv[3],std::ios::binary);f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);
        std::cout<<im.w<<" "<<im.h<<"\n";return 0;
    }
    if(std::string_view(argv[1])=="--map-images"||std::string_view(argv[1])=="--map-memory") {
        const bool memory=std::string_view(argv[1])=="--map-memory";
        assert(argc==(memory?2:3));
        for(const auto& row:MapArtRooms){
            std::string area=row.key;std::replace(area.begin(),area.end(),'/','-');
            bool multi=false,found=false;for(const auto& r:re4::MapStateRooms)if(r.key==area){multi=r.multi;found=true;}assert(found);
            for(unsigned n=0;n<(multi?10u:4u);++n){
                const std::string state=n<4?std::to_string(n+1):"4-f"+std::to_string(n-4);
                const std::string suffix=n<4?state:"4/f"+std::to_string(n-4);
                const std::string key="module:re4/room/"+std::string(row.key)+"/"+suffix;
                const auto start=std::chrono::steady_clock::now();im={};
                assert(ext->load_image(module_instance,&host,key.c_str(),&im,Receive));
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                std::string name="map-room-"+area+"-"+state;
                if(!memory){std::ofstream f(std::string(argv[2])+"/"+name+".rgba",std::ios::binary);
                    f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);}
                std::cout<<name<<" "<<im.w<<" "<<im.h<<" "<<ms<<"\n";
            }
        }
        if(memory)for(const char* key:{"module:re4/map/backing","module:re4/map/unmapped","module:re4/map/frame","module:re4/map/label/village","module:re4/map/label/castle","module:re4/map/label/island","module:re4/map/legend","module:re4/mark/atlas","module:re4/mark/arrow","module:re4/door/0/1","module:re4/door/0/2","module:re4/door/0/3"}){
            im={};assert(ext->load_image(module_instance,&host,key,&im,Receive));
        }
        Image source_atlas;assert(Texture(&host,"3a000009",27,source_atlas)&&source_atlas.pixels.size()<=Cache::Limit);
        std::cout<<"source_atlas_27 "<<source_atlas.w<<" "<<source_atlas.h<<" "<<source_atlas.pixels.size()<<"\n";
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);std::cout<<"peak_rss_kib "<<usage.ru_maxrss<<"\n";return 0;
    }
    re4::RuntimeNames retry;retry.StageChanged(1);retry.Load(nullptr);retry.Load(&host);assert(!retry.Ready());
    retry.StageChanged(2);retry.Load(&host);assert(retry.Ready());
    re4::RuntimeNames names;names.Load(&host);assert(names.Ready());
    for(unsigned id=0;id<272;++id)assert(std::string(retry.Get(id))==names.Get(id));
    assert(!*names.Get(999));
    // Exercise the actual Case/picker publication paths with runtime-only strings.
    std::map<std::string,std::string> texts;EdenDsmodHostApi publisher{};publisher.userdata=&texts;
    publisher.publish_text=[](void* p,const char* k,const char* v){(*static_cast<decltype(texts)*>(p))[k]=v;};
    publisher.publish_i64=[](void*,const char*,int64_t){};
    publisher.publish_f64=[](void*,const char*,double){};
    re4::Snapshot state;state.case_capacity=std::array<uint8_t,2>{15,8};
    for(unsigned id=0;id<256;++id)if(re4::CaseArtSupported(id)){
        const auto size=re4::CaseSize(id);
        state.placed_items=std::vector<re4::PlacedItem>{{1,static_cast<uint16_t>(id),1,
            static_cast<int8_t>(size[0]-1),static_cast<int8_t>(size[1]-1),0,0}};
        re4::PublishCaseSelection(publisher,state,1,&names);
        assert(texts["re4.case.name"]==names.Get(id));
        re4::PublishSlots(publisher,state,"@flag:slots.assignment.0",&names);
        if(re4::StripWeapon(id))assert(texts["re4.picker.0.name"]==names.Get(id));
    }
    names.StageChanged(2);names.Load(nullptr);assert(names.Ready()); // cached across stages

    re4::RuntimeNames dump_names;dump_names.Load(nullptr,directory);assert(dump_names.Ready());
    if(std::string_view(argv[1])!="--samples")for(const auto* pack:{"3b000001","0f000001","07000000","39000009","3a000009","2a000009","2d000009"}){
        const auto start=std::chrono::steady_clock::now();assert(Texture(&host,pack,0,im));
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"atlas "<<pack<<" slot0_ms "<<ms<<" rgba_bytes "<<im.pixels.size()<<"\n";
    }
    const char* keys[]={"module:re4/font/atlas","module:re4/r5/castle-label","module:re4/icon/3b"};
    const char* files[]={"font","castle-label","key-3b"};
    for(unsigned i=0;i<3;++i){
        const auto start=std::chrono::steady_clock::now();im={};
        assert(ext->load_image(module_instance,&host,keys[i],&im,Receive));
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::cout<<keys[i]<<" "<<im.w<<"x"<<im.h<<" ms "<<ms<<"\n";
        if(argc==3){std::ofstream f(std::string(argv[2])+"/"+files[i]+".rgba",std::ios::binary);f.write(reinterpret_cast<const char*>(im.pixels.data()),im.pixels.size());assert(f);}
    }
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);std::cout<<"peak_rss_kib "<<usage.ru_maxrss<<"\n";
}

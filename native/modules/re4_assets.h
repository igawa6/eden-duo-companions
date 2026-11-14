// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_extensions.h"
#include "re4_font.h"
#include "re4_asset_buffer.h"
#include <span>
#include <map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <array>
#include <string>
#include <vector>
namespace re4::assets {
constexpr size_t MaxBytes = 32u << 20;
struct Image {
    unsigned w=0,h=0;
    ByteBuffer pixels;
    explicit operator bool() const { return w && h && pixels.size()==size_t(w)*h*4; }
};
// Optional callback profiler. Durations are exclusive, so their sum is callback time.
enum class Phase { Read, Inflate, Decode, Raster, Compose, Sink, Other, Count };
struct Profile {
    std::array<double,size_t(Phase::Count)> ms{};
    struct Source { unsigned reads=0,inflates=0,decodes=0; };
    std::map<std::string,Source> sources;
};
struct ProfileScope {
    Profile* profile; Phase phase; ProfileScope* parent;
    std::chrono::steady_clock::time_point start; double children=0;
    explicit ProfileScope(Phase);
    ~ProfileScope();
};
// Decoder storage is owned by one module instance; mutable data is guarded across loader calls.
struct Cache {
    static constexpr size_t Limit=4u<<20;
    static constexpr size_t SourceLimit=8u<<20;
    static constexpr size_t FrameLimit=4u<<20;
    std::mutex mutex;
    Profile* profile=nullptr;
    std::atomic<unsigned> stage{0};
    unsigned decoded_stage=0;
    Font font;
    // The case container is retained only during an inventory decode burst.
    // Static UI/map/font requests release it before allocating their work images.
    static constexpr size_t CaseLimit=28u<<20;
    // Immutable final Case rasters survive stage/idle source-cache release.
    // Store cell-sized outputs, never the old transparent page-sized canvases.
    static constexpr size_t CaseImageLimit=4u<<20;
    size_t case_image_bytes=0;
    struct CaseImage { unsigned w,h; ByteBuffer encoded; };
    std::map<unsigned,CaseImage> case_images;
    Image frame_geometry;
    // Shared resampled materials for compact background recipes, bounded per burst.
    static constexpr size_t UiLimit=6u<<20;
    size_t ui_bytes=0;
    bool ui_burst=false;
    std::map<unsigned,std::shared_ptr<const Image>> ui_materials;
    ByteBuffer frame_mask;
    ByteBuffer case_pack;
    std::vector<size_t> case_offsets;
    size_t bytes=0;
    std::map<std::string,Image> textures;
    std::map<std::string,ByteBuffer> sources;
    std::map<std::string,uint64_t> used,source_used;
    // Large UI DAGs reserve room for their simultaneous working surfaces.
    bool composition=false;
    // Final rasters displace 5 MiB of the transient source/decoded working set.
    size_t TextureLimit()const{return composition?(case_image_bytes?0:1u<<20):case_image_bytes?3u<<20:Limit;}
    size_t DdsLimit()const{return composition?(case_image_bytes?1u<<20:2u<<20):case_image_bytes?4u<<20:SourceLimit;}
    uint64_t clock=0;
    size_t source_bytes=0;
    std::chrono::steady_clock::time_point last{};
    void Reset();
    void ReleaseIdle();
    void Trim(size_t incoming=0);
    void TrimSources(size_t incoming=0);
    void StoreSource(const std::string&,std::span<const uint8_t>);

};
EdenDsmodBool LoadCached(Cache&,const EdenDsmodHostApi*,const char*,void*,EdenDsmodImageSink);
// Directory fallback is explicit and test-only; production reads only the supplied host.
bool Read(const EdenDsmodHostApi* host, std::string_view path, std::vector<uint8_t>& out,
          const std::string& directory={});
bool InflateFen(std::span<const uint8_t> in, std::vector<uint8_t>& out);
bool Read(const EdenDsmodHostApi*,std::string_view,ByteBuffer&,const std::string& directory={});
bool InflateFen(std::span<const uint8_t>,ByteBuffer&);
bool DecodeDds(std::span<const uint8_t> in, Image& out);
bool DecodeDdsRegion(std::span<const uint8_t>,Image&,int x,int y,unsigned w,unsigned h);
bool Texture(const EdenDsmodHostApi* host, const char* pack, unsigned index, Image& out,
             const std::string& directory={});
bool Textures(const EdenDsmodHostApi*,std::span<const unsigned>,std::vector<Image>&,const std::string& directory={},const char* pack="3b000001");
bool TextureRegion(const EdenDsmodHostApi*,const char*,unsigned,Image&,int,int,unsigned,unsigned,const std::string& directory={});
bool HasCachedCaseImage(unsigned);
bool CachedCaseImage(unsigned,Image&);
bool StoreCaseImage(unsigned,const Image&);
bool GetFrameGeometry(Image&,ByteBuffer&);
void StoreFrameGeometry(const Image&,const ByteBuffer&);
bool HasUiMaterial(unsigned);
void ClearUiMaterials();
std::shared_ptr<const Image> UiMaterial(unsigned);
std::shared_ptr<const Image> StoreUiMaterial(unsigned,Image&&);
Image Crop(const Image& in,int x,int y,unsigned w,unsigned h);
Image Resize(const Image& in,unsigned w,unsigned h,unsigned filter=1);
void Composite(Image& dst,const Image& src,int x,int y);
Image Trim(const Image& in);
bool Load(const EdenDsmodHostApi* host,std::string_view key,Image& out,
          const std::string& directory={});
EdenDsmodBool LoadImage(void*,const EdenDsmodHostApi*,const char*,void*,EdenDsmodImageSink);
}

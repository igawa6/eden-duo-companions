// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_assets.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <span>
#include <vector>
namespace re4 {
struct MapVertex { double x,y,z,mx,my,mh; };
struct MapTriangle { uint16_t a,b,c; };
struct MapMesh {
    std::string area;
    std::vector<MapVertex> vertices;
    std::vector<MapTriangle> triangles;
    bool narrow=false; double heading=0; bool affine=false; double ox=0,oz=0;
};
struct GeometryStage {
    unsigned stage=0;
    size_t bytes=0;
    std::vector<MapMesh> meshes;
};
// One immutable stage is published by the asset worker. Readers never decode or wait
// for IO. A tick may briefly retain the preceding stage across an atomic replacement.
class GeometryCache {
public:
    static constexpr size_t Limit=1u<<20;
    bool Warm(const EdenDsmodHostApi*,unsigned,const std::string& directory={});
    std::shared_ptr<const GeometryStage> Get(unsigned stage) const {
        auto p=std::atomic_load_explicit(&ready,std::memory_order_acquire);
        return p&&p->stage==stage?p:nullptr;
    }
private:
    std::mutex worker;
    std::shared_ptr<const GeometryStage> ready;
};
}

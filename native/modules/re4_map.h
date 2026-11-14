// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_map_bounds.h"
#include "re4_geometry.h"
#include <cmath>
#include <optional>
#include <array>

namespace re4 {
// Native mapPositionCheck transfers barycentric weights from the world hit mesh
// to the room's map mesh, then applies the first model's parts matrix.
// Use the native first vertical bracket; a wider native retry needs scenario state.
// No nearest-floor or global /10 fallback: a missing hit leaves the map unavailable, except
// for Separate Ways rooms whose map mesh is exactly world/10 + root (generator-checked).
inline std::optional<std::array<double,3>> MapPose(std::string_view area,
        const std::array<float,3>& position, const std::array<float,2>& facing,
        double* native_height=nullptr, std::span<const MapMesh> meshes={}) {
    const double x=position[0], y=position[1], z=position[2];
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return {};
    for(const auto& mesh:meshes)if(mesh.area==area) {
        std::optional<std::array<double,3>> result;
        double best=INFINITY;
        for(const auto& t:mesh.triangles) {
            const auto& a=mesh.vertices[t.a];
            const auto& b=mesh.vertices[t.b];
            const auto& c=mesh.vertices[t.c];
            const double bx=b.x-a.x, bz=b.z-a.z, cx=c.x-a.x, cz=c.z-a.z;
            const double det=bx*cz-bz*cx;
            if(std::abs(det)<1e-9)continue;
            const double s=((x-a.x)*cz-(z-a.z)*cx)/det;
            const double u=(bx*(z-a.z)-bz*(x-a.x))/det;
            if(s < -1e-6 || u < -1e-6 || s+u > 1+1e-6)continue;
            const double hit=a.y+s*(b.y-a.y)+u*(c.y-a.y);
            const double reach=mesh.narrow?1000.:100000.;
            if(hit<y-reach || hit>y+reach || hit>best)continue;
            best=hit;
            if(native_height)*native_height=a.mh+s*(b.mh-a.mh)+u*(c.mh-a.mh);
            result=std::array<double,3>{a.mx+s*(b.mx-a.mx)+u*(c.mx-a.mx),
                a.my+s*(b.my-a.my)+u*(c.my-a.my),
                std::atan2(facing[1],-facing[0])*180./3.14159265358979323846-mesh.heading};
        }
        // Affine SW rooms: the native on-mesh transfer is world/10 + root, so keep it off-mesh
        // as the 2026-11-08 SW contract. Rotated/distorted SW pieces never guess off-mesh.
        if(!result && mesh.affine && !native_height)
            result=std::array<double,3>{x/10.+mesh.ox,-(z/10.+mesh.oz),
                std::atan2(facing[1],-facing[0])*180./3.14159265358979323846-mesh.heading};
        return result;
    }
    return {};
}
// Affine SW rooms keep an off-bracket pose (see MapPose). Their floor then follows the hit under
// Ada that is vertically nearest: live, room 0x51E 1142 units above its hit drew that native
// floor (analysis/20261114-gallery-sw-live.md). Other rooms never take this path.
inline std::optional<double> AffineFloorHeight(std::string_view area,const std::array<float,3>& position,std::span<const MapMesh> meshes={}) {
    const double x=position[0], y=position[1], z=position[2];
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return {};
    for(const auto& mesh:meshes)if(mesh.area==area) {
        if(!mesh.affine)return {};
        std::optional<double> result;
        double best=INFINITY;
        for(const auto& t:mesh.triangles) {
            const auto& a=mesh.vertices[t.a];
            const auto& b=mesh.vertices[t.b];
            const auto& c=mesh.vertices[t.c];
            const double bx=b.x-a.x, bz=b.z-a.z, cx=c.x-a.x, cz=c.z-a.z;
            const double det=bx*cz-bz*cx;
            if(std::abs(det)<1e-9)continue;
            const double s=((x-a.x)*cz-(z-a.z)*cx)/det;
            const double u=(bx*(z-a.z)-bz*(x-a.x))/det;
            if(s < -1e-6 || u < -1e-6 || s+u > 1+1e-6)continue;
            const double gap=std::abs(a.y+s*(b.y-a.y)+u*(c.y-a.y)-y);
            if(gap>=best)continue;
            best=gap;result=a.mh+s*(b.mh-a.mh)+u*(c.mh-a.mh);
        }
        return result;
    }
    return {};
}
inline std::optional<int> MapFloor(std::string_view area,const std::array<float,3>& position,std::span<const MapMesh> meshes={}) {
    double height=0;
    if(!MapPose(area,position,{0,1},&height,meshes)) {
        const auto affine=AffineFloorHeight(area,position,meshes);
        if(!affine)return {};
        height=*affine;
    }
    const double y=height/100.;
    return static_cast<int>(y+(y>0?.5:-.5));
}
// Retain the original village's local map scale as the stage outline grows.
// Both accepted viewports are 810x738 now; Ashley's controls live in the top bar.
inline std::optional<std::array<double,4>> MapCamera(std::string_view area,
        double player_x, double player_y, bool /*escort*/) {
    if(!MapBounds(area))return {};
    constexpr double width=30950.6484375*1.06;
    constexpr double height=33846.5693359375*1.06;
    if(area.starts_with("stage5-")) {
        auto b=*MapCameraBounds(area);
        const double w=(b[2]-b[0])*.03,h=(b[3]-b[1])*.03;
        return std::array<double,4>{b[0]-w,b[1]-h,b[2]+w,b[3]+h};
    }
    return std::array<double,4>{player_x-width/2,player_y-height/2,
        player_x+width/2,player_y+height/2};
}
}

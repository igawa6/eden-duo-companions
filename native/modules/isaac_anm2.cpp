// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isaac_anm2.h"

#include "isaac_assets_xml.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace isaac_anm2 {

const Animation* File::Find(std::string_view name) const {
    for (const auto& a : anims)
        if (a.name == name)
            return &a;
    return nullptr;
}
const Layer* File::FindLayer(int id) const {
    for (const auto& l : layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
const Sheet* File::FindSheet(int id) const {
    for (const auto& s : sheets)
        if (s.id == id)
            return &s;
    return nullptr;
}

namespace {
int IntAttr(const isaac_xml::Node& n, std::string_view k, int fallback) {
    double v;
    if (!isaac_xml::AttrFloat(n, k, v) || std::abs(v) > 100000)
        return fallback;
    return static_cast<int>(std::trunc(v));
}
double FloatAttr(const isaac_xml::Node& n, std::string_view k, double fallback) {
    double v;
    return isaac_xml::AttrFloat(n, k, v) && std::abs(v) <= 100000 ? v : fallback;
}
} // namespace

bool Parse(std::string_view xml, File& out) {
    out = File{};
    isaac_xml::Node root;
    if (!isaac_xml::Parse(xml, root) || root.name != "AnimatedActor")
        return false;
    const isaac_xml::Node* content = root.Child("Content");
    const isaac_xml::Node* anims = root.Child("Animations");
    if (!content || !anims)
        return false;
    if (const auto* sheets = content->Child("Spritesheets"))
        for (const auto& s : sheets->children) {
            long long id;
            if (s->name != "Spritesheet" || !isaac_xml::AttrInt(*s, "Id", id))
                return false;
            out.sheets.push_back({static_cast<int>(id), std::string{s->AttrOr("Path")}});
        }
    if (const auto* layers = content->Child("Layers"))
        for (const auto& l : layers->children) {
            long long id, sheet;
            if (l->name != "Layer" || !isaac_xml::AttrInt(*l, "Id", id) ||
                !isaac_xml::AttrInt(*l, "SpritesheetId", sheet))
                return false;
            out.layers.push_back({static_cast<int>(id), std::string{l->AttrOr("Name")},
                                  static_cast<int>(sheet)});
        }
    out.default_anim = std::string{anims->AttrOr("DefaultAnimation")};
    for (const auto& a : anims->children) {
        if (a->name != "Animation")
            continue;
        Animation an;
        an.name = std::string{a->AttrOr("Name")};
        an.frame_num = static_cast<int>(isaac_xml::AttrIntOr(*a, "FrameNum", 0));
        if (const auto* las = a->Child("LayerAnimations"))
            for (const auto& L : las->children) {
                if (L->name != "LayerAnimation")
                    continue;
                long long lid;
                if (!isaac_xml::AttrInt(*L, "LayerId", lid))
                    return false;
                LayerAnim la;
                la.layer_id = static_cast<int>(lid);
                la.visible = L->AttrOr("Visible", "true") == "true";
                for (const auto& f : L->children) {
                    if (f->name != "Frame")
                        continue;
                    Frame fr;
                    fr.x = IntAttr(*f, "XCrop", 0);
                    fr.y = IntAttr(*f, "YCrop", 0);
                    fr.w = IntAttr(*f, "Width", 0);
                    fr.h = IntAttr(*f, "Height", 0);
                    fr.pivot_x = IntAttr(*f, "XPivot", 0);
                    fr.pivot_y = IntAttr(*f, "YPivot", 0);
                    fr.pos_x = FloatAttr(*f, "XPosition", 0);
                    fr.pos_y = FloatAttr(*f, "YPosition", 0);
                    fr.scale_x = FloatAttr(*f, "XScale", 100);
                    fr.scale_y = FloatAttr(*f, "YScale", 100);
                    fr.delay = IntAttr(*f, "Delay", 1);
                    fr.visible = f->AttrOr("Visible", "true") == "true";
                    if (fr.w < 0 || fr.h < 0 || fr.w > 4096 || fr.h > 4096 || fr.delay < 0)
                        return false;
                    la.frames.push_back(fr);
                }
                an.layers.push_back(std::move(la));
            }
        out.anims.push_back(std::move(an));
    }
    return true;
}

const Frame* FrameAt(const LayerAnim& la, int t) {
    if (la.frames.empty())
        return nullptr;
    long long acc = 0;
    for (const auto& f : la.frames) {
        if (t < acc + f.delay)
            return &f;
        acc += f.delay;
    }
    return &la.frames.back();
}

std::string NormalizePath(std::string_view path) {
    std::vector<std::string> parts;
    std::string cur;
    const auto flush = [&] {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (const char c0 : path) {
        const char c = c0 == '\\' ? '/' : c0;
        if (c == '/')
            flush();
        else
            cur.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    flush();
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out.push_back('/');
        out += p;
    }
    return out;
}

std::string SheetPath(std::string_view anm2_rel, std::string_view sheet) {
    std::string dir;
    const std::size_t slash = anm2_rel.find_last_of("/\\");
    if (slash != std::string_view::npos)
        dir = std::string{anm2_rel.substr(0, slash + 1)};
    std::string p = NormalizePath(dir + std::string{sheet});
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".png") == 0)
        p.replace(p.size() - 4, 4, ".pcx");
    return p;
}

namespace {
// AnmCache::PreloadBinary/read_frames_binary/read_nullframes_binary (AB+ NRO).
// Strings use a u16 byte count. Render frames contain 10 floats, delay, visibility,
// seven colour floats, rotation and interpolation; root/null frames have 54 bytes.
struct BinaryCursor {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at{};
    void Skip(std::size_t n) {
        if (at > bytes.size() || n > bytes.size()-at) throw std::runtime_error("animation bounds");
        at+=n;
    }
    template<class T> T Get() {
        const auto p=at;Skip(sizeof(T));T v;std::memcpy(&v,bytes.data()+p,sizeof(T));return v;
    }
    std::uint32_t Count(std::uint32_t max=4096) {
        const auto n=Get<std::uint32_t>();if(n>max)throw std::runtime_error("animation count");return n;
    }
    std::string Text() {
        const auto n=Get<std::uint16_t>();if(n>4096)throw std::runtime_error("animation string");
        const auto p=at;Skip(n);return {reinterpret_cast<const char*>(bytes.data()+p),n};
    }
    void Names(bool sheets=false,File* file=nullptr) {
        const auto n=Count();
        for(std::uint32_t i=0;i<n;++i) {
            const auto id=Get<std::uint32_t>();auto name=Text();
            if(file && sheets)file->sheets.push_back({static_cast<int>(id),std::move(name)});
        }
    }
    Frame RenderFrame() {
        float v[10];for(auto& n:v){n=Get<float>();if(!std::isfinite(n)||std::abs(n)>100000)throw std::runtime_error("animation float");}
        Frame f;f.x=static_cast<int>(v[0]);f.y=static_cast<int>(v[1]);
        f.w=static_cast<int>(v[2]);f.h=static_cast<int>(v[3]);
        // Compiled frames store position before scale and pivot after it.
        // XML lists the same fields by name; confusing these pairs drops the AB+ map inset.
        f.pos_x=v[4];f.pos_y=v[5];
        f.scale_x=v[6]*100.0;f.scale_y=v[7]*100.0;
        f.pivot_x=static_cast<int>(v[8]);f.pivot_y=static_cast<int>(v[9]);
        f.delay=Get<std::int32_t>();f.visible=Get<std::uint8_t>()!=0;
        if(f.delay<0||f.delay>100000)throw std::runtime_error("animation delay");
        Skip(7*4+4+1);return f;
    }
    File ReadFile() {
        File f;f.sheet_root=Text();Names(true,&f);
        const auto nl=Count();
        for(std::uint32_t i=0;i<nl;++i){const int id=Get<std::uint32_t>(),sheet=Get<std::uint32_t>();auto name=Text();f.layers.push_back({id,std::move(name),sheet});}
        Names();Names();f.default_anim=Text();
        const auto na=Count();
        for(std::uint32_t i=0;i<na;++i) {
            Animation a;a.name=Text();a.frame_num=Count(100000);Skip(1);
            Skip(static_cast<std::size_t>(Count(100000))*54);
            const auto n=Count();
            for(std::uint32_t j=0;j<n;++j) {
                LayerAnim la;la.layer_id=Get<std::uint32_t>();la.visible=Get<std::uint8_t>()!=0;
                const auto nf=Count(100000);
                if(nf>(bytes.size()-at)/78)throw std::runtime_error("animation frame bounds");
                la.frames.reserve(nf);
                for(std::uint32_t k=0;k<nf;++k)la.frames.push_back(RenderFrame());
                a.layers.push_back(std::move(la));
            }
            const auto nn=Count();
            for(std::uint32_t j=0;j<nn;++j){Skip(4+1);Skip(static_cast<std::size_t>(Count(100000))*54);}
            Skip(static_cast<std::size_t>(Count(100000))*8);f.anims.push_back(std::move(a));
        }
        return f;
    }
};
}

std::uint32_t BundleHash(std::string_view path) {
    std::uint32_t h=5381;
    const std::string p="resources/"+NormalizePath(path);
    for(const unsigned char c:p)h=h*33+c;
    return h;
}

bool ParseBundle(const std::vector<std::uint8_t>& bytes,
                 std::unordered_map<std::uint32_t,std::shared_ptr<const File>>& out) {
    out.clear();
    if(bytes.size()>32*1024*1024)return false;
    try {
        BinaryCursor c{bytes};const auto n=c.Count();
        for(std::uint32_t i=0;i<n;++i) {
            const auto hash=c.Get<std::uint32_t>();auto f=std::make_shared<File>(c.ReadFile());
            if(!out.emplace(hash,std::move(f)).second)throw std::runtime_error("duplicate animation");
        }
        if(c.at!=bytes.size())throw std::runtime_error("animation tail");
        return true;
    } catch(const std::exception&) {out.clear();return false;}
}

} // namespace isaac_anm2

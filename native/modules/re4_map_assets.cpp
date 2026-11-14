// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_map_assets.h"
#include "re4_map_art_tables.h"
#include "re4_map_state_tables.h"
#include <charconv>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace re4::assets {
namespace {
using Bytes=std::span<const uint8_t>;
uint64_t Int(Bytes b,size_t p,unsigned n){
    if(p>b.size()||n>b.size()-p)throw std::runtime_error("map bounds");
    uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[p+i])<<(8*i);return v;
}
double Float(Bytes b,size_t p){double v=std::bit_cast<float>(uint32_t(Int(b,p,4)));if(!std::isfinite(v)||std::abs(v)>2000000)throw std::runtime_error("map coordinate");return v;}
struct Entry{std::array<char,4> tag;Bytes bytes;};
std::vector<Entry> Container(Bytes b){
    const auto n=Int(b,0,4);if(!n||n>512||16+n*8>b.size())throw std::runtime_error("map container");
    std::vector<Entry> result;
    for(size_t i=0;i<n;++i){size_t at=Int(b,16+4*i,4),end=i+1<n?Int(b,20+4*i,4):b.size();
        if(at<16+n*8||end<at||end>b.size())throw std::runtime_error("map entry");
        Entry e{};std::memcpy(e.tag.data(),b.data()+16+4*n+4*i,4);e.bytes=b.subspan(at,end-at);result.push_back(e);
    }return result;
}
Image Blank(unsigned w,unsigned h){if(!w||!h||w>4096||h>4096||uint64_t(w)*h*4>(16u<<20))throw std::runtime_error("map dimensions");return {w,h,ByteBuffer(size_t(w)*h*4)};}
struct Vertex{double x,y,z,u=0,v=0;};
struct Model{std::vector<Vertex> vertices;std::vector<std::array<unsigned,3>> triangles;int floor=0;};
Model Parse(Bytes b,bool room,double scale=1){
    if(Int(b,0x5c,4)!=0x20030818)throw std::runtime_error("map model version");
    const auto count=Int(b,0x58,2),vertex=Int(b,0x48,8),uv=Int(b,0x18,8);
    if(!count||count>65535)throw std::runtime_error("map vertices");
    auto range=[&](size_t at,size_t bytes){if(at>b.size()||bytes>b.size()-at)throw std::runtime_error("map array bounds");};
    range(vertex,count*12);if(room)range(uv,count*8);
    double tx=0,ty=0,tz=0;
    if(room){const auto head=Int(b,0,8);range(head,16);if(Int(b,0x29,1)!=1||Int(b,head,1)!=0||Int(b,head+1,1)!=255)throw std::runtime_error("map hierarchy");tx=Float(b,head+4);ty=Float(b,head+8);tz=Float(b,head+12);}
    Model m;m.floor=int(scale*ty/100+.5);
    for(size_t i=0;i<count;++i)m.vertices.push_back({scale*(Float(b,vertex+i*12)+tx),scale*(Float(b,vertex+i*12+4)+ty),scale*(Float(b,vertex+i*12+8)+tz),room?Float(b,uv+i*8):0,room?Float(b,uv+i*8+4):0});
    size_t off=Int(b,0x30,8);unsigned base=0;
    for(unsigned g=0;g<Int(b,0x2a,2);++g){
        range(off,36);
        if(room&&Int(b,off+12,1)!=0)throw std::runtime_error("map material");
        const size_t end=off+32+Int(b,off+24,4);if(end>b.size())throw std::runtime_error("map primitive bounds");
        size_t at=off+32;const auto runs=Int(b,at,4);at+=4;if(runs>count)throw std::runtime_error("map runs");
        for(unsigned j=0;j<runs;++j){const auto mode=Int(b,at,2),n=Int(b,at+2,2);at+=4;
            if(n>count-base||at>end)throw std::runtime_error("map primitive");
            auto tri=[&](unsigned a,unsigned c,unsigned d){m.triangles.push_back({base+a,base+c,base+d});};
            if(mode==6){for(unsigned i=0;i+2<n;++i)tri(i,i+1,i+2);}
            else if(mode==4||mode==5){if(n%3)throw std::runtime_error("map triangle list");for(unsigned i=0;i<n;i+=3)tri(i,i+1,i+2);}
            else if(mode==8){if(n%4)throw std::runtime_error("map quad list");for(unsigned i=0;i<n;i+=4){tri(i,i+1,i+2);tri(i,i+2,i+3);}}
            else throw std::runtime_error("map primitive mode");base+=n;
        }off=end;
    }
    if(base!=count)throw std::runtime_error("map vertex count");return m;
}
struct Box{double x0,y0,x1,y1;unsigned w,h;};
Box Bounds(const std::vector<Model>& models,double scale){
    double x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
    for(const auto& m:models)for(const auto& v:m.vertices){x0=std::min(x0,v.x);x1=std::max(x1,v.x);y0=std::min(y0,-v.z);y1=std::max(y1,-v.z);}
    const double pad=4/scale;x0-=pad;x1+=pad;y0-=pad;y1+=pad;
    if(!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(x1)||!std::isfinite(y1))throw std::runtime_error("map box");
    const double width=std::ceil((x1-x0)*scale),height=std::ceil((y1-y0)*scale);
    if(width<1||height<1||width>4096||height>4096||width*height*4>(16u<<20))throw std::runtime_error("map box dimensions");
    const unsigned w=width,h=height;
    return {x0,y1-h/scale,x0+w/scale,y1,w,h};
}
using Colour=std::array<uint8_t,4>;
struct Palette {
    std::array<Colour,256> values{};
    std::array<bool,256> present{};
    const Colour& operator[](unsigned id)const{if(!present[id])throw std::runtime_error("missing map colour unit");return values[id];}
};
Palette Colours(Bytes b,bool endpoint=false){
    if(b.size()<8||std::memcmp(b.data(),"2.00",4))throw std::runtime_error("map colours");
    Palette result;
    for(unsigned i=0;i<Int(b,5,1);++i){const size_t at=8+i*0xa8;const unsigned id=Int(b,at+4,1);if(id==255)continue;auto& c=result.values[id];result.present[id]=true;bool animated=false;
        for(unsigned k=0;k<4;++k){c[k]=Int(b,at+0x58+k,1);animated|=Int(b,at+0x5c+k,1)!=0;}
        if(endpoint&&animated)for(unsigned k=0;k<4;++k)c[k]=std::nearbyint(.3*c[k]+.7*Int(b,at+0x5c+k,1));
    }return result;
}
void Material(Image& im,Colour c={255,255,255,255},double diffuse=.83,std::array<int,3> ambient={46,28,11}){
    ProfileScope timing(Phase::Raster);
    for(size_t p=0;p<im.pixels.size();p+=4){const double a=double(im.pixels[p+3])*c[3]/255;
        if(a==0){std::memset(im.pixels.data()+p,0,4);continue;}
        for(int k=0;k<3;++k)im.pixels[p+k]=std::nearbyint(std::min(255.,double(im.pixels[p+k])*c[k]/255*diffuse+ambient[k]));im.pixels[p+3]=std::nearbyint(a);
    }
}
Image Raster(const Model& m,const Box& box,double scale,const Image& texture){
 ProfileScope timing(Phase::Raster);
    Image canvas=Blank(box.w,box.h);AssetBuffer<double> depth(size_t(box.w)*std::min(32u,box.h));
    for(unsigned strip=0;strip<box.h;strip+=32){
    std::fill(depth.begin(),depth.end(),-INFINITY);
    for(const auto ids:m.triangles){
        const Vertex a=m.vertices[ids[0]],c=m.vertices[ids[1]],d=m.vertices[ids[2]];
        double ax=(a.x-box.x0)*scale,ay=(a.z+box.y1)*scale,cx=(c.x-box.x0)*scale,cy=(c.z+box.y1)*scale,dx=(d.x-box.x0)*scale,dy=(d.z+box.y1)*scale;
        const double den=(cy-dy)*(ax-dx)+(dx-cx)*(ay-dy);if(std::abs(den)<1e-9)continue;
        int l=std::max(0,int(std::floor(std::min({ax,cx,dx})))),r=std::min(int(box.w),int(std::ceil(std::max({ax,cx,dx})))),t=std::max(0,int(std::floor(std::min({ay,cy,dy})))),b=std::min(int(box.h),int(std::ceil(std::max({ay,cy,dy}))));
        t=std::max(t,int(strip));b=std::min(b,int(strip+32));
        for(int y=t;y<b;++y)for(int x=l;x<r;++x){const double xx=x+.5,yy=y+.5;
            double wa=((cy-dy)*(xx-dx)+(dx-cx)*(yy-dy))/den,wb=((dy-ay)*(xx-dx)+(ax-dx)*(yy-dy))/den,wc=1-wa-wb;
            const size_t p=size_t(y)*box.w+x,pd=size_t(y-strip)*box.w+x;const double height=wa*a.y+wb*c.y+wc*d.y;
            if(wa<0||wb<0||wc<0||height<depth[pd])continue;
            const int u=std::clamp(int(std::clamp((wa*a.u+wb*c.u+wc*d.u)*texture.w,0.,double(texture.w-1))),0,int(texture.w)-1),v=std::clamp(int(std::clamp((wa*a.v+wb*c.v+wc*d.v)*texture.h,0.,double(texture.h-1))),0,int(texture.h)-1);
            std::memcpy(canvas.pixels.data()+p*4,texture.pixels.data()+(size_t(v)*texture.w+u)*4,4);depth[pd]=height;
        }
    }}return canvas;
}
// Pillow's polygon API truncates input vertices to integer pixels. Fill closed
// scanline spans; horizontal edges are included independently.
Image Door(const EdenDsmodHostApi* host,unsigned angle,unsigned state,const std::string& directory){
    ProfileScope timing(Phase::Raster);
    std::vector<uint8_t> common;Image star;
    if(!Read(host,"bio4/ss/eng/ss_map.dat",common,directory)||!Texture(host,"3a000009",2,star,directory))return {};
    auto entries=Container(common);if(entries.size()<9)return {};
    Model model=Parse(entries[8].bytes,false);const double rad=angle*3.14159265358979323846/180,c=std::cos(rad),s=std::sin(rad);
    for(auto& v:model.vertices){const double x=v.x,z=v.z;v.x=x*c+z*s;v.z=-x*s+z*c;}
    auto box=Bounds({model},.15);Image mask=Blank(box.w,box.h);
    auto span=[&](int y,int l,int r){if(y<0||y>=int(box.h))return;for(int x=std::max(0,l);x<=std::min(int(box.w)-1,r);++x)mask.pixels[(size_t(y)*box.w+x)*4+3]=255;};
    for(auto ids:model.triangles){std::array<std::array<int,2>,3> v;
        for(int i=0;i<3;++i){auto p=model.vertices[ids[i]];v[i]={int((p.x-box.x0)*.15),int((p.z+box.y1)*.15)};}
        int lo=std::min({v[0][1],v[1][1],v[2][1]}),hi=std::max({v[0][1],v[1][1],v[2][1]});
        struct Edge{int x,y,lo,hi;float slope;};std::vector<Edge> edges;
        for(int i=0;i<3;++i){auto a=v[i],b=v[(i+1)%3];if(a[1]==b[1]){span(a[1],std::min(a[0],b[0]),std::max(a[0],b[0]));continue;}
            edges.push_back({a[0],a[1],std::min(a[1],b[1]),std::max(a[1],b[1]),float(b[0]-a[0])/(b[1]-a[1])});}
        auto crossing=[](const Edge& e,int y){return (y-e.y)*e.slope+e.x;};
        for(int y=std::max(0,lo);y<=std::min(int(box.h)-1,hi);++y){std::vector<float> xs;
            for(size_t i=0;i<edges.size();++i){const auto e=edges[i];if(y<e.lo||y>e.hi)continue;float x=crossing(e,y);
                if(y==e.hi&&y<hi){xs.push_back(x);}
                else if((y==e.lo||y==e.hi)&&e.slope!=0){
                    for(size_t j=0;j<i;++j){const auto other=edges[j];if((y!=other.lo&&y!=other.hi)||!other.slope||std::round(x)!=std::round(crossing(other,y)))continue;
                        const int next=y+(y==e.hi?-1:1);if(next<other.lo||next>other.hi)continue;
                        const float a=crossing(e,next),b=crossing(other,next);
                        if(x>a+1&&x>b+1)x=std::round(std::max(a,b))+1;
                        else if(x<a-1&&x<b-1)x=std::round(std::min(a,b))-1;
                        break;
                    }
                }xs.push_back(x);
            }
            std::sort(xs.begin(),xs.end());for(size_t i=1;i<xs.size();i+=2)span(y,int(std::floor(xs[i-1]+.5f)),int(std::ceil(xs[i]-.5f)));
        }
    }
    auto colours=Colours(entries[1].bytes,true);Image sample{1,1,{255,255,252,255}};Material(sample,colours[15+state],.83,{53,32,11});
    Colour shadow{};unsigned score=1000;
    for(size_t p=0;p<star.pixels.size();p+=4)if(star.pixels[p+3]==255){unsigned sum=star.pixels[p]+star.pixels[p+1]+star.pixels[p+2];if(sum<score){score=sum;std::copy_n(star.pixels.data()+p,4,shadow.begin());}}
    if(score==1000)return {};
    Image out=Blank(box.w,box.h);
    for(unsigned y=0;y<box.h;++y)for(unsigned x=0;x<box.w;++x){bool edge=false;
        for(int j=std::max(0,int(y)-2);j<=std::min(int(box.h)-1,int(y)+2);++j)for(int i=std::max(0,int(x)-2);i<=std::min(int(box.w)-1,int(x)+2);++i)edge|=mask.pixels[(size_t(j)*box.w+i)*4+3]!=0;
        auto* dest=out.pixels.data()+(size_t(y)*box.w+x)*4;
        // Palette PNGs preserve the shadow RGB underneath transparent pixels too.
        std::copy(shadow.begin(),shadow.end(),dest);dest[3]=edge?255:0;
        if(mask.pixels[(size_t(y)*box.w+x)*4+3])std::copy_n(sample.pixels.data(),4,dest);
    }return out;
}

}
bool MapImage(const EdenDsmodHostApi* host,std::string_view key,Image& out,const std::string& directory){
    out={};
    if(key.starts_with("module:re4/door/")){
        unsigned angle=999,state=0;
        const auto tail=key.substr(16);const auto slash=tail.find('/');if(slash==tail.npos)return false;
        const auto a=std::from_chars(tail.data(),tail.data()+slash,angle),b=std::from_chars(tail.data()+slash+1,tail.data()+tail.size(),state);
        if(a.ec!=std::errc{}||a.ptr!=tail.data()+slash||b.ec!=std::errc{}||b.ptr!=tail.data()+tail.size()||angle>180||state<1||state>3||tail!=std::to_string(angle)+"/"+std::to_string(state))return false;
        bool known=false;for(const auto& d:re4::MapDoorDefs)known|=angle==d.angle;
        if(!known)return false;out=Door(host,angle,state,directory);return bool(out);
    }
    for(const auto& row:MapArtRooms){
        const std::string prefix="module:re4/room/"+std::string(row.key)+"/";
        if(!key.starts_with(prefix))continue;
        const auto suffix=key.substr(prefix.size());unsigned state=0;int floor=0;
        if(suffix.size()==1&&suffix[0]>='1'&&suffix[0]<='4')state=suffix[0]-'0';
        else if(suffix.size()==4&&suffix.starts_with("4/f")&&suffix[3]>='0'&&suffix[3]<='5'){state=4;floor=suffix[3]-'0';}
        else return false;
        std::vector<uint8_t> bytes,common;Image texture;
        if(!Read(host,std::string("bio4/ss/cmn/")+row.file,bytes,directory)||!Read(host,"bio4/ss/eng/ss_map.dat",common,directory)||!Texture(host,"3a000009",27,texture,directory))return false;
        auto entries=Container(bytes);if(row.entry>=entries.size())return false;
        auto parts=Container(entries[row.entry].bytes);std::vector<Model> models;
        for(const auto& p:parts)if(p.tag==std::array<char,4>{'B','I','N',0})models.push_back(Parse(p.bytes,true,row.scale));
        if(models.empty()||models.size()>6||(models.size()==1&&suffix.size()>1))return false;
        auto common_entries=Container(common);if(common_entries.size()<2)return false;
        auto colours=Colours(common_entries[1].bytes);auto box=Bounds(models,row.ppu);out=Blank(box.w,box.h);
        std::stable_sort(models.begin(),models.end(),[](const Model& a,const Model& b){return a.floor<b.floor;});
        for(const auto& m:models){if(m.floor<0||m.floor>5)return false;
            Image layer=Raster(m,box,row.ppu,texture);
            const unsigned tint=state!=4?state:(models.size()==1||m.floor==floor?0:5+m.floor);
            Material(layer,colours[tint]);Composite(out,layer,0,0);
        }return true;
    }
    return MapSprite(host,key,out,directory);
}
}

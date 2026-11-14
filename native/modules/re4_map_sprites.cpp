// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
namespace re4::assets {
namespace {
Image Blank(unsigned w,unsigned h){return {w,h,ByteBuffer(size_t(w)*h*4)};}
Image Thumbnail(Image src,unsigned w,unsigned h){
    if(!src||src.w<=w&&src.h<=h)return src;
    const double aspect=double(src.w)/src.h;unsigned sw,sh;
    if(double(w)/h>=aspect){sh=h;double ideal=sh*aspect,lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sw=std::abs(aspect-lo/sh)<std::abs(aspect-hi/sh)?lo:hi;}
    else{sw=w;double ideal=sw/aspect,lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sh=std::abs(aspect-sw/lo)<std::abs(aspect-sw/hi)?lo:hi;}
    return Resize(src,sw,sh);
}
unsigned Lum(const uint8_t* p){return (19595u*p[0]+38470u*p[1]+7471u*p[2]+32768u)>>16;}
Image Goal(const EdenDsmodHostApi* host,const std::string& dir){
    std::vector<Image> effects;constexpr unsigned indices[]={250,350,355};
    if(!Textures(host,indices,effects,dir,"07000000"))return {};
    auto layer=[&](const char* pack,unsigned index,unsigned size,std::array<unsigned,3> colour,int ox=0,int oy=0){
        Image src;if(std::string_view(pack)=="07000000")src=effects[index==250?0:index==350?1:2];
        else if(!Texture(host,pack,index,src,dir))return Image{};src=Resize(src,size,size);
        for(size_t p=0;p<src.pixels.size();p+=4)for(int k=0;k<3;++k)src.pixels[p+k]=unsigned(src.pixels[p+k])*colour[k]/255;
        Image out=Blank(64,64);Composite(out,src,(64-size)/2+ox,(64-size)/2+oy);return out;
    };
    Image canvas=layer("3a000009",21,24,{255,255,255}),core=layer("07000000",250,14,{95,0,0});if(!canvas||!core)return {};Composite(canvas,core,0,0);
    for(unsigned index:{350,355}){
        Image glow=index==350?layer("07000000",index,21,{255,0,0},-1,-1):layer("07000000",index,64,{255,70,70});if(!glow)return {};
        const std::array<float,3> background={66/255.f,43/255.f,21/255.f};
        for(size_t p=0;p<canvas.pixels.size();p+=4){std::array<float,3> target{};float ba=canvas.pixels[p+3]/255.f,ga=glow.pixels[p+3]/255.f,alpha=ba;
            for(int k=0;k<3;++k){target[k]=std::min(1.f,background[k]*(1-ba)+canvas.pixels[p+k]/255.f*ba+glow.pixels[p+k]/255.f*ga);alpha=std::max(alpha,std::max(0.f,(target[k]-background[k])/(1-background[k])));}
            for(int k=0;k<3;++k){float rgb=alpha>0?(target[k]-background[k]*(1-alpha))/alpha:0;canvas.pixels[p+k]=std::clamp(rgb*255,0.f,255.f);}canvas.pixels[p+3]=std::clamp(alpha*255,0.f,255.f);
        }
    }
    for(size_t p=0;p<canvas.pixels.size();p+=4){canvas.pixels[p+1]=std::nearbyint(canvas.pixels[p+1]*.9);canvas.pixels[p+2]=std::nearbyint(canvas.pixels[p+2]*.69);}return canvas;
}
Image Arrow(const EdenDsmodHostApi* host,const std::string& dir){
    Image src;if(!Texture(host,"3a000009",1,src,dir))return {};src=Resize(src,64,64);
    Image base=Blank(64,64),tint=base;
    for(size_t p=0;p<base.pixels.size();p+=4){base.pixels[p]=14;base.pixels[p+1]=48;base.pixels[p+2]=42;base.pixels[p+3]=src.pixels[p+3];tint.pixels[p]=58;tint.pixels[p+1]=200;tint.pixels[p+2]=176;tint.pixels[p+3]=Lum(src.pixels.data()+p);}
    Composite(base,tint,0,0);Image out=Blank(64,64);Composite(out,base,0,0);return out;
}
Image MapFrame(const EdenDsmodHostApi* host,const std::string& dir,unsigned mode=0,std::array<unsigned,4> region={0,0,1240,1080}){
    ProfileScope timing(Phase::Raster);
    constexpr unsigned w=981,h=838;
    Image fr;ByteBuffer mask;
    if(!GetFrameGeometry(fr,mask)){
        Image source;if(!Texture(host,"3a000009",10,source,dir))return {};
        Image scaled=Resize(source,w,std::nearbyint(source.h*double(w)/source.w));if(!scaled||scaled.h>=h)return {};
        const unsigned mid=scaled.h/2;
        fr=Blank(w,h);Composite(fr,Crop(scaled,0,0,w,mid),0,0);Composite(fr,Resize(Crop(scaled,0,mid,w,1),w,h-scaled.h),0,mid);Composite(fr,Crop(scaled,0,mid,w,scaled.h-mid),0,h-(scaled.h-mid));
        source={};scaled={};
        mask.resize(size_t(w)*h);for(size_t p=0;p<mask.size();++p)mask[p]=fr.pixels[p*4+3]>60?255:0;
        const size_t seed=size_t(h/2)*w+w/2;const uint8_t original=mask[seed];
        if(original!=0)return {};
        AssetBuffer<size_t> todo{seed};mask[seed]=128;
        while(!todo.empty()){const auto p=todo.back();todo.pop_back();const unsigned x=p%w,y=p/w;
            for(auto q:{x?p-1:p,x+1<w?p+1:p,y?p-w:p,y+1<h?p+w:p})if(mask[q]==original){mask[q]=128;todo.push_back(q);}}
        AssetBuffer<size_t>().swap(todo);
        StoreFrameGeometry(fr,mask);
    }
    Image noise;if(!Texture(host,"3a000009",25,noise,dir))return {};
    Image out=Blank(region[2],region[3]),occ=Blank(w,1),fill=occ,edge=occ;
    const unsigned x0=region[0]>6?region[0]-6:0,y0=region[1]>114?region[1]-114:0;
    const unsigned x1=std::min(w,region[0]+region[2]-6),y1=std::min(h,region[1]+region[3]-114);
    for(unsigned y=y0;y<y1;++y){for(unsigned x=x0;x<x1;++x){const size_t p=size_t(x)*4;bool inside=false;
        for(unsigned j=y>2?y-2:0;j<=std::min(h-1,y+2);++j)for(unsigned i=x>2?x-2:0;i<=std::min(w-1,x+2);++i)inside|=mask[size_t(j)*w+i]==128;
        // This crop is below the header and above the nav: backdrop pixels only.
        const auto* n=noise.pixels.data()+(size_t((y+114)%noise.h)*noise.w+(x+6)%noise.w)*4;
        std::array<uint8_t,4> bright{};for(int k=0;k<3;++k)bright[k]=n[3]?int(n[k]*.55):0;const unsigned lum=Lum(bright.data());
        for(int k=0;k<3;++k){unsigned tint=lum*std::array<unsigned,3>{128,120,104}[k]/255;occ.pixels[p+k]=(tint*90+std::array<unsigned,3>{29,27,22}[k]*165+127)/255;}
        occ.pixels[p+3]=mode?255:inside?0:255;
        if(mode&&inside){
            const auto* local=noise.pixels.data()+(size_t(y%noise.h)*noise.w+x%noise.w)*4;
            for(int k=0;k<3;++k)bright[k]=local[3]?std::min(255,int(local[k]*(mode==2?1.1:1.6))):0;
            const unsigned lum=Lum(bright.data()),alpha=mode==2?60:70;
            for(int k=0;k<3;++k){unsigned tint=lum*(mode==2?std::array<unsigned,3>{90,84,72}:std::array<unsigned,3>{122,82,42})[k]/255;
                unsigned value=(tint*alpha+(mode==2?std::array<unsigned,3>{36,32,26}:std::array<unsigned,3>{80,52,24})[k]*(255-alpha)+127)/255;
                if(x%88==0||y%88==0)value=(mode==2?std::array<unsigned,3>{68,64,56}:std::array<unsigned,3>{75,49,25})[k];
                occ.pixels[p+k]=mode==2?value:std::min(255.,std::nearbyint(value*std::array<double,3>{66./56,43./37,21./18}[k]));
            }
        }
        const auto* f=fr.pixels.data()+(size_t(y)*w+x)*4;for(int k=0;k<3;++k)bright[k]=std::min(255,int(f[k]*(mode==2?1.4:2.2)));const auto shade=Lum(bright.data()),original_lum=Lum(f);
        for(int k=0;k<3;++k){fill.pixels[p+k]=shade*(mode==2?std::array<unsigned,3>{68,66,60}:std::array<unsigned,3>{106,64,24})[k]/255;edge.pixels[p+k]=(mode==2?std::array<unsigned,3>{138,132,120}:std::array<unsigned,3>{213,123,42})[k];}
        fill.pixels[p+3]=mode!=1&&original_lum<200?f[3]:0;edge.pixels[p+3]=mode!=1&&original_lum>200?f[3]:0;
    }Composite(out,occ,6-int(region[0]),y+114-int(region[1]));Composite(out,fill,6-int(region[0]),y+114-int(region[1]));Composite(out,edge,6-int(region[0]),y+114-int(region[1]));}
    return out;
}

Image FontText(const EdenDsmodHostApi* host,std::string_view text,const std::string& dir,unsigned track=0,std::array<unsigned,3> tint={229,217,207}){
    std::vector<uint8_t> fnt;Image atlas;
    if(!Read(host,"bio4/font/common_p.fnt",fnt,dir)||fnt.size()<8||!Texture(host,"0f000001",0,atlas,dir)||atlas.w!=2048||atlas.h!=768)return {};
    const size_t table=uint32_t(fnt[4])|uint32_t(fnt[5])<<8|uint32_t(fnt[6])<<16|uint32_t(fnt[7])<<24;
    std::vector<Image> glyphs;unsigned width=0;
    for(char c:text){if(c==' '){glyphs.push_back(Blank(16,64));width+=16+track;continue;}unsigned id=c>='A'&&c<='Z'?unsigned(c-'A'+39):c>='a'&&c<='z'?unsigned(c-'a'+65):0;if(!id||table+id*2+1>=fnt.size())return {};
        const int l=int8_t(fnt[table+id*2]),r=int8_t(fnt[table+id*2+1]);if(r<=l||l<0||r>64)return {};
        glyphs.push_back(Crop(atlas,id%32*64+l,id/32*64,r-l,64));width+=r-l+track;
    }
    Image out=Blank(width,64);int x=0;for(const auto& g:glyphs){Composite(out,g,x,0);x+=g.w+track;}
    for(size_t p=0;p<out.pixels.size();p+=4)for(int k=0;k<3;++k)out.pixels[p+k]=unsigned(out.pixels[p+k])*tint[k]/255;return out;
}
Image Unmapped(const EdenDsmodHostApi* host,const std::string& dir){
    Image out=MapFrame(host,dir,2);if(!out)return {};
    Image band=Blank(861,150);for(size_t p=0;p<band.pixels.size();p+=4){band.pixels[p]=20;band.pixels[p+1]=19;band.pixels[p+2]=16;band.pixels[p+3]=220;}
    Composite(out,band,66,458);
    for(unsigned y=458;y<=608;++y)for(unsigned x=66;x<=927;++x)if(y<460||y>606||x<68||x>925){
        auto* p=out.pixels.data()+(size_t(y)*out.w+x)*4;std::copy_n(std::array<uint8_t,4>{138,132,120,220}.data(),4,p);
    }
    Image message=FontText(host,"Not mapped yet",dir),sub=FontText(host,"No treasure data for this area",dir,0,{180,174,160});if(!message||!sub)return {};
    Composite(out,message,496-message.w/2,478);
    sub=Resize(sub,std::nearbyint(sub.w*.6),38);Composite(out,sub,496-sub.w/2,543);return out;
}
Image Legend(const EdenDsmodHostApi* host,const std::string& dir){
    Image out=Blank(220,64);
    // Integer spans of the code-drawn radius-12, two-pixel outline (not game geometry).
    constexpr unsigned outer[]={9,7,5,4,3,2,2,1,1,0,0,0,0},inner[]={999,999,9,7,6,5,4,3,3,2,2,2,2};
    for(unsigned y=0;y<64;++y){unsigned row=std::min({y,64-y,12u});for(unsigned x=0;x<220;++x){
        unsigned edge=std::min(x,220-x);if(edge<outer[row])continue;
        const std::array<uint8_t,4> colour=edge<inner[row]?std::array<uint8_t,4>{138,106,64,220}:std::array<uint8_t,4>{20,18,14,210};
        std::copy(colour.begin(),colour.end(),out.pixels.data()+(size_t(y)*220+x)*4);
    }}
    Image star;if(!Texture(host,"3a000009",2,star,dir))return {};
    star=Resize(star,unsigned(star.w*.75*.55),unsigned(star.h*.75*.55));Composite(out,star,int(36-star.w/2.),int(32-star.h/2.));
    Image text=FontText(host,"Treasure",dir,2);if(!text)return {};
    Image big=Blank(text.w+15,64);Composite(big,text,15,0);Image sheared=big;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<big.w;++x){double pos=x+.21*(y+.5),fraction=pos-std::floor(pos);int at=std::floor(pos);
        auto* target=sheared.pixels.data()+(size_t(y)*big.w+x)*4;std::array<unsigned,4> result{};
        for(unsigned k=0;k<4;++k){std::array<double,4> samples{};
            for(int j=-1;j<=2;++j){auto* p=big.pixels.data()+(size_t(y)*big.w+std::clamp(at+j,0,int(big.w)-1))*4;samples[j+1]=k==3?p[k]:(unsigned(p[k])*p[3]+127)/255;}
            auto a=samples[0],b=samples[1],c=samples[2],d=samples[3];
            double value=b+fraction*((-a+c)+fraction*(2*(a-b)+c-d+fraction*(-a+b-c+d)));
            result[k]=pos+.5>=big.w?0:std::clamp(value,0.,255.);
        }
        for(unsigned k=0;k<3;++k)target[k]=result[3]?std::min(255u,result[k]*255/result[3]):result[k];target[3]=result[3];
    }
    Image trimmed=Trim(sheared);if(!trimmed)return {};
    unsigned right=0;for(unsigned y=0;y<sheared.h;++y)for(unsigned x=0;x<sheared.w;++x)if(sheared.pixels[(size_t(y)*sheared.w+x)*4+3])right=std::max(right,x+1);
    text=Resize(Crop(sheared,0,0,right+1,64),std::nearbyint((right+1)*.58),37);Composite(out,text,66,13);return out;
}

}
bool MapSprite(const EdenDsmodHostApi* host,std::string_view key,Image& out,const std::string& dir){
    out={};Image src;
    if(key.starts_with("module:re4/map/frame/")){
        // Disjoint border strips exclude the transparent centre of the frame.
        constexpr unsigned boxes[][4]={{0,0,981,70},{0,70,69,663},{916,70,65,663},{0,733,981,105}};
        auto tail=key.substr(std::string_view("module:re4/map/frame/").size());
        if(tail.size()!=1||tail[0]<'0'||tail[0]>'3')return false;
        const auto& b=boxes[tail[0]-'0'];out=MapFrame(host,dir,0,{6+b[0],114+b[1],b[2],b[3]});return bool(out);
    }
    if(key=="module:re4/map/backing"){out=Crop(MapFrame(host,dir,1),6,114,981,838);return bool(out);}
    if(key=="module:re4/map/unmapped"){out=Crop(Unmapped(host,dir),6,114,981,838);return bool(out);}
    if(key=="module:re4/map/legend"){out=Legend(host,dir);return bool(out);}
    if(key=="module:re4/map/frame"){out=Crop(MapFrame(host,dir),6,114,981,838);return bool(out);}
    if(key=="module:re4/mark/arrow"){out=Arrow(host,dir);return bool(out);}
    if(key=="module:re4/mark/goal"){src=Goal(host,dir);if(!src)return false;out=Blank(64,64);Composite(out,src,0,0);return true;}
    if(key=="module:re4/mark/star"||key=="module:re4/mark/medallion"){
        if(!Texture(host,"3a000009",key.ends_with("star")?2:23,src,dir))return false;src=Resize(src,64,64);out=Blank(64,64);Composite(out,src,0,0);return true;
    }
    if(key=="module:re4/mark/save"){
        if(!Texture(host,"3a000009",20,src,dir))return false;
        for(size_t p=0;p<src.pixels.size();p+=4){if(!src.pixels[p+3]){std::memset(src.pixels.data()+p,0,4);continue;}for(int k=0;k<3;++k)src.pixels[p+k]=std::min(255,src.pixels[p+k]+std::array<int,3>{46,28,11}[k]);}
        src=Thumbnail(std::move(src),32,32);out=Blank(64,64);Composite(out,src,(64-src.w)/2,32-src.h);return true;
    }
    if(key=="module:re4/mark/atlas"){
        out=Blank(256,64);unsigned n=0;
        for(const char* mark:{"star","medallion","save","goal"}){Image part;if(!MapSprite(host,std::string("module:re4/mark/")+mark,part,dir)){out={};return false;}Composite(out,part,64*n++,0);}return true;
    }
    if(key=="module:re4/map/label/village"){
        src=FontText(host,"VILLAGE",dir);if(!src)return false;src=Resize(src,std::nearbyint(src.w*1.3),std::nearbyint(64*1.3));Image canvas=Blank(700,140);Composite(canvas,src,0,int(40-14*1.3));src=Trim(canvas);
    }else if(key=="module:re4/map/label/castle"||key=="module:re4/map/label/island"){
        if(!Texture(host,"3a000009",key.ends_with("castle")?16:17,src,dir))return false;src=Trim(src);
    }else return false;
    if(!src)return false;src=Thumbnail(std::move(src),211,30);out=Blank(227,46);Composite(out,src,(227-src.w)/2,(46-src.h)/2);return true;
}
}

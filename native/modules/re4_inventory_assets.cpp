// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_assets.h"
#include "re4_case_art.h"
#include "re4_key_sprites.h"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace re4::assets {
namespace {
using Bytes=std::span<const uint8_t>;
uint64_t Int(Bytes b,size_t p,unsigned n){if(p>b.size()||n>b.size()-p)throw std::runtime_error("item bounds");uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[p+i])<<(8*i);return v;}
double Float(Bytes b,size_t p){double f=std::bit_cast<float>(uint32_t(Int(b,p,4)));if(!std::isfinite(f)||std::abs(f)>2000000)throw std::runtime_error("item coordinate");return f;}
Image Blank(unsigned w,unsigned h){if(!w||!h||uint64_t(w)*h*4>MaxBytes)throw std::runtime_error("item dimensions");return {w,h,ByteBuffer(size_t(w)*h*4)};}
Image Thumb(Image src,unsigned w,unsigned h){
 if(!src||src.w<=w&&src.h<=h)return src;
 double aspect=double(src.w)/src.h;unsigned sw,sh;
 if(double(w)/h>=aspect){sh=h;double ideal=sh*aspect,lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sw=std::abs(aspect-lo/sh)<=std::abs(aspect-hi/sh)?lo:hi;}
 else{sw=w;double ideal=sw/aspect,lo=std::max(1.,std::floor(ideal)),hi=std::max(1.,std::ceil(ideal));sh=std::abs(aspect-sw/lo)<=std::abs(aspect-sw/hi)?lo:hi;}
 return Resize(src,sw,sh);
}
// Match the host's integer nearest-neighbour sampling at the widget's size.
Image WidgetSize(const Image& src,unsigned w,unsigned h){
 Image out=Blank(w,h);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
  std::memcpy(out.pixels.data()+(size_t(y)*w+x)*4,src.pixels.data()+(size_t(y*src.h/h)*src.w+x*src.w/w)*4,4);
 return out;
}
Image Piece(const EdenDsmodHostApi* host,unsigned id,bool placed,const std::string& dir){
 ProfileScope timing(Phase::Raster);
 std::vector<uint8_t> arc;if(!Read(host,"bio4/ss/eng/ss_pzzl.dat",arc,dir))return {};
 const unsigned mdl=id==64?33:(id==23||id==53||id==109)?53:(id>=18&&id<23||id==168)?18:(id==1||id==2||id==14)?1:(id==8||id==9||id==10)?8:(id==6||id==25||id==28)?6:(id==0||id==4||id==7||id==24||id==26||id==32||id==106||id==160)?0:id;
 const size_t start=Int(arc,(mdl*2+4)*4,4),end=Int(arc,(mdl*2+5)*4,4);
 if(end<start||end>arc.size())return {};Bytes b=Bytes(arc).subspan(start,end-start);
 const unsigned tid=id==64?33:(id==149||id==151)?149:id==109?53:id;
 const size_t to=Int(arc,(tid*2+5)*4,4);if(to>arc.size())return {};Bytes tpl=Bytes(arc).subspan(to);
 const auto ntex=Int(tpl,4,4);if(!ntex||ntex>64)return {};std::vector<Image> textures;std::vector<unsigned> indices;
 for(unsigned i=0;i<ntex;++i){auto h=Int(tpl,16+i*16,4),ref=Int(tpl,h+8,4);if(Int(tpl,ref,4)!=0x3b000001)return {};indices.push_back(Int(tpl,ref+4,4));}
 if(!Textures(host,indices,textures,dir))return {};
 struct V{float x,y,z,u,v;};std::vector<V> vs;const auto nv=Int(b,88,2),pos=Int(b,72,8),uv=Int(b,24,8);
 if(!nv)return {};float lx=INFINITY,ly=INFINITY,hx=-INFINITY,hy=-INFINITY;
 for(unsigned i=0;i<nv;++i){V v{float(Float(b,pos+i*12)),float(Float(b,pos+i*12+4)),float(Float(b,pos+i*12+8)),float(Float(b,uv+i*8)),float(Float(b,uv+i*8+4))};if(std::abs(v.u)>16||std::abs(v.v)>16)return {};lx=std::min(lx,v.x);hx=std::max(hx,v.x);ly=std::min(ly,v.y);hy=std::max(hy,v.y);vs.push_back(v);}
 if(hx<=lx||hy<=ly)return {};const unsigned cw=placed?unsigned(std::nearbyint((hx-lx)/100))*120:400,ch=placed?unsigned(std::nearbyint((hy-ly)/100))*120:200;
 if(!cw||!ch||cw>1800||ch>960)return {};const float scale=std::min(float(cw-(placed?8:20))/(hx-lx),float(ch-(placed?8:20))/(hy-ly));
 if(!std::isfinite(scale))return {};
 for(auto& v:vs){v.x=(v.x-(hx+lx)/2)*scale;v.y=(v.y-(hy+ly)/2)*scale;v.x+=cw/2.f;v.y=-v.y+ch/2.f;}
 Image out=Blank(cw,ch);AssetBuffer<double> depth(size_t(cw)*ch,-INFINITY);
 size_t off=Int(b,48,8);unsigned base=0;
 for(unsigned part=0;part<Int(b,42,2);++part){const auto mat=Int(b,off+12,1),size=Int(b,off+24,4);if(mat>=textures.size()||off+32+size>b.size())return {};const auto& t=textures[mat];bool lime=true;for(size_t p=0;p<t.pixels.size();p+=4)if(t.pixels[p]!=231||t.pixels[p+1]!=255||t.pixels[p+2]!=51){lime=false;break;}
  size_t at=off+32;const auto runs=Int(b,at,4);at+=4;if(runs>nv)return {};const size_t part_end=off+32+size;
  auto tri=[&](unsigned ia,unsigned ib,unsigned ic){if(lime)return;const auto a=vs[ia],c=vs[ib],d=vs[ic];const float den=(c.y-d.y)*(a.x-d.x)+(d.x-c.x)*(a.y-d.y);if(std::abs(den)<1e-7)return;
   const int x0=std::max(0,int(std::min({a.x,c.x,d.x}))),x1=std::min(int(cw)-1,int(std::max({a.x,c.x,d.x})+1)),y0=std::max(0,int(std::min({a.y,c.y,d.y}))),y1=std::min(int(ch)-1,int(std::max({a.y,c.y,d.y})+1));
   for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){double w0=((c.y-d.y)*(x+.5-d.x)+(d.x-c.x)*(y+.5-d.y))/den,w1=((d.y-a.y)*(x+.5-d.x)+(a.x-d.x)*(y+.5-d.y))/den,w2=1-w0-w1,z=w0*a.z+w1*c.z+w2*d.z;size_t p=size_t(y)*cw+x;if(w0<0||w1<0||w2<0||z<depth[p])continue;
    int u=std::clamp(int((w0*a.u+w1*c.u+w2*d.u)*t.w),0,int(t.w)-1),v=std::clamp(int((w0*a.v+w1*c.v+w2*d.v)*t.h),0,int(t.h)-1);std::memcpy(out.pixels.data()+p*4,t.pixels.data()+(size_t(v)*t.w+u)*4,3);out.pixels[p*4+3]=255;depth[p]=z;
   }};
  for(unsigned r=0;r<runs;++r){if(at+4>part_end)return {};auto mode=Int(b,at,2),n=Int(b,at+2,2);at+=4;if(n>nv-base)return {};if(mode==6){for(unsigned j=0;j+2<n;++j)tri(base+j,base+j+1,base+j+2);}else if(mode==4||mode==5){if(n%3)return {};for(unsigned j=0;j<n;j+=3)tri(base+j,base+j+1,base+j+2);}else if(mode==8){if(n%4)return {};for(unsigned j=0;j<n;j+=4){tri(base+j,base+j+1,base+j+2);tri(base+j,base+j+2,base+j+3);}}else return {};base+=n;}
  off+=32+size;
 }
 if(base!=nv)return {};if(!placed)return out;out=Trim(out);if(!out)return {};const auto cells=re4::CaseSize(id==160?0:id);if(!cells[0])return {};
 if(out.w>cells[0]*120||out.h>cells[1]*120)return {};
 Image canvas=Blank(cells[0]*120,cells[1]*120);Composite(canvas,out,(int(canvas.w)-int(out.w))/2,(int(canvas.h)-int(out.h))/2);return canvas;
}
}
bool InventoryImage(const EdenDsmodHostApi* host,std::string_view key,Image& out,const std::string& dir){
 out={};if(!key.starts_with("module:re4/"))return false;key.remove_prefix(11);
 auto parse=[](std::string_view s,unsigned base,unsigned& n){auto r=std::from_chars(s.data(),s.data()+s.size(),n,base);return !s.empty()&&r.ec==std::errc{}&&r.ptr==s.data()+s.size();};
 if(key.starts_with("case/prewarm/")){
  unsigned group;if(!parse(key.substr(13),10,group)||group>=8)return false;
  // Fixed materials only; eight pieces per worker turn leave font/drain gaps.
  // A failed piece does not prevent its neighbours from being prepared.
  bool ok=true;unsigned ordinal=0;
  for(unsigned id=0;id<256;++id)if(re4::CaseArtSupported(id)){
   if(ordinal++/8!=group||HasCachedCaseImage(id))continue;
   Image image=Piece(host,id,true,dir);
   if(!image||!StoreCaseImage(id,image))ok=false;
  }
  if(!ok)return false;
  out=Blank(1,1);return true;
 }
 unsigned id;Image src;
 if(key.starts_with("icon/")){
  if(!parse(key.substr(5),16,id))return false;const int index=re4::KeySprite(id);if(index<0||!Texture(host,"39000009",index,src,dir))return false;
  src=Thumb(Trim(src),245,125);if(!src)return false;out=Blank(245,125);Composite(out,src,(245-src.w)/2,(125-src.h)/2);return true;
 }
 if(key.starts_with("digit/")){if(!parse(key.substr(6),10,id)||id>9||!Texture(host,"2a000009",id,src,dir))return false;out=Resize(src,23,34);return bool(out);}
 const bool placed=key.starts_with("case/"),slot=key.starts_with("slot/"),weapon=key.starts_with("weapon/");
 if(!placed&&!slot&&!weapon)return false;key.remove_prefix(placed||slot?5:7);bool missing=key.ends_with("/missing");if(missing){if(!slot)return false;key.remove_suffix(8);}
 if(placed){if(!key.ends_with("/120"))return false;key.remove_suffix(4);}
 if(key.size()!=2||!parse(key,16,id)||id>255)return false;
 static constexpr unsigned weapons[]={3,33,64,35,37,39,41,42,44,45,46,47,48,52,53,54,55,56,148,23,109,62,82,1,2,14,65,16,71,83};
 if(!placed&&std::find(std::begin(weapons),std::end(weapons),id)==std::end(weapons))return false;
 if(placed&&CachedCaseImage(id,out))return true;
 src=Piece(host,id,placed,dir);if(!src)return false;if(slot){src=Thumb(Trim(src),180,180);if(!src)return false;out=Blank(200,200);Composite(out,src,(200-src.w)/2,(200-src.h)/2);if(missing)for(size_t p=0;p<out.pixels.size();p+=4)for(int k=0;k<3;++k)out.pixels[p+k]=int(out.pixels[p+k]*.35f);}else out=std::move(src);if(placed)StoreCaseImage(id,out);else out=WidgetSize(out,slot?163:180,slot?163:90);return bool(out);
}
}

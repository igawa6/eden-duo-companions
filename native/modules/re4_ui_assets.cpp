// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <zlib.h>
namespace re4::assets {
struct UiNode{std::string_view op;std::span<const unsigned> inputs;std::span<const double> args;std::string_view data;};
}
#include "re4_ui_recipes.h"
namespace re4::assets {
namespace {
struct Surface {Image image;unsigned channels=4;std::shared_ptr<const Image> borrowed;
 const Image& Get()const{return borrowed?*borrowed:image;}
};
Image Blank(unsigned w,unsigned h){if(!w||!h||w>4096||h>4096||uint64_t(w)*h*4>(16u<<20))throw std::runtime_error("UI dimensions");return {w,h,ByteBuffer(size_t(w)*h*4)};}
unsigned Lum(const uint8_t* p){return (19595u*p[0]+38470u*p[1]+7471u*p[2]+32768u)>>16;}
Surface Constant(const UiNode& n){
 Surface s{Blank(n.args[0],n.args[1]),unsigned(n.args[2])};ByteBuffer compressed(n.data.size()/2),bytes(size_t(s.image.w)*s.image.h*s.channels);
 auto hex=[](char c){return c<='9'?c-'0':c-'a'+10;};for(size_t i=0;i<compressed.size();++i)compressed[i]=hex(n.data[i*2])*16+hex(n.data[i*2+1]);
 uLongf size=bytes.size();if(uncompress(bytes.data(),&size,compressed.data(),compressed.size())!=Z_OK||size!=bytes.size())throw std::runtime_error("UI constant");
 for(size_t i=0;i<size_t(s.image.w)*s.image.h;++i){auto* p=s.image.pixels.data()+i*4;for(unsigned k=0;k<3;++k)p[k]=bytes[i*s.channels+(s.channels==1?0:k)];p[3]=s.channels==4?bytes[i*4+3]:255;}return s;
}
// Pillow's separable bilinear and nearest operations, including premultiplied alpha.
Image Scale(const Image& in,unsigned w,unsigned h,unsigned filter){
 if(filter==1||filter==3)return Resize(in,w,h,filter);
 if(filter==0){Image out=Blank(w,h);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned xx=std::min(in.w-1,unsigned((x+.5)*in.w/w)),yy=std::min(in.h-1,unsigned((y+.5)*in.h/h));std::memcpy(out.pixels.data()+(size_t(y)*w+x)*4,in.pixels.data()+(size_t(yy)*in.w+xx)*4,4);}return out;}
 if(filter==2)return Resize(in,w,h,filter);
 throw std::runtime_error("UI resampler");
}
Image Shear(const Image& in,const UiNode& n){
 ProfileScope timing(Phase::Raster);
 if(n.args.size()!=10||n.args[2]!=0||n.args[9]!=3)throw std::runtime_error("UI transform");
 Image out=Blank(n.args[0],n.args[1]);
 for(unsigned y=0;y<out.h;++y)for(unsigned x=0;x<out.w;++x){double sx=n.args[3]*(x+.5)+n.args[4]*(y+.5)+n.args[5],sy=n.args[6]*(x+.5)+n.args[7]*(y+.5)+n.args[8];if(sx<0||sy<0||sx>=in.w||sy>=in.h)continue;
  sx-=.5;sy-=.5;int ix=std::floor(sx),iy=std::floor(sy);double fx=sx-ix,fy=sy-iy;std::array<unsigned,4> rgba{};
  auto cubic=[](double a,double b,double c,double d,double t){return b+t*((-a+c)+t*(2*(a-b)+c-d+t*(-a+b-c+d)));};
  for(unsigned k=0;k<4;++k){std::array<double,4> rows{};for(int j=-1;j<=2;++j){std::array<double,4> v{};for(int i=-1;i<=2;++i){const auto* p=in.pixels.data()+(size_t(std::clamp(iy+j,0,int(in.h)-1))*in.w+std::clamp(ix+i,0,int(in.w)-1))*4;v[i+1]=k==3?p[k]:(unsigned(p[k])*p[3]+127)/255;}rows[j+1]=cubic(v[0],v[1],v[2],v[3],fx);}rgba[k]=std::clamp(cubic(rows[0],rows[1],rows[2],rows[3],fy),0.,255.);}
  auto* p=out.pixels.data()+(size_t(y)*out.w+x)*4;for(unsigned k=0;k<3;++k)p[k]=rgba[3]?std::min(255u,rgba[k]*255/rgba[3]):rgba[k];p[3]=rgba[3];
 }return out;
}
Surface Execute(const UiNode& n,std::vector<Surface>& values,const std::vector<unsigned>& uses,const EdenDsmodHostApi* host,const std::string& dir){
 ProfileScope timing(Phase::Compose);
 auto input=[&](unsigned at)->const Surface&{return values.at(n.inputs[at]);};auto arg=[&](unsigned at,int fallback=0){return at<n.args.size()?int(n.args[at]):fallback;};
 if(n.op=="constant")return Constant(n);
 if(n.op=="crop"&&UiNodes[n.inputs[0]].op=="texture"){
  const auto& t=UiNodes[n.inputs[0]];Surface s;char pack[9];std::snprintf(pack,sizeof(pack),"%08x",unsigned(t.args[0]));
  if(!TextureRegion(host,pack,t.args[1],s.image,arg(0),arg(1),arg(2)-arg(0),arg(3)-arg(1),dir))throw std::runtime_error("UI crop texture");return s;
 }
 if(n.op=="texture"){Surface s;char pack[9];std::snprintf(pack,sizeof(pack),"%08x",unsigned(n.args[0]));if(!Texture(host,pack,n.args[1],s.image,dir))throw std::runtime_error("UI texture");return s;}
 if(n.op=="crop")return {Crop(input(0).Get(),arg(0),arg(1),arg(2)-arg(0),arg(3)-arg(1)),input(0).channels};
 if(n.op=="resize")return {Scale(input(0).Get(),arg(0),arg(1),arg(2)),input(0).channels};
 if(n.op=="copy")return uses[n.inputs[0]]==1?std::move(values[n.inputs[0]]):input(0);
 Surface out=uses[n.inputs[0]]==1&&n.op!="merge"&&!input(0).borrowed?std::move(values[n.inputs[0]]):Surface{input(0).Get(),input(0).channels};auto& im=out.image;
 if(n.op=="transform"){im=Shear(im,n);return out;}
 if(n.op=="convert"){unsigned channels=arg(0);for(size_t p=0;p<im.pixels.size();p+=4){if(channels==1){auto l=Lum(im.pixels.data()+p);for(unsigned k=0;k<3;++k)im.pixels[p+k]=l;}if(channels!=4)im.pixels[p+3]=255;}out.channels=channels;return out;}
 if(n.op=="getchannel"){for(size_t p=0;p<im.pixels.size();p+=4){auto v=im.pixels[p+arg(0)];for(unsigned k=0;k<3;++k)im.pixels[p+k]=v;im.pixels[p+3]=255;}out.channels=1;return out;}
 if(n.op=="putalpha"){for(size_t p=0;p<im.pixels.size();p+=4)im.pixels[p+3]=n.inputs.size()>1?input(1).Get().pixels[p]:arg(0);out.channels=4;return out;}
 if(n.op=="merge"){out={Blank(input(0).Get().w,input(0).Get().h),unsigned(arg(0))};for(size_t p=0;p<out.image.pixels.size();p+=4){for(unsigned k=0;k<3;++k)out.image.pixels[p+k]=input(k).Get().pixels[p];out.image.pixels[p+3]=out.channels==4?input(3).Get().pixels[p]:255;}return out;}
 if(n.op=="alpha_composite"){Composite(im,input(1).Get(),arg(0),arg(1));return out;}
 if(n.op=="paste"||n.op=="paint"){
  if(n.inputs.size()<2)throw std::runtime_error("UI paste");const auto& src=input(1).Get();
  for(unsigned y=0;y<src.h;++y)for(unsigned x=0;x<src.w;++x){int xx=int(x)+arg(0),yy=int(y)+arg(1);if(xx<0||yy<0||xx>=im.w||yy>=im.h)continue;size_t a=(size_t(y)*src.w+x)*4,b=(size_t(yy)*im.w+xx)*4;unsigned mask=n.inputs.size()>2?input(2).Get().pixels[a]:255;for(unsigned k=0;k<4;++k)im.pixels[b+k]=(src.pixels[a+k]*mask+im.pixels[b+k]*(255-mask)+127)/255;}return out;
 }
 if(n.op=="point"){for(size_t p=0;p<im.pixels.size();p+=4)for(unsigned k=0;k<(out.channels==4?4u:3u);++k)im.pixels[p+k]=std::clamp(int(n.args[im.pixels[p+k]]),0,255);return out;}
 if(n.op=="maxfilter"){Image source=im;int r=arg(0)/2;for(unsigned y=0;y<im.h;++y)for(unsigned x=0;x<im.w;++x)for(unsigned k=0;k<3;++k){unsigned v=0;for(int j=-r;j<=r;++j)for(int i=-r;i<=r;++i)v=std::max(v,unsigned(source.pixels[(size_t(std::clamp(int(y)+j,0,int(im.h)-1))*im.w+std::clamp(int(x)+i,0,int(im.w)-1))*4+k]));im.pixels[(size_t(y)*im.w+x)*4+k]=v;}return out;}
 for(size_t p=0;p<im.pixels.size();p+=4)for(unsigned k=0;k<4;++k){unsigned a=im.pixels[p+k],b=n.inputs.size()>1?input(1).Get().pixels[p+k]:0,v=0;
  if(n.op=="composite"){unsigned m=input(2).Get().pixels[p];v=(a*m+b*(255-m)+127)/255;}
  else if(n.op=="blend"){float alpha=n.args[0];v=std::clamp(int(float(a)+alpha*(int(b)-int(a))),0,255);}
  else if(n.op=="multiply")v=a*b/255;
  else if(n.op=="invert")v=255-a;
  else if(n.op=="lighter")v=std::max(a,b);
  else if(n.op=="subtract")v=a>b?a-b:0;
  else if(n.op=="add")v=std::min(255u,a+b);
  else throw std::runtime_error("UI operation");
  im.pixels[p+k]=out.channels!=4&&k==3?255:v;
 }return out;
}
}
bool UiRecipeImage(const EdenDsmodHostApi* host,unsigned index,Image& out,const std::string& dir){
 out={};if(index>=std::size(UiNodes))return false;
 try{
  std::vector<unsigned> uses(std::size(UiNodes));std::vector<bool> needed(std::size(UiNodes));
  const bool reuse=std::find(std::begin(UiLargeOutputs),std::end(UiLargeOutputs),index)==std::end(UiLargeOutputs);
  if(!reuse)ClearUiMaterials();
  std::vector<std::shared_ptr<const Image>> cached(std::size(UiNodes));
  auto material=[&](unsigned i){return reuse&&(UiNodes[i].op=="resize"||UiNodes[i].op=="transform");};
  auto visit=[&](auto&& self,unsigned i)->void{if(needed[i])return;needed[i]=true;if(material(i)&&(cached[i]=UiMaterial(i)))return;for(auto j:UiNodes[i].inputs){if(UiNodes[i].op=="crop"&&UiNodes[j].op=="texture")continue;++uses[j];self(self,j);}};visit(visit,index);
  std::vector<Surface> values(std::size(UiNodes));for(unsigned i=0;i<=index;++i)if(needed[i]){
   if(cached[i]){values[i]={{},4,std::move(cached[i])};continue;}
   values[i]=Execute(UiNodes[i],values,uses,host,dir);if(!values[i].Get())throw std::runtime_error("UI empty");
   if(material(i)&&values[i].channels==4)values[i].borrowed=StoreUiMaterial(i,std::move(values[i].image));
   for(auto j:UiNodes[i].inputs)if(uses[j]&&!--uses[j])values[j]={};
  }
  out=values[index].borrowed?values[index].Get():std::move(values[index].image);return bool(out);
 }catch(...){out={};return false;}
}
bool UiImage(const EdenDsmodHostApi* host,std::string_view key,Image& out,const std::string& dir){
 auto found=std::find_if(std::begin(UiOutputs),std::end(UiOutputs),[&](const auto& row){return row.first==key;});
 return found!=std::end(UiOutputs)&&UiRecipeImage(host,found->second,out,dir);
}
}

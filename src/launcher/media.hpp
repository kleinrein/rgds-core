#pragma once
#include "launcher/model.hpp"
#include "launcher/asset_cache.hpp"
#include "launcher/bmp_budget.hpp"
#include "platform/sdl_dynamic.hpp"
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
namespace rgds {
inline std::map<std::string,std::string> fields(const fs::path& path) {
    std::map<std::string,std::string> result;std::ifstream file(path);std::string line;
    while(std::getline(file,line)){auto p=line.find('=');if(p!=std::string::npos)result[line.substr(0,p)]=line.substr(p+1);}
    return result;
}
inline std::string game_key(const fs::path& path) {
    uint64_t value=14695981039346656037ull;for(unsigned char c:path.string()){value^=c;value*=1099511628211ull;}
    std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<value;return out.str();
}
struct Banner { std::vector<unsigned char> pixels;std::string title; };
inline Banner read_banner(const fs::path& path) {
    Banner result;if(lower(path.extension().string())==".zip")return result;
    std::ifstream file(path,std::ios::binary);if(!file)return result;
    file.seekg(0,std::ios::end);auto length=file.tellg();if(length<0x6c)return result;
    file.seekg(0x68);unsigned char offset[4]{};if(!file.read(reinterpret_cast<char*>(offset),4))return result;
    uint32_t begin=offset[0]|(uint32_t(offset[1])<<8)|(uint32_t(offset[2])<<16)|(uint32_t(offset[3])<<24);
    if(begin<0x200 || uint64_t(begin)+0x440>static_cast<uint64_t>(length))return result;
    std::array<unsigned char,0x440> bytes{};file.seekg(begin);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return result;
    result.pixels.resize(32*32*4);
    for(int y=0;y<32;y++)for(int x=0;x<32;x++) {
        int tile=(y/8)*4+x/8,index=bytes[0x20+tile*32+(y%8)*4+(x%8)/2];index=(x%2)?index>>4:index&15;
        unsigned rgb=bytes[0x220+index*2]|(unsigned(bytes[0x221+index*2])<<8);
        auto* p=&result.pixels[(y*32+x)*4];p[0]=(rgb&31)*255/31;p[1]=((rgb>>5)&31)*255/31;p[2]=((rgb>>10)&31)*255/31;p[3]=index?255:0;
    }
    for(int i=0x340;i<0x440;i+=2) {
        unsigned cp=bytes[i]|(unsigned(bytes[i+1])<<8);if(!cp)break;
        if(cp=='\n')result.title+=' ';else if(cp<128)result.title+=static_cast<char>(cp);
        else if(cp<2048){result.title+=static_cast<char>(0xc0|(cp>>6));result.title+=static_cast<char>(0x80|(cp&63));}
        else if(cp<0xd800 || cp>0xdfff){result.title+=static_cast<char>(0xe0|(cp>>12));result.title+=static_cast<char>(0x80|((cp>>6)&63));result.title+=static_cast<char>(0x80|(cp&63));}
    }return result;
}
class Media {
    Sdl& s;
    struct CachedBanner {Banner value;AssetStamp stamp;uint64_t used=0;};
    std::map<std::string,CachedBanner> banners;
    struct Image { SDL_Texture* texture=nullptr;int w=0,h=0;fs::path path;bool icon=false;AssetStamp stamp;uint64_t used=0,bytes=0; };
    std::map<std::pair<SDL_Renderer*,std::string>,Image> images;
    uint64_t clock=0,texture_bytes=0;
    static constexpr size_t image_limit=64,banner_limit=128;
    static constexpr uint64_t byte_limit=16*1024*1024;
    AssetStamp stamp(const fs::path& path){counters.stamp_checks++;return asset_stamp(path);}
    void discard(decltype(images)::iterator entry){if(entry->second.texture)s.DestroyTexture(entry->second.texture);texture_bytes-=entry->second.bytes;images.erase(entry);}
    void trim(size_t count,uint64_t bytes=0) {
        while(!images.empty() && (images.size()>count || texture_bytes+bytes>byte_limit)){
            auto oldest=std::min_element(images.begin(),images.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});discard(oldest);counters.evictions++;
        }
    }
public:
    CacheCounters counters;
    uint64_t banner_reads=0,image_reads=0;
    Media(Sdl& api):s(api){}~Media(){clear();}
    void clear(){for(auto entry:images)if(entry.second.texture)s.DestroyTexture(entry.second.texture);images.clear();banners.clear();texture_bytes=0;}
    size_t image_count() const{return images.size();}
    size_t banner_count() const{return banners.size();}
    uint64_t bytes() const{return texture_bytes;}
    bool prepared(SDL_Renderer* r,const fs::path& path) const{return images.count(std::make_pair(r,"art:"+path.string()))!=0;}
    Banner& banner(const fs::path& path){
        counters.lookups++;auto key=path.string();auto found=banners.find(key);
        if(found==banners.end()){
            if(banners.size()>=banner_limit){auto oldest=std::min_element(banners.begin(),banners.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});banners.erase(oldest);counters.evictions++;}
            CachedBanner value;value.stamp=stamp(path);
            if(value.stamp.present && lower(path.extension().string())!=".zip"){banner_reads++;counters.file_reads++;value.value=read_banner(path);}
            found=banners.emplace(std::move(key),std::move(value)).first;
        }else counters.hits++;
        found->second.used=++clock;return found->second.value;
    }
    // Loading happens before rendering. Missing/invalid files are cached too;
    // refresh() allows newly created or replaced artwork to become visible.
    bool warm(SDL_Renderer* r,const fs::path& path,bool icon=false) {
        counters.lookups++;auto key=std::make_pair(r,(icon?"icon:":"art:")+path.string());auto found=images.find(key);
        if(found!=images.end()){counters.hits++;found->second.used=++clock;return found->second.texture!=nullptr;}
        Image image;image.path=path;image.icon=icon;image.used=++clock;image.stamp=stamp(path);
        if(image.stamp.present) {
            SDL_Surface* surface=nullptr;
            if(icon) {auto& b=banner(path);if(!b.pixels.empty()){surface=s.CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);
                if(surface)for(int y=0;y<32;y++)std::copy_n(b.pixels.data()+y*32*4,32*4,static_cast<unsigned char*>(surface->pixels)+y*surface->pitch);}
            }else if(image.stamp.size<=32*1024*1024){
                image_reads++;counters.file_reads++;auto* file=s.RWFromFile(path.c_str(),"rb");
                if(file){if(bmp_within_budget(file,byte_limit))surface=s.LoadBMP_RW(file,1);else file->close(file);}
            }
            if(surface){
                uint64_t bytes=surface->w>0 && surface->h>0?uint64_t(surface->w)*surface->h*4:0;
                if(bytes && bytes<=byte_limit){
                    trim(image_limit-1,bytes);image.w=surface->w;image.h=surface->h;
                    s.SetHint("SDL_RENDER_SCALE_QUALITY",icon || path.filename()=="icon.bmp"?"0":"1");
                    image.texture=s.CreateTextureFromSurface(r,surface);if(image.texture){image.bytes=bytes;texture_bytes+=bytes;s.SetTextureBlendMode(image.texture,SDL_BLENDMODE_BLEND);}
                }s.FreeSurface(surface);
            }
        }
        trim(image_limit-1);bool ready=image.texture!=nullptr;images.emplace(std::move(key),std::move(image));return ready;
    }
    bool refresh() {
        bool changed=false;std::map<std::string,AssetStamp> versions;
        auto version=[&](const fs::path& path){auto key=path.string();auto found=versions.find(key);if(found==versions.end())found=versions.emplace(key,stamp(path)).first;return found->second;};
        for(auto it=images.begin();it!=images.end();){if(version(it->second.path)!=it->second.stamp){auto old=it++;discard(old);changed=true;counters.invalidations++;}else ++it;}
        for(auto it=banners.begin();it!=banners.end();){if(version(it->first)!=it->second.stamp){it=banners.erase(it);changed=true;counters.invalidations++;}else ++it;}
        return changed;
    }
    SDL_Rect fitted_box(SDL_Renderer* r,const fs::path& path,SDL_Rect box) const {
        auto found=images.find(std::make_pair(r,"art:"+path.string()));
        if(found==images.end() || !found->second.texture)return box;
        const auto& image=found->second;double scale=std::min(double(box.w)/image.w,double(box.h)/image.h);
        int width=std::max(1,int(std::lround(image.w*scale))),height=std::max(1,int(std::lround(image.h*scale)));
        return {box.x+(box.w-width)/2,box.y+(box.h-height)/2,width,height};
    }
    bool draw(SDL_Renderer* r,const fs::path& path,SDL_Rect box,bool icon=false,unsigned char alpha=255,bool fill=false,int radius=0) {
        counters.lookups++;auto found=images.find(std::make_pair(r,(icon?"icon:":"art:")+path.string()));if(found==images.end())return false;
        counters.hits++;auto& image=found->second;image.used=++clock;if(!image.texture)return false;
        SDL_Rect source{0,0,image.w,image.h},destination=box;
        if(fill){
            // Center-crop to fill the frame without stretching or letterboxing.
            double scale=std::max(double(box.w)/image.w,double(box.h)/image.h);
            int width=std::clamp(int(std::lround(box.w/scale)),1,image.w),height=std::clamp(int(std::lround(box.h/scale)),1,image.h);
            source={(image.w-width)/2,(image.h-height)/2,width,height};
        }else{
            double scale=std::min(double(box.w)/image.w,double(box.h)/image.h);
            int width=std::max(1,int(std::lround(image.w*scale))),height=std::max(1,int(std::lround(image.h*scale)));
            destination={box.x+(box.w-width)/2,box.y+(box.h-height)/2,width,height};
        }
        s.SetTextureAlphaMod(image.texture,alpha);
        radius=std::clamp(radius,0,std::min(destination.w,destination.h)/2);
        if(!radius){s.RenderCopy(r,image.texture,&source,&destination);return true;}
        // Draw rounded strips inside the parent's clip. The software renderer
        // can discard one-pixel scissor regions after logical-size scaling.
        auto strip=[&](SDL_Rect clip){
            if(clip.w<=0 || clip.h<=0)return;
            int left=(clip.x-destination.x)*source.w/destination.w,top=(clip.y-destination.y)*source.h/destination.h;
            int right=(clip.x+clip.w-destination.x)*source.w/destination.w,bottom=(clip.y+clip.h-destination.y)*source.h/destination.h;
            SDL_Rect sample{source.x+left,source.y+top,std::max(1,right-left),std::max(1,bottom-top)};
            s.RenderCopy(r,image.texture,&sample,&clip);
        };
        strip({destination.x,destination.y+radius,destination.w,destination.h-radius*2});
        for(int dy=0;dy<radius;dy++){
            int inset=radius-int(std::sqrt(radius*radius-(radius-dy-.5)*(radius-dy-.5)));
            strip({destination.x+inset,destination.y+dy,destination.w-inset*2,1});
            strip({destination.x+inset,destination.y+destination.h-1-dy,destination.w-inset*2,1});
        }
        return true;
    }
};
}

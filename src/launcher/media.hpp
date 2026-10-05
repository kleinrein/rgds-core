#pragma once
#include "launcher/model.hpp"
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
    Sdl& s;std::map<std::string,Banner> banners;
    struct Image { SDL_Texture* texture=nullptr;int w=0,h=0;fs::file_time_type stamp{}; };
    std::map<std::pair<SDL_Renderer*,std::string>,Image> images;
public:
    Media(Sdl& api):s(api){}~Media(){clear();}
    void clear(){for(auto entry:images)if(entry.second.texture)s.DestroyTexture(entry.second.texture);images.clear();}
    Banner& banner(const fs::path& path){auto key=path.string();auto found=banners.find(key);if(found==banners.end())found=banners.emplace(key,read_banner(path)).first;return found->second;}
    bool draw(SDL_Renderer* r,const fs::path& path,SDL_Rect box,bool icon=false,unsigned char alpha=255) {
        auto key=std::make_pair(r,(icon?"icon:":"art:")+path.string());auto& image=images[key];
        std::error_code ec;auto stamp=fs::last_write_time(path,ec);
        if(image.texture && !icon && !ec && stamp!=image.stamp){s.DestroyTexture(image.texture);image.texture=nullptr;}
        if(!image.texture) {
            SDL_Surface* surface=nullptr;
            if(icon) {auto& b=banner(path);if(b.pixels.empty())return false;surface=s.CreateRGBSurfaceWithFormat(0,32,32,32,SDL_PIXELFORMAT_RGBA32);
                if(surface)for(int y=0;y<32;y++)std::copy_n(b.pixels.data()+y*32*4,32*4,static_cast<unsigned char*>(surface->pixels)+y*surface->pitch);
            }else if(!ec){auto* file=s.RWFromFile(path.c_str(),"rb");if(file)surface=s.LoadBMP_RW(file,1);}
            if(!surface)return false;image.w=surface->w;image.h=surface->h;image.stamp=stamp;
            s.SetHint("SDL_RENDER_SCALE_QUALITY",icon || path.filename()=="icon.bmp"?"0":"1");
            image.texture=s.CreateTextureFromSurface(r,surface);s.FreeSurface(surface);if(!image.texture)return false;s.SetTextureBlendMode(image.texture,SDL_BLENDMODE_BLEND);
        }
        double scale=std::min(double(box.w)/image.w,double(box.h)/image.h);
        SDL_Rect destination{box.x+(box.w-int(image.w*scale))/2,box.y+(box.h-int(image.h*scale))/2,int(image.w*scale),int(image.h*scale)};
        s.SetTextureAlphaMod(image.texture,alpha);s.RenderCopy(r,image.texture,nullptr,&destination);return true;
    }
};
}

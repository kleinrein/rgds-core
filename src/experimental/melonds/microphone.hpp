#pragma once
#include <cstdint>
#include <cstdio>
#ifdef __linux__
#include <dlfcn.h>
#include <cerrno>
#endif
// Direct ALSA avoids a desktop PulseAudio default on the stock handheld.
class CaptureMic {
    void* library=nullptr;void* pcm=nullptr;
    long (*readi)(void*,void*,unsigned long)=nullptr;
    int (*recover)(void*,int,int)=nullptr;int (*close_pcm)(void*)=nullptr;
public:
    ~CaptureMic(){close();}
    bool open(unsigned rate){
#ifdef __linux__
        if(pcm)return true;
        if(!library)library=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);
        if(!library)return false;
        auto start=reinterpret_cast<int(*)(void**,const char*,int,int)>(dlsym(library,"snd_pcm_open"));
        auto params=reinterpret_cast<int(*)(void*,int,int,unsigned,unsigned,int,unsigned)>(dlsym(library,"snd_pcm_set_params"));
        readi=reinterpret_cast<long(*)(void*,void*,unsigned long)>(dlsym(library,"snd_pcm_readi"));
        recover=reinterpret_cast<int(*)(void*,int,int)>(dlsym(library,"snd_pcm_recover"));
        close_pcm=reinterpret_cast<int(*)(void*)>(dlsym(library,"snd_pcm_close"));
        if(!start || !params || !readi || !recover || !close_pcm)return false;
        if(start(&pcm,"plughw:0,0",1,1)<0){pcm=nullptr;return false;}
        if(params(pcm,2,3,1,rate,1,100000)<0){close();return false;}
        std::fprintf(stderr,"RGDS microphone: ALSA capture at %u Hz\n",rate);return true;
#else
        (void)rate;return false;
#endif
    }
    bool active()const{return pcm;}
    void close(){if(pcm && close_pcm)close_pcm(pcm);pcm=nullptr;}
    int read(int16_t* data,int count){
#ifdef __linux__
        if(!pcm || !data || count<=0)return 0;
        auto n=readi(pcm,data,static_cast<unsigned long>(count));
        if(n<0 && n!=-EAGAIN)recover(pcm,int(n),1);
        return n>0?int(n):0;
#else
        (void)data;(void)count;return 0;
#endif
    }
};

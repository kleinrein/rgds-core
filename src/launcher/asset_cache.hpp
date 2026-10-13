#pragma once
#include "launcher/model.hpp"
#include <cstdint>
#include <sys/stat.h>

namespace rgds {
// A single stat detects disappearance and atomic replacements, including files
// copied with their previous modification time. Call only during cache refresh.
struct AssetStamp {
    bool present=false;
    uint64_t device=0,inode=0,size=0;
    int64_t seconds=0,nanos=0;
    bool operator==(const AssetStamp& other) const {
        return present==other.present && device==other.device && inode==other.inode && size==other.size && seconds==other.seconds && nanos==other.nanos;
    }
    bool operator!=(const AssetStamp& other) const {return !(*this==other);}
};
inline AssetStamp asset_stamp(const fs::path& path) {
    struct stat value{};if(::stat(path.c_str(),&value)!=0)return {};
    AssetStamp result;result.present=true;result.device=value.st_dev;result.inode=value.st_ino;result.size=value.st_size;
#ifdef __APPLE__
    result.seconds=value.st_mtimespec.tv_sec;result.nanos=value.st_mtimespec.tv_nsec;
#else
    result.seconds=value.st_mtim.tv_sec;result.nanos=value.st_mtim.tv_nsec;
#endif
    return result;
}
struct CacheCounters {uint64_t lookups=0,hits=0,file_reads=0,stamp_checks=0,evictions=0,invalidations=0;};
}

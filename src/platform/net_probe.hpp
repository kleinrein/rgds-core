#pragma once
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#ifdef __linux__
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace rgds {
// A cheap kernel view of the network: link state, IPv4 addresses and Wi-Fi
// quality. The launcher refreshes the NetworkManager-backed status helper
// (a Python process plus several nmcli calls, ~0.75 s of CPU) only when this
// signature changes, instead of every fifteen seconds.
struct NetworkProbe { std::string signature; int signal=-1; };

inline NetworkProbe probe_network(const std::string& proc="/proc/net/wireless"){
    NetworkProbe result;
#ifdef __linux__
    std::ifstream wireless(proc);std::string line;
    while(std::getline(wireless,line)){
        auto colon=line.find(':');if(colon==std::string::npos)continue;
        auto name=line.substr(0,colon);name.erase(0,name.find_first_not_of(' '));
        if(name.empty() || name.rfind("p2p",0)==0)continue;
        std::istringstream fields(line.substr(colon+1));std::string status;double link=0;
        if(fields>>status>>link){result.signal=std::clamp(int(link),0,100);break;}
    }
    ifaddrs* list=nullptr;
    if(getifaddrs(&list)==0){
        for(auto* item=list;item;item=item->ifa_next){
            if(!item->ifa_name || (item->ifa_flags&IFF_LOOPBACK))continue;
            result.signature+=std::string(item->ifa_name)+((item->ifa_flags&IFF_RUNNING)?"+":"-");
            if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET){
                char text[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET,&reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr,text,sizeof(text));
                result.signature+=std::string("=")+text;
            }
            result.signature+=';';
        }
        freeifaddrs(list);
    }
#else
    (void)proc;
#endif
    return result;
}
}

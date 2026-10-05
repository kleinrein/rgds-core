#pragma once
#include "launcher/model.hpp"
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <signal.h>
#endif

namespace rgds {
struct SystemStatus {
    bool brightness=false,power=false,sleep=false,lid=false,charging=false;
    int brightness_percent=60,battery=-1;
};
class SystemControls {
    bool preview;
    fs::path app,data;
    int invoke(const std::string& command,std::optional<int> value,std::string* output=nullptr) {
        if(preview)return 0;
        if(app.empty() || data.empty())return 1;
        std::vector<std::string> args={"python3","-B",(app/"runtime/system_controls.py").string(),command,"--data",data.string()};
        if(value)args.push_back(std::to_string(*value));
        int pipe_fd[2];if(pipe(pipe_fd)!=0)return 1;
        const auto parent=getpid();auto child=fork();
        if(child==0) {
#ifdef __linux__
            // Helpers must not outlive a killed launcher and race guardian recovery.
            if(prctl(PR_SET_PDEATHSIG,SIGTERM)!=0 || getppid()!=parent)_exit(127);
#else
            (void)parent;
#endif
            close(pipe_fd[0]);dup2(pipe_fd[1],STDOUT_FILENO);close(pipe_fd[1]);
            std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
            execvp(argv[0],argv.data());_exit(127);
        }
        close(pipe_fd[1]);std::string result;char buffer[512];ssize_t count;
        while((count=read(pipe_fd[0],buffer,sizeof(buffer)))!=0){if(count>0)result.append(buffer,count);else if(errno!=EINTR)break;}
        close(pipe_fd[0]);if(child<0)return 1;
        int status=0;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return 1;}
        if(output)*output=result;
        return WIFEXITED(status)?WEXITSTATUS(status):1;
    }
public:
    SystemControls(bool fake,fs::path application,fs::path userdata):preview(fake),app(std::move(application)),data(std::move(userdata)){}
    ~SystemControls(){if(!preview && !std::getenv("RGDS_SYSTEM_MANAGED"))invoke("restore",std::nullopt);}
    SystemStatus status() {
        if(preview)return {true,true,true,true,false,60,82};
        SystemStatus status;std::string output;if(invoke("status",std::nullopt,&output)!=0)return status;
        std::istringstream stream(output);std::string line;
        while(std::getline(stream,line)) {
            auto separator=line.find('=');if(separator==std::string::npos)continue;
            const auto key=line.substr(0,separator);int value=std::atoi(line.substr(separator+1).c_str());
            if(key=="brightness_supported")status.brightness=value==1;
            else if(key=="power_supported")status.power=value==1;
            else if(key=="sleep_supported")status.sleep=value==1;
            else if(key=="lid_supported")status.lid=value==1;
            else if(key=="brightness")status.brightness_percent=std::clamp(value,5,100);
            else if(key=="battery")status.battery=value>=0 && value<=100?value:-1;
            else if(key=="charging")status.charging=value==1;
        }
        return status;
    }
    bool set(const std::string& command,int value){return invoke(command,value)==0;}
    bool sleep(){return invoke("sleep",std::nullopt)==0;}
    std::optional<bool> lid_closed() {
        if(preview)return std::nullopt;
        std::ifstream file("/sys/class/anbernic_misc/hallkey");int value;
        if(file>>value && (value==0 || value==1))return value==0;return std::nullopt;
    }
};
}

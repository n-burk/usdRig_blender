#include "translator.h"
#include "config.h"
#include "pxr/base/plug/plugin.h"
#include "pxr/base/plug/registry.h"
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdBlenderRig {
namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::random_device random;
        for(int i=0;i<100;++i) {
            path=std::filesystem::temp_directory_path()/
                ("usdBlenderRig-"+std::to_string(random())+"-"+std::to_string(random()));
            std::error_code error;
            if(std::filesystem::create_directory(path,error)) {
#ifndef _WIN32
                std::filesystem::permissions(path,std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace);
#endif
                return;
            }
        }
        throw std::runtime_error("cannot create private Blender extraction directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path,error); }
};
std::string ReadFile(const std::filesystem::path &path,size_t limit) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("missing Blender extraction output: "+path.string());
    const auto size=std::filesystem::file_size(path);
    if(size>limit) throw std::runtime_error("Blender extraction output exceeds size limit");
    std::string result(static_cast<size_t>(size),'\0');
    if(size && !input.read(result.data(),static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read Blender extraction output");
    return result;
}
void Run(const std::vector<std::string> &args,const std::filesystem::path &log) {
#ifdef _WIN32
    // Windows argv quoting; never invoke cmd.exe or a shell.
    auto quote=[](const std::string &s) {
        std::string out="\""; size_t slashes=0;
        for(char c:s) {
            if(c=='\\') { ++slashes; continue; }
            out.append(slashes*(c=='\"' ? 2 : 1),'\\'); slashes=0;
            if(c=='\"') out+='\\'; out+=c;
        }
        out.append(slashes*2,'\\'); return out+'\"';
    };
    std::string command;
    for(const auto &arg:args) { if(!command.empty()) command+=' '; command+=quote(arg); }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE output=CreateFileA(log.string().c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,
                              CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create Blender log");
    STARTUPINFOA startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=output; startup.hStdError=output;
    startup.hStdInput=CreateFileA("NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                &security,OPEN_EXISTING,0,nullptr);
    PROCESS_INFORMATION process{};
    BOOL started=CreateProcessA(nullptr,command.data(),nullptr,nullptr,TRUE,
                               CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
    CloseHandle(output); CloseHandle(startup.hStdInput);
    if(!started) throw std::runtime_error("cannot launch Blender; set USDBLENDERRIG_BLENDER");
    DWORD waited=WaitForSingleObject(process.hProcess,120000),code=1;
    if(waited!=WAIT_OBJECT_0) { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,INFINITE); }
    else GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if(waited!=WAIT_OBJECT_0) throw std::runtime_error("Blender extraction timed out after 120 seconds");
#else
    std::vector<char *> argv;
    for(const auto &arg:args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,log.string().c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO);
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes,0);
    pid_t pid=0;
    const int error=posix_spawnp(&pid,argv[0],&actions,&attributes,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    if(error) throw std::runtime_error("cannot launch Blender; set USDBLENDERRIG_BLENDER to its executable (error "+std::to_string(error)+")");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    int status=0;
    for(;;) {
        const pid_t result=waitpid(pid,&status,WNOHANG);
        if(result==pid) break;
        if(result<0 && errno!=EINTR) throw std::runtime_error("cannot wait for Blender");
        if(std::chrono::steady_clock::now()>=deadline) {
            kill(-pid,SIGKILL); while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
            throw std::runtime_error("Blender extraction timed out after 120 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int code=WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    if(code) {
        std::string detail;
        try { detail=ReadFile(log,1024*1024); } catch(...) {}
        if(detail.size()>4096) detail=detail.substr(detail.size()-4096);
        throw std::runtime_error("Blender extraction failed (exit "+std::to_string(code)+"): "+detail);
    }
}
}
std::string Extract(const std::string &source) {
    if(!std::filesystem::is_regular_file(source))
        throw std::runtime_error(".blend extraction requires a resolved local file");
    auto plugin=PlugRegistry::GetInstance().GetPluginWithName("usdBlenderRig");
    if(!plugin) throw std::runtime_error("usdBlenderRig plugin resources are unavailable");
    const auto script=std::filesystem::path(plugin->GetResourcePath())/"extract.py";
    if(!std::filesystem::is_regular_file(script)) throw std::runtime_error("missing extract.py resource");
    TemporaryDirectory temp;
    const auto output=temp.path/"scene.blendrig";
    const char *overridePath=std::getenv("USDBLENDERRIG_BLENDER");
    const std::string executable=overridePath && *overridePath ? overridePath : USDBLENDERRIG_DEFAULT_BLENDER;
    Run({executable,"--background","--factory-startup","--disable-autoexec",
        "--python-exit-code","1","--python",script.string(),"--",
        std::filesystem::absolute(source).string(),output.string()},temp.path/"blender.log");
    return ReadFile(output,512*1024*1024);
}
}

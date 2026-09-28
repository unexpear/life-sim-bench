#pragma once
#include "sim.hpp"
#include "file_paths.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <set>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#endif

namespace bench::projects {
namespace fs=std::filesystem;
inline std::string utf8(const fs::path& p) {return path_text(p);}
struct Document {
    std::string name,model,source,dll,rule,body;
    std::vector<std::pair<std::string,float>> knobs;
    std::vector<std::pair<std::string,bool>> switches;
};
inline std::string token() {
    static std::atomic<unsigned> serial{0};
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++);
}
inline bool atomic_write(const fs::path& path,const std::string& bytes,std::string& error) {
    const auto temporary=path.parent_path()/path_from_utf8(utf8(path.filename())+".tmp-"+token());
    try {
        if(!path.parent_path().empty())fs::create_directories(path.parent_path());
        {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out.write(bytes.data(),std::streamsize(bytes.size()));out.close();
         if(!out)throw std::runtime_error("Could not finish writing the simulation file.");}
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace the simulation file (Windows error "+std::to_string(GetLastError())+").");
#else
        fs::rename(temporary,path);
#endif
        return true;
    } catch(const std::exception& e) {error=e.what();std::error_code ec;fs::remove(temporary,ec);return false;}
}
inline std::string encode(const Document& d) {
    std::ostringstream out;out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"LIFE_SIM_PROJECT 1\nname "<<std::quoted(d.name)<<"\nmodel "<<std::quoted(d.model)
       <<"\nsource "<<std::quoted(d.source)<<"\ndll "<<std::quoted(d.dll)<<"\nrule "<<std::quoted(d.rule)
       <<"\nbody "<<std::quoted(d.body)<<'\n';
    for(const auto& [key,value]:d.knobs)out<<"knob "<<std::quoted(key)<<' '<<value<<'\n';
    for(const auto& [key,value]:d.switches)out<<"switch "<<std::quoted(key)<<' '<<value<<'\n';
    out<<"end\n";return out.str();
}
inline bool read(const fs::path& path,Document& result,std::string& error) {
    try {
        if(fs::file_size(path)>4*1024*1024)throw std::runtime_error("Simulation file is too large.");
        std::ifstream file(path,std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)),{});
        if(!file||bytes.find('\0')!=std::string::npos)throw std::runtime_error("Could not read this simulation file.");
        std::istringstream in(bytes);std::string key,magic;int version=0;
        if(!(in>>magic>>version)||magic!="LIFE_SIM_PROJECT"||version!=1)
            throw std::runtime_error("This is not a supported .benchsim simulation file.");
        Document d;std::set<std::string> seen,knobs,switches;bool ended=false;
        while(in>>key) {
            if(key=="end") {ended=true;break;}
            if(key=="knob") {
                std::string name;float value=0;in>>std::quoted(name)>>value;
                if(!in||name.empty()||!std::isfinite(value)||!knobs.insert(name).second||knobs.size()>1024)
                    throw std::runtime_error("Invalid or repeated simulation setting.");
                d.knobs.emplace_back(name,value);continue;
            }
            if(key=="switch") {
                std::string name;int value=0;in>>std::quoted(name)>>value;
                if(!in||name.empty()||(value!=0&&value!=1)||!switches.insert(name).second||switches.size()>1024)
                    throw std::runtime_error("Invalid or repeated simulation switch.");
                d.switches.emplace_back(name,value!=0);continue;
            }
            if(!seen.insert(key).second)throw std::runtime_error("Repeated simulation field.");
            std::string* value=nullptr;
            if(key=="name")value=&d.name;else if(key=="model")value=&d.model;
            else if(key=="source")value=&d.source;else if(key=="dll")value=&d.dll;
            else if(key=="rule")value=&d.rule;else if(key=="body")value=&d.body;
            else throw std::runtime_error("Unknown simulation field: "+key);
            if(!(in>>std::quoted(*value)))throw std::runtime_error("Incomplete simulation file.");
        }
        if(!ended||(in>>key)||d.name.empty()||d.name.size()>240||
           (d.model.empty()?(d.source.empty()&&d.dll.empty()):(!d.source.empty()||!d.dll.empty())))
            throw std::runtime_error("Incomplete or conflicting simulation definition.");
        result=std::move(d);return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
inline fs::path resolve(const fs::path& project,const std::string& file) {
    if(file.empty())return {};
    const auto path=bench::path_from_utf8(file);return path.is_absolute()?path:(project.parent_path()/path).lexically_normal();
}
inline bool save(const fs::path& path,Document d,std::string& error) {
    // Source and its runnable DLL live beside the document, independently of
    // the install directory and of the file originally imported by the user.
    try {
        if(d.model.empty()) {
            const auto source=bench::path_from_utf8(d.source),dll=bench::path_from_utf8(d.dll);
            const auto owner=!source.empty()?source.parent_path():dll.parent_path();
            const auto prefix=utf8(path.stem())+".assets-";
            const bool alreadyOwned=owner.parent_path()==path.parent_path()&&utf8(owner.filename()).starts_with(prefix);
            if(alreadyOwned) {
                if((source.empty()||!fs::is_regular_file(source))&&(dll.empty()||!fs::is_regular_file(dll)))
                    throw std::runtime_error("The custom simulation's source and DLL are missing.");
                if(!source.empty())d.source=utf8(source.lexically_relative(path.parent_path()));
                if(!dll.empty())d.dll=utf8(dll.lexically_relative(path.parent_path()));
            } else {
                const auto assets=path.parent_path()/bench::path_from_utf8(prefix+token());fs::create_directories(assets);
                const auto copiedSource=assets/"simulation.cpp",copiedDll=assets/"simulation.dll";
                try {
                    bool have=false;
                    if(!source.empty()&&fs::is_regular_file(source)) {fs::copy_file(source,copiedSource);have=true;}
                    if(!dll.empty()&&fs::is_regular_file(dll)) {fs::copy_file(dll,copiedDll);have=true;}
                    if(!have)throw std::runtime_error("The custom simulation's source and DLL are missing.");
                    d.source=fs::is_regular_file(copiedSource)?utf8(copiedSource.lexically_relative(path.parent_path())):"";
                    d.dll=utf8(copiedDll.lexically_relative(path.parent_path()));
                } catch(...) {std::error_code ec;fs::remove(copiedSource,ec);fs::remove(copiedDll,ec);fs::remove(assets,ec);throw;}
            }
        }
        return atomic_write(path,encode(d),error);
    } catch(const std::exception& e) {error=e.what();return false;}
}
// A legal file stem for "Copy a library simulation". The document's display
// name can keep punctuation; the file name cannot keep the characters Windows
// rejects, and a very long title is shortened so the token still fits.
inline std::string copy_stem(std::string name) {
    for(char& c:name)
        if(c=='\\'||c=='/'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c='-';
    if(name.size()>80) name.resize(80);
    while(!name.empty()&&(name.back()==' '||name.back()=='.')) name.pop_back();
    if(name.empty()) name="Copy";
    return name;
}
inline void capture_settings(Sim& sim,Document& d) {
    for(const auto& k:sim.knobs())d.knobs.emplace_back(k.key,k.value);
    for(const auto& s:sim.switches())d.switches.emplace_back(s.key,s.value);
}
// A saved starting point whose model is a registry id and whose settings are
// that simulation's defaults. Source and DLL stay empty: this is not a custom
// C++ simulation, and it does not capture a live world.
inline void library_start(Document& d,std::string name,std::string model,Sim& sim) {
    d=Document{};
    d.name=std::move(name);
    d.model=std::move(model);
    capture_settings(sim,d);
}
inline bool apply_settings(Sim& sim,const Document& d,std::string& error) {
    for(const auto& [key,value]:d.knobs) {
        const auto& knobs=sim.knobs();auto it=std::find_if(knobs.begin(),knobs.end(),[&](const Knob& k){return k.key==key;});
        if(it==knobs.end()||!std::isfinite(value)||value<it->min||value>it->max) {error="Unsupported saved setting: "+key;return false;}
    }
    for(const auto& [key,value]:d.switches) {
        (void)value;const auto& switches=sim.switches();
        if(std::none_of(switches.begin(),switches.end(),[&](const Switch& s){return s.key==key;})) {error="Unsupported saved switch: "+key;return false;}
    }
    for(const auto& [key,value]:d.knobs)sim.on_knob(key,value);
    for(const auto& [key,value]:d.switches)sim.on_switch(key,value);
    sim.reset();return true;
}
inline std::vector<fs::path> files(const fs::path& directory) {
    std::vector<fs::path> out;std::error_code ec;
    for(fs::directory_iterator it(directory,ec),end;!ec&&it!=end;it.increment(ec))
        if(it->is_regular_file(ec)&&it->path().extension()==".benchsim")out.push_back(it->path());
    std::sort(out.begin(),out.end());return out;
}
} // namespace bench::projects

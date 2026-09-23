#include "projects.hpp"
#include "sims/sorting.hpp"
#include "sims/rulespec.hpp"
#include <cstdio>

int main() {
    namespace p=bench::projects;namespace fs=std::filesystem;
    int checks=0,failed=0;auto check=[&](bool ok,const char* text){++checks;if(!ok){++failed;std::printf("FAIL %s\n",text);}};
    const auto root=fs::temp_directory_path()/bench::path_from_utf8("bench saved sims "+p::token()+" \xCE\xA9");
    fs::create_directories(root);
    std::string error;p::Document original;original.name="A \"quoted\" simulation";original.model="sorting3d";
    original.knobs={{"algorithm",15},{"size",257},{"direction",1},{"seed",77}};
    original.body="line one\nline two\\end\"";
    const auto project=root/"saved.benchsim";
    check(p::save(project,original,error),"save a project under a Unicode path with spaces");
    p::Document reopened;
    check(p::read(project,reopened,error)&&p::encode(original)==p::encode(reopened),"quotes, multiline bodies and settings round-trip");
    bench::SortingSim first(1);check(p::apply_settings(first,reopened,error),"apply a saved sorting setup");
    check(first.engine().values.size()==257&&int(first.engine().algorithm)==15,"reopening restores the saved model parameters");
    auto expected=first.engine().values;first.step();
    bench::SortingSim second(1);check(p::apply_settings(second,reopened,error)&&second.engine().values==expected&&second.generation()==0,"fresh instances reproduce the starting setup without pretending to resume progress");
    p::Document invalid=reopened;invalid.knobs.emplace_back("not-a-knob",1);
    check(!p::apply_settings(second,invalid,error)&&second.engine().values==expected,"unknown settings fail before changing the simulation");
    invalid=reopened;invalid.knobs[1].second=10000;
    check(!p::apply_settings(second,invalid,error)&&second.engine().values==expected,"out-of-range settings fail without silently clamping");
    for(const std::string bad:{"LIFE_SIM_PROJECT 99\nname \"x\"\nmodel \"life\"\nend\n",
         "LIFE_SIM_PROJECT 1\nname \"x\"\nmodel \"life\"\nknob \"size\" 16\nknob \"size\" 32\nend\n",
         "LIFE_SIM_PROJECT 1\nname \"x\"\nmodel \"life\"\nsource \"x.cpp\"\nend\n",
         "LIFE_SIM_PROJECT 1\nname \"x\"\nmodel \"life\"\n",
         "LIFE_SIM_PROJECT 1\nname \"x\"\nmodel \"life\"\nend\ntrailing"}) {
        p::atomic_write(root/"bad.benchsim",bad,error);const auto before=p::encode(reopened);
        check(!p::read(root/"bad.benchsim",reopened,error)&&p::encode(reopened)==before,"damaged documents do not replace a valid project");
    }
    original.name="Updated";check(p::save(project,original,error)&&p::read(project,reopened,error)&&reopened.name=="Updated","an existing project is replaced successfully");
    fs::create_directories(root/"blocked.benchsim");
    check(!p::save(root/"blocked.benchsim",original,error)&&fs::is_directory(root/"blocked.benchsim"),"failed replacement leaves the original destination untouched");

    const auto source=root/"custom source.cpp",dll=root/"custom source.dll";
    p::atomic_write(source,"// my permanent simulation\n",error);p::atomic_write(dll,"test binary bytes",error);
    p::Document custom;custom.name="My custom sim";custom.source=p::utf8(source);custom.dll=p::utf8(dll);
    const auto saved=root/"custom.benchsim";
    check(p::save(saved,custom,error)&&p::read(saved,reopened,error),"save source and its runnable binary beside the project");
    const auto savedSource=p::resolve(saved,reopened.source),savedDll=p::resolve(saved,reopened.dll);
    check(!fs::path(reopened.source).is_absolute()&&fs::is_regular_file(savedSource)&&fs::is_regular_file(savedDll),"saved custom assets use relative paths");
    fs::remove(source);fs::remove(dll);
    check(fs::file_size(savedSource)>0&&fs::file_size(savedDll)>0,"deleting the imported originals does not remove saved code or its DLL");
    const auto assets=reopened.source;reopened.source=p::utf8(savedSource);reopened.dll=p::utf8(savedDll);
    check(p::save(saved,reopened,error)&&p::read(saved,reopened,error)&&reopened.source==assets,"resaving an owned project reuses its code directory");
    const auto moved=root/"moved";fs::create_directories(moved);fs::copy_file(saved,moved/saved.filename());
    const auto copied=moved/savedSource.parent_path().filename();fs::create_directories(copied);
    fs::copy_file(savedSource,copied/savedSource.filename());fs::copy_file(savedDll,copied/savedDll.filename());
    check(p::read(moved/saved.filename(),reopened,error)&&fs::is_regular_file(p::resolve(moved/saved.filename(),reopened.source)),"a project and its assets can move together");
    bool clean=true;for(const auto& entry:fs::directory_iterator(root))if(p::utf8(entry.path()).find(".tmp-")!=std::string::npos)clean=false;
    check(clean,"successful and failed saves leave no temporary document files");
    // This process owns the uniquely named fixture directory and all its files.
    for(const auto& file:fs::recursive_directory_iterator(root))if(file.is_regular_file())fs::remove(file.path());
    std::vector<fs::path> dirs;for(const auto& d:fs::recursive_directory_iterator(root))if(d.is_directory())dirs.push_back(d.path());
    std::sort(dirs.rbegin(),dirs.rend());for(const auto& dir:dirs)fs::remove(dir);fs::remove(root);
    std::printf("%d saved-simulation checks, %d failed\n",checks,failed);return failed?1:0;
}

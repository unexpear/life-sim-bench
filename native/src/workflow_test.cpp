#include "registry.hpp"
#include "workflows.hpp"
#include "patterns.hpp"
#include "history.hpp"
#include <cstdio>
#include <fstream>

int main() {
    int checks=0, failed=0;
    auto check=[&](bool ok,const std::string& name){++checks;if(!ok){++failed;std::printf("FAIL %s\n",name.c_str());}};
    for(const char* header:{"x = 3, y = 1, rule = B3/S23", "x=3,y=1,rule=B3/S23", "x= 3, y= 1, rule= B3/S23", "x =3,y =1,rule =B3/S23"}) {
        auto p=bench::parse_rle(std::string(header)+"\n3o!");
        check(p.ok && p.w==3 && p.h==1 && p.rule=="B3/S23" && p.cells==std::vector<std::uint8_t>({1,1,1}),header);
    }
    bench::Knob numeric{"test","Number",-2,2,0,0.25f};float value=0;
    check(bench::parse_control_value("1.3",numeric,value)&&value==1.25f,"typed values quantise");
    for(const char* text:{"nan","inf","1garbage","","4","-3"}) check(!bench::parse_control_value(text,numeric,value),"reject invalid numeric input");
    for(const auto& e:bench::registry()) {
        const auto& wf=bench::workflow(e.id);
        check(wf.id==e.id && !wf.title.empty() && !wf.hint.empty(),e.id+" has a workflow");
        auto sim=e.make();
        check(e.id=="rule" ? bench::catalog_title(e.id).find("Rule lab")!=std::string::npos :
              bench::catalog_title(e.id)==sim->about().title,e.id+" library title identifies the template");
        for(const auto& key:wf.primary) check(std::any_of(sim->knobs().begin(),sim->knobs().end(),[&](const auto& k){return k.key==key;}),e.id+" primary control "+key);
    }
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("bench workflow test "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto out=bench::export_path(root.string(),"metrics-test-by-100 epochs.csv");
    bench::History history;
    auto life=bench::registry()[0].make();history.set_min_interval(0);
    life->step();history.observe(*life);
    check(history.depth()>0,"balanced mode records a snapshot");
    history.set_record_snapshots(false);
    check(history.depth()==0&&history.bytes()==0,"large-run mode releases snapshots immediately");
    life->step();history.observe(*life);
    check(history.depth()==0&&!history.names().empty(),"large-run mode keeps measurements without field copies");
    history.set_record_snapshots(true);life->step();history.observe(*life);
    check(history.depth()>0,"balanced mode resumes recording");
    check(history.write_csv(out,bench::History::Axis::Epoch),"CSV exports through a directory containing spaces");
    check(fs::path(out).parent_path()==root,"export preserves parent directory");
    std::ofstream(root/"new_rule.cpp")<<"// source only\n";
    auto files=bench::plugin_files(root.string());
    check(files.size()==1 && files[0].name=="new_rule","source-only plugin is discoverable");
    std::ofstream(root/"new_rule.dll")<<"fixture";
    std::ofstream(root/"new_rule.dll.loaded.dll")<<"fixture";
    files=bench::plugin_files(root.string());
    check(files.size()==1,"source/DLL pair is one plugin and shadow is excluded");
    // Delete only files this test created, without a recursive directory deletion.
    for(const auto& file:{fs::path(out),root/"new_rule.cpp",root/"new_rule.dll",root/"new_rule.dll.loaded.dll"})fs::remove(file);
    fs::remove(root);
    std::printf("%d workflow checks, %d failed\n",checks,failed);return failed?1:0;
}

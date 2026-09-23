// Included by the hidden-window harness only. Write and read run in separate
// processes, with LIFESIM_USER_DATA pointing at a dedicated test directory.
int verifySavedProjects(const std::string& mode,int expectedTemplates) {
    namespace p=bench::projects;namespace fs=std::filesystem;
    if(!std::getenv("LIFESIM_USER_DATA")){std::printf("Set LIFESIM_USER_DATA to a test directory.\n");return 2;}
    int checks=0,failed=0;
    auto check=[&](bool ok,const std::string& name){++checks;std::printf("%s %s\n",ok?"PASS":"FAIL",name.c_str());std::fflush(stdout);if(!ok)++failed;return ok;};
    const auto directory=bench::path_from_utf8(bench::Paths::get().projects());
    const auto custom=directory/bench::path_from_utf8("My saved mover \xCE\xA9.benchsim");
    const auto rule=directory/"My rule.benchsim",creature=directory/"My creature.benchsim";
    const auto sorting=directory.parent_path()/"Outside library"/"My sort.benchsim";
    auto pick=[&](const fs::path& path){
        rebuildRows();for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].projectPath==p::utf8(path)){select(i);return g.sel==i;}
        return false;
    };
    auto knob=[&](const std::string& key){if(g.sim)for(const auto& k:g.sim->knobs())if(k.key==key)return k.value;return -999.f;};
    auto build=[&](){
        buildPlugin();const auto start=GetTickCount64();
        while(g.buildProcess&&GetTickCount64()-start<120000){pollBuild();Sleep(10);}
        if(g.buildProcess){TerminateProcess(g.buildProcess,1);WaitForSingleObject(g.buildProcess,1000);CloseHandle(g.buildProcess);g.buildProcess=nullptr;}
        const bool ok=g.buildOk&&g.sim;
        if(!ok)for(const auto& line:g.build)std::printf("  %s\n",line.c_str());
        return ok;
    };
    auto seed=[&](const fs::path& path,const std::string& model){
        p::Document d;d.name=p::utf8(path.stem());d.model=model;std::string error;
        return p::save(path,d,error)&&pick(path)&&g.sim;
    };
    std::string error;
    check(!g.sim,"startup loads no simulation into memory");
    if(mode=="--verify-library") {
        int templates=0;for(const auto& row:g.rows)if(!row.saved&&!row.plugin)++templates;
        check(templates==expectedTemplates,"installer selected exactly "+std::to_string(expectedTemplates)+" templates (found "+std::to_string(templates)+")");
        openLibrary();HDC dc=GetDC(g.hwnd),mem=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,1800,1100);auto old=SelectObject(mem,bitmap);
        g.ui.audit=true;layout(g.hwnd,mem);layout(g.hwnd,mem);
        check(g.ui.overflow==0,"library controls fit the window");
        check(!g.sim,"browsing the library does not construct a simulation");
        SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(g.hwnd,dc);
    } else if(mode=="--verify-saved-write") {
        g.autoSave=true;newFromTemplate();
        if(check(g.sel<g.rows.size()&&g.rows[g.sel].plugin&&!g.sim,"Create saves a source-only simulation")) {
            const auto source=bench::path_from_utf8(g.rows[g.sel].source);
            {std::ofstream edit(source,std::ios::app);edit<<"\n// Permanent user edit: reopen-proof\n";}
            if(check(build(),"custom simulation compiles and runs using the application's Build action")) {
                g.sim->on_knob("density",.25f);g.sim->reset();
                check(saveCurrent(p::utf8(custom),true,error),"Save as retains custom source and DLL under a Unicode name: "+error);
                rebuildRows();g.sim->on_knob("density",.31f);g.sim->reset();
                check(build()&&std::abs(knob("density")-.31f)<.001f,"rebuilding saved custom code preserves its controls");
            }
        }
        newRuleSimulation();
        if(check(g.sim&&dynamic_cast<bench::RuleSim*>(g.sim.get())&&g.rows[g.sel].saved,"Create makes a saved rule simulation without installed templates or a compiler")) {
            setControl("density",.19f);setControl("seed",7);g.ruleText="B2-a/S12/256";applyRule();
            const auto& cells=g.sim->field().cells;
            check(p::atomic_write(directory.parent_path()/"expected-rule.bin",std::string(cells.begin(),cells.end()),error),"record initial rule cells for fresh-process comparison");
            const bool saved=saveCurrent(p::utf8(rule),true,error);check(saved,"save a custom rulestring: "+error);
        }
        if(check(seed(creature,"locomotion"),"switch to the movement template")) {
            auto* body=dynamic_cast<bench::Locomotion*>(g.sim.get());body->preset(2);
            check(p::atomic_write(directory.parent_path()/"expected-body.txt",body->saveBody(),error),"record a custom creature body for the fresh-process comparison");
            const bool saved=saveCurrent(p::utf8(creature),true,error);check(saved,"save a custom creature body: "+error);
        }
        if(check(seed(directory/"Sorting starter.benchsim","sorting2d"),"open a sorting template")) {
            setControl("algorithm",15);setControl("size",180);applySetup();
            check(saveCurrent(p::utf8(sorting),true,error),"index a simulation saved outside the default folder");
            setControl("size",256); // Close must retain staged setup too.
        }
        if(!failed){WndProc(g.hwnd,WM_CLOSE,0,0);check(!IsWindow(g.hwnd),"closing the app autosaves before destroying its window");}
    } else if(mode=="--verify-saved-read") {
        if(check(pick(custom)&&g.sim,"fresh process finds and loads saved custom code")) {
            check(std::abs(knob("density")-.31f)<.001f,"switching simulations autosaved custom settings");
            std::ifstream in(bench::path_from_utf8(g.rows[g.sel].source));std::string source((std::istreambuf_iterator<char>(in)),{});
            check(source.find("Permanent user edit: reopen-proof")!=std::string::npos,"the user's edited source survives restart");
            check(build()&&std::abs(knob("density")-.31f)<.001f,"saved code rebuilds and retains settings in a fresh process");
            const auto before=g.sim->generation();g.sim->step();check(g.sim->generation()>before,"saved custom simulation can run");
        }
        if(check(pick(rule)&&g.sim,"fresh process loads the saved rule")) {
            auto* sim=dynamic_cast<bench::RuleSim*>(g.sim.get());check(sim&&sim->spec().text=="B2-a/S12/256","custom Hensel/256-state rulestring restored");
            std::ifstream in(directory.parent_path()/"expected-rule.bin",std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});
            const auto& cells=g.sim->field().cells;
            check(std::string(cells.begin(),cells.end())==bytes&&knob("seed")==7&&std::abs(knob("density")-.19f)<.001f,"reopened rule has identical initial cells and seed");
        }
        if(check(pick(creature)&&g.sim,"fresh process loads the creature")) {
            std::ifstream in(directory.parent_path()/"expected-body.txt");std::string body((std::istreambuf_iterator<char>(in)),{});
            auto* sim=dynamic_cast<bench::Locomotion*>(g.sim.get());check(sim&&sim->saveBody()==body,"joints, bones and muscles restored exactly");
        }
        if(check(pick(sorting)&&g.sim,"external save location remains in the library after restart")) {
            check(knob("size")==256&&knob("algorithm")==15,"closing retained staged sorting size and chosen algorithm");
            check(g.sim->generation()==0,"reopening starts a new run with the saved setup");
        }
    }
    g.autoSave=false;std::printf("%d persistence UI checks, %d failed\n",checks,failed);return failed?1:0;
}

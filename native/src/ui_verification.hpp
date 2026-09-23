// Included only by the headless UI executable, after the application functions.
// Exercises the real window procedure and layout, with a hidden window.
int verifyUi() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const std::string& name){++checks;if(!ok){++failed;std::printf("FAIL %s\n",name.c_str());}};
    HDC dc=GetDC(g.hwnd), mem=CreateCompatibleDC(dc);
    HBITMAP bmp=CreateCompatibleBitmap(dc,1800,1100);auto old=SelectObject(mem,bmp);
    auto sizeWindow=[&](int w,int h){RECT r{0,0,w,h};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(g.hwnd,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);};
    g.ui.audit=true;g.ui.mouse={-1,-1};g.running=false;
    check(!g.sim,"startup library does not construct a simulation");
    openLibrary();WndProc(g.hwnd,WM_APP+4,0,0);
    check(g.library&&g.librarySearch,"library opens its native search field");
    SetWindowTextW(g.librarySearch,L"traffic");layout(g.hwnd,mem);
    int visible=0;for(const auto& r:g.rowRects)if(r.right>r.left)++visible;
    check(visible==2,"search narrows the library to the two traffic models");
    SetWindowTextW(g.librarySearch,L"voxelcity");WndProc(g.hwnd,WM_APP+5,0,0);
    check(g.sim&&g.rows[g.sel].id=="voxelcity","Enter uses the latest search even before repaint");
    g.libraryQuery.clear();
    for(const auto& size:std::vector<std::pair<int,int>>{{1084,661},{1500,900}}) {
        sizeWindow(size.first,size.second);
        for(std::size_t i=0;i<bench::registry().size();++i) {
            select(i);g.sim->step();g.hist.observe(*g.sim);
            for(int tab=0;tab<3;++tab) {
                g.inspector=tab;g.notesScroll=0;
                for(int pass=0;pass<4;++pass)layout(g.hwnd,mem);
                const auto name=g.rows[i].id+" tab "+std::to_string(tab)+" width "+std::to_string(size.first);
                check(g.ui.overflow==0,name+" controls stay inside their pane");
                bool overlap=false;
                for(std::size_t a=0;a<g.ui.widgets.size();++a)for(std::size_t b=a+1;b<g.ui.widgets.size();++b){RECT r{};if(IntersectRect(&r,&g.ui.widgets[a],&g.ui.widgets[b]))overlap=true;}
                check(!overlap,name+" controls do not overlap");
                check(g.canvasRect.right-g.canvasRect.left>=200 && g.canvasRect.bottom-g.canvasRect.top>=150,name+" canvas remains usable");
            }
        }
    }
    select(0);g.sim->step();const auto before=g.sim->generation();
    auto* active=g.sim.get();openLibrary();rebuildRows();layout(g.hwnd,mem);
    check(g.sim.get()==active&&g.sim->generation()==before,"browsing and refreshing preserve the only loaded simulation");
    check(!g.running&&!g.training,"library pauses computation");
    closeLibrary();
    check(!g.showLeft&&!g.librarySearch,"workspace removes the roster and search field");
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="rule") {
        select(i);layout(g.hwnd,mem);WndProc(g.hwnd,WM_APP+8,0,0);
        check(g.ruleEdit!=nullptr,"rulestring has a native text editor");
        if(g.ruleEdit) {
            DWORD begin=0,end=0;SendMessageW(g.ruleEdit,EM_GETSEL,WPARAM(&begin),LPARAM(&end));
            check(begin==0&&end==6,"rule opens with text selected for replacement");
            SendMessageW(g.ruleEdit,EM_REPLACESEL,TRUE,LPARAM(L"B2-a/S12/256"));
            check(g.ruleText=="B2-a/S12/256"&&g.ruleMsg.empty(),"native replacement validates a Hensel Generations rule");
            SendMessageW(g.ruleEdit,EM_UNDO,0,0);
            check(g.ruleText=="B3/S23","native undo restores previous text");
            SetWindowTextW(g.ruleEdit,L"B3/S23/3junk");
            const auto cells=g.sim->field().cells;auto* current=g.sim.get();
            WndProc(g.hwnd,WM_APP+9,1,0);
            check(g.sim.get()==current&&g.sim->field().cells==cells&&!g.ruleMsg.empty()&&g.ruleEdit,"invalid rule leaves current world and editor intact");
            WndProc(g.hwnd,WM_APP+9,0,0);
            check(!g.ruleEdit&&g.ruleText=="B3/S23"&&g.ruleMsg.empty(),"Cancel restores the applied rule");
            setControl("size",1);setControl("density",.17f);setControl("seed",7);
            layout(g.hwnd,mem);beginRuleEditor();
            SetWindowTextW(g.ruleEdit,L"B2cekin/S12/256");g.running=true;
            WndProc(g.hwnd,WM_APP+9,1,0);
            check(g.ruleText=="B2-a/S12/256"&&!g.ruleEdit&&!g.running&&g.sim->generation()==0,"Apply canonicalizes the rule and pauses a new run");
            bench::projects::Document settings;bench::projects::capture_settings(*g.sim,settings);
            auto expected=bench::make_rule_workspace(g.ruleText);std::string error;
            bench::projects::apply_settings(*expected,settings,error);
            check(g.sim->field().w==512&&g.pendingSetup.empty()&&std::abs(g.sim->knobs()[1].value-.17f)<1e-6f&&g.sim->knobs()[2].value==7,"Apply retains world size, density and staged seed");
            check(expected->field().cells==g.sim->field().cells,"applied rule matches a restored workspace's initial cells");
            for(const auto& size:std::vector<std::pair<int,int>>{{1084,661},{1500,900}}) {
                sizeWindow(size.first,size.second);g.notesScroll=0;
                g.ruleText="B3/S23/3.5";layout(g.hwnd,mem);
                check(g.ui.overflow==0,"inline rule error fits the controls panel");
                beginRuleEditor();g.notesScroll=400;layout(g.hwnd,mem);
                check(!g.ruleEdit,"scrolling an editor out of view removes its child window");
            }
            closeRuleEditor(true);g.notesScroll=0;layout(g.hwnd,mem);beginRuleEditor();
            g.inspector=2;layout(g.hwnd,mem);check(!g.ruleEdit,"Guide tab hides the rulestring child window");
            namespace fs=std::filesystem;const auto path=fs::temp_directory_path()/("bench rule mismatch "+std::to_string(GetTickCount64())+".rle");
            {std::ofstream file(path);file<<"x = 1, y = 1, rule = B3/S23\no!\n";}
            const auto beforeImport=g.sim->field().cells;loadPatternFile(path.string());
            check(g.sim->field().cells==beforeImport&&g.toast.find("Pattern uses")!=std::string::npos,"mismatched RLE rule refuses before erasing the world");
            {std::ofstream file(path);file<<"x = 1, y = 1, rule = B2cekin/S12/256\no!\n";}
            loadPatternFile(path.string());check(std::count(g.sim->field().cells.begin(),g.sim->field().cells.end(),1)==1,"equivalent canonical RLE rule imports");
            std::error_code ec;fs::remove(path,ec);
            g.inspector=0;g.notesScroll=0;layout(g.hwnd,mem);beginRuleEditor();
            {MSG q;while(PeekMessageW(&q,g.hwnd,WM_APP,WM_APP+16,PM_REMOVE)){}}
            g.ui.down=false;g.ui.clicked=false;g.running=true;g.acc=1;g.msPerStep=1000;
            // Dispatch explicitly: this test isolates editing while a worker
            // owns the sim. The frame scheduler is exercised separately below.
            startOffThread(App::OffUnit::Step);
            check(workBusy()&&g.ruleEdit,"rule editor can remain open during a slow step");
            SetWindowTextW(g.ruleEdit,L"B2/S12V");
            check(g.ruleText=="B2/S12V","typing during a worker step preserves the latest draft");
            WndProc(g.hwnd,WM_APP+9,1,0);
            check(workBusy()&&!g.heldMessages.empty(),"rule application waits for the worker");
            const auto started=GetTickCount64();
            while(g.work&&!g.work->ready()&&GetTickCount64()-started<30000)Sleep(5);
            stepFrame(.016);g.running=false;
            {MSG q;while(PeekMessageW(&q,g.hwnd,WM_APP,WM_APP+16,PM_REMOVE))WndProc(q.hwnd,q.message,q.wParam,q.lParam);}
            const auto* applied=dynamic_cast<bench::RuleSim*>(g.sim.get());
            check(applied&&applied->spec().text=="B2/S12V"&&!g.ruleEdit,"deferred application uses the text typed during the step");
        }
        select(0);break;
    }
    g.sim->step();
    g.numberKey="density";g.numberRect=RECT{800,300,1000,332};
    WndProc(g.hwnd,WM_APP+2,0,0);
    check(g.numberEdit!=nullptr,"native exact-value editor opens");
    if(g.numberEdit) {
        SetWindowTextW(g.numberEdit,L"0.42");WndProc(g.hwnd,WM_APP+3,1,0);
        check(!g.numberEdit && g.pendingSetup.size()==1,"Enter stages setup and closes editor");
        check(g.sim->generation()==before,"staging does not reset the running simulation");
        applySetup();
        check(g.pendingSetup.empty()&&g.sim->generation()==0,"Apply restarts exactly once");
        auto& k=g.sim->knobs();auto density=std::find_if(k.begin(),k.end(),[](const auto& v){return v.key=="density";});
        check(density!=k.end()&&std::fabs(density->value-0.42f)<1e-5f,"typed setup value reaches simulation");
        g.numberKey="density";WndProc(g.hwnd,WM_APP+2,0,0);
        SetWindowTextW(g.numberEdit,L"nan");WndProc(g.hwnd,WM_APP+3,1,0);
        check(g.numberEdit&&g.pendingSetup.empty(),"invalid numeric entry does not change the simulation");
        WndProc(g.hwnd,WM_APP+3,0,0);check(!g.numberEdit,"Escape cancels editor");
    }
    g.inspector=0;g.advanced=true;setControl("density",0.31f);
    for(int pass=0;pass<4;++pass)layout(g.hwnd,mem);
    check(g.notesRect.bottom<900,"staged setup gets a persistent apply bar");
    g.notesScroll=100000;for(int pass=0;pass<4;++pass)layout(g.hwnd,mem);
    check(g.notesScroll<=std::max(0,g.notesContentH-int(g.notesRect.bottom-g.notesRect.top)),"scroll is clamped after layout changes");
    setLargeRun(true);setControl("size",4);applySetup();
    check(g.sim->field().w==4096,"world setup opens a 4096-wide Life field");
    g.sim->step();g.hist.observe(*g.sim);layout(g.hwnd,mem);
    check(g.hist.bytes()==0&&g.hist.depth()==0&&!g.hist.names().empty(),"large world runs and renders with measurements and no timeline copies");
    select(9);setControl("world",7);applySetup();g.sim->step();
    auto* volume=dynamic_cast<bench::Life3D*>(g.sim.get());
    check(volume&&volume->size()==160,"3D setup runs the largest offered 160-cubed world");
    setLargeRun(false);
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="locomotion"){select(i);break;}
    g.inspector=0;g.advanced=false;g.notesScroll=0;sizeWindow(1500,900);
    for(int pass=0;pass<4;++pass)layout(g.hwnd,mem);
    auto clickPoint=[&](POINT point){
        WndProc(g.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        WndProc(g.hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
        layout(g.hwnd,mem);
    };
    auto clickButton=[&](RECT rect){clickPoint(POINT{(rect.left+rect.right)/2,(rect.top+rect.bottom)/2});};
    clickButton(motionUi.edit);
    check(movement()&&movement()->editing()&&!g.running&&!g.training,"Edit body opens and pauses the movement workshop");
    auto bodyBefore=movement()->saveBody();
    clickButton(motionUi.tools[1]);
    check(movement()->tool()==bench::Locomotion::Tool::Joint,"Joint button selects the direct canvas tool");
    auto canvasClick=[&](double x,double y){
        const auto a=g.canvasRect;const double scale=std::min(double(a.right-a.left)/1120,double(a.bottom-a.top)/680);
        const double left=a.left+((a.right-a.left)-1120*scale)/2,top=a.top+((a.bottom-a.top)-680*scale)/2;
        clickPoint(POINT{LONG(left+(560+x*84)*scale),LONG(top+(548-y*76)*scale)});
    };
    canvasClick(2,2);
    check(movement()->evolution().body.joints.size()==5,"window canvas input reaches the body editor through letterboxing");
    clickButton(motionUi.edit);
    check(movement()->editing()&&!movement()->message().empty(),"invalid body is kept in the editor with an actionable error");
    clickButton(motionUi.tools[5]);check(movement()->saveBody()==bodyBefore,"Undo button restores body geometry");
    for(const auto size:std::vector<std::pair<int,int>>{{1084,661},{1500,900}}){
        sizeWindow(size.first,size.second);g.notesScroll=0;for(int pass=0;pass<4;++pass)layout(g.hwnd,mem);
        check(g.ui.overflow==0,"movement editor controls fit both workspace sizes");
        bool overlap=false;for(std::size_t a=0;a<g.ui.widgets.size();++a)for(std::size_t b=a+1;b<g.ui.widgets.size();++b){RECT r{};if(IntersectRect(&r,&g.ui.widgets[a],&g.ui.widgets[b]))overlap=true;}
        check(!overlap,"movement editor tools do not overlap");
    }
    clickButton(motionUi.edit);g.sim->advance_epoch();g.hist.observe(*g.sim);layout(g.hwnd,mem);
    const auto movementEpoch=g.sim->epoch_count();clickButton(motionUi.replay);
    check(movement()->replaying()&&g.running&&!g.training,"Replay champion starts playback through the real button");
    g.sim->step();clickButton(motionUi.replay);
    check(!movement()->replaying()&&g.sim->epoch_count()==movementEpoch,"returning from replay preserves training progress");
    // A dedicated run through the real launch path: the window's APPLIED setup
    // goes to bench_run, the window pauses, and the run's own record agrees with
    // the setting the window was showing. Into folders with spaces, which is
    // where every quoting bug in a command line is found.
    {
        namespace fs=std::filesystem;
        auto waitFor=[&](DWORD ms){const auto t0=GetTickCount64();while(g.dedicatedProcess&&GetTickCount64()-t0<ms){pollDedicated();Sleep(20);}};
        auto readAll=[](const std::string& path){std::ifstream in(path);std::stringstream ss;ss<<in.rdbuf();return ss.str();};
        auto tidy=[](const std::string& dir){std::error_code ec;for(const char* n:{"metrics.csv","run.json","runner.log","stop"})fs::remove(fs::path(dir)/n,ec);fs::remove(dir,ec);};
        const std::string stamp=std::to_string(GetTickCount64());
        const std::string out=(fs::temp_directory_path()/("bench dedicated UI "+stamp)).string();
        const std::string outStop=(fs::temp_directory_path()/("bench dedicated UI stop "+stamp)).string();

        select(0);setControl("density",0.4f);
        launchDedicated(200,false,out);
        check(!g.dedicatedProcess,"a staged, unapplied setup is refused rather than silently dropped");
        g.pendingSetup.clear();

        setControl("density",0.33f);applySetup();
        const auto& kn=g.sim->knobs();
        const auto dk=std::find_if(kn.begin(),kn.end(),[](const auto& k){return k.key=="density";});
        const std::string want=dk!=kn.end()?"\"density\": {\"value\": "+bench::run::json_num(dk->value):std::string("(no density knob)");
        g.running=true;
        launchDedicated(200,false,out);
        check(g.dedicatedProcess!=nullptr,"a dedicated run starts from the window");
        check(!g.running&&!g.training,"starting a dedicated run pauses the window's own run");
        waitFor(120000);
        check(!g.dedicatedProcess,"the dedicated run finishes and the window collects it");
        const std::string json=readAll(out+"\\run.json");
        check(json.find("\"exit_code\": 0")!=std::string::npos,"the dedicated run exits cleanly");
        check(json.find(want)!=std::string::npos,"and ran the density the window had applied: "+want);
        check(json.find("\"units\": 200")!=std::string::npos,"for exactly the 200 steps asked for");
        tidy(out);

        launchDedicated(0,false,outStop);
        stopDedicated();                  // at once: held until the run says it is running
        waitFor(30000);
        const std::string stopped=readAll(outStop+"\\run.json");
        check(!g.dedicatedProcess&&stopped.find("\"reason\": \"stop-file\"")!=std::string::npos,
              "Stop pressed the instant a run starts is delivered, and the run ends cleanly with its results");
        tidy(outStop);
        for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="rule") {
            select(i);g.ruleText="B2-a/S12/4";applyRule();g.ruleText="B3/S23/invalid-draft";
            launchDedicated(5,false,out);waitFor(30000);
            const auto result=readAll(out+"\\run.json");
            check(result.find("\"rule\": \"B2-a/S12/4\"")!=std::string::npos&&result.find("\"exit_code\": 0")!=std::string::npos,"dedicated run uses the applied rule despite an invalid editor draft");
            tidy(out);select(0);break;
        }
    }
    // Stepping off the window's thread, through the real stepFrame() and the
    // real window procedure. A unit is sent to the worker by setting the cost
    // estimate the window itself uses; everything after that is the product's
    // own path. What is checked is the rule that makes it safe — while a unit is
    // in flight nothing lays out and no control reaches the sim; Pause is kept
    // and lands with the unit — and that the world advances by exactly the unit.
    {
        // The harness calls WndProc directly, so messages the app POSTED earlier
        // are still sitting in the queue; stepFrame rightly defers to them.
        {MSG q;while(PeekMessageW(&q,g.hwnd,WM_APP,WM_APP+16,PM_REMOVE)){}}
        g.ui.down=false;g.ui.clicked=false;   // no press left over from earlier checks holds the steps back
        auto waitReady=[&]{const auto t0=GetTickCount64();while(g.work&&!g.work->ready()&&GetTickCount64()-t0<30000)Sleep(5);};
        auto press=[&](POINT p){WndProc(g.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));};
        auto release=[&](POINT p){WndProc(g.hwnd,WM_LBUTTONUP,0,MAKELPARAM(p.x,p.y));};
        select(0);layout(g.hwnd,mem);
        const auto gen0=g.sim->generation();
        g.running=true;g.acc=1.0;g.msPerStep=1000.0;g.pauseAfter=false;
        stepFrame(0.016);
        check(workBusy(),"a step measured as slow goes to the worker");
        const long long layouts=g.layoutCalls;
        WndProc(g.hwnd,WM_PAINT,0,0);
        check(g.layoutCalls==layouts,"a paint while it is in flight re-shows the last frame and never lays out");
        const POINT elsewhere{g.runButton.right+40,(g.runButton.top+g.runButton.bottom)/2};
        press(elsewhere);release(elsewhere);
        check(!g.pauseAfter&&g.running,"any other control waits for the unit instead of acting on a sim in use");
        const POINT pauseAt{(g.runButton.left+g.runButton.right)/2,(g.runButton.top+g.runButton.bottom)/2};
        press(pauseAt);
        check(g.pauseAfter&&g.running,"Pause pressed during the unit is kept, not applied under it");
        release(pauseAt);
        waitReady();
        stepFrame(0.016);
        check(!workBusy()&&!g.running&&!g.pauseAfter,"the unit lands and the kept Pause takes effect with it");
        check(g.sim->generation()==gen0+1,"the world advanced by exactly the one step that ran on the worker");
        layout(g.hwnd,mem);
        check(g.sim->generation()==gen0+1&&!g.running,
              "a press made during the unit does not become a click after it — no late Step, no Pause toggled back");

        g.running=true;g.acc=1.0;g.msPerStep=1000.0;
        stepFrame(0.016);
        WndProc(g.hwnd,WM_CHAR,' ',0);
        check(g.pauseAfter,"Space during a unit is kept as a Pause");
        waitReady();stepFrame(0.016);
        check(!g.running,"and lands with it");

        // What arrives during a unit and must not be lost with it. The unit here
        // is a real Life step: however soon it finishes, the sim stays the
        // worker's until stepFrame collects it, so the timing cannot matter.
        g.pendingSetup.clear();g.ui.down=false;g.ui.clicked=false;
        WndProc(g.hwnd,WM_PAINT,0,0);         // compose a frame to keep, as the window does every frame
        {MSG q;while(PeekMessageW(&q,g.hwnd,WM_APP,WM_APP+16,PM_REMOVE)){}}
        const auto& kd=g.sim->knobs();
        const auto dk=std::find_if(kd.begin(),kd.end(),[](const auto& k){return k.key=="density";});
        check(dk!=kd.end(),"Life has a density setting for the typed-during-a-unit check");
        // Distinct from the current value, or staging it would rightly be a no-op.
        const float typed=dk!=kd.end()?dk->quantised(dk->value>0.5f?dk->value-0.1f:dk->value+0.1f):0.f;
        g.numberKey="density";g.numberRect=RECT{800,300,1000,332};
        WndProc(g.hwnd,WM_APP+2,0,0);
        check(g.numberEdit!=nullptr,"a number box opens for the typed-during-a-unit check");
        if(g.numberEdit)SetWindowTextW(g.numberEdit,ui::widen(std::to_string(typed)).c_str());
        g.running=true;g.acc=1.0;g.msPerStep=1000.0;
        stepFrame(0.016);
        check(workBusy(),"with a slow step in flight");
        WndProc(g.hwnd,WM_APP+3,1,0);
        check(g.numberEdit!=nullptr&&g.pendingSetup.empty()&&g.heldMessages.size()==1,
              "Enter in a number box during the unit is kept: not applied under it, and not lost");

        g.panning=true;g.orbiting=true;g.rightPan=true;
        WndProc(g.hwnd,WM_RBUTTONUP,0,MAKELPARAM(10,10));
        check(!g.panning&&!g.orbiting&&!g.rightPan,
              "a right-drag released during the unit ends, so the view does not follow the mouse afterwards");
        g.orbiting=true;g.rightPan=true;
        WndProc(g.hwnd,WM_MBUTTONUP,0,MAKELPARAM(10,10));
        check(!g.orbiting&&!g.rightPan,"and so does a middle-drag");

        check(g.frameCache!=nullptr&&g.frameW>0&&g.frameH>0,"the last composed frame is kept for paints during a unit");
        if(g.frameCache&&g.frameW>0&&g.frameH>0){
            HDC screen=GetDC(nullptr);
            HDC probe=CreateCompatibleDC(screen);
            HBITMAP bm=CreateCompatibleBitmap(screen,g.frameW+64,g.frameH+64);
            ReleaseDC(nullptr,screen);
            HGDIOBJ was=SelectObject(probe,bm);
            RECT all{0,0,g.frameW+64,g.frameH+64};
            FillRect(probe,&all,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            paintBusy(probe,all);
            check(GetPixel(probe,g.frameW+32,8)==RGB(0,0,0)&&GetPixel(probe,8,g.frameH+32)==RGB(0,0,0),
                  "a window enlarged during a unit shows blank where the kept frame does not reach, not stale pixels");
            SelectObject(probe,was);DeleteObject(bm);DeleteDC(probe);
        }

        waitReady();stepFrame(0.016);
        check(!workBusy()&&g.heldMessages.empty(),"the unit lands and gives back what it held");
        stepFrame(0.016);
        check(!workBusy(),"the next slow step waits while the held Enter is still queued");
        {
            MSG q{};const bool queued=PeekMessageW(&q,g.hwnd,WM_APP+3,WM_APP+3,PM_REMOVE)!=0;
            check(queued&&q.wParam==1,"the held Enter is back in the queue, unchanged");
            if(queued)WndProc(q.hwnd,q.message,q.wParam,q.lParam);
            const bool staged=std::any_of(g.pendingSetup.begin(),g.pendingSetup.end(),
                [&](const auto& p){return p.first=="density"&&std::fabs(p.second-typed)<1e-4f;});
            check(staged&&!g.numberEdit,"and runs then, staging the typed value exactly as if no unit had been in flight");
        }
        g.pendingSetup.clear();
        stepFrame(0.016);
        check(workBusy(),"after which the next slow step goes out");
        waitReady();stepFrame(0.016);g.running=false;

        // A click still going through keeps the next slow step back.
        g.running=true;g.acc=1.0;g.msPerStep=1000.0;
        g.ui.clicked=true;stepFrame(0.016);
        check(!workBusy(),"a click not yet laid out keeps the next slow step back, so it neither waits a unit nor is lost");
        g.ui.clicked=false;g.ui.down=true;stepFrame(0.016);
        check(!workBusy(),"so does a press still held");
        g.ui.down=false;stepFrame(0.016);
        check(workBusy(),"and the step goes out once the click is through");
        waitReady();stepFrame(0.016);g.running=false;

        // Time in flight counts toward the rate asked for. A sim whose step
        // waits for a signal makes "still in flight" certain, not a race.
        {
            struct Held:bench::Sim{
                HANDLE go=CreateEventW(nullptr,TRUE,FALSE,nullptr);
                std::uint64_t gen=0;
                bench::Field f{4,4};
                ~Held(){if(go)CloseHandle(go);}
                const bench::Provenance& about()const override{static const bench::Provenance p{};return p;}
                const std::vector<bench::Swatch>& palette()const override{static const std::vector<bench::Swatch> s{{{0,0,0},"x"}};return s;}
                const bench::Field& field()const override{return f;}
                void reset()override{gen=0;}
                void step()override{WaitForSingleObject(go,30000);++gen;}
                std::uint64_t generation()const override{return gen;}
            };
            g.sim=std::make_unique<Held>();
            auto* held=static_cast<Held*>(g.sim.get());
            const auto sps0=g.sps;g.sps=30;
            const double budget=1.0/double(g.sps);
            g.running=true;g.acc=budget;g.msPerStep=1000.0;g.ui.down=false;g.ui.clicked=false;
            stepFrame(0.016);
            check(workBusy(),"a step that waits for its signal goes out");
            const double a0=g.acc;
            stepFrame(0.1);
            check(workBusy()&&std::fabs(g.acc-std::min(1.0,a0+0.1))<1e-9,
                  "time spent in flight counts toward the next step, as it does on the window's own thread");
            SetEvent(held->go);waitReady();stepFrame(0.016);
            check(!workBusy()&&held->gen==1,"the signalled step lands");
            stepFrame(0.0);
            check(workBusy(),"and the next goes out the frame after, not a whole interval later");
            waitReady();stepFrame(0.016);
            check(!workBusy()&&held->gen==2,"and lands in turn");
            g.running=false;g.sps=sps0;
        }
        select(0);

        for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="gridworld"){select(i);break;}
        {MSG q;while(PeekMessageW(&q,g.hwnd,WM_APP,WM_APP+16,PM_REMOVE)){}}
        const int e0=g.sim->epoch_count();
        g.training=true;g.trainTo=0;
        check(g.msPerEpoch>50.0,"an epoch nobody has timed yet is treated as slow");
        stepFrame(0.016);
        check(workBusy(),"so the first epoch of a sim goes to the worker");
        waitReady();stepFrame(0.016);
        check(!workBusy()&&g.sim->epoch_count()==e0+1,"and lands as exactly one completed epoch");
        g.training=false;

        // A unit that throws is reported and paused exactly as an on-thread
        // failure is, through the same handlers.
        struct Thrower:bench::Sim{
            bench::Field f{4,4};
            const bench::Provenance& about()const override{static const bench::Provenance p{};return p;}
            const std::vector<bench::Swatch>& palette()const override{static const std::vector<bench::Swatch> s{{{0,0,0},"x"}};return s;}
            const bench::Field& field()const override{return f;}
            void reset()override{}
            void step()override{throw std::runtime_error("thrown on the worker");}
            std::uint64_t generation()const override{return 0;}
        };
        g.sim=std::make_unique<Thrower>();
        g.running=true;g.acc=1.0;g.msPerStep=1000.0;
        stepFrame(0.016);waitReady();stepFrame(0.016);
        check(!workBusy()&&!g.running&&g.toast.find("thrown on the worker")!=std::string::npos,
              "an exception from the worker reaches the window's own handler, which pauses and says why");
        select(0);
    }
    // Sorting is finite. Its two views share progress, and completion pauses playback.
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="sorting2d") {
        select(i);auto* sim=sortingLab();
        check(sim&&!sim->has_camera(),"2D sorting opens without camera controls");
        if(!sim)break;
        sim->step();const auto progress=sim->generation();const auto data=sim->engine().values;
        setControl("view",1);
        check(sim->has_camera()&&sim->generation()==progress&&sim->engine().values==data&&g.pendingSetup.empty(),"switching to 3D preserves sort and applies live");
        setControl("algorithm",4);
        check(!g.pendingSetup.empty()&&sim->generation()==progress,"algorithm choice stages without disturbing current sort");
        applySetup();check(sim->generation()==0&&sim->engine().algorithm==bench::sorting::Algorithm::Merge,"Apply restarts with the chosen algorithm");
        setControl("speed",512);g.running=true;g.msPerStep=0;g.acc=1;
        for(int k=0;k<100&&g.running;++k)stepFrame(.1);
        check(sim->finished()&&!g.running&&sim->engine().ordered(),"completed sort pauses through the real frame loop");
        // Printable shortcuts arrive as WM_CHAR after TranslateMessage.
        WndProc(g.hwnd,WM_CHAR,' ',0);
        check(g.running&&!sim->finished()&&sim->generation()==0,"Space replays a completed sort");
        g.running=false;select(0);break;
    }
    // The disparity sphere opens as its own entry, the flat circle drops the
    // camera live, and a rebuilt-from-beads sort finishes through the frame loop.
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].id=="sortingsphere") {
        select(i);auto* sim=sortingLab();
        check(sim&&sim->has_camera()&&sim->view()==bench::SortingSim::Sphere,"the disparity sphere opens with camera controls");
        if(!sim)break;
        layout(g.hwnd,mem);
        setControl("view",float(bench::SortingSim::Circle));
        check(!sim->has_camera()&&g.pendingSetup.empty(),"switching to the disparity circle applies live and drops the camera");
        setControl("algorithm",float(int(bench::sorting::Algorithm::Gravity)));
        applySetup();
        check(sim->generation()==0&&sim->engine().algorithm==bench::sorting::Algorithm::Gravity,"Apply restarts with Gravity");
        setControl("speed",512);g.running=true;g.msPerStep=0;g.acc=1;
        for(int k=0;k<100&&g.running;++k)stepFrame(.1);
        check(sim->finished()&&!g.running&&sim->engine().ordered()&&sim->engine().drops>0,"a gravity sort completes and pauses through the real frame loop");
        g.running=false;select(0);break;
    }
    // Build and load through the application's real asynchronous compiler path.
    // It starts from the project's own example plugin, which this exe finds by
    // walking up from itself (paths.hpp). A build outside the project tree
    // cannot find it, and the copy used to throw and take every later check and
    // the summary with it. Checked first, reported by path, and only this block
    // is skipped.
    namespace fs=std::filesystem;
    const std::string example=bench::Paths::get().inPlugins("example_rule.cpp");
    std::error_code exampleEc;
    const bool haveExample=fs::is_regular_file(example,exampleEc);
    check(haveExample,"the project's example plugin is found: "+example+" is missing, so the plugin "
          "build-and-load checks did not run. The project root is the first folder, from this exe's "
          "own up to four above it, holding both plugins\\ and native\\src, so build inside native\\");
    if(haveExample){
        const auto root=fs::temp_directory_path()/("bench plugin UI "+std::to_string(GetTickCount64()));fs::create_directories(root);
        const auto source=root/"new_rule.cpp", dll=root/"new_rule.dll";
        fs::copy_file(example,source);
        Row row;row.plugin=true;row.label="UI test plugin";row.source=source.string();row.dllPath=dll.string();
        g.rows.push_back(row);select(g.rows.size()-1);
        check(!g.sim && g.mode==Mode::Code,"source-only plugin opens in source view");
        buildPlugin();check(g.buildProcess!=nullptr,"compiler starts asynchronously");
        const auto start=GetTickCount64();
        while(g.buildProcess && GetTickCount64()-start<120000){pollBuild();Sleep(10);}
        check(!g.buildProcess&&g.buildOk&&g.sim,"new plugin builds and loads from path containing spaces");
        if(g.buildProcess){TerminateProcess(g.buildProcess,1);WaitForSingleObject(g.buildProcess,1000);CloseHandle(g.buildProcess);g.buildProcess=nullptr;}
        g.sim.reset();g.plug.unload();
        for(const auto& f:{source,dll,fs::path(dll.string()+".build.log")}){std::error_code ec;fs::remove(f,ec);}
        {std::error_code ec;fs::remove(root,ec);}
    }
    SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(g.hwnd,dc);
    std::printf("%d UI checks, %d failed\n",checks,failed);return failed?1:0;
}

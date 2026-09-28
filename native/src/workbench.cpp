// workbench.cpp — the desktop app.
//
// Win32 + GDI, statically linked, no vendored libraries. It draws one rectangle
// of indexed pixels plus chrome, and StretchDIBits does that without anyone
// having to build a dependency first.
//
// Desktop-app behaviour, which is mostly about not assuming a terminal:
//   · paths resolve from the EXE, never the working directory (see paths.hpp),
//     so double-click, shortcut and taskbar-pin all work
//   · no console window (-mwindows)
//   · window size and last-open sim persist between runs
//   · statically linked, because MSYS2 and Git-for-Windows both ship a
//     libstdc++-6.dll and picking the wrong one segfaults at startup
//
// Panes: roster left · sim centre with timeline · notes and plots right ·
// toolbars under the canvas.

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#undef near
#undef far

#include "registry.hpp"
#include "sims/rulespec.hpp"
#include "patterns.hpp"
#include "identify.hpp"
#include "render/raster.hpp"
#include "history.hpp"
#include "plugin.hpp"
#include "projects.hpp"
#include "paths.hpp"
#include "hardware.hpp"
#include "workflows.hpp"
#include "runner.hpp"
#include "sim_worker.hpp"
#include <chrono>
#include <memory>
#include <sstream>
#include <cstdlib>
#include "ui.hpp"
#include "render/png.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <functional>
#include <filesystem>

namespace {

// Layout is scaled from the display's DPI rather than fixed in pixels. At 150%
// the old constants gave 14px Consolas on a 2560-wide screen, which is why the
// text was small: the app was not DPI-aware, so Windows was not scaling it and
// neither was the app.
float gScale = 1.0f;
int kLeft = 248, kRight = 356, kTools = 84, kScrub = 24, kStatus = 26;
int kSplit = 5;   // grab band on each splitter, in pixels

// The DEFAULT widths at this scale. What the panes actually use is g.leftW and
// g.rightW, which start here and then follow the splitter.
void applyScale(float s) {
    gScale  = s;
    kLeft   = int(232 * s);
    kRight  = int(360 * s);
    kTools  = int( 58 * s);
    kScrub  = int( 26 * s);
    kStatus = int( 30 * s);
    kSplit  = std::max(4, int(5 * s));
}

enum class Mode { Run, Code };

struct Row {
    std::string label, era, source, id, blurb;  // id is the registry key, "" for plugins
    bool        plugin = false;
    std::size_t regIndex = 0;
    std::string dllPath;
    std::string projectPath;
    bool saved=false;
};

struct MenuItem { std::string label; std::function<void()> run; bool checked = false; bool enabled = true; };

struct App {
    std::vector<Row> rows;
    std::size_t      sel = std::size_t(-1);

    // Plugin BEFORE sim: members die in reverse, so the sim must be destroyed
    // while the DLL that created it is still mapped.
    bench::Plugin   plug;
    bench::SimPtr   sim;
    // One slow step or epoch off this thread; see stepFrame(). Declared AFTER
    // the sim so it is destroyed FIRST: its destructor waits for a unit still in
    // flight, and that unit must finish before the sim it is stepping goes away.
    std::unique_ptr<bench::SimWorker> work;

    bench::Raster   raster;
    bench::View     view;
    bench::History  hist;

    Mode   mode = Mode::Run;
    bool   running = true, watch = true, brush = false;
    int    sps = 30;
    // Training runs one EPOCH per frame rather than one step per tick. A
    // generation takes as long as it takes; doing them in a blocking loop
    // freezes the window for minutes, and a training run you cannot watch is
    // not much better than a log file. trainTo is a stop line: 0 means "keep
    // going", anything else halts the moment that epoch is reached.
    bool   training  = false;
    int    trainTo   = 0;
    // Stop when it stops improving. Every measurement in this project needed
    // "how long until it plateaus" answered, and counting generations by hand
    // off a plot is exactly the sort of thing the bench should do for you.
    bool   stopStale = false;
    bench::Plateau plateau;
    static constexpr int kStaleLimit = 15;
    // The notes panel scrolls. It did not, and the moment a sim reported five
    // metric series instead of three the last two plots simply fell off the
    // bottom of the window with nothing to say they were there.
    // How many rows of buttons the tool bar used last frame. The pane is sized
    // from it, so a wrapped bar gets the height it needs.
    int    toolRows  = 1;
    int    inspector = 0; // Controls, Measurements, Guide
    int    rosterScroll = 0, rosterContentH = 0;
    bool revealSelection = true;
    RECT   rosterRect{}, timelineTrack{};
    bool   advanced = false, displaySettings = false, placeAction = false;
    std::vector<std::pair<std::string, float>> pendingSetup;
    std::vector<MenuItem> menuItems;
    POINT menuPoint{};
    HWND numberEdit = nullptr;
    WNDPROC numberProc = nullptr;
    std::string numberKey;
    RECT numberRect{};
    RECT   notesRect{};
    int    notesScroll   = 0;
    int    notesContentH = 0;
    double acc = 0, msPerStep = 0;
    // An epoch's cost, measured like msPerStep. It starts ABOVE the off-thread
    // line, so the first epoch of a sim nobody has timed yet runs on the worker:
    // an epoch of unknown cost must not be able to freeze the window even once.
    static constexpr double kUnmeasured = 1000.0;
    double msPerEpoch = kUnmeasured;

    int    scrubAt = -1;
    std::vector<std::string> code;
    int    codeTop = 0;

    // build output pane
    std::vector<std::string> build;
    bool   buildOk = true, showBuild = false;
    HANDLE buildProcess = nullptr;
    std::string buildDll, buildLog;
    // A dedicated run: bench_run hosting a copy of this simulation in its own
    // process. See launchDedicated().
    HANDLE      dedicatedProcess = nullptr;
    std::string dedicatedOut, dedicatedLog, dedicatedLabel, dedicatedLine;
    std::string dedicatedFinished, dedicatedError, dedicatedTail;
    std::streamoff dedicatedPos = 0;
    DWORD       dedicatedPolled = 0, dedicatedStopAt = 0;
    bool        dedicatedRunning = false, dedicatedStopPending = false;
    // Off-thread stepping. See stepFrame() and paintBusy().
    enum class OffUnit { None, Step, Epoch };
    OffUnit     offUnit = OffUnit::None;
    int         offEpochBefore = 0;
    double      offMs = 0.0;            // how long the unit in flight took, measured on the worker
    DWORD       offStarted = 0;
    bool        pauseAfter = false;     // Pause pressed while a unit was in flight
    bool        dragEndPending = false; // a release that arrived while the sim was busy
    RECT        runButton{};            // where Run/Pause was last drawn, for clicks while busy
    HBITMAP     frameCache = nullptr;   // the last composed frame, re-shown while busy
    int         frameW = 0, frameH = 0;
    long long   layoutCalls = 0;        // counted, so a test can prove a busy paint never lays out
    std::vector<MSG> heldMessages;      // the app's own posted actions that arrived while busy

    HWND   hwnd = nullptr;
    HFONT  font = nullptr, bold = nullptr, mono = nullptr;
    ui::Ctx ui;
    std::vector<RECT> rowRects;
    std::vector<RECT> palRects;      // legend swatches, clickable: picks what to paint
    std::vector<RECT> switchRects;   // rule bits, clickable: edits the transition rule
    RECT   scrubRect{}, canvasRect{};

    // ── how much of the window the simulation actually gets ─────────────────
    //
    // The canvas used to be whatever was left after two fixed side panels. On
    // this machine that is 402 + 588 of 2560 pixels, so 39% of the width was
    // chrome and the sim got the rest whether or not anything in the panels
    // was being read. Three ways out, because they answer different questions:
    // drag a splitter when you want a bit more room, Tab when you want the
    // panels gone for a minute, F11 when you want the wall.
    //
    // Widths are pixels, not fractions. A fraction re-flows the roster every
    // time the window resizes; a pixel width means the panel you sized stays
    // the size you made it and the canvas absorbs the change, which is the way
    // round you want when the canvas is the thing you are looking at.
    int    leftW = 0, rightW = 0;      // 0 until the first layout: take the scaled default
    bool   showLeft = false, showRight = true;
    bool library = true, largeRun = false, suppressClick = false;
    HWND librarySearch = nullptr;
    WNDPROC librarySearchProc = nullptr;
    std::string libraryQuery, libraryCategory = "All simulations";
    std::string currentProject;
    bool autoSave=false;
    bool   focus = false;              // canvas only: no panels, no tools, no status
    bool   fullscreen = false;
    int    splitDrag = 0;              // 0 none, -1 dragging the left splitter, +1 the right
    RECT   leftSplit{}, rightSplit{};  // the grab bands, recomputed every layout
    WINDOWPLACEMENT prevPlace{};       // to restore from fullscreen
    DWORD  prevStyle = 0;
    std::string ruleText = "B3/S23";  // editor draft; RuleSim::spec() is the applied rule
    bool   ruleFocus = false;
    HWND ruleEdit = nullptr;
    WNDPROC ruleEditProc = nullptr;
    RECT   ruleRect{};
    std::string ruleMsg;              // parse error, shown under the field
    std::string identity;             // last identify() result, shown in the header
    RECT   tipLast{};                 // which widget the cursor was over last frame
    DWORD  tipSince = 0;              // when it arrived there
    int    brushSize = 2;            // radius in cells; 0 paints a single cell
    int    paintIdx  = -1;           // -1 = whatever the sim calls its paint value
    bool   panning = false, scrubbing = false, orbiting = false, rightPan = false;
    bool   dragging = false;      // the SIM grabbed something, not the camera
    bool   dragSpeed = false, dragTrail = false, dragGamma = false;
    std::vector<bool> knobDrag;
    POINT  lastPt{};
    int    curCellX = -1, curCellY = -1;
    std::string toast; DWORD toastAt = 0;
    DWORD  lastWatch = 0;

    bench::Hardware  hw;
    bench::RateGuard guard;
    bool   throttled = false;
    int    lastAllowed = 0;
    DWORD  lastProbe = 0;

    void say(const std::string& s) { toast = s; toastAt = GetTickCount(); }
};
App g;

const bench::Workflow& currentWorkflow() {
    return bench::workflow(g.sel < g.rows.size() ? g.rows[g.sel].id : "");
}

// Built-in rule controls must not reinterpret a custom DLL's implementation,
// or pass --rule to a runner whose target is a plugin rather than Rule lab.
bench::RuleSim* ruleLab() {
    if(g.sel>=g.rows.size()||g.rows[g.sel].plugin||g.rows[g.sel].id!="rule")return nullptr;
    return dynamic_cast<bench::RuleSim*>(g.sim.get());
}

void queueMenu(RECT anchor, std::vector<MenuItem> items) {
    g.ui.clicked = false;
    g.menuItems = std::move(items);
    g.menuPoint = POINT{anchor.left, anchor.bottom};
    PostMessageW(g.hwnd, WM_APP + 1, 0, 0);
}

void cancelNumber() {
    if (g.numberEdit) { HWND h = g.numberEdit; g.numberEdit = nullptr; DestroyWindow(h); }
    g.numberKey.clear();
}

void closeRuleEditor(bool discard=false) {
    if(g.ruleEdit){const HWND edit=g.ruleEdit;g.ruleEdit=nullptr;DestroyWindow(edit);}
    g.ruleFocus=false;
    if(discard) {
        if(auto* sim=ruleLab())g.ruleText=sim->spec().text;
        g.ruleMsg.clear();
    }
}

LRESULT CALLBACK RuleEditProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_KEYDOWN) {
        if(w==VK_RETURN||w==VK_ESCAPE) {PostMessageW(g.hwnd,WM_APP+9,w==VK_RETURN,0);return 0;}
        if(w=='A'&&GetKeyState(VK_CONTROL)<0) {SendMessageW(h,EM_SETSEL,0,-1);return 0;}
        if(w==VK_TAB) {SetFocus(g.hwnd);return 0;}
        if((w=='S'||w=='O')&&GetKeyState(VK_CONTROL)<0) {PostMessageW(g.hwnd,WM_APP+7,w=='S',0);return 0;}
    }
    if(m==WM_CHAR&&(w==VK_RETURN||w==VK_ESCAPE||w==VK_TAB||w==1))return 0;
    if(m==WM_KILLFOCUS)PostMessageW(g.hwnd,WM_APP+10,WPARAM(h),0);
    if(m==WM_MOUSEWHEEL){PostMessageW(g.hwnd,m,w,l);return 0;}
    return CallWindowProcW(g.ruleEditProc,h,m,w,l);
}

void beginRuleEditor() {
    if(g.ruleEdit){SetFocus(g.ruleEdit);return;}
    const RECT r=g.ruleRect;
    if(!ruleLab()||r.right<=r.left||r.top<g.notesRect.top||r.bottom>g.notesRect.bottom)return;
    cancelNumber();
    g.ruleEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",ui::widen(g.ruleText).c_str(),
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,r.left,r.top,r.right-r.left,r.bottom-r.top,
        g.hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g.ruleEdit)return;
    SendMessageW(g.ruleEdit,WM_SETFONT,WPARAM(g.font),TRUE);
    SendMessageW(g.ruleEdit,EM_LIMITTEXT,2048,0);
    SendMessageW(g.ruleEdit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(6,6));
    g.ruleEditProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.ruleEdit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(RuleEditProc)));
    g.ruleFocus=true;SetFocus(g.ruleEdit);SendMessageW(g.ruleEdit,EM_SETSEL,0,-1);
}

void syncRuleEditor() {
    if(!g.ruleEdit)return;
    const RECT r=g.ruleRect;
    if(g.library||g.focus||!g.showRight||r.right<=r.left||r.top<g.notesRect.top||r.bottom>g.notesRect.bottom) {closeRuleEditor();return;}
    RECT old{};GetWindowRect(g.ruleEdit,&old);MapWindowPoints(nullptr,g.hwnd,reinterpret_cast<POINT*>(&old),2);
    if(!EqualRect(&old,&r))SetWindowPos(g.ruleEdit,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
}

void resetRun() {
    if (!g.sim) return;
    g.training = false; g.trainTo = 0; g.plateau.reset();
    g.sim->reset(); g.hist.clear(); g.raster.clear_accumulator();
    g.scrubAt = -1; g.acc = 0; g.identity.clear();
}

float controlValue(const bench::Knob& k) {
    for (const auto& p : g.pendingSetup) if (p.first == k.key) return p.second;
    return k.value;
}

void setControl(const std::string& key, float value) {
    if (!g.sim) return;
    for (const auto& k : g.sim->knobs()) if (k.key == key) {
        value = k.quantised(value);
        if (k.on_reset) {
            auto it = std::find_if(g.pendingSetup.begin(), g.pendingSetup.end(), [&](const auto& p) { return p.first == key; });
            if (value == k.value) { if (it != g.pendingSetup.end()) g.pendingSetup.erase(it); }
            else if (it != g.pendingSetup.end()) it->second = value;
            else g.pendingSetup.emplace_back(key, value);
        } else g.sim->on_knob(key, value);
        return;
    }
}

void applySetup() {
    if (!g.sim || g.pendingSetup.empty()) return;
    const auto settings = std::move(g.pendingSetup); g.pendingSetup.clear();
    for (const auto& p : settings) g.sim->on_knob(p.first, p.second);
    resetRun();
    g.say("Setup applied. Started a new run.");
}

LRESULT CALLBACK NumberProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        PostMessageW(g.hwnd, WM_APP + 3, w == VK_RETURN, 0); return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return CallWindowProcW(g.numberProc, h, m, w, l);
}

void queueNumber(const std::string& key, RECT r) {
    g.numberKey = key; g.numberRect = r; g.ui.clicked = false;
    PostMessageW(g.hwnd, WM_APP + 2, 0, 0);
}

[[maybe_unused]] std::string cfgPath() { return bench::Paths::get().data() + "/workbench.cfg"; }
bool saveCurrent(std::string path,bool named,std::string& error);
void simulationFile(bool save);
bool rememberCurrent();
bool workBusy();

// ── possession ──────────────────────────────────────────────────────────────
//
// Driving one agent of voxelcity from the keyboard, so the bench finally has a
// HUMAN BASELINE to put next to its scripted (~5) and learned (~1) numbers.
//
// This is the only sim-specific code in the workbench, and it is here rather
// than behind a new Sim virtual on purpose: possession needs an action space,
// and "the action space" is not a thing sims in general have. A virtual would
// have to be a stringly-typed command channel that every other sim implements
// as {} — twenty-eight empty overrides to avoid one dynamic_cast.
[[nodiscard]] bench::VoxelCity* possessable() {
    return dynamic_cast<bench::VoxelCity*>(g.sim.get());
}

// Which key does what. Chosen after reading the whole WM_KEYDOWN and WM_CHAR
// handler, because this window is dense with hotkeys and a collision is silent:
// the second handler simply never runs.
//
// TAKEN, and therefore avoided: F1 F2 F3 F5-F9 F11 TAB arrows PgUp PgDn 'H'
// (camera home) in WM_KEYDOWN, and '1' '2' '3' '4' '5' '7' '8' '0' 'F' (the
// standard views and zoom-to-fit), space, '.' ',' 'R' 'B' '[' ']' in WM_CHAR.
//
// FREE and now used: W A S D E Q (movement), T Y U I O (the five Go options),
// Z X C V N M (mine, craft, place, eat, explore, ascend), P (possess/release),
// K (next agent). '6' '9' G J L remain free.
struct PossKey { unsigned vk; int code; const char* what; };
const PossKey kPossKeys[] = {
    // Movement is WASD with E/Q for up and down, the layout every voxel game
    // uses. The sim's facings are 0 -x, 1 +x, 2 -z, 3 +z, 4 up, 5 down.
    {'W', bench::VoxelCity::kMoveBase + 2, "W  forward (-z)"},
    {'S', bench::VoxelCity::kMoveBase + 3, "S  back (+z)"},
    {'A', bench::VoxelCity::kMoveBase + 0, "A  left (-x)"},
    {'D', bench::VoxelCity::kMoveBase + 1, "D  right (+x)"},
    {'E', bench::VoxelCity::kMoveBase + 4, "E  up"},
    {'Q', bench::VoxelCity::kMoveBase + 5, "Q  down"},
    {'Z', bench::VoxelCity::Mine,          "Z  mine"},
    {'X', bench::VoxelCity::Craft,         "X  craft"},
    {'C', bench::VoxelCity::PlaceBlock,    "C  place"},
    {'V', bench::VoxelCity::Eat,           "V  eat"},
    {'N', bench::VoxelCity::Explore,       "N  explore"},
    {'M', bench::VoxelCity::Ascend,        "M  ascend"},
    {'T', bench::VoxelCity::GoWood,        "T  go wood"},
    {'Y', bench::VoxelCity::GoStone,       "Y  go stone"},
    {'U', bench::VoxelCity::GoCoal,        "U  go coal"},
    {'I', bench::VoxelCity::GoIron,        "I  go iron"},
    {'O', bench::VoxelCity::GoDiamond,     "O  go diamond"},
};

// Returns true if the key was consumed. Keys are only consumed while somebody
// is actually being driven, so with possession off they stay free for whatever
// gets bound to them next.
bool possessKey(unsigned vk) {
    bench::VoxelCity* vc = possessable();
    if (!vc) return false;

    if (vk == 'P') {
        if (vc->possession_live()) {
            vc->release();
            g.say("released — the agent is back on its own policy  ·  P to take it again");
        } else {
            vc->possess(0);
            g.say("POSSESSED agent 0 (the amber one)  ·  WASD+QE move  ·  Z mine  X craft  "
                  "C place  V eat  N explore  M ascend  ·  TYUIO go wood/stone/coal/iron/"
                  "diamond  ·  K next agent  ·  P release");
        }
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    if (!vc->possession_live()) return false;

    if (vk == 'K') {
        const int n = int(vc->agents().size());
        vc->possess(std::size_t((vc->possessed_index() + 1) % std::max(1, n)));
        g.say("now driving agent " + std::to_string(vc->possessed_index()) +
              " — the recording starts again from here");
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    for (const auto& k : kPossKeys)
        if (k.vk == vk) {
            vc->possess_command(k.code);
            // A queued command that the sim never steps is a key that did
            // nothing, and "I pressed it and nothing happened" is exactly the
            // inert control this project keeps finding. Say so instead.
            if (!g.running)
                g.say(std::string(k.what) + " queued — the sim is PAUSED, space to run it");
            InvalidateRect(g.hwnd, nullptr, FALSE);
            return true;
        }
    return false;
}

// ── roster ──────────────────────────────────────────────────────────────────
bool projectRow(std::string path,bool saved,Row& row,std::string& error) {
    path=bench::path_text(bench::path_from_utf8(path));
    namespace p=bench::projects;p::Document document;
    if(!p::read(bench::path_from_utf8(path),document,error))return false;
    row={};row.projectPath=path;row.saved=saved;row.label=document.name;
    row.era=saved?"Saved simulations":"Templates";
    row.blurb="Saved code and setup. Open to start a new run.";
    if(!document.model.empty()) {
        const auto& reg=bench::registry();
        for(std::size_t i=0;i<reg.size();++i)if(reg[i].id==document.model) {
            row.id=document.model;row.regIndex=i;row.source=bench::Paths::get().inSrc(reg[i].source);
            if(!saved)row.era=reg[i].era;
            return true;
        }
        error="This simulation uses an unavailable model: "+document.model;return false;
    }
    row.plugin=true;row.source=p::utf8(p::resolve(bench::path_from_utf8(path),document.source));
    row.dllPath=p::utf8(p::resolve(bench::path_from_utf8(path),document.dll));return true;
}
void rebuildRows() {
    const bool hadSelection=g.sel<g.rows.size();
    const Row previous=hadSelection?g.rows[g.sel]:Row{};
    const auto active = g.sel < g.rows.size() ? (g.rows[g.sel].projectPath.empty()?g.rows[g.sel].source:g.rows[g.sel].projectPath) : std::string{};
    g.rows.clear();
    g.sel=std::size_t(-1);
    const auto& reg = bench::registry();
    for (std::size_t i = 0; !bench::Paths::get().installed()&&i < reg.size(); ++i) {
        Row r; r.era = reg[i].era; r.id = reg[i].id;
        r.label = bench::catalog_title(r.id);
        r.blurb = bench::workflow(r.id).hint;
        r.source = bench::Paths::get().inSrc(reg[i].source);
        r.regIndex = i;
        g.rows.push_back(std::move(r));
    }
    auto addProjects=[&](const std::string& directory,bool saved) {
        for(const auto& file:bench::projects::files(bench::path_from_utf8(directory))) {
            Row row;std::string error;
            if(projectRow(bench::projects::utf8(file),saved,row,error))g.rows.push_back(std::move(row));
        }
    };
    if(bench::Paths::get().installed())addProjects(bench::Paths::get().templates(),false);
    addProjects(bench::Paths::get().projects(),true);
    {std::ifstream in(bench::path_from_utf8(bench::Paths::get().data()+"/saved-locations.txt"));std::string path;
     for(int count=0;count<1000&&(in>>std::quoted(path));++count) {
         if(std::any_of(g.rows.begin(),g.rows.end(),[&](const Row& r){return r.projectPath==path;}))continue;
         Row row;std::string error;if(projectRow(path,true,row,error))g.rows.push_back(std::move(row));
     }}
    {Row row;std::string error;const auto path=bench::Paths::get().data()+"/last-workspace.benchsim";
     if(projectRow(path,true,row,error)){row.label="Last workspace: "+row.label;g.rows.push_back(std::move(row));}}
    for (const auto& file : bench::plugin_files(bench::Paths::get().plugins())) {
        Row r; r.plugin = true; r.dllPath = file.dll; r.era = "Your plugins";
        r.source = file.source; r.label = file.name;
        r.blurb = "Edit the source and choose Build. The DLL reloads after a successful build.";
        g.rows.push_back(std::move(r));
    }
    for (std::size_t i=0;i<g.rows.size();++i) if((g.rows[i].projectPath.empty()?g.rows[i].source:g.rows[i].projectPath)==active) {g.sel=i;break;}
    // Refresh must not assign a running simulation to an unrelated library row.
    if(hadSelection&&g.sel>=g.rows.size()){g.sel=g.rows.size();g.rows.push_back(previous);}
}

void closeLibrary() {
    if(g.librarySearch) {DestroyWindow(g.librarySearch);g.librarySearch=nullptr;}
    g.library=false;g.showLeft=false;
    SetFocus(g.hwnd);
}

void openLibrary() {
    cancelNumber();closeRuleEditor();g.running=false;g.training=false;g.trainTo=0;
    g.library=true;g.focus=false;g.rosterScroll=0;g.ui.clicked=false;
    PostMessageW(g.hwnd,WM_APP+4,0,0);
    InvalidateRect(g.hwnd,nullptr,FALSE);
}

void setLargeRun(bool enabled) {
    g.largeRun=enabled;g.hist.set_record_snapshots(!enabled);g.scrubAt=-1;
    g.say(enabled ? "Large run: timeline off, display up to 15 fps. Measurements remain available."
                  : "Balanced: timeline recording on, display up to 60 fps.");
}

void loadCode(const std::string& path) {
    g.code.clear(); g.codeTop = 0;
    std::ifstream in(bench::path_from_utf8(path));
    std::string line;
    while (std::getline(in, line)) g.code.push_back(line);
    if (g.code.empty()) g.code.push_back("(could not open " + path + ")");
}

// A plugin can ask for any field it likes, so every path that adopts a sim has
// to pass through here. select() used to check and reloadPlugin() did not —
// and reloadPlugin is called automatically by the 400 ms watch timer, so a
// rebuilt plugin asking for a 20000^2 field was adopted with nobody touching
// anything.
//
// Budgeted in BYTES, not cells: the renderer commits 12 bytes per cell for the
// accumulation buffer alone, plus one for the field and one for its back
// buffer. A cell-denominated limit let a 64 M-cell field through and then
// needed 768 MB of accumulator before drawing a single pixel.
bool admit(const std::string& label) {
    if (!g.sim) return false;
    const std::size_t cells = g.sim->field().cells.size();
    const std::size_t need  = cells * 14;
    const std::size_t cap   = std::max<std::size_t>(g.hw.availRam / 4, 64u*1024u*1024u);
    if (need > cap) {
        char b[220];
        std::snprintf(b, sizeof b,
            "refused: %s wants %.1f M cells (%.0f MB to render), limit is %.0f MB free-RAM budget",
            label.c_str(), double(cells)/1e6, double(need)/1048576.0, double(cap)/1048576.0);
        g.say(b);
        g.sim.reset();
        return false;
    }
    g.knobDrag.assign(g.sim->knobs().size(), false);
    return true;
}

// Parse, and only swap the sim in if it parsed. A bad rulestring leaves what
// is running alone and says why — it must never quietly become a different rule.
void applyRule() {
    if(!ruleLab())return;
    const bench::RuleSpec r = bench::parse_rule(g.ruleText);
    if (!r.ok) { g.ruleMsg = r.error; return; }
    auto sim = bench::make_rule_workspace(g.ruleText);
    if (!sim) { g.ruleMsg = "could not build that rule"; return; }
    if (ruleLab()) {
        bench::projects::Document setup;bench::projects::capture_settings(*g.sim,setup);
        for(const auto& [key,value]:g.pendingSetup)
            for(auto& setting:setup.knobs)if(setting.first==key)setting.second=value;
        if(!bench::projects::apply_settings(*sim,setup,g.ruleMsg))return;
    }
    cancelNumber();g.pendingSetup.clear();
    g.sim = std::move(sim);
    g.hist.clear(); g.raster.clear_accumulator(); g.scrubAt = -1;
    g.knobDrag.assign(g.sim->knobs().size(), false);
    g.ruleMsg.clear();g.ruleText=r.text;g.running=false;g.acc=0;g.identity.clear();
    g.paintIdx=-1;g.msPerStep=0;
    g.say("Applied " + r.text + ". New run ready; press Play.");
}

// Drop a pattern file on the window to load it. This is the point of the RLE
// reader: until now the only way to give the bench an interesting starting
// state was to edit C++ and rebuild.
//
// Pasted CENTRED and CLIPPED, never wrapped — a pattern larger than the field
// must lose its edges rather than write out of bounds.
void loadPatternFile(const std::string& path) {
    if (!g.sim) return;
    bench::Field* f = g.sim->editable();
    if (!f) { g.say("this sim renders a derived view — nothing to paste into"); return; }

    auto p = bench::load_pattern(path);
    if (!p.ok) { g.say("could not load: " + p.error); return; }

    // A rule in an RLE header is part of the pattern, not just a caption.
    // Refuse before clearing the field if this cellular model uses another rule.
    const auto activeRule=bench::parse_rule(g.sim->subtitle());
    if(!p.rule.empty()&&activeRule.ok) {
        const auto patternRule=bench::parse_rule(p.rule);
        if(!patternRule.ok||patternRule.text!=activeRule.text) {
            g.say("Pattern uses "+p.rule+"; current rule is "+activeRule.text+". Apply the matching rule in Rule lab, then import again.");
            return;
        }
    }
    if(activeRule.ok&&std::any_of(p.cells.begin(),p.cells.end(),[&](auto state){return state>=activeRule.states;})) {
        g.say("This pattern contains states outside the current rule's "+std::to_string(activeRule.states)+" states.");return;
    }

    const int ox = (f->w - p.w) / 2, oy = (f->h - p.h) / 2;
    f->fill(0);
    bench::paste(*f, p, ox, oy);
    g.hist.clear(); g.raster.clear_accumulator(); g.scrubAt = -1;

    std::string msg = p.title() + "  " + std::to_string(p.w) + "x" + std::to_string(p.h);
    if (!p.rule.empty()) msg += "  rule " + p.rule;
    // Say when a pattern carries no attribution, rather than showing an
    // anonymous blob as though its origin were known.
    if (!p.cited()) msg += "   (no attribution in the file)";
    if (p.w > f->w || p.h > f->h) msg += "   — CLIPPED to the field";
    g.say(msg);
}

// Write what is on screen as RLE, next to the exe. A finding that cannot leave
// the workbench is not much of a finding.
// Write every recorded metric series to CSV next to the exe. A run that cannot
// leave the window cannot be compared with another run, and comparing runs is
// most of what measuring a learning agent consists of.
void saveMetrics() {
    if (!g.sim) return;
    const std::string id = g.sel < g.rows.size() && !g.rows[g.sel].id.empty() ? g.rows[g.sel].id : "plugin";
    const std::string name = bench::export_path(bench::Paths::get().exeDir(),
        "metrics-" + id + "-" + std::to_string(g.sim->generation()) + ".csv");
    bool ok = g.hist.write_csv(name);
    std::string msg = ok ? (std::string("wrote ") + name) : "could not write the metrics file";
    // A learning sim gets a second file on its own clock. The two are not
    // interchangeable: one row per frame and one row per generation answer
    // different questions, and merging them would make both unreadable.
    if (g.sim->epoch_name() && g.hist.epoch_samples() > 0) {
        const auto en = bench::export_path(bench::Paths::get().exeDir(),
            "metrics-" + id + "-by-" + g.sim->epoch_name() + ".csv");
        if (g.hist.write_csv(en, bench::History::Axis::Epoch))
            msg += std::string("  +  ") + en;
        else msg += "  (epoch CSV could not be written)";
    }
    g.say(msg);
}

void savePattern() {
    if (!g.sim) return;
    char name[128];
    std::snprintf(name, sizeof name, "%s/pattern-gen%llu.rle",
                  bench::Paths::get().exeDir().c_str(),
                  (unsigned long long)g.sim->generation());
    std::ofstream out(name);
    if (!out) { g.say("could not write the pattern file"); return; }
    out << bench::to_rle(g.sim->field(), g.sim->subtitle(),
                         g.sim->about().title, g.sim->generation());
    g.say(std::string("saved ") + name);
}

// Identify what is on screen: run it forward, classify, then put it back.
//
// identify() steps the sim, so the field is copied first and restored after —
// the user asked what this is, not to be advanced 300 generations. That only
// works for sims that expose their real state; the derived-view ones say so.
void identifyPattern() {
    if (!g.sim) return;
    bench::Field* f = g.sim->editable();
    if (!f) { g.say("this sim renders a derived view — nothing to identify"); return; }

    const std::vector<std::uint8_t> keep = f->cells;
    const std::uint64_t genBefore = g.sim->generation();
    const auto id = bench::identify(*g.sim, 1200);
    f->cells = keep;                       // the field goes back exactly as it was
    g.raster.clear_accumulator();
    g.identity = id.describe();

    // The generation COUNTER cannot be put back — Sim owns it and identifying
    // costs real steps to do. Say so rather than leave a counter that quietly
    // disagrees with the field it is labelling.
    const std::uint64_t spent = g.sim->generation() - genBefore;
    char note[220];
    std::snprintf(note, sizeof note, "identified: %s   (took %llu generations; field restored, "
                  "counter not)", g.identity.c_str(), (unsigned long long)spent);
    g.say(note);
}

bool saveCurrent(std::string destination,bool named,std::string& error) {
    namespace p=bench::projects;
    destination=p::utf8(bench::path_from_utf8(destination));
    if(g.sel>=g.rows.size()||(!g.sim&&!g.rows[g.sel].plugin)) {error="Open a simulation first.";return false;}
    if(g.buildProcess) {error="Wait for the current build to finish before saving or closing.";return false;}
    const Row row=g.rows[g.sel];p::Document document;
    {std::string ignored;
     if(!row.projectPath.empty())p::read(bench::path_from_utf8(row.projectPath),document,ignored);}
    document.name=named?p::utf8(bench::path_from_utf8(destination).stem()):
        (destination==g.currentProject&&!document.name.empty()?document.name:row.label);
    document.model=row.plugin?"":row.id;
    document.source=row.plugin?row.source:"";document.dll=row.plugin?row.dllPath:"";
    if(g.sim) {
        document.rule.clear();document.body.clear();
        document.knobs.clear();document.switches.clear();p::capture_settings(*g.sim,document);
        for(const auto& [key,value]:g.pendingSetup)for(auto& setting:document.knobs)if(setting.first==key)setting.second=value;
        if(auto* rule=ruleLab())document.rule=rule->spec().text;
        if(auto* body=dynamic_cast<bench::Locomotion*>(g.sim.get()))document.body=body->saveBody();
        else if(auto* collision=dynamic_cast<bench::CollisionLab*>(g.sim.get()))document.body=collision->saveScene();
        else if(auto* fly=dynamic_cast<bench::FlyArena*>(g.sim.get()))document.body=fly->saveScene();
    }
    if(!p::save(bench::path_from_utf8(destination),document,error))return false;
    if(named) {
        const auto index=bench::path_from_utf8(bench::Paths::get().data()+"/saved-locations.txt");
        std::set<std::string> paths;std::ifstream in(index);std::string path;
        for(int count=0;count<1000&&(in>>std::quoted(path));++count)paths.insert(path);
        in.close(); // Windows cannot replace the index while this reader holds it.
        paths.insert(destination);std::ostringstream out;
        for(const auto& entry:paths)out<<std::quoted(entry)<<'\n';
        if(!p::atomic_write(index,out.str(),error))return false;
        g.currentProject=destination;
        g.rows[g.sel].projectPath=destination;g.rows[g.sel].saved=true;g.rows[g.sel].label=document.name;
    }
    return true;
}
bool rememberCurrent() {
    if(!g.autoSave||g.sel>=g.rows.size()||(!g.sim&&!g.rows[g.sel].plugin))return true;
    std::string error;const auto path=g.currentProject.empty()?bench::Paths::get().data()+"/last-workspace.benchsim":g.currentProject;
    if(saveCurrent(path,false,error))return true;
    g.say("Could not save your workspace: "+error);return false;
}

void select(std::size_t i) {
    if (i >= g.rows.size()) return;
    if(g.autoSave&&!rememberCurrent())return;
    Row requested=g.rows[i];bench::projects::Document document;
    if(!requested.projectPath.empty()) {
        std::string error;
        if(!projectRow(requested.projectPath,requested.saved,requested,error)||
           !bench::projects::read(bench::path_from_utf8(requested.projectPath),document,error)) {g.say(error);return;}
        g.rows[i]=requested;
    }
    closeLibrary();g.mode=Mode::Run;
    cancelNumber();closeRuleEditor(); g.pendingSetup.clear(); g.placeAction = false;
    g.brush = false; g.paintIdx = -1; g.advanced = false; g.displaySettings = false;
    g.inspector = 0; g.ruleFocus = false;g.revealSelection=true;
    g.sel = i;
    Row& r = g.rows[i];
    g.currentProject=r.saved?r.projectPath:std::string{};
    g.training = false; g.trainTo = 0;   // the old sim's epochs mean nothing here
    g.plateau.reset();
    g.hist.clear_baseline();             // a curve from another sim is not a baseline
    g.notesScroll = 0;                   // a new panel starts at its top
    g.sim.reset();                       // die before the code that made you
    g.hist.clear();                      // release the old world BEFORE allocating the next
    g.raster=bench::Raster{};g.raster.set_threads(g.hw.workers);
    g.msPerStep=0;g.msPerEpoch=App::kUnmeasured;g.throttled=false;g.lastAllowed=0;
    try {
    if (r.plugin) {
        if (!std::filesystem::exists(bench::path_from_utf8(r.dllPath))) { g.plug.unload(); g.say("Source saved. Choose Build to run this simulation."); g.mode = Mode::Code; }
        else if (g.plug.load(r.dllPath)) { g.sim = g.plug.make(); g.say("loaded " + r.label); }
        else g.say("plugin failed: " + g.plug.error());
    } else {
        g.plug.unload();
        g.sim = r.id=="rule"?bench::make_rule_workspace(document.rule.empty()?"B3/S23":document.rule):bench::registry()[r.regIndex].make();
        if(!g.sim)throw std::runtime_error("This simulation's saved rule is invalid.");
        if(r.id=="rule")g.ruleText=static_cast<bench::RuleSim*>(g.sim.get())->spec().text;
        g.ruleMsg.clear();
        g.say("loaded " + r.label);
    }
    if(g.sim&&!r.projectPath.empty()) {
        std::string error;
        if(!bench::projects::apply_settings(*g.sim,document,error))throw std::runtime_error(error);
        if(!document.body.empty()) {
            if(auto* loco=dynamic_cast<bench::Locomotion*>(g.sim.get())) {
                if(!loco->loadBody(document.body))throw std::runtime_error("Could not restore the saved creature body.");
            } else if(auto* collision=dynamic_cast<bench::CollisionLab*>(g.sim.get())) {
                if(!collision->loadScene(document.body))throw std::runtime_error("Could not restore the saved collision scene.");
            } else if(auto* fly=dynamic_cast<bench::FlyArena*>(g.sim.get())) {
                if(!fly->loadScene(document.body))throw std::runtime_error("Could not restore the saved fly arena.");
            } else throw std::runtime_error("This simulation has no place for the saved body data.");
        }
        g.say("Opened "+r.label+". Code and setup restored; ready for a new run.");
    }
    } catch(const std::exception& e) {
        g.sim.reset();g.running=false;g.say(std::string("Could not open simulation: ")+e.what());
    }
    g.identity.clear();
    g.hist.clear();
    g.hist.configure(g.hw.historyBudget, 4);
    g.raster.clear_accumulator();
    const float keepTrail = g.view.trail, keepGamma = g.view.gamma;
    const bool keepGrid = g.view.grid;
    g.view = bench::View{};
    g.view.trail = keepTrail; g.view.gamma = keepGamma; g.view.grid = keepGrid;
    g.scrubAt = -1; g.acc = 0;
    if (g.sim) admit(r.label);
    loadCode(r.source);
}

void simulationFile(bool save) {
    namespace p=bench::projects;
    if(workBusy())return;
    g.running=false;g.training=false;g.trainTo=0;
    wchar_t file[32768]{};std::wstring initial=ui::widen(bench::Paths::get().projects());
    if(save) {
        if(g.sel>=g.rows.size())return;
        std::string name=g.rows[g.sel].label;
        for(char& c:name)if(std::string("<>:\"/\\|?*").find(c)!=std::string::npos)c='_';
        const auto title=ui::widen(name+".benchsim");wcsncpy(file,title.c_str(),32767);
    }
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g.hwnd;
    dialog.lpstrFilter=L"Workbench simulations (*.benchsim)\0*.benchsim\0\0";
    dialog.lpstrFile=file;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"benchsim";dialog.lpstrInitialDir=initial.c_str();
    dialog.lpstrTitle=save?L"Save simulation - code and setup":L"Open simulation";
    dialog.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(!(save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)))return;
    const auto path=p::fs::path(file);std::string error;
    if(save) {
        if(!saveCurrent(p::utf8(path),true,error)){g.say("Save failed: "+error);return;}
        rebuildRows();g.say("Simulation saved. Its code and setup will be here when you reopen the workbench.");
    } else {
        Row row;if(!projectRow(p::utf8(path),true,row,error)){g.say(error);return;}
        p::Document document;if(!p::read(path,document,error)){g.say(error);return;}
        document.source=p::utf8(p::resolve(path,document.source));document.dll=p::utf8(p::resolve(path,document.dll));
        auto target=bench::path_from_utf8(bench::Paths::get().projects())/path.filename();
        if(path!=target&&p::fs::exists(target))target=target.parent_path()/bench::path_from_utf8(p::utf8(path.stem())+"-"+p::token()+".benchsim");
        if(path!=target&&!p::save(target,document,error)){g.say(error);return;}
        rebuildRows();
        for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].projectPath==p::utf8(target)){select(i);break;}
    }
    InvalidateRect(g.hwnd,nullptr,FALSE);
}

void reloadPlugin(bool quiet = false) {
    if (g.sel >= g.rows.size() || !g.rows[g.sel].plugin) return;
    bench::projects::Document settings;std::string restoreError;
    if(g.sim)bench::projects::capture_settings(*g.sim,settings);
    else if(!g.rows[g.sel].projectPath.empty())
        bench::projects::read(bench::path_from_utf8(g.rows[g.sel].projectPath),settings,restoreError);
    cancelNumber(); g.pendingSetup.clear(); g.training=false; g.trainTo=0;
    g.sim.reset();
    if (g.plug.load(g.rows[g.sel].dllPath)) {
        g.sim = g.plug.make();
        const bool restored=g.sim&&bench::projects::apply_settings(*g.sim,settings,restoreError);
        g.hist.clear(); g.raster.clear_accumulator(); g.scrubAt = -1;
        loadCode(g.rows[g.sel].source);
        // Same admission gate as select(). Without it the watch timer adopts
        // whatever the last rebuild produced, however large.
        if (admit(g.rows[g.sel].label)) g.say(restored?"reloaded " + g.rows[g.sel].label:
            "Built successfully; saved controls changed. "+restoreError);
    } else if (!quiet) g.say("reload failed: " + g.plug.error());
}

// ── build, from inside the app ──────────────────────────────────────────────
// The loop that makes this an IDE rather than a viewer: edit, build here, see
// the compiler's actual errors here, and reload on success without restarting.
void buildPlugin() {
    if(g.buildProcess) {g.say("A plugin build is already running.");return;}
    if(g.sel>=g.rows.size()||!g.rows[g.sel].plugin) {g.say("Select a plugin source to build.");return;}
    const auto r=g.rows[g.sel];
    std::string compiler=bench::Paths::get().root()+"/toolchain/bin/g++.exe";
    const bool bundled=std::filesystem::is_regular_file(bench::path_from_utf8(compiler));
    if(!bundled) {
        if(bench::Paths::get().installed()) {
            g.say("C++ build tools are not installed. Run the installer again and select C++ build tools.");return;
        }
#ifdef BENCH_CXX_COMPILER
        compiler=BENCH_CXX_COMPILER;
#else
        compiler="g++";
#endif
    }
    auto quote=[](const std::string& v){return "\""+v+"\"";};
    const auto source=bench::path_from_utf8(r.source),dll=bench::path_from_utf8(r.dllPath);
    if(!std::filesystem::is_regular_file(source)){g.say("This simulation has no source file to build.");return;}
    // Run in the source folder. Relative output names also avoid the linker's
    // legacy conversion of non-ASCII parent folders in absolute arguments.
    const auto workingDirectory=source.parent_path();
    const auto outputName=dll.parent_path()==workingDirectory?dll.filename():dll;
    const std::string sysroot=bundled?" --sysroot="+quote(bench::Paths::get().root()+"/toolchain"):"";
    std::wstring command=ui::widen(quote(compiler)+sysroot+" -std=c++20 -O2 -shared -I"+quote(bench::Paths::get().src())+" "+quote(bench::path_text(source.filename()))+" -o "+quote(bench::path_text(outputName))+" -static -static-libgcc -static-libstdc++");
    // GCC's subprocesses need the matching runtime DLLs from its bin folder.
    // Give only this child a modified PATH; do not alter the user's environment.
    auto* inherited=GetEnvironmentStringsW();
    if(!inherited){g.say("Could not prepare the compiler environment.");return;}
    std::vector<std::wstring> variables;std::wstring path;
    for(const wchar_t* item=inherited;*item;item+=wcslen(item)+1) {
        if(_wcsnicmp(item,L"PATH=",5)==0)path=item+5;else variables.emplace_back(item);
    }
    FreeEnvironmentStringsW(inherited);
    variables.push_back(L"PATH="+bench::path_from_utf8(compiler).parent_path().wstring()+L";"+path);
    std::sort(variables.begin(),variables.end(),[](const auto& a,const auto& b){return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;});
    std::wstring environment;for(const auto& item:variables){environment+=item;environment.push_back(0);}environment.push_back(0);
    g.build.clear();g.build.push_back("Building "+r.label+"...");g.showBuild=true;g.buildOk=false;
    g.buildDll=r.dllPath;g.buildLog=r.dllPath+".build.log";
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE output=CreateFileW(ui::widen(g.buildLog).c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE) {
        if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
        if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
        g.say("Could not create compiler log.");return;
    }
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=input;si.hStdOutput=si.hStdError=output;
    PROCESS_INFORMATION pi{};
    const bool started=CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),workingDirectory.c_str(),&si,&pi)!=0;
    const DWORD error=GetLastError();CloseHandle(output);CloseHandle(input);
    if(!started){g.build.push_back("Compiler could not start (Windows error "+std::to_string(error)+").");g.say("Build could not start. See the output pane.");return;}
    CloseHandle(pi.hThread);g.buildProcess=pi.hProcess;g.say("Building "+r.label+". You can keep using the workbench.");
}

// ── while a unit is in flight ───────────────────────────────────────────────
//
// The one rule that makes off-thread stepping safe: while the worker holds the
// sim, NOTHING on this thread touches it. Painting re-shows the last composed
// frame instead of laying out a new one — layout reads the sim over a hundred
// times — and input is held at the door in WndProc: Pause, from the button or
// Space, is kept and applied when the unit lands; everything else waits.

bool workBusy() { return g.work && g.work->busy(); }

void requestPause() {
    g.pauseAfter = true;
    g.say("Pausing: the step in progress finishes first.");
}

void paintBusy(HDC dc, const RECT& cr) {
    if (g.frameCache) {
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, g.frameCache);
        BitBlt(dc, 0, 0, std::min<int>(cr.right, g.frameW), std::min<int>(cr.bottom, g.frameH),
               src, 0, 0, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
        // A window made larger mid-unit: blank what the kept frame does not
        // reach, rather than leave whatever was on the screen there before.
        const HBRUSH blank = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (cr.right > g.frameW)  { RECT r{g.frameW, 0, cr.right, cr.bottom}; FillRect(dc, &r, blank); }
        if (cr.bottom > g.frameH) { RECT r{0, g.frameH, std::min<LONG>(cr.right, g.frameW), cr.bottom}; FillRect(dc, &r, blank); }
    } else {
        FillRect(dc, &cr, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    // Say why nothing is moving, and for how long. A still picture with no
    // explanation reads as a hang, which is the thing this exists to prevent.
    char b[200];
    std::snprintf(b, sizeof b, "  %s in progress: %.1f s  |  %s  ",
                  g.offUnit == App::OffUnit::Epoch ? "Training" : "A step",
                  double(GetTickCount() - g.offStarted) / 1000.0,
                  g.pauseAfter ? "will pause when it lands" : "Space or Pause stops after it");
    const std::wstring w = ui::widen(b);
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0x0d, 0x12, 0x16));
    SetTextColor(dc, RGB(0xe8, 0xb8, 0x4a));
    const int x = g.canvasRect.right > g.canvasRect.left ? int(g.canvasRect.left) + 12 : 12;
    const int y = g.canvasRect.bottom > g.canvasRect.top ? int(g.canvasRect.top) + 12 : 12;
    TextOutW(dc, x, y, w.c_str(), int(w.size()));
    SelectObject(dc, oldFont);
}

[[maybe_unused]] void pollBuild() {
    // A finished build reloads the plugin, which destroys the sim: not while a
    // unit is stepping it. The build is simply collected on a later look.
    if(workBusy())return;
    if(!g.buildProcess||WaitForSingleObject(g.buildProcess,0)!=WAIT_OBJECT_0)return;
    DWORD code=1;GetExitCodeProcess(g.buildProcess,&code);CloseHandle(g.buildProcess);g.buildProcess=nullptr;
    g.buildOk=code==0;
    std::ifstream log(bench::path_from_utf8(g.buildLog));std::string line;
    while(std::getline(log,line)) if(g.build.size()<400)g.build.push_back(line);
    g.build.push_back(g.buildOk?"Build succeeded.":"Build failed. Full output: "+g.buildLog);
    if(g.buildOk && g.sel<g.rows.size() && g.rows[g.sel].dllPath==g.buildDll) {
        reloadPlugin();g.mode=g.sim?Mode::Run:Mode::Code;
    } else g.say(g.buildOk?"Plugin built successfully.":"Build failed. See the output pane.");
}

// ── dedicated runs ──────────────────────────────────────────────────────────
//
// This window is the right place to explore a world and the wrong place to run
// a big one: it cuts every step batch off at eight milliseconds so it can paint,
// copies the field into the timeline, and freezes for the whole of any single
// step longer than a frame. A dedicated run hands the CURRENT setup to
// bench_run, which hosts one copy of this simulation in its own process with
// none of that (see runner.hpp). The window stays usable, a crash there cannot
// take the window down, and results land in runs\ as CSV, frames and a
// run.json that records every setting.
//
// It starts a NEW run from the applied settings. It does not carry over cells
// painted by hand, a loaded pattern, a Movement Lab body, or how far this
// window's own run has got — and for the one sim where that is most likely to
// surprise, the label says so.

std::string dedicatedExe() { return bench::Paths::get().exeDir() + "\\bench_run.exe"; }

void launchDedicated(long long units, bool epochs, const std::string& outOverride = {}) {
    namespace fs = std::filesystem;
    if (g.dedicatedProcess) { g.say("A dedicated run is already going. Stop it first (Tools)."); return; }
    if (!g.sim || g.sel >= g.rows.size()) return;
    if (!g.pendingSetup.empty()) {
        g.say("Apply or discard the staged setup first, so the dedicated run is the one you set.");
        return;
    }
    const Row r = g.rows[g.sel];
    std::error_code ec;
    if (!fs::exists(dedicatedExe(), ec)) { g.say("bench_run.exe is missing beside the workbench. Rebuild to get it."); return; }
    if (r.plugin && !fs::exists(r.dllPath, ec)) { g.say("Build this plugin first: a dedicated run loads its DLL."); return; }
    const char* epochName = g.sim->epoch_name();
    if (epochs && !epochName) return;
    auto quote = [](const std::string& v) { return "\"" + v + "\""; };
    const std::string target = r.plugin ? r.dllPath : r.id;
    std::string cmd = quote(dedicatedExe()) + " " + quote(target);
    if (auto* rule=ruleLab())cmd += " --rule " + quote(rule->spec().text);
    // Every setting the window is in, not only the ones changed: a knob left at
    // its default is still part of the experiment. A choice goes by its LABEL,
    // which the runner reads first and is unambiguous; an index would not be,
    // for a knob whose labels are themselves numbers.
    for (const auto& k : g.sim->knobs()) {
        if (k.display_only) continue;
        std::string v;
        if (!k.choices.empty()) v = k.shown();
        else { char b[48]; std::snprintf(b, sizeof b, "%.9g", double(k.value)); v = b; }
        cmd += " --set " + quote(k.key + "=" + v);
    }
    if (units > 0) cmd += (epochs ? " --epochs " : " --steps ") + std::to_string(units);
    const std::string out = outOverride.empty() ? bench::run::default_out(target) : outOverride;
    cmd += " --out " + quote(out);
    fs::create_directories(out, ec);
    const std::string log = out + "\\runner.log";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE output = CreateFileW(ui::widen(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        g.say("Could not create the dedicated run's log in " + out + ".");
        return;
    }
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = input; si.hStdOutput = si.hStdError = output;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = ui::widen(cmd);
    const bool started = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi) != 0;
    const DWORD error = GetLastError();
    CloseHandle(output); CloseHandle(input);
    if (!started) { g.say("The dedicated run could not start (Windows error " + std::to_string(error) + ")."); return; }
    CloseHandle(pi.hThread);
    g.dedicatedProcess = pi.hProcess;
    g.dedicatedOut = out; g.dedicatedLog = log; g.dedicatedPos = 0;
    g.dedicatedFinished.clear(); g.dedicatedError.clear(); g.dedicatedTail.clear();
    g.dedicatedPolled = 0; g.dedicatedStopAt = 0;
    g.dedicatedRunning = false; g.dedicatedStopPending = false;
    g.dedicatedLabel = r.label + ", " +
        (units > 0 ? bench::run::count_of(units, epochs ? std::string(epochName) : std::string("step"))
                   : std::string("until stopped"));
    if (r.id == "locomotion") g.dedicatedLabel += " (starter body: a custom body is not carried over)";
    g.dedicatedLine = "Dedicated run starting: " + g.dedicatedLabel;
    // The run gets the machine. Both processes stepping at once would halve
    // each, and the window's own run is the one nobody asked to be big.
    g.running = false; g.training = false; g.trainTo = 0;
    g.say("Dedicated run started: " + g.dedicatedLabel + ". This window is paused so the run gets the machine.");
}

void sendDedicatedStop() {
    std::ofstream(g.dedicatedOut + "\\stop") << "stop\n";
    g.dedicatedStopPending = false;
}

[[maybe_unused]] void pollDedicated() {
    if (!g.dedicatedProcess) return;
    const DWORD now = GetTickCount();
    bool done = WaitForSingleObject(g.dedicatedProcess, 0) == WAIT_OBJECT_0;
    // A stop asked for politely and still unanswered after ten seconds is
    // forced. The run looks for its stop file between steps, so only one step
    // that long gets this far, and every row written before it stands.
    if (!done && g.dedicatedStopAt && now - g.dedicatedStopAt > 10000) {
        TerminateProcess(g.dedicatedProcess, DWORD(bench::run::Exit::Stopped));
        WaitForSingleObject(g.dedicatedProcess, 2000);
        g.dedicatedError = "forced to stop after ten seconds; everything written before that is on disk";
        done = true;
    }
    if (!done && now - g.dedicatedPolled < 500) return;
    g.dedicatedPolled = now;
    // Only what the run added since the last look, and only whole lines: the
    // log grows by a line a second for as long as the run lasts.
    if (std::ifstream in{g.dedicatedLog, std::ios::binary}) {
        in.seekg(g.dedicatedPos);
        const std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto end = chunk.rfind('\n');
        if (end != std::string::npos) {
            g.dedicatedPos += std::streamoff(end + 1);
            std::istringstream lines(chunk.substr(0, end + 1));
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("progress ", 0) == 0)       g.dedicatedLine = "Dedicated: " + line.substr(9);
                else if (line == "running")                g.dedicatedRunning = true;
                else if (line.rfind("finished: ", 0) == 0) g.dedicatedFinished = line.substr(10);
                else if (line.rfind("error: ", 0) == 0)    g.dedicatedError = line.substr(7);
                else if (!line.empty())                    g.dedicatedTail = line;
            }
        }
    }
    if (g.dedicatedRunning && g.dedicatedStopPending) sendDedicatedStop();
    if (!done) return;
    DWORD code = 1; GetExitCodeProcess(g.dedicatedProcess, &code);
    CloseHandle(g.dedicatedProcess); g.dedicatedProcess = nullptr;
    g.dedicatedStopAt = 0; g.dedicatedStopPending = false; g.dedicatedLine.clear();
    const char* verb = code == 0 ? "finished" : code == 1 ? "stopped" : "failed";
    std::string what = !g.dedicatedFinished.empty() ? g.dedicatedFinished
                     : !g.dedicatedError.empty()    ? g.dedicatedError
                     : !g.dedicatedTail.empty()     ? g.dedicatedTail
                                                    : "exit " + std::to_string(code);
    if (code > 1 && !g.dedicatedError.empty() && what != g.dedicatedError) what += "; " + g.dedicatedError;
    g.say(std::string("Dedicated run ") + verb + ": " + what + ". Tools > Open dedicated results.");
}

// The stop is held until the run has said "running", for the reason given in
// runner.hpp: a stop file written before that is deleted as left over.
void stopDedicated() {
    if (!g.dedicatedProcess || g.dedicatedStopAt) return;
    g.dedicatedStopAt = GetTickCount();
    g.dedicatedStopPending = true;
    if (g.dedicatedRunning) sendDedicatedStop();
    g.dedicatedLine = "Dedicated run stopping: finishing the current step and writing its results";
    g.say("Asked the dedicated run to stop. It finishes the current step and writes its results first.");
}

// Closing the window ends the run it started. One that is running finishes
// cleanly and writes its results; one that has not started yet has nothing to
// lose. A run meant to outlive the window is bench_run from a command line.
void releaseDedicated() {
    if (!g.dedicatedProcess) return;
    if (g.dedicatedRunning) sendDedicatedStop();
    else TerminateProcess(g.dedicatedProcess, DWORD(bench::run::Exit::Stopped));
}

void openDedicatedResults() {
    if (!g.dedicatedOut.empty())
        ShellExecuteW(nullptr, L"open", ui::widen(g.dedicatedOut).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void openSource() {
    if (g.sel >= g.rows.size()) return;
    ShellExecuteW(nullptr, L"open", ui::widen(g.rows[g.sel].source).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
}

void newFromTemplate() {
    namespace p=bench::projects;
    const auto path=bench::path_from_utf8(bench::Paths::get().projects())/("My simulation "+p::token()+".benchsim");
    p::Document document;document.name="My simulation";document.source=bench::Paths::get().starter();
    std::string error;
    if (p::save(path,document,error)) {
        rebuildRows();
        for (std::size_t i = 0; i < g.rows.size(); ++i) if (g.rows[i].projectPath == p::utf8(path)) { select(i); break; }
        g.mode = Mode::Code;
        g.say("Created and saved in your library. Edit source, then Build; use Save as to give it a name.");
    } else g.say("Could not create your simulation: "+error);
}

void newRuleSimulation() {
    namespace p=bench::projects;
    const auto path=bench::path_from_utf8(bench::Paths::get().projects())/("My rule "+p::token()+".benchsim");
    p::Document document;document.name="My rule";document.model="rule";document.rule="B3/S23";
    std::string error;
    if(!p::save(path,document,error)){g.say("Could not create your rule: "+error);return;}
    rebuildRows();
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].projectPath==p::utf8(path)){select(i);break;}
    g.running=false;g.showRight=true;
    g.say("Rule simulation created in your library. Edit its rule, then Apply & restart. Save as gives it a name.");
}

void newFromLibraryModel(const std::string& id) {
    namespace p=bench::projects;
    const bench::Entry* entry=nullptr;
    for(const auto& e:bench::registry()) if(e.id==id) entry=&e;
    if(!entry){g.say("That library simulation is not available.");return;}
    auto sim=entry->make();
    if(!sim){g.say("Could not copy that simulation.");return;}
    const std::string title=bench::catalog_title(id);
    const auto path=bench::path_from_utf8(bench::Paths::get().projects())/(p::copy_stem(title)+" "+p::token()+".benchsim");
    p::Document document;
    p::library_start(document,title,id,*sim);
    std::string error;
    if(!p::save(path,document,error)){g.say("Could not copy that simulation: "+error);return;}
    rebuildRows();
    for(std::size_t i=0;i<g.rows.size();++i)if(g.rows[i].projectPath==p::utf8(path)){select(i);break;}
    g.running=false;g.showRight=true;
    g.say("Copied "+title+" into your library. Settings start at the template defaults. Save as gives it a name.");
}

void createSimulationMenu(RECT rect) {
    queueMenu(rect,{{"Rule-based simulation (no coding)",[]{newRuleSimulation();}},
                    {"C++ simulation",[]{newFromTemplate();}},
                    {"Copy a library simulation",[rect]{
                        std::vector<MenuItem> items;
                        for(const auto& e:bench::registry()) {
                            const std::string id=e.id;
                            items.push_back(MenuItem{bench::catalog_title(id),[id]{newFromLibraryModel(id);}});
                        }
                        queueMenu(rect,std::move(items));
                    }}});
}

// ── syntax colouring, deliberately shallow ──────────────────────────────────
// Enough structure to read by: comments, strings, numbers, a few keywords.
// Not a parser, and it does not pretend to be.
bool isKeyword(const std::string& w) {
    static const char* k[] = {"auto","bool","break","case","char","class","const","constexpr",
        "continue","default","delete","do","double","else","enum","explicit","export","extern",
        "false","float","for","friend","if","inline","int","long","namespace","new","nullptr",
        "operator","override","private","protected","public","return","short","signed","sizeof",
        "static","struct","switch","template","this","true","typedef","typename","union",
        "unsigned","using","virtual","void","while","std","uint8_t","size_t","final"};
    for (const char* s : k) if (w == s) return true;
    return false;
}

void drawCodeLine(ui::Ctx& c, int x, int y, const std::string& L) {
    const auto firstNonWs = L.find_first_not_of(" \t");
    const bool fullComment = firstNonWs != std::string::npos &&
                             L.compare(firstNonWs, 2, "//") == 0;
    if (fullComment) { ui::text(c, x, y, L, c.t.faint); return; }

    int px = x;
    std::size_t i = 0;
    auto emit = [&](const std::string& s, COLORREF col) {
        if (s.empty()) return;
        ui::text(c, px, y, s, col);
        px += ui::measure(c, s, c.font).cx;
    };
    while (i < L.size()) {
        if (L.compare(i, 2, "//") == 0) { emit(L.substr(i), c.t.faint); break; }
        if (L[i] == '"') {
            std::size_t j = i + 1;
            while (j < L.size() && !(L[j] == '"' && L[j-1] != '\\')) ++j;
            emit(L.substr(i, j - i + 1), RGB(0xc8,0xa8,0x6b)); i = j + 1; continue;
        }
        if (std::isalpha((unsigned char)L[i]) || L[i] == '_') {
            std::size_t j = i;
            while (j < L.size() && (std::isalnum((unsigned char)L[j]) || L[j] == '_')) ++j;
            const std::string w = L.substr(i, j - i);
            emit(w, isKeyword(w) ? RGB(0x9b,0x7a,0xe6) : c.t.ink);
            i = j; continue;
        }
        if (std::isdigit((unsigned char)L[i])) {
            std::size_t j = i;
            while (j < L.size() && (std::isalnum((unsigned char)L[j]) || L[j] == '.')) ++j;
            emit(L.substr(i, j - i), RGB(0x7b,0xd8,0x8f)); i = j; continue;
        }
        emit(std::string(1, L[i]), c.t.dim); ++i;
    }
}

// ── panes ───────────────────────────────────────────────────────────────────
std::vector<std::size_t> libraryMatches() {
    auto lower=[](std::string text){for(char& ch:text)ch=char(std::tolower(static_cast<unsigned char>(ch)));return text;};
    const auto query=lower(g.libraryQuery);
    std::vector<std::size_t> shown;
    for(std::size_t i=0;i<g.rows.size();++i) {
        const auto& row=g.rows[i];
        if(g.libraryCategory!="All simulations" && g.libraryCategory!=row.era)continue;
        if(!query.empty() && lower(row.label+" "+row.id+" "+row.era+" "+row.blurb).find(query)==std::string::npos)continue;
        shown.push_back(i);
    }
    return shown;
}

void paneLibrary(ui::Ctx& c, RECT a) {
    const int left=32,right=a.right-32;
    ui::text(c,left,24,"Simulation library",c.t.ink,c.bold);
    ui::text(c,left,56,"Choose one world. Open it in a dedicated workspace.",c.t.dim);
    ui::text(c,left,86,"Search by name, family or activity",c.t.faint);
    // The native EDIT window occupies this rectangle in the interactive app.
    ui::rounded(c,RECT{left,108,left+390,142},c.t.sunken,c.t.line);
    if(!g.librarySearch)ui::text(c,left+10,114,g.libraryQuery.empty()?"Search simulations...":g.libraryQuery,c.t.dim);
    RECT filter{438,108,690,142};
    if(ui::button(c,filter,g.libraryCategory+" ▾")) {
        std::vector<MenuItem> items;
        std::vector<std::string> categories{"All simulations"};
        for(const auto& row:g.rows)if(std::find(categories.begin(),categories.end(),row.era)==categories.end())categories.push_back(row.era);
        for(const auto& category:categories)items.push_back({category,[category]{g.libraryCategory=category;g.rosterScroll=0;},g.libraryCategory==category});
        queueMenu(filter,std::move(items));
    }
    if(g.sim || g.mode==Mode::Code) {
        if(ui::button(c,RECT{right-230,28,right,62},"Return to workspace"))closeLibrary();
        if(g.sel<g.rows.size())ui::text_clipped(c,right-230,70,g.rows[g.sel].label,c.t.acc,right);
    }
    const auto shown=libraryMatches();
    ui::text(c,710,116,std::to_string(shown.size())+" available",c.t.faint);
    g.rosterRect=RECT{left,164,right,a.bottom-118};
    const int rowH=124,colW=(right-left-16)/2;
    g.rosterContentH=int((shown.size()+1)/2)*rowH;
    const int vh=g.rosterRect.bottom-g.rosterRect.top;
    const int maxScroll=std::max(0,g.rosterContentH-vh);
    g.rosterScroll=std::clamp(g.rosterScroll,0,maxScroll);
    g.rowRects.assign(g.rows.size(),RECT{});
    const int saved=SaveDC(c.dc);IntersectClipRect(c.dc,left,g.rosterRect.top,right,g.rosterRect.bottom);
    const RECT oldClip=c.inputClip;c.inputClip=g.rosterRect;
    for(std::size_t n=0;n<shown.size();++n) {
        const auto i=shown[n];const auto& row=g.rows[i];
        const int x=left+int(n%2)*(colW+16),y=164+int(n/2)*rowH-g.rosterScroll;
        RECT card{x,y,x+colW,y+112};
        if(!IntersectRect(&g.rowRects[i],&card,&g.rosterRect))continue;
        ui::record(c,card);
        const bool active=g.sim&&g.sel==i;
        ui::rounded(c,card,ui::hit(c,card)?c.t.hover:c.t.panel,active?c.t.acc:c.t.line);
        if(ui::hit(c,card))c.hot=1;
        ui::text_clipped(c,x+18,y+13,row.era,c.t.faint,x+colW-18);
        ui::text_clipped(c,x+18,y+36,row.label,c.t.ink,x+colW-18,"…",c.bold);
        const auto& wf=bench::workflow(row.id);
        ui::text_clipped(c,x+18,y+68,row.plugin?"Open source and build your simulation":wf.title,c.t.dim,x+colW-18);
        ui::text_clipped(c,x+18,y+89,active?"Currently loaded · opening starts a new run":"Open simulation →",c.t.acc,x+colW-18);
    }
    if(shown.empty())ui::text(c,left+16,184,g.rows.empty()?"Your bench is ready. Create a simulation or open a saved .benchsim file.":"No matches. Try another name or choose All simulations.",c.t.dim);
    RestoreDC(c.dc,saved);c.inputClip=oldClip;
    if(maxScroll) {
        const int thumb=std::max(24,vh*vh/g.rosterContentH),yy=g.rosterRect.top+(vh-thumb)*g.rosterScroll/maxScroll;
        ui::fill(c,RECT{right+8,yy,right+11,yy+thumb},c.t.line);
    }
    ui::hline(c,left,right,a.bottom-105,c.t.line);
    const RECT create{left,a.bottom-90,left+190,a.bottom-56};
    if(ui::button(c,create,"+ Create simulation"))createSimulationMenu(create);
    if(ui::button(c,RECT{left+202,a.bottom-90,left+386,a.bottom-56},"Open simulation"))PostMessageW(g.hwnd,WM_APP+7,0,0);
    if(ui::button(c,RECT{left+398,a.bottom-90,left+580,a.bottom-56},"Saved files"))
        ShellExecuteW(g.hwnd,L"open",ui::widen(bench::Paths::get().projects()).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    if(ui::button(c,RECT{left+592,a.bottom-90,left+702,a.bottom-56},"Refresh"))rebuildRows();
    ui::text_clipped(c,left,a.bottom-42,"One simulation at a time. Saved simulations keep their code and setup between visits.",c.t.faint,right);
}

LRESULT CALLBACK LibrarySearchProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_KEYDOWN && (w==VK_ESCAPE||w==VK_F4||w==VK_RETURN)) {
        PostMessageW(g.hwnd,w==VK_RETURN?WM_APP+5:WM_KEYDOWN,w,0);return 0;
    }
    if(m==WM_CHAR && (w==VK_RETURN||w==VK_ESCAPE))return 0;
    return CallWindowProcW(g.librarySearchProc,h,m,w,l);
}

void paneRoster(ui::Ctx& c, RECT a) {
    ui::fill(c, a, c.t.panel);
    ui::text(c, 18, 18, "Life / Workbench", c.t.ink, c.bold);
    ui::text(c, 18, 46, std::to_string(g.rows.size())+" simulations available", c.t.faint);
    g.rosterRect = RECT{a.left, 78, a.right, a.bottom - 90};
    const int saved = SaveDC(c.dc);
    IntersectClipRect(c.dc, g.rosterRect.left, g.rosterRect.top, g.rosterRect.right, g.rosterRect.bottom);
    const RECT oldClip = c.inputClip; c.inputClip = g.rosterRect;
    int y = g.rosterRect.top - g.rosterScroll;
    int selectedTop=0;
    g.rowRects.assign(g.rows.size(), RECT{});
    std::string era;
    for (std::size_t i = 0; i < g.rows.size(); ++i) {
        if (g.rows[i].era != era) {
            era = g.rows[i].era; y += 14;
            ui::text(c, 18, y, era, c.t.faint); y += 25;
        }
        RECT row{a.left + 8, y, a.right - 8, y + 30};
        if(i==g.sel)selectedTop=y+g.rosterScroll-g.rosterRect.top;
        IntersectRect(&g.rowRects[i], &row, &g.rosterRect);
        if (i == g.sel) {
            ui::rounded(c, row, c.t.active, c.t.line);
            ui::fill(c, RECT{row.left, row.top + 7, row.left + 3, row.bottom - 7}, c.t.acc);
        } else if (ui::hit(c, row)) ui::fill(c, row, c.t.hover);
        ui::text_clipped(c, 20, y + 6, g.rows[i].label,
                         i == g.sel ? c.t.acc : c.t.dim, a.right - 15, " ");
        ui::tip(c, row, g.rows[i].label + "\n" + g.rows[i].blurb);
        y += 32;
    }
    g.rosterContentH = y + g.rosterScroll - g.rosterRect.top;
    RestoreDC(c.dc, saved); c.inputClip = oldClip;
    const int maxScroll = std::max(0, g.rosterContentH - int(g.rosterRect.bottom-g.rosterRect.top));
    g.rosterScroll = std::clamp(g.rosterScroll, 0, maxScroll);
    if(g.revealSelection) {g.rosterScroll=std::clamp(selectedTop-int(g.rosterRect.bottom-g.rosterRect.top)/3,0,maxScroll);g.revealSelection=false;InvalidateRect(g.hwnd,nullptr,FALSE);}
    if (maxScroll) {
        const int h = int(g.rosterRect.bottom-g.rosterRect.top);
        const int thumb = std::max(24, h*h/g.rosterContentH);
        const int yy = g.rosterRect.top + (h-thumb)*g.rosterScroll/maxScroll;
        ui::fill(c, RECT{a.right-4,yy,a.right-1,yy+thumb},c.t.line);
    }
    ui::hline(c, 16, a.right - 16, a.bottom - 85, c.t.line);
    const RECT create{16,a.bottom-74,a.right-16,a.bottom-42};
    if (ui::button(c, create, "+ New simulation"))createSimulationMenu(create);
    if (ui::button(c, RECT{16,a.bottom-36,a.right-16,a.bottom-8},
                   g.watch ? "Auto-reload plugins: on" : "Auto-reload plugins: off",g.watch)) g.watch = !g.watch;
}

void drawControl(ui::Ctx& c, const bench::Knob& original, int x, int& y, int w, std::size_t index) {
    bench::Knob k = original; k.value = controlValue(original);
    const std::string key = k.key;
    std::string unmet;
    // Follow dependency chains using the values the user is configuring.
    auto req = k;
    for (std::size_t depth=0; depth<g.sim->knobs().size() && !req.requires_key.empty(); ++depth) {
        auto it=std::find_if(g.sim->knobs().begin(),g.sim->knobs().end(),[&](const auto& o){return o.key==req.requires_key;});
        if(it==g.sim->knobs().end()) break;
        if(std::fabs(controlValue(*it)-req.requires_value)>1e-4f) {
            auto wanted=*it; wanted.value=req.requires_value;
            unmet="Requires " + it->label + ": " + wanted.shown(); break;
        }
        req=*it;
    }
    y += ui::para(c,x,y,w,k.label,c.t.dim) + 6;
    RECT value{x,y,x+w,y+30};
    const bool enabled=unmet.empty();
    ui::tip(c,RECT{x,y-24,x+w,y+34},k.help + (k.on_reset ? "\nStaged until Apply & restart." : "\nApplies to the running simulation."));
    if(!k.choices.empty()) {
        if(ui::button(c,value,k.shown()+"  ▾",false,enabled)) {
            std::vector<MenuItem> menu;
            for(std::size_t j=0;j<k.choices.size();++j) {
                const float v=k.min+float(j);
                menu.push_back({k.choices[j],[key,v]{setControl(key,v);}, std::fabs(v-k.value)<1e-4f});
            }
            queueMenu(value,std::move(menu));
        }
        y+=38;
    } else {
        RECT minus{x,y,x+30,y+30}, plus{x+w-30,y,x+w,y+30};
        RECT edit{x+36,y,x+w-36,y+30};
        const float increment = k.step>0 ? k.step : (k.max-k.min)/100.f;
        if(ui::button(c,minus,"−",false,enabled && k.value>k.min)) setControl(key,k.value-increment);
        if(ui::button(c,plus,"+",false,enabled && k.value<k.max)) setControl(key,k.value+increment);
        if(ui::button(c,edit,k.shown(),false,enabled)) queueNumber(key,edit);
        ui::tip(c,edit,"Click to type an exact value. Enter applies; Escape cancels.");
        y+=35;
        if(k.step<1.f && enabled) {
            bool drag=g.knobDrag[index]; float v=k.value;
            if(ui::slider(c,RECT{x+5,y,x+w-5,y+18},v,k.min,k.max,drag)) setControl(key,v);
            g.knobDrag[index]=drag; y+=24;
        }
    }
    if(!unmet.empty()) y+=ui::para(c,x,y,w,unmet,c.t.amber)+5;
    y+=10;
}

struct MotionUi {
    RECT edit{},replay{},tools[6]{};
} motionUi;

bench::Locomotion* movement() {return dynamic_cast<bench::Locomotion*>(g.sim.get());}
bench::SortingSim* sortingLab() {return dynamic_cast<bench::SortingSim*>(g.sim.get());}
void replayFinishedSort() {
    if(auto* sim=sortingLab();sim&&sim->finished()) {
        sim->reset();g.hist.clear();g.raster.clear_accumulator();g.scrubAt=-1;g.acc=0;
    }
}
bool stopFinishedSort() {
    if(auto* sim=sortingLab();sim&&sim->finished()) {
        g.running=false;g.acc=0;g.say("Sort complete. Replay repeats the same data; New data changes the seed.");return true;
    }
    return false;
}

void editMovement() {
    auto* sim=movement();if(!sim)return;
    g.running=g.training=false;g.scrubAt=-1;
    const bool restart=sim->editing()&&sim->dirty();
    if(sim->edit(!sim->editing())&&restart){g.hist.clear();g.raster.clear_accumulator();}
    if(!sim->message().empty())g.say(sim->message());
}

void movementFile(bool save) {
    auto* sim=movement();if(!sim)return;
    g.running=g.training=false;
    wchar_t filename[32768]=L"my-creature.creature";
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof dialog;dialog.hwndOwner=g.hwnd;
    dialog.lpstrFilter=L"Creature bodies (*.creature)\0*.creature\0All files\0*.*\0\0";
    dialog.lpstrFile=filename;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"creature";
    dialog.lpstrTitle=save?L"Save creature body":L"Load creature body";
    dialog.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_HIDEREADONLY|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    // Microsoft documents OFN_NOCHANGEDIR as ineffective for the open dialog.
    std::error_code ec;const auto cwd=std::filesystem::current_path(ec);
    const bool accepted=save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog);
    if(!cwd.empty())std::filesystem::current_path(cwd,ec);
    if(!accepted){if(CommDlgExtendedError())g.say("The creature file dialog could not open.");return;}
    const std::filesystem::path path(filename);
    if(save){
        std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<sim->saveBody();out.close();
        g.say(out?"Creature body saved. Trained brains are not included.":"Could not save the creature body.");
    } else {
        const auto bytes=std::filesystem::file_size(path,ec);
        if(ec||bytes>32768){g.say("Cannot load this creature: unreadable or larger than 32 KB.");return;}
        std::ifstream in(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(in)),{});
        if(!in||!sim->loadBody(data)){g.say(sim->message().empty()?"Could not read the creature file.":sim->message());return;}
        g.scrubAt=-1;g.notesScroll=0;g.inspector=0;g.say("Creature loaded into the body workshop. Return to experiment when ready.");
    }
    InvalidateRect(g.hwnd,nullptr,FALSE);
}

void paneNotes(ui::Ctx& c, RECT a) {
    ui::fill(c, a, c.t.panel);
    ui::hline(c, a.left, a.left + 1, a.top, c.t.line);
    g.palRects.clear(); g.switchRects.clear(); g.ruleRect = RECT{};
    const int x=a.left+18, w=a.right-a.left-36;
    ui::text_clipped(c,x,a.top+16,g.sel<g.rows.size()?g.rows[g.sel].label:"Simulation",c.t.ink,a.right-18," ");
    const char* tabs[]={"Controls","Metrics","Guide"};
    const int tw=(w-8)/3;
    for(int i=0;i<3;++i) if(ui::button(c,RECT{x+i*(tw+4),a.top+46,x+i*(tw+4)+tw,a.top+78},tabs[i],g.inspector==i)) {
        cancelNumber();closeRuleEditor(); g.inspector=i; g.notesScroll=0;
    }
    const int top=a.top+92;
    const int footer=g.inspector==0&&!g.pendingSetup.empty()?52:0;
    g.notesRect=RECT{a.left,top,a.right,a.bottom-footer};
    if(footer) {
        const int half=(w-6)/2;const int yy=a.bottom-42;
        ui::hline(c,x,a.right-18,yy-8,c.t.line);
        const bool rule=ruleLab()!=nullptr;
        if(ui::button(c,RECT{x,yy,x+half,yy+32},"Apply & restart",true)) {
            if(rule)PostMessageW(g.hwnd,WM_APP+9,1,0);else applySetup();
        }
        if(ui::button(c,RECT{x+half+6,yy,x+w,yy+32},rule?"Discard setup":"Discard changes"))g.pendingSetup.clear();
    }
    const int savedDC=SaveDC(c.dc);
    IntersectClipRect(c.dc,a.left,top,a.right,g.notesRect.bottom);
    const RECT oldClip=c.inputClip; c.inputClip=g.notesRect;
    struct Restore { ui::Ctx& c; int id; RECT clip; ~Restore(){RestoreDC(c.dc,id); c.inputClip=clip;} } restore{c,savedDC,oldClip};
    if(!g.sim) {
        ui::para(c,x,top,w,"This source has not been built yet. Use Build in the toolbar to create and load the plugin.",c.t.dim);
        g.notesContentH=100; return;
    }
    const auto& ab=g.sim->about();
    int y=top-g.notesScroll;

    // The measured series, as a lambda because WHERE they go depends on the
    // sim. For a learning agent the curves are the reason the panel is open,
    // and they were last — under the full provenance essay, the legend and the
    // parameters, off the bottom of a 1000px window. For a lattice rule the
    // prose comes first, because there the rule IS the subject.
    auto measured = [&](int yy) {
        int y = yy;
        // Which clock to plot against. For a learning sim the epoch axis is the
        // only one that answers the question being asked — a curve sampled per
        // frame during a NEAT run is the inside of one generation, not the shape of
        // the training. Fall back to frames until at least two epochs exist,
        // because a two-point line is the shortest thing that has a direction.
        using Axis = bench::History::Axis;
        const bool byEpoch = g.sim && g.sim->epoch_name() && g.hist.epoch_samples() >= 2;
        const Axis axis    = byEpoch ? Axis::Epoch : Axis::Frame;
        const auto& names  = g.hist.names(axis);
        if (!names.empty()) {
            y += 6; ui::hline(c, x, a.right - 16, y, c.t.line); y += 12;
            char hdr[96];
            // The epoch range goes in the header, once. It used to be printed
            // inside every plot box, where it sat on top of any line that ran
            // near the bottom — and every series shares the same axis, so four
            // of the five copies were redundant as well as in the way.
            if (byEpoch)
                std::snprintf(hdr, sizeof hdr, "MEASURED  ·  %s %d-%d",
                              g.sim->epoch_name(), g.hist.epoch_index(0),
                              g.hist.epoch_index(g.hist.epoch_samples() - 1));
            else std::snprintf(hdr, sizeof hdr, "MEASURED");
            y += ui::text(c, x, y, hdr, c.t.faint) + 8;
            const COLORREF series[] = { c.t.acc, c.t.amber, c.t.viol, c.t.good, c.t.bad };
            int si = 0;
            for (const auto& n : names) {
                const auto* t = g.hist.trace(n, axis);
                if (!t || t->values.size() < 2) { ++si; continue; }
                const COLORREF col = series[si % 5];
                ui::text(c, x, y, n, c.t.dim);
                char bv[48]; std::snprintf(bv, sizeof bv, "%.4g", t->values.back());
                ui::textRight(c, a.right - 16, y, bv, col);
                y += 22;
                // Best-so-far and a recent mean beside the current value. A raw
                // learning curve is noisy enough that "is this improving" is
                // genuinely hard to read off it; these are the two numbers people
                // actually want, and neither is invented — both come straight from
                // the recorded series. On the epoch axis the window is stated in
                // epochs, because "last 30 frames" of a generation means nothing.
                {
                    const std::size_t win = byEpoch ? 10u : 30u;
                    char stat[128];
                    // No unit in this line. Naming it here ran the text past
                    // the right edge of the panel and truncated the number the
                    // line exists to show — and the header three rows up
                    // already says which clock these are on.
                    const bool scored = g.hist.direction(n, axis) != bench::Metric::Neither;
                    const double bb = byEpoch && g.hist.has_baseline()
                                    ? g.hist.baseline_best(n) : 0.0;
                    if (!scored) {
                        // No direction, so no best and no verdict — just what
                        // it is doing lately. Saying "7 better" about a species
                        // count is a claim the number cannot support.
                        std::snprintf(stat, sizeof stat, "not a score   ·   mean of last %zu  %.4g",
                                      win, g.hist.mean_of(n, win, axis));
                    } else if (byEpoch && g.hist.has_baseline()) {
                        // Worded, not signed. A raw "+27" on "steps to goal"
                        // reads as an improvement and is the opposite of one —
                        // the sign of the arithmetic and the direction of
                        // better are not the same thing, and only the metric
                        // knows which way it runs.
                        const double now  = g.hist.best_of(n, axis);
                        const double gain = (g.hist.direction(n, axis) == bench::Metric::Higher)
                                          ? now - bb : bb - now;
                        const char* how = (std::fabs(gain) < 1e-9) ? "same"
                                        : (gain > 0 ? "better" : "worse");
                        if (std::fabs(gain) < 1e-9)
                            std::snprintf(stat, sizeof stat, "best %.4g   ·   kept %.4g  (same)", now, bb);
                        else
                            std::snprintf(stat, sizeof stat, "best %.4g   ·   kept %.4g  (%.4g %s)",
                                          now, bb, std::fabs(gain), how);
                    }
                    else
                        std::snprintf(stat, sizeof stat, "best %.4g   ·   mean of last %zu  %.4g",
                                      g.hist.best_of(n, axis), win,
                                      g.hist.mean_of(n, win, axis));
                    ui::text(c, x, y, stat, c.t.faint);
                    y += 22;
                }
                const int ph = 38;
                RECT box{ x, y, a.right - 16, y + ph };
                ui::fill(c, box, c.t.sunken); ui::frame(c, box, c.t.line);
                // Vertical range. This used to assume every series was
                // non-negative and divide by the maximum — so the gridworld's
                // episode reward, which is negative by construction (every
                // step costs), mapped BELOW the box and drew a polyline across
                // the paragraph underneath it. A plot that escapes its own
                // frame is worse than no plot: it corrupts the text around it.
                // The kept run, if there is one. It is drawn behind the live
                // curve and — this is the part that matters — its values go
                // into the SAME range calculation. Two curves autoscaled
                // separately look identical no matter how far apart they are,
                // which is a comparison that lies.
                const auto* base = byEpoch ? g.hist.baseline_trace(n) : nullptr;
                double hi = t->max_hint, lo = 0.0;
                if (hi <= 0) {
                    hi = -1e300; lo = 1e300;
                    for (double d : t->values) { hi = std::max(hi, d); lo = std::min(lo, d); }
                    if (base) for (double d : base->values) { hi = std::max(hi, d); lo = std::min(lo, d); }
                    // Keep the floor at zero when nothing is negative, so a
                    // population that hovers near 0.4 is not autoscaled into
                    // looking like it swings across the whole range.
                    if (lo > 0) lo = 0.0;
                    if (hi < 0) hi = 0.0;
                }
                const bool flat = (hi - lo) < 1e-12;
                if (flat) { hi = lo + 1.0; }
                const double span = hi - lo;
                auto ypix = [&](double v) {
                    const double f = (v - lo) / span;
                    return box.bottom - 2 - int(std::clamp(f, 0.0, 1.0) * (ph - 4));
                };
                // Zero line, when the series crosses it — otherwise "is this
                // reward positive yet" is unanswerable from the picture.
                if (lo < 0.0 && hi > 0.0) {
                    const int zy = ypix(0.0);
                    for (int px2 = box.left + 2; px2 < box.right - 2; px2 += 3)
                        ui::fill(c, RECT{px2, zy, px2 + 1, zy + 1}, c.t.dim);
                }
                if (g.hist.direction(n, axis) != bench::Metric::Neither) {
                    const int by = ypix(g.hist.best_of(n, axis));
                    for (int px2 = box.left + 2; px2 < box.right - 2; px2 += 4)
                        ui::fill(c, RECT{px2, by, px2 + 2, by + 1}, c.t.faint);
                }
                const int n0 = int(t->values.size());
                const int stride = (n0 > (box.right - box.left)) ? n0 / (box.right - box.left) + 1 : 1;
                // Clip to the box as well as clamping, so no rounding or future
                // change to the mapping can put ink outside the frame again.
                const int savedPlot = SaveDC(c.dc);
                IntersectClipRect(c.dc, box.left + 1, box.top + 1, box.right - 1, box.bottom - 1);
                // Baseline behind, dim, on its own horizontal extent — a kept
                // run of 40 generations compared against a live one of 12
                // should show 12 generations' worth of the live curve next to
                // the whole of the old one, not stretched to match it.
                if (base && base->values.size() > 1) {
                    const int bn = int(base->values.size());
                    const int bstride = (bn > (box.right - box.left)) ? bn / (box.right - box.left) + 1 : 1;
                    const int wide = std::max(n0, bn);
                    HPEN bp = CreatePen(PS_SOLID, 1, c.t.dim);
                    HGDIOBJ bo = SelectObject(c.dc, bp);
                    bool bfirst = true;
                    for (int i2 = 0; i2 < bn; i2 += bstride) {
                        const int px = box.left + 1 + int(double(i2)/double(wide-1)*(box.right-box.left-3));
                        const int py = ypix(base->values[std::size_t(i2)]);
                        if (bfirst) { MoveToEx(c.dc, px, py, nullptr); bfirst = false; } else LineTo(c.dc, px, py);
                    }
                    SelectObject(c.dc, bo); DeleteObject(bp);
                }
                HPEN pen = CreatePen(PS_SOLID, 1, col);
                HGDIOBJ op = SelectObject(c.dc, pen);
                bool first = true;
                {
                    const int wide = (base && base->values.size() > 1)
                                   ? std::max(n0, int(base->values.size())) : n0;
                    for (int i2 = 0; i2 < n0; i2 += stride) {
                        const int px = box.left + 1 + int(double(i2)/double(wide-1)*(box.right-box.left-3));
                        const int py = ypix(t->values[std::size_t(i2)]);
                        if (first) { MoveToEx(c.dc, px, py, nullptr); first = false; } else LineTo(c.dc, px, py);
                    }
                }
                SelectObject(c.dc, op); DeleteObject(pen);
                RestoreDC(c.dc, savedPlot);
                // Both ends of the axis when the floor is not zero; one when it
                // is, because a "0" printed under every plot is just noise.
                char mxs[32]; std::snprintf(mxs, sizeof mxs, "%.3g", flat ? lo : hi);
                ui::textRight(c, box.right - 4, box.top + 1, mxs, c.t.faint);
                if (lo < 0.0) {
                    char los[32]; std::snprintf(los, sizeof los, "%.3g", lo);
                    ui::textRight(c, box.right - 4, box.bottom - 15, los, c.t.faint);
                }
                y += ph + 12; ++si;
            }
        }

        return y;
    };
    const bool learner = g.sim->epoch_name() != nullptr;
    if(g.inspector==1) {
        if(ui::button(c,RECT{x,y,x+w,y+32},"Export measurements (CSV)")) saveMetrics();
        y+=42;
        if(g.hist.names().empty()) y+=ui::para(c,x,y,w,"Run a few steps to start recording measurements.",c.t.dim)+12;
        y=measured(y);
        y+=ui::para(c,x,y,w,learner ? "Curves use completed epochs when available. Keep a run from the Train menu to compare it with the next run." : "Measurements are recorded as the simulation runs. The timeline previews snapshots; it does not restore simulation state.",c.t.faint)+12;
    }
    if(g.inspector==2) {
    if(ruleLab()) {
        y+=ui::text(c,x,y,"EDITING RULES",c.t.acc,c.bold)+8;
        y+=ui::para(c,x,y,w,"B = birth, S = survival. Add /3 through /256 for fading states. V selects four orthogonal neighbours; H selects six, excluding northeast and southwest. Hensel letters select arrangements: B2-a/S12 excludes adjacent corner/edge pairs from birth with two neighbours.",c.t.dim)+10;
        y+=ui::para(c,x,y,w,"Enter applies and restarts with your world settings; Escape restores the applied rule. Ctrl+A selects all, Ctrl+V pastes, and Ctrl+Z undoes. Apply before saving. Reopening restores the seeded setup; export an RLE file separately to keep a painted pattern.",c.t.dim)+16;
    }
    y += ui::para(c, x, y, w, ab.title, c.t.ink, c.bold) + 2;
    y += ui::text(c, x, y, ab.year, c.t.acc) + 6;
    y += ui::para(c, x, y, w, ab.who, c.t.dim) + 10;

    const bool yes = ab.replicates == bench::Replication::Yes;
    const bool dis = ab.replicates == bench::Replication::Disputed;
    const char* v = yes ? "SELF-REPLICATES" : dis ? "DISPUTED" : "DOES NOT SELF-REPLICATE";
    const COLORREF vc = yes ? c.t.good : dis ? c.t.amber : c.t.dim;
    const auto chipText=ui::measure(c,v,c.font);
    RECT chip{ x, y, std::min(x+w,x+int(chipText.cx)+16),y+chipText.cy+10 };
    ui::fill(c, chip, RGB(0x0e,0x16,0x12)); ui::frame(c, chip, vc);
    ui::text_clipped(c,x+8,y+5,v,vc,chip.right-8);y=chip.bottom+12;
    y += ui::para(c, x, y, w, ab.replication_note, c.t.dim) + 12;


    ui::hline(c, x, a.right - 16, y, c.t.line); y += 12;
    y += ui::para(c, x, y, w, ab.blurb, c.t.ink) + 12;

    ui::hline(c, x, a.right - 16, y, c.t.line); y += 12;
    // A sim that renders itself is not being coloured by this palette, and a
    // legend that claims otherwise is exactly the kind of quiet lie the rest of
    // the bench refuses. Say what is actually on screen.
    const bool selfRendered = g.sim->surface() && !g.sim->surface()->empty();
    if (selfRendered) {
        y += ui::text(c, x, y, "RENDERING", c.t.faint) + 6;
        y += ui::para(c, x, y, w,
             g.sim->has_camera()
               ? "Drag to orbit the scene. Use the standard views in Controls, or hold Shift while dragging to pan."
               : "Drawn by the sim itself into an RGB image rather than a grid of palette "
                 "indices.", c.t.dim) + 10;
        // Guard the whole block, not just its heading. A one-line `if` in front
        // of the header left the camera instructions themselves showing beside
        // a chart that has no camera.
        if (g.sim->has_camera()) {
            y += ui::text(c, x, y, "CAMERA", c.t.faint) + 6;
            y += ui::para(c, x, y, w,
                 "Middle-drag or drag to orbit  ·  shift/right-drag to pan  ·  wheel zooms "
                 "toward the POINTER  ·  ctrl-click a voxel to orbit that instead of the centre. "
                 "1 front · 3 right · 7 top · 0 iso · 5 parallel/perspective · F fit · H recentre.",
                 c.t.dim) + 10;
        }
        y += ui::text(c, x, y, "TIMELINE VIEW", c.t.faint) + 6;
        y += ui::para(c, x, y, w,
             g.sim->has_camera()
               ? "The recorded timeline stores the coarse index view below, so rewinding "
                 "shows depth bands rather than the lit render."
               : "The timeline previews recorded index snapshots. Returning to Live resumes the current simulation; scrubbing does not restore model state.", c.t.faint) + 8;
    }
    // Name the legend for what it actually is. A chart of accuracy curves is
    // not "depth bands", and a sim that renders itself is not using this
    // palette to colour what you are looking at.
    y += ui::text(c, x, y,
                  !selfRendered            ? "LEGEND"
                  : g.sim->has_camera()    ? "  depth bands (timeline view)"
                                           : "  recorded series", c.t.faint) + 8;
    // The legend doubles as the paint picker: click a swatch to paint that
    // state. It is already the authoritative list of what the states are, so
    // making it clickable beats inventing a second control that can disagree.
    g.palRects.clear();
    const auto& pal0 = g.sim->palette();
    for (std::size_t i = 0; i < pal0.size(); ++i) {
        const auto& sw  = pal0[i];
        const bool  sel = (g.paintIdx < 0) ? (i == g.sim->paint_value())
                                           : (int(i) == g.paintIdx);
        RECT row{ x - 4, y - 1, a.right - 16, y + 16 };
        g.palRects.push_back(row);
        if (sel && g.brush) ui::fill(c, row, c.t.active);
        RECT sq{ x, y + 2, x + 11, y + 13 };
        ui::fill(c, sq, RGB(sw.colour.r, sw.colour.g, sw.colour.b));
        ui::frame(c, sq, sel ? c.t.amber : c.t.line);
        ui::tip(c, row, "State " + std::to_string(i) + ": " + sw.label +
                        ".  With the brush on, click to paint this state.");
        ui::text(c, x + 18, y, sw.label, sel ? c.t.ink : c.t.dim);
        y += 18;
    }
    if (g.brush && !g.sim->editable())
        y += ui::para(c, x, y + 4, w, "This sim renders a derived view, so the brush "
                      "stamps its seed pattern instead of painting cells.", c.t.faint) + 4;

    }
    if(g.inspector==0) {
        const auto& wf=currentWorkflow();
        y+=ui::para(c,x,y,w,wf.title,c.t.ink,c.bold)+8;
        y+=ui::para(c,x,y,w,ruleLab()?
            "Choose an example or paste a rule. Apply starts a new run.":wf.hint,c.t.dim)+16;
        if(auto* sim=ruleLab()) {
            const auto draft=bench::parse_rule(g.ruleText);
            const bool changed=g.ruleText!=sim->spec().text;
            y+=ui::text(c,x,y,"RULESTRING",c.t.faint)+8;
            g.ruleRect=RECT{x,y,x+w,y+32};
            ui::record(c,g.ruleRect);
            const int ruleClip=SaveDC(c.dc);IntersectClipRect(c.dc,x,y,x+w,y+32);
            ui::text_field(c,g.ruleRect,g.ruleText,g.ruleFocus,"B3/S23");
            RestoreDC(c.dc,ruleClip);
            ui::tip(c,g.ruleRect,"Click to edit. Ctrl+A selects all; Ctrl+V pastes; Ctrl+Z undoes. Enter applies and restarts; Escape restores the applied rule.");
            y+=40;
            const int half=(w-6)/2;
            if(ui::button(c,RECT{x,y,x+half,y+32},"Apply & restart",true,draft.ok))PostMessageW(g.hwnd,WM_APP+9,1,0);
            if(ui::button(c,RECT{x+half+6,y,x+w,y+32},"Cancel",false,changed))PostMessageW(g.hwnd,WM_APP+9,0,0);
            y+=42;
            if(!draft.ok)y+=ui::para(c,x,y,w,draft.error,c.t.bad)+8;
            else if(!g.ruleMsg.empty())y+=ui::para(c,x,y,w,g.ruleMsg,c.t.bad)+8;
            else {
                y+=ui::para(c,x,y,w,std::to_string(draft.neighbors())+" neighbours · "+std::to_string(draft.states)+" states · wraps at edges",c.t.acc)+6;
                if(changed)y+=ui::para(c,x,y,w,"Ready to apply: "+draft.text,c.t.dim)+6;
            }
            if(changed)y+=ui::para(c,x,y,w,"Currently applied: "+sim->spec().text,c.t.faint)+8;
            RECT examples{x,y,x+w,y+30};
            if(ui::button(c,examples,"Example rules  ▾")) {
                std::vector<MenuItem> items;
                for(const auto& sample:std::vector<std::pair<std::string,std::string>>{
                    {"Conway's Life","B3/S23"},{"HighLife","B36/S23"},{"Seeds","B2/S"},
                    {"Brian's Brain · 3 states","B2/S/3"},{"Just Friends · Hensel","B2-a/S12"},
                    {"Four neighbours","B2/S12V"},{"Six neighbours","B2/S34H"}}) {
                    const auto rule=sample.second;
                    items.push_back({sample.first+"  ("+rule+")",[rule]{closeRuleEditor();g.ruleText=rule;g.ruleMsg.clear();}});
                }
                queueMenu(examples,std::move(items));
            }
            y+=38;
            y+=ui::para(c,x,y,w,"B birth · S survival · /states fading. Hensel letters and H/V supported. See Guide for formats and shortcuts.",c.t.faint)+10;
            if(ui::button(c,RECT{x,y,x+w,y+28},"Rulestring reference"))
                ShellExecuteW(g.hwnd,L"open",L"https://golly.sourceforge.io/Help/Algorithms/QuickLife.html",nullptr,nullptr,SW_SHOWNORMAL);
            y+=40;ui::hline(c,x,x+w,y,c.t.line);y+=14;
        }
        if(auto* sim=movement()) {
            motionUi.edit=RECT{x,y,x+w,y+34};
            if(ui::button(c,motionUi.edit,sim->editing()?"Return to experiment":"Edit body",sim->editing()))editMovement();
            y+=42;
            if(sim->editing()) {
                const int bw=(w-12)/3;
                const char* names[]={"Move","Joint","Bone","Muscle","Erase","Undo"};
                for(int i=0;i<6;++i){int xx=x+(i%3)*(bw+6),yy=y+(i/3)*38;motionUi.tools[i]=RECT{xx,yy,xx+bw,yy+32};
                    if(ui::button(c,motionUi.tools[i],names[i],i<5&&int(sim->tool())==i)) {
                        if(i==5)sim->undo();else sim->setTool(bench::Locomotion::Tool(i));
                    }
                }
                y+=82;y+=ui::para(c,x,y,w,std::string(sim->toolHint())+" Keys 1-5 choose tools. Ctrl+Z undoes.",c.t.dim)+12;
                RECT r{x,y,x+w,y+32};
                if(ui::button(c,r,"Starter bodies  ▾"))queueMenu(r,{
                    {"Walker",[]{if(auto* m=movement())m->preset(0);}},
                    {"Triangle hopper",[]{if(auto* m=movement())m->preset(1);}},
                    {"Crawler",[]{if(auto* m=movement())m->preset(2);}},
                    {"Empty canvas (can undo)",[]{if(auto* m=movement())m->clearBody();}}});
                y+=40;
                const auto validation=sim->evolution().body.validate();
                y+=ui::para(c,x,y,w,validation.empty()?"Body ready. Returning starts a fresh experiment if the body changed.":validation,validation.empty()?c.t.acc:c.t.amber)+12;
            } else {
                motionUi.replay=RECT{x,y,x+w,y+32};
                if(ui::button(c,motionUi.replay,sim->replaying()?"Back to population":"Replay champion",sim->replaying())) {
                    if(sim->watchChampion()){g.training=false;g.running=sim->replaying();g.scrubAt=-1;}
                    else g.say(sim->message());
                }
                y+=40;
                if(ui::button(c,RECT{x,y,x+w,y+30},"Follow the best creature"))sim->followBest();
                y+=38;
            }
            const int half=(w-6)/2;
            if(ui::button(c,RECT{x,y,x+half,y+32},"Save body"))PostMessageW(g.hwnd,WM_APP+6,1,0);
            if(ui::button(c,RECT{x+half+6,y,x+w,y+32},"Load body"))PostMessageW(g.hwnd,WM_APP+6,0,0);
            y+=44;
            if(!sim->message().empty())y+=ui::para(c,x,y,w,sim->message(),c.t.amber)+12;
        }
        if(g.sim->has_camera()) {
            const int bw=(w-12)/3;
            using V=bench::Sim::StdView;
            const char* labels[]={"Front","Top","Isometric"};
            const V views[]={V::Front,V::Top,V::Iso};
            for(int i=0;i<3;++i) if(ui::button(c,RECT{x+i*(bw+6),y,x+i*(bw+6)+bw,y+30},labels[i])) g.sim->camera_view(views[i]);
            y+=36;
            if(ui::button(c,RECT{x,y,x+bw,y+30},"Fit view")) g.sim->camera_fit();
            if(ui::button(c,RECT{x+bw+6,y,x+w,y+30},g.sim->camera_is_ortho()?"Projection: parallel":"Projection: perspective")) g.sim->camera_ortho(!g.sim->camera_is_ortho());
            y+=44;
        }
        if(auto* vc=possessable()) {
            if(ui::button(c,RECT{x,y,x+w,y+32},vc->possession_live()?"Release agent":"Drive an agent",vc->possession_live())) possessKey('P');
            y+=40;
            if(vc->possession_live()) {
                const int cw=(w-12)/3;
                const char* labels[]={"Left","Forward","Right","Down","Back","Up","Mine","Craft","Eat"};
                const unsigned keys[]={'A','W','D','Q','S','E','Z','X','V'};
                for(int i=0;i<9;++i) {
                    int xx=x+(i%3)*(cw+6), yy=y+(i/3)*36;
                    if(ui::button(c,RECT{xx,yy,xx+cw,yy+30},labels[i])) possessKey(keys[i]);
                }
                y+=110;
                RECT r{x,y,x+w,y+30};
                if(ui::button(c,r,"More agent commands  ▾")) {
                    std::vector<MenuItem> items;
                    for(const auto& k:kPossKeys) { unsigned vk=k.vk; items.push_back({k.what,[vk]{possessKey(vk);}}); }
                    items.push_back({"Next agent",[]{possessKey('K');}});
                    queueMenu(r,std::move(items));
                }
                y+=40;
            }
        }

    // ── the rule itself ────────────────────────────────────────────────────
    // Laid out as rows of chips grouped by clause, because eighteen separate
    // full-width toggles would bury everything else in this pane.
    g.switchRects.clear();
    auto& sws = g.sim->switches();
    if (!sws.empty()) {
        y += 8; ui::hline(c, x, a.right - 16, y, c.t.line); y += 12;
        y += ui::text(c, x, y, "RULE", c.t.faint) + 6;
        std::string group;
        int cx = x;
        for (std::size_t i = 0; i < sws.size(); ++i) {
            if (sws[i].group != group) {
                group = sws[i].group;
                if (i) y += 26;
                y += ui::text(c, x, y, group, c.t.dim) + 4;
                cx = x;
            }
            RECT chip{ cx, y, cx + 22, y + 22 };
            RECT hit{}; IntersectRect(&hit,&chip,&g.notesRect); g.switchRects.push_back(hit);
            ui::chip(c, chip, sws[i].label, sws[i].value);
            ui::tip(c, chip, sws[i].help);
            cx += 25;
            if(cx+22>x+w) {cx=x; y+=27;}
        }
        y += 30;
        y += ui::para(c, x, y, w,
             "Click a bit to change the transition rule while it runs. The title "
             "and citation follow the rule, so an edit cannot keep another rule's "
             "name.", c.t.faint) + 8;
    }

    auto& knobs=g.sim->knobs();
    if(g.knobDrag.size()!=knobs.size()) g.knobDrag.assign(knobs.size(),false);
    const auto& primary=currentWorkflow().primary;
    auto isPrimary=[&](const auto& k){return primary.empty() || std::find(primary.begin(),primary.end(),k.key)!=primary.end();};
    if(!knobs.empty() && !(movement()&&movement()->editing())) {
        y+=ui::text(c,x,y,"MAIN CONTROLS",c.t.faint)+14;
        y+=ui::para(c,x,y,w,"Setup values are staged until Apply & restart. Other controls apply immediately.",c.t.faint)+12;
        if(primary.empty()) {for(std::size_t i=0;i<knobs.size();++i)drawControl(c,knobs[i],x,y,w,i);}
        else for(const auto& key:primary)for(std::size_t i=0;i<knobs.size();++i)if(knobs[i].key==key)drawControl(c,knobs[i],x,y,w,i);
        if(ui::button(c,RECT{x,y,x+w,y+32},g.advanced?"Less settings  ▴":"All other settings  ▾",g.advanced)) g.advanced=!g.advanced;
        y+=44;
        if(g.advanced) for(std::size_t i=0;i<knobs.size();++i) if(!isPrimary(knobs[i])) drawControl(c,knobs[i],x,y,w,i);
    }
    if(g.sim->editable()) {
        y+=ui::text(c,x,y,"PAINT STATES",c.t.faint)+10;
        const auto& pal=g.sim->palette();
        for(std::size_t i=0;i<pal.size();++i) {
            RECT r{x,y,x+w,y+28};
            if(ui::button(c,r,pal[i].label,g.brush && g.paintIdx==int(i))) {g.paintIdx=int(i);g.brush=true;g.placeAction=false;}
            y+=34;
        }
    }
    if(ui::button(c,RECT{x,y,x+w,y+32},g.displaySettings?"Display settings  ▴":"Display settings  ▾",g.displaySettings)) g.displaySettings=!g.displaySettings;
    y+=44;
    if(g.displaySettings) {
        auto display=[&](const char* label,float& value,float lo,float hi,bool& drag){
            ui::text(c,x,y,label,c.t.dim); char text[24];std::snprintf(text,sizeof text,"%.2f",double(value));
            ui::textRight(c,x+w,y,text,c.t.acc);y+=22;
            ui::slider(c,RECT{x,y,x+w,y+22},value,lo,hi,drag);y+=36;
        };
        display("Motion trails",g.view.trail,0.f,0.97f,g.dragTrail);
        display("Brightness",g.view.gamma,0.35f,1.6f,g.dragGamma);
        if(g.view.trail<=0.001f)g.raster.clear_accumulator();
    }
    }

    // How tall the content actually was, measured rather than guessed — it
    // changes with the sim, the number of knobs and the number of series.
    g.notesContentH = y + g.notesScroll - top;
    const int viewH = g.notesRect.bottom - top;
    const int boundedScroll=std::clamp(g.notesScroll,0,std::max(0,g.notesContentH-viewH));
    if(boundedScroll!=g.notesScroll){g.notesScroll=boundedScroll;InvalidateRect(g.hwnd,nullptr,FALSE);}
    if (g.notesContentH > viewH) {
        // A thumb, so there is something on screen saying more exists below.
        const int trackX = a.right - 5, trackH = viewH;
        const int thumbH = std::max(24, trackH * viewH / g.notesContentH);
        const int maxScroll = g.notesContentH - viewH;
        const int ty = top + (trackH - thumbH) *
                       (maxScroll > 0 ? g.notesScroll : 0) / (maxScroll > 0 ? maxScroll : 1);
        ui::fill(c, RECT{ trackX, top, trackX + 3, a.bottom }, c.t.sunken);
        ui::fill(c, RECT{ trackX, ty, trackX + 3, ty + thumbH }, c.t.line);
    }
}

// Paint into the sim's real grid, if it has one to offer.
//
// This used to call poke() for everything, which is why the brush appeared
// broken: poke() is optional and only four of the sims implemented it, so on
// the rest a stroke silently did nothing. poke() is now the *stamp* action —
// plant a loop, drop a patch of defectors — and ordinary painting goes through
// editable(), which any sim that renders its own state exposes.
// Client point -> 0..1 inside the sim's own rendered image, going through the
// same fit the raster used. Duplicating that maths is how a pick lands
// somewhere other than where the cursor is.
bool surfacePoint(POINT p, float& nx, float& ny) {
    if (!g.sim) return false;
    const bench::Surface* s = g.sim->surface();
    if (!s || s->empty()) return false;
    const auto fit = g.raster.fit_of(s->w, s->h, g.view);
    const float sx = (float(p.x - g.canvasRect.left) - fit.ox) / fit.scale;
    const float sy = (float(p.y - g.canvasRect.top)  - fit.oy) / fit.scale;
    if (sx < 0 || sy < 0 || sx >= float(s->w) || sy >= float(s->h)) return false;
    nx = sx / float(s->w); ny = sy / float(s->h);
    return true;
}

void paintAt(POINT p, bool erase) {
    if (!g.sim || !ui::inside(g.canvasRect, p)) return;
    const bench::Field& f = g.sim->field();
    const auto cell = g.raster.cell_at(float(p.x - g.canvasRect.left),
                                       float(p.y - g.canvasRect.top), f.w, f.h, g.view);
    if (cell.first < 0) return;

    bench::Field* ed = g.sim->editable();
    if (!ed) {                                  // derived-view sim: stamp instead
        g.sim->poke(float(cell.first) / f.w, float(cell.second) / f.h);
        g.scrubAt = -1;
        return;
    }
    const std::uint8_t v = erase ? g.sim->erase_value()
                                 : (g.paintIdx >= 0 ? std::uint8_t(g.paintIdx)
                                                    : g.sim->paint_value());
    const int r = g.brushSize;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (dx*dx + dy*dy > r*r + r) continue;          // round brush
            int x = (cell.first  + dx) % ed->w; if (x < 0) x += ed->w;
            int y = (cell.second + dy) % ed->h; if (y < 0) y += ed->h;
            ed->set(x, y, v);
        }
    // Editing while rewound would be painting on a snapshot you are about to
    // discard, so a stroke always returns you to the live state.
    g.scrubAt = -1;
}

constexpr int kHead = 58;

void paneCanvas(ui::Ctx& c, RECT a) {
    // A slim header over the canvas. The sim used to run flush against the top
    // of the window with nothing naming it — and now that the rule is editable
    // you need to see WHICH rule is running without looking away from the grid.
    {
        RECT hd{ a.left, a.top, a.right, a.top + kHead };
        ui::fill(c, hd, c.t.panel);
        ui::hline(c, hd.left, hd.right, hd.bottom - 1, c.t.line);
        if(g.sim) {
            const int textRight=hd.right-(g.focus?180:14);
            ui::text_clipped(c,hd.left+14,hd.top+5,g.sim->about().title,c.t.ink,textRight," ",c.bold);
            std::string detail=g.sim->subtitle();
            if(!detail.empty())detail+="  ·  ";
            detail+=std::string(sortingLab()?"event ":movement()?"tick ":"gen ")+std::to_string(g.sim->generation());
            if(!g.identity.empty())detail+="  ·  "+g.identity;
            ui::text_clipped(c,hd.left+14,hd.top+32,detail,c.t.acc,textRight);
        }
        a.top += kHead;
    }
    const int w = a.right - a.left, h = a.bottom - a.top;
    if (w <= 0 || h <= 0 || !g.sim) return;
    g.canvasRect = a;
    g.raster.resize(w, h);

    const bench::Field* f = &g.sim->field();
    if (g.scrubAt >= 0) if (const bench::Field* s = g.hist.at(std::size_t(g.scrubAt))) f = s;

    // A sim that renders itself is drawn from its own image. Rewinding still
    // uses the index snapshots, because that is what the timeline stores — so
    // scrubbing a 3D sim shows the coarse view and says so by looking different.
    const bench::Surface* surf = (g.scrubAt < 0) ? g.sim->surface() : nullptr;
    if (surf && !surf->empty()) g.raster.draw(*surf, g.view);
    else                        g.raster.draw(*f, g.sim->palette(), g.view);
    // Outline the field. Everything here wraps, so the boundary is where a
    // glider leaving the right side reappears on the left — worth being able
    // to see, and without it the canvas has no visible extent at all.
    if (!(surf && !surf->empty()))
        g.raster.outline_field(f->w, f->h, g.view, 34, 44, 52);

    // cursor cell readout
    g.curCellX = g.curCellY = -1;
    if (ui::inside(a, c.mouse)) {
        const auto cell = g.raster.cell_at(float(c.mouse.x - a.left), float(c.mouse.y - a.top),
                                           f->w, f->h, g.view);
        g.curCellX = cell.first; g.curCellY = cell.second;
        // Outline the actual footprint, so the brush size is visible rather
        // than something you infer from the damage afterwards.
        if (g.brush && cell.first >= 0) {
            const int r = g.brushSize;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx*dx + dy*dy > r*r + r) continue;
                    int hx = (cell.first + dx) % f->w; if (hx < 0) hx += f->w;
                    int hy = (cell.second + dy) % f->h; if (hy < 0) hy += f->h;
                    g.raster.highlight(hx, hy, f->w, f->h, g.view, 255, 210, 90);
                }
        }
        if (cell.first >= 0)
            g.raster.highlight(cell.first, cell.second, f->w, f->h, g.view, 255, 255, 255);
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    static std::vector<std::uint8_t> bgra;
    const std::size_t n = std::size_t(w) * h * 4;
    if (bgra.size() != n) bgra.resize(n);
    const std::uint8_t* s = g.raster.pixels();
    for (std::size_t i = 0; i < n; i += 4) {
        bgra[i] = s[i+2]; bgra[i+1] = s[i+1]; bgra[i+2] = s[i]; bgra[i+3] = 255;
    }
    StretchDIBits(c.dc, a.left, a.top, w, h, 0, 0, w, h, bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);

    // overlay: what is under the cursor, in the sim's own vocabulary
    if (g.curCellX >= 0) {
        const std::uint8_t idx = f->at(g.curCellX, g.curCellY);
        const auto& pal = g.sim->palette();
        const std::string label = idx < pal.size() ? pal[idx].label : "?";
        char b[160];
        std::snprintf(b, sizeof b, "(%d, %d)  %s", g.curCellX, g.curCellY, label.c_str());
        const SIZE sz = ui::measure(c, b, c.font);
        RECT tip{ a.left + 10, a.bottom - 26, a.left + 22 + sz.cx, a.bottom - 6 };
        ui::fill(c, tip, RGB(0x0d,0x12,0x16)); ui::frame(c, tip, c.t.line);
        ui::text(c, tip.left + 6, tip.top + 3, b, c.t.ink);
    }

    // ── what the possessed agent sees and carries ───────────────────────────
    //
    // Top-left, over the render, because it has to be readable while you are
    // driving — a panel on the far side of the window is a thing you look at
    // between sessions. The lines come straight out of the sim's own
    // observation and inventory (see possession_readout), so they cannot drift
    // from what the agent is actually acting on.
    if (bench::VoxelCity* vc = possessable(); vc && !vc->possession_live()) {
        // A feature nobody can find is a feature nobody has. One faint line,
        // bottom-right of the canvas so it is clear of the cursor readout at
        // bottom-left and of the panel possession itself draws top-left.
        ui::textRight(c, a.right - 12, a.bottom - 26,
                      "P  take one of these agents and drive it yourself", c.t.faint);
    } else if (vc) {
        const auto lines = vc->possession_readout();
        int wmax = 0;
        for (const auto& l : lines) wmax = std::max<int>(wmax, ui::measure(c, l, c.font).cx);
        const int lh = ui::measure(c,"M",c.font).cy+2;
        RECT box{ a.left + 10, a.top + 10, a.left + 26 + wmax,
                  a.top + 16 + lh * int(lines.size()) };
        // Clamp, so a long inventory line cannot push the panel off the canvas
        // and take the health readout with it.
        box.right = std::min<LONG>(box.right, a.right - 10);
        ui::fill(c, box, RGB(0x0d,0x12,0x16));
        ui::frame(c, box, vc->possessed_auto() ? c.t.amber : c.t.acc);
        for (std::size_t i = 0; i < lines.size(); ++i)
            ui::text_clipped(c, box.left + 8, box.top + 6 + lh * int(i), lines[i],
                     i == 0 ? (vc->possessed_auto() ? c.t.amber : c.t.acc) : c.t.ink,box.right-8);
        // The tape, which is the actual deliverable: a run nobody recorded is a
        // run nobody can re-score.
        char t[160];
        std::snprintf(t, sizeof t, "recording: %zu decisions over %lld ticks  ·  "
                                   "replayable",
                      vc->tape().entries.size(), (long long)vc->tape().ticks);
        ui::text_clipped(c, box.left + 8, box.bottom + 4, t, c.t.faint,a.right-12);
    }
}

void paneScrub(ui::Ctx& c, RECT a) {
    ui::fill(c, a, RGB(0x0d,0x12,0x16));
    g.scrubRect = a;
    ui::tip(c, a,
        "Recorded pictures of earlier states. Drag to inspect them; Run returns to the live state. Snapshots are bounded by generation "
        "interval, by wall-clock rate and by count — so running at 600 steps a second records "
        "no faster than about thirty frames a second and cannot exhaust memory.");
    if(!g.hist.recording_snapshots()) {
        ui::text(c,a.left+12,a.top+4,"Large run · timeline off · measurements retained",c.t.faint);
        g.timelineTrack=RECT{};return;
    }
    const std::size_t d = g.hist.depth();
    // Reserve by MEASURING the readout, not by guessing 190px. The guess was
    // short and the text ran off the right edge — "12 frames · 3 MB" was cut to
    // "12 frame".
    char readout[160] = "timeline: recording…";
    const int idx = (d == 0) ? 0 : (g.scrubAt < 0 ? int(d) - 1 : g.scrubAt);
    if (d) std::snprintf(readout, sizeof readout, "%s  %s %llu  ·  %zu frames  ·  %zu MB",
            g.scrubAt < 0 ? "LIVE" : "REWOUND",
            sortingLab()?"event":movement()?"tick":"gen",
            (unsigned long long)(g.scrubAt<0&&g.sim?g.sim->generation():g.hist.generation_at(std::size_t(idx))), d,
            g.hist.bytes() / (1024*1024));
    const int reserve = ui::measure(c, readout, nullptr).cx + 24;
    const int x0 = a.left + 10, x1 = std::max(x0 + 50, int(a.right) - reserve);
    g.timelineTrack=RECT{x0,a.top,x1,a.bottom};
    ui::hline(c, x0, x1, a.top + 12, c.t.line);
    if (d) {
        const int px = x0 + int(double(idx) / double(d > 1 ? d - 1 : 1) * (x1 - x0));
        RECT k{ px - 2, a.top + 5, px + 2, a.top + 19 };
        ui::fill(c, k, g.scrubAt < 0 ? c.t.acc : c.t.amber);
        ui::textRight(c, a.right - 12, a.top + 4, readout,
                      g.scrubAt < 0 ? c.t.faint : c.t.amber);
    } else ui::text(c, x0, a.top + 4, readout, c.t.faint);
}

void toggleFullscreen();

void paneTools(ui::Ctx& c, RECT a) {
    ui::fill(c,a,c.t.panel); ui::hline(c,a.left,a.right,a.top,c.t.line);
    const int saved=SaveDC(c.dc); IntersectClipRect(c.dc,a.left,a.top,a.right,a.bottom);
    const RECT oldClip=c.inputClip; c.inputClip=a;
    ui::Bar b{a.left+12,a.top+12,32}; b.x0=b.x;b.limit=a.right-12;b.lineH=40;
    auto button=[&](const std::string& label,bool active=false,bool enabled=true){
        RECT r=b.next(ui::measure(c,label,nullptr).cx+26);
        return ui::button(c,r,label,active,enabled);
    };
    if(button("Library")) openLibrary();
    if(button("Inspector",g.showRight)) {cancelNumber();g.showRight=!g.showRight;}
    const bool live=g.sim!=nullptr;
    // Drawn through the same Bar as the lambda above, so the layout is identical;
    // done by hand only to keep the rectangle, because this is the one control
    // that still answers while a unit is in flight (see paintBusy).
    const std::string runLabel=g.running||g.training?"Pause":sortingLab()&&sortingLab()->finished()?"Replay":"Run";
    const RECT runRect=b.next(ui::measure(c,runLabel,nullptr).cx+26);
    g.runButton=runRect;
    if(ui::button(c,runRect,runLabel,g.running||g.training,live)) {
        if(movement()&&movement()->editing()){editMovement();g.running=!movement()->editing();}
        else if(g.training) {g.training=false;g.trainTo=0;g.running=false;}
        else {replayFinishedSort();g.running=!g.running;g.scrubAt=-1;}
    }
    if(button("Step",false,live&&!(movement()&&movement()->editing()))) {g.running=false;g.training=false;g.sim->step();g.hist.observe(*g.sim);g.scrubAt=-1;}
    if(button("Reset",false,live)) resetRun();
    if(live && g.sim->epoch_name()) {
        RECT r=b.next(76);
        if(ui::button(c,r,"Train ▾",g.training)) {
            const std::string unit=g.sim->epoch_name();
            auto train=[](int n){g.running=false;g.training=true;g.scrubAt=-1;g.plateau.reset();g.trainTo=n?g.sim->epoch_count()+n:0;};
            queueMenu(r,{
                {"Next " + unit,[train]{train(1);}},
                {movement()?"Next 25 generations":"Next 25 epochs",[train]{train(25);}},
                {movement()?"Next 100 generations":"Next 100 epochs",[train]{train(100);}},
                {"Train continuously",[train]{train(0);}},
                {"Stop after 15 epochs without improvement",[]{g.stopStale=!g.stopStale;g.plateau.reset();},g.stopStale},
                {"Keep this run for comparison",[]{g.hist.keep_baseline();g.say("Run kept. Reset or change setup to compare.");},false,g.hist.epoch_samples()>=2},
                {"Clear comparison",[]{g.hist.clear_baseline();},false,g.hist.has_baseline()},
                {"Show measurements",[]{g.inspector=1;g.notesScroll=0;g.showRight=true;}}
            });
        }
        ui::tip(c,r,movement()?"Train in generations. Movement physics runs in short batches so you can pause during a trial.":"Train in the simulation's own units. Each epoch completes before the next UI action is processed.");
    }
    if(live && g.sim->editable() && button("Paint",g.brush)) {g.brush=!g.brush;g.placeAction=false;g.inspector=0;}
    if(live && !currentWorkflow().action.empty() && button(currentWorkflow().action,g.placeAction)) {
        if(currentWorkflow().placed) {g.placeAction=!g.placeAction;g.brush=false;g.say(g.placeAction?"Click the canvas to " + currentWorkflow().action:"Placement cancelled");}
        else if(g.sim->poke(0.5f,0.5f)) {if(sortingLab())g.running=false;g.hist.clear();g.raster.clear_accumulator();g.scrubAt=-1;g.say(currentWorkflow().action);}
    }
    if(g.sel<g.rows.size() && g.rows[g.sel].plugin && button(g.buildProcess?"Building...":"Build",false,!g.buildProcess)) buildPlugin();
    if(button("Save as",false,live||(g.sel<g.rows.size()&&g.rows[g.sel].plugin)))PostMessageW(g.hwnd,WM_APP+7,1,0);
    RECT perf=b.next(g.largeRun?112:110);
    if(ui::button(c,perf,g.largeRun?"Large run ▾":"Balanced ▾")) queueMenu(perf,{
        {"Balanced — timeline on, display up to 60 fps",[]{setLargeRun(false);},!g.largeRun},
        {"Large run — timeline off, display up to 15 fps",[]{setLargeRun(true);},g.largeRun}
    });
    RECT more=b.next(76);
    if(ui::button(c,more,"Tools ▾")) {
        std::vector<MenuItem> menu={
            {"Save simulation as...",[]{PostMessageW(g.hwnd,WM_APP+7,1,0);},false,live||(g.sel<g.rows.size()&&g.rows[g.sel].plugin)},
            {"Open simulation...",[]{PostMessageW(g.hwnd,WM_APP+7,0,0);}},
            {"Open custom code folder",[]{ShellExecuteW(g.hwnd,L"open",ui::widen(bench::Paths::get().plugins()).c_str(),nullptr,nullptr,SW_SHOWNORMAL);}},
            {"License & source",[]{
                MessageBoxW(g.hwnd,
                    L"Life-sim Workbench\nCopyright 2026 Life-sim Workbench contributors.\n\n"
                    L"Free software: you may copy, modify and redistribute it under GNU GPL version 3 or any later version.\n"
                    L"There is no warranty, including merchantability or fitness for a particular purpose.\n\n"
                    L"The application folder contains LICENSE, LICENSING.md and third-party notices. "
                    L"Installed copies include the matching workbench source in the source folder; "
                    L"optional compiler sources are in toolchain/sources.\n\n"
                    L"Press OK to open the application folder.",L"License & source",MB_OK|MB_ICONINFORMATION);
                ShellExecuteW(g.hwnd,L"open",ui::widen(bench::Paths::get().root()).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            }},
            {"Run speed: 10 ticks/s",[]{g.sps=10;},g.sps==10},
            {"Run speed: 30 ticks/s",[]{g.sps=30;},g.sps==30},
            {"Run speed: 60 ticks/s",[]{g.sps=60;},g.sps==60},
            {"Run speed: maximum",[]{g.sps=600;},g.sps==600},
            {"Fit view",[]{if(g.sim&&g.sim->has_camera())g.sim->camera_fit();else {g.view.zoom=1;g.view.pan_x=g.view.pan_y=0;}},false,live},
            {"Grid overlay",[]{g.view.grid=!g.view.grid;},g.view.grid,live&&!g.sim->has_camera()},
            {"Identify pattern",[]{identifyPattern();},false,live&&g.sim->editable()!=nullptr},
            {"Save pattern (RLE)",[]{savePattern();},false,live&&g.sim->editable()!=nullptr},
            {"Export measurements (CSV)",[]{saveMetrics();},false,live},
            {"Show source",[]{g.mode=Mode::Code;}},
            {"Show simulation",[]{g.mode=Mode::Run;}},
            {"Edit source in editor",[]{openSource();}},
            {"Display settings",[]{g.showRight=true;g.inspector=0;g.displaySettings=true;g.advanced=false;g.notesScroll=100000;}},
            {"Focus canvas (F3)",[]{g.focus=!g.focus;},g.focus},
            {"Fullscreen (F11)",[]{toggleFullscreen();},g.fullscreen},
            {"Return to live timeline",[]{g.scrubAt=-1;}}
        };
        // This setup, in its own process, with the whole machine. See launchDedicated().
        if(live) {
            if(const char* en=g.sim->epoch_name()) for(long long n:{100LL,1000LL})
                menu.push_back({"Dedicated training: "+bench::run::count_of(n,en),[n]{launchDedicated(n,true);},false,!g.dedicatedProcess});
            for(long long n:{100000LL,1000000LL})
                menu.push_back({"Dedicated run: "+bench::run::count_of(n,"step"),[n]{launchDedicated(n,false);},false,!g.dedicatedProcess});
            menu.push_back({"Dedicated run: until stopped",[]{launchDedicated(0,false);},false,!g.dedicatedProcess});
        }
        menu.push_back({"Stop dedicated run",[]{stopDedicated();},false,g.dedicatedProcess!=nullptr&&!g.dedicatedStopAt});
        menu.push_back({"Open dedicated results",[]{openDedicatedResults();},false,!g.dedicatedOut.empty()});
        if(g.brush) for(int radius:{0,1,2,4,8}) menu.push_back({"Brush diameter: "+std::to_string(radius*2+1),[radius]{g.brushSize=radius;},g.brushSize==radius});
        queueMenu(more,std::move(menu));
    }
    g.toolRows=b.lines;
    RestoreDC(c.dc,saved); c.inputClip=oldClip;
}

void paneCode(ui::Ctx& c, RECT a) {
    const int clip=SaveDC(c.dc);IntersectClipRect(c.dc,a.left,a.top,a.right,a.bottom);
    struct ClipRestore{HDC dc;int id;~ClipRestore(){RestoreDC(dc,id);}} clipRestore{c.dc,clip};
    const auto previous=c.font;
    if(!g.mono)g.mono=CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
    c.font=g.mono;
    struct FontRestore{ui::Ctx& c;HFONT previous;~FontRestore(){c.font=previous;}} restore{c,previous};
    ui::fill(c, a, RGB(0x0a,0x0e,0x12));
    const int lh = ui::measure(c,"M",c.font).cy+3, x = a.left + 14;
    int y = a.top + 10;
    const std::string& src = g.sel < g.rows.size() ? g.rows[g.sel].source : std::string();
    y += ui::text(c, x, y, src, c.t.amber, c.bold) + 8;

    const int avail = a.bottom - y - (g.showBuild ? 150 : 8);
    const int rows = avail / lh;
    for (int i = 0; i < rows && g.codeTop + i < int(g.code.size()); ++i) {
        char num[12]; std::snprintf(num, sizeof num, "%4d", g.codeTop + i + 1);
        ui::text(c, x, y, num, RGB(0x2c,0x38,0x42));
        drawCodeLine(c, x + 44, y, g.code[std::size_t(g.codeTop + i)]);
        y += lh;
    }
    if (g.showBuild) {
        RECT out{ a.left, a.bottom - 148, a.right, a.bottom };
        ui::fill(c, out, RGB(0x0d,0x11,0x15));
        ui::hline(c, out.left, out.right, out.top, c.t.line);
        int by = out.top + 6;
        ui::text(c, x, by, g.buildOk ? "BUILD OK" : "BUILD FAILED",
                 g.buildOk ? c.t.good : c.t.bad, c.bold);
        RECT close{ out.right - 70, out.top + 4, out.right - 12, out.top + 24 };
        if (ui::button(c, close, "close")) g.showBuild = false;
        by += 20;
        const int maxLines = (out.bottom - by) / 14;
        const int start = std::max(0, int(g.build.size()) - maxLines);
        for (int i = start; i < int(g.build.size()); ++i) {
            const std::string& L = g.build[std::size_t(i)];
            const bool err = L.find("error") != std::string::npos;
            ui::text(c, x, by, L, err ? c.t.bad : c.t.dim);
            by += 14;
        }
    }
}

void paneStatus(ui::Ctx& c, RECT a) {
    ui::fill(c, a, RGB(0x0d,0x12,0x16));
    ui::hline(c, a.left, a.right, a.top, c.t.line);
    // Hotkeys on the left, machine summary on the right, and the transient
    // message wins the right slot while it is showing. The machine note used to
    // be drawn across the toolbar's button row and collided with it.
    // Decide the right-hand message FIRST, then give the hotkey list whatever
    // room is left. Drawing both from their own edges made them overlap.
    std::string msg; COLORREF msgCol = c.t.faint;
    if (!g.toast.empty() && GetTickCount() - g.toastAt < 5000) { msg = g.toast; msgCol = c.t.acc; }
    else if (!g.dedicatedLine.empty()) { msg = g.dedicatedLine; msgCol = c.t.acc; }
    else if (g.throttled) {
        char b[128];
        std::snprintf(b, sizeof b, "THROTTLED — %d steps/frame at %.2f ms each",
                      g.lastAllowed, g.msPerStep);
        msg = b; msgCol = c.t.amber;
    } else if (g.hw.memoryTight()) { msg = "memory tight — timeline paused"; msgCol = c.t.amber; }
    else msg = g.hw.note;

    const int msgWidth=std::min(int(a.right-a.left)-24,int(ui::measure(c,msg,nullptr).cx));
    ui::text_clipped(c,a.right-14-msgWidth,a.top+5,msg,msgCol,a.right-14);
    ui::tip(c,a,msg);
    const int limit = a.right - 14 - ui::measure(c, msg, nullptr).cx - 20;
    ui::text_clipped(c, a.left + 12, a.top + 5,
        "space play  ·  . step  ·  , back  ·  R reset  ·  F1 run  ·  F2 code  "
        "·  F5 reload  ·  F6 build  ·  F7 save .rle  ·  F8 identify  ·  F9 metrics.csv  "
        "·  wheel zoom  ·  drag pan", c.t.faint, limit);
}

// ── window ──────────────────────────────────────────────────────────────────
// Borderless fullscreen on the monitor the window is currently on — not a
// display-mode change. Changing the mode would give the sim the same pixels
// and cost a resolution switch, a black flash, and every other window on the
// desktop being rearranged when it switches back.
void toggleFullscreen() {
    HWND h = g.hwnd;
    if (!h) return;
    if (!g.fullscreen) {
        g.prevPlace.length = sizeof(g.prevPlace);
        GetWindowPlacement(h, &g.prevPlace);
        g.prevStyle = DWORD(GetWindowLongPtr(h, GWL_STYLE));
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtr(h, GWL_STYLE, LONG_PTR(g.prevStyle & ~DWORD(WS_OVERLAPPEDWINDOW)));
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g.fullscreen = true;
        g.say("fullscreen  ·  F11 back  ·  F3 for the panels as well");
    } else {
        SetWindowLongPtr(h, GWL_STYLE, LONG_PTR(g.prevStyle));
        SetWindowPlacement(h, &g.prevPlace);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g.fullscreen = false;
        g.say("windowed");
    }
}

void layout(HWND hwnd, HDC dc) {
    ++g.layoutCalls;
    RECT cr; GetClientRect(hwnd, &cr);
    g.ui.dc = dc;
    g.ui.hot = 0;
    g.ui.overflow=0;g.ui.widgets.clear();g.ui.inputClip=cr;
    g.rowRects.clear();g.palRects.clear();g.switchRects.clear();g.notesRect=g.rosterRect=g.scrubRect=g.ruleRect=g.timelineTrack=g.canvasRect=g.leftSplit=g.rightSplit=RECT{};
    ui::fill(g.ui, cr, g.ui.t.bg);

    if(g.library) {syncRuleEditor();paneLibrary(g.ui,cr);g.ui.clicked=false;g.ui.tipText.clear();return;}
    // First layout adopts the scaled defaults; a DPI change re-adopts them,
    // because a width in pixels means something different on the new monitor.
    if (g.leftW  <= 0) g.leftW  = kLeft;
    if (g.rightW <= 0) g.rightW = kRight;
    // Never let the panels squeeze the canvas out of existence, however small
    // the window gets or however far the splitter is dragged. The canvas keeps
    // a quarter of the width as an absolute floor.
    const int W = int(cr.right);
    const int floorC = std::max(160, W / 4);
    int lw = (g.focus || !g.showLeft)  ? 0 : g.leftW;
    int rw = (g.focus || !g.showRight) ? 0 : g.rightW;
    if (lw + rw > W - floorC) {
        const int over = lw + rw - (W - floorC);
        // Take it off the wider one first, so shrinking the window eats into
        // the notes before it starts on the roster.
        if (rw >= lw) rw = std::max(0, rw - over);
        else          lw = std::max(0, lw - over);
    }

    const int centreL = lw, centreR = W - rw;
    const int toolsH = kTools + (g.toolRows - 1) * 40;
    const int bodyB = g.focus ? cr.bottom : cr.bottom - toolsH - kStatus;

    if (g.mode == Mode::Run) {
        // Focus mode drops the scrub bar too. It is the timeline, not the sim,
        // and this mode exists to hand every row of pixels to the sim.
        const int canvasB = g.focus ? bodyB : bodyB - kScrub;
        RECT canvas{ centreL, 0, centreR, canvasB };
        paneCanvas(g.ui, canvas);
        if (!g.focus) {
            RECT sc{ centreL, bodyB - kScrub, centreR, bodyB };
            paneScrub(g.ui, sc);
        }
    } else {
        RECT code{ centreL, 0, centreR, bodyB };
        paneCode(g.ui, code);
    }
    if (lw > 0) paneRoster(g.ui, RECT{ 0, 0, lw, cr.bottom });
    if (rw > 0) paneNotes (g.ui, RECT{ W - rw, 0, W, cr.bottom });
    syncRuleEditor();
    // The splitters, drawn as a hairline that brightens under the cursor. They
    // are only there when the panel is, since there is nothing to resize
    // otherwise — and the hit band is wider than the line, because a 1px
    // target is a target you miss.
    g.leftSplit = g.rightSplit = RECT{};
    if (lw > 0) {
        g.leftSplit = RECT{ lw - kSplit, 0, lw + kSplit, cr.bottom };
        const bool hot = g.splitDrag == -1 || ui::inside(g.leftSplit, g.ui.mouse);
        ui::vline(g.ui, lw - 1, 0, cr.bottom, hot ? g.ui.t.acc : g.ui.t.line);
    }
    if (rw > 0) {
        const int x = W - rw;
        g.rightSplit = RECT{ x - kSplit, 0, x + kSplit, cr.bottom };
        const bool hot = g.splitDrag == 1 || ui::inside(g.rightSplit, g.ui.mouse);
        ui::vline(g.ui, x, 0, cr.bottom, hot ? g.ui.t.acc : g.ui.t.line);
    }
    if (g.focus) {
        if(ui::button(g.ui,RECT{cr.right-160,12,cr.right-16,44},"Exit focus (F3)"))g.focus=false;
        g.ui.tipText.clear();g.ui.clicked=false;return;
    }
    const int rowsBefore = g.toolRows;
    paneTools (g.ui, RECT{ centreL, bodyB, centreR, cr.bottom - kStatus });
    // The bar only discovers it needs a second row while drawing. Repaint once
    // if that changed, so the layout settles on the same frame rather than
    // leaving one frame of clipped buttons behind.
    if (g.toolRows != rowsBefore) InvalidateRect(g.hwnd, nullptr, FALSE);
    paneStatus(g.ui, RECT{ centreL, cr.bottom - kStatus, centreR, cr.bottom });

    // Tooltips last, and only after the cursor has SETTLED. One that appears
    // the instant the pointer crosses a control is noise you learn to ignore;
    // one that waits for you to stop is help. The timer restarts whenever the
    // cursor moves to a different widget, so sweeping across a row of rule bits
    // shows nothing until you actually pause on one.
    const bool same = g.ui.tipRect.left == g.tipLast.left && g.ui.tipRect.top == g.tipLast.top
                   && g.ui.tipRect.right == g.tipLast.right && g.ui.tipRect.bottom == g.tipLast.bottom;
    if (g.ui.tipText.empty()) { g.tipSince = 0; g.tipLast = RECT{}; }
    else if (!same) { g.tipLast = g.ui.tipRect; g.tipSince = GetTickCount(); }
    else if (g.tipSince && GetTickCount() - g.tipSince > 450) ui::draw_tip(g.ui, cr);

    g.ui.tipText.clear();
    g.ui.clicked = false;   // unconsumed clicks expire with the frame
}

[[maybe_unused]] LRESULT CALLBACK WndProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
    // Draft text belongs to the window, not to the worker's simulation. Keep
    // edits even during a long step; only applying them must wait for the step.
    if(m==WM_COMMAND&&g.ruleEdit&&reinterpret_cast<HWND>(lp)==g.ruleEdit&&HIWORD(wp)==EN_CHANGE) {
        wchar_t wide[2049]{};GetWindowTextW(g.ruleEdit,wide,2049);
        char text[8196]{};WideCharToMultiByte(CP_UTF8,0,wide,-1,text,sizeof text,nullptr,nullptr);
        g.ruleText=text;g.ruleMsg=bench::parse_rule(g.ruleText).error;
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    // Held at the door while a unit is in flight — see paintBusy(). This is the
    // complete list of what may happen then. Every other message this procedure
    // handles would read or change the sim: the app's own posted actions are
    // kept and run when the unit lands, and other input is dropped, a click
    // saying why. Messages it does not handle go to DefWindowProc as always and
    // never see the sim.
    if (workBusy()) {
        switch (m) {
        case WM_CLOSE: {
            requestPause();MSG held{};held.hwnd=hwnd;held.message=WM_CLOSE;g.heldMessages.push_back(held);return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            paintBusy(dc, cr);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            // Its release must not become a click once the unit lands: that
            // would fire whatever button was held, and toggle Pause straight back.
            g.suppressClick = true;
            if (ui::inside(g.runButton, p)) requestPause();
            else g.say("A step is in progress, so controls wait for it. Space or Pause stops after it.");
            return 0;
        }
        case WM_LBUTTONUP:
            g.ui.down = false; g.ui.clicked = false; g.suppressClick = false;
            g.panning = false; g.scrubbing = false; g.orbiting = false; g.splitDrag = 0;
            if (g.dragging) { g.dragEndPending = true; g.dragging = false; }
            std::fill(g.knobDrag.begin(), g.knobDrag.end(), false);
            ReleaseCapture();
            return 0;
        case WM_CHAR:      if (wp == ' ') requestPause(); return 0;
        case WM_KEYDOWN:   if (wp == VK_F11) toggleFullscreen(); return 0;
        case WM_MOUSEMOVE: g.ui.mouse = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; return 0;
        case WM_SETCURSOR: return DefWindowProcW(hwnd, m, wp, lp);
        case WM_DROPFILES:
            DragFinish(reinterpret_cast<HDROP>(wp));
            g.say("A step is in progress. Drop the file again once it lands.");
            return 0;
        // A pan or orbit begun before the unit ends with its release, exactly as
        // below. Dropped, the view would follow the mouse with no button held
        // once the unit lands.
        case WM_MBUTTONUP: g.orbiting = false; g.rightPan = false; ReleaseCapture(); return 0;
        case WM_RBUTTONUP: g.panning = false; g.orbiting = false; g.rightPan = false;
                           ReleaseCapture(); return 0;
        case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        case WM_MOUSEWHEEL:  case WM_COMMAND:
            return 0;
        default:
            // The app's own posted actions all touch the sim. They are kept, in
            // order, and posted again when the unit lands: an Enter typed into
            // a number box while a slow run steps is applied then, not lost.
            if (m >= WM_APP && m <= WM_APP + 16) {
                MSG held{}; held.hwnd = hwnd; held.message = m; held.wParam = wp; held.lParam = lp;
                g.heldMessages.push_back(held);
                return 0;
            }
            break;                                            // the rest never read the sim
        }
    }
    switch (m) {
    case WM_APP + 8: beginRuleEditor();return 0;
    case WM_APP + 9:
        if(wp) {applyRule();if(g.ruleMsg.empty())closeRuleEditor();}
        else closeRuleEditor(true);
        if(!g.ruleEdit)SetFocus(hwnd);
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_APP + 10:
        if(g.ruleEdit==reinterpret_cast<HWND>(wp)&&GetFocus()!=g.ruleEdit)closeRuleEditor();
        return 0;
    case WM_APP + 7: simulationFile(wp!=0);return 0;
    case WM_CLOSE: {
        if(!rememberCurrent()) {
            MessageBoxW(hwnd,L"Your simulation could not be saved. Check the status message, then use Save as to choose a writable location before closing.",L"Simulation was not saved",MB_OK|MB_ICONERROR);
            return 0;
        }
        RECT wr{};GetWindowRect(hwnd,&wr);std::string error;
        bench::projects::atomic_write(bench::path_from_utf8(cfgPath()),std::to_string(wr.right-wr.left)+" "+std::to_string(wr.bottom-wr.top)+"\n",error);
        DestroyWindow(hwnd);return 0;
    }
    case WM_APP + 6: movementFile(wp!=0);return 0;
    case WM_APP + 4: {
        if(!g.library || g.librarySearch)return 0;
        g.librarySearch=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",ui::widen(g.libraryQuery).c_str(),
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,32,108,390,34,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(g.librarySearch,WM_SETFONT,WPARAM(g.font),TRUE);
        SendMessageW(g.librarySearch,EM_LIMITTEXT,120,0);
        g.librarySearchProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.librarySearch,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(LibrarySearchProc)));
        SetFocus(g.librarySearch);return 0;
    }
    case WM_APP + 5:
        if(g.library) {const auto shown=libraryMatches();if(!shown.empty()){select(shown.front());g.running=false;}}
        return 0;
    case WM_COMMAND:
        if(reinterpret_cast<HWND>(lp)==g.librarySearch && HIWORD(wp)==EN_CHANGE) {
            char text[512]{};GetWindowTextA(g.librarySearch,text,512);g.libraryQuery=text;
            g.rosterScroll=0;InvalidateRect(hwnd,nullptr,FALSE);return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if(g.ruleEdit&&reinterpret_cast<HWND>(lp)==g.ruleEdit) {
            const HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,g.ui.t.ink);SetBkColor(dc,g.ui.t.bg);
            SetDCBrushColor(dc,g.ui.t.bg);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_APP + 1: {
        auto items=std::move(g.menuItems); g.menuItems.clear();
        HMENU menu=CreatePopupMenu();
        for(std::size_t i=0;i<items.size();++i) AppendMenuW(menu,MF_STRING | (items[i].checked?MF_CHECKED:0) | (items[i].enabled?0:MF_GRAYED),i+1,ui::widen(items[i].label).c_str());
        POINT pt=g.menuPoint; ClientToScreen(hwnd,&pt);
        g.ui.down=false;g.ui.clicked=false;ReleaseCapture();
        const int picked=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|TPM_TOPALIGN,pt.x,pt.y,hwnd,nullptr);
        DestroyMenu(menu);
        if(picked>0 && picked<=int(items.size()) && items[std::size_t(picked-1)].enabled) items[std::size_t(picked-1)].run();
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_APP + 2: {
        const auto key=g.numberKey; const auto rect=g.numberRect; cancelNumber();
        if(!g.sim) return 0;
        for(const auto& k:g.sim->knobs()) if(k.key==key) {
            auto shown=k;shown.value=controlValue(k);g.numberKey=key;
            g.numberEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",ui::widen(shown.shown()).c_str(),
                WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_CENTER,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!g.numberEdit) {g.numberKey.clear(); return 0;}
            SendMessageW(g.numberEdit,WM_SETFONT,WPARAM(g.font),TRUE);
            SendMessageW(g.numberEdit,EM_LIMITTEXT,32,0);
            g.numberProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.numberEdit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(NumberProc)));
            SetFocus(g.numberEdit);SendMessageW(g.numberEdit,EM_SETSEL,0,-1);
            break;
        }
        return 0;
    }
    case WM_APP + 3: {
        if(!g.numberEdit) return 0;
        if(wp && g.sim) {
            char buf[64]{};GetWindowTextA(g.numberEdit,buf,64);
            for(const auto& k:g.sim->knobs()) if(k.key==g.numberKey) {
                float value=0;
                if(!bench::parse_control_value(buf,k,value)) {g.say("Enter a number between " + std::to_string(k.min) + " and " + std::to_string(k.max));return 0;}
                const auto key=g.numberKey;setControl(key,value);break;
            }
        }
        cancelNumber();SetFocus(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    case WM_DESTROY: releaseDedicated(); PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = 1100; mm->ptMinTrackSize.y = 700;
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            // The splitter is a hairline. Without the cursor changing over it
            // there is nothing to tell you it can be dragged at all.
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            if (g.splitDrag || ui::inside(g.leftSplit, p) || ui::inside(g.rightSplit, p)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE;
            }
            if (g.ui.hot) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        layout(hwnd, mem);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteDC(mem);
        // Kept rather than deleted: the frame re-shown while a unit is in
        // flight. One client-sized bitmap, replaced every paint.
        if (g.frameCache) DeleteObject(g.frameCache);
        g.frameCache = bmp; g.frameW = int(cr.right); g.frameH = int(cr.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        cancelNumber();closeRuleEditor(); SetFocus(hwnd);
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g.ui.mouse = p; g.ui.down = true;g.ui.pressPoint=p;
        SetCapture(hwnd);
        // Splitters first. They overlap the edge of the panel behind them, and
        // the resize has to win there or the band is unusable on the inner half.
        if (ui::inside(g.leftSplit, p))  { g.splitDrag = -1; return 0; }
        if (ui::inside(g.rightSplit, p)) { g.splitDrag =  1; return 0; }
        for (std::size_t i = 0; i < g.rowRects.size(); ++i)
            if (ui::inside(g.rowRects[i], p)) {g.suppressClick=true;select(i);g.running=false;return 0;}
        if(g.library)return 0;
        if (g.mode == Mode::Run && ui::inside(g.timelineTrack, p)) {
            g.scrubbing = true;
            const auto d=g.hist.depth();
            if(d) {const float t=std::clamp(float(p.x-g.timelineTrack.left)/float(std::max(1L,g.timelineTrack.right-g.timelineTrack.left)),0.f,1.f);g.scrubAt=int(t*float(d-1));if(g.scrubAt>=int(d)-1)g.scrubAt=-1;}
            return 0;
        }
        if (g.ruleRect.right > g.ruleRect.left) {
            const bool hit = ui::inside(g.ruleRect, p) && ui::inside(g.notesRect,p);
            if (hit) {g.ui.down=false;ReleaseCapture();PostMessageW(hwnd,WM_APP+8,0,0);return 0;}
        } else g.ruleFocus = false;
        // Rule bits. Editing the rule does NOT reset — the whole point is
        // watching a running pattern react to the rule changing underneath it.
        if (g.sim)
            for (std::size_t i = 0; i < g.switchRects.size(); ++i)
                if (ui::inside(g.switchRects[i], p)) {
                    auto& sws = g.sim->switches();
                    if (i < sws.size()) {
                        const bool nv = !sws[i].value;
                        g.sim->on_switch(sws[i].key, nv);
                        // Say what it became, not just what was clicked: the
                        // title follows the rule, so this is where you find out
                        // you have stopped running Conway's.
                        g.say((nv ? "on: " : "off: ") + sws[i].group + " " + sws[i].label
                              + "   ->   " + g.sim->about().title);
                    }
                    return 0;
                }
        if (g.brush)
            for (std::size_t i = 0; i < g.palRects.size(); ++i)
                if (ui::inside(g.palRects[i], p)) {
                    g.paintIdx = int(i);
                    if (g.sim && i < g.sim->palette().size())
                        g.say("painting " + g.sim->palette()[i].label);
                    return 0;
                }
        if (ui::inside(g.canvasRect, p)) {
            if(g.placeAction && g.sim) {
                float nx=0.5f,ny=0.5f;
                if(g.sim->surface()) { if(!surfacePoint(p,nx,ny)) return 0; }
                else {
                    const auto& f=g.sim->field();
                    const auto cell=g.raster.cell_at(float(p.x-g.canvasRect.left),float(p.y-g.canvasRect.top),f.w,f.h,g.view);
                    if(cell.first<0)return 0;
                    nx=float(cell.first)/float(f.w);ny=float(cell.second)/float(f.h);
                }
                if(g.sim->poke(nx,ny)){g.scrubAt=-1;g.say(currentWorkflow().action);}
                else g.say("This position cannot accept that action.");
                InvalidateRect(hwnd,nullptr,FALSE);return 0;
            }
            // A sim that renders a SPACE gets flown, not panned. Left-drag
            // orbits, shift-drag pans the pivot, and a plain click sets the
            // pivot to whatever solid thing is under the cursor — so you orbit
            // the part you are looking at rather than always the middle.
            if (g.sim && g.sim->surface() && !g.sim->surface()->empty()) {
                // Ask the sim whether it grabbed something before assuming the
                // drag belongs to the camera. A network diagram has nodes to
                // take hold of; a voxel volume has nothing and says so.
                float nx, ny;
                if (surfacePoint(p, nx, ny) && g.sim->drag_begin(nx, ny)) {
                    g.dragging = true; g.lastPt = p;
                    SetCapture(hwnd);
                    return 0;
                }
                if (!g.sim->has_camera()) return 0;   // nothing to grab, no camera
                g.orbiting = true; g.lastPt = p;
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    float nx, ny;
                    if (surfacePoint(p, nx, ny) && g.sim->camera_pick(nx, ny))
                        g.say("pivot set — orbiting that voxel");
                    else g.say("nothing solid under the cursor; pivot unchanged");
                    g.orbiting = false;
                }
                return 0;
            }
            if (g.brush && g.sim) { g.lastPt = p; paintAt(p, (GetKeyState(VK_SHIFT) & 0x8000) != 0); }
            else { g.panning = true; g.lastPt = p; }
        }
        return 0;
    }

    // Middle button is the CAD convention for navigating: drag to orbit,
    // shift or ctrl to pan. Left stays free for editing, which is why CAD put
    // navigation on the middle button in the first place.
    case WM_MBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.sim && ui::inside(g.canvasRect, p) &&
            g.sim->surface() && !g.sim->surface()->empty()) {
            g.orbiting = true;
            g.rightPan = (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_CONTROL) & 0x8000);
            g.lastPt = p;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MBUTTONUP:
        g.orbiting = false; g.rightPan = false; ReleaseCapture(); return 0;

    case WM_RBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (ui::inside(g.canvasRect, p)) {
            if (g.sim && g.sim->has_camera()) {
                g.orbiting = true; g.rightPan = true; g.lastPt = p; SetCapture(hwnd);
                return 0;
            }
            g.panning = true; g.lastPt = p; SetCapture(hwnd);
        }
        return 0;
    }
    case WM_RBUTTONUP: g.panning = false; g.orbiting = false; g.rightPan = false;
                       ReleaseCapture(); return 0;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g.ui.mouse = p;
        if (g.splitDrag) {
            RECT cr; GetClientRect(hwnd, &cr);
            // A minimum, not zero: a panel dragged to nothing is a panel you
            // cannot get back with the mouse. Tab is how you make it vanish.
            const int lo = int(140 * gScale), hi = std::max(lo, int(cr.right) / 2);
            if (g.splitDrag < 0) g.leftW  = std::clamp(int(p.x), lo, hi);
            else                 g.rightW = std::clamp(int(cr.right) - int(p.x), lo, hi);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Repaint on plain movement as well, so the splitter under the cursor
        // lights up. Cheap: the whole window is redrawn each frame anyway.
        if (ui::inside(g.leftSplit, p) || ui::inside(g.rightSplit, p))
            InvalidateRect(hwnd, nullptr, FALSE);
        if (g.scrubbing) {
            const int x0 = g.timelineTrack.left, x1 = g.timelineTrack.right;
            const std::size_t d = g.hist.depth();
            if (d) {
                const float t = std::clamp(float(p.x - x0) / float(std::max(1,x1 - x0)), 0.f, 1.f);
                g.scrubAt = int(t * float(d - 1));
                if (g.scrubAt >= int(d) - 1) g.scrubAt = -1;
            }
        } else if (g.dragging && g.sim) {
            float nx, ny;
            if (surfacePoint(p, nx, ny)) g.sim->drag_move(nx, ny);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g.orbiting && g.sim) {
            const float dx = float(p.x - g.lastPt.x), dy = float(p.y - g.lastPt.y);
            g.lastPt = p;
            if (g.rightPan || (GetKeyState(VK_SHIFT) & 0x8000)) g.sim->camera_pan(dx, dy);
            else                                g.sim->camera_orbit(dx, dy);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g.panning) {
            g.view.pan_x += float(p.x - g.lastPt.x);
            g.view.pan_y += float(p.y - g.lastPt.y);
            g.lastPt = p;
        } else if (g.brush && g.ui.down && g.sim && ui::inside(g.canvasRect, p)) {
            // Interpolate along the drag. Sampling only at WM_MOUSEMOVE points
            // leaves gaps in a fast stroke — the brush looks like it stutters.
            const int steps = std::max(1, int(std::max(std::abs(p.x - g.lastPt.x),
                                                       std::abs(p.y - g.lastPt.y))) / 2);
            const bool erase = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            for (int i = 1; i <= steps; ++i) {
                POINT q{ g.lastPt.x + (p.x - g.lastPt.x) * i / steps,
                         g.lastPt.y + (p.y - g.lastPt.y) * i / steps };
                paintAt(q, erase);
            }
        }
        if (g.brush && g.ui.down) g.lastPt = p;
        return 0;
    }

    case WM_LBUTTONUP:
        g.ui.mouse = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g.ui.down = false; g.ui.clicked = !g.suppressClick;g.suppressClick=false;
        g.panning = false; g.scrubbing = false; g.orbiting = false; g.splitDrag = 0;
        if (g.dragging && g.sim) g.sim->drag_end();
        g.dragging = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL: {
        cancelNumber();closeRuleEditor();
        const int d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT at{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&at);
        if(ui::inside(g.rosterRect,at)) {
            const int maxScroll=std::max(0,g.rosterContentH-int(g.rosterRect.bottom-g.rosterRect.top));
            g.rosterScroll=std::clamp(g.rosterScroll-(d>0?96:-96),0,maxScroll);
            InvalidateRect(hwnd,nullptr,FALSE);return 0;
        }
        if (g.mode == Mode::Code && !ui::inside(g.notesRect,at)) {
            g.codeTop = std::clamp(g.codeTop - (d > 0 ? 3 : -3), 0,
                                   std::max(0, int(g.code.size()) - 5));
            return 0;
        }
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &p);
        if (ui::inside(g.notesRect, p)) {
            const int viewH = (g.notesRect.bottom - g.notesRect.top);
            const int maxScroll = std::max(0, g.notesContentH - viewH);
            g.notesScroll = std::clamp(g.notesScroll - (d > 0 ? 60 : -60), 0, maxScroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.sim && ui::inside(g.canvasRect, p) && g.sim->has_camera()) {
            float nx = 0.5f, ny = 0.5f;
            surfacePoint(p, nx, ny);          // zoom toward the pointer, CAD-style
            g.sim->camera_dolly(d > 0 ? 1.0f : -1.0f, nx, ny);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.sim && ui::inside(g.canvasRect, p)) {
            const auto& f = g.sim->field();
            const float cx = float(p.x - g.canvasRect.left), cy = float(p.y - g.canvasRect.top);
            auto f0 = g.raster.fit_of(f.w, f.h, g.view); auto b4 = g.raster.to_cell(cx, cy, f0);
            g.view.zoom = std::clamp(g.view.zoom * (d > 0 ? 1.15f : 1.f/1.15f), 1.f, 60.f);
            auto f1 = g.raster.fit_of(f.w, f.h, g.view); auto af = g.raster.to_cell(cx, cy, f1);
            g.view.pan_x += (af.first  - b4.first)  * f1.scale;
            g.view.pan_y += (af.second - b4.second) * f1.scale;
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP h = (HDROP)wp;
        wchar_t wide[MAX_PATH]{};
        if (DragQueryFileW(h, 0, wide, MAX_PATH)) {
            char narrow[MAX_PATH * 2]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof narrow, nullptr, nullptr);
            loadPatternFile(narrow);
        }
        DragFinish(h);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if(GetKeyState(VK_CONTROL)<0&&(wp=='S'||wp=='O')) {PostMessageW(hwnd,WM_APP+7,wp=='S',0);return 0;}
        if(wp==VK_F4) {if(g.library&&g.sim)closeLibrary();else openLibrary();return 0;}
        if(g.library) {if(wp==VK_ESCAPE&&g.sim)closeLibrary();return 0;}
        if(g.ruleFocus)return 0;
        // Driving comes BEFORE the hotkeys, but only claims a key when a body
        // is actually possessed — see possessKey(). Putting it after the switch
        // would have been the same code with none of the letters reaching it.
        if(auto* sim=movement();sim&&sim->editing()&&wp=='Z'&&(GetKeyState(VK_CONTROL)&0x8000)){sim->undo();InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        if (possessKey(unsigned(wp))) return 0;
        switch (wp) {
        case VK_F1: g.mode = Mode::Run; break;
        case VK_F2: g.mode = Mode::Code; break;
        // ── giving the simulation the room ──────────────────────────────────
        // Tab folds the side panels away and back. Shift+Tab does one side, so
        // you can keep the metrics up while the roster goes, which is the case
        // that actually comes up: you are watching a plot, not picking a sim.
        case VK_TAB:
            cancelNumber();g.showRight=!g.showRight;
            g.say(g.showRight?"Inspector shown":"Inspector hidden — Tab restores it");
            break;
        case VK_F3:
            g.focus = !g.focus;
            g.say(g.focus ? "focus: the whole window is the simulation  ·  F3 back"
                          : "focus off");
            break;
        case VK_F11: toggleFullscreen(); break;
        case VK_F5: reloadPlugin(); break;
        case VK_F6: buildPlugin(); break;
        case VK_F7: savePattern(); break;
        case VK_F9: saveMetrics(); break;
        case VK_F8: identifyPattern(); break;
        case 'H': if (g.sim) { g.sim->camera_home(); g.say("camera recentred on the origin"); } break;
        case VK_LEFT:  if (g.hist.depth()) { if (g.scrubAt < 0) g.scrubAt = int(g.hist.depth())-1;
                                             g.scrubAt = std::max(0, g.scrubAt-1); } break;
        case VK_RIGHT: if (g.scrubAt >= 0) { ++g.scrubAt;
                         if (g.scrubAt >= int(g.hist.depth())-1) g.scrubAt = -1; } break;
        case VK_PRIOR: g.codeTop = std::max(0, g.codeTop - 25); break;
        case VK_NEXT:  g.codeTop = std::min(std::max(0,int(g.code.size())-5), g.codeTop + 25); break;
        default: break;
        }
        return 0;

    case WM_CHAR:
        if(g.library)return 0;
        if(g.ruleFocus)return 0;
        if(auto* sim=movement();sim&&sim->editing()) {
            if(wp>='1'&&wp<='5'){sim->setTool(bench::Locomotion::Tool(wp-'1'));InvalidateRect(hwnd,nullptr,FALSE);return 0;}
            if(wp==' '){editMovement();g.running=!sim->editing();return 0;}
        }
        // Standard views on the digits, the numpad convention every CAD tool
        // and Blender shares: 1 front, 3 right, 7 top, 0 isometric, 5 toggles
        // parallel projection.
        if (g.sim && g.sim->has_camera()) {
            using V = bench::Sim::StdView;
            bool handled = true;
            switch (wp) {
            case '1': g.sim->camera_view(V::Front);  g.say("front view");  break;
            case '2': g.sim->camera_view(V::Back);   g.say("back view");   break;
            case '3': g.sim->camera_view(V::Right);  g.say("right view");  break;
            case '4': g.sim->camera_view(V::Left);   g.say("left view");   break;
            case '7': g.sim->camera_view(V::Top);    g.say("top view");    break;
            case '8': g.sim->camera_view(V::Bottom); g.say("bottom view"); break;
            case '0': g.sim->camera_view(V::Iso);    g.say("isometric");   break;
            case '5': {
                const bool now = !g.sim->camera_is_ortho();
                g.sim->camera_ortho(now);
                g.say(now ? "parallel projection — equal features measure equal"
                          : "perspective projection");
                break;
            }
            case 'f': case 'F': g.sim->camera_fit(); g.say("zoomed to fit"); break;
            default: handled = false;
            }
            if (handled) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        }
        switch (wp) {
        case ' ': if(g.training){g.training=false;g.trainTo=0;g.running=false;}else {replayFinishedSort();g.running = !g.running;} break;
        case '.': if (g.sim) { g.sim->step(); g.hist.observe(*g.sim); g.scrubAt = -1; } break;
        case ',': if (g.hist.depth()) { if (g.scrubAt < 0) g.scrubAt = int(g.hist.depth())-1;
                                        g.scrubAt = std::max(0, g.scrubAt-1); } break;
        case 'r': case 'R': resetRun(); break;
        case 'b': case 'B': if(g.sim&&g.sim->editable()){g.brush = !g.brush;g.placeAction=false;} break;
        case VK_OEM_4: g.brushSize = std::max(0,  g.brushSize - 1); break;   // [
        case VK_OEM_6: g.brushSize = std::min(64, g.brushSize + 1); break;   // ]
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

} // namespace

// Write one line naming everything needed to reproduce. The app is built
// -mwindows, so it has no console: without this, every bad_alloc and every
// access violation is a window that simply vanishes, and one crash looks
// exactly like every other. That is why the max-speed crash had to be
// diagnosed by reproducing it rather than by reading anything.
void writeCrashLine(const char* what) {
    std::ofstream out(bench::Paths::get().exeDir() + "/workbench-crash.txt", std::ios::app);
    if (!out) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const bool isPlugin = g.sel < g.rows.size() && g.rows[g.sel].plugin;
    out << stamp << "  " << what
        << "  sim="      << (g.sel < g.rows.size() ? g.rows[g.sel].label : "?")
        << "  plugin="   << (isPlugin ? "yes" : "no")
        << "  sps="      << g.sps
        << "  allowed="  << g.lastAllowed
        << "  ms/step="  << g.msPerStep
        << "  snaps="    << g.hist.depth()
        << "  histMB="   << (g.hist.bytes() / (1024*1024))
        << "  field="    << (g.sim ? g.sim->field().cells.size() : 0)
        << "  freeMB="   << (g.hw.availRam / (1024*1024))
        << std::endl;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    char b[64];
    std::snprintf(b, sizeof b, "FATAL code=0x%08lX",
                  ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
    writeCrashLine(b);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ── one frame's worth of simulation ─────────────────────────────────────────
//
// Shared by the window's loop and the UI tests, so the path the tests drive is
// the path the app runs. It used to sit inline in wWinMain, compiled into the
// app alone, where no test could reach it.
//
// A unit MEASURED as slow runs on the worker instead of here — a step past
// kOffThreadMs, or an epoch past it, where an epoch nobody has timed counts as
// slow — and while it is in flight this returns at once and the window stays
// alive; paintBusy() says what that requires of everything else. Anything cheap
// takes exactly the path it always took, batching included: a handoff per
// cheap step would cost more than the step, and would halve fast training by
// spreading each epoch across two turns of the loop.

constexpr double kOffThreadMs = 50.0;   // three frames: past this a unit visibly stalls the window

void startOffThread(App::OffUnit unit) {
    if (!g.work) g.work = std::make_unique<bench::SimWorker>();
    g.offUnit = unit;
    g.offStarted = GetTickCount();
    g.offEpochBefore = g.sim->epoch_count();
    bench::Sim* sim = g.sim.get();
    g.work->start([sim, unit] {
        const auto t0 = std::chrono::steady_clock::now();
        if (unit == App::OffUnit::Epoch) sim->advance_epoch();
        else sim->step();
        g.offMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    });
}

// Everything that follows an epoch, wherever it ran.
void afterEpoch(int before, bool movementProgress) {
    g.hist.observe(*g.sim);
    // An epoch that does not complete is not an epoch. voxelcraft's
    // runs until the agent gains a tech-tree rung and gives up after
    // 60000 steps, so an agent that cannot reach the next rung would
    // spin here forever — and the plateau counter, fed once per frame,
    // would report "no improvement for 15 tree rungs" when not one rung
    // had passed. Stop, and say the true reason.
    const bool advanced = g.sim->epoch_count() != before;
    if (!advanced && !movementProgress) {
        g.training = false; g.trainTo = 0; g.plateau.reset();
        g.say(std::string("stopped: a ") + g.sim->epoch_name() +
              " did not complete — this run cannot get any further");
    }
    // Improvement is judged on the FIRST metric the sim reports, which
    // is the one it puts first for a reason. Judging on all of them
    // would mean a run never counts as stalled as long as any single
    // series still wobbles upward — including ones like "species" that
    // drift on their own and say nothing about progress.
    if (g.stopStale && advanced) {
        const auto& en = g.hist.names(bench::History::Axis::Epoch);
        // The first series that actually has a better direction. A
        // leading metric like "episodes" only counts upward and would
        // register as improvement forever.
        std::size_t pick = en.size();
        for (std::size_t i = 0; i < en.size(); ++i)
            if (g.hist.direction(en[i], bench::History::Axis::Epoch) != bench::Metric::Neither)
                { pick = i; break; }
        if (pick < en.size() &&
            g.plateau.observe(g.hist.best_of(en[pick], bench::History::Axis::Epoch),
                              App::kStaleLimit)) {
            g.training = false; g.trainTo = 0;
            char m[160];
            std::snprintf(m, sizeof m,
                "stopped: %s has not improved on \"%s\" for %d %ss (best %.4g)",
                g.sim->epoch_name(), en[pick].c_str(), App::kStaleLimit,
                g.sim->epoch_name(), g.plateau.best());
            g.say(m);
            g.plateau.reset();
        }
    }
    if (g.trainTo > 0 && g.sim->epoch_count() >= g.trainTo) {
        g.training = false;
        g.say(std::string("stopped at ") + std::to_string(g.sim->epoch_count()) +
              " " + g.sim->epoch_name() + "s");
        g.trainTo = 0;
    }
}

void stepFrame(double dt) {
    // Stepping and observing are where allocation happens, so this is the
    // block that can fail. Recover rather than die: drop the history, stop
    // the clock, and leave a line saying what the state was. A frozen sim
    // you can inspect beats a window that disappeared.
    try {
        if (workBusy()) {
            // Time passes while a step is in flight, and the rate asked for is
            // per second of it. Without this every slow step would also wait
            // out a fresh interval after it landed.
            if (g.offUnit == App::OffUnit::Step) g.acc = std::min(1.0, g.acc + dt);
            if (!g.work->ready()) return;
            const App::OffUnit unit = g.offUnit;
            g.offUnit = App::OffUnit::None;
            // Actions held at the door go back in the queue first, so nothing
            // below can lose them, and the check further down keeps the next
            // unit from going out until they have run.
            for (const MSG& held : g.heldMessages) PostMessageW(held.hwnd, held.message, held.wParam, held.lParam);
            g.heldMessages.clear();
            g.work->collect();                  // rethrows what the unit threw, into the handlers below
            if (g.dragEndPending) { g.dragEndPending = false; if (g.sim) g.sim->drag_end(); }
            if (unit == App::OffUnit::Epoch) {
                g.msPerEpoch = g.msPerEpoch * 0.7 + g.offMs * 0.3;
                afterEpoch(g.offEpochBefore, false);
            } else {
                g.acc = std::max(0.0, g.acc - 1.0 / double(g.sps));
                if (!g.hw.memoryTight()) g.hist.observe(*g.sim);
                else g.hist.shrink_to(g.hw.historyBudget / 4);
                g.msPerStep = g.msPerStep * 0.7 + g.offMs * 0.3;
                stopFinishedSort();
            }
            if (g.pauseAfter) {
                g.pauseAfter = false;
                g.running = false; g.training = false; g.trainTo = 0;
                g.say("Paused.");
            }
            return;          // the window paints the landed unit before the next one goes out
        }
        // The window's own posted actions get a turn before the sim is handed
        // away: a typed number, a picked menu item — each a message already
        // queued, which would otherwise arrive to find the sim busy and wait.
        // So does a click still going through. Pressed and then released
        // mid-unit it would be lost; released but not yet laid out it would
        // fire a whole unit late. Holding the button therefore holds a slow
        // run, which is what painting into it or dragging a control needs.
        MSG queued{};
        const bool pending = PeekMessageW(&queued, g.hwnd, WM_APP, WM_APP + 16, PM_NOREMOVE) != 0 ||
                             g.ui.down || g.ui.clicked;

        // Training takes precedence over free-running: an epoch is the larger
        // unit, and stepping frames underneath it would advance the sim twice
        // per frame on two different clocks.
        if (!g.library && g.training && g.sim && g.sim->epoch_name() && g.scrubAt < 0) {
            if (auto* sim = movement()) {
                // The movement lab slices its own generations into 12 ms pieces,
                // so it never needed the worker.
                const int before = g.sim->epoch_count();
                const bool progress = sim->advance_slice();
                afterEpoch(before, progress);
            } else if (g.msPerEpoch > kOffThreadMs) {
                if (!pending) startOffThread(App::OffUnit::Epoch);
            } else {
                const int before = g.sim->epoch_count();
                const auto t0 = std::chrono::steady_clock::now();
                g.sim->advance_epoch();
                g.msPerEpoch = g.msPerEpoch * 0.7 +
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() * 0.3;
                afterEpoch(before, false);
            }
        } else if (g.training && (!g.sim || !g.sim->epoch_name())) {
            g.training = false;          // the sim changed underneath us
        } else if (!g.library && g.running && g.sim && g.scrubAt < 0) {
            g.acc += dt;
            if (g.msPerStep > kOffThreadMs) {
                // Already longer than three frames: batching buys nothing, and
                // stepping here would freeze the window for the whole step.
                if (!pending && g.acc >= 1.0 / double(g.sps)) startOffThread(App::OffUnit::Step);
                if (g.acc > 1.0) g.acc = 1.0;
                return;
            }
            const double budget = 1.0 / double(g.sps);
            const double want   = g.acc / budget;
            const int allowed   = g.guard.allow(want, g.msPerStep);
            g.throttled         = g.guard.throttled(want, allowed);
            g.lastAllowed       = allowed;

            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            int done = 0;
            while(done<allowed) {
                g.sim->step();g.acc-=budget;++done;
                if(stopFinishedSort())break;
                QueryPerformanceCounter(&t1);
                // Recheck actual elapsed time: an old cheap-step estimate must
                // not lock the window after a large world or expensive rule change.
                if(1000.0*double(t1.QuadPart-t0.QuadPart)/double(f.QuadPart)>=g.hw.stepBudgetMs)break;
            }
            // Once per FRAME, not once per step. Observing per step at 2000
            // steps/frame allocated and evicted a snapshot ~500 times a frame.
            if (done) {
                if (!g.hw.memoryTight()) g.hist.observe(*g.sim);
                // Under pressure, give memory back rather than merely stopping
                // taking more — the ring was sized when the machine was idle.
                else g.hist.shrink_to(g.hw.historyBudget / 4);
            }
            QueryPerformanceCounter(&t1);
            if (done) {
                const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
                // smooth, so one slow frame does not whipsaw the limiter
                g.msPerStep = g.msPerStep * 0.7 + (ms / double(done)) * 0.3;
            }
            if (g.acc > 1.0) g.acc = 1.0;   // never build an unpayable debt
        }
    } catch (const std::bad_alloc&) {
        g.offUnit = App::OffUnit::None; g.pauseAfter = false; g.dragEndPending = false;
        writeCrashLine("bad_alloc while stepping");
        g.hist.clear(); g.raster.clear_accumulator();
        g.running = false; g.training=false;g.trainTo=0;g.sps = 30; g.acc = 0;
        g.say("out of memory while stepping — history dropped, paused. "
              "See workbench-crash.txt");
    } catch (const std::exception& e) {
        g.offUnit = App::OffUnit::None; g.pauseAfter = false; g.dragEndPending = false;
        writeCrashLine(e.what());
        g.running = false;g.training=false;g.trainTo=0;g.acc = 0;
        g.say(std::string("step failed: ") + e.what() + " — paused");
    }
}

#ifdef BENCH_UI_SHOT
// ── headless screenshot ─────────────────────────────────────────────────────
//
// The app's layout had never actually been LOOKED at from this side — only
// confirmed alive as a process. That makes any UI work guesswork. layout()
// only needs an HWND for its client rect and an HDC to draw into, so a hidden
// window plus a memory DIB renders the real interface, with the real widgets
// and the real sim, straight to a PNG.
//
//   ui_shot.exe [simId] [steps] [w] [h] [out.png] [mouseX] [mouseY]
//
// The mouse position is optional and exists so hover states — tooltips above
// all — can actually be looked at instead of taken on trust.
//
// ── and then the shot has to be looked at too ──────────────────────────────
//
// This wrote a PNG, printed the canvas geometry, and exited 0 whatever was in
// the image. An all-black frame — a sim that failed to construct, a layout that
// collapsed, a paint that never ran — produced exactly the same output as a
// good one, so "I took a screenshot and the layout is fine" could be said
// without anything having looked. The written image is now measured the same
// way the contact sheet measures its tiles, and a frame that is one flat colour
// fails.
#include "tool_freshness.hpp"
#include <map>

// Fraction of the image that differs from its most common colour, and how many
// distinct colours it has. Measured against the MODE rather than against black:
// the workbench's own chrome is a dark grey, and counting non-black pixels
// calls an empty window full.
static void measureShot(const std::vector<std::uint8_t>& rgba, int w, int h,
                        double& ink, std::size_t& colours) {
    std::map<std::uint32_t, std::size_t> hist;
    const std::size_t n = std::size_t(w) * std::size_t(h);
    for (std::size_t i = 0; i < n; ++i)
        ++hist[std::uint32_t(rgba[i*4+0]) | (std::uint32_t(rgba[i*4+1]) << 8)
             | (std::uint32_t(rgba[i*4+2]) << 16)];
    std::size_t mode = 0;
    for (const auto& e : hist) mode = std::max(mode, e.second);
    ink = n ? 1.0 - double(mode) / double(n) : 0.0;
    colours = hist.size();
}

#include "ui_verification.hpp"
#include "project_ui_verification.hpp"

int main(int argc, char** argv) {
    bench::gate("workbench");
    const std::string verification=argc>1?argv[1]:"";
    const bool verify=verification=="--verify-ui"||verification=="--verify-library"||verification=="--verify-saved-write"||verification=="--verify-saved-read";
    const std::string simId = verify ? "life" : (argc > 1 ? argv[1] : "life");
    const int steps = argc > 2 ? std::atoi(argv[2]) : 300;
    int W     = argc > 3 ? std::atoi(argv[3]) : 1640;
    int H     = argc > 4 ? std::atoi(argv[4]) : 1000;
    const std::string out = argc > 5 ? argv[5] : "ui.png";

    WNDCLASSW wc{};
    wc.lpfnWndProc = verify ? WndProc : DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LifeSimShot";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"shot", WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                             0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) { std::printf("no window\n"); return 1; }
    // Client area, not window area, so the shot matches what layout() gets.
    RECT wr{0,0,W,H}; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g.hwnd, nullptr, 0, 0, wr.right-wr.left, wr.bottom-wr.top, SWP_NOMOVE|SWP_NOZORDER);
    // ...and then believe the window rather than the request. Windows clamps a
    // window to the desktop, so asking for a 1920x1080 client on a 1920x1080
    // screen gets you 1711x910 and the tool was rendering that into a
    // 1920x1080 bitmap: every shot at that size carried 209 dead pixels down
    // the right edge and 170 along the bottom, and any "percent of the window"
    // computed from W and H was measured against a window that did not exist.
    {
        RECT cr{}; GetClientRect(g.hwnd, &cr);
        if (cr.right > 0 && cr.bottom > 0 && (int(cr.right) != W || int(cr.bottom) != H)) {
            std::printf("   note: asked for %dx%d, the desktop allows %dx%d — using that\n",
                        W, H, int(cr.right), int(cr.bottom));
            W = int(cr.right); H = int(cr.bottom);
        }
    }

    applyScale(1.0f);        // shots are taken at 1x so sizes are comparable
    g.font = CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g.bold = CreateFontW(-20,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g.ui.font = g.font; g.ui.bold = g.bold;
    g.view.trail = 0.85f;
    g.hw = bench::Hardware::probe();
    g.guard.configure(g.hw.stepBudgetMs, g.hw.maxStepsPerFrame);
    rebuildRows();

    if(verify){
        // A throw no check anticipated ends the run as a reported failure, not
        // as a process that vanishes mid-list with no summary.
        try{return verification=="--verify-ui"?verifyUi():verifySavedProjects(verification,argc>2?std::atoi(argv[2]):0);}
        catch(const std::exception& e){std::printf("FAIL the UI checks stopped early on an uncaught exception: %s\n",e.what());}
        catch(...){std::printf("FAIL the UI checks stopped early on an uncaught non-standard exception\n");}
        return 1;
    }

    // An unknown id used to fall through to pick = 0 and render registry[0]
    // while PRINTING the name that was asked for. Every screenshot taken from a
    // typo was a confident picture of the wrong sim, byte-identical to the
    // default and labelled as something else — the worst kind of wrong output,
    // because nothing about it looks wrong.
    std::size_t pick = 0;
    bool found = false;
    for (std::size_t i = 0; i < g.rows.size(); ++i)
        if (g.rows[i].id == simId) { pick = i; found = true; }
    if (!found&&simId!="library") {
        std::printf("ui_shot: no sim called \"%s\". Available:\n", simId.c_str());
        for (const auto& r : g.rows)
            if (!r.id.empty()) std::printf("  %s\n", r.id.c_str());
        return 2;
    }
    if(simId=="library")openLibrary();else select(pick);
    // Trailing key=value arguments set knobs before the run, so a shot can show
    // a configuration rather than only the defaults. Applied after select(),
    // which is what builds the sim, and before the steps below.
    for (int i = 9; i < argc; ++i) {
        const std::string kv = argv[i];
        const auto eq = kv.find('=');
        if (eq == std::string::npos || !g.sim) continue;
        const std::string key = kv.substr(0, eq);
        const float val = float(std::atof(kv.c_str() + eq + 1));
        for (auto& kn : g.sim->knobs()) if (kn.key == key) kn.value = val;
        g.sim->on_knob(key, val);
        g.sim->reset();
    }
    // BENCH_SHOT_KEEP=<n>: run n epochs, freeze them as the baseline, reset,
    // then run whatever was asked for. Without it the comparison overlay
    // cannot be looked at headlessly at all — it takes two runs and a button
    // press to exist.
    if (const char* keep = std::getenv("BENCH_SHOT_KEEP")) {
        const int n = std::atoi(keep);
        for (int i = 0; i < n && g.sim; ++i) { g.sim->advance_epoch(); g.hist.observe(*g.sim); }
        g.hist.keep_baseline();
        if (g.sim) g.sim->reset();
        g.hist.clear();
    }

    // BENCH_SHOT_SCROLL=<px>: the notes panel scrolls, so the lower half of a
    // long panel cannot be photographed without a way to scroll it.
    if (const char* sc = std::getenv("BENCH_SHOT_SCROLL")) g.notesScroll = std::atoi(sc);
    if (const char* tab = std::getenv("BENCH_SHOT_TAB")) g.inspector = std::clamp(std::atoi(tab),0,2);
    if (std::getenv("BENCH_SHOT_ADVANCED")) g.advanced = true;

    // A negative count means EPOCHS, not frames. Without this the epoch plots
    // cannot be looked at headlessly at all: eighty genomes playing to death is
    // thousands of frames, so any frame count small enough to run in a test is
    // too small to complete a single generation.
    // BENCH_SHOT_DRIVE=<keys>: press these, one every BENCH_SHOT_DRIVE_EVERY
    // frames, through the SAME possessKey() the window calls. Without it the
    // possession key map cannot be exercised headlessly at all, and "the keys
    // are wired" would be a claim about code that nothing had ever run — which
    // is the exact shape of the inert control this project keeps catching. It
    // also makes the readout photographable mid-run instead of only at rest.
    const char* drive = std::getenv("BENCH_SHOT_DRIVE");
    const int driveEvery = std::getenv("BENCH_SHOT_DRIVE_EVERY")
                         ? std::max(1, std::atoi(std::getenv("BENCH_SHOT_DRIVE_EVERY"))) : 10;
    std::size_t driveAt = 0;
    const std::string driveKeys = drive ? drive : "";

    if (steps < 0) {
        for (int i = 0; i < -steps && g.sim; ++i) { g.sim->advance_epoch(); g.hist.observe(*g.sim); }
    } else
    for (int i = 0; i < steps && g.sim; ++i) {
        if (driveAt < driveKeys.size() && i % driveEvery == 0)
            possessKey(unsigned(std::toupper(static_cast<unsigned char>(driveKeys[driveAt++]))));
        g.sim->step(); if (i % 8 == 0) g.hist.observe(*g.sim);
    }

    if(auto* sim=movement()) {
        if(std::getenv("BENCH_SHOT_BODY"))editMovement();
        else if(std::getenv("BENCH_SHOT_REPLAY")){sim->watchChampion();for(int i=0;i<90;++i)sim->step();}
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);

    const bool hovering = argc > 7;
    g.ui.mouse = hovering ? POINT{ std::atoi(argv[6]), std::atoi(argv[7]) } : POINT{-1,-1};
    // Optional layout mode, so the panel states can be photographed rather
    // than reasoned about: "panels" (the default), "nopanels" (what Tab does),
    // "focus" (what F3 does).
    if (argc > 8) {
        const std::string mode = argv[8];
        if (mode == "nopanels") { g.showLeft = g.showRight = false; }
        else if (mode == "library") {openLibrary();}
        else if (mode == "focus") { g.focus = true; }
        else if (mode == "wide")  { g.leftW = int(150 * gScale); g.rightW = int(150 * gScale); }
    }
    // Twice. The tool bar only discovers it needs a second row while drawing,
    // and the pane is sized from that — one pass would photograph the layout
    // mid-settle and the sliders would be cut in half.
    layout(g.hwnd, mem);
    layout(g.hwnd, mem);
    if (hovering) {
        // Tooltips only appear after the cursor has settled. The first layout
        // registers which widget is under the cursor; backdating the dwell
        // clock and laying out again produces the settled frame.
        g.tipSince = GetTickCount() - 5000;
        layout(g.hwnd, mem);
    }
    GdiFlush();

    // GDI gives BGRA; the PNG writer wants RGBA.
    std::vector<std::uint8_t> rgba(std::size_t(W) * H * 4);
    const std::uint8_t* src = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
        rgba[i*4+0] = src[i*4+2]; rgba[i*4+1] = src[i*4+1];
        rgba[i*4+2] = src[i*4+0]; rgba[i*4+3] = 255;
    }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    if (!bench::write_png(out.c_str(), W, H, rgba.data())) { std::printf("write failed\n"); return 1; }
    // Report the space the SIMULATION got, not the space the canvas got. They
    // are not the same number and the difference is the whole point: the field
    // is fitted by min(w/fw, h/fh), so a square world in a wide window is
    // limited by the window's HEIGHT, and widening the canvas buys it nothing
    // but wider black bars. Anything claiming to have made the simulation
    // bigger has to be measured here.
    {
        const RECT& a = g.canvasRect;
        const int cw = int(a.right - a.left), ch = int(a.bottom - a.top);
        int fw = 1, fh = 1;
        if (g.sim) { fw = g.sim->field().w; fh = g.sim->field().h; }
        const float s = std::min(float(cw) / float(fw), float(ch) / float(fh)) * g.view.zoom;
        std::printf("wrote %s (%dx%d) — %s after %d steps\n", out.c_str(), W, H, simId.c_str(), steps);
        std::printf("   canvas %dx%d = %d px   sim %dx%d = %d px (%.0f%% of the window)\n",
                    cw, ch, cw * ch, int(fw * s), int(fh * s), int(fw * s) * int(fh * s),
                    100.0 * double(int(fw * s)) * double(int(fh * s)) / (double(W) * double(H)));

        // What is actually in the file. One flat colour is a failed shot, and
        // until now it was an indistinguishable one. The threshold is stated
        // rather than tuned: a real workbench frame carries the index, the
        // knob panel, the plots and the canvas, and every genuine shot taken
        // while writing this measured above 40% ink over hundreds of colours.
        double ink = 0.0; std::size_t colours = 0;
        measureShot(rgba, W, H, ink, colours);
        std::printf("   image %.1f%% ink over %zu colours\n", ink * 100.0, colours);
        if (ink < 0.01 || colours < 8) {
            std::printf("   BLANK SHOT — the window rendered nothing. Do not quote this image.\n");
            return 1;
        }
        // And the sharper one, which the ink test misses entirely: the chrome
        // draws whatever happens, so a frame with a healthy 21% ink and 165
        // colours can still contain no simulation at all. At 141x40 the panels
        // eat the window and the canvas comes out 0x0 — a picture of the tool
        // bar, exiting 0, with the number that says so printed one line above
        // and nothing checking it.
        if (!g.library && int(fw * s) * int(fh * s) <= 0) {
            std::printf("   NO SIMULATION IN FRAME — the canvas got %dx%d. This shot shows the\n"
                        "   interface and none of the sim; it cannot be evidence about either.\n",
                        cw, ch);
            return 1;
        }
    }
    return 0;
}
#else

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetUnhandledExceptionFilter(crashFilter);
    // Take responsibility for DPI. Without this Windows bitmap-stretches the
    // window on a scaled display, which is blurry; with it, everything below
    // has to be scaled by hand, which applyScale() does.
    if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL (WINAPI*)(HANDLE);
        if (auto f = (SetCtxFn)(void*)GetProcAddress(user, "SetProcessDpiAwarenessContext"))
            f(HANDLE(-4));            // PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }
    // WNDCLASSEXW, not WNDCLASSW: only the EX form carries hIconSm, and without
    // it Windows downscales the 256px icon for the title bar instead of using
    // the crisp 16px variant that is already in the .ico.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"LifeSimWorkbench";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    // Resource id 1 is workbench.ico, generated by make_icon.exe: a glider,
    // four generations into a real Conway simulation. LoadImage with
    // LR_DEFAULTSIZE picks the right resolution per use, so the small title-bar
    // icon is the crisp 16px variant rather than a downscaled 256.
    wc.hIcon   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wc);

    // restore last window size
    int ww = 1640, wh = 1000;
    { std::ifstream in(bench::path_from_utf8(cfgPath()));
      if(!in)in.open(bench::path_from_utf8(bench::Paths::get().exeDir()+"/workbench.cfg"));
      if (in) in >> ww >> wh;
      ww = std::clamp(ww, 1100, 6000); wh = std::clamp(wh, 700, 4000); }

    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Life-sim workbench",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
        nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;

    {
        HDC screen = GetDC(nullptr);
        const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
        applyScale(std::clamp(float(dpi) / 96.0f, 1.0f, 3.0f));
    }
    const int base = int(16 * gScale + 0.5f), big = int(20 * gScale + 0.5f);
    g.font = CreateFontW(-base,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g.bold = CreateFontW(-big,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    g.ui.font = g.font; g.ui.bold = g.bold;
    g.view.trail = 0.85f;

    g.hw = bench::Hardware::probe();
    g.guard.configure(g.hw.stepBudgetMs, g.hw.maxStepsPerFrame);

    g.autoSave=true;
    rebuildRows();
    openLibrary();
    DragAcceptFiles(g.hwnd, TRUE);   // drop a .rle or .cells on the window
    ShowWindow(g.hwnd, show);

    DWORD last = GetTickCount(), lastPaint = 0;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // A unit still stepping the sim lands before the sim is destroyed
                // on the way out; what it threw no longer matters.
                if (g.work) { g.work->wait(); try { g.work->collect(); } catch (...) {} }
                return 0;
            }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();
        pollBuild();
        pollDedicated();
        // Clamp: if a frame took a second (a slow rule, a rebuild, the window
        // being dragged), do not then try to "catch up" a second of stepping.
        const double dt = std::min(0.25, double(now - last) / 1000.0); last = now;

        if (g.watch && !g.library && !g.buildProcess && !workBusy() && now - g.lastWatch > 400) {
            g.lastWatch = now;
            if (g.sel < g.rows.size() && g.rows[g.sel].plugin && g.plug.changed_on_disk())
                reloadPlugin(true);
        }
        // Volatile hardware facts move; re-read them occasionally so the limits
        // track reality rather than whatever was true at launch.
        if (now - g.lastProbe > 2000) {g.lastProbe=now;g.hw.refresh();g.guard.configure(g.hw.stepBudgetMs,g.hw.maxStepsPerFrame);}

        stepFrame(dt);
        // ~60fps rather than as fast as the CPU will spin
        if (now - lastPaint >= DWORD(g.largeRun?67:16)) {
            lastPaint = now;
            InvalidateRect(g.hwnd, nullptr, FALSE);
            UpdateWindow(g.hwnd);
        } else Sleep(1);
    }
}
#endif


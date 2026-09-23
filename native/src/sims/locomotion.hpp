#pragma once
#include "../sim.hpp"
#include "../learn/locomotion.hpp"
#include <chrono>

namespace bench {
class Locomotion final : public Sim {
public:
    enum class Tool { Move, Joint, Bone, Muscle, Erase };
    Locomotion() {
        knobs_={
            {"task","Movement task",0,2,0,1,{"Walk forward","Jump high","Climb stairs"},true,"Walking and stairs score final forward distance. Jumping scores peak body height above its settled start."},
            {"population","Creatures per generation",8,128,32,8,{},true,"Every creature has its own neural controller and independent physics trial."},
            {"seconds","Trial length (seconds)",3,20,8,1,{},true,"Equal simulated time for every creature, after a half-second settling period."},
            {"speed","Playback speed",1,16,2,1,{},false,"Physics ticks per visual step. Does not change generation results.",true},
            {"mutation","Mutation probability",0,0.5f,0.12f,0.01f,{},true,"Chance of perturbing each inherited neural weight. Two elites survive unchanged."},
            {"strength","Muscle stiffness",0.25f,3,1,0.05f,{},true,"Scales muscle stiffness; target lengths stay within 65-135% of their original length."},
            {"friction","Ground grip",0,1.5f,0.8f,0.05f,{},true,"Coulomb contact friction. Zero removes horizontal traction on a flat floor."},
            {"seed","Evolution seed",1,9999,42,1,{},true,"Reproduce the same initial controllers and breeding choices."}
        };
        surf_.resize(1120,680);reset();
    }
    const Provenance& about() const override {return about_;}
    const std::vector<Swatch>& palette() const override {return palette_;}
    const Field& field() const override {return field_;}
    const Surface* surface() const override {return &surf_;}
    std::vector<Knob>& knobs() override {return knobs_;}
    std::uint64_t generation() const override {return frames_;}
    const char* epoch_name() const override {return editing_||replaying_?nullptr:"generation";}
    int epoch_count() const override {return evo_.completed;}
    void on_knob(const std::string& key,float value) override {
        if(!std::isfinite(value))return;
        for(auto& k:knobs_)if(k.key==key){k.value=k.quantised(std::clamp(value,k.min,k.max));break;}
    }
    void reset() override {
        auto value=[&](const char* key){for(auto k:knobs_)if(k.key==key)return k.value;return 0.f;};
        evo_.settings={int(value("population")),int(value("seconds")),std::uint64_t(value("seed")),motion::Task(int(value("task"))),value("mutation"),value("strength"),value("friction")};
        evo_.reset();frames_=0;dirty_=false;replaying_=false;selected_=-1;message_=evo_.body.validate();render();
    }
    void step() override {
        if(editing_)return;
        const int speed=int(knobs_[3].value);
        for(int i=0;i<speed;++i) {if(replaying_)evo_.replayTick();else evo_.tick();++frames_;}
        render();
    }
    bool advance_epoch() override {
        if(editing_||replaying_||evo_.trials.empty())return false;
        frames_+=std::uint64_t(evo_.duration()-evo_.elapsed);const bool ok=evo_.epoch();render();return ok;
    }
    // The desktop trains in bounded slices. Headless callers retain the exact
    // full-epoch API, and both paths run the same fixed physics ticks and RNG.
    bool advance_slice(double milliseconds=12) {
        if(editing_||replaying_||evo_.trials.empty())return false;
        const int target=evo_.completed+1;const auto start=std::chrono::steady_clock::now();
        do {evo_.tick();++frames_;}
        while(evo_.completed<target&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<milliseconds);
        render();return true;
    }
    std::vector<Metric> metrics() const override {
        // Do not seed a fitness curve with an unevaluated zero: a first trial
        // that goes backwards must not inherit a fictional 0 m "best".
        if(!evo_.hasChampion)return {};
        return {{"Best distance / height (m)",evo_.best},{"Generation mean (m)",evo_.mean},
            {"Completed generations",double(evo_.completed),0,Metric::Neither}};
    }
    std::string subtitle() const override {
        if(editing_)return "Build a body | "+std::to_string(evo_.body.joints.size())+" joints | "+std::to_string(evo_.body.muscles())+" muscles";
        return std::string(replaying_?"Champion replay":"Generation "+std::to_string(evo_.completed+1))+" | "+taskName();
    }
    const motion::Evolution& evolution() const {return evo_;}
    bool editing() const {return editing_;}
    bool replaying() const {return replaying_;}
    bool dirty() const {return dirty_;}
    bool canUndo() const {return !undo_.empty();}
    void followBest() {selected_=-1;render();}
    Tool tool() const {return tool_;}
    const std::string& message() const {return message_;}
    const char* toolHint() const {
        switch(tool_){case Tool::Move:return "Drag a joint. Positions snap to the grid.";case Tool::Joint:return "Click an empty grid point to add a joint.";
            case Tool::Bone:return "Click two joints to connect a rigid bone.";case Tool::Muscle:return "Click two joints to connect an active muscle.";
            case Tool::Erase:return "Click a joint or link to remove it.";}return "";
    }
    void setTool(Tool tool) {tool_=tool;anchor_=-1;message_.clear();render();}
    bool edit(bool enabled) {
        if(enabled){editing_=true;replaying_=false;anchor_=-1;message_.clear();render();return true;}
        message_=evo_.body.validate();if(!message_.empty()){render();return false;}
        editing_=false;if(dirty_)reset();else render();return true;
    }
    bool watchChampion() {
        if(editing_||!evo_.hasChampion){message_="Complete a generation first, then replay its champion.";return false;}
        replaying_=!replaying_;if(replaying_)evo_.startReplay();message_.clear();render();return true;
    }
    void preset(int index) {remember();evo_.body=motion::Body::preset(index);changed();}
    void clearBody() {remember();evo_.body={};changed();}
    void undo() {if(undo_.empty())return;evo_.body=undo_.back();undo_.pop_back();changed();}
    std::string saveBody() const {return evo_.body.encode();}
    bool loadBody(const std::string& data) {
        motion::Body candidate;
        if(!motion::Body::decode(data,candidate,message_))return false;
        remember();evo_.body=std::move(candidate);editing_=true;replaying_=false;changed();return true;
    }
    bool poke(float nx,float ny) override {
        edit(true);const auto previous=tool_;tool_=Tool::Joint;const bool ok=drag_begin(nx,ny);tool_=previous;return ok;
    }
    bool drag_begin(float nx,float ny) override {
        if(!std::isfinite(nx)||!std::isfinite(ny))return false;
        if(!editing_) {
            if(!replaying_&&ny*680>=568&&ny*680<656&&nx*1120>=24&&nx*1120<760){selected_=std::min(int(evo_.trials.size())-1,int((nx*1120-24)/92));render();return true;}
            return false;
        }
        if(nx*1120<40||nx*1120>1080||ny*680<108||ny*680>560)return false;
        motion::Vec p=editorPoint(nx,ny);int hit=nearest(p);
        message_.clear();
        if(tool_==Tool::Move){anchor_=hit;if(hit>=0){remember();dragging_=true;}render();return true;}
        if(tool_==Tool::Joint){
            motion::Body candidate=evo_.body;candidate.joints.push_back(p);
            if(accept(candidate)){remember();evo_.body=std::move(candidate);changed();}
        } else if(tool_==Tool::Bone||tool_==Tool::Muscle){
            if(hit<0)message_="Choose a joint at each end of the link.";
            else if(anchor_<0){anchor_=hit;message_="Now choose the other joint.";}
            else if(hit==anchor_){anchor_=-1;}
            else {
                motion::Body candidate=evo_.body;candidate.links.push_back({anchor_,hit,tool_==Tool::Muscle});
                if(accept(candidate)){remember();evo_.body=std::move(candidate);changed();}anchor_=-1;
            }
        } else if(tool_==Tool::Erase){
            if(hit>=0){remember();auto& b=evo_.body;b.joints.erase(b.joints.begin()+hit);
                std::erase_if(b.links,[&](auto l){return l.a==hit||l.b==hit;});for(auto& l:b.links){if(l.a>hit)--l.a;if(l.b>hit)--l.b;}changed();}
            else for(std::size_t i=0;i<evo_.body.links.size();++i){auto l=evo_.body.links[i];auto a=evo_.body.joints[l.a],d=evo_.body.joints[l.b]-a;
                const double t=std::clamp(motion::dot(p-a,d)/motion::dot(d,d),0.0,1.0);
                if(motion::length(p-(a+d*t))<0.18){remember();evo_.body.links.erase(evo_.body.links.begin()+std::ptrdiff_t(i));changed();break;}}
        }
        render();return true;
    }
    void drag_move(float nx,float ny) override {
        if(!editing_||!dragging_||anchor_<0||!std::isfinite(nx)||!std::isfinite(ny))return;
        auto candidate=evo_.body;candidate.joints[anchor_]=editorPoint(nx,ny);
        if(accept(candidate)){evo_.body=std::move(candidate);dirty_=true;}render();
    }
    void drag_end() override {if(dragging_)anchor_=-1;dragging_=false;render();}
private:
    motion::Evolution evo_;
    Surface surf_;
    Field field_{224,136};
    std::uint64_t frames_=0;
    bool editing_=false,replaying_=false,dirty_=false,dragging_=false;
    Tool tool_=Tool::Move;
    int anchor_=-1,selected_=-1,clipTop_=0,clipBottom_=680;
    std::string message_;
    std::vector<motion::Body> undo_;
    std::vector<Knob> knobs_;
    const std::vector<Swatch> palette_={{{16,25,34},"Background"},{{226,233,226},"Bone / joint"},{{240,150,119},"Muscle"},{{136,211,170},"Course / contact"}};
    const Provenance about_{"Creature evolution: movement lab","2026","Original workbench implementation, inspired by Evolution by Keiwan",
        "Keiwan, Evolution (keiwando.com/evolution). Physics: Macklin, Muller & Chentanez, XPBD (2016). Neural weight selection: Goldberg (1989).",
        Replication::No,"The experiment breeds neural controllers. Creatures do not reproduce inside the physical world.",
        "Build with joints, rigid bones and contracting muscles, then evolve a population of neural controllers. Walk and stairs score final center-of-mass travel; jump scores peak rise after settling. Each trial uses fixed 1/240 s physics substeps and the same starting body. Eight hidden neurons receive joint positions, contacts, velocity and a clock. Tournament selection, crossover and mutation breed controllers; two elites survive. Edit the body to start a new experiment, or replay the champion without changing training. This is an original approximation: muscles join pairs of joints, only joints collide with the course, and links can cross. There is no self-collision, flight or aerodynamics. Save body stores geometry, not trained brains."};
    const char* taskName() const {switch(evo_.settings.task){case motion::Task::Walk:return "WALK FORWARD";case motion::Task::Jump:return "JUMP HIGH";case motion::Task::Stairs:return "CLIMB STAIRS";}return "";}
    void remember(){if(undo_.size()==40)undo_.erase(undo_.begin());undo_.push_back(evo_.body);}
    void changed(){dirty_=true;editing_=true;replaying_=false;anchor_=-1;message_.clear();render();}
    bool accept(const motion::Body& body){message_=body.validate(false);return message_.empty();}
    motion::Vec editorPoint(float nx,float ny) const {return {std::clamp(std::round((double(nx)*1120-560)/84*10)/10,-4.0,4.0),std::clamp(std::round((548-double(ny)*680)/76*10)/10,0.12,5.5)};}
    int nearest(motion::Vec p) const {int hit=-1;double distance=0.25;for(std::size_t i=0;i<evo_.body.joints.size();++i){double d=motion::length(p-evo_.body.joints[i]);if(d<distance){distance=d;hit=int(i);}}return hit;}
    using Colour=std::array<int,3>;
    void pixel(int x,int y,Colour colour,double alpha=1){
        if(x<0||y<clipTop_||x>=surf_.w||y>=clipBottom_)return;
        auto i=(std::size_t(y)*surf_.w+x)*4;
        for(int c=0;c<3;++c)surf_.rgba[i+c]=std::uint8_t(surf_.rgba[i+c]*(1-alpha)+colour[c]*alpha);
        surf_.rgba[i+3]=255;
    }
    void rect(int x,int y,int w,int h,Colour colour,double alpha=1){for(int yy=std::max(0,y);yy<std::min(680,y+h);++yy)for(int xx=std::max(0,x);xx<std::min(1120,x+w);++xx)pixel(xx,yy,colour,alpha);}
    void disc(double x,double y,double radius,Colour colour,double alpha=1){
        if(!std::isfinite(x)||!std::isfinite(y)||x+radius<0||x-radius>1120||y+radius<0||y-radius>680)return;
        for(int yy=std::max(0,int(y-radius-1));yy<std::min(680,int(y+radius+2));++yy)
            for(int xx=std::max(0,int(x-radius-1));xx<std::min(1120,int(x+radius+2));++xx){double d=std::sqrt((xx-x)*(xx-x)+(yy-y)*(yy-y));pixel(xx,yy,colour,alpha*std::clamp(radius+0.5-d,0.0,1.0));}
    }
    void line(motion::Vec a,motion::Vec b,double width,Colour colour,double alpha=1){
        if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(b.x)||!std::isfinite(b.y))return;
        // Clip before sampling so creatures far behind the camera cost no draw time.
        const auto d=b-a;double lo=0,hi=1;
        auto clip=[&](double p,double q){if(std::fabs(p)<1e-10)return q>=0;double t=q/p;if(p<0)lo=std::max(lo,t);else hi=std::min(hi,t);return lo<=hi;};
        if(!clip(-d.x,a.x+width)||!clip(d.x,1120+width-a.x)||!clip(-d.y,a.y+width)||!clip(d.y,680+width-a.y))return;
        b=a+d*hi;a=a+d*lo;
        // Rasterize a narrow strip once per pixel. Stamping an overlapping disc
        // at every line sample multiplied both work and ghost opacity by width.
        const bool vertical=std::fabs(b.y-a.y)>std::fabs(b.x-a.x);
        if(vertical){std::swap(a.x,a.y);std::swap(b.x,b.y);}
        const auto direction=b-a;const double square=motion::dot(direction,direction);
        if(square<1e-10){disc(vertical?a.y:a.x,vertical?a.x:a.y,width/2,colour,alpha);return;}
        const double radius=width/2,slope=std::fabs(direction.x)>1e-10?direction.y/direction.x:0;
        const double extent=(radius+1)*std::sqrt(1+slope*slope);
        const int majorLimit=vertical?680:1120,minorLimit=vertical?1120:680;
        const int begin=std::max(0,int(std::floor(std::min(a.x,b.x)-radius-1)));
        const int end=std::min(majorLimit-1,int(std::ceil(std::max(a.x,b.x)+radius+1)));
        for(int major=begin;major<=end;++major){
            const double at=std::clamp((major-a.x)/direction.x,0.0,1.0);
            const double center=a.y+at*direction.y;
            const int low=std::max(0,int(std::floor(center-extent))),high=std::min(minorLimit-1,int(std::ceil(center+extent)));
            for(int minor=low;minor<=high;++minor){
                const motion::Vec p{double(major),double(minor)};
                const double t=std::clamp(motion::dot(p-a,direction)/square,0.0,1.0);
                const double coverage=std::clamp(radius+0.5-motion::length(p-(a+direction*t)),0.0,1.0);
                if(coverage>0)pixel(vertical?minor:major,vertical?major:minor,colour,alpha*coverage);
            }
        }
    }
    void text(int x,int y,const std::string& label,Colour colour,int scale=2){
        // Small original block alphabet for portable canvas labels. The host
        // retains native text for controls, descriptions and accessibility.
        static constexpr unsigned char glyphs[][7]={
            {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
            {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
            {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
            {17,27,21,21,17,17,17},{17,25,25,21,19,19,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
            {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
            {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
            {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
            {14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
            {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
        for(char c:label){int g=(c>='A'&&c<='Z')?c-'A':(c>='0'&&c<='9'?26+c-'0':-1);
            if(g>=0)for(int row=0;row<7;++row)for(int col=0;col<5;++col)if(glyphs[g][row]&(1<<(4-col)))rect(x+col*scale,y+row*scale,scale,scale,colour);
            if(c=='.')rect(x+2*scale,y+6*scale,scale,scale,colour);
            if(c=='-')rect(x,y+3*scale,5*scale,scale,colour);
            if(c=='/')line({double(x),double(y+6*scale)},{double(x+4*scale),double(y)},scale,colour);
            x+=6*scale;
        }
    }
    static std::string number(double value,int decimals=1){char b[40];std::snprintf(b,sizeof b,"%.*f",decimals,value);return b;}
    void render(){
        const Colour ink{227,235,227},muted{129,151,156},mint{141,221,181},coral{241,146,115},grid{36,54,65};
        for(int y=0;y<680;++y)rect(0,y,1120,1,{16+y/85,25+y/70,34+y/60});
        rect(24,24,4,46,mint);text(44,25,editing_?"BODY WORKSHOP":"MOVEMENT LAB",ink,3);
        text(44,58,editing_?"JOINTS / BONES / MUSCLES":taskName(),muted);
        text(772,28,editing_?"DESIGN YOUR MOVER":"GEN "+std::to_string(evo_.completed+1),mint);
        text(772,56,editing_?"24 JOINTS MAX":std::to_string(evo_.settings.population)+" CREATURES",muted);
        line({24,88},{1096,88},1,grid);
        if(editing_){
            for(int i=-5;i<=5;++i)line({560.0+i*84,108},{560.0+i*84,548},1,grid);
            for(int i=0;i<=5;++i)line({80,548.0-i*76},{1040,548.0-i*76},1,grid);
            rect(40,550,1040,12,{52,79,73});text(58,521,"GROUND",muted,1);
            std::vector<motion::Vec> points;for(auto p:evo_.body.joints)points.push_back({560+p.x*84,548-p.y*76});
            drawBody(points,nullptr,1,ink,coral);
            for(std::size_t i=0;i<points.size();++i){auto p=points[i];text(int(p.x+13),int(p.y-19),std::to_string(i+1),muted,1);if(int(i)==anchor_)disc(p.x,p.y,15,mint,0.45);}
            text(40,596,std::to_string(points.size())+" JOINTS",ink);text(270,596,std::to_string(evo_.body.links.size()-evo_.body.muscles())+" BONES",ink);
            text(480,596,std::to_string(evo_.body.muscles())+" MUSCLES",coral);
            text(40,632,evo_.body.validate().empty()?"BODY READY - RETURN TO EXPERIMENT TO EVOLVE":"CONNECT THE BODY - THEN RETURN TO EXPERIMENT",mint,1);
        } else {
            const motion::Trial* shown=nullptr;
            if(replaying_&&!evo_.replay.joints.empty())shown=&evo_.replay;
            else if(!evo_.trials.empty())shown=(selected_<0&&evo_.elapsed==0&&evo_.hasChampion)?&evo_.winner:&evo_.trials[selected_<0?evo_.leader():std::min(selected_,int(evo_.trials.size())-1)];
            motion::Vec c=shown?shown->center():motion::Vec{};
            const double cx=c.x,base=std::max(0.0,c.y-2.1),scale=104;
            clipTop_=102;clipBottom_=536;
            auto point=[&](motion::Vec p){return motion::Vec{500+(p.x-cx)*scale,478-(p.y-base)*scale};};
            for(int i=int(cx)-8;i<=int(cx)+9;++i){auto p=point({double(i),0});line({p.x,112},{p.x,520},1,grid);}
            const int ground=int(point({0,0}).y);rect(24,std::max(112,ground),1072,std::max(0,534-std::max(112,ground)),{37,59,56});
            line({24,double(ground)},{1096,double(ground)},2,mint,0.75);
            if(evo_.settings.task==motion::Task::Stairs)for(int i=0;i<12;++i){auto p=point({2.5+i,0.28*(i+1)});rect(int(p.x),int(p.y),int(scale),std::max(0,ground-int(p.y)),{52,79,73});line(p,p+motion::Vec{scale,0},2,mint);}
            for(int i=int(cx)-8;i<=int(cx)+9;++i){auto p=point({double(i),0});if(p.x>24&&p.x<1060)text(int(p.x+4),516,std::to_string(i)+" M",muted,1);}
            const std::size_t ghosts=evo_.body.links.size()>12?2:8;
            if(!replaying_)for(std::size_t i=0;i<std::min(evo_.trials.size(),ghosts);++i)if(&evo_.trials[i]!=shown){std::vector<motion::Vec> ps;for(auto j:evo_.trials[i].joints)ps.push_back(point(j.p));drawBody(ps,&evo_.trials[i],0.10,ink,coral);}
            if(shown){std::vector<motion::Vec> ps;for(auto j:shown->joints)ps.push_back(point(j.p));drawBody(ps,shown,1,ink,coral);}
            clipTop_=0;clipBottom_=680;
            // The cards are drawn after the world so passing creatures cannot
            // cover labels. Best is from completed trials, never an extrapolation.
            rect(40,112,270,78,{22,35,44});text(56,124,replaying_?"CHAMPION REPLAY":(shown==&evo_.winner?"BEST BODY":"LIVE "+number(shown?shown->score:0)+" M"),mint);
            text(56,155,"BEST "+(evo_.hasChampion?number(evo_.best)+" M":"AWAITING TRIAL"),muted,1);
            rect(846,112,230,86,{22,35,44});text(862,126,"TRIAL TIME",muted,1);
            text(862,146,number(std::max(0,(shown?shown->ticks:0)-motion::Trial::settleTicks)/60.0)+" / "+std::to_string(evo_.settings.seconds)+" S",ink);
            rect(862,179,196,3,grid);rect(862,179,int(196.0*(shown?shown->ticks:0)/evo_.duration()),3,mint);
            if(shown){
                rect(846,212,230,52+evo_.body.muscles()*14,{22,35,44});text(862,226,"MUSCLE LENGTH",muted,1);
                int m=0;for(std::size_t i=0;i<evo_.body.links.size();++i)if(evo_.body.links[i].muscle){
                    int yy=248+m*14;double ratio=shown->target[i]/shown->rest[i];text(862,yy,"M"+std::to_string(++m),muted,1);
                    rect(898,yy,158,5,grid);rect(898,yy,int(158*std::clamp((ratio-0.65)/0.7,0.0,1.0)),5,ratio<1?coral:Colour{125,192,191});rect(976,yy-1,1,7,ink);
                }
            }
            rect(0,540,1120,140,{16,25,34});text(24,546,replaying_?"REPLAY LEAVES TRAINING UNCHANGED":"FIRST 8 OF "+std::to_string(evo_.trials.size())+" - CLICK A CREATURE TO FOLLOW",muted,1);
            for(int i=0;i<std::min(8,int(evo_.trials.size()));++i){
                int x=24+i*92;rect(x,568,86,88,selected_==i?Colour{42,68,63}:Colour{26,40,49});
                const auto& joints=evo_.trials[i].joints;
                motion::Vec low=joints.front().p,high=low;
                for(auto j:joints){low.x=std::min(low.x,j.p.x);low.y=std::min(low.y,j.p.y);high.x=std::max(high.x,j.p.x);high.y=std::max(high.y,j.p.y);}
                const auto center=(low+high)*0.5;
                const double fit=std::min({15.0,66/std::max(0.1,high.x-low.x),46/std::max(0.1,high.y-low.y)});
                std::vector<motion::Vec> ps;for(auto j:joints)ps.push_back({x+43+(j.p.x-center.x)*fit,605-(j.p.y-center.y)*fit});
                drawBody(ps,&evo_.trials[i],0.9,ink,coral,0.4);text(x+8,640,number(evo_.trials[i].score)+" M",muted,1);
            }
            text(792,546,"BEST / GENERATION",mint,1);
            line({792,650},{1092,650},1,grid);
            const auto& history=evo_.bestHistory;
            if(history.size()>1){double lo=std::min(0.0,*std::min_element(history.begin(),history.end())),hi=std::max(lo+0.1,*std::max_element(history.begin(),history.end()));
                for(std::size_t i=1;i<history.size();++i)line({792+300.0*(i-1)/(history.size()-1),646-72*(history[i-1]-lo)/(hi-lo)},{792+300.0*i/(history.size()-1),646-72*(history[i]-lo)/(hi-lo)},2,mint);}
            else text(792,598,"COMPLETE A GENERATION",muted,1);
        }
        // Compact truthful raster for the existing history/export contract.
        for(int y=0;y<field_.h;++y)for(int x=0;x<field_.w;++x){auto i=(std::size_t(y*5)*1120+x*5)*4;auto r=surf_.rgba[i],g=surf_.rgba[i+1],b=surf_.rgba[i+2];field_.set(x,y,r>190?(g>190?1:2):(g>90&&g>r&&g>b?3:0));}
    }
    void drawBody(const std::vector<motion::Vec>& points,const motion::Trial* trial,double alpha,Colour ink,Colour coral,double size=1){
        for(std::size_t i=0;i<evo_.body.links.size();++i){auto l=evo_.body.links[i];auto a=points[l.a],b=points[l.b];
            if(l.muscle){Colour col=coral;if(trial&&trial->target[i]>trial->rest[i])col={125,192,191};line(a,b,7*size,{10,18,24},alpha);line(a,b,4*size,col,alpha);
                if(size>0.5){auto d=b-a;double len=motion::length(d);if(len>1){auto n=motion::Vec{-d.y/len,d.x/len};for(int k=1;k<=5;++k){auto p=a+d*(k/6.0);line(p-n*4,p+n*4,1,col,alpha);}}}}
            else {line(a,b,10*size,{10,18,24},alpha);line(a,b,5*size,ink,alpha);}}
        for(std::size_t i=0;i<points.size();++i){auto p=points[i];disc(p.x,p.y,9*size,{12,23,29},alpha);disc(p.x,p.y,5.5*size,trial&&trial->joints[i].touching?Colour{141,221,181}:ink,alpha);}
    }
};
inline SimPtr make_locomotion(){return std::make_unique<Locomotion>();}
} // namespace bench

#pragma once
// Original 2D movement model. Distance constraints use XPBD Algorithm 1,
// equations 17/18 (Macklin, Muller & Chentanez, 2016):
// https://matthias-research.github.io/pages/publications/XPBD.pdf
// Joints carry equal mass and collide with the course. Links have no collision
// thickness or self-collision. Muscles connect joints, not bone midpoints.
#include "mlp.hpp"
#include "select.hpp"
#include <array>
#include <iomanip>
#include <locale>
#include <sstream>

namespace bench::motion {
struct Vec {
    double x = 0, y = 0;
    Vec operator+(Vec b) const { return {x+b.x,y+b.y}; }
    Vec operator-(Vec b) const { return {x-b.x,y-b.y}; }
    Vec operator*(double k) const { return {x*k,y*k}; }
    Vec& operator+=(Vec b) { x+=b.x; y+=b.y; return *this; }
};
inline double dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y; }
inline double length(Vec a) { return std::sqrt(dot(a,a)); }
struct Link { int a=0,b=1; bool muscle=false; };
struct Body {
    static constexpr int maxJoints=24, maxLinks=64, maxMuscles=16;
    std::vector<Vec> joints;
    std::vector<Link> links;
    int muscles() const { return int(std::count_if(links.begin(),links.end(),[](auto l){return l.muscle;})); }
    std::string validate(bool runnable=true) const {
        if (joints.size()>maxJoints || links.size()>maxLinks || muscles()>maxMuscles) return "Limit: 24 joints, 64 links and 16 muscles.";
        for (auto p:joints) if (!std::isfinite(p.x)||!std::isfinite(p.y)||p.x< -4||p.x>4||p.y<0.12||p.y>5.5) return "Keep joints inside the build grid, above ground.";
        for (std::size_t i=0;i<joints.size();++i) for(std::size_t j=0;j<i;++j)
            if(length(joints[i]-joints[j])<0.20) return "Leave at least 0.2 m between joints.";
        for (std::size_t i=0;i<links.size();++i) {
            auto l=links[i];
            if(l.a<0||l.b<0||l.a>=int(joints.size())||l.b>=int(joints.size())||l.a==l.b) return "A link needs two different existing joints.";
            for(std::size_t j=0;j<i;++j) if((l.a==links[j].a&&l.b==links[j].b)||(l.a==links[j].b&&l.b==links[j].a)) return "Only one bone or muscle per joint pair.";
        }
        if(!runnable) return {};
        if(joints.size()<3) return "Add at least three joints.";
        if(muscles()==0) return "Add a muscle so the brain can move the body.";
        std::array<bool,maxJoints> reached{}; reached[0]=true;
        for(std::size_t i=0;i<joints.size();++i) for(auto l:links) if(reached[l.a]||reached[l.b]) reached[l.a]=reached[l.b]=true;
        for(std::size_t i=0;i<joints.size();++i) if(!reached[i]) return "Connect every joint to the body with a bone or muscle.";
        return {};
    }
    static Body preset(int i) {
        if(i==1) return {{{-0.9,0.2},{0.9,0.2},{0,1.7}},{{0,1,false},{0,2,true},{1,2,true}}};
        if(i==2) return {{{-1.3,0.2},{0,0.2},{1.3,0.2},{-0.7,1},{0.7,1}},
            {{0,3,false},{3,4,false},{4,2,false},{3,1,true},{4,1,true},{0,1,true},{1,2,true}}};
        return {{{-0.7,1.3},{0.7,1.3},{-1,0.2},{1,0.2}},
            {{0,1,false},{0,2,false},{1,3,false},{1,2,true},{0,3,true},{2,3,true}}};
    }
    std::string encode() const {
        std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(17);
        out<<"MOTION_BODY 1\n"<<joints.size()<<' '<<links.size()<<'\n';
        for(auto p:joints) out<<p.x<<' '<<p.y<<'\n';
        for(auto l:links) out<<l.a<<' '<<l.b<<' '<<int(l.muscle)<<'\n';
        return out.str();
    }
    static bool decode(const std::string& data,Body& destination,std::string& error) {
        if(data.size()>32768) {error="Creature file exceeds 32 KB.";return false;}
        std::istringstream in(data); in.imbue(std::locale::classic());
        std::string magic; int version=0,n=0,m=0;
        if(!(in>>magic>>version>>n>>m)||magic!="MOTION_BODY"||version!=1||n<0||n>maxJoints||m<0||m>maxLinks) {
            error="Expected a MOTION_BODY version 1 creature file.";return false;
        }
        Body candidate; candidate.joints.resize(n);candidate.links.resize(m);
        for(auto& p:candidate.joints) if(!(in>>p.x>>p.y)) {error="Invalid joint coordinates.";return false;}
        for(auto& l:candidate.links) {int type=-1;if(!(in>>l.a>>l.b>>type)||type<0||type>1){error="Invalid link.";return false;}l.muscle=type==1;}
        in>>std::ws;
        if(!in.eof()) {error="Unexpected data after the creature.";return false;}
        error=candidate.validate(false);
        if(!error.empty()) return false;
        destination=std::move(candidate);return true;
    }
};

enum class Task { Walk, Jump, Stairs };
struct Settings { int population=32,seconds=8;std::uint64_t seed=42;Task task=Task::Walk;double mutation=0.12,strength=1.0,friction=0.8; };
struct Joint { Vec p,v,old,normal;double correction=0;bool touching=false; };
struct Trial {
    static constexpr double dt=1.0/240.0,radius=0.10;
    static constexpr int settleTicks=30;
    std::vector<Joint> joints;
    std::vector<double> rest,target,desired,lambda;
    MLP brain;
    std::vector<float> sensors;
    int ticks=0;
    double startX=0,startY=0,peakY=0,score=0;
    bool failed=false;
    Vec center() const {Vec p;for(const auto& j:joints)p+=j.p;return p*(1.0/double(joints.size()));}
    void init(const Body& body,const MLP& controller) {
        *this=Trial{}; brain=controller;
        for(auto p:body.joints) joints.push_back({p,{},{},{},0,false});
        // Every trial starts with the same geometry, on the same piece of ground.
        double minY=10;for(auto j:joints) minY=std::min(minY,j.p.y);
        Vec c=center();for(auto& j:joints){j.p.x-=c.x;j.p.y+=radius+0.03-minY;}
        for(auto l:body.links) rest.push_back(length(body.joints[l.a]-body.joints[l.b]));
        target=desired=rest;lambda.resize(rest.size());sensors.resize(7+3*joints.size());
        c=center();startX=c.x;startY=peakY=c.y;
    }
    static void project(Joint& j,Vec normal,double depth) {
        if(depth<=0) return;
        j.p+=normal*depth;j.normal+=normal*depth;j.correction+=depth;j.touching=true;
    }
    static void contact(Joint& j,Task task) {
        project(j,{0,1},radius-j.p.y);
        if(task!=Task::Stairs||j.p.x<2.5-radius) return;
        for(int s=0;s<12;++s) {
            const double left=2.5+s, right=left+1, top=0.28*(s+1);
            if(j.p.x<left-radius||j.p.x>right+radius||j.p.y>top+radius) continue;
            Vec nearest{std::clamp(j.p.x,left,right),std::clamp(j.p.y,0.0,top)};
            Vec d=j.p-nearest;double len=length(d);
            if(len>1e-10) project(j,d*(1/len),radius-len);
            else {
                // Interior: eject through the nearest exposed face. Adjacent
                // boxes are a union; their shared vertical faces stay internal.
                double depth=top-j.p.y;Vec normal{0,1};
                if(j.p.y>0.28*s && j.p.x-left<depth){depth=j.p.x-left;normal={-1,0};}
                if(s==11&&right-j.p.x<depth){depth=right-j.p.x;normal={1,0};}
                project(j,normal,depth+radius);
            }
        }
    }
    void control(const Body& body) {
        const Vec c=center();Vec velocity;for(auto j:joints)velocity+=j.v;
        velocity=velocity*(1.0/double(joints.size()));
        const double phase=(ticks-settleTicks)*(6.283185307179586/60.0);
        sensors[0]=float(std::sin(phase));sensors[1]=float(std::cos(phase));
        sensors[2]=float(std::sin(phase*0.5));sensors[3]=float(std::cos(phase*0.5));
        sensors[4]=float(std::clamp(velocity.x/5.0,-1.0,1.0));sensors[5]=float(std::clamp(velocity.y/5.0,-1.0,1.0));
        sensors[6]=float(std::clamp(c.y/3.0,0.0,2.0));
        for(std::size_t i=0;i<joints.size();++i){sensors[7+i*3]=float((joints[i].p.x-c.x)/4);sensors[8+i*3]=float((joints[i].p.y-c.y)/4);sensors[9+i*3]=joints[i].touching?1.f:0.f;}
        const auto& out=brain.forward(sensors);std::size_t muscle=0;
        for(std::size_t i=0;i<body.links.size();++i) if(body.links[i].muscle) desired[i]=rest[i]*(0.65+0.7*out[muscle++]);
    }
    void tick(const Body& body,const Settings& settings,bool powered=true) {
        if(failed) {++ticks;return;}
        if(powered&&ticks>=settleTicks&&ticks%2==0)control(body);
        for(int sub=0;sub<4;++sub) {
            for(std::size_t i=0;i<target.size();++i)
                target[i]+=std::clamp(desired[i]-target[i],-rest[i]*2*dt,rest[i]*2*dt);
            for(auto& j:joints) {
                j.old=j.p;j.v.y-=9.81*dt;j.p+=j.v*dt;
                j.normal={};j.correction=0;j.touching=false;
            }
            std::fill(lambda.begin(),lambda.end(),0.0);
            for(int iteration=0;iteration<6;++iteration) {
                for(std::size_t i=0;i<body.links.size();++i) {
                    const auto l=body.links[i]; auto& a=joints[l.a];auto& b=joints[l.b];
                    const Vec d=b.p-a.p; const double len=length(d);if(len<1e-10)continue;
                    const double compliance=l.muscle?0.0008/settings.strength:0.0000001;
                    const double alpha=compliance/(dt*dt);
                    const double delta=(-(len-target[i])-alpha*lambda[i])/(2+alpha);
                    lambda[i]+=delta;const Vec correction=d*(delta/len);
                    a.p+=correction*(-1);b.p+=correction;
                }
                for(auto& j:joints)contact(j,settings.task);
            }
            for(auto& j:joints) {
                j.v=(j.p-j.old)*(1/dt);
                if(j.correction>1e-12) {
                    const double normalLength=length(j.normal);
                    if(normalLength>1e-12) {
                        const Vec n=j.normal*(1/normalLength);double vn=dot(j.v,n);
                        // Inelastic normal contact and Coulomb tangential bound.
                        Vec tangent=j.v-n*vn;double speed=length(tangent);
                        const double reduction=settings.friction*j.correction/dt;
                        if(speed>0)tangent=tangent*(std::max(0.0,speed-reduction)/speed);
                        j.v=tangent+n*std::max(0.0,vn);
                    }
                }
                if(!std::isfinite(j.p.x)||!std::isfinite(j.p.y)||length(j.v)>100||std::fabs(j.p.x)>500||j.p.y>100)failed=true;
            }
        }
        ++ticks;const Vec c=center();
        if(ticks<=settleTicks){startX=c.x;startY=peakY=c.y;score=0;}
        else {peakY=std::max(peakY,c.y);score=settings.task==Task::Jump?peakY-startY:c.x-startX;}
        if(failed)score=-1000;
    }
};

class Evolution {
public:
    Body body=Body::preset(0);
    Settings settings;
    std::vector<Trial> trials;
    Trial winner,replay;
    std::vector<double> bestHistory,meanHistory;
    int completed=0,elapsed=0;
    double best=0,mean=0,lastBest=0;
    bool hasChampion=false;
    Rng rng;
    void reset() {
        completed=elapsed=0;best=mean=lastBest=0;hasChampion=false;
        bestHistory.clear();meanHistory.clear();trials.clear();winner=Trial{};replay=Trial{};
        rng.reseed(settings.seed);
        if(!body.validate().empty())return;
        for(int i=0;i<settings.population;++i){
            MLP brain({7+int(body.joints.size())*3,8,body.muscles()},rng.next());
            trials.emplace_back();trials.back().init(body,brain);
        }
    }
    int duration() const {return Trial::settleTicks+settings.seconds*60;}
    int leader() const {
        int bestIndex=0;for(std::size_t i=1;i<trials.size();++i)if(trials[i].score>trials[bestIndex].score)bestIndex=int(i);
        return bestIndex;
    }
    bool tick() {
        if(trials.empty())return false;
        for(auto& t:trials)t.tick(body,settings);
        if(++elapsed>=duration())finish();
        return true;
    }
    void finish() {
        std::vector<double> scores;for(const auto& t:trials)scores.push_back(t.score);
        std::vector<std::size_t> rank(trials.size());std::iota(rank.begin(),rank.end(),0);
        std::stable_sort(rank.begin(),rank.end(),[&](auto a,auto b){return scores[a]>scores[b];});
        lastBest=scores[rank[0]];mean=std::accumulate(scores.begin(),scores.end(),0.0)/scores.size();
        if(!hasChampion||lastBest>best){winner=trials[rank[0]];best=lastBest;hasChampion=true;}
        bestHistory.push_back(best);meanHistory.push_back(mean);
        if(bestHistory.size()>256){bestHistory.erase(bestHistory.begin());meanHistory.erase(meanHistory.begin());}
        std::vector<Trial> children;children.reserve(trials.size());
        // Two unchanged elites; remaining brains inherit weights via the shared
        // tournament/uniform crossover implementation. Bodies stay user-defined.
        for(std::size_t i=0;i<trials.size();++i){
            MLP brain=trials[rank[std::min(i,std::size_t(1))]].brain;
            if(i>=2){
                const auto a=select_one(scores,Selection::Tournament,rng,3);
                const auto b=select_one(scores,Selection::Tournament,rng,3);
                auto genes=crossover_vec(trials[a].brain.parameters(),trials[b].brain.parameters(),Crossover::Uniform,rng);
                for(double& gene:genes) if(rng.unit()<settings.mutation)
                    gene=std::clamp(gene+(double(rng.unit())+rng.unit()+rng.unit()-1.5)*0.7,-6.0,6.0);
                brain.set_parameters(genes);
            }
            children.emplace_back();children.back().init(body,brain);
        }
        trials=std::move(children);elapsed=0;++completed;
    }
    bool epoch() {if(trials.empty())return false;const int target=completed+1;while(completed<target)tick();return true;}
    void startReplay() {if(hasChampion)replay.init(body,winner.brain);}
    void replayTick() {if(!hasChampion)return;if(replay.joints.empty()||replay.ticks>=duration())startReplay();replay.tick(body,settings);}
};
} // namespace bench::motion

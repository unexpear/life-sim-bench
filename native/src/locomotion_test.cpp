#include "sims/locomotion.hpp"
#include <chrono>
#include <cstdio>
#include <limits>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* description){++checks;if(!ok){++failed;std::printf("FAIL %s\n",description);}};
    using namespace bench;
    using namespace bench::motion;
    MLP network({4,8,3},18),copy({4,8,3},19);
    auto genes=network.parameters();
    check(copy.set_parameters(genes),"MLP accepts a matching evolutionary genome");
    check(network.forward({0.2f,0.3f,-0.5f,1})==copy.forward({0.2f,0.3f,-0.5f,1}),"MLP genome includes every weight and bias in forward order");
    auto malformed=genes;malformed[3]=std::numeric_limits<double>::infinity();
    check(!copy.set_parameters(malformed)&&copy.parameters()==genes,"MLP rejects nonfinite genomes atomically");
    check(!copy.set_parameters({1,2})&&copy.parameters()==genes,"MLP rejects the wrong genome shape");
    Body body=Body::preset(0),decoded;std::string error;
    for(int i=0;i<3;++i)check(Body::preset(i).validate().empty(),"starter body is connected and runnable");
    check(Body::decode(body.encode(),decoded,error)&&decoded.encode()==body.encode(),"body save/load round trip is lossless");
    const auto original=decoded.encode();
    for(const auto& text:std::vector<std::string>{"", "MOTION_BODY 1\n-1 0", "MOTION_BODY 9\n0 0", "MOTION_BODY 1\n25 0", "MOTION_BODY 1\n1 1\n0 1\n0 99 0", "MOTION_BODY 1\n1 0\nnan 1", body.encode()+"garbage",std::string(32769,'x')})
        check(!Body::decode(text,decoded,error)&&decoded.encode()==original,"malformed load preserves the existing body");
    auto duplicate=body;duplicate.links.push_back(duplicate.links[0]);check(!duplicate.validate().empty(),"duplicate bone/muscle pairs are rejected");
    auto disconnected=body;disconnected.joints.push_back({3,3});check(!disconnected.validate().empty(),"detached joints cannot enter training");
    Body empty;check(empty.validate(false).empty()&&!empty.validate().empty(),"unfinished bodies can be saved but cannot train");

    Settings settings;
    MLP brain({7+3*int(body.joints.size()),8,body.muscles()},42);
    Trial passive;passive.init(body,brain);
    for(int i=0;i<510;++i)passive.tick(body,settings,false);
    check(std::fabs(passive.score)<0.03,"an unpowered creature does not acquire a walking reward from falling");
    Trial frictionless;frictionless.init(body,brain);settings.friction=0;
    for(int i=0;i<510;++i)frictionless.tick(body,settings);
    check(std::fabs(frictionless.score)<1e-7,"internal muscles cannot translate the center of mass without ground traction");
    Body particle;particle.joints={{0,1}};Trial fall;MLP unused({10,8,1},1);fall.init(particle,unused);fall.joints[0].p={0,10};
    for(int i=0;i<15;++i)fall.tick(particle,settings,false);
    const double expected=10-9.81*Trial::dt*Trial::dt*60*61/2;
    check(std::fabs(fall.joints[0].p.y-expected)<1e-9,"free fall follows the fixed-step semi-implicit gravity solution");
    Joint ground;ground.p={0,-0.5};Trial::contact(ground,Task::Walk);check(ground.p.y>=Trial::radius-1e-12,"floor resolves penetration to floating-point precision");
    Joint riser;riser.p={2.48,0.15};Trial::contact(riser,Task::Stairs);check(riser.p.x<=2.400001,"stairs collide at their vertical riser");
    Joint tread;tread.p={2.9,0.26};Trial::contact(tread,Task::Stairs);check(tread.p.y>=0.379999,"stairs collide at their top surface");

    const auto start=std::chrono::steady_clock::now();
    for(int task=0;task<3;++task){
        Evolution evolution;evolution.settings.task=Task(task);evolution.reset();double first=0,prior=-1001;
        for(int generation=0;generation<12;++generation){
            check(evolution.epoch(),"a generation completes");
            if(generation==0)first=evolution.best;
            check(evolution.lastBest+1e-9>=prior,"unchanged elites retain the previous best physical score");prior=evolution.best;
            check(evolution.bestHistory.size()==std::size_t(generation+1),"one history sample per completed generation");
            check(evolution.completed==generation+1&&evolution.elapsed==0,"generation boundary is exact");
        }
        check(evolution.best>first+0.1,"selection improves the measured course result across twelve generations");
        check(evolution.winner.score==evolution.best,"reported champion is a real evaluated trial");
        auto nextRng=evolution.rng;auto populationGenes=evolution.trials[0].brain.parameters();
        evolution.startReplay();for(int i=0;i<evolution.duration();++i)evolution.replayTick();
        check(std::fabs(evolution.replay.score-evolution.best)<1e-9,"champion replay reproduces its measured fitness");
        check(evolution.completed==12&&evolution.elapsed==0&&populationGenes==evolution.trials[0].brain.parameters()&&nextRng.next()==evolution.rng.next(),"replay preserves the population, generation and breeding RNG");
        std::printf("course %d: initial %.4f m -> generation 12 %.4f m\n",task,first,evolution.best);
    }
    Evolution a,b;a.settings.seed=b.settings.seed=19;a.reset();b.reset();
    for(int i=0;i<4;++i){a.epoch();b.epoch();}
    check(a.bestHistory==b.bestHistory&&a.trials.back().brain.parameters()==b.trials.back().brain.parameters(),"seeded evolution is reproducible");

    for(int shape=0;shape<3;++shape)for(double strength:{0.25,3.0})for(Task task:{Task::Walk,Task::Jump,Task::Stairs}){
        Evolution e;e.body=Body::preset(shape);e.settings.strength=strength;e.settings.task=task;e.settings.population=8;e.reset();
        for(int i=0;i<e.duration();++i){
            // Advance individual trials so the final geometry remains available.
            for(auto& t:e.trials)t.tick(e.body,e.settings);
        }
        bool finite=true;double boneError=0;
        for(auto& t:e.trials){for(auto j:t.joints)finite=finite&&!t.failed&&std::isfinite(j.p.x)&&j.p.y>=Trial::radius-1e-8;
            for(std::size_t i=0;i<e.body.links.size();++i)if(!e.body.links[i].muscle){auto l=e.body.links[i];boneError=std::max(boneError,std::fabs(length(t.joints[l.a].p-t.joints[l.b].p)-t.rest[i]));}}
        check(finite,"starter physics stays finite at both stiffness limits on each course");
        check(boneError<0.05,"rigid bone lengths stay within 5 cm under active muscle load");
    }

    Locomotion sim;check(sim.metrics().empty(),"all metric columns begin together, after an evaluated trial");
    auto initial=sim.saveBody();sim.edit(true);sim.setTool(Locomotion::Tool::Joint);
    auto click=[&](double x,double y){sim.drag_begin(float((560+x*84)/1120),float((548-y*76)/680));sim.drag_end();};
    click(2,2);check(sim.evolution().body.joints.size()==5,"joint tool creates a joint on the grid");
    check(!sim.edit(false)&&sim.editing(),"unconnected body stays in the workshop with validation");
    sim.setTool(Locomotion::Tool::Bone);click(0.7,1.3);click(2,2);
    check(sim.evolution().body.links.size()==7,"bone tool preserves its first endpoint across mouse release");
    check(sim.edit(false)&&!sim.editing()&&sim.epoch_count()==0,"connected custom body starts a new experiment");
    sim.edit(true);sim.undo();sim.undo();check(sim.saveBody()==initial,"undo restores the body across add-joint and add-link edits");
    sim.setTool(Locomotion::Tool::Move);sim.drag_begin(float((560-0.7*84)/1120),float((548-1.3*76)/680));
    sim.drag_move(float((560-1.5*84)/1120),float((548-2*76)/680));sim.drag_end();
    check(std::fabs(sim.evolution().body.joints[0].x+1.5)<1e-6,"move tool drags and snaps an existing joint");sim.undo();
    sim.setTool(Locomotion::Tool::Erase);click(-0.7,1.3);
    check(sim.evolution().body.joints.size()==3&&sim.evolution().body.validate(false).empty(),"erase removes incident links and renumbers remaining endpoints");sim.undo();
    sim.clearBody();check(!sim.edit(false)&&sim.editing(),"empty workshop cannot start a population");sim.undo();
    check(sim.saveBody()==initial&&sim.edit(false),"undo recovers from clear canvas");
    sim.advance_epoch();const double savedBest=sim.evolution().best;const int completed=sim.epoch_count();
    check(sim.watchChampion()&&sim.epoch_name()==nullptr,"replay disables breeding actions");sim.step();sim.watchChampion();
    check(sim.epoch_count()==completed&&sim.evolution().best==savedBest,"UI replay preserves the experiment");
    sim.edit(true);sim.edit(false);check(sim.epoch_count()==completed,"opening and closing the editor without edits preserves training");
    Locomotion slow,fast;slow.on_knob("speed",1);fast.on_knob("speed",16);
    for(int i=0;i<3;++i){slow.advance_epoch();fast.advance_epoch();}
    check(slow.evolution().bestHistory==fast.evolution().bestHistory,"playback speed does not change epoch results");
    Locomotion sliced;
    while(sliced.epoch_count()<3)sliced.advance_slice(2);
    check(sliced.evolution().bestHistory==slow.evolution().bestHistory,"interruptible training slices give identical full-generation results");
    Locomotion builder;builder.edit(true);builder.clearBody();builder.setTool(Locomotion::Tool::Joint);
    auto buildClick=[&](double x,double y){builder.drag_begin(float((560+x*84)/1120),float((548-y*76)/680));builder.drag_end();};
    buildClick(-1,0.2);buildClick(1,0.2);buildClick(0,1.8);buildClick(0,1.8);
    check(builder.evolution().body.joints.size()==3,"joint tool rejects overlapping joints without losing the body");
    builder.setTool(Locomotion::Tool::Bone);buildClick(-1,0.2);buildClick(1,0.2);
    builder.setTool(Locomotion::Tool::Muscle);buildClick(-1,0.2);buildClick(0,1.8);buildClick(1,0.2);buildClick(0,1.8);
    check(builder.evolution().body.muscles()==2&&builder.edit(false)&&builder.advance_epoch(),"a creature built from an empty canvas can evolve");
    Body large;
    for(int y=0;y<4;++y)for(int x=0;x<6;++x)large.joints.push_back({-3.0+x,0.2+y});
    for(int i=1;i<24;++i)large.links.push_back({i-1,i,false});
    for(int gap=2;large.links.size()<64;++gap)for(int i=0;i+gap<24&&large.links.size()<64;++i)
        large.links.push_back({i,i+gap,large.muscles()<16});
    check(large.validate().empty()&&large.muscles()==16,"largest supported custom body validates");
    Locomotion complex;check(complex.loadBody(large.encode())&&complex.edit(false),"largest custom body loads into an experiment");
    complex.on_knob("population",128);complex.on_knob("seconds",20);complex.reset();
    auto physics=complex.evolution();const auto physicsStart=std::chrono::steady_clock::now();physics.tick();
    const double physicsMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-physicsStart).count();
    const auto sliceStart=std::chrono::steady_clock::now();complex.advance_slice(12);
    const double sliceMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sliceStart).count();
    check(complex.evolution().elapsed>0&&complex.epoch_count()==0,"large training returns control before a complete generation");
    complex.edit(true);const int stoppedAt=complex.evolution().elapsed;
    check(!complex.advance_slice()&&complex.evolution().elapsed==stoppedAt,"opening the editor stops further training slices");
    std::printf("128-creature / 24-joint / 64-link physics tick: %.2f ms; training slice including render: %.2f ms\n",physicsMs,sliceMs);
    std::printf("%d checks, %d failures; %.3f s\n",checks,failed,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    return failed?1:0;
}

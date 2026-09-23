#include "sims/rulespec.hpp"
#include "sims/life_like.hpp"
#include "sims/small_lattice.hpp"
#include "projects.hpp"
#include <iostream>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const std::string& message){++checks;if(!ok){++failed;std::cerr<<"FAIL: "<<message<<'\n';}};
    for(const auto& [input,canonical]:std::vector<std::pair<std::string,std::string>>{
        {" b3 / s23 ","B3/S23"},{"23/3","B3/S23"},{"S23/B3","B3/S23"},
        {"/2/3","B2/S/3"},{"B2/S/C256","B2/S/256"},{"B2/S/G3","B2/S/3"},
        {"B3_S23_2","B3/S23"},{"B2cekin/S12","B2-a/S12"},{"B2nic/S","B2cin/S"},
        {"B4-qjrtwz/S","B4aceikny/S"},{"B2aceikn3/S","B23/S"},{"B2-3/S","B23/S"},
        {"B2-aceikn3/S","B3/S"},{"B2/S12v","B2/S12V"},{"/2/3h","B2/S/3H"},
        {"B/S","B/S"},{"B1c/S01e/256","B1c/S01e/256"}}) {
        const auto rule=bench::parse_rule(input);
        check(rule.ok&&rule.text==canonical,"canonical "+input+" -> "+rule.text+" "+rule.error);
        const auto roundtrip=bench::parse_rule(rule.text);
        check(roundtrip.ok&&roundtrip.transitions==rule.transitions&&roundtrip.states==rule.states,"round trip "+input);
    }
    for(const auto& input:{"", "B3/S23/3junk","B3/S23/3.5","B3/S23/Cabc","B3/S23/C0",
        "23/3/x","23/3/-1","B3/S23/","B3/S23/1","B3/S23/257","B3/S23/999999999999999999999",
        "B3/B2/S23","B3/S23/S2","B1a/S","B5z/S","B0/S23","B9/S23","B5/SV","B7/SH",
        "B2a/SV","B2-/SH","B8a/S","B3/S23:T30,20","MAPabc","R2,C2,S2,B3","B3/S23/3/4","B3//3"})
        check(!bench::parse_rule(input).ok,"reject "+std::string(input));
    check(!bench::parse_rule(std::string(2049,'3')).ok,"bounded input length");

    // Exhaust all 512 local configurations against separate geometric counts.
    for(const auto& name:{"B3/S23","B2/S12V","B2/S34H","B2-a/S12"}) {
        const auto rule=bench::parse_rule(name);check(rule.ok,"parse for exhaustive check");
        auto sim=bench::make_rule(name,5,0.f);auto* grid=dynamic_cast<bench::RuleSim*>(sim.get());
        for(unsigned bits=0;bits<512;++bits) {
            sim->editable()->fill(0);int neighbours=0;bool adjacent=false;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
                const bool live=(bits&(1u<<((y+1)*3+x+1)))!=0;
                sim->editable()->set((x+5)%5,(y+5)%5,live?1:0);
                if(!x&&!y)continue;
                bool counted=true;
                if(rule.neighbors()==4)counted=(x==0||y==0);
                if(rule.neighbors()==6)counted=!(x==-y&&x!=0);
                if(live&&counted)++neighbours;
                if(live)for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx)
                    if((xx||yy)&&std::abs(x-xx)+std::abs(y-yy)==1&&(bits&(1u<<((yy+1)*3+xx+1))))adjacent=true;
            }
            const bool live=(bits&16u)!=0;bool expected=false;
            if(std::string(name)=="B2-a/S12")expected=live?(neighbours==1||neighbours==2):(neighbours==2&&!adjacent);
            else expected=((live?rule.survive:rule.birth)&(1u<<neighbours))!=0;
            check(rule.transitions[bits]==expected,std::string(name)+" table "+std::to_string(bits));
            check(grid->rule(0,0)==expected,std::string(name)+" wrapped model "+std::to_string(bits));
        }
    }
    // Every published Hensel class has one label and covers a disjoint orbit.
    // Rotations/reflections are independently encoded as permutations of bits.
    const int rotate[]={2,5,8,1,4,7,0,3,6},mirror[]={2,1,0,5,4,3,8,7,6};
    auto permute=[](unsigned bits,const int* p){unsigned out=0;for(int i=0;i<9;++i)if(bits&(1u<<i))out|=1u<<p[i];return out;};
    for(int count=1;count<=7;++count) {
        const std::string letters=count==1||count==7?"ce":count==2||count==6?"aceikn":count==3||count==5?"aceijknqry":"aceijknqrtwyz";
        for(unsigned bits=0;bits<512;++bits)if(!(bits&16u)&&std::popcount(bits)==count) {
            int membership=0;
            for(char letter:letters) {
                const auto rule=bench::parse_rule("B"+std::to_string(count)+letter+"/S");
                check(rule.ok,"Hensel letter supported");membership+=rule.transitions[bits];
                check(rule.transitions[bits]==rule.transitions[permute(bits,rotate)]&&rule.transitions[bits]==rule.transitions[permute(bits,mirror)],"isotropic symmetry");
                const auto inverse=bench::parse_rule("B"+std::to_string(count)+"-"+letter+"/S");
                check(inverse.transitions[bits]!=rule.transitions[bits],"Hensel exclusion complement");
            }
            check(membership==1,"Hensel classes partition neighbours");
        }
    }
    auto longDecay=bench::make_rule("B2/S/256",5,0.f);
    check(longDecay&&longDecay->palette().size()==256,"full byte palette");
    longDecay->editable()->set(0,0,254);longDecay->editable()->set(1,0,255);
    longDecay->editable()->set(4,0,1);longDecay->editable()->set(4,1,2);
    longDecay->step();
    check(longDecay->field().at(0,0)==255&&longDecay->field().at(1,0)==0,"last decay state returns to dead");
    check(longDecay->field().at(3,0)==0,"refractory states do not count as alive");
    for(const auto& name:{"B3/S23","B2/S/4","B2-a/S12/256","B2/S12H"}) {
        auto original=bench::make_rule_workspace(name);original->on_knob("density",.17f);original->on_knob("seed",7);original->reset();
        bench::projects::Document saved;bench::projects::capture_settings(*original,saved);std::string error;
        auto restored=bench::make_rule_workspace(name);check(bench::projects::apply_settings(*restored,saved,error),"restore setup");
        check(original->field().cells==restored->field().cells,"restore exact starting field "+std::string(name));
    }
    auto identical=[&](const char* text,bench::SimPtr reference,float density,std::uint64_t seed) {
        auto model=bench::make_rule(text,128,density,seed);
        for(int i=0;i<200;++i){model->step();reference->step();}
        check(model->field().cells==reference->field().cells,std::string(text)+" agrees with independent engine after 200 generations");
    };
    identical("B3/S23",bench::make_life(128),.28f,0xC0FFEEull);
    identical("B36/S23",bench::make_highlife(128),.28f,0xC0FFEEull);
    identical("B2/S",bench::make_seeds(128),.004f,0xC0FFEEull);
    identical("B3678/S34678",bench::make_day_night(128),.5f,0xC0FFEEull);
    identical("B2/S/3",bench::make_brains_brain(128),.12f,0xB2A1ull);
    std::cout<<checks<<" rulestring checks, "<<failed<<" failed\n";
    return failed?1:0;
}

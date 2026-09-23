#include "sims/sorting.hpp"
#include "render/png.hpp"
#include <cstdio>
#include <chrono>
#include <set>

int main(int argc,char** argv) {
    using namespace bench::sorting;
    int checks=0,failed=0;
    std::string context;
    auto check=[&](bool ok,const char* message){
        ++checks;
        if(!ok){++failed;if(failed<=60)std::printf("FAIL %s  [%s]\n",message,context.c_str());}
    };
    auto isGravity=[](Algorithm a){return a==Algorithm::Gravity;};
    auto run=[&](std::vector<Item> input,Algorithm algorithm,bool descending) {
        context=std::string(names[int(algorithm)])+(descending?", descending, n=":", ascending, n=")+std::to_string(input.size());
        Engine e;e.reset(input,algorithm,descending);
        std::uint64_t budget=4*input.size()*input.size()+8*input.size()+100;
        // Gravity drops one rod per unit of key range, whatever n is.
        if(isGravity(algorithm)&&!input.empty()) {
            const auto [lo,hi]=std::minmax_element(input.begin(),input.end(),[](const Item& a,const Item& b){return a.value<b.value;});
            budget+=std::uint64_t(static_cast<long long>(hi->value)-lo->value);
        }
        // One event per call means exactly one of the kind counters moves, by one.
        auto tally=[](const Engine& x){return x.comparisons+x.swaps+x.copies+x.reads+x.drops;};
        bool single=true;
        while(!e.done && budget--) {
            const auto previous=tally(e);
            const bool took=e.next();
            if(took && tally(e)!=previous+1) {single=false;break;}
        }
        check(single,"each call makes exactly one comparison, swap, copy, read or drop");
        check(e.done && e.ordered(),"algorithm finishes in the requested order");
        if(isGravity(algorithm)) {
            // Beads rebuild the values; the multiset is what must survive.
            std::vector<int> before,after;
            for(const auto& it:input)before.push_back(it.value);
            for(const auto& it:e.values)after.push_back(it.value);
            std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
            check(before==after,"gravity preserves every value");
            check(e.comparisons==0&&e.swaps==0&&e.copies==0,"gravity neither compares, swaps nor copies");
            long long laid=0;long long lowest=input.empty()?0:input[0].value,highest=lowest;
            for(const auto& it:input){lowest=std::min<long long>(lowest,it.value);highest=std::max<long long>(highest,it.value);}
            for(const auto& it:input)laid+=it.value-lowest;
            if(input.size()>=2&&highest>lowest) {
                check(e.auxiliaryWrites==std::uint64_t(laid)+e.writes,"abacus writes are the beads laid plus one per bead change");
                check(e.reads==2*input.size()&&e.drops==std::uint64_t(highest-lowest),"gravity reads every key twice and drops every rod once");
                check(std::all_of(e.values.begin(),e.values.end(),[](const Item& it){return it.identity==-1;}),"rebuilt rows carry no item identity");
            }
        } else {
            auto originals=input,output=e.values;
            auto identity=[](const Item& a,const Item& b){return a.identity<b.identity;};
            std::sort(originals.begin(),originals.end(),identity);std::sort(output.begin(),output.end(),identity);
            check(originals==output,"all values and identities preserved");
            check(e.copies==(e.writes-2*e.swaps)+e.auxiliaryWrites,"each copy is exactly one write");
            check(e.drops==0,"only gravity drops rods");
        }
        check(e.events==e.comparisons+e.swaps+e.copies+e.reads+e.drops,"event accounting");
        check(e.writes>=2*e.swaps,"swap counts two main-array writes");
        if(algorithm==Algorithm::RadixLSD||algorithm==Algorithm::RadixMSD) check(e.comparisons==0&&e.swaps==0,"radix never compares two keys");
        else if(!isGravity(algorithm)) check(e.reads==0,"comparison sorts make no bare key reads");
        if(stable(algorithm)) {
            bool ok=true;
            for(std::size_t k=1;k<e.values.size();++k)if(e.values[k-1].value==e.values[k].value && e.values[k-1].identity>e.values[k].identity)ok=false;
            check(ok,"stable methods preserve duplicate order");
        }
        if(algorithm==Algorithm::Cycle) {
            std::set<int> keys;for(const auto& it:input)keys.insert(it.value);
            if(keys.size()==input.size()) {
                auto sorted=input;
                std::sort(sorted.begin(),sorted.end(),[&](const Item& a,const Item& b){return descending?a.value>b.value:a.value<b.value;});
                std::uint64_t misplaced=0;for(std::size_t k=0;k<input.size();++k)misplaced+=input[k].value!=sorted[k].value;
                check(e.writes==misplaced,"cycle sort writes each misplaced item exactly once");
            }
        }
        check(!e.holding,"nothing is left held");
        check(e.meanDisplacement()==0,"a finished sort has no displacement");
        auto before=e.values;auto events=e.events;e.advance(100);
        check(before==e.values&&events==e.events,"completion is idempotent");
        return e;
    };
    for(int a=0;a<algorithmCount;++a)for(bool descending:{false,true}) {
        for(int size:{0,1,2,3,7,16,63,128,512})for(int pattern=0;pattern<5;++pattern)
            run(dataset(size,pattern,42),Algorithm(a),descending);
        run({{0,0},{-3,1},{4,2},{-3,3},{0,4},{-8,5}},Algorithm(a),descending);
        run({{2,0},{2,1},{2,2},{2,3}},Algorithm(a),descending);
        run({{1000,0},{-1000,1},{7,2},{999,3},{-999,4}},Algorithm(a),descending);
        // The extremes of int, where any plain int subtraction of keys overflows.
        // Gravity would need 2^32 beads a row, and refuses: tested below.
        if(!isGravity(Algorithm(a)))
            run({{INT_MIN,0},{INT_MAX,1},{0,2},{-1,3},{INT_MAX,4},{INT_MIN+1,5}},Algorithm(a),descending);
        std::vector<int> permutation={0,1,2,3,4,5};
        do {
            std::vector<Item> items;for(int v:permutation)items.push_back({v,int(items.size())});
            run(items,Algorithm(a),descending);
        } while(std::next_permutation(permutation.begin(),permutation.end()));
        // Every input of 0s and 1s up to ten items: duplicates everywhere.
        for(int size=0;size<=10;++size)for(int bits=0;bits<(1<<size);++bits) {
            std::vector<Item> items;for(int k=0;k<size;++k)items.push_back({(bits>>k)&1,k});
            run(items,Algorithm(a),descending);
        }
    }
    // A comparison network is correct for n if it sorts every 0-1 input of length n,
    // PROVIDED the comparisons do not depend on the data. Both are checked, to 16 items.
    int networks=0;
    for(int index=0;index<algorithmCount;++index)if(network(Algorithm(index)))for(bool descending:{false,true})for(int size=0;size<=16;++size) {
        const Algorithm a=Algorithm(index);
        networks+=size==0&&!descending;
        context=std::string(names[int(a)])+" 0-1 principle, n="+std::to_string(size)+(descending?" descending":"");
        std::uint64_t reference=0,referenceCount=0;
        bool sortedAll=true,sameNetwork=true;
        for(int bits=0;bits<(1<<size);++bits) {
            std::vector<Item> items;for(int k=0;k<size;++k)items.push_back({(bits>>k)&1,k});
            Engine e;e.reset(items,a,descending);
            std::uint64_t hash=1469598103934665603ull,count=0;
            while(e.next()) if(e.event==Engine::Event::Compare) {
                hash=(hash^std::uint64_t(e.activeA*1024+e.activeB))*1099511628211ull;++count;
            }
            sortedAll=sortedAll&&e.ordered();
            if(bits==0){reference=hash;referenceCount=count;}
            else sameNetwork=sameNetwork&&hash==reference&&count==referenceCount;
        }
        check(sortedAll,"network sorts every 0-1 input");
        check(sameNetwork,"network compares the same positions whatever the data");
    }
    context="networks";
    check(networks==2,"both sorting networks are held to the 0-1 principle");
    context="fixtures";
    auto bubble=run({{3,0},{2,1},{1,2}},Algorithm::Bubble,false);
    check(bubble.comparisons==3&&bubble.swaps==3&&bubble.writes==6,"bubble known reverse fixture");
    auto insertion=run(dataset(64,4,1),Algorithm::Insertion,false);
    check(insertion.comparisons==63&&insertion.swaps==0,"ordered insertion is linear");
    auto selection=run(dataset(64,0,1),Algorithm::Selection,false);
    check(selection.comparisons==64*63/2,"selection exact comparison count");
    auto merge=run({{2,0},{1,1}},Algorithm::Merge,false);
    check(merge.comparisons==1&&merge.swaps==0&&merge.writes==2&&merge.auxiliaryWrites==2&&merge.copies==4,"merge exact copy counts");
    auto gnome=run({{3,0},{2,1},{1,2}},Algorithm::Gnome,false);
    check(gnome.comparisons==6&&gnome.swaps==3,"gnome known reverse fixture");
    auto lsdSmall=run({{3,0},{1,1},{2,2}},Algorithm::RadixLSD,false);
    check(lsdSmall.reads==6&&lsdSmall.auxiliaryWrites==3&&lsdSmall.writes==3&&lsdSmall.events==12,"radix LSD reads the range, then one digit pass");
    auto lsd=run(dataset(64,0,1),Algorithm::RadixLSD,false);
    check(lsd.reads==192&&lsd.auxiliaryWrites==128&&lsd.writes==128,"radix LSD makes two base-10 passes over keys 0..63");
    auto msd=run(dataset(64,0,1),Algorithm::RadixMSD,false);
    check(msd.reads==192&&msd.auxiliaryWrites==128&&msd.writes==128,"radix MSD distributes the tens, then each bucket of ten on the units");
    auto gravity=run({{3,0},{1,1},{2,2}},Algorithm::Gravity,false);
    check(gravity.reads==6&&gravity.drops==2&&gravity.writes==4&&gravity.auxiliaryWrites==7&&gravity.events==8,"gravity known fixture");
    auto pancake=run({{2,0},{1,1},{2,2}},Algorithm::Pancake,false);
    check(pancake.swaps==1,"pancake leaves an equal largest key already at the end in place");
    {
        // Radix digits are the key's own digits, whatever the smallest key is.
        Engine e;e.reset({{37,0},{5,1}},Algorithm::RadixLSD,false);
        e.next();e.next();e.next();
        check(e.event==Engine::Event::Read&&e.detail()=="VALUE 37 - DIGIT 7 AT PLACE 1","the radix readout names the value's own units digit");
        context="fixtures, radix with a negative key";
        e.reset({{-37,0},{5,1}},Algorithm::RadixMSD,false);
        while(e.next()){}
        // Shifted to 0 and 42: one pass on the tens, and neither bucket holds two.
        check(e.ordered()&&e.reads==4&&e.auxiliaryWrites==2&&e.writes==2,"negative keys are shifted up to zero");
        context="fixtures";
    }
    {
        // Gravity's rows pass through values the input never had. They are not home.
        Engine e;e.reset({{3,0},{1,1}},Algorithm::Gravity,false);
        while(!e.done&&e.drops<1)e.next();
        check(!e.done&&e.values[0].value==2&&e.values[1].value==2&&e.meanDisplacement()>0,"an unfinished gravity sort never reads as fully home");
    }
    for(int k=1;k<=9;++k) {
        const int size=1<<k;
        context="network sizes, n="+std::to_string(size);
        auto bitonic=run(dataset(size,0,3),Algorithm::Bitonic,false);
        check(bitonic.comparisons==std::uint64_t(k*(k+1)/2)<<(k-1),"bitonic uses k(k+1)/2 * 2^(k-1) comparators");
        auto batcher=run(dataset(size,0,3),Algorithm::OddEvenMerge,false);
        const std::uint64_t expected=k==1?1:std::uint64_t(k*k-k+4)*(std::uint64_t(1)<<(k-2))-1;
        check(batcher.comparisons==expected,"odd-even merge uses (k^2-k+4) * 2^(k-2) - 1 comparators");
    }
    context="displacement";
    {
        Engine e;e.reset({{4,0},{3,1},{2,2},{1,3}},Algorithm::Bubble,false);
        check(e.displacement(0)==3&&e.displacement(1)==1&&e.displacement(2)==1&&e.displacement(3)==3,"displacement counts positions from home");
        check(std::abs(e.meanDisplacement()-8.0/4/3)<1e-12,"mean displacement is a fraction of the array");
        e.reset({{1,0},{1,1},{2,2},{1,3}},Algorithm::Bubble,false);
        check(e.displacement(3)==1&&e.displacement(0)==0&&e.displacement(1)==0,"a duplicate is home anywhere in its run");
    }
    check(dataset(128,0,7)==dataset(128,0,7)&&dataset(128,0,7)!=dataset(128,0,8),"seeds reproduce input");
    {
        Engine big;
        std::vector<Item> wide={{0,0},{1<<30,1}};
        big.reset(wide,Algorithm::Gravity,false);
        bool refused=false;
        try{while(big.next()){}}catch(const std::length_error&){refused=true;}
        check(refused,"gravity refuses an abacus too large to hold");
    }

    context="simulation";
    // Checks on pictures must look at the drawing area alone: the title, the view's
    // name, the counters and the readout all change by themselves, so a whole-image
    // comparison passes even when a view draws nothing at all.
    using Picture=std::vector<std::uint8_t>;
    auto area=[](const Picture& rgba,bool onlyInk) {
        Picture out;
        const int W=Canvas::W;
        // The drawing area below the "drag to orbit" line and above the labels.
        for(int y=172;y<540;++y)for(int x=24;x<1096;++x) {
            const auto p=(std::size_t(y)*W+x)*4;
            const std::uint8_t r=rgba[p],g=rgba[p+1],b=rgba[p+2];
            // Background, grid lines, the baseline, and the circle's rim and sphere's guides.
            const bool furniture=(r==17&&g==26&&b==38)||(r==33&&g==49&&b==65)||(r==66&&g==87&&b==109)||(r==46&&g==66&&b==86)||(r==42&&g==61&&b==78);
            if(onlyInk&&furniture) {out.push_back(0);out.push_back(0);out.push_back(0);continue;}
            out.push_back(r);out.push_back(g);out.push_back(b);
        }
        return out;
    };
    auto drawnPixels=[&](const Picture& rgba) {
        const Picture ink=area(rgba,true);
        std::size_t count=0;
        for(std::size_t p=0;p+2<ink.size();p+=3)count+=ink[p]||ink[p+1]||ink[p+2];
        return count;
    };
    bench::SortingSim two,three(bench::SortingSim::Columns);
    check(two.engine().values==three.engine().values,"2D and 3D start on identical data");
    for(int algorithm=0;algorithm<algorithmCount;++algorithm) {
        context=std::string("simulation, ")+names[algorithm];
        two.on_knob("algorithm",float(algorithm));three.on_knob("algorithm",float(algorithm));
        two.reset();three.reset();two.on_knob("speed",1);three.on_knob("speed",512);
        int guard=0;
        while(!two.finished()&&++guard<4000000)two.step();
        guard=0;
        while(!three.finished()&&++guard<4000000)three.step();
        check(two.finished()&&three.finished(),"both finish within the step budget");
        check(two.engine().values==three.engine().values&&two.generation()==three.generation()&&two.engine().comparisons==three.engine().comparisons,"batch speed and view preserve outcome and counts");
        // Every view renders every algorithm mid-sort, highlights included.
        bench::SortingSim sim;sim.on_knob("algorithm",float(algorithm));sim.reset();
        for(int k=0;k<9;++k)sim.step();
        std::vector<Picture> pictures;
        bool drawn=true;
        for(int view=0;view<4;++view) {
            sim.on_knob("view",float(view));
            const Picture& shot=sim.surface()->rgba;
            pictures.push_back(area(shot,false));
            // At least a hundred pixels of the view's own drawing, not furniture.
            drawn=drawn&&drawnPixels(shot)>100;
        }
        bool distinct=true;
        for(std::size_t a=0;a<pictures.size();++a)for(std::size_t b=a+1;b<pictures.size();++b)distinct=distinct&&pictures[a]!=pictures[b];
        check(distinct&&drawn&&!sim.finished(),"all four views draw the array mid-sort, and draw it differently");
    }
    context="simulation";
    two.on_knob("algorithm",5);three.on_knob("algorithm",5);
    three.reset();three.step();auto values=three.engine().values;auto events=three.generation();
    auto image=three.surface()->rgba;
    three.camera_orbit(30,18);check(image!=three.surface()->rgba,"orbit changes actual 3D render");
    three.camera_ortho(true);check(three.camera_is_ortho(),"orthographic projection supported");
    for(auto view:{bench::Sim::StdView::Front,bench::Sim::StdView::Back,bench::Sim::StdView::Top,bench::Sim::StdView::Bottom,bench::Sim::StdView::Left,bench::Sim::StdView::Right,bench::Sim::StdView::Iso}) {
        check(three.camera_view(view),"standard camera view");three.surface();
    }
    three.on_knob("view",0);check(!three.has_camera()&&events==three.generation()&&values==three.engine().values,"view switch preserves progress");
    three.on_knob("view",2);check(!three.has_camera()&&!three.camera_orbit(10,10)&&values==three.engine().values,"the disparity circle is flat and keeps progress");
    three.on_knob("view",3);
    // The balls must move, not just the guide circles.
    const Picture balls=area(three.surface()->rgba,true);
    check(three.has_camera()&&three.camera_orbit(40,12)&&balls!=area(three.surface()->rgba,true)&&values==three.engine().values,"the disparity sphere orbits its items and keeps progress");
    three.on_knob("view",1);check(three.has_camera(),"3D controls return");
    three.reset();values=three.engine().values;three.poke(.5f,.5f);
    check(three.generation()==0&&values!=three.engine().values,"new data increments the seed and resets counts");
    values=three.engine().values;three.step();three.reset();check(values==three.engine().values,"reset replays same input");
    {
        bench::SortingSim sphere(bench::SortingSim::Sphere);
        check(sphere.about().title=="Sorting lab: disparity sphere"&&sphere.has_camera()&&sphere.view()==3,"the sphere entry opens in the disparity sphere");
        // Sorted input: every item is home, so the sphere is complete before any work.
        sphere.on_knob("pattern",4);sphere.reset();
        const auto m=sphere.metrics();
        check(m.size()>1&&m[1].name=="mean displacement"&&m[1].value==0,"sorted input has no displacement");
        sphere.on_knob("pattern",1);sphere.reset();
        check(sphere.metrics()[1].value>.4,"reversed input is far from home");
    }
    {
        // Gravity in the bar view: rows keep their bars until their beads are laid,
        // and then beads hang in the air above the rods that have already dropped.
        bench::SortingSim sim;sim.on_knob("algorithm",float(int(Algorithm::Gravity)));sim.on_knob("size",16);sim.on_knob("speed",1);sim.reset();
        const std::size_t bars=drawnPixels(sim.surface()->rgba);
        while(sim.engine().beadsLaid()<8)sim.step();
        check(sim.engine().abacusShown()&&sim.engine().beadsLaid()==8,"beads are laid one row at a time");
        const std::size_t half=drawnPixels(sim.surface()->rgba);
        check(half>bars*9/10,"rows waiting for their beads still show their value");
        while(sim.engine().event!=Engine::Event::Drop&&!sim.finished())sim.step();
        check(sim.engine().event==Engine::Event::Drop,"rods then drop");
        // A bead hanging above a gap of at least one whole cell, in some column.
        bool floating=false;
        const int cell=356/16;
        for(int k=0;k<40&&!floating&&!sim.finished();++k) {
            const auto& shot=sim.surface()->rgba;
            for(int x=48;x<1080&&!floating;x+=65) {
                int gap=0;bool below=false;
                for(int y=530;y>=174;--y) {
                    const auto p=(std::size_t(y)*Canvas::W+x)*4;
                    const bool ink=!(shot[p]==17&&shot[p+1]==26&&shot[p+2]==38)&&!(shot[p]==33&&shot[p+1]==49&&shot[p+2]==65);
                    if(ink) {if(below&&gap>=cell-2)floating=true;below=true;gap=0;}
                    else if(below)++gap;
                }
            }
            if(!floating)sim.step();
        }
        check(floating,"beads hang above the rods that have dropped");
        int guard=0;
        while(!sim.finished()&&++guard<10000)sim.step();
        check(sim.finished()&&sim.engine().ordered()&&sim.engine().meanDisplacement()==0,"the beads finish sorted");
    }
    if(argc>1) {
        for(int view=0;view<4;++view) {
            bench::SortingSim sim(view);sim.on_knob("speed",1);
            for(int k=0;k<18;++k)sim.step();
            auto s=sim.surface();std::string path=std::string(argv[1])+"-view"+std::to_string(view)+".png";
            check(bench::write_png(path.c_str(),s->w,s->h,s->rgba.data()),"write preview");
        }
    }
    std::printf("%d sorting checks, %d failed\n",checks,failed);
    return failed?1:0;
}

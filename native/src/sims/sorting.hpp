#pragma once
#include "../sim.hpp"
#include "../learn/sorting.hpp"
#include "../render/sorting_canvas.hpp"
#include <cmath>
#include <iterator>

namespace bench {
class SortingSim final : public Sim {
public:
    enum View { Bars, Columns, Circle, Sphere };
    // `view` is the view the lab opens in; every entry can switch to any view.
    explicit SortingSim(int view=Bars) {
        view=std::clamp(view,0,3);
        static constexpr const char* titles[]={"Sorting lab: 2D","Sorting lab: 3D","Sorting lab: disparity circle","Sorting lab: disparity sphere"};
        about_={titles[view],"Algorithms","Original incremental implementations of classic sorting algorithms",
            "Sedgewick and Wayne, Algorithms, 4th edition, chapter 2. https://algs4.cs.princeton.edu/20sorting/ "
            "Bitonic sorting for any n after H. W. Lang; odd-even merge sort after K. E. Batcher (1968).",
            Replication::No,"Sorting rearranges existing values; nothing reproduces.",
            "Watch eighteen real sorting algorithms rearrange the same seeded input: exchange sorts, selection and insertion families, "
            "merge, quick and heap, two sorting networks, two radix sorts and a bead-by-bead gravity sort. "
            "Amber marks comparisons, coral swaps and copies, violet key reads, and green a completed sort. "
            "3D columns follow the array across each row, from the front row to the back. "
            "The disparity circle places items clockwise from the top and the disparity sphere along a spiral from its top to its bottom; "
            "each item's distance from the centre shrinks with how far it is from where it belongs, so a finished sort is a full disc or sphere. "
            "The strip below shows the array, or the working buffer of Merge and both radix sorts. Change View while running without restarting. "
            "One event is one key comparison, swap, item copy, key read, or the drop of one abacus rod. "
            "A swap is two main-array writes; buffer copies and abacus bead changes have a separate count. "
            "Gravity rebuilds each value from beads, so items are not carried and stability does not apply. "
            "Adjacent order measures neighbouring pairs, not percent complete. Counters exclude input generation and drawing."};
        std::vector<std::string> methods(std::begin(sorting::names),std::end(sorting::names));
        knobs_={
            {"algorithm","Algorithm",0,float(sorting::algorithmCount-1),5,1,methods,true,"Apply to restart this algorithm on the same seed and input."},
            {"size","Items",16,512,64,1,{},true,"16 to 512 values. Every value is shown in every view."},
            {"pattern","Starting data",0,4,0,1,{"Shuffled","Reversed","Nearly sorted","Few unique","Sorted"},true,"Starting order is defined low to high, independently of the chosen sorting direction."},
            {"speed","Events per step",1,512,8,1,{},false,"Use 1 for a single comparison, swap, copy, read or rod drop per Step. Higher values batch the same events.",true},
            {"view","View",0,3,float(view),1,{"2D bars","3D columns","Disparity circle","Disparity sphere"},false,"Switch views at any time. The current sort and counters are preserved.",true},
            {"direction","Sort order",0,1,0,1,{"Ascending","Descending"},true,"The order this algorithm should produce."},
            {"seed","Data seed",1,9999,1,1,{},true,"Use the same seed, size and pattern to compare algorithms on identical input."}
        };
        pal_={{{17,26,38},"background"},{{103,160,225},"values"},{{248,196,103},"comparison"},{{243,125,116},"swap or copy"},
              {{113,221,180},"sorted"},{{176,150,247},"key read"}};
        field_=Field(128,72);camera_home();reset();
    }
    const Provenance& about() const override {return about_;}
    const std::vector<Swatch>& palette() const override {return pal_;}
    const Field& field() const override {return field_;}
    const Surface* surface() const override {if(dirty_){render();dirty_=false;}return &canvas_.image;}
    std::vector<Knob>& knobs() override {return knobs_;}
    void on_knob(const std::string& key,float v) override {
        for(auto& k:knobs_)if(k.key==key && std::isfinite(v))k.value=k.quantised(v);
        dirty_=true;
    }
    void reset() override {
        engine_.reset(sorting::dataset(int(value("size")),int(value("pattern")),unsigned(value("seed"))),
            sorting::Algorithm(int(value("algorithm"))),value("direction")>0);
        publish();
    }
    void step() override {if(!engine_.done){engine_.advance(int(value("speed")));publish();}}
    std::uint64_t generation() const override {return engine_.events;}
    const sorting::Engine& engine() const {return engine_;}
    bool finished() const {return engine_.done;}
    int view() const {return int(value("view"));}
    bool poke(float,float) override {
        for(auto& k:knobs_)if(k.key=="seed")k.value=k.value>=k.max?k.min:k.value+1;
        reset();return true;
    }
    std::string subtitle() const override {
        return std::string(sorting::names[int(engine_.algorithm)])+" | "+std::to_string(engine_.values.size())+" items | "+
            (engine_.done?"Sorted - Reset to replay":engine_.events?engine_.eventName():"Ready - Run or Step");
    }
    std::vector<Metric> metrics() const override {
        return {{"adjacent order",engine_.adjacentOrder(),1,Metric::Higher},
            {"mean displacement",engine_.meanDisplacement(),1,Metric::Lower},
            {"comparisons",double(engine_.comparisons),0,Metric::Neither},{"swaps",double(engine_.swaps),0,Metric::Neither},
            {"array writes",double(engine_.writes),0,Metric::Neither},{"buffer writes",double(engine_.auxiliaryWrites),0,Metric::Neither},
            {"key reads",double(engine_.reads),0,Metric::Neither},
            {"events",double(engine_.events),0,Metric::Neither},{"items",double(engine_.values.size()),0,Metric::Neither},
            {"complete",engine_.done?1.:0.,1,Metric::Neither}};
    }
    bool has_camera() const override {return view()==Columns||view()==Sphere;}
    bool camera_orbit(float dx,float dy) override {
        if(!has_camera())return false;
        cam_.yaw-=dx*.008f;cam_.pitch-=dy*.008f;cam_.clampPitch();dirty_=true;return true;
    }
    bool camera_dolly(float steps,float,float) override {
        if(!has_camera())return false;
        cam_.distance=std::clamp(cam_.distance*std::pow(.9f,steps),2.8f,10.f);dirty_=true;return true;
    }
    bool camera_view(StdView v) override {
        if(!has_camera())return false;
        switch(v) {
            case StdView::Front:cam_.yaw=0;cam_.pitch=0;break;
            case StdView::Back:cam_.yaw=3.14159265f;cam_.pitch=0;break;
            case StdView::Left:cam_.yaw=1.5707963f;cam_.pitch=0;break;
            case StdView::Right:cam_.yaw=-1.5707963f;cam_.pitch=0;break;
            case StdView::Top:cam_.yaw=0;cam_.pitch=1.5533f;break;
            case StdView::Bottom:cam_.yaw=0;cam_.pitch=-1.5533f;break;
            case StdView::Iso:cam_.yaw=.65f;cam_.pitch=.65f;break;
        }
        dirty_=true;return true;
    }
    bool camera_fit() override {cam_.distance=4.7f;dirty_=true;return has_camera();}
    void camera_home() override {cam_=Camera{};cam_.yaw=.5f;cam_.pitch=.6f;cam_.distance=4.7f;dirty_=true;}
    bool camera_ortho(bool on) override {cam_.ortho=on;dirty_=true;return has_camera();}
    bool camera_is_ortho() const override {return cam_.ortho;}
private:
    Provenance about_;
    std::vector<Knob> knobs_;
    std::vector<Swatch> pal_;
    Field field_;
    sorting::Engine engine_;
    mutable sorting::Canvas canvas_;
    mutable bool dirty_=true;
    mutable std::vector<float> polarAngle_,polarDistance_;
    Camera cam_;
    static constexpr int circleX=560,circleY=349,circleRadius=190;
    float value(const char* key) const {for(const auto& k:knobs_)if(k.key==key)return k.value;return 0;}
    // Bar heights are scaled to the largest key of the finished array, which
    // every intermediate array shares; Gravity's rows pass through values in between.
    int scale() const {return std::max(1,engine_.maxKey());}
    // The highlight for position i, if the latest event touched it.
    const Rgb* mark(int i,bool auxiliary=false) const {
        using E=sorting::Engine::Event;
        if(engine_.done || auxiliary!=engine_.auxiliaryEvent || (i!=engine_.activeA&&i!=engine_.activeB))return nullptr;
        return &pal_[engine_.event==E::Compare?2:engine_.event==E::Read?5:3].colour;
    }
    Rgb colour(int i,bool auxiliary=false) const {
        if(engine_.done)return pal_[4].colour;
        if(const Rgb* m=mark(i,auxiliary))return *m;
        const auto& a=auxiliary?engine_.auxiliary():engine_.values;
        const float t=std::clamp(float(a[std::size_t(i)].value)/float(scale()),0.f,1.f);
        return {std::uint8_t(72+50*t),std::uint8_t(114+75*t),std::uint8_t(175+55*t)};
    }
    // Hue by key, from red at the lowest to magenta at the highest, for the disparity views.
    // Kept when the sort completes: the finished colour wheel is the result.
    Rgb hue(int i) const {
        if(const Rgb* m=mark(i))return *m;
        const int lo=engine_.minKey(),hi=engine_.maxKey();
        const float t=hi>lo?std::clamp(float(double(engine_.values[std::size_t(i)].value-static_cast<long long>(lo))/double(static_cast<long long>(hi)-lo)),0.f,1.f):0.f;
        const float h=t*5,f=h-std::floor(h),v=.95f,s=.72f;
        const float p=v*(1-s),q=v*(1-s*f),u=v*(1-s*(1-f));
        float r=v,g=u,b=p;
        switch(int(h)) {case 0:break;case 1:r=q;g=v;b=p;break;case 2:r=p;g=v;b=u;break;
            case 3:r=p;g=q;b=v;break;case 4:r=u;g=p;b=v;break;default:r=v;g=p;b=v;break;}
        return {std::uint8_t(r*255),std::uint8_t(g*255),std::uint8_t(b*255)};
    }
    void publish() {
        using E=sorting::Engine::Event;
        field_.fill(0);int n=int(engine_.values.size()),m=scale();
        for(int x=0;x<field_.w && n>0;++x) {
            int i=x*n/field_.w,h=engine_.values[std::size_t(i)].value*(field_.h-1)/m;
            std::uint8_t c=1;
            if(engine_.done)c=4;
            else if(!engine_.auxiliaryEvent && (i==engine_.activeA||i==engine_.activeB))
                c=engine_.event==E::Compare?2:engine_.event==E::Read?5:3;
            for(int y=field_.h-1;y>=field_.h-h;--y)field_.set(x,y,c);
        }
        dirty_=true;
    }
    void bars(int x,int y,int w,int h,bool auxiliary=false) const {
        const auto& a=auxiliary?engine_.auxiliary():engine_.values;
        int n=int(a.size()),m=scale();if(!n)return;
        for(int i=0;i<n;++i) {
            int left=x+i*w/n,right=x+(i+1)*w/n,height=std::max(1,a[std::size_t(i)].value*h/m);
            canvas_.rect(left,y+h-height,std::max(1,right-left-(n<180?2:0)),height,colour(i,auxiliary));
        }
    }
    // Gravity in the bar view: each row is its beads, one cell per rod, over a
    // solid base for the lowest key. Beads that have fallen leave gaps.
    void beadBars(int x,int y,int w,int h) const {
        const auto& e=engine_;
        const int n=int(e.values.size()),m=scale();
        const long long base=std::max(0LL,e.beadBase());
        for(int r=0;r<n;++r) {
            const int left=x+r*w/n,right=x+(r+1)*w/n,width=std::max(1,right-left-(n<180?2:0));
            const Rgb ink=colour(r);
            if(r>=e.beadsLaid()) {
                // Not laid yet: the row is still just its key.
                const int height=std::max(1,e.values[std::size_t(r)].value*h/m);
                canvas_.rect(left,y+h-height,width,height,ink);
                continue;
            }
            const int baseHeight=int(base*h/m);
            if(baseHeight>0)canvas_.rect(left,y+h-baseHeight,width,baseHeight,ink);
            for(int l=0;l<e.beadLevels();++l) if(e.bead(r,l)) {
                const int bottom=int((base+l)*h/m),top=int((base+l+1)*h/m);
                const int cell=std::max(1,top-bottom-(top-bottom>=4?1:0));
                canvas_.rect(left,y+h-bottom-cell,width,cell,l==e.activeLevel?pal_[3].colour:ink);
            }
        }
    }
    void circle() const {
        auto& c=canvas_;const auto& e=engine_;
        const int n=int(e.values.size());if(!n)return;
        constexpr int sampling=sorting::Canvas::samples;
        const int radius=circleRadius*sampling,side=2*radius+1;
        if(polarAngle_.empty()) {
            polarAngle_.resize(std::size_t(side)*side);polarDistance_.resize(polarAngle_.size());
            for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx) {
                const auto at=std::size_t(dy+radius)*side+std::size_t(dx+radius);
                // Clockwise from the top of the screen.
                float turn=std::atan2(float(dx)+.5f,-(float(dy)+.5f))/6.2831853f;
                polarAngle_[at]=turn<0?turn+1:turn;
                polarDistance_[at]=std::sqrt((dx+.5f)*(dx+.5f)+(dy+.5f)*(dy+.5f))/float(radius);
            }
        }
        std::vector<float> reach(static_cast<std::size_t>(n));std::vector<Rgb> ink(static_cast<std::size_t>(n));
        for(int i=0;i<n;++i) {
            // Around a circle the array wraps, so distance is measured both ways round.
            const int d=e.circularDisplacement(i);
            reach[std::size_t(i)]=.06f+.94f*std::max(0.f,1-float(d)/float(std::max(1,n/2)));
            ink[std::size_t(i)]=hue(i);
        }
        const Rgb rim{46,66,86};
        for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx) {
            const auto at=std::size_t(dy+radius)*side+std::size_t(dx+radius);
            const float r=polarDistance_[at];
            if(r>1)continue;
            const int i=std::min(n-1,int(polarAngle_[at]*float(n)));
            if(r<=reach[std::size_t(i)])c.samplePixel(circleX*sampling+dx,circleY*sampling+dy,ink[std::size_t(i)]);
            else if(r>=1-1.5f/float(circleRadius))c.samplePixel(circleX*sampling+dx,circleY*sampling+dy,rim);
        }
        // A wedge narrower than two pixels at its tip may cover no pixel centre at
        // all, so its item is marked there with a dot and every item stays visible.
        for(int i=0;i<n;++i) {
            const float tip=reach[std::size_t(i)]*float(circleRadius);
            if(6.2831853f*tip/float(n)>=2)continue;
            const float turn=6.2831853f*(float(i)+.5f)/float(n);
            const int px=circleX+int(std::lround(std::sin(turn)*(tip-1))),py=circleY-int(std::lround(std::cos(turn)*(tip-1)));
            c.rect(px-1,py-1,2,2,ink[std::size_t(i)]);
        }
        c.text(40,545,"CLOCKWISE FROM THE TOP - THE RIM MEANS IN PLACE",{143,164,185});
    }
    void sphere() const {
        auto& c=canvas_;const auto& e=engine_;
        c.camera(cam_);
        const float radius=1.15f;
        const Rgb guide{46,66,86};
        constexpr int segments=96;
        for(int s=0;s<segments;++s) {
            const float a=6.2831853f*float(s)/segments,b=6.2831853f*float(s+1)/segments;
            c.line({radius*std::cos(a),0,radius*std::sin(a)},{radius*std::cos(b),0,radius*std::sin(b)},guide);
            c.line({radius*std::cos(a),radius*std::sin(a),0},{radius*std::cos(b),radius*std::sin(b),0},guide);
            c.line({0,radius*std::sin(a),radius*std::cos(a)},{0,radius*std::sin(b),radius*std::cos(b)},guide);
        }
        const int n=int(e.values.size());
        // About a third of the spacing between neighbours on a full sphere.
        const float size=std::clamp(radius*std::sqrt(4/float(std::max(n,1)))*.3f,.02f,.075f);
        for(int i=0;i<n;++i) {
            // A golden-angle spiral from the top pole to the bottom one spaces every item evenly.
            const float y=1-2*(float(i)+.5f)/float(n),ring=std::sqrt(std::max(0.f,1-y*y)),turn=float(i)*2.3999632f;
            const float reach=.06f+.94f*(1-float(e.displacement(i))/float(std::max(1,n-1)));
            const float r=radius*reach;
            c.ball({r*ring*std::cos(turn),r*y,r*ring*std::sin(turn)},mark(i)?size*1.35f:size,hue(i));
        }
        c.text(40,154,"DRAG TO ORBIT - SCROLL TO ZOOM",{143,164,185});
        c.text(40,545,"ARRAY ORDER SPIRALS FROM TOP TO BOTTOM - THE SURFACE MEANS IN PLACE",{143,164,185});
    }
    void render() const {
        auto& c=canvas_;const Rgb ink{231,239,247},muted{143,164,185},mint=pal_[4].colour;
        const int v=view();
        c.clear(pal_[0].colour);
        c.rect(24,24,4,46,mint);c.text(44,26,"SORTING LAB",ink,3);
        c.text(44,61,std::string(sorting::names[int(engine_.algorithm)])+" SORT",muted);
        static constexpr const char* viewNames[]={"2D BARS","3D COLUMNS","DISPARITY CIRCLE","DISPARITY SPHERE"};
        c.text(790,28,viewNames[v],mint);
        c.text(790,58,std::to_string(engine_.values.size())+" ITEMS",muted);
        const std::uint64_t counts[]={engine_.comparisons,engine_.swaps,engine_.writes,engine_.auxiliaryWrites,engine_.reads};
        const char* labels[]={"COMPARISONS","SWAPS","ARRAY WRITES","BUFFER WRITES","KEY READS"};
        for(int j=0;j<5;++j) {int x=24+j*216;c.rect(x,92,200,48,{23,36,50});c.text(x+12,100,labels[j],muted);c.text(x+12,117,std::to_string(counts[j]),ink);}
        if(v==Columns) {
            c.camera(cam_);c.box(-1.48f,-.77f,-1.02f,2.96f,.07f,2.04f,{42,61,78});
            int n=int(engine_.values.size()),cols=int(std::ceil(std::sqrt(n*1.5))),rows=(n+cols-1)/cols;
            float w=2.8f/cols,d=1.9f/rows,m=float(scale());
            for(int i=0;i<n;++i)c.box(-1.4f+(i%cols)*w,-.7f,.95f-(i/cols+1)*d,w*.82f,std::max(0,engine_.values[std::size_t(i)].value)/m*1.22f,d*.82f,colour(i));
            c.text(40,154,"DRAG TO ORBIT - SCROLL TO ZOOM",muted);
            c.text(40,545,"ARRAY ORDER - LEFT TO RIGHT - FRONT ROW TO BACK",muted);
        } else if(v==Circle) {
            circle();
        } else if(v==Sphere) {
            sphere();
        } else {
            for(int y=185;y<=530;y+=69)c.rect(40,y,1040,1,{33,49,65});
            if(engine_.abacusShown())beadBars(40,174,1040,356);else bars(40,174,1040,356);
            c.rect(40,531,1040,2,{66,87,109});
            c.text(40,545,engine_.abacusShown()?"ARRAY POSITION - LEFT TO RIGHT - ONE CELL PER BEAD":"ARRAY POSITION - LEFT TO RIGHT",muted);
        }
        const bool buffer=engine_.usesBuffer();
        c.text(40,568,buffer?"WORKING BUFFER":"ARRAY OVERVIEW",muted);
        bars(40,586,1040,38,buffer);
        c.rect(24,640,1072,1,{42,61,78});
        c.text(40,654,engine_.eventName(),engine_.done?mint:ink);
        c.text(250,656,engine_.detail(),muted);
        c.resolve();
    }
};
inline SimPtr make_sorting(int view=SortingSim::Bars) {return std::make_unique<SortingSim>(view);}
} // namespace bench

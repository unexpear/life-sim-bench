#include "sims/sorting.hpp"
#include "render/raster.hpp"
#include "render/png.hpp"
#include <chrono>
#include <cstdio>

int main(int argc,char** argv) {
    using namespace bench;
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* message) {
        ++checks;if(!ok){++failed;std::printf("FAIL %s\n",message);}
    };
    auto fill=[](Surface& s,int w,int h,Rgb c) {
        s.resize(w,h);
        for(std::size_t p=0;p<s.rgba.size();p+=4) {
            s.rgba[p]=c.r;s.rgba[p+1]=c.g;s.rgba[p+2]=c.b;s.rgba[p+3]=255;
        }
    };
    Surface source;Raster raster;View view;view.grid=false;
    fill(source,17,11,{17,26,38});
    for(std::size_t i=0;i<source.rgba.size();i+=4)source.rgba[i]=std::uint8_t(i%253);
    raster.resize(17,11);raster.draw(source,view);
    check(std::equal(source.rgba.begin(),source.rgba.end(),raster.pixels()),"1:1 images preserve every byte and the last row/column");

    fill(source,2,2,{0,0,0});
    for(int y=0;y<2;++y)for(int c=0;c<3;++c)source.rgba[(y*2+1)*4+c]=255;
    raster.resize(4,4);raster.draw(source,view);
    check(raster.pixels()[0]==0&&raster.pixels()[4]==64&&raster.pixels()[8]==191&&raster.pixels()[12]==255,
          "magnification blends between pixel centres and extends edge colours");
    view.pan_x=1;raster.draw(source,view);
    check(raster.pixels()[3]==0&&raster.pixels()[8]==64,"panning refreshes filter weights and preserves the letterbox");
    view.pan_x=0;view.gamma=2;raster.draw(source,view);
    check(raster.pixels()[4]==16,"gamma applies after reconstruction");
    view.gamma=1;

    for(int parity=0;parity<2;++parity) {
        fill(source,64,64,{0,0,0});
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)
            if((x+y+parity)%2)for(int c=0;c<3;++c)source.rgba[(y*64+x)*4+c]=255;
        raster.resize(8,8);raster.draw(source,view);
        bool even=true;
        for(int p=0;p<64;++p)even=even&&raster.pixels()[p*4]==128;
        check(even,"shrinking integrates a checkerboard without moire or phase bias");
        raster.resize(1,1);raster.draw(source,view);
        check(raster.pixels()[0]==128,"extreme reduction averages the entire source");
    }
    for(int row:{30,31}) {
        fill(source,64,64,{0,0,0});
        for(int x=0;x<64;++x)for(int c=0;c<3;++c)source.rgba[(row*64+x)*4+c]=255;
        raster.resize(23,23);raster.draw(source,view);
        int brightness=0;
        for(int p=0;p<23*23;++p)brightness+=raster.pixels()[p*4];
        check(brightness>2000&&brightness<2150,"a thin line survives fractional reduction on either source row");
    }
    fill(source,8,8,{103,160,225});
    raster.resize(13,19);raster.draw(source,view);
    bool solid=true;
    for(int p=0;p<13*19;++p)if(raster.pixels()[p*4+3])
        solid=solid&&raster.pixels()[p*4]==103&&raster.pixels()[p*4+1]==160&&raster.pixels()[p*4+2]==225;
    check(solid,"changed source and destination sizes keep a flat colour exact");
    view.zoom=.7f;view.pan_x=-3.25f;view.pan_y=2.5f;raster.draw(source,view);
    auto fit=raster.fit_of(source.w,source.h,view);
    auto cell=raster.to_cell(fit.ox+3.5f*fit.scale,fit.oy+4.5f*fit.scale,fit);
    check(std::abs(cell.first-3.5f)<.0001f&&std::abs(cell.second-4.5f)<.0001f,"filtered views keep the same inverse mapping for input");
    view={};view.grid=false;
    Field field(2,2);field.set(1,0,1);
    raster.resize(16,16);raster.draw(field,{{{0,0,0},"off"},{{255,255,255},"on"}},view);
    bool discrete=true;int white=0;
    for(int p=0;p<256;++p){auto c=raster.pixels()[p*4];discrete=discrete&&(c==0||c==255);white+=c==255;}
    check(discrete&&white==64,"cell simulations retain sharp, unfiltered cells when enlarged");

    sorting::Canvas canvas;
    canvas.clear({17,26,38});
    canvas.samplePixel(100,100,{255,0,0});canvas.samplePixel(101,100,{0,255,0});
    canvas.samplePixel(100,101,{0,0,255});canvas.samplePixel(101,101,{255,255,255});canvas.resolve();
    const auto mixed=(std::size_t(50)*canvas.W+50)*4;
    check(canvas.image.rgba[mixed]==128&&canvas.image.rgba[mixed+1]==128&&canvas.image.rgba[mixed+2]==128&&canvas.image.rgba[mixed+3]==255,
          "sample resolution averages saturated channels without overflow or colour bleed");
    check(canvas.image.rgba[0]==17&&canvas.image.rgba[1]==26&&canvas.image.rgba[2]==38,
          "sample resolution preserves flat background colours exactly");
    Camera camera;camera.yaw=0;camera.pitch=0;camera.distance=4.7f;
    for(bool ortho:{false,true}) {
        camera.ortho=ortho;canvas.camera(camera);canvas.clear({0,0,0});
        canvas.line({-5,-5,0},{5,4,0},{255,255,255});canvas.resolve();
        int partial=0;bool clipped=true;
        for(int y=0;y<canvas.H;++y)for(int x=0;x<canvas.W;++x) {
            auto c=canvas.image.rgba[(std::size_t(y)*canvas.W+x)*4];
            if(c>0&&c<255)++partial;
            if(x<canvas.clipLeft||x>canvas.clipRight||y<canvas.clipTop||y>canvas.clipBottom)clipped=clipped&&c==0;
        }
        check(partial>100,"diagonal 3D guides have partial pixel coverage");
        check(clipped,"3D coverage stays inside the plotting area");
        auto scene=[&](bool reverse) {
            canvas.clear({17,26,38});
            auto front=[&]{canvas.ball({.05f,0,.24f},.32f,{243,125,116});};
            auto back=[&]{canvas.ball({0,0,-.24f},.32f,{103,160,225});};
            if(reverse){front();back();}else{back();front();}
            canvas.resolve();return canvas.image.rgba;
        };
        auto a=scene(false),b=scene(true);
        check(a==b,"overlapping sphere silhouettes depth-test independently of drawing order");
        const auto centre=(std::size_t(320)*canvas.W+canvas.W/2)*4;
        check(a[centre]>a[centre+2],"the near sphere hides the far sphere at the centre");
        auto boxes=[&](bool reverse) {
            canvas.clear({17,26,38});
            auto front=[&]{canvas.box(-.3f,-.3f,.2f,.6f,.6f,.3f,{243,125,116});};
            auto back=[&]{canvas.box(-.5f,-.5f,-.4f,.8f,.8f,.3f,{103,160,225});};
            if(reverse){front();back();}else{back();front();}
            canvas.resolve();return canvas.image.rgba;
        };
        check(boxes(false)==boxes(true),"supersampled box faces occlude independently of drawing order");
    }

    for(int size:{64,512})for(int mode=0;mode<4;++mode) {
        SortingSim sim(mode);sim.on_knob("size",float(size));sim.reset();
        sim.surface(); // Warm the polar cache before measuring sustained drawing.
        const auto values=sim.engine().values;const auto events=sim.generation();
        raster.resize(960,600);
        auto begin=std::chrono::steady_clock::now();
        for(int frame=0;frame<24;++frame) {
            if(sim.has_camera())sim.camera_orbit(1,.3f);
            else sim.on_knob("view",float(mode));
            raster.draw(*sim.surface(),view);
        }
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/24;
        check(values==sim.engine().values&&events==sim.generation(),"repeated drawing and orbiting leave the sort untouched");
        std::printf("Render + resize %3d items, view %d: %.2f ms/frame\n",size,mode,ms);
        if(argc>1) {
            const auto* s=sim.surface();
            std::string prefix=std::string(argv[1])+"-"+std::to_string(size)+"-view"+std::to_string(mode);
            check(write_png((prefix+".png").c_str(),s->w,s->h,s->rgba.data()),"save full-size preview");
            raster.resize(720,450);raster.draw(*s,view);
            check(write_png((prefix+"-small.png").c_str(),720,450,raster.pixels()),"save small-window preview");
        }
    }
    std::printf("%d rendering checks, %d failed\n",checks,failed);
    return failed?1:0;
}

#pragma once
#include "voxel.hpp"

namespace bench {
// Image reconstruction for rendered surfaces only. Cell fields retain their
// discrete, nearest-neighbour magnification in Raster. Pixel centres align at
// 1:1; reduction integrates the whole footprint so thin details cannot vanish.
class SurfaceFilter {
public:
    void draw(const Surface& source,std::uint8_t* target,int w,int h,
              float scale,float ox,float oy,const std::uint8_t* gamma) {
        if(!std::isfinite(scale)||scale<=0||!std::isfinite(ox)||!std::isfinite(oy))return;
        horizontal.prepare(w,source.w,scale,ox);
        vertical.prepare(h,source.h,scale,oy);
        for(int y=0;y<h;++y) {
            const auto ys=vertical.spans[std::size_t(y)];
            if(!ys.count)continue;
            for(int x=0;x<w;++x) {
                const auto xs=horizontal.spans[std::size_t(x)];
                if(!xs.count)continue;
                float colour[3]{};
                if(xs.count<=2&&ys.count<=2) {
                    // Most window sizes use a 2x2 footprint. Keep this common
                    // case out of the variable-size accumulation loops.
                    const auto x0=horizontal.taps[xs.start],x1=horizontal.taps[xs.start+xs.count-1];
                    const auto y0=vertical.taps[ys.start],y1=vertical.taps[ys.start+ys.count-1];
                    const auto* a=&source.rgba[(std::size_t(y0.index)*source.w+x0.index)*4];
                    const auto* b=&source.rgba[(std::size_t(y0.index)*source.w+x1.index)*4];
                    const auto* c=&source.rgba[(std::size_t(y1.index)*source.w+x0.index)*4];
                    const auto* d=&source.rgba[(std::size_t(y1.index)*source.w+x1.index)*4];
                    const float tx=xs.count==1?0:x1.weight,ty=ys.count==1?0:y1.weight;
                    for(int channel=0;channel<3;++channel) {
                        const float top=a[channel]+(b[channel]-a[channel])*tx;
                        const float bottom=c[channel]+(d[channel]-c[channel])*tx;
                        colour[channel]=top+(bottom-top)*ty;
                    }
                } else {
                    for(std::size_t iy=ys.start;iy<ys.start+ys.count;++iy) {
                        const auto yt=vertical.taps[iy];
                        const auto row=std::size_t(yt.index)*source.w;
                        for(std::size_t ix=xs.start;ix<xs.start+xs.count;++ix) {
                            const auto xt=horizontal.taps[ix];
                            const auto p=(row+xt.index)*4;
                            const float weight=yt.weight*xt.weight;
                            for(int c=0;c<3;++c)colour[c]+=source.rgba[p+c]*weight;
                        }
                    }
                }
                const auto out=(std::size_t(y)*w+x)*4;
                for(int c=0;c<3;++c) {
                    const auto value=std::uint8_t(std::clamp(int(colour[c]+.5f),0,255));
                    target[out+c]=gamma?gamma[value]:value;
                }
                target[out+3]=255;
            }
        }
    }
private:
    struct Tap {int index;float weight;};
    struct Span {std::size_t start=0,count=0;};
    struct Axis {
        std::vector<Tap> taps;
        std::vector<Span> spans;
        int sourceSize=0;
        float lastScale=0,lastOffset=0;
        void prepare(int output,int input,float scale,float offset) {
            if(int(spans.size())==output&&sourceSize==input&&lastScale==scale&&lastOffset==offset)return;
            sourceSize=input;lastScale=scale;lastOffset=offset;
            spans.assign(std::size_t(output),{});taps.clear();
            for(int p=0;p<output;++p) {
                const float centre=(p+.5f-offset)/scale;
                if(centre<0||centre>=input)continue;
                auto& span=spans[std::size_t(p)];span.start=taps.size();
                if(scale>=1) {
                    const float at=centre-.5f;
                    const int left=int(std::floor(at));const float t=at-left;
                    taps.push_back({std::clamp(left,0,input-1),1-t});
                    if(t>0)taps.push_back({std::clamp(left+1,0,input-1),t});
                } else {
                    const float lo=std::max(0.f,(p-offset)/scale);
                    const float hi=std::min(float(input),(p+1-offset)/scale);
                    for(int q=int(std::floor(lo));q<std::min(input,int(std::ceil(hi)));++q) {
                        const float weight=(std::min(hi,float(q+1))-std::max(lo,float(q)))/(hi-lo);
                        if(weight>0)taps.push_back({q,weight});
                    }
                }
                span.count=taps.size()-span.start;
            }
        }
    } horizontal,vertical;
};
} // namespace bench

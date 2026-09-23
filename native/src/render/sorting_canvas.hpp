#pragma once
#include "voxel.hpp"
#include <cmath>
#include <limits>
#include <string>

namespace bench::sorting {
// Portable canvas with four independently depth-tested samples per output pixel.
// Layout stays in logical pixels; only the drawing buffer is supersampled.
class Canvas {
public:
    Surface image;
    static constexpr int W=1120,H=680;
    static constexpr int samples=2;
    // Where 3D geometry may draw: below the counters, above the strip.
    static constexpr int clipLeft=24,clipRight=W-25,clipTop=146,clipBottom=552;
    Canvas() {image.resize(W,H);drawing.resize(drawW*drawH);depth.resize(drawW*drawH);}
    void pixel(int x,int y,Rgb c) {
        if(x<0||y<0||x>=W||y>=H)return;
        for(int sy=0;sy<samples;++sy)for(int sx=0;sx<samples;++sx)
            samplePixel(x*samples+sx,y*samples+sy,c);
    }
    // Subpixel coordinates for the polar view, which has its own rasterizer.
    void samplePixel(int x,int y,Rgb c) {
        if(x<0||y<0||x>=drawW||y>=drawH)return;
        drawing[std::size_t(y)*drawW+x]=packed(c);
    }
    void rect(int x,int y,int w,int h,Rgb c) {
        const int left=std::max(0,x)*samples,rightEdge=std::min(W,x+w)*samples;
        if(left>=rightEdge)return;
        for(int yy=std::max(0,y)*samples;yy<std::min(H,y+h)*samples;++yy)
            std::fill(drawing.begin()+std::size_t(yy)*drawW+left,drawing.begin()+std::size_t(yy)*drawW+rightEdge,packed(c));
    }
    void clear(Rgb c) {std::fill(drawing.begin(),drawing.end(),packed(c));std::fill(depth.begin(),depth.end(),0.f);}
    void resolve() {
        for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
            const auto p=(std::size_t(y)*W+x)*4;
            const auto q=std::size_t(y*samples)*drawW+x*samples;
            static_assert(samples==2);
            const auto a=drawing[q],b=drawing[q+1],c=drawing[q+drawW],d=drawing[q+drawW+1];
            // Two separated 16-bit lanes add red and blue without carry between
            // channels; green is averaged separately. Round to nearest byte.
            const auto rb=((a&0xff00ff)+(b&0xff00ff)+(c&0xff00ff)+(d&0xff00ff)+0x20002)>>2;
            const auto g=(((a>>8)&255)+((b>>8)&255)+((c>>8)&255)+((d>>8)&255)+2)>>2;
            image.rgba[p]=std::uint8_t(rb);image.rgba[p+1]=std::uint8_t(g);image.rgba[p+2]=std::uint8_t(rb>>16);
            image.rgba[p+3]=255;
        }
    }
    void text(int x,int y,const std::string& s,Rgb c,int scale=2) {
        // Original stroke alphabet. Rounded, antialiased strokes stay legible at
        // fractional window scales without a platform font or external asset.
        // Pairs are x/y coordinates; a space starts a new path.
        static constexpr const char* glyph[]={
            "062046 1434", "0006 003041423303 033445463606", "4130100105163645", "06003041453606",
            "40000646 0333", "400006 0333", "41301001051636454323", "0006 4046 0343",
            "0040 2026 0646", "0040 4045361605", "0006 400346", "000646",
            "0600234046", "06004640", "103041453616050110", "06003041423303",
            "103041453616050110 2446", "06003041423303 2346", "403010010213334445463606", "0040 2026",
            "000516364540", "002640", "0006244640", "0046 4006", "002340 2326", "00400646",
            "103041453616050110 1432", "112026 1646", "01103041420646", "0030414233 233344453606",
            "4046 40300444", "4000033344453606", "4030100105163645443303", "004016",
            "103041423313020110 133344453616050413", "4333130201103041453616"
        };
        for(char ch:s) {
            if(ch>='a'&&ch<='z')ch=char(ch-'a'+'A');
            const char* path="";
            if(ch>='A'&&ch<='Z')path=glyph[ch-'A'];
            else if(ch>='0'&&ch<='9')path=glyph[26+ch-'0'];
            else switch(ch) {
                case '(':path="3011121536";break;case ')':path="1031323516";break;
                case '/':path="0640";break;case ':':path="2222 2525";break;
                case ',':path="252616";break;case '.':path="2626";break;
                case '-':path="0343";break;case '+':path="0343 2125";break;
                case '=':path="0242 0444";break;
            }
            float px=0,py=0;bool move=true;
            while(*path) {
                if(*path==' ') {move=true;++path;continue;}
                const float nx=x+scale*(.4f+1.05f*(path[0]-'0'));
                const float ny=y+scale*(.45f+(path[1]-'0'));
                if(!move)stroke(px,py,nx,ny,c,.68f*scale);
                px=nx;py=ny;move=false;path+=2;
            }
            x+=6*scale;
        }
    }
    struct V {float x,y,z;};
    void camera(const Camera& cam) {
        cam.eye(eye);cam.basis(right,up);cam.forward(forward);
        ortho=cam.ortho;focal=340/std::tan(cam.fov*.5f);distance=cam.distance;
    }
    V project(V v) const {
        float d[3]={v.x-eye[0],v.y-eye[1],v.z-eye[2]};
        float x=dot(d,right),y=dot(d,up),z=dot(d,forward);
        const float scale=focal/(ortho?distance:z);
        return {(W*.5f+x*scale)*samples,(320-y*scale)*samples,1/z};
    }
    void box(float x,float y,float z,float w,float h,float d,Rgb colour) {
        V a[8]={{x,y,z},{x+w,y,z},{x+w,y+h,z},{x,y+h,z},
                {x,y,z+d},{x+w,y,z+d},{x+w,y+h,z+d},{x,y+h,z+d}};
        V p[8];for(int k=0;k<8;++k)p[k]=project(a[k]);
        const int faces[6][4]={{0,1,2,3},{4,7,6,5},{0,4,5,1},{3,2,6,7},{0,3,7,4},{1,5,6,2}};
        const V normals[6]={{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}};
        const float shade[6]={.68f,.84f,.5f,1.f,.64f,.78f};
        for(int f=0;f<6;++f) {
            const V corner=a[faces[f][0]],normal=normals[f];
            const V toEye=ortho?V{-forward[0],-forward[1],-forward[2]}:V{eye[0]-corner.x,eye[1]-corner.y,eye[2]-corner.z};
            if(normal.x*toEye.x+normal.y*toEye.y+normal.z*toEye.z<=0)continue;
            Rgb c{std::uint8_t(colour.r*shade[f]),std::uint8_t(colour.g*shade[f]),std::uint8_t(colour.b*shade[f])};
            triangle(p[faces[f][0]],p[faces[f][1]],p[faces[f][2]],c);
            triangle(p[faces[f][0]],p[faces[f][2]],p[faces[f][3]],c);
        }
    }
    // A ball of world radius r, drawn as a shaded disc whose depth follows the
    // ball's front surface, so balls that overlap intersect correctly.
    void ball(V centre,float r,Rgb colour) {
        const V p=project(centre);
        if(p.z<=0)return;
        const float zc=1/p.z;
        const float rs=r*focal*samples/(ortho?distance:zc);
        if(rs<.5f) {depthPixel(int(p.x),int(p.y),p.z,colour);return;}
        const int x0=std::max(clipLeft*samples,int(std::floor(p.x-rs))),x1=std::min((clipRight+1)*samples-1,int(std::ceil(p.x+rs)));
        const int y0=std::max(clipTop*samples,int(std::floor(p.y-rs))),y1=std::min((clipBottom+1)*samples-1,int(std::ceil(p.y+rs)));
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            const float dx=(x+.5f-p.x)/rs,dy=(y+.5f-p.y)/rs,d2=dx*dx+dy*dy;
            if(d2>1)continue;
            const float nz=std::sqrt(1-d2);
            const float front=zc-r*nz;
            if(front<=0)continue;
            // Lit from the upper left, facing the viewer.
            const float light=.3f+.7f*std::max(0.f,-.36f*dx-.48f*dy+.8f*nz);
            // A restrained highlight makes the small balls read as spheres.
            float spec=std::max(0.f,-.19f*dx-.25f*dy+.949f*nz);
            spec*=spec;spec*=spec;spec*=spec;spec*=spec;spec*=spec;
            auto lit=[&](std::uint8_t channel) {return std::uint8_t(std::min(255.f,channel*light+46*spec));};
            depthPixel(x,y,1/front,{lit(colour.r),lit(colour.g),lit(colour.b)});
        }
    }
    // A round, one-logical-pixel stroke. Samples keep coverage and depth together
    // so an edge cannot leave a bright fringe through a foreground ball.
    void line(V a,V b,Rgb colour) {
        const V p=project(a),q=project(b);
        if(p.z<=0||q.z<=0)return;
        const float dx=q.x-p.x,dy=q.y-p.y,len2=dx*dx+dy*dy,radius=.5f*samples;
        const int x0=std::max(clipLeft*samples,int(std::floor(std::min(p.x,q.x)-radius)));
        const int x1=std::min((clipRight+1)*samples-1,int(std::ceil(std::max(p.x,q.x)+radius)));
        const int y0=std::max(clipTop*samples,int(std::floor(std::min(p.y,q.y)-radius)));
        const int y1=std::min((clipBottom+1)*samples-1,int(std::ceil(std::max(p.y,q.y)+radius)));
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            const float t=len2>0?std::clamp(((x+.5f-p.x)*dx+(y+.5f-p.y)*dy)/len2,0.f,1.f):0;
            const float ex=x+.5f-(p.x+t*dx),ey=y+.5f-(p.y+t*dy);
            if(ex*ex+ey*ey>radius*radius)continue;
            // Perspective interpolates inverse depth; parallel projection, depth itself.
            const float z=ortho?1/((1-t)/p.z+t/q.z):(1-t)*p.z+t*q.z;
            depthPixel(x,y,z,colour);
        }
    }
private:
    static constexpr int drawW=W*samples,drawH=H*samples;
    std::vector<std::uint32_t> drawing;
    std::vector<float> depth;
    static std::uint32_t packed(Rgb c) {return std::uint32_t(c.r)|(std::uint32_t(c.g)<<8)|(std::uint32_t(c.b)<<16);}
    void stroke(float ax,float ay,float bx,float by,Rgb c,float width) {
        ax*=samples;ay*=samples;bx*=samples;by*=samples;
        const float radius=width*samples*.5f,dx=bx-ax,dy=by-ay,len2=dx*dx+dy*dy;
        const int x0=std::max(0,int(std::floor(std::min(ax,bx)-radius-1)));
        const int x1=std::min(drawW-1,int(std::ceil(std::max(ax,bx)+radius+1)));
        const int y0=std::max(0,int(std::floor(std::min(ay,by)-radius-1)));
        const int y1=std::min(drawH-1,int(std::ceil(std::max(ay,by)+radius+1)));
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            const float t=len2>0?std::clamp(((x+.5f-ax)*dx+(y+.5f-ay)*dy)/len2,0.f,1.f):0;
            const float ex=x+.5f-ax-t*dx,ey=y+.5f-ay-t*dy;
            const float coverage=std::clamp(radius+.5f-std::sqrt(ex*ex+ey*ey),0.f,1.f);
            auto& p=drawing[std::size_t(y)*drawW+x];
            std::uint32_t result=0;
            for(int ch=0;ch<3;++ch) {
                const int ink=ch==0?c.r:ch==1?c.g:c.b;
                const float old=float((p>>(ch*8))&255);
                result|=std::uint32_t(old*(1-coverage)+ink*coverage+.5f)<<(ch*8);
            }
            p=result;
        }
    }
    float eye[3]{},right[3]{},up[3]{},forward[3]{},focal=1,distance=1;
    bool ortho=false;
    static float dot(const float* a,const float* b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
    static float edge(V a,V b,float x,float y) {return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);}
    void depthPixel(int x,int y,float z,Rgb colour) {
        if(x<clipLeft*samples||x>=(clipRight+1)*samples||y<clipTop*samples||y>=(clipBottom+1)*samples)return;
        auto index=std::size_t(y)*drawW+x;
        if(z>depth[index]) {depth[index]=z;samplePixel(x,y,colour);}
    }
    void triangle(V a,V b,V c,Rgb colour) {
        if(a.z<=0||b.z<=0||c.z<=0)return;
        const float area=edge(a,b,c.x,c.y);if(std::abs(area)<.001f)return;
        int x0=std::max(clipLeft*samples,int(std::floor(std::min({a.x,b.x,c.x}))));
        int x1=std::min((clipRight+1)*samples-1,int(std::ceil(std::max({a.x,b.x,c.x}))));
        int y0=std::max(clipTop*samples,int(std::floor(std::min({a.y,b.y,c.y}))));
        int y1=std::min((clipBottom+1)*samples-1,int(std::ceil(std::max({a.y,b.y,c.y}))));
        const float invArea=1/area,du=(b.y-c.y)*invArea,dv=(c.y-a.y)*invArea;
        const float za=ortho?1/a.z:a.z,zb=ortho?1/b.z:b.z,zc=ortho?1/c.z:c.z;
        const auto ink=packed(colour);
        for(int y=y0;y<=y1;++y) {
            float u=edge(b,c,x0+.5f,y+.5f)*invArea,v=edge(c,a,x0+.5f,y+.5f)*invArea;
            for(int x=x0;x<=x1;++x,u+=du,v+=dv) {
                const float w=1-u-v;
                if(u<-.000001f||v<-.000001f||w<-.000001f)continue;
                // Perspective interpolates inverse depth; parallel projection,
                // camera depth. Reciprocal vertex depths are computed once.
                const float interpolated=u*za+v*zb+w*zc;
                const float z=ortho?1/interpolated:interpolated;
                const auto index=std::size_t(y)*drawW+x;
                if(z>depth[index]) {depth[index]=z;drawing[index]=ink;}
            }
        }
    }
};
} // namespace bench::sorting

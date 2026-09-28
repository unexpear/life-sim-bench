// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic arenas for the reusable fly actor (actors/fly.hpp).
// Hosted by sims/fly_arena.hpp. Not a Brian2/FlyWire build.
// Original 2D walking arenas. Synthetic odor/light, circular body and obstacles;
// no wing aerodynamics, fluid solver, biomechanical legs or neural data.
#pragma once
#include "fly.hpp"
#include "../rng.hpp"
#include <limits>
#include <stdexcept>
#include <vector>

namespace bench::fly {
enum class Scene { Food, Obstacles, Light, Wind };
struct Config {
    int seed=1,obstacles=10;
    double speed=10,source_x=84,source_y=30,spread=20,wind=8;
    bool seek_light=true;
};
struct Rock { Point centre; double radius; };
class World {
public:
    static constexpr double width=100,height=60;
    Scene scene;
    Config config;
    Body body;
    std::vector<Rock> rocks;
    std::uint64_t tick=0,contacts=0;
    double travelled=0,first_arrival=-1,light_time=0;
    bool contact=false,reached=false;
    explicit World(Scene s=Scene::Food):scene(s) {}
    Point source() const {return {config.source_x,config.source_y};}
    Point target() const {return scene==Scene::Light&&!config.seek_light?Point{12,config.source_y}:source();}
    double seconds() const {return double(tick)*dt;}
    void reset(Config c) {
        if(c.seed<1||c.seed>9999||c.obstacles<0||c.obstacles>20||
            !std::isfinite(c.speed)||c.speed<0||c.speed>20||
            !std::isfinite(c.source_x)||c.source_x<8||c.source_x>92||
            !std::isfinite(c.source_y)||c.source_y<8||c.source_y>52||
            !std::isfinite(c.spread)||c.spread<8||c.spread>35||
            !std::isfinite(c.wind)||c.wind<0||c.wind>20)
            throw std::invalid_argument("Fly arena settings are outside supported limits.");
        config=c;tick=contacts=0;travelled=light_time=0;first_arrival=-1;contact=reached=false;
        Rng rng(mix_seed(0xF17A4Eull,c.seed));
        body={scene==Scene::Wind?Point{88,30}:scene==Scene::Light?Point{50,30}:Point{12,30},
              (scene==Scene::Wind?pi:0)+(.6*rng.unit()-.3)};
        rocks.clear();
        if(scene==Scene::Obstacles) {
            // Fixed lanes with seeded offsets leave gaps wider than the body.
            // No rock may cover the spawn or the configured target.
            for(int i=0;i<c.obstacles;++i) {
                Point p{29+12.0*(i%5),9+14.0*(i/5)+2*(rng.unit()-.5)};
                double r=2.4+.6*rng.unit();
                if(distance(p,source())<r+6 || distance(p,body.position)<r+6)continue;
                rocks.push_back({p,r});
            }
        }
    }
    bool free(Point p) const {
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<Body::radius||p.x>width-Body::radius||p.y<Body::radius||p.y>height-Body::radius)return false;
        for(const auto& r:rocks)if(distance(p,r.centre)<r.radius+Body::radius-1e-8)return false;
        return true;
    }
    // Swept-circle distance: exact ray intersections against expanded rocks and
    // inward-offset walls. Used for both sensors and motion to prevent tunnelling.
    double clearance(Point p,double angle,double limit=24) const {
        const double dx=std::cos(angle),dy=std::sin(angle);
        double result=limit;
        if(dx>1e-12)result=std::min(result,(width-Body::radius-p.x)/dx);
        if(dx< -1e-12)result=std::min(result,(Body::radius-p.x)/dx);
        if(dy>1e-12)result=std::min(result,(height-Body::radius-p.y)/dy);
        if(dy< -1e-12)result=std::min(result,(Body::radius-p.y)/dy);
        for(const auto& r:rocks) {
            const double x=p.x-r.centre.x,y=p.y-r.centre.y,b=x*dx+y*dy;
            const double q=x*x+y*y-(r.radius+Body::radius)*(r.radius+Body::radius);
            const double disc=b*b-q;
            if(disc>=0) {const double t=-b-std::sqrt(disc);if(t>=0)result=std::min(result,t);}
        }
        return std::max(0.0,result);
    }
    double odor(Point p) const {
        if(scene==Scene::Light)return 0;
        const double x=p.x-config.source_x,y=p.y-config.source_y;
        if(scene!=Scene::Wind || config.wind==0)
            return std::exp(-(x*x+y*y)/(2*config.spread*config.spread));
        // Bounded analytic plume drifting toward +x. The meander is retarded
        // by downwind travel time. This is a stimulus field, not CFD.
        if(x<0)return std::exp(-(x*x+y*y)/18);
        const double travel=x/config.wind;
        const double centre=2.8*std::sin((seconds()-travel)*1.4)*(1-std::exp(-x/20));
        const double sigma=2.5+x*config.spread/120;
        return std::clamp(std::exp(-x/(35+3*config.wind)-(y-centre)*(y-centre)/(2*sigma*sigma)),0.0,1.0);
    }
    double light(Point p) const {
        if(scene!=Scene::Light)return .15;
        const double d=distance(p,source());return std::exp(-d*d/800);
    }
    Observation observe() const {
        Observation o;o.tick=tick;o.seconds=seconds();o.contact=contact;
        const double cs=std::cos(body.heading),sn=std::sin(body.heading);
        for(int i=0;i<2;++i) {
            const double side=i==0?.9:-.9;
            Point p{body.position.x+1.8*cs-side*sn,body.position.y+1.8*sn+side*cs};
            o.odor[i]=odor(p);o.light[i]=light(p);
        }
        for(int i=0;i<3;++i)o.clearance[i]=clearance(body.position,body.heading+(.65-.65*i));
        return o;
    }
    bool advance(const Action& a) {
        if(!valid(a,tick))return false; // invalid/stale commands never move or advance time
        body.heading=std::remainder(body.heading+a.turn*4*dt,2*pi);
        const double requested=a.forward*config.speed*dt;
        const double allowed=std::max(0.0,clearance(body.position,body.heading,requested)-1e-9);
        const bool hit=requested>allowed+1e-7;
        if(hit&&!contact)++contacts;
        contact=hit;travelled+=allowed;
        body.position.x+=allowed*std::cos(body.heading);body.position.y+=allowed*std::sin(body.heading);
        ++tick;
        if(light(body.position)>.5)light_time+=dt;
        // 5 mm matches the reactive controller stopping when both odor sensors saturate.
        if(distance(body.position,target())<5 && !reached) {reached=true;first_arrival=seconds();}
        return true;
    }
};
} // namespace bench::fly
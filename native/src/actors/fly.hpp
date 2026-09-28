// SPDX-License-Identifier: GPL-3.0-or-later
// Reusable fly actor contract. Hosted by sims/fly_arena.hpp (registered).
// Hand-written sensor/controller boundary — not a Brian2 brain, and it does
// not carry FlyWire (CC BY-NC) data. See native/RESEARCH-NEXT.md and PACKS.md.
// Original, deliberately simplified sensor/controller boundary. No connectome.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace bench::fly {
inline constexpr double pi=3.14159265358979323846;
inline constexpr double dt=1.0/60.0;
struct Point { double x=0,y=0; };
inline double distance(Point a,Point b) { return std::hypot(a.x-b.x,a.y-b.y); }
// World coordinates are millimetres, +x right, +y up; positive yaw turns left.
struct Body { Point position{12,30}; double heading=0; static constexpr double radius=1.2; };
struct Observation {
    static constexpr unsigned version=1;
    std::uint64_t tick=0;
    double seconds=0,interval=dt;
    std::array<double,2> odor{},light{}; // left, right; synthetic intensities 0..1
    std::array<double,3> clearance{};   // left, ahead, right; millimetres, capped at 24
    bool contact=false;
};
struct Action {
    unsigned version=Observation::version;
    std::uint64_t tick=0;
    double forward=0,turn=0; // 0..1 speed demand, -1..1 yaw demand
};
// Contract identity for hosts and save documents. Synthetic backends leave
// dataset_hash empty. When a local FlyWire pack is present, hosts should also
// surface pack::dataset_hash() (see fly_pack.hpp); that value is not baked into
// this constexpr because the pack is optional and gitignored under userdata/.
struct Identity {
    static constexpr unsigned contract_version = Observation::version;
    static constexpr const char* backend = "reactive-stub";
    static constexpr const char* body = "disk-2d-mm";
    static constexpr const char* dataset_hash = "";
};
inline bool valid(const Action& a,std::uint64_t tick) {
    return a.version==Observation::version && a.tick==tick &&
        std::isfinite(a.forward)&&std::isfinite(a.turn)&&a.forward>=0&&a.forward<=1&&a.turn>=-1&&a.turn<=1;
}
class Controller {
public:
    virtual ~Controller()=default;
    virtual void reset(std::uint64_t seed)=0;
    virtual Action act(const Observation&)=0;
    virtual const char* backend() const { return Identity::backend; }
};
// A transparent, hand-written baseline, not a brain reconstruction or learner.
// It receives local sensors only: no target position, map or optimal path.
class ReactiveController final : public Controller {
public:
    double odor_gain=1,light_gain=0;
    void reset(std::uint64_t seed) override { phase_=double(seed%6283)*.001;avoid_=0; }
    Action act(const Observation& o) override {
        auto contrast=[](const auto& s) {return (s[0]-s[1])/(.01+s[0]+s[1]);};
        double turn=9*(odor_gain*contrast(o.odor)+light_gain*contrast(o.light));
        double drive=.8;
        if(o.clearance[1]<6 || o.contact) {
            if(!avoid_)avoid_=o.clearance[0]>=o.clearance[2]?1:-1;
            turn=avoid_;drive=o.clearance[1]<2?.08:.35;
        } else {
            avoid_=0;
            turn+=.07*std::sin(o.seconds*1.7+phase_);
        }
        if(odor_gain>0 && std::min(o.odor[0],o.odor[1])>.99)drive=0;
        return {Observation::version,o.tick,drive,std::clamp(turn,-1.0,1.0)};
    }
    const char* backend() const override { return Identity::backend; }
private:
    double phase_=0,avoid_=0;
};
} // namespace bench::fly
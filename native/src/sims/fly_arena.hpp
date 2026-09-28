// SPDX-License-Identifier: GPL-3.0-or-later
// Fly Arena — registered synthetic host for the reusable fly actor contract.
//
// This is the first embodiment named in native/RESEARCH-NEXT.md: a clearly
// labeled simple body with light/contact/odor-field inputs and a configurable
// movement decoder. The controller is the hand-written ReactiveController in
// actors/fly.hpp. It is NOT a Brian2 brain, NOT a Shiu reproduction, and it
// carries NO FlyWire (CC BY-NC) data. Those remain an optional pack and a
// separate Windows runtime gate. See STATUS.md and PACKS.md.

#pragma once
#include "../sim.hpp"
#include "../actors/fly_world.hpp"
#include "../actors/fly_pack.hpp"
#include "../actors/fly_brian2.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

class FlyArena final : public Sim {
public:
    static constexpr int kViewW = 200;
    static constexpr int kViewH = 120;

    FlyArena() {
        about_ = Provenance{
            "Fly Arena (reactive stub)",
            "2026",
            "Life-sim Workbench research stub; not a connectome model",
            "native/RESEARCH-NEXT.md, Fly brain — first embodiment. Controller: "
            "bench::fly::ReactiveController. Optional local FlyWire pack and "
            "Brian2 venv are discovered when present (see fly_pack.hpp); neither "
            "is required to run this synthetic arena.",
            Replication::No,
            "No. A single disk walks an arena. Nothing copies itself.",
            "A millimetre-scale walking disk with left/right odor and light sensors, "
            "three clearance rays and contact. Scenes: food plume, rock field, light "
            "patch and drifting wind plume. The on-board controller is a transparent "
            "reactive baseline so the actor Observation/Action contract can be tested "
            "without neural or NC data. It is not a brain reconstruction."
        };
        pal_ = {
            {{12, 14, 20}, "empty"},
            {{28, 48, 40}, "weak plume"},
            {{48, 110, 72}, "plume"},
            {{90, 78, 64}, "rock"},
            {{220, 180, 70}, "target"},
            {{236, 210, 120}, "body"},
            {{250, 240, 200}, "heading"},
        };
        view_ = Field(kViewW, kViewH);
        knobs_ = {
            {"scene", "scene", 0.f, 3.f, 0.f, 1.f,
             {"food plume", "obstacles", "light patch", "wind plume"}, true,
             "Which synthetic arena to build. Changing it starts that scene over."},
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent layout and controller phase this is."},
            // Rock count only shapes the obstacles scene.
            {"obstacles", "rock count", 0.f, 20.f, 10.f, 1.f, {}, true,
             "How many rocks the obstacles scene tries to place (gaps stay walkable).",
             false, false, "scene", 1.f},
            {"speed", "walk speed", 2.f, 20.f, 10.f, 1.f, {}, true,
             "Millimetres per second at full forward demand."},
            {"odor_gain", "odor turn gain", 0.f, 2.f, 1.f, 0.1f, {}, false,
             "How strongly left/right odor contrast turns the body. 0 ignores odor."},
            // Light contrast is flat outside the light scene, so the dead-knob
            // test must force scene=light before judging this gain.
            {"light_gain", "light turn gain", 0.f, 2.f, 1.f, 0.1f, {}, false,
             "How strongly left/right light contrast turns the body. Defaults off "
             "except the light scene forces it on at reset.",
             false, false, "scene", 2.f},
            {"seek_light", "seek bright patch", 0.f, 1.f, 1.f, 1.f,
             {"avoid", "seek"}, true,
             "Light scene only: whether the target is the bright patch or the dark side.",
             false, false, "scene", 2.f},
            {"rate", "steps per frame", 1.f, 12.f, 4.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        brain_ = std::make_unique<fly::ReactiveController>();
        reset();
    }

    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return view_; }
    std::uint64_t generation() const override { return world_.tick; }
    std::vector<Knob>& knobs() override { return knobs_; }
    std::string subtitle() const override {
        std::string s = std::string(fly::Identity::backend) + " / " + fly::Identity::body;
        const auto h = fly::pack::dataset_hash();
        if (!h.empty()) s += " / " + fly::pack::identity_label();
        else if (fly::brian2ref::runtime_present()) s += " / brian2-venv";
        return s;
    }

    void on_knob(const std::string& key, float v) override {
        for (auto& k : knobs_) if (k.key == key) k.value = k.quantised(v);
        if (key == "scene" || key == "seed" || key == "obstacles" || key == "speed"
            || key == "seek_light")
            reset();
        else if (key == "odor_gain" || key == "light_gain")
            apply_gains();
    }

    void reset() override {
        const int scene_i = std::clamp(int(knob("scene") + 0.5f), 0, 3);
        world_.scene = static_cast<fly::Scene>(scene_i);
        fly::Config c;
        c.seed = std::max(1, int(knob("seed") + 0.5f));
        c.obstacles = std::clamp(int(knob("obstacles") + 0.5f), 0, 20);
        c.speed = double(knob("speed"));
        c.seek_light = knob("seek_light") > 0.5f;
        // Do not rewrite light_gain here: the dead-knob suite resets after setting
        // the value, and a forced 1 would erase a deliberate 0.
        world_.reset(c);
        brain_->reset(std::uint64_t(c.seed));
        apply_gains();
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("rate") + 0.5f));
        for (int i = 0; i < n; ++i) {
            const auto o = world_.observe();
            const auto a = brain_->act(o);
            if (!world_.advance(a)) break;
        }
        publish();
    }

    bool poke(float nx, float ny) override {
        // Move the synthetic source/target toward the click. Rocks stay fixed.
        const double x = std::clamp(double(nx) * fly::World::width, 8.0, 92.0);
        const double y = std::clamp((1.0 - double(ny)) * fly::World::height, 8.0, 52.0);
        world_.config.source_x = x;
        world_.config.source_y = y;
        world_.reached = false;
        world_.first_arrival = -1;
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        const auto o = world_.observe();
        const double odor = 0.5 * (o.odor[0] + o.odor[1]);
        const double light = 0.5 * (o.light[0] + o.light[1]);
        return {
            Metric{"travelled mm", world_.travelled, 0.0, Metric::Neither},
            Metric{"contacts", double(world_.contacts), 0.0, Metric::Neither},
            Metric{"reached target", world_.reached ? 1.0 : 0.0, 1.0, Metric::Higher},
            Metric{"first arrival s", world_.first_arrival, 0.0, Metric::Lower},
            Metric{"mean odor", odor, 1.0, Metric::Neither},
            Metric{"mean light", light, 1.0, Metric::Neither},
            Metric{"clearance ahead mm", o.clearance[1], 24.0, Metric::Neither},
        };
    }

    [[nodiscard]] const fly::World& world() const { return world_; }
    fly::World& world() { return world_; }
    [[nodiscard]] fly::ReactiveController& brain() { return *brain_; }

    std::string saveScene() const {
        std::ostringstream out;
        out.precision(17);
        out << "flyarena-scene 1\n"
            << "scene " << int(world_.scene) << "\n"
            << "seed " << world_.config.seed << "\n"
            << "obstacles " << world_.config.obstacles << "\n"
            << "speed " << world_.config.speed << "\n"
            << "source_x " << world_.config.source_x << "\n"
            << "source_y " << world_.config.source_y << "\n"
            << "spread " << world_.config.spread << "\n"
            << "wind " << world_.config.wind << "\n"
            << "seek_light " << (world_.config.seek_light ? 1 : 0) << "\n"
            << "odor_gain " << brain_->odor_gain << "\n"
            << "light_gain " << brain_->light_gain << "\n"
            << "tick " << world_.tick << "\n"
            << "x " << world_.body.position.x << "\n"
            << "y " << world_.body.position.y << "\n"
            << "heading " << world_.body.heading << "\n"
            << "travelled " << world_.travelled << "\n"
            << "contacts " << world_.contacts << "\n"
            << "reached " << (world_.reached ? 1 : 0) << "\n"
            << "first_arrival " << world_.first_arrival << "\n"
            << "rocks " << world_.rocks.size() << "\n";
        for (const auto& r : world_.rocks)
            out << r.centre.x << ' ' << r.centre.y << ' ' << r.radius << '\n';
        out << "end\n";
        return out.str();
    }

    bool loadScene(const std::string& data) {
        std::istringstream in(data);
        std::string tag; int version = 0;
        if (!(in >> tag >> version) || tag != "flyarena-scene" || version != 1) return false;
        fly::Config c;
        int scene_i = 0, seek = 1, reached = 0;
        double odor_gain = 1, light_gain = 0;
        double x = 12, y = 30, heading = 0, travelled = 0, first = -1;
        std::uint64_t tick = 0, contacts = 0;
        std::vector<fly::Rock> rocks;
        std::string key;
        while (in >> key) {
            if (key == "end") break;
            if (key == "scene") { if (!(in >> scene_i)) return false; }
            else if (key == "seed") { if (!(in >> c.seed)) return false; }
            else if (key == "obstacles") { if (!(in >> c.obstacles)) return false; }
            else if (key == "speed") { if (!(in >> c.speed)) return false; }
            else if (key == "source_x") { if (!(in >> c.source_x)) return false; }
            else if (key == "source_y") { if (!(in >> c.source_y)) return false; }
            else if (key == "spread") { if (!(in >> c.spread)) return false; }
            else if (key == "wind") { if (!(in >> c.wind)) return false; }
            else if (key == "seek_light") { if (!(in >> seek)) return false; c.seek_light = seek != 0; }
            else if (key == "odor_gain") { if (!(in >> odor_gain)) return false; }
            else if (key == "light_gain") { if (!(in >> light_gain)) return false; }
            else if (key == "tick") { if (!(in >> tick)) return false; }
            else if (key == "x") { if (!(in >> x)) return false; }
            else if (key == "y") { if (!(in >> y)) return false; }
            else if (key == "heading") { if (!(in >> heading)) return false; }
            else if (key == "travelled") { if (!(in >> travelled)) return false; }
            else if (key == "contacts") { if (!(in >> contacts)) return false; }
            else if (key == "reached") { if (!(in >> reached)) return false; }
            else if (key == "first_arrival") { if (!(in >> first)) return false; }
            else if (key == "rocks") {
                std::size_t n = 0;
                if (!(in >> n) || n > 64) return false;
                rocks.resize(n);
                for (std::size_t i = 0; i < n; ++i)
                    if (!(in >> rocks[i].centre.x >> rocks[i].centre.y >> rocks[i].radius))
                        return false;
            } else return false;
        }
        if (scene_i < 0 || scene_i > 3) return false;
        try {
            world_.scene = static_cast<fly::Scene>(scene_i);
            world_.reset(c);
        } catch (...) {
            return false;
        }
        world_.rocks = std::move(rocks);
        world_.body.position = {x, y};
        world_.body.heading = heading;
        world_.tick = tick;
        world_.travelled = travelled;
        world_.contacts = contacts;
        world_.reached = reached != 0;
        world_.first_arrival = first;
        world_.contact = !world_.free(world_.body.position);
        for (auto& k : knobs_) {
            if (k.key == "scene") k.value = float(scene_i);
            else if (k.key == "seed") k.value = float(c.seed);
            else if (k.key == "obstacles") k.value = float(c.obstacles);
            else if (k.key == "speed") k.value = float(c.speed);
            else if (k.key == "seek_light") k.value = c.seek_light ? 1.f : 0.f;
            else if (k.key == "odor_gain") k.value = float(odor_gain);
            else if (k.key == "light_gain") k.value = float(light_gain);
        }
        brain_->reset(std::uint64_t(c.seed));
        apply_gains();
        publish();
        return true;
    }

private:
    [[nodiscard]] float knob(const std::string& key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.f;
    }

    void apply_gains() {
        brain_->odor_gain = double(knob("odor_gain"));
        brain_->light_gain = double(knob("light_gain"));
    }

    void disk(int cx, int cy, int rad, std::uint8_t v) {
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                if (dx * dx + dy * dy > rad * rad) continue;
                const int x = cx + dx, y = cy + dy;
                if (x >= 0 && y >= 0 && x < view_.w && y < view_.h)
                    view_.set(x, y, v);
            }
    }

    void publish() {
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        // Coarse plume/light field for the eye; sensors still use the analytic field.
        for (int j = 0; j < view_.h; ++j) {
            for (int i = 0; i < view_.w; ++i) {
                const double x = (double(i) + 0.5) / double(view_.w) * fly::World::width;
                const double y = (1.0 - (double(j) + 0.5) / double(view_.h)) * fly::World::height;
                const fly::Point p{x, y};
                double strength = 0;
                if (world_.scene == fly::Scene::Light) strength = world_.light(p);
                else strength = world_.odor(p);
                if (strength > 0.55) view_.set(i, j, 2);
                else if (strength > 0.18) view_.set(i, j, 1);
            }
        }
        for (const auto& r : world_.rocks) {
            const int cx = int(r.centre.x / fly::World::width * double(view_.w));
            const int cy = view_.h - 1 - int(r.centre.y / fly::World::height * double(view_.h));
            const int rad = std::max(1, int(r.radius / fly::World::width * double(view_.w)));
            disk(cx, cy, rad, 3);
        }
        {
            const auto t = world_.target();
            const int cx = int(t.x / fly::World::width * double(view_.w));
            const int cy = view_.h - 1 - int(t.y / fly::World::height * double(view_.h));
            disk(cx, cy, 3, 4);
        }
        {
            const auto& b = world_.body;
            const int cx = int(b.position.x / fly::World::width * double(view_.w));
            const int cy = view_.h - 1 - int(b.position.y / fly::World::height * double(view_.h));
            const int rad = std::max(2, int(fly::Body::radius / fly::World::width * double(view_.w)));
            disk(cx, cy, rad, 5);
            const int hx = cx + int(std::cos(b.heading) * double(rad + 2));
            const int hy = cy - int(std::sin(b.heading) * double(rad + 2));
            disk(hx, hy, 1, 6);
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    fly::World world_;
    std::unique_ptr<fly::ReactiveController> brain_;
};

inline SimPtr make_fly_arena() { return std::make_unique<FlyArena>(); }

} // namespace bench

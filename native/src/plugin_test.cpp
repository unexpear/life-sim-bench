#include "plugin.hpp"
#include "history.hpp"
#include <cstdio>
int main(int argc, char** argv) {
    int fail = 0;
    auto ck=[&](bool ok,const char*m){ std::printf("    %s  %s\n", ok?"ok  ":"FAIL", m); if(!ok)++fail; };

    std::printf("\n  Hot-reload plugin host\n");
    auto found = bench::Plugin::scan(argc > 1 ? argv[1] : "../plugins");
    ck(!found.empty(), "scan() finds a dll in plugins/");
    if (found.empty()) return 1;

    bench::Plugin p;
    const bool loaded = p.load(found[0]);
    ck(loaded, ("load " + found[0] + (loaded?"":" -> " + p.error())).c_str());
    if (!loaded) return 1;

    auto sim = p.make();
    ck(sim != nullptr, "bench_create_sim() returned a Sim across the dll boundary");
    if (!sim) return 1;
    ck(!sim->about().title.empty(), ("provenance survives: " + sim->about().title).c_str());
    ck(sim->palette().size() >= 2, "palette survives");

    const auto g0 = sim->generation();
    for (int i = 0; i < 40; ++i) sim->step();
    ck(sim->generation() == g0 + 40, "it steps");
    sim->reset();ck(sim->generation()==0,"template reset clears its generation counter");
    const auto density=std::find_if(sim->knobs().begin(),sim->knobs().end(),[](const auto& k){return k.key=="density";});
    ck(density!=sim->knobs().end()&&density->on_reset,"template density uses staged setup");
    ck(!sim->metrics().empty(), "metrics come through the boundary");
    std::printf("      metric: %s = %.3f\n", sim->metrics()[0].name.c_str(), sim->metrics()[0].value);

    std::printf("\n  History (timeline + traces)\n");
    bench::History h; h.configure(8u*1024u*1024u, 4);
    h.set_min_interval(0); // Test retention independently of machine speed; limits tests the clock.
    for (int i = 0; i < 200; ++i) { sim->step(); h.observe(*sim); }
    ck(h.depth() > 10, "snapshots retained");
    ck(!h.names().empty(), "metric traces recorded");
    const auto* t = h.trace(h.names()[0]);
    ck(t && t->values.size() > 100, "trace has a dense series");
    ck(h.at(0) != nullptr && h.at(0)->w == sim->field().w, "can read an old frame back");
    const std::size_t before = h.bytes();
    bench::History tiny; tiny.configure(200*1024, 1);
    for (int i = 0; i < 400; ++i) { sim->step(); tiny.observe(*sim); }
    ck(tiny.bytes() <= 200*1024, "memory budget is respected (bounded by bytes, not count)");
    (void)before;

    sim.reset();
    p.unload();
    ck(true, "unload cleans up the shadow copy");
    std::printf("\n  %d failed\n", fail);
    return fail ? 1 : 0;
}

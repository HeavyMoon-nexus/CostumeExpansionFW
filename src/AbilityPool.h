#pragma once

#include <string>

namespace RE
{
    class TESObjectREFR;
}

namespace CostumeFW::pool
{
    // SPIKE - throwaway. Measures ONE question, and nothing else:
    //
    //   Does an ability whose source SPEL is a STATIC plugin form, filled in at
    //   runtime, still come off cleanly after a full process restart?
    //
    // That is the whole of ENCHANTMENT_ABILITY_POOL_DESIGN.md §7.1. Every other
    // part of that design - the registry, the tombstones, the state machine, the
    // dedicated plugin - is worthless if the answer is no, so it is measured
    // FIRST, on a throwaway test plugin, before any of it is built.
    //
    // D2 restated: a dynamic (0xFF) SPEL does not resolve on the next launch, so
    // the saved ActiveEffect that referenced it is dropped WITHOUT Finish(), and
    // the actor-value modifier it applied is stranded. Measured 2026-09-12 to hit
    // Perm AND Temp. A static form resolves - but resolving is only half of it.
    // The engine also needs the spell's effect LIST to be there when it restores
    // the ActiveEffect, and on a pool spell that list is ours to refill.
    //
    // So the experiment has a CONTROL. Each slot is hydrated either:
    //
    //   early - at kDataLoaded, before any save can be loaded (what §5.3 asks for)
    //   late  - only when the console says so, i.e. after the save is already up
    //
    // early green + late red  -> the ordering rule is real, and §5.3 is load-bearing
    // both green              -> ordering does not matter; the design gets simpler
    // both red                -> a static form is not sufficient; the design is void
    //
    // Test plugin: CEFTest_AbilityPool.esp, 8 empty constant-effect abilities at
    // local 0x800-0x807. Nothing here touches CostumeFW.esp or the box path.
    void HydrateAtDataLoaded();

    // `cef pool [list | set | on | off | hydrate | clear]`
    void PoolCommand(RE::TESObjectREFR* a_target, const std::string& a_args);
}

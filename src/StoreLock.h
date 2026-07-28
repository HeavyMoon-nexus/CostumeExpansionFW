#pragma once

#include <mutex>

namespace CostumeFW
{
    // ONE recursive mutex for every piece of CEF's shared store state - the
    // injection registry (SkinRebind g_active) AND the settings store (BoxStore
    // g_boxes / g_persist / the per-content maps).
    //
    // Why it exists (F1, audited 2026-07-27):
    //   g_active carries the comment "Main-thread access only", but it is read
    //   from the Papyrus VM thread (CFW_Native GetActive/IsActive, the persist
    //   natives) and from the SMF render callback every frame, while main-thread
    //   tasks push_back into it, erase from it, and assign std::strings into its
    //   elements inside Reconcile. The settings store is worse: box and persist
    //   definitions are written SYNCHRONOUSLY on the caller's thread by design,
    //   because the MCM needs the accept/refuse answer before it moves the item
    //   (Papyrus.cpp, "Box mutators split work").
    //   The comment that justified all this - "while the MCM is open the game is
    //   paused, so no task races these reads" - does not hold: the SKSE task
    //   queue drains every frame while a menu is up, which is the only reason
    //   CEF's own deferred mutators run at all.
    //
    // Why ONE mutex and not one per module:
    //   the two call each other in BOTH directions (SkinRebind reads body-morph
    //   and hide-shape settings from BoxStore; BoxStore calls Register /
    //   DetachSkinned / Reconcile in SkinRebind). Two locks would be a
    //   lock-order inversion waiting to deadlock. One lock cannot invert.
    //
    // Why RECURSIVE:
    //   those cross-calls mean a locked function routinely calls another locked
    //   function on the same thread. Recursion makes that free instead of fatal,
    //   and lets the rule stay simple: every exported store function takes it.
    //
    // Contention is not a concern: this state is touched by menu interaction and
    // by CEF's own main-thread tasks, not by anything per-frame in gameplay. The
    // one visibly slow holder is Reconcile (NIF loads), and it runs while the
    // menu has the game paused anyway.
    //
    // No deadlock cycle exists: nothing taken under this lock ever waits on
    // another thread. The sync worker and the delay timers hand their results
    // back through SKSE::AddTask, which takes no lock.
    inline std::recursive_mutex& StoreMutex()
    {
        static std::recursive_mutex m;
        return m;
    }

    struct StoreLock
    {
        std::lock_guard<std::recursive_mutex> lk{ StoreMutex() };
    };
}

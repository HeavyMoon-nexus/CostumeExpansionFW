#pragma once

#include <mutex>

namespace CostumeFW
{
    // ONE recursive mutex for every piece of CEF's shared store state - the
    // injection registry (SkinRebind g_active) AND the settings store (BoxStore
    // g_boxes / g_persist / the per-content maps).
    //
    // Who takes it, as it actually is (review 2026-09-23 M1 - this comment said
    // otherwise for two months, and a "fix" built on it would race for real):
    //   - the CFW_Native natives: registered without a_callableFromTasklets, so
    //     the VM runs every one of them on the MAIN thread, synchronized with
    //     the frame (that is also why each call costs a frame). Never make them
    //     NoWait - they would then run on a VM thread and race everything below.
    //   - the SMF render callbacks: SMF-3 calls them from inside Main::Draw, on
    //     the main thread, like the SKSE task pump.
    //   - main-thread tasks (Reconcile and the rest), and the PlayerCharacter::
    //     Update hook - EVERY frame (containment sweep, visual-state sync).
    //   - nothing off the main thread since W8: the Load3D hooks run on loader
    //     threads and only AddTask, and RunAfterDelayMs takes only its own mutex.
    //   The lock is kept anyway. It costs one uncontended acquire, and it is what
    //   makes a future off-thread caller safe instead of silently racing.
    //   The settings store is written SYNCHRONOUSLY on the caller's thread by
    //   design, because the MCM needs the accept/refuse answer before it moves
    //   the item (Papyrus.cpp, "Box mutators split work").
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
    // Contention is not a concern: every holder is on the main thread, so the
    // lock is never actually waited on. The per-frame holders (the Update hook)
    // are record-based and cheap; the one slow holder is Reconcile (NIF loads).
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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
    class EnchantmentItem;
    class SpellItem;
}

namespace CostumeFW::abilities
{
    // The fixed ability pool - CostumeFW_Abilities.esp.
    //
    // WHY THIS EXISTS
    //   Up to 1.6.2, a costume's enchantment effects were put on a SpellItem
    //   created at runtime. A runtime form has a 0xFF form ID, which does not
    //   exist on the next launch, so the saved ActiveEffect that referenced it
    //   could not be rebuilt - it was dropped WITHOUT Finish(), and the actor
    //   value modifier it had applied stayed on the character forever. Measured
    //   2026-09-12 to hit both Perm and Temp modifiers.
    //
    //   Measured 2026-09-13: an ability whose source SPEL is a STATIC plugin
    //   form, filled in at runtime, comes off cleanly after a full restart -
    //   conditional effects included - PROVIDED the effect list is there before
    //   the save is read. A control slot filled after the save was already up
    //   stranded its modifier exactly as before. That is why InitAtDataLoaded()
    //   is synchronous and runs at kDataLoaded: not as an optimisation, as the
    //   condition of the whole thing working.
    //
    // WHY IT IS NOT ENOUGH TO RESOLVE THE FORM
    //   In that same measurement the pool listing showed the control slots as
    //   GRANTED with 0 effects: the spell was on the actor and its effect list
    //   was empty. What drops the ActiveEffect is the missing effect LIST, not
    //   a missing form. Anything that only reserves stable form IDs (DPF RE,
    //   for instance) does not answer this by itself.
    //
    // See cef_documents/active/ENCHANTMENT_ABILITY_POOL_DESIGN.md.

    enum class State
    {
        Uninitialized,
        PoolValidated,      // the plugin is present and big enough
        RecipesLoaded,      // the registry parsed and its checksum matched
        AbilitiesHydrated,  // every allocated ability carries its effects
        Ready,
        DisabledSafe        // something failed; passthrough is OFF and stays off
    };

    // Call from kDataLoaded, SYNCHRONOUSLY, on the main thread. Not from
    // AddTask: that would run after the save is loaded, which is the case the
    // measurement showed strands modifiers.
    void InitAtDataLoaded();

    [[nodiscard]] State CurrentState();
    [[nodiscard]] bool Ready();
    // Empty when Ready. Otherwise one sentence a user can act on.
    [[nodiscard]] std::string DisabledReason();

    // Pool accounting, for the UI and the log (design 5.2).
    struct Usage
    {
        int used{ 0 };        // allocated and live
        int tombstoned{ 0 };  // content gone, recipe kept for old saves
        int free{ 0 };
        int total{ 0 };
    };
    [[nodiscard]] Usage PoolUsage();

    // The ability that carries a_contentId's enchantment, allocating one and
    // writing the registry if this content has never had one. Returns nullptr
    // when not Ready, when the content contributes no effects, when the pool is
    // full, or when the recipe cannot be serialized faithfully - never a
    // half-filled spell. Main thread only.
    //
    // Phase 1 note: nothing calls this from the box path yet. It is reachable
    // from `cef abilities alloc` so the registry can be exercised on its own.
    [[nodiscard]] RE::SpellItem* AbilityFor(const std::string& a_contentId);

    // A save written before the pool existed has just been loaded, and these
    // contents were active in it. Main thread.
    //
    // There is nothing to PREVENT here, which is worth being clear about. The
    // engine restores the actor's active effects before any CEF code runs, and
    // a 1.6.2 save's effects point at runtime spells that no longer exist - so
    // they are dropped, and whatever they applied is already part of the
    // character by the time this is called. A form ID cannot be reserved in
    // advance, so there is no way to catch them. The only release that could
    // have stopped it is the one that wrote the save.
    //
    // So this reports. It names the numbers that are most likely stuck, in the
    // actor-value spellings the console wants, and leaves the fixing to the
    // player: CEF cannot tell its own leftovers from a bonus another mod
    // applied on purpose, so it does not subtract anything by itself.
    void ReportLegacySave(const std::vector<std::string>& a_contents);

    // `cef abilities [state | list | alloc <id> | usage | legacy]`
    void AbilitiesCommand(const std::string& a_args);
}

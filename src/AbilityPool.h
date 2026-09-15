#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
    class Actor;
    class TESObjectREFR;
    struct Effect;
    class EffectSetting;
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

    // Put a BROKEN pool's reason in front of the user, once per process. Call it
    // after a save finishes loading. Silent for the benign "the ability plugin
    // is not installed" case, which is a choice and not a fault.
    void ReportStateToUser();

    // Pool accounting, for the UI and the log (design 5.2).
    struct Usage
    {
        int used{ 0 };        // allocated and live
        int tombstoned{ 0 };  // content gone, recipe kept for old saves
        int free{ 0 };
        int total{ 0 };
    };
    [[nodiscard]] Usage PoolUsage();

    // The plugin that would ADD capacity: the generation after the highest
    // installed one. Named rather than described because the spelling is not
    // guessable - generation 1 is CostumeFW_Abilities.esp with NO number, so
    // "the next one" invites CostumeFW_Abilities1.esp, which is the one
    // spelling that is explicitly not valid (see TokenIdentity.h).
    [[nodiscard]] std::string NextPoolPluginName();

    // One effect a content contributes: either a LIVE source Effect (full
    // fidelity - magnitude, area, duration and the conditions that gate it) or
    // a flat mgef+magnitude snapshot for the case where the original item is
    // out of reach.
    //
    // The pool takes effects rather than a content id and a form to read them
    // off, and that split is deliberate. Deciding what a content is worth is a
    // five-source priority that belongs to BoxStore - the stored original's
    // instance enchantment, the base form when the store verifiably holds the
    // original, a flat snapshot, the bare base form, a holder's frozen copy -
    // and getting it wrong drops player enchantments and tempering. The pool's
    // job starts once that is decided: serialize it, allocate a slot, rebuild
    // it after a restart. See BoxStore's ContentEffectsFor.
    struct SourceEffect
    {
        RE::EffectSetting* mgef{ nullptr };  // used when live is null
        float magnitude{ 0.0f };
        const RE::Effect* live{ nullptr };
    };

    // The ability carrying these effects for a_contentId, allocating a slot and
    // writing the registry if this content has never had one. Returns nullptr
    // when not Ready, when there are no effects, when the pool is full, or when
    // the recipe cannot be serialized faithfully - never a half-filled spell.
    // Main thread only.
    [[nodiscard]] RE::SpellItem* AbilityFor(const std::string& a_contentId,
        const std::vector<SourceEffect>& a_effects);

    // Converge an actor: every content in a_wanted gets its ability, every
    // other allocated ability comes off. Allocates for a content that has none
    // yet. Main thread only.
    //
    // Convergence is over the WHOLE pool rather than a remembered per-box list,
    // which is what makes it idempotent: a content dropped from a box, a box
    // deleted, the master switch turned off, a load - all of them are just
    // "not in a_wanted any more", with nothing to keep in step and nothing to
    // strand. The old per-box ability needed four separate paths to guarantee
    // the same thing.
    void SyncToActor(RE::Actor* a_actor, const std::vector<std::string>& a_wanted);

    // Re-derive a content's recipe. If it differs from the one on record, a NEW
    // slot is allocated at the next generation and the old one is tombstoned.
    //
    // The old recipe is never edited in place, because a save may still hold an
    // active effect built from it: rewriting it under that save would change
    // what an existing bonus is worth without the engine ever being told. Call
    // this when a content's enchantment may have changed - a re-enchant, a
    // capture, a settings reload - not on the hot path, where re-deriving costs
    // a walk of the form table for anything with conditions.
    void RefreshContent(const std::string& a_contentId);

    // A save written before the pool existed has just been loaded. Call AFTER
    // Reconcile, on the main thread: it reads the active set itself, because
    // the co-save does not carry it. The co-save's ACTV record holds PERSIST
    // items only - box definitions are global config, and which boxes are worn
    // lives in the save's own equip state, which is why this is asked after the
    // reconcile rather than handed a list by the loader.
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
    void ReportLegacySave(bool a_prePoolSave);

    // `cef abilities [state | list | alloc <id> | usage | legacy | npc <id> | npcoff]`
    // Takes the console's selected reference, so the npc subcommands can act on
    // whoever is clicked - the only way to measure the pool on an actor that is
    // not the player until published costumes move onto it.
    void AbilitiesCommand(RE::TESObjectREFR* a_target, const std::string& a_args);
}
